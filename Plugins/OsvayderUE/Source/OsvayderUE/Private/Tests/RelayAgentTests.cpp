// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "CodexCliRunner.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "OsvayderUERelayAgent.h"
#include "OsvayderUERestartSurvival.h"
#include "OsvayderUESettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString NormalizePathForRelayTest(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return FString();
		}

		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	FString MakeFreshRelayAgentTestRoot(const FString& TestName)
	{
		FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("OsvayderUE"),
			TEXT("Automation"),
			TEXT("RelayAgent"),
			TestName);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	struct FScopedRelayRootOverride
	{
		explicit FScopedRelayRootOverride(const FString& InDir)
		{
			FOsvayderUERestartSurvivalManager::SetTestStateRootOverride(InDir);
		}

		~FScopedRelayRootOverride()
		{
			FOsvayderUERestartSurvivalManager::ClearTestStateRootOverride();
		}
	};

	int32 CountRecursiveFiles(const FString& Directory)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.*"), true, false, false);
		return Files.Num();
	}

	bool LoadNormalizedTextFile(const FString& Path, FString& OutContents)
	{
		if (!FFileHelper::LoadFileToString(OutContents, *Path))
		{
			return false;
		}

		OutContents.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_HandoffRoundTripPreservesPromptAndBudgets,
	"OsvayderUE.RelayAgent.HandoffRoundTripPreservesPromptAndBudgets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_ProgressJsonlAppendsAndLoadsLatest,
	"OsvayderUE.RelayAgent.ProgressJsonlAppendsAndLoadsLatest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_ProgressTaskFilterIgnoresStaleOtherTaskEntries,
	"OsvayderUE.RelayAgent.ProgressTaskFilterIgnoresStaleOtherTaskEntries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_ResultTaskFilterRejectsMismatchedTask,
	"OsvayderUE.RelayAgent.ResultTaskFilterRejectsMismatchedTask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_CancelRequestRoundTrip,
	"OsvayderUE.RelayAgent.CancelRequestRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_SettingsSnapshotParityMatchesCurrentCodexResolution,
	"OsvayderUE.RelayAgent.SettingsSnapshotParityMatchesCurrentCodexResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_ActivePlanRoundTripPreservesOriginalTaskAndLocalizedState,
	"OsvayderUE.RelayAgent.ActivePlanRoundTripPreservesOriginalTaskAndLocalizedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_SaveActivePlanIsAtomicAndLeavesNoTempFile,
	"OsvayderUE.RelayAgent.SaveActivePlanIsAtomicAndLeavesNoTempFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_ArchiveTerminalArtifactsArchivesAndPrunesBoundedRetention,
	"OsvayderUE.RelayAgent.ArchiveTerminalArtifactsArchivesAndPrunesBoundedRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_ScriptCopiesStayInSyncAndAvoidDetachedActivePlanWrites,
	"OsvayderUE.RelayAgent.ScriptCopiesStayInSyncAndAvoidDetachedActivePlanWrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRelayAgent_ScriptWritesDirectRelayArtifactSchema,
	"OsvayderUE.RelayAgent.ScriptWritesDirectRelayArtifactSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRelayAgent_HandoffRoundTripPreservesPromptAndBudgets::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRelayAgentTestRoot(TEXT("HandoffRoundTripPreservesPromptAndBudgets"));
	FScopedRelayRootOverride Override(TestRoot);

	FOsvayderUERelayHandoffContext Context;
	Context.TaskId = TEXT("task_relay_1");
	Context.RelaySessionId = TEXT("relay_session_1");
	Context.ProjectRoot = TEXT("D:/RelayProject");
	Context.UProjectPath = TEXT("D:/RelayProject/RelayProject.uproject");
	Context.ReattachToken = TEXT("reattach_token_1");
	Context.ReattachNotice = TEXT("resume notice");
	Context.CreatedAtUtc = TEXT("2026-04-16T00:00:00Z");
	Context.OriginalUserPrompt = TEXT("Original prompt must survive verbatim.");
	Context.bVisualProofRequired = true;
	Context.bVisualQaManifestRequired = true;
	Context.AttachedImagePaths = { TEXT("D:/RelayProject/References/combat_inventory.png") };
	Context.EditorAgentSummary = TEXT("Editor found a closed-editor blocker.");
	Context.LastKnownBlockerFamily = TEXT("uht_generated_rename_lock");
	Context.LastKnownBlockerSignature = TEXT("CombatCharacter.generated.h");
	Context.KnownFacts = { TEXT("fact one"), TEXT("fact two") };
	Context.RelevantArtifactPaths = { TEXT("D:/RelayProject/Saved/Logs/build.log") };
	Context.RelevantToolReceipts = { TEXT("tool_receipt_1") };
	Context.NextAttemptHypothesis = TEXT("Delete stale generated outputs and rebuild.");
	Context.BoundedObjective = TEXT("closed_editor_complex_build_repair_v1");
	Context.BoundedObjectiveDetail = TEXT("bounded to project source, generated artifacts, and build logs");
	Context.ReasoningIterationBudget = 10;
	Context.WallClockBudgetSeconds = 900;
	Context.Settings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();

	FString Error;
	TestTrue(TEXT("handoff context should save"), FOsvayderUERelayAgentManager::SaveHandoffContext(Context, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
	}

	FOsvayderUERelayHandoffContext Loaded;
	TestTrue(TEXT("handoff context should load"), FOsvayderUERelayAgentManager::LoadHandoffContext(Loaded, Error));
	TestEqual(TEXT("original prompt should survive verbatim"), Loaded.OriginalUserPrompt, Context.OriginalUserPrompt);
	TestEqual(TEXT("iteration budget should survive"), Loaded.ReasoningIterationBudget, 10);
	TestEqual(TEXT("wall clock budget should survive"), Loaded.WallClockBudgetSeconds, 900);
	TestTrue(TEXT("handoff visual proof requirement should survive"), Loaded.bVisualProofRequired);
	TestTrue(TEXT("handoff visual QA manifest requirement should survive"), Loaded.bVisualQaManifestRequired);
	TestEqual(TEXT("handoff attached image count should survive"), Loaded.AttachedImagePaths.Num(), 1);
	TestEqual(TEXT("artifact path count should survive"), Loaded.RelevantArtifactPaths.Num(), 1);
	TestEqual(TEXT("settings auth path should survive"), Loaded.Settings.AuthPath, Context.Settings.AuthPath);
	return true;
}

bool FRelayAgent_ProgressJsonlAppendsAndLoadsLatest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRelayAgentTestRoot(TEXT("ProgressJsonlAppendsAndLoadsLatest"));
	FScopedRelayRootOverride Override(TestRoot);

	FOsvayderUERelayProgressEntry First;
	First.TaskId = TEXT("task_old");
	First.RelaySessionId = TEXT("relay_session_old");
	First.TimestampUtc = TEXT("2026-04-16T00:00:00Z");
	First.EntryKind = TEXT("summary");
	First.Summary = TEXT("relay started");
	First.CurrentAction = TEXT("inspect build log");
	First.IterationIndex = 1;
	First.ElapsedSeconds = 3.0;
	First.HeartbeatAgeSeconds = 0.2;

	FOsvayderUERelayProgressEntry Second;
	Second.TaskId = TEXT("task_current");
	Second.RelaySessionId = TEXT("relay_session_current");
	Second.TimestampUtc = TEXT("2026-04-16T00:00:05Z");
	Second.EntryKind = TEXT("tool_result");
	Second.Summary = TEXT("build rerun failed");
	Second.CurrentAction = TEXT("parse UHT rename failure");
	Second.CurrentToolName = TEXT("command_execution");
	Second.IterationIndex = 2;
	Second.ElapsedSeconds = 8.0;
	Second.HeartbeatAgeSeconds = 0.1;
	Second.bIsStale = true;
	Second.TerminalOutcome = EOsvayderUERelayTerminalOutcome::TerminalFail;

	FString Error;
	TestTrue(TEXT("first progress entry should append"), FOsvayderUERelayAgentManager::AppendProgressEntry(First, Error));
	TestTrue(TEXT("second progress entry should append"), FOsvayderUERelayAgentManager::AppendProgressEntry(Second, Error));

	FOsvayderUERelayProgressEntry Latest;
	TestTrue(TEXT("latest progress entry should load"), FOsvayderUERelayAgentManager::LoadLatestProgressEntry(Latest, Error));
	TestEqual(TEXT("latest task id should survive"), Latest.TaskId, Second.TaskId);
	TestEqual(TEXT("latest relay session id should survive"), Latest.RelaySessionId, Second.RelaySessionId);
	TestEqual(TEXT("latest progress should be second summary"), Latest.Summary, Second.Summary);
	TestEqual(TEXT("latest iteration should be second iteration"), Latest.IterationIndex, 2);
	TestEqual(TEXT("latest tool name should survive"), Latest.CurrentToolName, Second.CurrentToolName);
	TestTrue(TEXT("stale flag should survive"), Latest.bIsStale);
	TestEqual(TEXT("terminal outcome should survive"), static_cast<uint8>(Latest.TerminalOutcome), static_cast<uint8>(EOsvayderUERelayTerminalOutcome::TerminalFail));
	return true;
}

