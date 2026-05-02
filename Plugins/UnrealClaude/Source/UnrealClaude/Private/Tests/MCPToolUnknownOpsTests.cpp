// Copyright Natali Caggiano. All Rights Reserved.

/**
 * Unit tests for SP1-S5 — Unified "Unknown operation" UX across MCP tool domains.
 *
 * Each test invokes the tool with an obviously-invalid operation and asserts that
 * the returned error message follows the shape established by
 * MCPTool_CharacterData.cpp:69 — "Unknown <domain> operation: '%s'. Valid: op1, op2, ...".
 *
 * Scope (W1-L3): AI, Multiplayer, Niagara, Sequencer, AnimBlueprintModify, CppReflection.
 * GAS is covered separately in W2-L4 as part of the MCPTool_GAS.cpp file-split pilot.
 */

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "MCP/Tools/MCPTool_AI.h"
#include "MCP/Tools/MCPTool_Multiplayer.h"
#include "MCP/Tools/MCPTool_Niagara.h"
#include "MCP/Tools/MCPTool_Sequencer.h"
#include "MCP/Tools/MCPTool_AnimBlueprintModify.h"
#include "MCP/Tools/MCPTool_CppReflection.h"
#include "MCP/MCPToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr const TCHAR* kBogusOperation = TEXT("definitely_not_a_real_op_xyz");

	TSharedRef<FJsonObject> MakeBogusOpParams()
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), kBogusOperation);
		return Params;
	}
}

// ===== AI =====

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPTool_AI_UnknownOp_FormatAndValidList,
	"UnrealClaude.MCP.Tools.UnknownOps.AI.FormatAndValidList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPTool_AI_UnknownOp_FormatAndValidList::RunTest(const FString& Parameters)
{
	FMCPTool_AI Tool;
	FMCPToolResult Result = Tool.Execute(MakeBogusOpParams());

	TestFalse("Unknown op should fail", Result.bSuccess);
	TestTrue("Error should start with 'Unknown AI operation:'",
		Result.Message.Contains(TEXT("Unknown AI operation:")));
	TestTrue("Error should include the bogus op name in quotes",
		Result.Message.Contains(TEXT("'definitely_not_a_real_op_xyz'")));
	TestTrue("Error should list 'Valid:' valid ops",
		Result.Message.Contains(TEXT("Valid:")));
	TestTrue("Error should name at least one real AI op (list_behavior_trees)",
		Result.Message.Contains(TEXT("list_behavior_trees")));
	TestTrue("Error should name at least one real AI op (configure_ai_foundation)",
		Result.Message.Contains(TEXT("configure_ai_foundation")));

	return true;
}

// ===== Multiplayer =====

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPTool_Multiplayer_UnknownOp_FormatAndValidList,
	"UnrealClaude.MCP.Tools.UnknownOps.Multiplayer.FormatAndValidList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPTool_Multiplayer_UnknownOp_FormatAndValidList::RunTest(const FString& Parameters)
{
	FMCPTool_Multiplayer Tool;
	FMCPToolResult Result = Tool.Execute(MakeBogusOpParams());

	TestFalse("Unknown op should fail", Result.bSuccess);
	TestTrue("Error should start with 'Unknown multiplayer operation:'",
		Result.Message.Contains(TEXT("Unknown multiplayer operation:")));
	TestTrue("Error should include the bogus op name in quotes",
		Result.Message.Contains(TEXT("'definitely_not_a_real_op_xyz'")));
	TestTrue("Error should list 'Valid:' valid ops",
		Result.Message.Contains(TEXT("Valid:")));
	TestTrue("Error should name at least one real multiplayer op (get_replication_info)",
		Result.Message.Contains(TEXT("get_replication_info")));
	TestTrue("Error should name at least one real multiplayer op (audit_live_replication)",
		Result.Message.Contains(TEXT("audit_live_replication")));

	return true;
}

// ===== Niagara =====

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPTool_Niagara_UnknownOp_FormatAndValidList,
	"UnrealClaude.MCP.Tools.UnknownOps.Niagara.FormatAndValidList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPTool_Niagara_UnknownOp_FormatAndValidList::RunTest(const FString& Parameters)
{
	FMCPTool_Niagara Tool;
	FMCPToolResult Result = Tool.Execute(MakeBogusOpParams());

	TestFalse("Unknown op should fail", Result.bSuccess);
	TestTrue("Error should start with 'Unknown Niagara operation:'",
		Result.Message.Contains(TEXT("Unknown Niagara operation:")));
	TestTrue("Error should include the bogus op name in quotes",
		Result.Message.Contains(TEXT("'definitely_not_a_real_op_xyz'")));
	TestTrue("Error should list 'Valid:' valid ops",
		Result.Message.Contains(TEXT("Valid:")));
	TestTrue("Error should name at least one real Niagara op (list_systems)",
		Result.Message.Contains(TEXT("list_systems")));
	TestTrue("Error should name at least one real Niagara op (set_system_parameter)",
		Result.Message.Contains(TEXT("set_system_parameter")));

	return true;
}

