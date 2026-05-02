// Copyright Natali Caggiano. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MCP/UnrealClaudeMCPServer.h"
#include "MCP/MCPToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	const FString Packet690Token = TEXT("packet690-test-token");

	FString MakePacket690TestRoot()
	{
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Logs/packet690_auth_boundary_tests"));
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	FHttpServerRequest MakePacket690Request(const FString& Token = FString())
	{
		FHttpServerRequest Request;
		Request.RelativePath = FHttpPath(TEXT("/mcp/tool/get_output_log"));
		Request.Verb = EHttpServerRequestVerbs::VERB_POST;
		if (!Token.IsEmpty())
		{
			Request.Headers.Add(TEXT("X-UnrealClaude-MCP-Token"), { Token });
		}
		return Request;
	}

	class FPacket690ReadOnlyFixtureTool : public IMCPTool
	{
	public:
		virtual FMCPToolInfo GetInfo() const override
		{
			FMCPToolInfo Info;
			Info.Name = TEXT("packet690_readonly_fixture");
			Info.Description = TEXT("Packet 690 read-only auth boundary fixture");
			Info.Annotations = FMCPToolAnnotations::ReadOnly();
			return Info;
		}

		virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("result_type"), TEXT("packet690_readonly_fixture"));
			return FMCPToolResult::Success(TEXT("packet690 read-only fixture executed"), Data);
		}
	};

	bool LoadAuditFile(const FString& Path, FString& OutText)
	{
		return FFileHelper::LoadFileToString(OutText, *Path);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAuthBoundary_MissingTokenRejectsAndAudits,
	"UnrealClaude.MCP.AuthBoundary.MissingTokenRejectsAndAudits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAuthBoundary_MissingTokenRejectsAndAudits::RunTest(const FString& Parameters)
{
	const FString AuditPath = MakePacket690TestRoot() / TEXT("missing_token_audit.jsonl");
	IFileManager::Get().Delete(*AuditPath);

	FUnrealClaudeMCPServer Server;
	Server.SetSessionTokenForTests(Packet690Token);
	Server.SetDeniedAuditPathForTests(AuditPath);

	const FMCPHttpAuthValidationResult Result = Server.ValidateRequestAuthForTests(
		MakePacket690Request(),
		TEXT("/mcp/tool/get_output_log"),
		TEXT("get_output_log"));

	TestFalse(TEXT("Missing token should reject protected tool execution"), Result.bAllowed);
	TestEqual(TEXT("Missing token reason should be structured"), Result.Reason, TEXT("missing_token"));
	TestEqual(TEXT("Missing token should use 401"), Result.ResponseCode, EHttpServerResponseCodes::Denied);

	FString AuditText;
	TestTrue(TEXT("Missing token should write denied audit"), LoadAuditFile(AuditPath, AuditText));
	TestTrue(TEXT("Audit should include reason"), AuditText.Contains(TEXT("\"reason\": \"missing_token\"")));
	TestFalse(TEXT("Audit must not leak session token"), AuditText.Contains(Packet690Token));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAuthBoundary_InvalidTokenRejectsAndAudits,
	"UnrealClaude.MCP.AuthBoundary.InvalidTokenRejectsAndAudits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAuthBoundary_InvalidTokenRejectsAndAudits::RunTest(const FString& Parameters)
{
	const FString AuditPath = MakePacket690TestRoot() / TEXT("invalid_token_audit.jsonl");
	IFileManager::Get().Delete(*AuditPath);

	FUnrealClaudeMCPServer Server;
	Server.SetSessionTokenForTests(Packet690Token);
	Server.SetDeniedAuditPathForTests(AuditPath);

	const FMCPHttpAuthValidationResult Result = Server.ValidateRequestAuthForTests(
		MakePacket690Request(TEXT("wrong-token")),
		TEXT("/mcp/tool/get_output_log"),
		TEXT("get_output_log"));

	TestFalse(TEXT("Invalid token should reject protected tool execution"), Result.bAllowed);
	TestTrue(TEXT("Invalid-token request should record header presence"), Result.bTokenHeaderPresent);
	TestEqual(TEXT("Invalid token reason should be structured"), Result.Reason, TEXT("invalid_token"));

	FString AuditText;
	TestTrue(TEXT("Invalid token should write denied audit"), LoadAuditFile(AuditPath, AuditText));
	TestTrue(TEXT("Audit should include invalid_token reason"), AuditText.Contains(TEXT("\"reason\": \"invalid_token\"")));
	TestFalse(TEXT("Audit must not leak invalid token value"), AuditText.Contains(TEXT("wrong-token")));
	TestFalse(TEXT("Audit must not leak session token"), AuditText.Contains(Packet690Token));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAuthBoundary_ValidTokenAllowsReadOnlyFixture,
	"UnrealClaude.MCP.AuthBoundary.ValidTokenAllowsReadOnlyFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAuthBoundary_ValidTokenAllowsReadOnlyFixture::RunTest(const FString& Parameters)
{
	FUnrealClaudeMCPServer Server;
	Server.SetSessionTokenForTests(Packet690Token);

	const FMCPHttpAuthValidationResult AuthResult = Server.ValidateRequestAuthForTests(
		MakePacket690Request(Packet690Token),
		TEXT("/mcp/tool/packet690_readonly_fixture"),
		TEXT("packet690_readonly_fixture"));

	TestTrue(TEXT("Valid token should authorize protected tool execution"), AuthResult.bAllowed);

	FMCPToolRegistry Registry;
	Registry.RegisterTool(MakeShared<FPacket690ReadOnlyFixtureTool>());
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	const FMCPToolResult ToolResult = Registry.ExecuteTool(TEXT("packet690_readonly_fixture"), Params);

	TestTrue(TEXT("Authorized read-only fixture should execute"), ToolResult.bSuccess);
	TestTrue(TEXT("Fixture should return data"), ToolResult.Data.IsValid());
	if (ToolResult.Data.IsValid())
	{
		TestEqual(TEXT("Fixture result type"), ToolResult.Data->GetStringField(TEXT("result_type")), TEXT("packet690_readonly_fixture"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAuthBoundary_ExpertHighRiskDeniedWithoutArmedState,
	"UnrealClaude.MCP.AuthBoundary.ExpertHighRiskDeniedWithoutArmedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAuthBoundary_ExpertHighRiskDeniedWithoutArmedState::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));
	Params->SetBoolField(TEXT("_mcp_http_auth_valid"), true);
	Params->SetBoolField(TEXT("_mcp_expert_session_armed"), false);

	FMCPToolResult Result;
	TestTrue(TEXT("High-risk explicit expert tool should be governed"), Registry.TryBuildGovernanceDenyResult(TEXT("execute_script"), Params, Result));
	TestFalse(TEXT("Unarmed expert request should be denied"), Result.bSuccess);
	TestTrue(TEXT("Deny result should include structured data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("Result type should be policy_denied"), Result.Data->GetStringField(TEXT("result_type")), TEXT("policy_denied"));
		TestEqual(TEXT("Error category should be expert_unarmed"), Result.Data->GetStringField(TEXT("error_category")), TEXT("expert_unarmed"));
		TestTrue(TEXT("HTTP auth should be reflected as valid"), Result.Data->GetBoolField(TEXT("mcp_http_auth_valid")));
		TestFalse(TEXT("Expert armed state should be reflected as false"), Result.Data->GetBoolField(TEXT("expert_session_armed")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAuthBoundary_ExpertDeniedAttemptAuditNoSecret,
	"UnrealClaude.MCP.AuthBoundary.ExpertDeniedAttemptAuditNoSecret",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAuthBoundary_ExpertDeniedAttemptAuditNoSecret::RunTest(const FString& Parameters)
{
	const FString AuditPath = MakePacket690TestRoot() / TEXT("expert_unarmed_audit.jsonl");
	IFileManager::Get().Delete(*AuditPath);

	FUnrealClaudeMCPServer Server;
	Server.SetSessionTokenForTests(Packet690Token);
	Server.SetDeniedAuditPathForTests(AuditPath);
	Server.AppendDeniedAuditForTests(
		TEXT("/mcp/tool/execute_script"),
		TEXT("execute_script"),
		TEXT("expert_unarmed"),
		MakePacket690Request(Packet690Token));

	FString AuditText;
	TestTrue(TEXT("Expert denial should write audit"), LoadAuditFile(AuditPath, AuditText));
	TestTrue(TEXT("Audit should include expert_unarmed reason"), AuditText.Contains(TEXT("\"reason\": \"expert_unarmed\"")));
	TestTrue(TEXT("Audit should include high-risk tool name"), AuditText.Contains(TEXT("\"tool_name\": \"execute_script\"")));
	TestTrue(TEXT("Audit should mark token header present"), AuditText.Contains(TEXT("\"token_header_present\": true")));
	TestFalse(TEXT("Audit must not leak session token"), AuditText.Contains(Packet690Token));

	return true;
}

#endif