bool FRelayAgent_ProgressTaskFilterIgnoresStaleOtherTaskEntries::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRelayAgentTestRoot(TEXT("ProgressTaskFilterIgnoresStaleOtherTaskEntries"));
	FScopedRelayRootOverride Override(TestRoot);

	FString Error;

	FOsvayderUERelayProgressEntry StaleEntry;
	StaleEntry.TaskId = TEXT("task_old");
	StaleEntry.RelaySessionId = TEXT("relay_session_old");
	StaleEntry.TimestampUtc = TEXT("2026-04-16T00:00:00Z");
	StaleEntry.Summary = TEXT("stale relay timeout");
	StaleEntry.CurrentAction = TEXT("terminal_outcome");
	StaleEntry.IterationIndex = 9;
	StaleEntry.TerminalOutcome = EOsvayderUERelayTerminalOutcome::TerminalFail;
	TestTrue(TEXT("stale progress entry should append"), FOsvayderUERelayAgentManager::AppendProgressEntry(StaleEntry, Error));

	FOsvayderUERelayProgressEntry CurrentEntry;
	CurrentEntry.TaskId = TEXT("task_current");
	CurrentEntry.RelaySessionId = TEXT("relay_session_current");
	CurrentEntry.TimestampUtc = TEXT("2026-04-16T00:00:05Z");
	CurrentEntry.Summary = TEXT("current relay build progressing");
	CurrentEntry.CurrentAction = TEXT("rerun_build");
	CurrentEntry.IterationIndex = 2;
	TestTrue(TEXT("current progress entry should append"), FOsvayderUERelayAgentManager::AppendProgressEntry(CurrentEntry, Error));

	FOsvayderUERelayProgressEntry FilteredEntry;
	TestTrue(
		TEXT("task-filtered progress load should find the current task entry"),
		FOsvayderUERelayAgentManager::LoadLatestProgressEntryForTask(TEXT("task_current"), FilteredEntry, Error));
	TestEqual(TEXT("filtered progress should return the matching task id"), FilteredEntry.TaskId, CurrentEntry.TaskId);
	TestEqual(TEXT("filtered progress should return the matching summary"), FilteredEntry.Summary, CurrentEntry.Summary);

	TestFalse(
		TEXT("task-filtered progress load should reject unknown task ids"),
		FOsvayderUERelayAgentManager::LoadLatestProgressEntryForTask(TEXT("task_missing"), FilteredEntry, Error));
	return true;
}