// ===== Sequencer =====

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPTool_Sequencer_UnknownOp_FormatAndValidList,
	"UnrealClaude.MCP.Tools.UnknownOps.Sequencer.FormatAndValidList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPTool_Sequencer_UnknownOp_FormatAndValidList::RunTest(const FString& Parameters)
{
	FMCPTool_Sequencer Tool;
	FMCPToolResult Result = Tool.Execute(MakeBogusOpParams());

	TestFalse("Unknown op should fail", Result.bSuccess);
	TestTrue("Error should start with 'Unknown sequencer operation:'",
		Result.Message.Contains(TEXT("Unknown sequencer operation:")));
	TestTrue("Error should include the bogus op name in quotes",
		Result.Message.Contains(TEXT("'definitely_not_a_real_op_xyz'")));
	TestTrue("Error should list 'Valid:' valid ops",
		Result.Message.Contains(TEXT("Valid:")));
	TestTrue("Error should name at least one real sequencer op (list_sequences)",
		Result.Message.Contains(TEXT("list_sequences")));
	TestTrue("Error should name at least one real sequencer op (configure_sequence_foundation)",
		Result.Message.Contains(TEXT("configure_sequence_foundation")));

	return true;
}

// ===== AnimBlueprintModify =====

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPTool_AnimBlueprintModify_UnknownOp_FormatAndValidList,
	"UnrealClaude.MCP.Tools.UnknownOps.AnimBlueprintModify.FormatAndValidList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPTool_AnimBlueprintModify_UnknownOp_FormatAndValidList::RunTest(const FString& Parameters)
{
	// AnimBlueprintModify::Execute validates blueprint_path before operation,
	// so we need a well-formed (but not necessarily existing) /Game/ path to reach
	// the unknown-op branch.
	FMCPTool_AnimBlueprintModify Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/Test/ABP_Unknown_Op_Probe"));
	Params->SetStringField(TEXT("operation"), kBogusOperation);

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Unknown op should fail", Result.bSuccess);
	TestTrue("Error should start with 'Unknown anim_blueprint_modify operation:'",
		Result.Message.Contains(TEXT("Unknown anim_blueprint_modify operation:")));
	TestTrue("Error should include the bogus op name in quotes",
		Result.Message.Contains(TEXT("'definitely_not_a_real_op_xyz'")));
	TestTrue("Error should list 'Valid:' valid ops",
		Result.Message.Contains(TEXT("Valid:")));
	TestTrue("Error should name at least one real anim op (get_info)",
		Result.Message.Contains(TEXT("get_info")));
	TestTrue("Error should name at least one real anim op (setup_transition_conditions)",
		Result.Message.Contains(TEXT("setup_transition_conditions")));

	return true;
}

// ===== CppReflection =====

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPTool_CppReflection_UnknownOp_FormatAndValidList,
	"UnrealClaude.MCP.Tools.UnknownOps.CppReflection.FormatAndValidList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPTool_CppReflection_UnknownOp_FormatAndValidList::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;
	FMCPToolResult Result = Tool.Execute(MakeBogusOpParams());

	TestFalse("Unknown op should fail", Result.bSuccess);
	TestTrue("Error should start with 'Unknown cpp_reflection operation:'",
		Result.Message.Contains(TEXT("Unknown cpp_reflection operation:")));
	TestTrue("Error should include the bogus op name in quotes",
		Result.Message.Contains(TEXT("'definitely_not_a_real_op_xyz'")));
	TestTrue("Error should list 'Valid:' valid ops",
		Result.Message.Contains(TEXT("Valid:")));
	TestTrue("Error should name at least one real cpp_reflection op (list_reflected_contracts)",
		Result.Message.Contains(TEXT("list_reflected_contracts")));
	TestTrue("Error should name at least one real cpp_reflection op (apply_property_metadata_mutation)",
		Result.Message.Contains(TEXT("apply_property_metadata_mutation")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPTool_GenericError_StructuredFailureData,
	"UnrealClaude.MCP.Tools.ErrorResult.StructuredFailureData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPTool_GenericError_StructuredFailureData::RunTest(const FString& Parameters)
{
	const FMCPToolResult Result = FMCPToolResult::Error(TEXT("Missing required parameter: actor_name"));

	TestFalse("Generic error should fail", Result.bSuccess);
	TestTrue("Generic error should preserve message", Result.Message.Contains(TEXT("actor_name")));
	TestTrue("Generic error should return structured data", Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestEqual("result_type should identify a tool error",
		Result.Data->GetStringField(TEXT("result_type")),
		FString(TEXT("tool_error")));
	TestEqual("schema_version should be stable",
		Result.Data->GetStringField(TEXT("schema_version")),
		FString(TEXT("mcp_tool_error.v1")));
	TestEqual("error_category should classify missing parameters",
		Result.Data->GetStringField(TEXT("error_category")),
		FString(TEXT("missing_required_parameter")));
	TestEqual("error_message should preserve the original message",
		Result.Data->GetStringField(TEXT("error_message")),
		FString(TEXT("Missing required parameter: actor_name")));
	TestFalse("Generic error should not masquerade as policy denial",
		Result.Data->HasField(TEXT("policy_denied_contract")));

	const FMCPToolResult LoadFailure = FMCPToolResult::Error(TEXT("Could not load Blueprint: /Game/Missing/BP_Missing"));
	TestTrue("Load failure should return structured data", LoadFailure.Data.IsValid());
	if (!LoadFailure.Data.IsValid())
	{
		return false;
	}
	TestEqual("load failures should classify as not_found",
		LoadFailure.Data->GetStringField(TEXT("error_category")),
		FString(TEXT("not_found")));

	const FMCPToolResult InvalidPath = FMCPToolResult::Error(TEXT("Blueprint path contains invalid character: ':'"));
	TestTrue("Invalid path should return structured data", InvalidPath.Data.IsValid());
	if (!InvalidPath.Data.IsValid())
	{
		return false;
	}
	TestEqual("invalid path character should classify as invalid_path",
		InvalidPath.Data->GetStringField(TEXT("error_category")),
		FString(TEXT("invalid_path")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
