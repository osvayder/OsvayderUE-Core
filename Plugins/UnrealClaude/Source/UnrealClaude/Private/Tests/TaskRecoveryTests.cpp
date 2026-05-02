// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 633 Task Recovery x restart_survival coexistence unit tests.
 *
 * Covers:
 *   - Part 1 detector: stale-active detection fires / does not fire on fresh
 *     active without crash / does not fire on completed plan / crash-window
 *     classifies "previous_session_crashed" including the fresh-immediate
 *     crash override path.
 *   - Part 2 summarizer: groups consecutive same-tool calls / detects repeated
 *     errors as StallIndicator / handles zero tool calls without crash /
 *     ElapsedFormatting edge cases.
 *   - Part 3 injection: mirror of policy-routing single-slot — set context,
 *     consume once = returns text, consume again = returns empty.
 *
 * Reference: dispatch `AgentBridge/CODEX_TO_CLAUDE.md` 2026-04-20 14:15 (633).
 */

#include "CoreMinimal.h"
#include "AgentOrchestrator.h"
#include "ClaudeEditorWidget.h"
#include "ClaudeSubsystem.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UnrealClaudeDeferredTasksBrowser.h"
#include "UnrealClaudeCanonRouting.h"
#include "UnrealClaudeRelayAgent.h"
#include "UnrealClaudeRestartSurvival.h"
#include "UnrealClaudeTaskRecoveryDetector.h"
#include "UnrealClaudeTaskRecoveryDialog.h"
#include "UnrealClaudeTaskRecoverySummarizer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Build a minimal "fresh" plan with a few tool calls for testing. */
	FUnrealClaudeActivePlan BuildSynthPlan(const FString& UpdatedAtUtc,
	                                       const FString& Status,
	                                       const FString& ResultStatus)
	{
		FUnrealClaudeActivePlan Plan;
		Plan.PlanId = TEXT("plan_test_synth_001");
		Plan.OriginalUserTask = TEXT("Write a short blueprint that prints Hello World and verify via screenshot.");
		Plan.CreatedAtUtc = UpdatedAtUtc;
		Plan.UpdatedAtUtc = UpdatedAtUtc;
		Plan.Status = Status;
		Plan.ResultStatus = ResultStatus;
		Plan.CurrentMechanicId = TEXT("perform_bounded_work");
		Plan.CurrentAction = TEXT("Adding print node to graph");
		Plan.CurrentActionRu = TEXT("Adding print node to graph");
		Plan.Mechanics = {
			{ 1, TEXT("inspect_current_state"), TEXT("Inspect"), TEXT("Inspect"), TEXT("completed") },
			{ 1, TEXT("perform_bounded_work"), TEXT("Perform"), TEXT("Perform"), TEXT("in_progress") },
			{ 1, TEXT("verify_and_report"), TEXT("Verify"), TEXT("Verify"), TEXT("pending") }
		};
		return Plan;
	}

	FUnrealClaudePlanToolCallEntry MakeToolCall(const FString& ToolName,
	                                            const FString& Summary,
	                                            const FString& ResultStatus)
	{
		FUnrealClaudePlanToolCallEntry TC;
		TC.ToolName = ToolName;
		TC.Summary = Summary;
		TC.ResultStatus = ResultStatus;
		return TC;
	}

	FUnrealClaudeRestartSurvivalState BuildRestartState(
		const EUnrealClaudeRestartSurvivalPhase Phase,
		const FString& LastUpdatedAtUtc,
		const FString& DetachedTerminalOutcome = FString(),
		const FString& FailureReason = FString(),
		const bool bPostReattachCompletionPending = false,
		const bool bPostReattachCompletionDispatched = false)
	{
		FUnrealClaudeRestartSurvivalState State;
		State.Phase = Phase;
		State.LastUpdatedAtUtc = LastUpdatedAtUtc;
		State.DetachedTerminalOutcome = DetachedTerminalOutcome;
		State.FailureReason = FailureReason;
		State.bPostReattachCompletionPending = bPostReattachCompletionPending;
		State.bPostReattachCompletionDispatched = bPostReattachCompletionDispatched;
		return State;
	}

	FString MakeFreshDeferredBrowserTestRoot(const FString& TestName)
	{
		FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UnrealClaude"),
			TEXT("Automation"),
			TEXT("TaskRecovery"),
			TestName);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	struct FScopedTaskRecoveryRootOverride
	{
		explicit FScopedTaskRecoveryRootOverride(const FString& InDir)
		{
			FUnrealClaudeRestartSurvivalManager::SetTestStateRootOverride(InDir);
		}

		~FScopedTaskRecoveryRootOverride()
		{
			FUnrealClaudeRestartSurvivalManager::ClearTestStateRootOverride();
		}
	};

	FUnrealClaudeActivePlan BuildDeferredPlan(
		const FString& PlanId,
		const FString& UpdatedAtUtc,
		const FString& Status,
		const FString& OriginalTask)
	{
		FUnrealClaudeActivePlan Plan = BuildSynthPlan(UpdatedAtUtc, Status, TEXT("incomplete"));
		Plan.PlanId = PlanId;
		Plan.OriginalUserTask = OriginalTask;
		Plan.UpdatedAtUtc = UpdatedAtUtc;
		Plan.CreatedAtUtc = UpdatedAtUtc;
		Plan.Status = Status;
		Plan.InterruptionDetectedAtUtc = UpdatedAtUtc;
		Plan.InterruptionReason = TEXT("editor_startup_after_quiet_period");
		Plan.InterruptionElapsedSeconds = 3600;
		Plan.UserRecoveryChoice =
			Status == TEXT("abandoned_for_fresh_session")
				? TEXT("start_fresh")
				: (Status == TEXT("abandoned_by_user") ? TEXT("closed_as_irrelevant") : TEXT(""));
		Plan.UserClosedReason =
			Status == TEXT("abandoned_by_user")
				? TEXT("user parked this task for later")
				: FString();
		Plan.ToolCalls = {
			MakeToolCall(TEXT("blueprint_modify"), TEXT("Created movement graph"), TEXT("success")),
			MakeToolCall(TEXT("capture_viewport"), TEXT("Captured proof screenshot"), TEXT("success"))
		};
		return Plan;
	}

	bool SaveDeferredArchive(
		FAutomationTestBase& Test,
		const FUnrealClaudeActivePlan& Plan,
		const FString& ArchivePath)
	{
		FString Error;
		const bool bSaved = FUnrealClaudeRelayAgentManager::SavePlanToPath(ArchivePath, Plan, Error);
		Test.TestTrue(TEXT("deferred archive should save"), bSaved);
		if (!Error.IsEmpty())
		{
			Test.AddInfo(Error);
		}
		return bSaved;
	}
}

