// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeMCPServer.h"
#include "MCPToolRegistry.h"
#include "UnrealClaudeModule.h"
#include "UnrealClaudeConstants.h"
#include "UnrealClaudeSettings.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "IPAddress.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"

namespace
{
	const TCHAR* GMCPAuthHeaderName = TEXT("X-UnrealClaude-MCP-Token");
	const TCHAR* GMCPAuthTokenSchema = TEXT("unrealclaude_mcp_auth_token.v1");
	const TCHAR* GMCPAuthDeniedSchema = TEXT("unrealclaude_mcp_auth_denied.v1");

	FString BuildMcpUtcTimestamp()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	FString BuildMcpSessionToken()
	{
		return FString::Printf(
			TEXT("%s-%s-%s-%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	FString GetHeaderValueCaseInsensitive(const FHttpServerRequest& Request, const FString& HeaderName)
	{
		for (const TPair<FString, TArray<FString>>& HeaderPair : Request.Headers)
		{
			if (HeaderPair.Key.Equals(HeaderName, ESearchCase::IgnoreCase) && HeaderPair.Value.Num() > 0)
			{
				return HeaderPair.Value[0].TrimStartAndEnd();
			}
		}

		return FString();
	}

	bool TryGetJsonStringField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, FString& OutValue)
	{
		return Object.IsValid() && Object->TryGetStringField(FieldName, OutValue) && !OutValue.IsEmpty();
	}
}

FUnrealClaudeMCPServer::FUnrealClaudeMCPServer()
	: bIsRunning(false)
	, ServerPort(UUnrealClaudeSettings::Get() ? UUnrealClaudeSettings::Get()->MCPServerPort : UnrealClaudeConstants::MCPServer::DefaultPort)
{
	ToolRegistry = MakeShared<FMCPToolRegistry>();
	InitializeSecurityState();
}

FUnrealClaudeMCPServer::~FUnrealClaudeMCPServer()
{
	Stop();
}

void FUnrealClaudeMCPServer::InitializeSecurityState()
{
	SessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	SessionToken = BuildMcpSessionToken();
	AuthTokenFilePath = BuildAuthTokenFilePath();
	DeniedAuditPath = BuildDeniedAuditPath();
}

FString FUnrealClaudeMCPServer::BuildAuthTokenFilePath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealClaude/mcp_auth_token.json"));
}

FString FUnrealClaudeMCPServer::BuildDeniedAuditPath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealClaude/mcp_denied_audit.jsonl"));
}