bool FRelayAgent_ResultTaskFilterRejectsMismatchedTask::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRelayAgentTestRoot(TEXT("ResultTaskFilterRejectsMismatchedTask"));
	FScopedRelayRootOverride Override(TestRoot);

	FOsvayderUERelayResult Result;
	Result.TaskId = TEXT("task_other");
	Result.RelaySessionId = TEXT("relay_session_other");
	Result.CompletedAtUtc = TEXT("2026-04-16T00:00:00Z");
	Result.TerminalOutcome = EOsvayderUERelayTerminalOutcome::TerminalFail;
	Result.Summary = TEXT("stale relay fail");

	FString Error;
	TestTrue(TEXT("mismatched relay result should save"), FOsvayderUERelayAgentManager::SaveRelayResult(Result, Error));

	FOsvayderUERelayResult LoadedResult;
	TestFalse(
		TEXT("task-filtered relay result load should reject a mismatched task"),
		FOsvayderUERelayAgentManager::LoadRelayResultForTask(TEXT("task_current"), LoadedResult, Error));

	Result.TaskId = TEXT("task_current");
	Result.RelaySessionId = TEXT("relay_session_current");
	Result.Summary = TEXT("current relay success");
	Result.TerminalOutcome = EOsvayderUERelayTerminalOutcome::Success;
	TestTrue(TEXT("matching relay result should overwrite"), FOsvayderUERelayAgentManager::SaveRelayResult(Result, Error));
	TestTrue(
		TEXT("task-filtered relay result load should accept the matching task"),
		FOsvayderUERelayAgentManager::LoadRelayResultForTask(TEXT("task_current"), LoadedResult, Error));
	TestEqual(TEXT("filtered relay result should preserve the current summary"), LoadedResult.Summary, Result.Summary);
	return true;
}

bool FRelayAgent_CancelRequestRoundTrip::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRelayAgentTestRoot(TEXT("CancelRequestRoundTrip"));
	FScopedRelayRootOverride Override(TestRoot);

	TestFalse(TEXT("cancel request should not exist initially"), FOsvayderUERelayAgentManager::HasCancelRequest());

	FString Error;
	TestTrue(TEXT("cancel request should write"), FOsvayderUERelayAgentManager::WriteCancelRequest(TEXT("user_cancelled"), Error));
	TestTrue(TEXT("cancel request should exist after write"), FOsvayderUERelayAgentManager::HasCancelRequest());
	TestTrue(TEXT("cancel request should delete"), FOsvayderUERelayAgentManager::DeleteCancelRequest(Error));
	TestFalse(TEXT("cancel request should be gone after delete"), FOsvayderUERelayAgentManager::HasCancelRequest());
	return true;
}