// ================================================================
// Test 1: Detector fires on stale active plan
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecoveryDetector_DetectsInterruptedStaleActive,
	"UnrealClaude.TaskRecovery.Detector.DetectsInterruptedStaleActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecoveryDetector_DetectsInterruptedStaleActive::RunTest(const FString& /*Parameters*/)
{
	// Plan updated 1 hour ago with status=active + result_status=incomplete.
	const FDateTime Now = FDateTime::UtcNow();
	const FDateTime OneHourAgo = Now - FTimespan::FromHours(1);

	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		OneHourAgo.ToIso8601(), TEXT("active"), TEXT("incomplete"));

	TArray<FDateTime> NoCrashes;
	const FTaskRecoveryDetectionResult Result =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(Plan, Now, NoCrashes);

	TestTrue(TEXT("Detection fires on 1-hour-stale active/incomplete plan"),
		Result.bInterruptionDetected);
	TestEqual(TEXT("Reason is editor_startup_after_quiet_period (no crash)"),
		Result.InterruptionReason, FString(TEXT("editor_startup_after_quiet_period")));
	TestTrue(TEXT("Elapsed seconds positive"), Result.ElapsedSeconds > 0);
	return true;
}

// ================================================================
// Test 2: Detector does NOT fire on fresh (within threshold, no crash)
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecoveryDetector_DoesNotFireOnFreshActive,
	"UnrealClaude.TaskRecovery.Detector.DoesNotFireOnFreshActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecoveryDetector_DoesNotFireOnFreshActive::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now = FDateTime::UtcNow();
	const FDateTime TwoMinsAgo = Now - FTimespan::FromMinutes(2);

	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		TwoMinsAgo.ToIso8601(), TEXT("active"), TEXT("incomplete"));

	TArray<FDateTime> NoCrashes;
	const FTaskRecoveryDetectionResult Result =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(Plan, Now, NoCrashes);

	TestFalse(TEXT("Detection does NOT fire on 2-minute-fresh plan"),
		Result.bInterruptionDetected);
	return true;
}

// ================================================================
// Test 3: Detector does NOT fire on completed plan
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecoveryDetector_DoesNotFireOnCompletedPlan,
	"UnrealClaude.TaskRecovery.Detector.DoesNotFireOnCompletedPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_FreshSessionDoesNotResumeStopLossedFeatureWorkflow,
	"UnrealClaude.TaskRecovery.FreshSessionDoesNotResumeStopLossedFeatureWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecoveryDetector_DoesNotFireOnCompletedPlan::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now = FDateTime::UtcNow();
	const FDateTime OneHourAgo = Now - FTimespan::FromHours(1);

	// Old, but result_status=achieved_fully → completed, must not fire.
	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		OneHourAgo.ToIso8601(), TEXT("active"), TEXT("achieved_fully"));

	TArray<FDateTime> NoCrashes;
	const FTaskRecoveryDetectionResult Result =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(Plan, Now, NoCrashes);

	TestFalse(TEXT("Detection does NOT fire when result_status==achieved_fully"),
		Result.bInterruptionDetected);
	return true;
}

bool FTaskRecovery_FreshSessionDoesNotResumeStopLossedFeatureWorkflow::RunTest(const FString& /*Parameters*/)
{
	const FString UpdatedAt = FDateTime::UtcNow().ToIso8601();
	FUnrealClaudeActivePlan StoppedPlan = BuildSynthPlan(UpdatedAt, TEXT("failed"), TEXT("not_achieved"));
	StoppedPlan.PlanId = TEXT("plan_stop_lossed_interaction_access");
	StoppedPlan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_old_stop_lossed");
	StoppedPlan.FeatureWorkflow.RecipeId = TEXT("feature.interaction_access_slice_v1");
	StoppedPlan.FeatureWorkflow.CurrentPhase = TEXT("input_asset_authoring");
	StoppedPlan.FeatureWorkflow.TerminalStatus = TEXT("stop_loss");
	StoppedPlan.FeatureWorkflow.bStopLossTriggered = true;
	StoppedPlan.FeatureWorkflow.StopLossReason =
		TEXT("command_execution_without_phase_advance_gt_5:proof_prerequisites_missing");
	StoppedPlan.FeatureWorkflow.BlockerFamily = TEXT("proof_prerequisites_missing");

	TestFalse(TEXT("fresh session should not resume a terminal stop-lossed feature workflow"),
		FAgentOrchestrator::ShouldResumeFeatureWorkflowFromActivePlan(StoppedPlan));

	TArray<FAgentPromptContextBlock> ContextBlocks;
	const FAgentCanonExecution FreshExecution =
		UnrealClaudeCanonRouting::BuildInitialCanonExecution(
			TEXT("Continue the prison-access slice on the existing proof map and persistent input assets without recreating them."),
			EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
			ContextBlocks);

	TestTrue(TEXT("fresh session should create/reset an interaction-access feature workflow"),
		FreshExecution.FeatureWorkflow.HasAnySignal());
	TestEqual(TEXT("fresh session workflow should begin at project_context_preflight"),
		FreshExecution.FeatureWorkflow.CurrentPhase,
		FString(TEXT("project_context_preflight")));
	TestFalse(TEXT("fresh session workflow must not inherit stop-loss state"),
		FreshExecution.FeatureWorkflow.bStopLossTriggered);
	TestNotEqual(TEXT("fresh session workflow should not reuse the old stopped workflow identity"),
		FreshExecution.FeatureWorkflow.FeatureWorkflowId,
		StoppedPlan.FeatureWorkflow.FeatureWorkflowId);
	return true;
}