bool FUnrealClaudeMCPServer::PersistAuthTokenFile() const
{
	if (SessionToken.IsEmpty() || AuthTokenFilePath.IsEmpty())
	{
		return false;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(AuthTokenFilePath), true);

	TSharedPtr<FJsonObject> TokenJson = MakeShared<FJsonObject>();
	TokenJson->SetStringField(TEXT("schema_version"), GMCPAuthTokenSchema);
	TokenJson->SetStringField(TEXT("token"), SessionToken);
	TokenJson->SetStringField(TEXT("header"), GMCPAuthHeaderName);
	TokenJson->SetStringField(TEXT("session_id"), SessionId);
	TokenJson->SetStringField(TEXT("created_at_utc"), BuildMcpUtcTimestamp());
	TokenJson->SetNumberField(TEXT("port"), ServerPort);
	TokenJson->SetNumberField(TEXT("process_id"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
	TokenJson->SetStringField(TEXT("scope"), TEXT("local_editor_session"));

	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(TokenJson.ToSharedRef(), Writer);

	return FFileHelper::SaveStringToFile(JsonString, *AuthTokenFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FUnrealClaudeMCPServer::Start(uint32 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("MCP Server is already running on port %d"), ServerPort);
		return true;
	}

	ServerPort = Port;
	InitializeSecurityState();
	if (!PersistAuthTokenFile())
	{
		UE_LOG(LogUnrealClaude, Error, TEXT("Failed to persist MCP auth token handoff file at %s"), *AuthTokenFilePath);
		return false;
	}

	// Get or start the HTTP server module
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();

	// Start listening on the specified port
	HttpRouter = HttpServerModule.GetHttpRouter(ServerPort);
	if (!HttpRouter.IsValid())
	{
		UE_LOG(LogUnrealClaude, Error, TEXT("Failed to get HTTP router for port %d"), ServerPort);
		return false;
	}

	// Setup routes
	SetupRoutes();

	// Start the listeners
	HttpServerModule.StartAllListeners();

	bIsRunning = true;

	// Start the async task queue
	if (ToolRegistry.IsValid())
	{
		ToolRegistry->StartTaskQueue();
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("MCP Server started on http://localhost:%d"), ServerPort);
	UE_LOG(LogUnrealClaude, Log, TEXT("  GET  /mcp/tools      - List available tools (token required)"));
	UE_LOG(LogUnrealClaude, Log, TEXT("  POST /mcp/tool/{name} - Execute a tool (token required)"));
	UE_LOG(LogUnrealClaude, Log, TEXT("  GET  /mcp/status     - Server status (local read-only discovery)"));
	UE_LOG(LogUnrealClaude, Log, TEXT("MCP auth token handoff file written at %s"), *AuthTokenFilePath);

	return true;
}

void FUnrealClaudeMCPServer::Stop()
{
	if (!bIsRunning)
	{
		return;
	}

	// Stop the async task queue first
	if (ToolRegistry.IsValid())
	{
		ToolRegistry->StopTaskQueue();
	}

	// Unbind routes
	if (HttpRouter.IsValid())
	{
		if (ListToolsHandle.IsValid())
		{
			HttpRouter->UnbindRoute(ListToolsHandle);
		}
		if (ExecuteToolHandle.IsValid())
		{
			HttpRouter->UnbindRoute(ExecuteToolHandle);
		}
		if (StatusHandle.IsValid())
		{
			HttpRouter->UnbindRoute(StatusHandle);
		}
	}

	bIsRunning = false;
	UE_LOG(LogUnrealClaude, Log, TEXT("MCP Server stopped"));
}

bool FUnrealClaudeMCPServer::ConstantTimeTokenEquals(const FString& A, const FString& B) const
{
	const int32 MaxLen = FMath::Max(A.Len(), B.Len());
	int32 Diff = A.Len() ^ B.Len();
	for (int32 Index = 0; Index < MaxLen; ++Index)
	{
		const TCHAR AChar = Index < A.Len() ? A[Index] : 0;
		const TCHAR BChar = Index < B.Len() ? B[Index] : 0;
		Diff |= static_cast<int32>(AChar ^ BChar);
	}
	return Diff == 0;
}

FString FUnrealClaudeMCPServer::ExtractRequestToken(const FHttpServerRequest& Request, bool& bOutHeaderPresent) const
{
	bOutHeaderPresent = false;

	FString HeaderToken = GetHeaderValueCaseInsensitive(Request, GMCPAuthHeaderName);
	if (!HeaderToken.IsEmpty())
	{
		bOutHeaderPresent = true;
		return HeaderToken;
	}

	const FString Authorization = GetHeaderValueCaseInsensitive(Request, TEXT("Authorization"));
	if (!Authorization.IsEmpty())
	{
		bOutHeaderPresent = true;
		const FString BearerPrefix = TEXT("Bearer ");
		if (Authorization.StartsWith(BearerPrefix, ESearchCase::IgnoreCase))
		{
			return Authorization.RightChop(BearerPrefix.Len()).TrimStartAndEnd();
		}
	}

	return FString();
}

bool FUnrealClaudeMCPServer::IsRequestFromLocalPeer(const FHttpServerRequest& Request, FString& OutRemoteAddress) const
{
	OutRemoteAddress.Reset();
	if (!Request.PeerAddress.IsValid())
	{
		return true;
	}

	OutRemoteAddress = Request.PeerAddress->ToString(false);
	if (OutRemoteAddress.IsEmpty())
	{
		return true;
	}

	return OutRemoteAddress.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)
		|| OutRemoteAddress.Equals(TEXT("::1"), ESearchCase::IgnoreCase)
		|| OutRemoteAddress.Equals(TEXT("0:0:0:0:0:0:0:1"), ESearchCase::IgnoreCase)
		|| OutRemoteAddress.StartsWith(TEXT("127."));
}

FMCPHttpAuthValidationResult FUnrealClaudeMCPServer::ValidateRequestAuth(
	const FHttpServerRequest& Request,
	const FString& Endpoint,
	const FString& ToolName)
{
	FMCPHttpAuthValidationResult Result;
	Result.Endpoint = Endpoint;
	Result.ToolName = ToolName;

	if (!IsRequestFromLocalPeer(Request, Result.RemoteAddress))
	{
		Result.Reason = TEXT("non_local_remote_address");
		Result.ResponseCode = EHttpServerResponseCodes::Forbidden;
		AppendDeniedAudit(Endpoint, ToolName, Result.Reason, Request);
		return Result;
	}

	bool bHeaderPresent = false;
	const FString RequestToken = ExtractRequestToken(Request, bHeaderPresent);
	Result.bTokenHeaderPresent = bHeaderPresent;

	if (RequestToken.IsEmpty())
	{
		Result.Reason = TEXT("missing_token");
		Result.ResponseCode = EHttpServerResponseCodes::Denied;
		AppendDeniedAudit(Endpoint, ToolName, Result.Reason, Request);
		return Result;
	}

	if (SessionToken.IsEmpty() || !ConstantTimeTokenEquals(RequestToken, SessionToken))
	{
		Result.Reason = TEXT("invalid_token");
		Result.ResponseCode = EHttpServerResponseCodes::Denied;
		AppendDeniedAudit(Endpoint, ToolName, Result.Reason, Request);
		return Result;
	}

	Result.bAllowed = true;
	Result.Reason = TEXT("authorized");
	Result.ResponseCode = EHttpServerResponseCodes::Ok;
	return Result;
}

void FUnrealClaudeMCPServer::AttachAuthenticatedRequestMetadata(const TSharedRef<FJsonObject>& Params) const
{
	Params->SetBoolField(TEXT("_mcp_http_auth_valid"), true);
	Params->SetBoolField(TEXT("_mcp_expert_session_armed"), false);
	Params->SetStringField(TEXT("_mcp_auth_boundary"), TEXT("token_valid_expert_unarmed"));
}

void FUnrealClaudeMCPServer::AppendDeniedAudit(
	const FString& Endpoint,
	const FString& ToolName,
	const FString& Reason,
	const FHttpServerRequest& Request,
	const TSharedPtr<FJsonObject>& ExtraData) const
{
	if (DeniedAuditPath.IsEmpty())
	{
		return;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(DeniedAuditPath), true);

	bool bHeaderPresent = false;
	ExtractRequestToken(Request, bHeaderPresent);

	FString RemoteAddress;
	IsRequestFromLocalPeer(Request, RemoteAddress);

	TSharedPtr<FJsonObject> AuditJson = MakeShared<FJsonObject>();
	AuditJson->SetStringField(TEXT("schema_version"), GMCPAuthDeniedSchema);
	AuditJson->SetStringField(TEXT("timestamp_utc"), BuildMcpUtcTimestamp());
	AuditJson->SetStringField(TEXT("endpoint"), Endpoint);
	AuditJson->SetStringField(TEXT("tool_name"), ToolName);
	AuditJson->SetStringField(TEXT("reason"), Reason);
	AuditJson->SetStringField(TEXT("remote_address"), RemoteAddress.IsEmpty() ? TEXT("unknown") : RemoteAddress);
	AuditJson->SetBoolField(TEXT("token_header_present"), bHeaderPresent);
	AuditJson->SetStringField(TEXT("session_id"), SessionId);
	AuditJson->SetStringField(TEXT("secret_redaction"), TEXT("token_value_never_logged"));
	if (ExtraData.IsValid())
	{
		AuditJson->SetObjectField(TEXT("details"), ExtraData);
	}

	FString JsonLine;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonLine);
	FJsonSerializer::Serialize(AuditJson.ToSharedRef(), Writer);
	JsonLine += LINE_TERMINATOR;

	FFileHelper::SaveStringToFile(
		JsonLine,
		*DeniedAuditPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(),
		FILEWRITE_Append);
}