bool FRelayAgent_SettingsSnapshotParityMatchesCurrentCodexResolution::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRelayAgentTestRoot(TEXT("SettingsSnapshotParityMatchesCurrentCodexResolution"));
	FScopedRelayRootOverride Override(TestRoot);

	const FOsvayderUERelaySettingsSnapshot Snapshot = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	TestNotNull(TEXT("settings should be available"), Settings);
	if (!Settings)
	{
		return false;
	}

	TestEqual(TEXT("model should match configured settings"), Snapshot.Model, Settings->GetConfiguredCodexModel());
	TestEqual(TEXT("profile should match effective profile label"), Snapshot.Profile, Settings->GetEffectiveCodexProfileLabel());
	TestEqual(TEXT("requested speed should match configured speed"), Snapshot.RequestedSpeedMode, Settings->GetConfiguredCodexSpeedModeName());
	TestEqual(TEXT("work mode should match configured work mode"), Snapshot.WorkMode, Settings->GetConfiguredCodexWorkModeName());
	TestEqual(TEXT("reasoning effort should match configured reasoning"), Snapshot.ReasoningEffort, Settings->GetConfiguredCodexReasoningEffortName());
	TestEqual(TEXT("verbosity should match configured verbosity"), Snapshot.Verbosity, Settings->GetConfiguredCodexVerbosityName());
	TestEqual(TEXT("auth mode should match configured auth mode"), Snapshot.AuthMode, Settings->GetConfiguredCodexAuthModeName());
	TestEqual(TEXT("auth path should match Codex runner resolution"), Snapshot.AuthPath, FCodexCliRunner::GetEffectiveAuthEntryPath());
	TestEqual(TEXT("auth ownership should match Codex runner resolution"), Snapshot.AuthOwnership, FCodexCliRunner::GetEffectiveAuthOwnershipModel());
	TestEqual(TEXT("codex home path should match Codex runner resolution"), NormalizePathForRelayTest(Snapshot.CodexHomePath), NormalizePathForRelayTest(FCodexCliRunner::GetConfiguredCodexHomePath()));
	TestEqual(TEXT("codex home source should match Codex runner resolution"), Snapshot.CodexHomeResolutionSource, FCodexCliRunner::GetConfiguredCodexHomeResolutionSource());
	TestEqual(TEXT("execution transport should match persistent transport resolution"),
		Snapshot.ExecutionTransport,
		FCodexCliRunner::ShouldUsePersistentConversationTransport() ? FString(TEXT("persistent_app_server")) : FString(TEXT("process_exec")));
	return true;
}

