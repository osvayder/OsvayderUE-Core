// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 619 P3 tests for FUnrealClaudeRelayAgentManager::LoadActivePlanWithFallback.
 *
 * Covers spec Test plan `UnrealClaude.RelayAgent.PostReattach.*`:
 *   - PostReattach_ReadsActivePlanJson_WhenPresent
 *   - PostReattach_FallsBackToStateJson_WhenActivePlanMissing
 *   - PostReattach_GracefulWarning_WhenBothMissing_DoesNotFailWidget
 *
 * Uses the existing SetTestStateRootOverride seam so tests share a fresh
 * filesystem root per test; no new override mechanism required. Each
 * test seeds exactly the files the fallback chain should see.
 */

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClaudeRelayAgent.h"
#include "UnrealClaudeRestartSurvival.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UnrealClaude::RelayAgentPostReattachTests
{
	FString MakeFreshTestRoot(const FString& TestName)
	{
		FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UnrealClaude"),
			TEXT("Automation"),
			TEXT("RelayAgentPostReattach"),
			TestName);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	struct FScopedStateRootOverride
	{
		explicit FScopedStateRootOverride(const FString& InDir)
		{
			FUnrealClaudeRestartSurvivalManager::SetTestStateRootOverride(InDir);
		}

		~FScopedStateRootOverride()
		{
			FUnrealClaudeRestartSurvivalManager::ClearTestStateRootOverride();
		}
	};

	bool WriteMinimalActivePlanJson(const FString& Path, const FString& OriginalUserTask, const FString& LastCompletedToolCallId)
	{
		// Minimum set of fields that ParseActivePlan requires not to reject the file.
		// We deliberately keep this lean so the test exercises the READ path, not the full
		// author path (BuildFallbackPlan covers that).
		const FString Json = FString::Printf(
			TEXT("{\n")
			TEXT("  \"schema_version\": 1,\n")
			TEXT("  \"plan_id\": \"plan_test_active\",\n")
			TEXT("  \"reviewer_plan_reference\": \"test_plan_v1\",\n")
			TEXT("  \"original_user_task\": \"%s\",\n")
			TEXT("  \"status\": \"active\",\n")
			TEXT("  \"result_status\": \"incomplete\",\n")
			TEXT("  \"created_at_utc\": \"2026-04-19T00:00:00Z\",\n")
			TEXT("  \"updated_at_utc\": \"2026-04-19T00:00:00Z\",\n")
			TEXT("  \"current_mechanic_id\": \"verify_and_report\",\n")
			TEXT("  \"handoff_policy\": \"full_batch_default\",\n")
			TEXT("  \"last_completed_tool_call_id\": \"%s\",\n")
			TEXT("  \"feature_workflow\": {\n")
			TEXT("    \"feature_workflow_id\": \"feature_post_reattach_1\",\n")
			TEXT("    \"recipe_id\": \"feature.inventory_basic_ui_v1\",\n")
			TEXT("    \"current_phase\": \"runtime_proof\",\n")
			TEXT("    \"compile_proof_required\": true,\n")
			TEXT("    \"compile_proof_state\": \"passed\",\n")
			TEXT("    \"runtime_proof_required\": true,\n")
			TEXT("    \"runtime_proof_state\": \"pending\",\n")
			TEXT("    \"phases\": [\n")
			TEXT("      { \"phase_id\": \"runtime_proof\", \"label\": \"Runtime proof\", \"status\": \"in_progress\", \"attempt_count\": 1, \"failure_count\": 0 }\n")
			TEXT("    ]\n")
			TEXT("  },\n")
			TEXT("  \"mechanics\": [\n")
			TEXT("    { \"step\": 1, \"mechanic_id\": \"verify_and_report\", \"label\": \"Verify\", \"label_ru\": \"Verify\", \"status\": \"pending\" }\n")
			TEXT("  ]\n")
			TEXT("}"),
			*OriginalUserTask,
			*LastCompletedToolCallId);
		return FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool WriteMinimalStateJson(const FString& Path, const FString& PostReattachCompletionText)
	{
		// Seeds just the post_reattach_completion_text field so the fallback
		// path is exercised. Other state.json fields default to empty.
		const FString Json = FString::Printf(
			TEXT("{\n")
			TEXT("  \"schema_version\": 2,\n")
			TEXT("  \"session_id\": \"session_post_reattach_test\",\n")
			TEXT("  \"task_id\": \"task_post_reattach_test\",\n")
			TEXT("  \"phase\": \"AwaitingReattach\",\n")
			TEXT("  \"post_reattach_completion_text\": \"%s\",\n")
			TEXT("  \"post_reattach_completion_pending\": true\n")
			TEXT("}"),
			*PostReattachCompletionText.Replace(TEXT("\""), TEXT("\\\"")));
		return FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

using namespace UnrealClaude::RelayAgentPostReattachTests;

// ------------------------------------------------------------------------
// Test 1: active_plan.json present -> read it, source = ActivePlanJson.
// (Back-compat: happy-path behavior unchanged.)
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_PostReattach_ReadsActivePlanJson_WhenPresent,
	"UnrealClaude.RelayAgent.PostReattach.ReadsActivePlanJson_WhenPresent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRelayAgent_PostReattach_ReadsActivePlanJson_WhenPresent::RunTest(const FString& /*Parameters*/)
{
	const FString Root = MakeFreshTestRoot(TEXT("ReadsActivePlanJson_WhenPresent"));
	FScopedStateRootOverride Scope(Root);

	// Seed both files so we can prove active_plan.json wins (back-compat).
	const FString ActivePlanPath = FUnrealClaudeRelayAgentManager::GetActivePlanPath();
	TestTrue(
		TEXT("active_plan.json seed should write"),
		WriteMinimalActivePlanJson(ActivePlanPath, TEXT("active-plan-task"), TEXT("tool_call_active_plan")));

	const FString StatePath = FUnrealClaudeRestartSurvivalManager::GetStatePath();
	TestTrue(
		TEXT("state.json seed should write"),
		WriteMinimalStateJson(StatePath, TEXT("state-json-completion-text")));

	FUnrealClaudeActivePlan Plan;
	FUnrealClaudeRelayAgentManager::EActivePlanFallbackSource Source
		= FUnrealClaudeRelayAgentManager::EActivePlanFallbackSource::NoneRecoverable;
	FString Error;
	const bool bLoaded = FUnrealClaudeRelayAgentManager::LoadActivePlanWithFallback(
		TEXT("user prompt"), Plan, Source, Error);

	TestTrue(TEXT("LoadActivePlanWithFallback should succeed on happy path"), bLoaded);
	TestEqual(TEXT("Source should be ActivePlanJson"),
		static_cast<int32>(Source),
		static_cast<int32>(FUnrealClaudeRelayAgentManager::EActivePlanFallbackSource::ActivePlanJson));
	TestEqual(TEXT("Plan.OriginalUserTask should match active_plan.json seed (not state.json)"),
		Plan.OriginalUserTask, FString(TEXT("active-plan-task")));
	TestEqual(TEXT("Plan.LastCompletedToolCallId should carry through from active_plan.json"),
		Plan.LastCompletedToolCallId, FString(TEXT("tool_call_active_plan")));
	TestEqual(TEXT("feature workflow id should be preserved on active_plan happy path"),
		Plan.FeatureWorkflow.FeatureWorkflowId, FString(TEXT("feature_post_reattach_1")));
	TestEqual(TEXT("feature workflow recipe should be preserved on active_plan happy path"),
		Plan.FeatureWorkflow.RecipeId, FString(TEXT("feature.inventory_basic_ui_v1")));
	TestEqual(TEXT("feature workflow phase should be preserved on active_plan happy path"),
		Plan.FeatureWorkflow.CurrentPhase, FString(TEXT("runtime_proof")));
	TestTrue(TEXT("Error should be empty on happy path"), Error.IsEmpty());
	return true;
}

// ------------------------------------------------------------------------
// Test 2: active_plan.json absent + state.json.post_reattach_completion_text
// present -> synthesize from state.json, source = RestartSurvivalStateJsonFallback.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_PostReattach_FallsBackToStateJson_WhenActivePlanMissing,
	"UnrealClaude.RelayAgent.PostReattach.FallsBackToStateJson_WhenActivePlanMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRelayAgent_PostReattach_FallsBackToStateJson_WhenActivePlanMissing::RunTest(const FString& /*Parameters*/)
{
	const FString Root = MakeFreshTestRoot(TEXT("FallsBackToStateJson_WhenActivePlanMissing"));
	FScopedStateRootOverride Scope(Root);

	// Only seed state.json. active_plan.json is deliberately absent.
	const FString StatePath = FUnrealClaudeRestartSurvivalManager::GetStatePath();
	const FString ContinuationText = TEXT("continue from post_reattach_completion_text recovered out of state.json");
	TestTrue(TEXT("state.json seed should write"), WriteMinimalStateJson(StatePath, ContinuationText));

	// Verify active_plan.json really is absent (defensive guard against stale fixtures).
	const FString ActivePlanPath = FUnrealClaudeRelayAgentManager::GetActivePlanPath();
	TestFalse(
		TEXT("active_plan.json must be absent in this test"),
		IFileManager::Get().FileExists(*ActivePlanPath));

	FUnrealClaudeActivePlan Plan;
	FUnrealClaudeRelayAgentManager::EActivePlanFallbackSource Source
		= FUnrealClaudeRelayAgentManager::EActivePlanFallbackSource::NoneRecoverable;
	FString Error;
	const bool bLoaded = FUnrealClaudeRelayAgentManager::LoadActivePlanWithFallback(
		TEXT("user-issued prompt"), Plan, Source, Error);

	TestTrue(TEXT("LoadActivePlanWithFallback should succeed on state.json fallback path"), bLoaded);
	TestEqual(
		TEXT("Source should be RestartSurvivalStateJsonFallback"),
		static_cast<int32>(Source),
		static_cast<int32>(FUnrealClaudeRelayAgentManager::EActivePlanFallbackSource::RestartSurvivalStateJsonFallback));
	TestEqual(
		TEXT("ReviewerPlanReference should mark this as the state.json fallback shape"),
		Plan.ReviewerPlanReference,
		FString(TEXT("post_reattach_state_json_fallback_plan_v1")));
	TestTrue(TEXT("Error should be empty on fallback path"), Error.IsEmpty());
	TestFalse(
		TEXT("Synthesized plan must be usable (non-empty ReviewerPlanReference + Mechanics)"),
		Plan.Mechanics.Num() == 0);
	return true;
}

// ------------------------------------------------------------------------
// Test 3: both absent/empty -> graceful warning, fresh default plan,
// source = NoneRecoverable, LoadActivePlanWithFallback returns true
// so the widget does NOT route to [failed] at ClaudeEditorWidget.cpp:3661.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_PostReattach_GracefulWarning_WhenBothMissing_DoesNotFailWidget,
	"UnrealClaude.RelayAgent.PostReattach.GracefulWarning_WhenBothMissing_DoesNotFailWidget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRelayAgent_PostReattach_GracefulWarning_WhenBothMissing_DoesNotFailWidget::RunTest(const FString& /*Parameters*/)
{
	const FString Root = MakeFreshTestRoot(TEXT("GracefulWarning_WhenBothMissing_DoesNotFailWidget"));
	FScopedStateRootOverride Scope(Root);

	// Do NOT seed either file. Both absent.
	const FString ActivePlanPath = FUnrealClaudeRelayAgentManager::GetActivePlanPath();
	const FString StatePath = FUnrealClaudeRestartSurvivalManager::GetStatePath();
	TestFalse(
		TEXT("active_plan.json must be absent in this test"),
		IFileManager::Get().FileExists(*ActivePlanPath));
	TestFalse(
		TEXT("state.json must be absent in this test"),
		IFileManager::Get().FileExists(*StatePath));

	const FString Prompt = TEXT("user-issued prompt with no continuation recoverable");
	FUnrealClaudeActivePlan Plan;
	FUnrealClaudeRelayAgentManager::EActivePlanFallbackSource Source
		= FUnrealClaudeRelayAgentManager::EActivePlanFallbackSource::ActivePlanJson;
	FString Error;
	const bool bLoaded = FUnrealClaudeRelayAgentManager::LoadActivePlanWithFallback(
		Prompt, Plan, Source, Error);

	// Critical anti-[failed] semantic: returns TRUE even when nothing is recoverable.
	// This is the exact property the spec calls for -- `PrepareActivePlanForCurrentSend`
	// returning true means the widget's `LastRuntimeConnectionState = Failed` branch
	// at ClaudeEditorWidget.cpp:1260 is NEVER entered, and the [failed] UI at :3661
	// does NOT render.
	TestTrue(
		TEXT("LoadActivePlanWithFallback must return true in NoneRecoverable (prevents [failed] UI)"),
		bLoaded);
	TestEqual(
		TEXT("Source should be NoneRecoverable"),
		static_cast<int32>(Source),
		static_cast<int32>(FUnrealClaudeRelayAgentManager::EActivePlanFallbackSource::NoneRecoverable));
	TestTrue(TEXT("Error should be empty on graceful-warning path"), Error.IsEmpty());

	// Synthesized plan should be a usable fresh plan for the current Prompt
	// (essentially equivalent to BuildDefaultActivePlan semantics).
	TestEqual(
		TEXT("Plan.OriginalUserTask should be the current prompt (fresh-session semantic)"),
		Plan.OriginalUserTask, Prompt);
	TestFalse(
		TEXT("Plan.Mechanics must not be empty"),
		Plan.Mechanics.Num() == 0);
	TestFalse(
		TEXT("Plan.ReviewerPlanReference must not be empty"),
		Plan.ReviewerPlanReference.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