void FUnrealClaudeMCPServer::AuditToolDenyResult(
	const FString& Endpoint,
	const FString& ToolName,
	const FMCPToolResult& Result,
	const FHttpServerRequest& Request) const
{
	if (Result.bSuccess || !Result.Data.IsValid())
	{
		return;
	}

	FString ResultType;
	if (!TryGetJsonStringField(Result.Data, TEXT("result_type"), ResultType)
		|| !ResultType.Equals(TEXT("policy_denied"), ESearchCase::IgnoreCase))
	{
		return;
	}

	FString ErrorCategory;
	const FString Reason = TryGetJsonStringField(Result.Data, TEXT("error_category"), ErrorCategory)
		? ErrorCategory
		: TEXT("policy_denied");

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("result_type"), ResultType);
	Details->SetStringField(TEXT("message"), Result.Message);
	FString TargetToolName;
	if (TryGetJsonStringField(Result.Data, TEXT("target_tool_name"), TargetToolName))
	{
		Details->SetStringField(TEXT("target_tool_name"), TargetToolName);
	}
	Details->SetStringField(TEXT("source"), TEXT("tool_registry"));
	AppendDeniedAudit(Endpoint, ToolName, Reason, Request, Details);
}

#if WITH_DEV_AUTOMATION_TESTS
void FUnrealClaudeMCPServer::SetSessionTokenForTests(const FString& Token)
{
	SessionToken = Token;
	if (SessionId.IsEmpty())
	{
		SessionId = TEXT("test_session");
	}
}