bool FRelayAgent_ActivePlanRoundTripPreservesOriginalTaskAndLocalizedState::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRelayAgentTestRoot(TEXT("ActivePlanRoundTripPreservesOriginalTaskAndLocalizedState"));
	FScopedRelayRootOverride Override(TestRoot);

	FOsvayderUEActivePlan Plan;
	Plan.PlanId = TEXT("plan_606");
	Plan.ReviewerPlanReference = TEXT("606_PostF1_PlanLifecycleAndAgent2UxSpec");
	Plan.OriginalUserTask = TEXT("Найди failed build attempt и доведи задачу до truthful build result.");
	Plan.CreatedAtUtc = TEXT("2026-04-16T00:00:00Z");
	Plan.UpdatedAtUtc = TEXT("2026-04-16T00:00:05Z");
	Plan.Status = TEXT("awaiting_resume");
	Plan.ResultStatus = TEXT("closed_editor_work_completed");
	Plan.Summary = TEXT("Closed-editor relay completed the bounded build lane.");
	Plan.SummaryRu = TEXT("Agent 2 завершил закрытый этап и вернул задачу в редактор.");
	Plan.TechnicalDetail = TEXT("Build.bat Poligon1Editor Win64 Development -> Result: Succeeded");
	Plan.CurrentMechanicId = TEXT("verify_and_report");
	Plan.CurrentToolCallId = TEXT("tool_call_verify_1");
	Plan.CurrentAction = TEXT("await_post_reattach_verification");
	Plan.CurrentActionRu = TEXT("Ожидание post-reattach проверки.");
	Plan.CurrentTechnicalDetail = TEXT("Relay final result is ready for truthful reopened-editor verification.");
	Plan.ResumeHint = TEXT("Resume from last_completed_tool_call_id=tool_call_build_2 after reattach.");
	Plan.LaneState.CurrentLane = TEXT("live_editor");
	Plan.LaneState.TransitionKind = TEXT("lane_return");
	Plan.LaneState.TransitionState = TEXT("in_progress");
	Plan.LaneState.TransitionReason = TEXT("Reopening the editor and reattaching to the persisted task.");
	Plan.LaneState.ContinuityTaskId = TEXT("task_606");
	Plan.LaneState.ContinuityPlanId = Plan.PlanId;
	Plan.LaneState.ContinuityWorkflowId = TEXT("feature_roundtrip_1");
	Plan.LaneState.ContinuityPhaseId = TEXT("runtime_proof");
	Plan.LaneState.ContinuationIntent = Plan.ResumeHint;
	Plan.LaneState.ExpectedReturnCondition = TEXT("Resume the same task from the saved editor boundary after reattach.");
	Plan.LastCompletedToolCallId = TEXT("tool_call_build_2");
	Plan.bHybridSplitTriggered = true;
	Plan.bPostReattachVerificationRequired = true;
	Plan.bVisualProofRequired = true;
	Plan.bVisualQaManifestRequired = true;
	Plan.VisualProofRequirement = TEXT("Final acceptance requires visual_qa_manifest.json proof.");
	Plan.VisualProofStatus = TEXT("pending_visual_qa_manifest");
	Plan.VisualQaManifestPath = TEXT("D:/Project/Saved/Logs/visual_qa_manifest.json");
	Plan.VisualQaManifestVerdict = TEXT("pending");
	Plan.VisualReferenceArtifactPaths = { TEXT("D:/References/combat_inventory.png") };
	Plan.Settings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
	Plan.CompileProof.bCompiledModuleMutationObserved = true;
	Plan.CompileProof.MutationToolFamily = TEXT("workspace_file_build");
	Plan.CompileProof.LastMutationAtUtc = TEXT("2026-04-16T00:00:03Z");
	Plan.CompileProof.LastMutationToolCallId = TEXT("tool_call_edit_1");
	Plan.CompileProof.LastMutationToolName = TEXT("write");
	Plan.CompileProof.LastCompileProofAtUtc = TEXT("2026-04-16T00:00:04Z");
	Plan.CompileProof.LastCompileProofToolCallId = TEXT("tool_call_compile_1");
	Plan.CompileProof.LastCompileProofToolName = TEXT("livecoding_compile");
	Plan.CompileProof.LastCompileProofOutcome = TEXT("success");
	Plan.CompileProof.LastCompileProofDetail = TEXT("Live Coding compile succeeded.");
	Plan.CompileProof.LastPostCompileVerificationAtUtc = TEXT("2026-04-16T00:00:05Z");
	Plan.CompileProof.LastPostCompileVerificationToolCallId = TEXT("tool_call_verify_1");
	Plan.CompileProof.LastPostCompileVerificationToolName = TEXT("execute_script");
	Plan.CompileProof.LastPostCompileVerificationOutcome = TEXT("pass");
	Plan.CompileProof.LastCloseoutGateReason = TEXT("compile_succeeded_without_post_compile_verification");
	Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_roundtrip_1");
	Plan.FeatureWorkflow.RecipeId = TEXT("feature.inventory_basic_ui_v1");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.bCompileProofRequired = true;
	Plan.FeatureWorkflow.CompileProofState = TEXT("passed");
	Plan.FeatureWorkflow.bRuntimeProofRequired = true;
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");
	Plan.FeatureWorkflow.LaneState = Plan.LaneState;
	Plan.FeatureWorkflow.CompletedPhaseIds = {
		TEXT("data_model"),
		TEXT("runtime_owner"),
		TEXT("input_controller"),
		TEXT("ui_widget"),
		TEXT("compile_gate")
	};
	Plan.FeatureWorkflow.Phases = {
		{ TEXT("data_model"), TEXT("Data model"), TEXT("completed"), 1, 0, TEXT("2026-04-16T00:00:00Z"), TEXT("2026-04-16T00:00:01Z"), TEXT("tool_data"), TEXT("write"), FString() },
		{ TEXT("runtime_owner"), TEXT("Runtime owner"), TEXT("completed"), 1, 0, TEXT("2026-04-16T00:00:01Z"), TEXT("2026-04-16T00:00:02Z"), TEXT("tool_owner"), TEXT("write"), FString() },
		{ TEXT("input_controller"), TEXT("Input controller"), TEXT("completed"), 1, 0, TEXT("2026-04-16T00:00:02Z"), TEXT("2026-04-16T00:00:03Z"), TEXT("tool_input"), TEXT("enhanced_input"), FString() },
		{ TEXT("ui_widget"), TEXT("UI widget"), TEXT("completed"), 1, 0, TEXT("2026-04-16T00:00:03Z"), TEXT("2026-04-16T00:00:04Z"), TEXT("tool_ui"), TEXT("blueprint_modify"), FString() },
		{ TEXT("compile_gate"), TEXT("Compile gate"), TEXT("completed"), 1, 0, TEXT("2026-04-16T00:00:04Z"), TEXT("2026-04-16T00:00:05Z"), TEXT("tool_compile"), TEXT("livecoding_compile"), FString() },
		{ TEXT("runtime_proof"), TEXT("Runtime proof"), TEXT("in_progress"), 1, 0, TEXT("2026-04-16T00:00:05Z"), FString(), TEXT("tool_verify"), TEXT("execute_script"), FString() },
		{ TEXT("memory_update"), TEXT("Memory update"), TEXT("pending"), 0, 0, FString(), FString(), FString(), FString(), FString() }
	};
	Plan.CompletedMechanicIds = { TEXT("inspect_current_state"), TEXT("perform_bounded_work") };
	Plan.VerificationChecklist = { TEXT("Open Osvayder UE"), TEXT("Review build result"), TEXT("Reply with truthful final status") };

	FOsvayderUEPlanMechanicEntry Inspect;
	Inspect.MechanicId = TEXT("inspect_current_state");
	Inspect.Label = TEXT("Inspect current state");
	Inspect.LabelRu = TEXT("Проверить текущее состояние");
	Inspect.Status = TEXT("completed");
	Inspect.LastSummaryRu = TEXT("Исторические build-логи просмотрены.");
	Plan.Mechanics.Add(Inspect);

	FOsvayderUEPlanMechanicEntry Verify;
	Verify.MechanicId = TEXT("verify_and_report");
	Verify.Label = TEXT("Verify and report");
	Verify.LabelRu = TEXT("Проверить и отчитаться");
	Verify.Status = TEXT("pending");
	Verify.bRequiresPostReattachVerification = true;
	Plan.Mechanics.Add(Verify);

	FOsvayderUEPlanToolCallEntry ToolCall;
	ToolCall.ToolCallId = TEXT("tool_call_build_2");
	ToolCall.MechanicId = TEXT("perform_bounded_work");
	ToolCall.ToolName = TEXT("command_execution");
	ToolCall.Status = TEXT("completed");
	ToolCall.SummaryRu = TEXT("Сборка завершена успешно.");
	ToolCall.TechnicalDetail = TEXT("Build.bat Poligon1Editor Win64 Development");
	ToolCall.ResultStatus = TEXT("completed");
	Plan.ToolCalls.Add(ToolCall);

	FString Error;
	TestTrue(TEXT("active plan should save"), FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
	}

	FOsvayderUEActivePlan Loaded;
	TestTrue(TEXT("active plan should load"), FOsvayderUERelayAgentManager::LoadActivePlan(Loaded, Error));
	TestEqual(TEXT("original user task should survive verbatim"), Loaded.OriginalUserTask, Plan.OriginalUserTask);
	TestEqual(TEXT("localized summary should survive"), Loaded.SummaryRu, Plan.SummaryRu);
	TestEqual(TEXT("current mechanic should survive"), Loaded.CurrentMechanicId, Plan.CurrentMechanicId);
	TestEqual(TEXT("tool-call count should survive"), Loaded.ToolCalls.Num(), 1);
	TestTrue(TEXT("compile-proof mutation flag should survive"), Loaded.CompileProof.bCompiledModuleMutationObserved);
	TestEqual(TEXT("compile-proof mutation timestamp should survive"), Loaded.CompileProof.LastMutationAtUtc, Plan.CompileProof.LastMutationAtUtc);
	TestEqual(TEXT("compile-proof outcome should survive"), Loaded.CompileProof.LastCompileProofOutcome, Plan.CompileProof.LastCompileProofOutcome);
	TestEqual(TEXT("post-compile verification outcome should survive"), Loaded.CompileProof.LastPostCompileVerificationOutcome, Plan.CompileProof.LastPostCompileVerificationOutcome);
	TestEqual(TEXT("closeout gate reason should survive"), Loaded.CompileProof.LastCloseoutGateReason, Plan.CompileProof.LastCloseoutGateReason);
	TestEqual(TEXT("feature workflow id should survive"), Loaded.FeatureWorkflow.FeatureWorkflowId, Plan.FeatureWorkflow.FeatureWorkflowId);
	TestEqual(TEXT("feature workflow recipe should survive"), Loaded.FeatureWorkflow.RecipeId, Plan.FeatureWorkflow.RecipeId);
	TestEqual(TEXT("feature workflow phase should survive"), Loaded.FeatureWorkflow.CurrentPhase, Plan.FeatureWorkflow.CurrentPhase);
	TestEqual(TEXT("active plan current lane should survive"), Loaded.LaneState.CurrentLane, Plan.LaneState.CurrentLane);
	TestTrue(TEXT("visual proof required should survive"), Loaded.bVisualProofRequired);
	TestTrue(TEXT("visual QA manifest required should survive"), Loaded.bVisualQaManifestRequired);
	TestEqual(TEXT("visual QA manifest path should survive"), Loaded.VisualQaManifestPath, Plan.VisualQaManifestPath);
	TestEqual(TEXT("visual QA manifest verdict should survive"), Loaded.VisualQaManifestVerdict, Plan.VisualQaManifestVerdict);
	TestEqual(TEXT("visual reference paths should survive"), Loaded.VisualReferenceArtifactPaths.Num(), Plan.VisualReferenceArtifactPaths.Num());
	TestEqual(TEXT("active plan transition kind should survive"), Loaded.LaneState.TransitionKind, Plan.LaneState.TransitionKind);
	TestEqual(TEXT("active plan continuity plan id should survive"), Loaded.LaneState.ContinuityPlanId, Plan.LaneState.ContinuityPlanId);
	TestEqual(TEXT("active plan continuity workflow id should survive"), Loaded.LaneState.ContinuityWorkflowId, Plan.LaneState.ContinuityWorkflowId);
	TestEqual(TEXT("feature workflow lane state should survive"), Loaded.FeatureWorkflow.LaneState.TransitionState, Plan.FeatureWorkflow.LaneState.TransitionState);
	TestEqual(TEXT("feature workflow compile proof state should survive"), Loaded.FeatureWorkflow.CompileProofState, Plan.FeatureWorkflow.CompileProofState);
	TestEqual(TEXT("feature workflow phase count should survive"), Loaded.FeatureWorkflow.Phases.Num(), Plan.FeatureWorkflow.Phases.Num());
	TestEqual(TEXT("completed mechanic ids should survive"), Loaded.CompletedMechanicIds.Num(), 2);
	TestEqual(TEXT("verification checklist should survive"), Loaded.VerificationChecklist.Num(), 3);
	return true;
}