// ================================================================
// Test 4: Detector classifies crash when mtime within window, including
// the fresh-crash override path below the quiet-period threshold.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecoveryDetector_CrashReasonDetected,
	"UnrealClaude.TaskRecovery.Detector.CrashReasonDetected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecoveryDetector_CrashReasonDetected::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now = FDateTime::UtcNow();
	const FDateTime OneHourAgo = Now - FTimespan::FromHours(1);
	// Crash report mtime 1 minute after plan's updated_at_utc — within ±2min window.
	const FDateTime CrashMtime = OneHourAgo + FTimespan::FromMinutes(1);

	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		OneHourAgo.ToIso8601(), TEXT("active"), TEXT("incomplete"));

	TArray<FDateTime> Crashes;
	Crashes.Add(CrashMtime);

	const FTaskRecoveryDetectionResult Result =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(Plan, Now, Crashes);

	TestTrue(TEXT("Stale crash path elapsed should be positive"), Result.ElapsedSeconds > 0);

	const FDateTime TwoMinsAgo = Now - FTimespan::FromMinutes(2);
	const FDateTime FreshCrashMtime = TwoMinsAgo + FTimespan::FromSeconds(30);
	FUnrealClaudeActivePlan FreshPlan = BuildSynthPlan(
		TwoMinsAgo.ToIso8601(), TEXT("active"), TEXT("incomplete"));

	TArray<FDateTime> FreshCrashes;
	FreshCrashes.Add(FreshCrashMtime);

	const FTaskRecoveryDetectionResult FreshResult =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(FreshPlan, Now, FreshCrashes);

	TestTrue(TEXT("Fresh crash evidence should override the quiet-period threshold"),
		FreshResult.bInterruptionDetected);
	TestEqual(TEXT("Fresh crash override should still classify as previous_session_crashed"),
		FreshResult.InterruptionReason,
		FString(TEXT("previous_session_crashed")));
	TestTrue(TEXT("Fresh crash override elapsed should remain below the normal stale threshold"),
		FreshResult.ElapsedSeconds > 0 && FreshResult.ElapsedSeconds < 300);

	TestTrue(TEXT("Detection fires"), Result.bInterruptionDetected);
	TestEqual(TEXT("Reason is previous_session_crashed when crash within ±2min window"),
		Result.InterruptionReason, FString(TEXT("previous_session_crashed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_SuppressesDialogDuringActiveRestartSurvival,
	"UnrealClaude.TaskRecovery.SuppressesDialogDuringActiveRestartSurvival",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_SuppressesDialogDuringActiveRestartSurvival::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now(2026, 4, 20, 12, 0, 0);
	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		(Now - FTimespan::FromHours(2)).ToIso8601(),
		TEXT("active"),
		TEXT("incomplete"));
	const TOptional<FUnrealClaudeRestartSurvivalState> RestartState = BuildRestartState(
		EUnrealClaudeRestartSurvivalPhase::DetachedRunning,
		(Now - FTimespan::FromMinutes(30)).ToIso8601());

	const TArray<FDateTime> NoCrashes;
	const FTaskRecoveryDetectionResult Result =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(Plan, Now, NoCrashes, RestartState);

	TestFalse(TEXT("active restart-survival should not fire interruption dialog"),
		Result.bInterruptionDetected);
	TestTrue(TEXT("active restart-survival should suppress dialog"),
		Result.bDialogSuppressed);
	TestEqual(TEXT("suppression reason should be active_restart_survival_in_progress"),
		Result.SuppressionReason,
		ETaskRecoverySuppressionReason::ActiveRestartSurvivalInProgress);
	TestEqual(TEXT("elapsed seconds should track restart state freshness"),
		Result.ElapsedSeconds,
		static_cast<int64>(30 * 60));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_SuppressesDialogDuringReattachCompletion,
	"UnrealClaude.TaskRecovery.SuppressesDialogDuringReattachCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_SuppressesDialogDuringReattachCompletion::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now(2026, 4, 20, 12, 0, 0);
	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		(Now - FTimespan::FromHours(3)).ToIso8601(),
		TEXT("active"),
		TEXT("incomplete"));
	const TOptional<FUnrealClaudeRestartSurvivalState> RestartState = BuildRestartState(
		EUnrealClaudeRestartSurvivalPhase::Reattached,
		(Now - FTimespan::FromMinutes(10)).ToIso8601(),
		FString(),
		FString(),
		true,
		false);

	const TArray<FDateTime> NoCrashes;
	const FTaskRecoveryDetectionResult Result =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(Plan, Now, NoCrashes, RestartState);

	TestFalse(TEXT("reattach completion should not fire interruption dialog"),
		Result.bInterruptionDetected);
	TestTrue(TEXT("reattach completion should suppress dialog"),
		Result.bDialogSuppressed);
	TestEqual(TEXT("suppression reason should be intentional continuation"),
		Result.SuppressionReason,
		ETaskRecoverySuppressionReason::IntentionalRestartSurvivalContinuation);
	TestEqual(TEXT("elapsed seconds should be driven by restart state"),
		Result.ElapsedSeconds,
		static_cast<int64>(10 * 60));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_FiresDialogAfterFailedRestartSurvival,
	"UnrealClaude.TaskRecovery.FiresDialogAfterFailedRestartSurvival",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_FiresDialogAfterFailedRestartSurvival::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now(2026, 4, 20, 12, 0, 0);
	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		(Now - FTimespan::FromMinutes(2)).ToIso8601(),
		TEXT("active"),
		TEXT("incomplete"));
	const TOptional<FUnrealClaudeRestartSurvivalState> RestartState = BuildRestartState(
		EUnrealClaudeRestartSurvivalPhase::FailedTerminal,
		(Now - FTimespan::FromMinutes(12)).ToIso8601(),
		TEXT("failed"),
		TEXT("Build.bat failed with exit code 6"));

	const TArray<FDateTime> NoCrashes;
	const FTaskRecoveryDetectionResult Result =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(Plan, Now, NoCrashes, RestartState);

	TestTrue(TEXT("failed restart-survival should fire interruption dialog"),
		Result.bInterruptionDetected);
	TestFalse(TEXT("failed restart-survival should not suppress dialog"),
		Result.bDialogSuppressed);
	TestEqual(TEXT("reason should describe failed restart-survival"),
		Result.InterruptionReason,
		FString(TEXT("previous_restart_survival_failed")));
	TestEqual(TEXT("elapsed seconds should be driven by restart failure timestamp"),
		Result.ElapsedSeconds,
		static_cast<int64>(12 * 60));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_FiresDialogAfterStuckSupervisor,
	"UnrealClaude.TaskRecovery.FiresDialogAfterStuckSupervisor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_FiresDialogAfterStuckSupervisor::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now(2026, 4, 20, 12, 0, 0);
	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		(Now - FTimespan::FromMinutes(2)).ToIso8601(),
		TEXT("active"),
		TEXT("incomplete"));
	const TOptional<FUnrealClaudeRestartSurvivalState> RestartState = BuildRestartState(
		EUnrealClaudeRestartSurvivalPhase::AwaitingReattach,
		(Now - FTimespan::FromHours(2)).ToIso8601());

	const TArray<FDateTime> NoCrashes;
	const FTaskRecoveryDetectionResult Result =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(Plan, Now, NoCrashes, RestartState);

	TestTrue(TEXT("stuck restart-survival should fire interruption dialog"),
		Result.bInterruptionDetected);
	TestFalse(TEXT("stuck restart-survival should not suppress dialog"),
		Result.bDialogSuppressed);
	TestEqual(TEXT("reason should describe stuck restart-survival"),
		Result.InterruptionReason,
		FString(TEXT("previous_restart_survival_stuck_or_crashed")));
	TestEqual(TEXT("elapsed seconds should be based on stale restart state"),
		Result.ElapsedSeconds,
		static_cast<int64>(2 * 60 * 60));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_FiresDialogAfterCleanRestartSurvivalLongAgo,
	"UnrealClaude.TaskRecovery.FiresDialogAfterCleanRestartSurvivalLongAgo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_FiresDialogAfterCleanRestartSurvivalLongAgo::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now(2026, 4, 20, 12, 0, 0);
	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		(Now - FTimespan::FromMinutes(2)).ToIso8601(),
		TEXT("active"),
		TEXT("incomplete"));
	const TOptional<FUnrealClaudeRestartSurvivalState> RestartState = BuildRestartState(
		EUnrealClaudeRestartSurvivalPhase::AttachedInEditor,
		(Now - FTimespan::FromMinutes(31)).ToIso8601(),
		TEXT("success"));

	const TArray<FDateTime> NoCrashes;
	const FTaskRecoveryDetectionResult Result =
		FUnrealClaudeTaskRecoveryDetector::ClassifyPlan(Plan, Now, NoCrashes, RestartState);

	TestTrue(TEXT("clean restart-survival long ago should fire interruption dialog"),
		Result.bInterruptionDetected);
	TestFalse(TEXT("clean restart-survival long ago should not suppress dialog"),
		Result.bDialogSuppressed);
	TestEqual(TEXT("reason should fall back to quiet-period interruption"),
		Result.InterruptionReason,
		FString(TEXT("editor_startup_after_quiet_period")));
	TestEqual(TEXT("elapsed seconds should be based on success timestamp"),
		Result.ElapsedSeconds,
		static_cast<int64>(31 * 60));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeferredBrowser_EnumeratesOnlyAbandonedStatuses,
	"UnrealClaude.TaskRecovery.DeferredBrowser.EnumeratesOnlyAbandonedStatuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeferredBrowser_EnumeratesOnlyAbandonedStatuses::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now(2026, 4, 20, 12, 0, 0);
	const FString TestRoot = MakeFreshDeferredBrowserTestRoot(TEXT("EnumeratesOnlyAbandonedStatuses"));
	FScopedTaskRecoveryRootOverride Override(TestRoot);

	const FString ArchiveDir = FUnrealClaudeRelayAgentManager::GetPlanArchiveDir();
	IFileManager::Get().MakeDirectory(*ArchiveDir, true);

	SaveDeferredArchive(
		*this,
		BuildDeferredPlan(
			TEXT("plan_success"),
			(Now - FTimespan::FromHours(3)).ToIso8601(),
			TEXT("achieved_fully"),
			TEXT("Completed task should stay hidden from deferred browser.")),
		FPaths::Combine(ArchiveDir, TEXT("20260420-plan_success.active_plan.json")));
	SaveDeferredArchive(
		*this,
		BuildDeferredPlan(
			TEXT("plan_abandoned"),
			(Now - FTimespan::FromHours(1)).ToIso8601(),
			TEXT("abandoned_by_user"),
			TEXT("Abandoned task should be listed first.")),
		FPaths::Combine(ArchiveDir, TEXT("20260420-plan_abandoned.active_plan.json")));
	SaveDeferredArchive(
		*this,
		BuildDeferredPlan(
			TEXT("plan_fresh"),
			(Now - FTimespan::FromMinutes(30)).ToIso8601(),
			TEXT("abandoned_for_fresh_session"),
			TEXT("Fresh-session task should also be listed.")),
		FPaths::Combine(ArchiveDir, TEXT("20260420-plan_fresh.active_plan.json")));

	const TArray<FUnrealClaudeDeferredPlanEntry> Entries =
		FUnrealClaudeDeferredTasksEnumerator::ListDeferredPlans();

	TestEqual(TEXT("enumerator should return only abandoned statuses"), Entries.Num(), 2);
	if (Entries.Num() == 2)
	{
		TestEqual(TEXT("newest deferred plan should sort first"), Entries[0].PlanId, FString(TEXT("plan_fresh")));
		TestEqual(TEXT("older deferred plan should sort second"), Entries[1].PlanId, FString(TEXT("plan_abandoned")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeferredBrowser_ResumeMovesFileAndClearsFields,
	"UnrealClaude.TaskRecovery.DeferredBrowser.ResumeMovesFileAndClearsFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeferredBrowser_ResumeMovesFileAndClearsFields::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now(2026, 4, 20, 12, 0, 0);
	const FString TestRoot = MakeFreshDeferredBrowserTestRoot(TEXT("ResumeMovesFileAndClearsFields"));
	FScopedTaskRecoveryRootOverride Override(TestRoot);

	FClaudeCodeSubsystem::Get().ConsumePendingTaskRecoveryContextForTests();

	const FString ArchiveDir = FUnrealClaudeRelayAgentManager::GetPlanArchiveDir();
	IFileManager::Get().MakeDirectory(*ArchiveDir, true);

	const FString ArchivePath = FPaths::Combine(ArchiveDir, TEXT("20260420-plan_resume.active_plan.json"));
	const FUnrealClaudeActivePlan ArchivedPlan = BuildDeferredPlan(
		TEXT("plan_resume"),
		(Now - FTimespan::FromHours(2)).ToIso8601(),
		TEXT("abandoned_by_user"),
		TEXT("Resume the archived telekinesis flight plan."));
	TestTrue(TEXT("archived deferred plan should save"), SaveDeferredArchive(*this, ArchivedPlan, ArchivePath));

	const FString ExistingActivePlanPath = FUnrealClaudeRelayAgentManager::GetActivePlanPath();
	const FUnrealClaudeActivePlan ExistingActivePlan = BuildDeferredPlan(
		TEXT("plan_existing"),
		(Now - FTimespan::FromMinutes(10)).ToIso8601(),
		TEXT("active"),
		TEXT("Current in-flight active plan should be backed up before resume."));
	TestTrue(TEXT("existing active plan should save"), SaveDeferredArchive(*this, ExistingActivePlan, ExistingActivePlanPath));

	FUnrealClaudeDeferredPlanEntry Entry;
	Entry.PlanId = TEXT("plan_resume");
	Entry.ArchiveFilePath = ArchivePath;
	Entry.OriginalUserTask = ArchivedPlan.OriginalUserTask;
	Entry.ShortTitle = TEXT("Resume the archived telekinesis flight plan.");
	Entry.Status = TEXT("abandoned_by_user");
	Entry.UserClosedReason = ArchivedPlan.UserClosedReason;
	Entry.UpdatedAtUtc = ArchivedPlan.UpdatedAtUtc;

	FString ResumedShortTitle;
	FString Error;
	TestTrue(TEXT("resume should succeed"), FUnrealClaudeDeferredTasksBrowser::ResumePlan(Entry, ResumedShortTitle, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
	}

	FUnrealClaudeActivePlan ActivePlan;
	TestTrue(TEXT("active plan should load after resume"), FUnrealClaudeRelayAgentManager::LoadActivePlan(ActivePlan, Error));
	TestEqual(TEXT("resumed plan should become active"), ActivePlan.Status, FString(TEXT("active")));
	TestEqual(TEXT("user_recovery_choice should be cleared"), ActivePlan.UserRecoveryChoice, FString());
	TestEqual(TEXT("user_closed_reason should be cleared"), ActivePlan.UserClosedReason, FString());
	TestEqual(TEXT("interruption_detected_at_utc should be cleared"), ActivePlan.InterruptionDetectedAtUtc, FString());
	TestEqual(TEXT("interruption_reason should be cleared"), ActivePlan.InterruptionReason, FString());
	TestEqual(TEXT("interruption_elapsed_seconds should reset"), ActivePlan.InterruptionElapsedSeconds, 0);
	TestEqual(TEXT("original task should survive resume"), ActivePlan.OriginalUserTask, ArchivedPlan.OriginalUserTask);
	TestFalse(TEXT("archive should be deleted after resume"), IFileManager::Get().FileExists(*ArchivePath));

	TArray<FString> BackupFiles;
	IFileManager::Get().FindFilesRecursive(
		BackupFiles,
		*ArchiveDir,
		TEXT("*.resume_backup.active_plan.json"),
		true,
		false,
		false);
	TestTrue(TEXT("existing active plan should be backed up before overwrite"), BackupFiles.Num() >= 1);

	FClaudeCodeSubsystem::Get().ConsumePendingTaskRecoveryContextForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeferredBrowser_ResumeQueuesContextBlock,
	"UnrealClaude.TaskRecovery.DeferredBrowser.ResumeQueuesContextBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeferredBrowser_ResumeQueuesContextBlock::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now(2026, 4, 20, 12, 0, 0);
	const FString TestRoot = MakeFreshDeferredBrowserTestRoot(TEXT("ResumeQueuesContextBlock"));
	FScopedTaskRecoveryRootOverride Override(TestRoot);

	FClaudeCodeSubsystem::Get().ConsumePendingTaskRecoveryContextForTests();

	const FString ArchiveDir = FUnrealClaudeRelayAgentManager::GetPlanArchiveDir();
	IFileManager::Get().MakeDirectory(*ArchiveDir, true);

	const FString ArchivePath = FPaths::Combine(ArchiveDir, TEXT("20260420-plan_queue.active_plan.json"));
	const FUnrealClaudeActivePlan ArchivedPlan = BuildDeferredPlan(
		TEXT("plan_queue"),
		(Now - FTimespan::FromMinutes(45)).ToIso8601(),
		TEXT("abandoned_for_fresh_session"),
		TEXT("Restore the teleport plus flight task from archives."));
	TestTrue(TEXT("queued archive should save"), SaveDeferredArchive(*this, ArchivedPlan, ArchivePath));

	FUnrealClaudeDeferredPlanEntry Entry;
	Entry.PlanId = TEXT("plan_queue");
	Entry.ArchiveFilePath = ArchivePath;
	Entry.OriginalUserTask = ArchivedPlan.OriginalUserTask;
	Entry.ShortTitle = TEXT("Restore the teleport plus flight task from archives.");
	Entry.Status = TEXT("abandoned_for_fresh_session");
	Entry.UpdatedAtUtc = ArchivedPlan.UpdatedAtUtc;

	FString ResumedShortTitle;
	FString Error;
	TestTrue(TEXT("resume should queue context successfully"), FUnrealClaudeDeferredTasksBrowser::ResumePlan(Entry, ResumedShortTitle, Error));

	const FString QueuedContext = FClaudeCodeSubsystem::Get().ConsumePendingTaskRecoveryContextForTests();
	TestFalse(TEXT("queued context should not be empty"), QueuedContext.IsEmpty());
	TestTrue(TEXT("queued context should include original deferred task"), QueuedContext.Contains(TEXT("teleport plus flight")));
	TestTrue(TEXT("queued context should include progress heading"), QueuedContext.Contains(TEXT("Progress so far:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeferredBrowser_DeleteRemovesArchiveFile,
	"UnrealClaude.TaskRecovery.DeferredBrowser.DeleteRemovesArchiveFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeferredBrowser_DeleteRemovesArchiveFile::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now(2026, 4, 20, 12, 0, 0);
	const FString TestRoot = MakeFreshDeferredBrowserTestRoot(TEXT("DeleteRemovesArchiveFile"));
	FScopedTaskRecoveryRootOverride Override(TestRoot);

	const FString ArchiveDir = FUnrealClaudeRelayAgentManager::GetPlanArchiveDir();
	IFileManager::Get().MakeDirectory(*ArchiveDir, true);

	const FString ArchivePath = FPaths::Combine(ArchiveDir, TEXT("20260420-plan_delete.active_plan.json"));
	const FUnrealClaudeActivePlan ArchivedPlan = BuildDeferredPlan(
		TEXT("plan_delete"),
		(Now - FTimespan::FromMinutes(20)).ToIso8601(),
		TEXT("abandoned_by_user"),
		TEXT("Delete this deferred task from archives."));
	TestTrue(TEXT("delete fixture archive should save"), SaveDeferredArchive(*this, ArchivedPlan, ArchivePath));

	FUnrealClaudeDeferredPlanEntry Entry;
	Entry.PlanId = TEXT("plan_delete");
	Entry.ArchiveFilePath = ArchivePath;
	Entry.OriginalUserTask = ArchivedPlan.OriginalUserTask;
	Entry.ShortTitle = TEXT("Delete this deferred task from archives.");
	Entry.Status = TEXT("abandoned_by_user");
	Entry.UserClosedReason = ArchivedPlan.UserClosedReason;
	Entry.UpdatedAtUtc = ArchivedPlan.UpdatedAtUtc;

	FString Error;
	TestTrue(TEXT("delete should succeed"), FUnrealClaudeDeferredTasksBrowser::DeletePlan(Entry, Error));
	TestFalse(TEXT("archive file should be removed"), IFileManager::Get().FileExists(*ArchivePath));
	return true;
}

// ================================================================
// Test 5: Summarizer groups consecutive same-tool calls
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecoverySummarizer_GroupsConsecutiveSameTool,
	"UnrealClaude.TaskRecovery.Summarizer.GroupsConsecutiveSameTool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecoverySummarizer_GroupsConsecutiveSameTool::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now = FDateTime::UtcNow();
	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		Now.ToIso8601(), TEXT("active"), TEXT("incomplete"));

	// 3× blueprint_modify in a row → should roll up into "Ran blueprint_modify 3 times (...)".
	Plan.ToolCalls.Add(MakeToolCall(TEXT("blueprint_modify"), TEXT("Added variable"), TEXT("success")));
	Plan.ToolCalls.Add(MakeToolCall(TEXT("blueprint_modify"), TEXT("Added variable"), TEXT("success")));
	Plan.ToolCalls.Add(MakeToolCall(TEXT("blueprint_modify"), TEXT("Added variable"), TEXT("success")));
	// Then 1× different tool.
	Plan.ToolCalls.Add(MakeToolCall(TEXT("capture_viewport"), TEXT("Took screenshot"), TEXT("success")));

	const FTaskRecoverySummary Summary =
		FUnrealClaudeTaskRecoverySummarizer::BuildRecoverySummary(Plan, Now, TEXT("evidence/plan.json"));

	TestTrue(TEXT("Progress contains 'blueprint_modify 3 times'"),
		Summary.ProgressSummary.Contains(TEXT("blueprint_modify 3 times")));
	TestTrue(TEXT("Progress contains 'capture_viewport once'"),
		Summary.ProgressSummary.Contains(TEXT("capture_viewport once")));
	return true;
}

// ================================================================
// Test 6: Summarizer detects repeated error as StallIndicator
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecoverySummarizer_DetectsErrorRepetition,
	"UnrealClaude.TaskRecovery.Summarizer.DetectsErrorRepetition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecoverySummarizer_DetectsErrorRepetition::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now = FDateTime::UtcNow();
	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		Now.ToIso8601(), TEXT("active"), TEXT("incomplete"));

	// Last 3 calls share identical result_status containing "error".
	Plan.ToolCalls.Add(MakeToolCall(TEXT("blueprint_modify"), TEXT("set var"), TEXT("success")));
	Plan.ToolCalls.Add(MakeToolCall(TEXT("blueprint_modify"), TEXT("add fn"), TEXT("error: function name required")));
	Plan.ToolCalls.Add(MakeToolCall(TEXT("blueprint_modify"), TEXT("add fn"), TEXT("error: function name required")));
	Plan.ToolCalls.Add(MakeToolCall(TEXT("blueprint_modify"), TEXT("add fn"), TEXT("error: function name required")));

	const FTaskRecoverySummary Summary =
		FUnrealClaudeTaskRecoverySummarizer::BuildRecoverySummary(Plan, Now, TEXT("evidence/plan.json"));

	TestFalse(TEXT("StallIndicator is NOT empty"), Summary.StallIndicator.IsEmpty());
	TestTrue(TEXT("StallIndicator mentions the error text"),
		Summary.StallIndicator.Contains(TEXT("function name required")));
	return true;
}

// ================================================================
// Test 7: Summarizer handles empty tool_calls without crash
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecoverySummarizer_HandlesEmptyToolCalls,
	"UnrealClaude.TaskRecovery.Summarizer.HandlesEmptyToolCalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecoverySummarizer_HandlesEmptyToolCalls::RunTest(const FString& /*Parameters*/)
{
	const FDateTime Now = FDateTime::UtcNow();
	FUnrealClaudeActivePlan Plan = BuildSynthPlan(
		Now.ToIso8601(), TEXT("active"), TEXT("incomplete"));
	// Empty tool_calls.

	const FTaskRecoverySummary Summary =
		FUnrealClaudeTaskRecoverySummarizer::BuildRecoverySummary(Plan, Now, TEXT("evidence/plan.json"));

	TestFalse(TEXT("ShortTitle populated from original task"), Summary.ShortTitle.IsEmpty());
	TestTrue(TEXT("Progress summary has 'No tool calls recorded'"),
		Summary.ProgressSummary.Contains(TEXT("No tool calls recorded")));
	TestTrue(TEXT("StallIndicator empty on zero tool calls"), Summary.StallIndicator.IsEmpty());
	return true;
}

// ================================================================
// Test 8: Summarizer ElapsedFormatting edge cases
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecoverySummarizer_ElapsedFormatting,
	"UnrealClaude.TaskRecovery.Summarizer.ElapsedFormatting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecoverySummarizer_ElapsedFormatting::RunTest(const FString& /*Parameters*/)
{
	// <30s → "just now"
	TestEqual(TEXT("5s → just now"),
		FUnrealClaudeTaskRecoverySummarizer::FormatElapsed(5),
		FString(TEXT("just now")));

	// <60min → "N minutes ago"
	TestEqual(TEXT("300s → 5 minutes ago"),
		FUnrealClaudeTaskRecoverySummarizer::FormatElapsed(300),
		FString(TEXT("5 minutes ago")));

	// <24h → "N hours M minutes ago"
	TestEqual(TEXT("3780s (1h 3m) → 1 hour 3 minutes ago"),
		FUnrealClaudeTaskRecoverySummarizer::FormatElapsed(3780),
		FString(TEXT("1 hour 3 minutes ago")));

	// <7 days → "N days M hours ago"
	TestEqual(TEXT("191520s (2d 5h 12m) → 2 days 5 hours ago"),
		FUnrealClaudeTaskRecoverySummarizer::FormatElapsed(2LL * 86400 + 5 * 3600 + 12 * 60),
		FString(TEXT("2 days 5 hours ago")));

	// >=7 days → "N weeks ago"
	TestEqual(TEXT("1210000s → 2 weeks ago"),
		FUnrealClaudeTaskRecoverySummarizer::FormatElapsed(14 * 86400),
		FString(TEXT("2 weeks ago")));

	// Minute singular
	TestEqual(TEXT("61s → 1 minute ago"),
		FUnrealClaudeTaskRecoverySummarizer::FormatElapsed(61),
		FString(TEXT("1 minute ago")));

	return true;
}

// ================================================================
// Test 9: Orchestrator PendingTaskRecoveryContext single-shot injection
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_InjectionFiresOnceThenClears,
	"UnrealClaude.TaskRecovery.Widget.InjectionFiresOnceThenClears",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_InjectionFiresOnceThenClears::RunTest(const FString& /*Parameters*/)
{
	// Use a locally-constructed orchestrator so we don't mutate global state.
	FAgentOrchestrator Local;

	// Slot starts empty.
	TestEqual(TEXT("Fresh consume returns empty"),
		Local.ConsumePendingTaskRecoveryContext(), FString());

	// Set → consume once returns the text → consume again returns empty.
	const FString Expected = TEXT("[TASK RECOVERY] Resume interrupted task foo.");
	Local.SetPendingTaskRecoveryContext(Expected);

	const FString First = Local.ConsumePendingTaskRecoveryContext();
	TestEqual(TEXT("First consume returns full text"), First, Expected);

	const FString Second = Local.ConsumePendingTaskRecoveryContext();
	TestEqual(TEXT("Second consume returns empty (single-shot)"), Second, FString());

	// Last-write-wins on double-set.
	Local.SetPendingTaskRecoveryContext(TEXT("old"));
	Local.SetPendingTaskRecoveryContext(TEXT("new"));
	TestEqual(TEXT("Last-write-wins on double-set"),
		Local.ConsumePendingTaskRecoveryContext(), FString(TEXT("new")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_AutoDispatchDecisionReady,
	"UnrealClaude.TaskRecovery.Widget.AutoDispatchDecisionReady",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_AutoDispatchDecisionReady::RunTest(const FString& /*Parameters*/)
{
	const FTaskRecoveryAutoDispatchDecision Decision =
		SClaudeEditorWidget::EvaluateTaskRecoveryAutoDispatch(
			false,
			true,
			FString(),
			false,
			FString());

	TestTrue(TEXT("ready state should allow auto-dispatch"), Decision.bCanAutoDispatch);
	TestTrue(TEXT("ready state should not report a blocker code"), Decision.BlockReasonCode.IsEmpty());
	TestTrue(TEXT("ready state should not surface a blocker message"), Decision.UserFacingMessage.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_AutoDispatchDecisionBlocksBusy,
	"UnrealClaude.TaskRecovery.Widget.AutoDispatchDecisionBlocksBusy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_AutoDispatchDecisionBlocksBusy::RunTest(const FString& /*Parameters*/)
{
	const FTaskRecoveryAutoDispatchDecision Decision =
		SClaudeEditorWidget::EvaluateTaskRecoveryAutoDispatch(
			true,
			true,
			FString(),
			false,
			FString());

	TestFalse(TEXT("busy state should block auto-dispatch"), Decision.bCanAutoDispatch);
	TestEqual(TEXT("busy state blocker code"), Decision.BlockReasonCode, FString(TEXT("request_already_running")));
	TestTrue(
		TEXT("busy-state message should mention another request running"),
		Decision.UserFacingMessage.Contains(TEXT("another request is already running"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_AutoDispatchDecisionBlocksBackendUnavailable,
	"UnrealClaude.TaskRecovery.Widget.AutoDispatchDecisionBlocksBackendUnavailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_AutoDispatchDecisionBlocksBackendUnavailable::RunTest(const FString& /*Parameters*/)
{
	const FTaskRecoveryAutoDispatchDecision Decision =
		SClaudeEditorWidget::EvaluateTaskRecoveryAutoDispatch(
			false,
			false,
			TEXT("Codex session is not authenticated."),
			false,
			FString());

	TestFalse(TEXT("backend-unavailable state should block auto-dispatch"), Decision.bCanAutoDispatch);
	TestEqual(TEXT("backend-unavailable blocker code"), Decision.BlockReasonCode, FString(TEXT("backend_unavailable")));
	TestTrue(
		TEXT("backend-unavailable message should surface the concrete detail"),
		Decision.UserFacingMessage.Contains(TEXT("not authenticated"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_AutoDispatchDecisionBlocksUnsafeTransportRetry,
	"UnrealClaude.TaskRecovery.Widget.AutoDispatchDecisionBlocksUnsafeTransportRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_AutoDispatchDecisionBlocksUnsafeTransportRetry::RunTest(const FString& /*Parameters*/)
{
	const FTaskRecoveryAutoDispatchDecision Decision =
		SClaudeEditorWidget::EvaluateTaskRecoveryAutoDispatch(
			false,
			true,
			FString(),
			true,
			TEXT("tool_activity_observed"));

	TestFalse(TEXT("unsafe transport retry should block auto-dispatch"), Decision.bCanAutoDispatch);
	TestEqual(TEXT("unsafe transport blocker code"), Decision.BlockReasonCode, FString(TEXT("transport_retry_blocked")));
	TestTrue(
		TEXT("unsafe transport message should mention retry-safe truth"),
		Decision.UserFacingMessage.Contains(TEXT("not retry-safe"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("unsafe transport message should mention tool activity"),
		Decision.UserFacingMessage.Contains(TEXT("tool activity"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaskRecovery_AutoResumePromptTruth,
	"UnrealClaude.TaskRecovery.Widget.AutoResumePromptTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTaskRecovery_AutoResumePromptTruth::RunTest(const FString& /*Parameters*/)
{
	const FString Prompt = SClaudeEditorWidget::BuildTaskRecoveryAutoResumePrompt();
	const FString Marker = SClaudeEditorWidget::BuildTaskRecoveryAutoResumeVisibleMarker();

	TestTrue(
		TEXT("auto-resume prompt should be explicitly system-generated"),
		Prompt.Contains(TEXT("SYSTEM-GENERATED TASK RECOVERY AUTO-RESUME"), ESearchCase::CaseSensitive));
	TestTrue(
		TEXT("auto-resume prompt should mention approved task recovery context"),
		Prompt.Contains(TEXT("[TASK RECOVERY]"), ESearchCase::CaseSensitive));
	TestTrue(
		TEXT("auto-resume prompt should forbid restart-from-scratch"),
		Prompt.Contains(TEXT("do not restart"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("visible marker should be clearly system-generated"),
		Marker.Contains(TEXT("System-generated"), ESearchCase::IgnoreCase));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