void FUnrealClaudeMCPServer::SetDeniedAuditPathForTests(const FString& AuditPath)
{
	DeniedAuditPath = AuditPath;
}

FMCPHttpAuthValidationResult FUnrealClaudeMCPServer::ValidateRequestAuthForTests(
	const FHttpServerRequest& Request,
	const FString& Endpoint,
	const FString& ToolName)
{
	return ValidateRequestAuth(Request, Endpoint, ToolName);
}

void FUnrealClaudeMCPServer::AppendDeniedAuditForTests(
	const FString& Endpoint,
	const FString& ToolName,
	const FString& Reason,
	const FHttpServerRequest& Request)
{
	AppendDeniedAudit(Endpoint, ToolName, Reason, Request);
}
#endif

void FUnrealClaudeMCPServer::SetupRoutes()
{
	if (!HttpRouter.IsValid())
	{
		return;
	}

	// GET /mcp/tools - List all available tools
	ListToolsHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp/tools")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FUnrealClaudeMCPServer::HandleListTools)
	);

	// POST /mcp/tool/* - Execute a tool (wildcard path)
	ExecuteToolHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp/tool")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FUnrealClaudeMCPServer::HandleExecuteTool)
	);

	// GET /mcp/status - Server status
	StatusHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp/status")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FUnrealClaudeMCPServer::HandleStatus)
	);
}

bool FUnrealClaudeMCPServer::HandleListTools(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const FMCPHttpAuthValidationResult AuthResult = ValidateRequestAuth(Request, TEXT("/mcp/tools"), FString());
	if (!AuthResult.bAllowed)
	{
		OnComplete(CreateAuthDeniedResponse(AuthResult));
		return true;
	}

	TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	if (ToolRegistry.IsValid())
	{
		TArray<FMCPToolInfo> Tools = ToolRegistry->GetAllTools();
		for (const FMCPToolInfo& Tool : Tools)
		{
			TSharedPtr<FJsonObject> ToolJson = MakeShared<FJsonObject>();
			ToolJson->SetStringField(TEXT("name"), Tool.Name);
			ToolJson->SetStringField(TEXT("description"), Tool.Description);

			// Add parameters schema
			TArray<TSharedPtr<FJsonValue>> ParamsArray;
			for (const FMCPToolParameter& Param : Tool.Parameters)
			{
				TSharedPtr<FJsonObject> ParamJson = MakeShared<FJsonObject>();
				ParamJson->SetStringField(TEXT("name"), Param.Name);
				ParamJson->SetStringField(TEXT("type"), Param.Type);
				ParamJson->SetStringField(TEXT("description"), Param.Description);
				ParamJson->SetBoolField(TEXT("required"), Param.bRequired);
				if (!Param.DefaultValue.IsEmpty())
				{
					ParamJson->SetStringField(TEXT("default"), Param.DefaultValue);
				}
				ParamsArray.Add(MakeShared<FJsonValueObject>(ParamJson));
			}
			ToolJson->SetArrayField(TEXT("parameters"), ParamsArray);

			// Add tool annotations (behavioral hints for LLM clients)
			TSharedPtr<FJsonObject> AnnotationsJson = MakeShared<FJsonObject>();
			AnnotationsJson->SetBoolField(TEXT("readOnlyHint"), Tool.Annotations.bReadOnlyHint);
			AnnotationsJson->SetBoolField(TEXT("destructiveHint"), Tool.Annotations.bDestructiveHint);
			AnnotationsJson->SetBoolField(TEXT("idempotentHint"), Tool.Annotations.bIdempotentHint);
			AnnotationsJson->SetBoolField(TEXT("openWorldHint"), Tool.Annotations.bOpenWorldHint);
			ToolJson->SetObjectField(TEXT("annotations"), AnnotationsJson);

			ToolsArray.Add(MakeShared<FJsonValueObject>(ToolJson));
		}
	}

	ResponseJson->SetArrayField(TEXT("tools"), ToolsArray);

	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);

	OnComplete(CreateJsonResponse(JsonString));
	return true;
}