bool FRelayAgent_SaveActivePlanIsAtomicAndLeavesNoTempFile::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRelayAgentTestRoot(TEXT("SaveActivePlanIsAtomicAndLeavesNoTempFile"));
	FScopedRelayRootOverride Override(TestRoot);

	FOsvayderUEActivePlan Plan;
	Plan.PlanId = TEXT("plan_atomic");
	Plan.OriginalUserTask = TEXT("Atomic active plan save.");
	Plan.SummaryRu = TEXT("План сохранен атомарно.");

	FString Error;
	TestTrue(TEXT("active plan should save atomically"), FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
	}

	const FString ActivePlanPath = FOsvayderUERelayAgentManager::GetActivePlanPath();
	const FString TempPath = ActivePlanPath + TEXT(".tmp");
	TestTrue(TEXT("active plan should exist"), IFileManager::Get().FileExists(*ActivePlanPath));
	TestFalse(TEXT("active plan temp file should not remain"), IFileManager::Get().FileExists(*TempPath));
	return true;
}

bool FRelayAgent_ArchiveTerminalArtifactsArchivesAndPrunesBoundedRetention::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRelayAgentTestRoot(TEXT("ArchiveTerminalArtifactsArchivesAndPrunesBoundedRetention"));
	FScopedRelayRootOverride Override(TestRoot);

	FOsvayderUEActivePlan Plan;
	Plan.PlanId = TEXT("plan_archive");
	Plan.ArchiveRunTag = TEXT("20260416-archive");
	Plan.OriginalUserTask = TEXT("Archive terminal relay artifacts.");
	Plan.SummaryRu = TEXT("Архивирование завершено.");

	FString Error;
	TestTrue(TEXT("active plan should save before archive"), FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error));

	FOsvayderUERelayHandoffContext Handoff;
	Handoff.TaskId = TEXT("task_archive");
	Handoff.ProjectRoot = TEXT("D:/ArchiveProject");
	Handoff.OriginalUserPrompt = TEXT("Archive relay artifacts.");
	TestTrue(TEXT("handoff should save before archive"), FOsvayderUERelayAgentManager::SaveHandoffContext(Handoff, Error));

	FOsvayderUERelayProgressEntry Progress;
	Progress.TaskId = TEXT("task_archive");
	Progress.PlanId = Plan.PlanId;
	Progress.MechanicId = TEXT("perform_bounded_work");
	Progress.ToolCallId = TEXT("tool_call_archive");
	Progress.SummaryRu = TEXT("Последний relay progress.");
	TestTrue(TEXT("progress should append before archive"), FOsvayderUERelayAgentManager::AppendProgressEntry(Progress, Error));

	FOsvayderUERelayResult Result;
	Result.TaskId = TEXT("task_archive");
	Result.PlanId = Plan.PlanId;
	Result.TerminalOutcome = EOsvayderUERelayTerminalOutcome::Success;
	Result.SummaryRu = TEXT("Relay завершен.");
	TestTrue(TEXT("relay result should save before archive"), FOsvayderUERelayAgentManager::SaveRelayResult(Result, Error));

	TestTrue(TEXT("cancel request should save before archive"), FOsvayderUERelayAgentManager::WriteCancelRequest(TEXT("archive test"), Error));

	TArray<FString> ArchivedPaths;
	TestTrue(TEXT("terminal archive should succeed"), FOsvayderUERelayAgentManager::ArchiveTerminalArtifacts(Plan, ArchivedPaths, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
	}

	TestTrue(TEXT("archive should include a plan snapshot"), ArchivedPaths.ContainsByPredicate([](const FString& Path)
	{
		return Path.Contains(TEXT(".active_plan.json"));
	}));
	TestFalse(TEXT("active plan should be cleared after archive"), IFileManager::Get().FileExists(*FOsvayderUERelayAgentManager::GetActivePlanPath()));
	TestFalse(TEXT("progress log should be cleared after archive"), IFileManager::Get().FileExists(*FOsvayderUERelayAgentManager::GetRelayProgressPath()));
	TestFalse(TEXT("relay result should be cleared after archive"), IFileManager::Get().FileExists(*FOsvayderUERelayAgentManager::GetRelayResultPath()));
	TestFalse(TEXT("handoff should be cleared after archive"), IFileManager::Get().FileExists(*FOsvayderUERelayAgentManager::GetHandoffContextPath()));
	TestFalse(TEXT("cancel request should be cleared after archive"), IFileManager::Get().FileExists(*FOsvayderUERelayAgentManager::GetRelayCancelRequestPath()));

	const FString PlanArchiveDir = FOsvayderUERelayAgentManager::GetPlanArchiveDir();
	const FString RelayArchiveDir = FOsvayderUERelayAgentManager::GetRelayArchiveDir();
	TestTrue(TEXT("plan archive directory should contain files"), CountRecursiveFiles(PlanArchiveDir) >= 1);
	TestTrue(TEXT("relay archive directory should contain files"), CountRecursiveFiles(RelayArchiveDir) >= 1);

	for (int32 Index = 0; Index < 5; ++Index)
	{
		const FString PlanArtifact = FPaths::Combine(PlanArchiveDir, FString::Printf(TEXT("dummy_plan_%d.json"), Index));
		const FString RelayArtifact = FPaths::Combine(RelayArchiveDir, FString::Printf(TEXT("dummy_relay_%d.json"), Index));
		TestTrue(TEXT("dummy plan artifact should save"), FFileHelper::SaveStringToFile(TEXT("{}"), *PlanArtifact, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		TestTrue(TEXT("dummy relay artifact should save"), FFileHelper::SaveStringToFile(TEXT("{}"), *RelayArtifact, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	TestTrue(TEXT("archive pruning should succeed"), FOsvayderUERelayAgentManager::PruneArchivedArtifacts(2, 2, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
	}
	TestTrue(TEXT("plan archive count should stay bounded"), CountRecursiveFiles(PlanArchiveDir) <= 2);
	TestTrue(TEXT("relay archive count should stay bounded"), CountRecursiveFiles(RelayArchiveDir) <= 2);
	return true;
}

bool FRelayAgent_ScriptCopiesStayInSyncAndAvoidDetachedActivePlanWrites::RunTest(const FString& Parameters)
{
	const FString PluginScriptPath = FOsvayderUERelayAgentManager::GetRelayAgentScriptPath();
	const FString ProjectPluginScriptPath = NormalizePathForRelayTest(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("OsvayderUE"), TEXT("Script"), TEXT("OsvayderUE-RelayAgent.ps1")));
	const FString OptionalProjectMirrorScriptPath = NormalizePathForRelayTest(
		FPaths::Combine(FPaths::ProjectDir(), TEXT("Script"), TEXT("OsvayderUE-RelayAgent.ps1")));

	TestTrue(TEXT("plugin relay script should exist"), IFileManager::Get().FileExists(*PluginScriptPath));
	TestEqual(TEXT("managed relay script path should resolve to the project plugin copy"), NormalizePathForRelayTest(PluginScriptPath), ProjectPluginScriptPath);

	FString PluginScriptContents;
	TestTrue(TEXT("plugin relay script should load"), LoadNormalizedTextFile(PluginScriptPath, PluginScriptContents));
	if (IFileManager::Get().FileExists(*OptionalProjectMirrorScriptPath))
	{
		FString ProjectScriptContents;
		TestTrue(TEXT("optional project-root relay script should load when present"), LoadNormalizedTextFile(OptionalProjectMirrorScriptPath, ProjectScriptContents));
		TestEqual(TEXT("optional project-root relay script mirror should stay byte-equivalent to plugin copy"), ProjectScriptContents, PluginScriptContents);
	}
	else
	{
		AddInfo(TEXT("Project-root Script/OsvayderUE-RelayAgent.ps1 mirror is absent in this host project; project-plugin script remains the active source of truth."));
	}

	TestFalse(TEXT("relay script should not carry detached active_plan_path state"), PluginScriptContents.Contains(TEXT("active_plan_path")));
	TestFalse(TEXT("relay script should not contain detached Load-ActivePlan helper"), PluginScriptContents.Contains(TEXT("Load-ActivePlan")));
	TestFalse(TEXT("relay script should not contain detached Save-ActivePlan helper"), PluginScriptContents.Contains(TEXT("Save-ActivePlan")));
	TestFalse(TEXT("relay script should not sync relay runtime back into active plan"), PluginScriptContents.Contains(TEXT("Sync-RelayRuntimeIntoActivePlan")));
	TestFalse(TEXT("relay script should not describe active_plan JSON as a detached artifact"), PluginScriptContents.Contains(TEXT("active_plan.json")));
	return true;
}

bool FRelayAgent_ScriptWritesDirectRelayArtifactSchema::RunTest(const FString& Parameters)
{
	const FString PluginScriptPath = FOsvayderUERelayAgentManager::GetRelayAgentScriptPath();
	FString PluginScriptContents;
	TestTrue(TEXT("plugin relay script should load"), LoadNormalizedTextFile(PluginScriptPath, PluginScriptContents));

	TestTrue(TEXT("relay script should append direct progress entries"), PluginScriptContents.Contains(TEXT("Append-JsonLine -Path $Path -Payload @{")));
	TestTrue(TEXT("relay script should save the final relay result JSON directly"), PluginScriptContents.Contains(TEXT("Save-JsonFile -Path $resolvedResultPath -Payload @{")));
	TestTrue(TEXT("relay progress payload should carry summary_ru directly"), PluginScriptContents.Contains(TEXT("summary_ru = Get-RelayRuntimeSummaryRu -Runtime $Runtime")));
	TestTrue(TEXT("relay progress payload should carry current_action_ru directly"), PluginScriptContents.Contains(TEXT("current_action_ru = [string]$Runtime.current_action_ru")));
	TestTrue(TEXT("relay result payload should carry summary_ru directly"), PluginScriptContents.Contains(TEXT("summary_ru = $runtime.summary_ru")));
	TestTrue(TEXT("relay result payload should expose relay_progress_path directly"), PluginScriptContents.Contains(TEXT("relay_progress_path = $resolvedProgressPath")));
	return true;
}

#endif
