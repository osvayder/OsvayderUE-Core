// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "UnrealClaudeConstants.h"

class FMCPToolRegistry;
struct FMCPToolResult;

struct FMCPHttpAuthValidationResult
{
	bool bAllowed = false;
	bool bTokenHeaderPresent = false;
	EHttpServerResponseCodes ResponseCode = EHttpServerResponseCodes::Denied;
	FString Reason;
	FString Endpoint;
	FString ToolName;
	FString RemoteAddress;
};

/**
 * MCP HTTP Server for editor control
 * Provides REST API endpoints for Claude to interact with the Unreal Editor
 */
class FUnrealClaudeMCPServer
{
public:
	FUnrealClaudeMCPServer();
	~FUnrealClaudeMCPServer();

	/** Start the MCP server on the specified port */
	bool Start(uint32 Port = UnrealClaudeConstants::MCPServer::DefaultPort);

	/** Stop the MCP server */
	void Stop();

	/** Check if server is running */
	bool IsRunning() const { return bIsRunning; }

	/** Get the server port */
	uint32 GetPort() const { return ServerPort; }

	/** Get the tool registry */
	TSharedPtr<FMCPToolRegistry> GetToolRegistry() const { return ToolRegistry; }

	/** Runtime token file path for local bridge discovery. Does not expose the secret token value. */
	FString GetAuthTokenFilePath() const { return AuthTokenFilePath; }

#if WITH_DEV_AUTOMATION_TESTS
	void SetSessionTokenForTests(const FString& Token);
	FString GetSessionTokenForTests() const { return SessionToken; }
	void SetDeniedAuditPathForTests(const FString& AuditPath);
	FMCPHttpAuthValidationResult ValidateRequestAuthForTests(const FHttpServerRequest& Request, const FString& Endpoint, const FString& ToolName);
	void AppendDeniedAuditForTests(const FString& Endpoint, const FString& ToolName, const FString& Reason, const FHttpServerRequest& Request);
#endif

private:
	/** Setup HTTP routes */
	void SetupRoutes();

	/** Handle GET /mcp/tools - List all available tools */
	bool HandleListTools(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle POST /mcp/tool/{name} - Execute a tool */
	bool HandleExecuteTool(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle GET /mcp/status - Get server status */
	bool HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Helper to create JSON response */
	TUniquePtr<FHttpServerResponse> CreateJsonResponse(const FString& JsonContent, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok);

	/** Helper to create error response */
	TUniquePtr<FHttpServerResponse> CreateErrorResponse(const FString& Message, EHttpServerResponseCodes Code = EHttpServerResponseCodes::BadRequest);

	/** Helper to create structured auth denial response */
	TUniquePtr<FHttpServerResponse> CreateAuthDeniedResponse(const FMCPHttpAuthValidationResult& AuthResult);

	/** Initialize per-session local MCP auth state */
	void InitializeSecurityState();

	/** Persist the runtime token handoff file for local Node bridge discovery */
	bool PersistAuthTokenFile() const;

	/** Runtime artifact paths */
	FString BuildAuthTokenFilePath() const;
	FString BuildDeniedAuditPath() const;

	/** Validate token and local-peer boundary for protected endpoints */
	FMCPHttpAuthValidationResult ValidateRequestAuth(const FHttpServerRequest& Request, const FString& Endpoint, const FString& ToolName);

	/** Attach non-secret auth metadata to tool params for downstream gates */
	void AttachAuthenticatedRequestMetadata(const TSharedRef<FJsonObject>& Params) const;

	/** Append a structured denied-attempt audit entry without logging secrets */
	void AppendDeniedAudit(const FString& Endpoint, const FString& ToolName, const FString& Reason, const FHttpServerRequest& Request, const TSharedPtr<FJsonObject>& ExtraData = nullptr) const;

	/** Audit policy/expert denials returned by the tool registry */
	void AuditToolDenyResult(const FString& Endpoint, const FString& ToolName, const FMCPToolResult& Result, const FHttpServerRequest& Request) const;

	/** Header/token helpers */
	FString ExtractRequestToken(const FHttpServerRequest& Request, bool& bOutHeaderPresent) const;
	bool IsRequestFromLocalPeer(const FHttpServerRequest& Request, FString& OutRemoteAddress) const;
	bool ConstantTimeTokenEquals(const FString& A, const FString& B) const;

private:
	/** HTTP router handle */
	TSharedPtr<IHttpRouter> HttpRouter;

	/** Route handles for cleanup */
	FHttpRouteHandle ListToolsHandle;
	FHttpRouteHandle ExecuteToolHandle;
	FHttpRouteHandle StatusHandle;

	/** Tool registry */
	TSharedPtr<FMCPToolRegistry> ToolRegistry;

	/** Server state */
	bool bIsRunning;
	uint32 ServerPort;

	/** Per-editor-session MCP auth token; never log this value */
	FString SessionToken;
	FString SessionId;
	FString AuthTokenFilePath;
	FString DeniedAuditPath;
};