bool FUnrealClaudeMCPServer::HandleExecuteTool(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Extract tool name from path: /mcp/tool/{name}
	FString RelativePath = Request.RelativePath.GetPath();

	// Parse tool name from path
	FString ToolName;
	if (RelativePath.StartsWith(TEXT("/mcp/tool/")))
	{
		ToolName = RelativePath.RightChop(10); // Remove "/mcp/tool/"
	}
	else if (RelativePath.StartsWith(TEXT("/")))
	{
		ToolName = RelativePath.RightChop(1);
	}
	else
	{
		ToolName = RelativePath;
	}

	if (ToolName.IsEmpty())
	{
		const FMCPHttpAuthValidationResult AuthResult = ValidateRequestAuth(Request, TEXT("/mcp/tool"), FString());
		if (!AuthResult.bAllowed)
		{
			OnComplete(CreateAuthDeniedResponse(AuthResult));
		}
		else
		{
			OnComplete(CreateErrorResponse(TEXT("Tool name not specified. Use POST /mcp/tool/{toolname}"), EHttpServerResponseCodes::BadRequest));
		}
		return true;
	}

	const FMCPHttpAuthValidationResult AuthResult = ValidateRequestAuth(
		Request,
		FString::Printf(TEXT("/mcp/tool/%s"), *ToolName),
		ToolName);
	if (!AuthResult.bAllowed)
	{
		OnComplete(CreateAuthDeniedResponse(AuthResult));
		return true;
	}

	// Parse JSON body for parameters
	TSharedPtr<FJsonObject> ParamsJson;
	if (Request.Body.Num() > UnrealClaudeConstants::MCPServer::MaxRequestBodySize)
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("Request body too large: %d bytes (max %d)"), Request.Body.Num(), UnrealClaudeConstants::MCPServer::MaxRequestBodySize);
		OnComplete(CreateErrorResponse(TEXT("Request body too large"), EHttpServerResponseCodes::BadRequest));
		return true;
	}
	if (Request.Body.Num() > 0)
	{
		// Ensure null-termination for safe string conversion
		TArray<uint8> NullTerminatedBody = Request.Body;
		NullTerminatedBody.Add(0);
		FString BodyString = UTF8_TO_TCHAR(reinterpret_cast<const char*>(NullTerminatedBody.GetData()));

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyString);
		if (!FJsonSerializer::Deserialize(Reader, ParamsJson) || !ParamsJson.IsValid())
		{
			UE_LOG(LogUnrealClaude, Warning, TEXT("Failed to parse JSON body: %s"), *BodyString);
			OnComplete(CreateErrorResponse(TEXT("Invalid JSON body"), EHttpServerResponseCodes::BadRequest));
			return true;
		}
	}
	else
	{
		ParamsJson = MakeShared<FJsonObject>();
	}
	AttachAuthenticatedRequestMetadata(ParamsJson.ToSharedRef());

	// Execute tool
	if (!ToolRegistry.IsValid())
	{
		OnComplete(CreateErrorResponse(TEXT("Tool registry not initialized"), EHttpServerResponseCodes::ServerError));
		return true;
	}

	FMCPToolResult Result = ToolRegistry->ExecuteTool(ToolName, ParamsJson.ToSharedRef());
	AuditToolDenyResult(FString::Printf(TEXT("/mcp/tool/%s"), *ToolName), ToolName, Result, Request);

	// Build response
	TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetBoolField(TEXT("success"), Result.bSuccess);
	ResponseJson->SetStringField(TEXT("message"), Result.Message);

	if (Result.Data.IsValid())
	{
		ResponseJson->SetObjectField(TEXT("data"), Result.Data);
	}

	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);

	EHttpServerResponseCodes Code = Result.bSuccess ? EHttpServerResponseCodes::Ok : EHttpServerResponseCodes::BadRequest;
	OnComplete(CreateJsonResponse(JsonString, Code));
	return true;
}

bool FUnrealClaudeMCPServer::HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();

	ResponseJson->SetStringField(TEXT("status"), TEXT("running"));
	ResponseJson->SetNumberField(TEXT("port"), ServerPort);
	ResponseJson->SetStringField(TEXT("version"), TEXT("1.0.0"));
	ResponseJson->SetNumberField(TEXT("toolCount"), ToolRegistry.IsValid() ? ToolRegistry->GetAllTools().Num() : 0);
	ResponseJson->SetStringField(TEXT("status_endpoint"), TEXT("unauthenticated_local_read_only_discovery"));

	TSharedPtr<FJsonObject> AuthJson = MakeShared<FJsonObject>();
	AuthJson->SetBoolField(TEXT("required"), true);
	AuthJson->SetBoolField(TEXT("status_requires_auth"), false);
	AuthJson->SetStringField(TEXT("header"), GMCPAuthHeaderName);
	AuthJson->SetStringField(TEXT("token_transport"), TEXT("header_or_authorization_bearer"));
	AuthJson->SetStringField(TEXT("token_file"), AuthTokenFilePath);
	AuthJson->SetStringField(TEXT("denied_audit_file"), DeniedAuditPath);
	AuthJson->SetBoolField(TEXT("token_value_redacted"), true);
	ResponseJson->SetObjectField(TEXT("auth"), AuthJson);

	// Add project info
	ResponseJson->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	ResponseJson->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());

	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);

	OnComplete(CreateJsonResponse(JsonString));
	return true;
}

TUniquePtr<FHttpServerResponse> FUnrealClaudeMCPServer::CreateJsonResponse(const FString& JsonContent, EHttpServerResponseCodes Code)
{
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(JsonContent, TEXT("application/json"));
	Response->Code = Code;

	// Add CORS headers - restricted to localhost for security
	Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("http://localhost") });
	Response->Headers.Add(TEXT("Access-Control-Allow-Methods"), { TEXT("GET, POST, OPTIONS") });
	Response->Headers.Add(TEXT("Access-Control-Allow-Headers"), { FString::Printf(TEXT("Content-Type, Authorization, %s"), GMCPAuthHeaderName) });

	return Response;
}

TUniquePtr<FHttpServerResponse> FUnrealClaudeMCPServer::CreateAuthDeniedResponse(const FMCPHttpAuthValidationResult& AuthResult)
{
	TSharedPtr<FJsonObject> DataJson = MakeShared<FJsonObject>();
	DataJson->SetStringField(TEXT("result_type"), TEXT("auth_denied"));
	DataJson->SetStringField(TEXT("schema_version"), TEXT("mcp_auth_denial.v1"));
	DataJson->SetStringField(TEXT("error_category"), TEXT("auth_denied"));
	DataJson->SetStringField(TEXT("denial_reason"), AuthResult.Reason);
	DataJson->SetStringField(TEXT("endpoint"), AuthResult.Endpoint);
	DataJson->SetStringField(TEXT("tool_name"), AuthResult.ToolName);
	DataJson->SetBoolField(TEXT("token_header_present"), AuthResult.bTokenHeaderPresent);
	DataJson->SetStringField(TEXT("remote_address"), AuthResult.RemoteAddress.IsEmpty() ? TEXT("unknown") : AuthResult.RemoteAddress);
	DataJson->SetStringField(TEXT("audit_file"), DeniedAuditPath);
	DataJson->SetBoolField(TEXT("token_value_redacted"), true);
	DataJson->SetBoolField(TEXT("structured_denial"), true);

	TSharedPtr<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
	ErrorJson->SetBoolField(TEXT("success"), false);
	ErrorJson->SetStringField(TEXT("error"), TEXT("MCP authentication failed"));
	ErrorJson->SetStringField(TEXT("message"), FString::Printf(TEXT("MCP authentication failed: %s"), *AuthResult.Reason));
	ErrorJson->SetObjectField(TEXT("data"), DataJson);

	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(ErrorJson.ToSharedRef(), Writer);

	return CreateJsonResponse(JsonString, AuthResult.ResponseCode);
}

TUniquePtr<FHttpServerResponse> FUnrealClaudeMCPServer::CreateErrorResponse(const FString& Message, EHttpServerResponseCodes Code)
{
	TSharedPtr<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
	ErrorJson->SetBoolField(TEXT("success"), false);
	ErrorJson->SetStringField(TEXT("error"), Message);

	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(ErrorJson.ToSharedRef(), Writer);

	return CreateJsonResponse(JsonString, Code);
}
