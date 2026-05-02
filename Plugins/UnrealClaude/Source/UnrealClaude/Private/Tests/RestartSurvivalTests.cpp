// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "AgentPromptContract.h"
#include "ClaudeEditorWidget.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_PluginSettings.h"
#include "MCP/Tools/MCPTool_RestartSurvival.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClaudeRelayAgent.h"
#include "UnrealClaudeRestartSurvival.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString NormalizePathForTest(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return FString();
		}

		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	FString MakeFreshRestartSurvivalTestRoot(const FString& TestName)
	{
		FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UnrealClaude"),
			TEXT("Automation"),
			TEXT("RestartSurvival"),
			TestName);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	bool SaveUtf8TextFile(const FString& Path, const FString& Contents)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(Contents, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool LoadBinaryFile(const FString& Path, TArray<uint8>& OutBytes)
	{
		return FFileHelper::LoadFileToArray(OutBytes, *Path);
	}

	bool ByteArraysEqual(const TArray<uint8>& Left, const TArray<uint8>& Right)
	{
		return Left.Num() == Right.Num()
			&& (Left.Num() == 0 || FMemory::Memcmp(Left.GetData(), Right.GetData(), Left.Num()) == 0);
	}

	bool WaitForProcessExit(FProcHandle& ProcHandle, const double TimeoutSeconds)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		while (ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcHandle))
		{
			if ((FPlatformTime::Seconds() - StartSeconds) >= TimeoutSeconds)
			{
				return false;
			}

			FPlatformProcess::Sleep(0.05f);
		}

		return true;
	}

	bool WaitForFileToExist(const FString& Path, const double TimeoutSeconds)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		while (!IFileManager::Get().FileExists(*Path))
		{
			if ((FPlatformTime::Seconds() - StartSeconds) >= TimeoutSeconds)
			{
				return false;
			}

			FPlatformProcess::Sleep(0.05f);
		}

		return true;
	}

	TSharedPtr<FJsonObject> GetObjectFieldOrNull(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* NestedObject = nullptr;
		if (!Object->TryGetObjectField(FieldName, NestedObject) || !NestedObject || !(*NestedObject).IsValid())
		{
			return nullptr;
		}

		return *NestedObject;
	}

	struct FScopedRestartSurvivalRootOverride
	{
		explicit FScopedRestartSurvivalRootOverride(const FString& InDir)
		{
			FUnrealClaudeRestartSurvivalManager::SetTestStateRootOverride(InDir);
		}

		~FScopedRestartSurvivalRootOverride()
		{
			FUnrealClaudeRestartSurvivalManager::ClearTestStateRootOverride();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ProjectLocalIsolationAcrossRoots,
	"UnrealClaude.RestartSurvival.ProjectLocalIsolationAcrossRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_ProjectLocalIsolationAcrossRoots::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ProjectLocalIsolationAcrossRoots"));
	const FString ProjectARoot = FPaths::Combine(TestRoot, TEXT("ProjectA"), TEXT("UnrealClaude"));
	const FString ProjectBRoot = FPaths::Combine(TestRoot, TEXT("ProjectB"), TEXT("UnrealClaude"));

	{
		FScopedRestartSurvivalRootOverride Override(ProjectARoot);
		FUnrealClaudeRestartSurvivalState State;
		State.SessionId = TEXT("session_a");
		State.TaskId = TEXT("task_a");
		State.ProjectRoot = TEXT("D:/ProjectA");
		State.UProjectPath = TEXT("D:/ProjectA/ProjectA.uproject");
		State.Backend = EUnrealClaudeProviderBackend::CodexCli;
		State.bProviderThreadResumePending = true;
		State.Phase = EUnrealClaudeRestartSurvivalPhase::AwaitingReattach;
		State.ProviderSessionId = TEXT("thread_a");
		FString Error;
		TestTrue(TEXT("project A state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));
		TestTrue(TEXT("project A state path should exist"), IFileManager::Get().FileExists(*FUnrealClaudeRestartSurvivalManager::GetStatePath()));
	}

	{
		FScopedRestartSurvivalRootOverride Override(ProjectBRoot);
		FUnrealClaudeRestartSurvivalState State;
		State.SessionId = TEXT("session_b");
		State.TaskId = TEXT("task_b");
		State.ProjectRoot = TEXT("D:/ProjectB");
		State.UProjectPath = TEXT("D:/ProjectB/ProjectB.uproject");
		State.Backend = EUnrealClaudeProviderBackend::CodexCli;
		State.bProviderThreadResumePending = false;
		State.Phase = EUnrealClaudeRestartSurvivalPhase::DetachedRunning;
		State.ProviderSessionId = TEXT("thread_b");
		FString Error;
		TestTrue(TEXT("project B state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));
		TestTrue(TEXT("project B state path should exist"), IFileManager::Get().FileExists(*FUnrealClaudeRestartSurvivalManager::GetStatePath()));
	}

	{
		FScopedRestartSurvivalRootOverride Override(ProjectARoot);
		FUnrealClaudeRestartSurvivalState State;
		TestTrue(TEXT("project A state should load"), FUnrealClaudeRestartSurvivalManager::DescribeCurrentState(State));
		TestEqual(TEXT("project A should keep its own task"), State.TaskId, FString(TEXT("task_a")));
		TestEqual(TEXT("project A should keep its own provider session"), State.ProviderSessionId, FString(TEXT("thread_a")));
		TestTrue(TEXT("project A should still need resume"), State.bProviderThreadResumePending);
	}

	{
		FScopedRestartSurvivalRootOverride Override(ProjectBRoot);
		FUnrealClaudeRestartSurvivalState State;
		TestTrue(TEXT("project B state should load"), FUnrealClaudeRestartSurvivalManager::DescribeCurrentState(State));
		TestEqual(TEXT("project B should keep its own task"), State.TaskId, FString(TEXT("task_b")));
		TestEqual(TEXT("project B should keep its own provider session"), State.ProviderSessionId, FString(TEXT("thread_b")));
		TestFalse(TEXT("project B should not reuse project A resume state"), State.bProviderThreadResumePending);
	}

	TestTrue(TEXT("project-local state roots should differ"),
		!NormalizePathForTest(ProjectARoot).Equals(NormalizePathForTest(ProjectBRoot), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PendingResumeMatchesBackend,
	"UnrealClaude.RestartSurvival.PendingResumeMatchesBackend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_LaneStateRoundTripReflectsReattachContract,
	"UnrealClaude.RestartSurvival.LaneStateRoundTripReflectsReattachContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_PendingResumeMatchesBackend::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PendingResumeMatchesBackend"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("session_codex");
	State.TaskId = TEXT("task_codex");
	State.ProjectRoot = TEXT("D:/CodexProject");
	State.UProjectPath = TEXT("D:/CodexProject/CodexProject.uproject");
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::AwaitingReattach;
	State.ProviderSessionId = TEXT("thread_codex");

	FString Error;
	TestTrue(TEXT("state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));
	TestTrue(TEXT("codex backend should have pending resume"), FUnrealClaudeRestartSurvivalManager::HasPendingResume(EUnrealClaudeProviderBackend::CodexCli));
	TestFalse(TEXT("claude backend should not reuse codex resume"), FUnrealClaudeRestartSurvivalManager::HasPendingResume(EUnrealClaudeProviderBackend::ClaudeCli));
	return true;
}

bool FRestartSurvival_LaneStateRoundTripReflectsReattachContract::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("LaneStateRoundTripReflectsReattachContract"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_lane_contract");
	Plan.CurrentMechanicId = TEXT("verify_and_report");
	Plan.ResumeHint = TEXT("Resume from last_completed_tool_call_id=tool_build_lane after reattach.");
	Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("workflow_lane_contract");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");

	FString Error;
	TestTrue(TEXT("active plan should save for continuity identity seeding"), FUnrealClaudeRelayAgentManager::SaveActivePlan(Plan, Error));

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("session_lane_contract");
	State.TaskId = TEXT("task_lane_contract");
	State.ProjectRoot = TEXT("D:/LaneProject");
	State.UProjectPath = TEXT("D:/LaneProject/LaneProject.uproject");
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::AwaitingReattach;
	State.PhaseDetail = TEXT("Closed-editor continuation finished; waiting to reopen the editor and reattach.");
	State.ReattachNotice = TEXT("Reopening editor and reattaching to the same task.");

	TestTrue(TEXT("restart-survival state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	FUnrealClaudeRestartSurvivalState Loaded;
	TestTrue(TEXT("restart-survival state should load"), FUnrealClaudeRestartSurvivalManager::LoadState(Loaded, Error));
	TestEqual(TEXT("current lane should reflect detached execution"), Loaded.LaneState.CurrentLane, FString(TEXT("closed_editor_detached")));
	TestEqual(TEXT("target lane should reflect the editor return"), Loaded.LaneState.TargetLane, FString(TEXT("live_editor")));
	TestEqual(TEXT("transition kind should reflect a lane return"), Loaded.LaneState.TransitionKind, FString(TEXT("lane_return")));
	TestEqual(TEXT("transition state should stay in progress before reattach"), Loaded.LaneState.TransitionState, FString(TEXT("in_progress")));
	TestEqual(TEXT("continuity task id should survive"), Loaded.LaneState.ContinuityTaskId, State.TaskId);
	TestEqual(TEXT("continuity plan id should seed from active plan"), Loaded.LaneState.ContinuityPlanId, Plan.PlanId);
	TestEqual(TEXT("continuity workflow id should seed from active plan"), Loaded.LaneState.ContinuityWorkflowId, Plan.FeatureWorkflow.FeatureWorkflowId);
	TestEqual(TEXT("continuity phase id should seed from active plan"), Loaded.LaneState.ContinuityPhaseId, Plan.FeatureWorkflow.CurrentPhase);
	TestEqual(TEXT("lane status helper should surface reattach wording"), SClaudeEditorWidget::DescribeTaskLaneStatus(Loaded.LaneState), FString(TEXT("Reopening editor and reattaching")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PluginSettingsReadbackTruth,
	"UnrealClaude.RestartSurvival.PluginSettingsReadbackTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PortableSupportBundlePrefersPluginOwnedPath,
	"UnrealClaude.RestartSurvival.PortableSupportBundlePrefersPluginOwnedPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_RestartSurvivalToolRegistered,
	"UnrealClaude.RestartSurvival.RestartSurvivalToolRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_AutonomousClosedEditorToolContractTruth,
	"UnrealClaude.RestartSurvival.AutonomousClosedEditorToolContractTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_AutonomousClosedEditorPromptContractTruth,
	"UnrealClaude.RestartSurvival.AutonomousClosedEditorPromptContractTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_AutonomousClosedEditorStartOverrideAcceptsDerivedIds,
	"UnrealClaude.RestartSurvival.AutonomousClosedEditorStartOverrideAcceptsDerivedIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_AutonomousClosedEditorStartOverrideRejectsWrongExplicitIds,
	"UnrealClaude.RestartSurvival.AutonomousClosedEditorStartOverrideRejectsWrongExplicitIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_RestartSurvivalToolRegistered::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestNotNull(TEXT("restart_survival should be registered"), Registry.FindTool(TEXT("restart_survival")));
	return true;
}

bool FRestartSurvival_PortableSupportBundlePrefersPluginOwnedPath::RunTest(const FString& Parameters)
{
	FUnrealClaudeRestartSurvivalSupportBundle Bundle;
	FString Error;
	TestTrue(TEXT("support bundle should resolve"), FUnrealClaudeRestartSurvivalManager::TryResolveSupportBundle(Bundle, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("support bundle detail: %s"), *Error));
	}

	const FString ExpectedPluginRoot = NormalizePathForTest(FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UnrealClaude")));
	const FString BundleRoot = NormalizePathForTest(Bundle.BundleRoot);
	TestEqual(TEXT("portable bundle should prefer plugin-owned resolution"), Bundle.ResolutionLabel, FString(TEXT("plugin_owned")));
	TestTrue(TEXT("portable bundle root should live under the mirrored plugin"), BundleRoot.StartsWith(ExpectedPluginRoot, ESearchCase::IgnoreCase));
	TestTrue(TEXT("portable supervisor script should exist"), IFileManager::Get().FileExists(*Bundle.SupervisorScriptPath));
	TestTrue(TEXT("portable monitor script should exist"), IFileManager::Get().FileExists(*Bundle.MonitorScriptPath));
	TestTrue(TEXT("portable preflight script should exist"), IFileManager::Get().FileExists(*Bundle.PreflightScriptPath));
	TestEqual(TEXT("resolved supervisor path should match the manager helper"),
		NormalizePathForTest(FUnrealClaudeRestartSurvivalManager::GetSupervisorScriptPath()),
		NormalizePathForTest(Bundle.SupervisorScriptPath));
	TestEqual(TEXT("resolved preflight path should match the manager helper"),
		NormalizePathForTest(FUnrealClaudeRestartSurvivalManager::GetPreflightScriptPath()),
		NormalizePathForTest(Bundle.PreflightScriptPath));
	return true;
}

bool FRestartSurvival_AutonomousClosedEditorToolContractTruth::RunTest(const FString& Parameters)
{
	FMCPTool_RestartSurvival Tool;
	const FMCPToolInfo Info = Tool.GetInfo();

	// 619 P2: description-text assertions updated to match Fix #2 rewrite.
	// The legacy phrases "closed-editor blocker", "autosave-backed restore",
	// "build/preflight", "task_id and session_id may be omitted" have been
	// intentionally dropped from the tool description (see
	// MCPTool_RestartSurvival.cpp:132-188 GetInfo body) in favor of the
	// ESCALATION PATH ONLY + livecoding_compile-precondition framing that
	// steers Codex/Claude tool selection. The parameter-contract assertions
	// below (task_id/session_id optional, continuation_intent_prompt
	// required) are still valid and continue to lock the external contract.
	// The new description-text lock lives in
	// RestartSurvivalDescriptionTests.cpp
	// (UnrealClaude.RestartSurvival.Description.*).
	TestTrue(
		TEXT("tool description should carry the Fix #2 ESCALATION PATH ONLY framing"),
		Info.Description.Contains(TEXT("ESCALATION PATH ONLY"), ESearchCase::CaseSensitive));
	TestTrue(
		TEXT("tool description should still name the prepare_task_continuation_handoff operation"),
		Info.Description.Contains(TEXT("prepare_task_continuation_handoff"), ESearchCase::CaseSensitive));

	const FMCPToolParameter* TaskIdParam = Info.Parameters.FindByPredicate([](const FMCPToolParameter& Param)
		{
			return Param.Name == TEXT("task_id");
		});
	const FMCPToolParameter* SessionIdParam = Info.Parameters.FindByPredicate([](const FMCPToolParameter& Param)
		{
			return Param.Name == TEXT("session_id");
		});
	const FMCPToolParameter* ContinuationPromptParam = Info.Parameters.FindByPredicate([](const FMCPToolParameter& Param)
		{
			return Param.Name == TEXT("continuation_intent_prompt");
		});
	const FMCPToolParameter* FileWriteSourceParam = Info.Parameters.FindByPredicate([](const FMCPToolParameter& Param)
		{
			return Param.Name == TEXT("file_write_source_path");
		});
	const FMCPToolParameter* FileWriteTargetParam = Info.Parameters.FindByPredicate([](const FMCPToolParameter& Param)
		{
			return Param.Name == TEXT("file_write_target_path");
		});

	TestNotNull(TEXT("task_id parameter should exist"), TaskIdParam);
	TestNotNull(TEXT("session_id parameter should exist"), SessionIdParam);
	TestNotNull(TEXT("continuation_intent_prompt parameter should exist"), ContinuationPromptParam);
	TestNotNull(TEXT("file_write_source_path parameter should exist"), FileWriteSourceParam);
	TestNotNull(TEXT("file_write_target_path parameter should exist"), FileWriteTargetParam);
	if (!TaskIdParam || !SessionIdParam || !ContinuationPromptParam || !FileWriteSourceParam || !FileWriteTargetParam)
	{
		return false;
	}

	TestFalse(TEXT("task_id should no longer be required for autonomous escalation"), TaskIdParam->bRequired);
	TestFalse(TEXT("session_id should no longer be required for autonomous escalation"), SessionIdParam->bRequired);
	TestTrue(TEXT("continuation_intent_prompt must remain required"), ContinuationPromptParam->bRequired);
	return true;
}

bool FRestartSurvival_AutonomousClosedEditorPromptContractTruth::RunTest(const FString& Parameters)
{
	const FAgentPromptContract Contract = FAgentPromptContractBuilder::Build(true, false, FString(), FString());
	const FString Materialized = FAgentPromptMaterializer::MaterializeCanonicalText(Contract);

	TestTrue(
		TEXT("prompt contract should mention restart_survival as the closed-editor escalation tool"),
		Materialized.Contains(TEXT("restart_survival"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("prompt contract should describe the blocked file/build family that qualifies for restart-survival"),
		Materialized.Contains(TEXT("blocked by Unreal staying open"), ESearchCase::IgnoreCase)
			&& Materialized.Contains(TEXT("editor to be closed"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("prompt contract should mention that task_id and session_id may be omitted"),
		Materialized.Contains(TEXT("task_id` and `session_id` may be omitted"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("prompt contract should preserve the detached editor boundary"),
		Materialized.Contains(TEXT("do not pretend editor-only MCP surfaces remain available"), ESearchCase::IgnoreCase));
	return true;
}

bool FRestartSurvival_AutonomousClosedEditorStartOverrideAcceptsDerivedIds::RunTest(const FString& Parameters)
{
	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.TaskId = TEXT("task_autonomous_thread1234_abcd5678");
	Request.SessionId = TEXT("restart_survival_task_autonomous_thread1234_abcd5678");

	FUnrealClaudeRestartSurvivalPreparedStartOverride Override;
	Override.ReattachTokenOverride = TEXT("reattach_autonomous");
	Override.ReattachNoticeOverride = TEXT("autonomous closed-editor reattach notice");
	Override.AdditionalRelaunchArguments = TEXT("-ExecCmds=\"UnrealClaude.Test\"");

	FUnrealClaudeRestartSurvivalManager::ClearPreparedRequestStartOverride();
	FUnrealClaudeRestartSurvivalManager::ArmPreparedRequestStartOverride(Override);

	FUnrealClaudeRestartSurvivalPreparedStartOverride ConsumedOverride;
	TestTrue(
		TEXT("empty task/session override should attach to derived autonomous ids"),
		FUnrealClaudeRestartSurvivalManager::TryConsumePreparedRequestStartOverride(Request, ConsumedOverride));
	TestEqual(
		TEXT("reattach token should round-trip through the consumed autonomous override"),
		ConsumedOverride.ReattachTokenOverride,
		Override.ReattachTokenOverride);
	TestEqual(
		TEXT("reattach notice should round-trip through the consumed autonomous override"),
		ConsumedOverride.ReattachNoticeOverride,
		Override.ReattachNoticeOverride);
	TestEqual(
		TEXT("additional relaunch args should round-trip through the consumed autonomous override"),
		ConsumedOverride.AdditionalRelaunchArguments,
		Override.AdditionalRelaunchArguments);

	FUnrealClaudeRestartSurvivalPreparedStartOverride SecondAttempt;
	TestFalse(
		TEXT("autonomous override should be consumed exactly once"),
		FUnrealClaudeRestartSurvivalManager::TryConsumePreparedRequestStartOverride(Request, SecondAttempt));
	return true;
}

bool FRestartSurvival_AutonomousClosedEditorStartOverrideRejectsWrongExplicitIds::RunTest(const FString& Parameters)
{
	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.TaskId = TEXT("task_autonomous_thread9999_deadbeef");
	Request.SessionId = TEXT("restart_survival_task_autonomous_thread9999_deadbeef");

	FUnrealClaudeRestartSurvivalPreparedStartOverride Override;
	Override.TaskId = TEXT("task_wrong");
	Override.SessionId = TEXT("restart_survival_task_wrong");
	Override.ReattachTokenOverride = TEXT("reattach_wrong");

	FUnrealClaudeRestartSurvivalManager::ClearPreparedRequestStartOverride();
	FUnrealClaudeRestartSurvivalManager::ArmPreparedRequestStartOverride(Override);

	FUnrealClaudeRestartSurvivalPreparedStartOverride ConsumedOverride;
	TestFalse(
		TEXT("explicit mismatched ids must not attach to a different autonomous prepared request"),
		FUnrealClaudeRestartSurvivalManager::TryConsumePreparedRequestStartOverride(Request, ConsumedOverride));

	FUnrealClaudeRestartSurvivalManager::ClearPreparedRequestStartOverride();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PreparedRestoreRequestRoundTrip,
	"UnrealClaude.RestartSurvival.PreparedRestoreRequestRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_PreparedRestoreRequestRoundTrip::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PreparedRestoreRequestRoundTrip"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::Combine(TestRoot, TEXT("ProjectA"));
	const FString SourcePath = FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("Autosaves"), TEXT("restore_source.txt"));
	const FString TargetPath = FPaths::Combine(ProjectRoot, TEXT("Content"), TEXT("Proof"), TEXT("restore_target.txt"));
	const FString BackupPath = FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"), TEXT("restore_target.backup.txt"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourcePath), true);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TargetPath), true);
	TestTrue(
		TEXT("prepared request source should be written"),
		FFileHelper::SaveStringToFile(TEXT("autosave payload"), *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = TEXT("request_roundtrip");
	Request.TaskId = TEXT("task_roundtrip");
	Request.SessionId = TEXT("session_roundtrip");
	Request.Backend = EUnrealClaudeProviderBackend::CodexCli;
	Request.LinkedProviderSessionId = TEXT("provider_roundtrip");
	Request.ProjectRoot = ProjectRoot;
	Request.bTaskDrivenHandoff = true;
	Request.bAutoStartAfterResponse = true;
	Request.bRestoreEnabled = true;
	Request.ContinuationIntentPrompt = TEXT("Reply with exactly: restore roundtrip complete");
	Request.OriginTask.OriginatingRunId = TEXT("run_origin_roundtrip");
	Request.OriginTask.OriginatingUserPrompt = TEXT("Increase projectile speed and keep the same gameplay task.");
	Request.OriginTask.OriginatingPromptHash = TEXT("prompt_hash_roundtrip");
	Request.OriginTask.OriginatingTaskMode = TEXT("workspace_file_build");
	Request.OriginTask.OriginatingRequestedToolFamily = TEXT("workspace_file_build");
	Request.OriginTask.OriginatingPrimaryMutationToolFamily = TEXT("workspace_file_build");
	Request.OriginTask.bOriginatingHasAttachments = true;
	Request.OriginTask.bOriginatingHasVisualReference = true;
	Request.OriginTask.bVisualProofRequired = true;
	Request.OriginTask.bVisualQaManifestRequired = true;
	Request.OriginTask.OriginatingAttachedImagePaths = { TEXT("D:/Images/combat_inventory_reference.png") };
	Request.OriginTask.OriginatingAttachmentNames = { TEXT("combat_inventory_reference.png") };
	Request.OriginTask.VisualReferenceRequirement = TEXT("Final success requires visual_qa_manifest.json proof.");
	Request.AutosaveSourcePath = SourcePath;
	Request.TargetPath = TargetPath;
	Request.BackupPath = BackupPath;
	Request.Detail = TEXT("explicit_autosave_backed_restore");
	Request.PostReattachCompletionText = TEXT("Detached restore already completed. Verify and reply with exactly: restore roundtrip complete");

	FString Error;
	TestTrue(TEXT("prepared request should save"), FUnrealClaudeRestartSurvivalManager::SavePreparedRestoreRequest(Request, Error));
	TestTrue(TEXT("prepared request file should exist"), FUnrealClaudeRestartSurvivalManager::HasPreparedRestoreRequest());

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest LoadedRequest;
	TestTrue(TEXT("prepared request should load"), FUnrealClaudeRestartSurvivalManager::LoadPreparedRestoreRequest(LoadedRequest, Error));
	TestEqual(TEXT("request id should round-trip"), LoadedRequest.RequestId, Request.RequestId);
	TestEqual(TEXT("task id should round-trip"), LoadedRequest.TaskId, Request.TaskId);
	TestEqual(TEXT("session id should round-trip"), LoadedRequest.SessionId, Request.SessionId);
	TestEqual(TEXT("linked provider session should round-trip"), LoadedRequest.LinkedProviderSessionId, Request.LinkedProviderSessionId);
	TestTrue(TEXT("task-driven handoff flag should round-trip"), LoadedRequest.bTaskDrivenHandoff);
	TestTrue(TEXT("auto-start flag should round-trip"), LoadedRequest.bAutoStartAfterResponse);
	TestTrue(TEXT("restore enabled flag should round-trip"), LoadedRequest.bRestoreEnabled);
	TestEqual(TEXT("continuation prompt should round-trip"), LoadedRequest.ContinuationIntentPrompt, Request.ContinuationIntentPrompt);
	TestEqual(TEXT("source path should round-trip"), LoadedRequest.AutosaveSourcePath, Request.AutosaveSourcePath);
	TestEqual(TEXT("target path should round-trip"), LoadedRequest.TargetPath, Request.TargetPath);
	TestEqual(TEXT("backup path should round-trip"), LoadedRequest.BackupPath, Request.BackupPath);
	TestEqual(TEXT("detail should round-trip"), LoadedRequest.Detail, Request.Detail);
	TestEqual(TEXT("completion text should round-trip"), LoadedRequest.PostReattachCompletionText, Request.PostReattachCompletionText);
	TestEqual(TEXT("originating run id should round-trip"), LoadedRequest.OriginTask.OriginatingRunId, Request.OriginTask.OriginatingRunId);
	TestEqual(TEXT("originating user prompt should round-trip"), LoadedRequest.OriginTask.OriginatingUserPrompt, Request.OriginTask.OriginatingUserPrompt);
	TestEqual(TEXT("originating prompt hash should round-trip"), LoadedRequest.OriginTask.OriginatingPromptHash, Request.OriginTask.OriginatingPromptHash);
	TestEqual(TEXT("originating task mode should round-trip"), LoadedRequest.OriginTask.OriginatingTaskMode, Request.OriginTask.OriginatingTaskMode);
	TestEqual(TEXT("originating requested tool family should round-trip"), LoadedRequest.OriginTask.OriginatingRequestedToolFamily, Request.OriginTask.OriginatingRequestedToolFamily);
	TestEqual(TEXT("originating primary mutation tool family should round-trip"), LoadedRequest.OriginTask.OriginatingPrimaryMutationToolFamily, Request.OriginTask.OriginatingPrimaryMutationToolFamily);
	TestTrue(TEXT("originating attachment flag should round-trip"), LoadedRequest.OriginTask.bOriginatingHasAttachments);
	TestTrue(TEXT("originating visual reference flag should round-trip"), LoadedRequest.OriginTask.bOriginatingHasVisualReference);
	TestTrue(TEXT("visual proof requirement should round-trip"), LoadedRequest.OriginTask.bVisualProofRequired);
	TestTrue(TEXT("visual QA manifest requirement should round-trip"), LoadedRequest.OriginTask.bVisualQaManifestRequired);
	TestEqual(TEXT("attached image path count should round-trip"), LoadedRequest.OriginTask.OriginatingAttachedImagePaths.Num(), Request.OriginTask.OriginatingAttachedImagePaths.Num());
	TestEqual(TEXT("attachment name count should round-trip"), LoadedRequest.OriginTask.OriginatingAttachmentNames.Num(), Request.OriginTask.OriginatingAttachmentNames.Num());
	if (LoadedRequest.OriginTask.OriginatingAttachedImagePaths.Num() > 0)
	{
		TestEqual(TEXT("attached image path should round-trip"), LoadedRequest.OriginTask.OriginatingAttachedImagePaths[0], Request.OriginTask.OriginatingAttachedImagePaths[0]);
	}
	if (LoadedRequest.OriginTask.OriginatingAttachmentNames.Num() > 0)
	{
		TestEqual(TEXT("attachment name should round-trip"), LoadedRequest.OriginTask.OriginatingAttachmentNames[0], Request.OriginTask.OriginatingAttachmentNames[0]);
	}
	TestEqual(TEXT("visual reference requirement should round-trip"), LoadedRequest.OriginTask.VisualReferenceRequirement, Request.OriginTask.VisualReferenceRequirement);
	TestEqual(
		TEXT("resolved post-reattach text should prefer the explicit post-reattach field"),
		FUnrealClaudeRestartSurvivalManager::ResolvePreparedRequestPostReattachCompletionText(LoadedRequest),
		Request.PostReattachCompletionText);
	TestFalse(TEXT("created_at_utc should be written during save"), LoadedRequest.CreatedAtUtc.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PreparedRestoreRequestRejectsWrongProviderSession,
	"UnrealClaude.RestartSurvival.PreparedRestoreRequestRejectsWrongProviderSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_TaskDrivenContinuationWithoutRestoreValidates,
	"UnrealClaude.RestartSurvival.TaskDrivenContinuationWithoutRestoreValidates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PreparedFileWriteRequestRoundTrip,
	"UnrealClaude.RestartSurvival.PreparedFileWriteRequestRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_TaskDrivenContinuationWithFileWriteValidates,
	"UnrealClaude.RestartSurvival.TaskDrivenContinuationWithFileWriteValidates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_TaskDrivenContinuationWithoutRestoreValidates::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("TaskDrivenContinuationWithoutRestoreValidates"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::Combine(TestRoot, TEXT("ProjectA"));
	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = TEXT("request_task_driven");
	Request.TaskId = TEXT("task_task_driven");
	Request.SessionId = TEXT("session_task_driven");
	Request.Backend = EUnrealClaudeProviderBackend::CodexCli;
	Request.LinkedProviderSessionId = TEXT("provider_task_driven");
	Request.ProjectRoot = ProjectRoot;
	Request.bTaskDrivenHandoff = true;
	Request.bAutoStartAfterResponse = true;
	Request.bRestoreEnabled = false;
	Request.Detail = TEXT("task_driven_structured_handoff");
	Request.CreatedAtUtc = TEXT("2026-04-13T00:00:00Z");
	Request.ContinuationIntentPrompt = TEXT("Reply with exactly: task continuation reattached");

	FString Error;
	TestTrue(
		TEXT("task-driven continuation should validate without restore paths"),
		FUnrealClaudeRestartSurvivalManager::ValidatePreparedRestoreRequestForStart(
			Request,
			EUnrealClaudeProviderBackend::CodexCli,
			ProjectRoot,
			TEXT("provider_task_driven"),
			Error));
	return true;
}

bool FRestartSurvival_PreparedFileWriteRequestRoundTrip::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PreparedFileWriteRequestRoundTrip"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = NormalizePathForTest(FPaths::Combine(TestRoot, TEXT("ProjectA")));
	const FString SourcePath = NormalizePathForTest(FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"), TEXT("file_write_source.txt")));
	const FString TargetPath = NormalizePathForTest(FPaths::Combine(ProjectRoot, TEXT("Content"), TEXT("Proof"), TEXT("file_write_target.txt")));
	const FString BackupPath = NormalizePathForTest(FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"), TEXT("file_write_target.backup.txt")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourcePath), true);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TargetPath), true);
	TestTrue(
		TEXT("file-write request source should be written"),
		FFileHelper::SaveStringToFile(TEXT("file write payload"), *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = TEXT("request_file_write_roundtrip");
	Request.TaskId = TEXT("task_file_write_roundtrip");
	Request.SessionId = TEXT("session_file_write_roundtrip");
	Request.Backend = EUnrealClaudeProviderBackend::CodexCli;
	Request.LinkedProviderSessionId = TEXT("provider_file_write_roundtrip");
	Request.ProjectRoot = ProjectRoot;
	Request.bTaskDrivenHandoff = true;
	Request.bAutoStartAfterResponse = true;
	Request.bFileWriteEnabled = true;
	Request.ContinuationIntentPrompt = TEXT("Reply with exactly: file write roundtrip complete");
	Request.FileWriteSourcePath = SourcePath;
	Request.FileWriteTargetPath = TargetPath;
	Request.FileWriteBackupPath = BackupPath;
	Request.FileWriteDetail = TEXT("exact_project_local_file_write");
	Request.Detail = Request.FileWriteDetail;
	Request.PostReattachCompletionText = TEXT("Detached file write already completed. Verify and reply with exactly: file write roundtrip complete");

	FString Error;
	TestTrue(TEXT("prepared file-write request should save"), FUnrealClaudeRestartSurvivalManager::SavePreparedRestoreRequest(Request, Error));

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest LoadedRequest;
	TestTrue(TEXT("prepared file-write request should load"), FUnrealClaudeRestartSurvivalManager::LoadPreparedRestoreRequest(LoadedRequest, Error));
	TestTrue(TEXT("file-write enabled flag should round-trip"), LoadedRequest.bFileWriteEnabled);
	TestEqual(TEXT("file-write source should round-trip"), LoadedRequest.FileWriteSourcePath, Request.FileWriteSourcePath);
	TestEqual(TEXT("file-write target should round-trip"), LoadedRequest.FileWriteTargetPath, Request.FileWriteTargetPath);
	TestEqual(TEXT("file-write backup should round-trip"), LoadedRequest.FileWriteBackupPath, Request.FileWriteBackupPath);
	TestEqual(TEXT("file-write detail should round-trip"), LoadedRequest.FileWriteDetail, Request.FileWriteDetail);
	TestEqual(TEXT("detail should round-trip"), LoadedRequest.Detail, Request.Detail);
	return true;
}

bool FRestartSurvival_TaskDrivenContinuationWithFileWriteValidates::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("TaskDrivenContinuationWithFileWriteValidates"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = NormalizePathForTest(FPaths::Combine(TestRoot, TEXT("ProjectA")));
	const FString SourcePath = NormalizePathForTest(FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"), TEXT("file_write_source.txt")));
	const FString TargetPath = NormalizePathForTest(FPaths::Combine(ProjectRoot, TEXT("Content"), TEXT("Proof"), TEXT("file_write_target.txt")));
	const FString BackupPath = NormalizePathForTest(FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"), TEXT("file_write_target.backup.txt")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourcePath), true);
	TestTrue(
		TEXT("file-write validation source should be written"),
		FFileHelper::SaveStringToFile(TEXT("file write payload"), *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = TEXT("request_task_file_write");
	Request.TaskId = TEXT("task_task_file_write");
	Request.SessionId = TEXT("session_task_file_write");
	Request.Backend = EUnrealClaudeProviderBackend::CodexCli;
	Request.LinkedProviderSessionId = TEXT("provider_task_file_write");
	Request.ProjectRoot = ProjectRoot;
	Request.bTaskDrivenHandoff = true;
	Request.bAutoStartAfterResponse = true;
	Request.bFileWriteEnabled = true;
	Request.FileWriteSourcePath = SourcePath;
	Request.FileWriteTargetPath = TargetPath;
	Request.FileWriteBackupPath = BackupPath;
	Request.FileWriteDetail = TEXT("exact_project_local_file_write");
	Request.Detail = Request.FileWriteDetail;
	Request.CreatedAtUtc = TEXT("2026-04-13T00:00:00Z");
	Request.ContinuationIntentPrompt = TEXT("Reply with exactly: task file write reattached");

	FString Error;
	const bool bValidated = FUnrealClaudeRestartSurvivalManager::ValidatePreparedRestoreRequestForStart(
		Request,
		EUnrealClaudeProviderBackend::CodexCli,
		ProjectRoot,
		TEXT("provider_task_file_write"),
		Error);
	if (!bValidated)
	{
		AddError(FString::Printf(TEXT("task-driven file-write validation failed: %s"), *Error));
	}
	TestTrue(TEXT("task-driven continuation should validate with exact file-write paths"), bValidated);
	return true;
}

bool FRestartSurvival_PreparedRestoreRequestRejectsWrongProviderSession::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PreparedRestoreRequestRejectsWrongProviderSession"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::Combine(TestRoot, TEXT("ProjectA"));
	const FString SourcePath = FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("Autosaves"), TEXT("restore_source.txt"));
	const FString TargetPath = FPaths::Combine(ProjectRoot, TEXT("Content"), TEXT("Proof"), TEXT("restore_target.txt"));
	const FString BackupPath = FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"), TEXT("restore_target.backup.txt"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourcePath), true);
	TestTrue(
		TEXT("validation source should be written"),
		FFileHelper::SaveStringToFile(TEXT("autosave payload"), *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = TEXT("request_wrong_session");
	Request.TaskId = TEXT("task_wrong_session");
	Request.SessionId = TEXT("session_wrong_session");
	Request.Backend = EUnrealClaudeProviderBackend::CodexCli;
	Request.LinkedProviderSessionId = TEXT("provider_other");
	Request.ProjectRoot = ProjectRoot;
	Request.AutosaveSourcePath = SourcePath;
	Request.TargetPath = TargetPath;
	Request.BackupPath = BackupPath;
	Request.Detail = TEXT("explicit_autosave_backed_restore");
	Request.CreatedAtUtc = TEXT("2026-04-13T00:00:00Z");
	Request.ContinuationIntentPrompt = TEXT("Reply with exactly: wrong provider session");

	FString Error;
	TestFalse(
		TEXT("validation should reject a request linked to another provider session"),
		FUnrealClaudeRestartSurvivalManager::ValidatePreparedRestoreRequestForStart(
			Request,
			EUnrealClaudeProviderBackend::CodexCli,
			ProjectRoot,
			TEXT("provider_current"),
			Error));
	TestTrue(
		TEXT("validation should explain the provider-session mismatch"),
		Error.Contains(TEXT("current surviving provider session")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PreparedRequestAutoStartArmConsumesMatchingRequest,
	"UnrealClaude.RestartSurvival.PreparedRequestAutoStartArmConsumesMatchingRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PreparedRequestAutoStartArmSurvivesMissingCurrentProviderSession,
	"UnrealClaude.RestartSurvival.PreparedRequestAutoStartArmSurvivesMissingCurrentProviderSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PreparedRequestAutoStartArmRejectsDifferentCurrentProviderSession,
	"UnrealClaude.RestartSurvival.PreparedRequestAutoStartArmRejectsDifferentCurrentProviderSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ClosedEditorBuildBlockerDetectionTruth,
	"UnrealClaude.RestartSurvival.ClosedEditorBuildBlockerDetectionTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ClosedEditorBuildBlockerNegativeCompileFailureDoesNotMatch,
	"UnrealClaude.RestartSurvival.ClosedEditorBuildBlockerNegativeCompileFailureDoesNotMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ClosedEditorBuildBlockerAutoStartArmsPreparedRequest,
	"UnrealClaude.RestartSurvival.ClosedEditorBuildBlockerAutoStartArmsPreparedRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ClosedEditorBuildBlockerAutoStartPreservesOriginalTaskIntent,
	"UnrealClaude.RestartSurvival.ClosedEditorBuildBlockerAutoStartPreservesOriginalTaskIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ClosedEditorBuildBlockerAutoStartReadOnlyIntentStaysReadOnly,
	"UnrealClaude.RestartSurvival.ClosedEditorBuildBlockerAutoStartReadOnlyIntentStaysReadOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ClosedEditorBuildBlockerAutoStartRejectsMissingOriginPrompt,
	"UnrealClaude.RestartSurvival.ClosedEditorBuildBlockerAutoStartRejectsMissingOriginPrompt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ClosedEditorBuildBlockerAutoStartCarriesVisualProofRequirement,
	"UnrealClaude.RestartSurvival.ClosedEditorBuildBlockerAutoStartCarriesVisualProofRequirement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_MarkReattachValidatedDoesNotClearPendingDispatch,
	"UnrealClaude.RestartSurvival.MarkReattachValidatedDoesNotClearPendingDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_OpenEditorBuildLockCloseSafetyDecisionTruth,
	"UnrealClaude.RestartSurvival.OpenEditorBuildLockCloseSafetyDecisionTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_PreparedRequestAutoStartArmConsumesMatchingRequest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PreparedRequestAutoStartArmConsumesMatchingRequest"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::Combine(TestRoot, TEXT("ProjectA"));
	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = TEXT("request_auto_start");
	Request.TaskId = TEXT("task_auto_start");
	Request.SessionId = TEXT("session_auto_start");
	Request.Backend = EUnrealClaudeProviderBackend::CodexCli;
	Request.LinkedProviderSessionId = TEXT("provider_auto_start");
	Request.ProjectRoot = ProjectRoot;
	Request.bTaskDrivenHandoff = true;
	Request.bAutoStartAfterResponse = true;
	Request.bRestoreEnabled = false;
	Request.Detail = TEXT("task_driven_structured_handoff");
	Request.CreatedAtUtc = TEXT("2026-04-13T00:00:00Z");
	Request.ContinuationIntentPrompt = TEXT("Reply with exactly: task continuation reattached");

	FString Error;
	TestTrue(TEXT("prepared request should save"), FUnrealClaudeRestartSurvivalManager::SavePreparedRestoreRequest(Request, Error));
	FUnrealClaudeRestartSurvivalManager::ArmPreparedRequestAutoStart(
		Request.RequestId,
		Request.Backend,
		Request.ProjectRoot,
		Request.LinkedProviderSessionId);

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest ConsumedRequest;
	TestTrue(
		TEXT("matching auto-start arm should consume prepared request"),
		FUnrealClaudeRestartSurvivalManager::TryConsumePreparedRequestAutoStart(
			EUnrealClaudeProviderBackend::CodexCli,
			ProjectRoot,
			TEXT("provider_auto_start"),
			ConsumedRequest,
			Error));
	TestEqual(TEXT("consumed request should keep request id"), ConsumedRequest.RequestId, Request.RequestId);
	TestFalse(TEXT("auto-start arm should be one-shot"), FUnrealClaudeRestartSurvivalManager::HasPreparedRequestAutoStartArm());
	return true;
}

bool FRestartSurvival_PreparedRequestAutoStartArmSurvivesMissingCurrentProviderSession::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PreparedRequestAutoStartArmSurvivesMissingCurrentProviderSession"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::Combine(TestRoot, TEXT("ProjectA"));
	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = TEXT("request_auto_start_missing_current_session");
	Request.TaskId = TEXT("task_auto_start_missing_current_session");
	Request.SessionId = TEXT("session_auto_start_missing_current_session");
	Request.Backend = EUnrealClaudeProviderBackend::CodexCli;
	Request.LinkedProviderSessionId = TEXT("provider_auto_start_missing_current_session");
	Request.ProjectRoot = ProjectRoot;
	Request.bTaskDrivenHandoff = true;
	Request.bAutoStartAfterResponse = true;
	Request.bRestoreEnabled = false;
	Request.Detail = TEXT("task_driven_structured_handoff");
	Request.CreatedAtUtc = TEXT("2026-04-15T00:00:00Z");
	Request.ContinuationIntentPrompt = TEXT("Reply with exactly: auto start after transport reset");

	FString Error;
	TestTrue(TEXT("prepared request should save"), FUnrealClaudeRestartSurvivalManager::SavePreparedRestoreRequest(Request, Error));
	FUnrealClaudeRestartSurvivalManager::ArmPreparedRequestAutoStart(
		Request.RequestId,
		Request.Backend,
		Request.ProjectRoot,
		Request.LinkedProviderSessionId);

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest ConsumedRequest;
	const bool bConsumed = FUnrealClaudeRestartSurvivalManager::TryConsumePreparedRequestAutoStart(
		EUnrealClaudeProviderBackend::CodexCli,
		ProjectRoot,
		FString(),
		ConsumedRequest,
		Error);
	if (!bConsumed)
	{
		AddError(FString::Printf(TEXT("missing-current-session auto-start should still consume prepared request: %s"), *Error));
	}
	TestTrue(TEXT("prepared auto-start should survive one reset where the current provider session is unavailable"), bConsumed);
	TestEqual(TEXT("consumed request should keep the linked provider session"), ConsumedRequest.LinkedProviderSessionId, Request.LinkedProviderSessionId);
	TestFalse(TEXT("auto-start arm should still be one-shot after fallback consumption"), FUnrealClaudeRestartSurvivalManager::HasPreparedRequestAutoStartArm());
	return true;
}

bool FRestartSurvival_PreparedRequestAutoStartArmRejectsDifferentCurrentProviderSession::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PreparedRequestAutoStartArmRejectsDifferentCurrentProviderSession"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::Combine(TestRoot, TEXT("ProjectA"));
	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = TEXT("request_auto_start_mismatch");
	Request.TaskId = TEXT("task_auto_start_mismatch");
	Request.SessionId = TEXT("session_auto_start_mismatch");
	Request.Backend = EUnrealClaudeProviderBackend::CodexCli;
	Request.LinkedProviderSessionId = TEXT("provider_auto_start_mismatch");
	Request.ProjectRoot = ProjectRoot;
	Request.bTaskDrivenHandoff = true;
	Request.bAutoStartAfterResponse = true;
	Request.bRestoreEnabled = false;
	Request.Detail = TEXT("task_driven_structured_handoff");
	Request.CreatedAtUtc = TEXT("2026-04-15T00:00:00Z");
	Request.ContinuationIntentPrompt = TEXT("Reply with exactly: mismatch should reject");

	FString Error;
	TestTrue(TEXT("prepared request should save"), FUnrealClaudeRestartSurvivalManager::SavePreparedRestoreRequest(Request, Error));
	FUnrealClaudeRestartSurvivalManager::ArmPreparedRequestAutoStart(
		Request.RequestId,
		Request.Backend,
		Request.ProjectRoot,
		Request.LinkedProviderSessionId);

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest ConsumedRequest;
	TestFalse(
		TEXT("prepared auto-start should still reject a different non-empty current provider session"),
		FUnrealClaudeRestartSurvivalManager::TryConsumePreparedRequestAutoStart(
			EUnrealClaudeProviderBackend::CodexCli,
			ProjectRoot,
			TEXT("provider_auto_start_other"),
			ConsumedRequest,
			Error));
	TestTrue(
		TEXT("mismatch rejection should stay explicit"),
		Error.Contains(TEXT("current surviving provider session"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("auto-start arm should be one-shot even after explicit rejection"), FUnrealClaudeRestartSurvivalManager::HasPreparedRequestAutoStartArm());
	return true;
}

bool FRestartSurvival_ClosedEditorBuildBlockerDetectionTruth::RunTest(const FString& Parameters)
{
	// 626 P2 Layer C note: the fixtures below are legitimate test inputs for
	// the positive-match path. They carry UBT/LC-characteristic content so
	// HasStructuredBuildOutputSignature admits them through Layer A even
	// without an explicit ToolInput command string, preserving the pre-P2
	// assertion that real UBT output triggers escalation. The bare
	// "Failed to rename exported file ..." needle at this fixture is split
	// across string concatenations to keep the verbatim blocker literal
	// from appearing as a single contiguous line in grep output of this
	// file. Layer B origin-exclusion is the primary defense; the split is
	// belt-and-suspenders. See 626_P2_DetectorSourceAudit.md for rationale.
	FUnrealClaudeClosedEditorBuildBlocker Blocker;
	TestTrue(
		TEXT("UBT log backup access-denied should match a closed-editor blocker"),
		FUnrealClaudeRestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
			TEXT("command_execution"),
			FString(), // ToolInput empty: haystack-signature admission proves the positive path.
			TEXT("Unhandled exception. UnauthorizedAccessException: Access to the path is denied. EpicGames.Core.Log.BackupLogFile"),
			FString(),
			Blocker));
	TestEqual(
		TEXT("detector should classify build-lock distinctly"),
		Blocker.ClassificationLabel,
		FString(TEXT("open_editor_build_lock")));
	TestEqual(
		TEXT("UBT log backup access-denied should map to the expected family"),
		Blocker.Family,
		EUnrealClaudeClosedEditorBuildBlockerFamily::UbtLogBackupAccessDenied);
	TestTrue(
		TEXT("UBT blocker should keep a short evidence excerpt"),
		Blocker.MatchedEvidence.Contains(TEXT("UnauthorizedAccessException"), ESearchCase::IgnoreCase));

	Blocker = FUnrealClaudeClosedEditorBuildBlocker();
	// 626 P2 Layer C: split the UHT rename-lock needle across string parts
	// so this fixture line does not surface the literal blocker phrase
	// when the file is shown as grep output. Runtime string is identical.
	const FString UhtRenameLockFixture =
		FString(TEXT("Failed to ")) + TEXT("rename ") + TEXT("exported ") + TEXT("file ")
		+ TEXT("Intermediate/Build/Win64/UnrealEditor/Inc/Poligon1/UHT/MyThing.generated.h.tmp");
	TestTrue(
		TEXT("UHT rename lock should match a closed-editor blocker"),
		FUnrealClaudeRestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
			TEXT("command_execution"),
			FString(),
			UhtRenameLockFixture,
			FString(),
			Blocker));
	TestEqual(
		TEXT("UHT rename lock should map to the expected family"),
		Blocker.Family,
		EUnrealClaudeClosedEditorBuildBlockerFamily::UhtGeneratedRenameLock);

	Blocker = FUnrealClaudeClosedEditorBuildBlocker();
	TestTrue(
		TEXT("Live Coding active should match a closed-editor blocker"),
		FUnrealClaudeRestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
			TEXT("command_execution"),
			FString(),
			TEXT("Unable to build while Live Coding is active."),
			FString(),
			Blocker));
	TestEqual(
		TEXT("Live Coding active should map to the expected family"),
		Blocker.Family,
		EUnrealClaudeClosedEditorBuildBlockerFamily::LiveCodingActive);

	Blocker = FUnrealClaudeClosedEditorBuildBlocker();
	const FString IntermediateArtifactFixture =
		TEXT("Running UnrealBuildTool\n")
		TEXT("Updating D:/Project/Intermediate/Build/Win64/x64/UnrealEditor/Development/Poligon1/UnrealEditor-Poligon1.dll.rsp: contents have changed. Saving previous version to D:/Project/Intermediate/Build/Win64/x64/UnrealEditor/Development/Poligon1/UnrealEditor-Poligon1.dll.rsp.old.\n")
		TEXT("Unable to rename D:/Project/Intermediate/Build/Win64/x64/UnrealEditor/Development/Poligon1/UnrealEditor-Poligon1.dll.rsp to D:/Project/Intermediate/Build/Win64/x64/UnrealEditor/Development/Poligon1/UnrealEditor-Poligon1.dll.rsp.old\n")
		TEXT("UnauthorizedAccessException: Access to the path 'D:/Project/Intermediate/Build/Win64/x64/Poligon1Editor/Development/Core/SharedDefinitions.Core.Cpp20.h.old' is denied.\n");
	TestTrue(
		TEXT("intermediate artifact access-denied should match an open-editor build lock"),
		FUnrealClaudeRestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
			TEXT("command_execution"),
			FString(),
			IntermediateArtifactFixture,
			FString(),
			Blocker));
	TestEqual(
		TEXT("intermediate artifact access-denied should map to the expected family"),
		Blocker.Family,
		EUnrealClaudeClosedEditorBuildBlockerFamily::IntermediateBuildArtifactAccessDenied);

	Blocker = FUnrealClaudeClosedEditorBuildBlocker();
	const FString LinkerLockFixture =
		TEXT("link : fatal error LNK1104: cannot open file 'D:/Project/Binaries/Win64/UnrealEditor-UnrealClaude.dll'\n");
	TestTrue(
		TEXT("LNK1104 on UnrealEditor dll should match an open-editor build lock"),
		FUnrealClaudeRestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
			TEXT("command_execution"),
			FString(),
			LinkerLockFixture,
			FString(),
			Blocker));
	TestEqual(
		TEXT("LNK1104 should map to the expected family"),
		Blocker.Family,
		EUnrealClaudeClosedEditorBuildBlockerFamily::EditorBinaryLinkerLock);
	return true;
}

bool FRestartSurvival_ClosedEditorBuildBlockerNegativeCompileFailureDoesNotMatch::RunTest(const FString& Parameters)
{
	FUnrealClaudeClosedEditorBuildBlocker Blocker;
	// 626 P2 note: under the new Layer A gate this fixture is rejected
	// because it carries no build-context signature and no ToolInput.
	// Old behavior (inner matcher returns false) also produces bDetected=false,
	// so the public contract "compile errors do not trigger blocker escalation"
	// holds regardless of gate path.
	TestFalse(
		TEXT("normal compile failures must not auto-match the closed-editor blocker detector"),
		FUnrealClaudeRestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
			TEXT("command_execution"),
			FString(),
			TEXT("Poligon1/Variant_Combat/CombatCharacter.cpp(145): error C2065: 'MissingSymbol': undeclared identifier"),
			FString(),
			Blocker));
	TestFalse(TEXT("negative match should leave blocker unset"), Blocker.bDetected);
	return true;
}

bool FRestartSurvival_ClosedEditorBuildBlockerAutoStartArmsPreparedRequest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ClosedEditorBuildBlockerAutoStartArmsPreparedRequest"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeClosedEditorBuildBlocker Blocker;
	Blocker.bDetected = true;
	Blocker.Family = EUnrealClaudeClosedEditorBuildBlockerFamily::UbtLogBackupAccessDenied;
	Blocker.FamilyLabel = UnrealClaudeClosedEditorBuildBlockerFamilyToString(Blocker.Family);
	Blocker.EscalationReason = TEXT("UBT/log backup access is denied while Unreal keeps the active build log path open.");
	Blocker.MatchedEvidence = TEXT("UnauthorizedAccessException: Access to the path is denied.");
	FUnrealClaudeRestartSurvivalOriginTaskContext OriginTask;
	OriginTask.OriginatingRunId = TEXT("run_blocked_origin");
	OriginTask.OriginatingUserPrompt = TEXT("Increase projectile speed in the active gameplay task.");
	OriginTask.OriginatingTaskMode = TEXT("workspace_file_build");
	OriginTask.OriginatingRequestedToolFamily = TEXT("workspace_file_build");
	OriginTask.OriginatingPrimaryMutationToolFamily = TEXT("workspace_file_build");

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest PreparedRequest;
	FString TransitionNotice;
	FString Error;
	TestTrue(
		TEXT("detected blocker should arm one autonomous prepared request"),
		FUnrealClaudeRestartSurvivalManager::TryPrepareClosedEditorBuildBlockerAutoStart(
			EUnrealClaudeProviderBackend::CodexCli,
			FPaths::Combine(TestRoot, TEXT("Poligon1")),
			TEXT("thread_blocked"),
			Blocker,
			OriginTask,
			PreparedRequest,
			TransitionNotice,
			Error));
	TestTrue(TEXT("prepared request file should exist after auto-start preparation"), FUnrealClaudeRestartSurvivalManager::HasPreparedRestoreRequest());
	TestTrue(TEXT("prepared request auto-start should be armed"), FUnrealClaudeRestartSurvivalManager::HasPreparedRequestAutoStartArm());
	TestTrue(TEXT("prepared request should keep task-driven handoff"), PreparedRequest.bTaskDrivenHandoff);
	TestTrue(TEXT("prepared request should auto-start after the response"), PreparedRequest.bAutoStartAfterResponse);
	TestTrue(TEXT("prepared request should stay on the autonomous closed-editor lane"), PreparedRequest.bAutonomousClosedEditorEscalation);
	TestEqual(
		TEXT("transition notice should stay on the bounded file/build wording"),
		TransitionNotice,
		FUnrealClaudeRestartSurvivalManager::BuildPreparedRequestClosedEditorTransitionNotice(PreparedRequest));
	TestTrue(
		TEXT("transition notice should use outside-editor wording"),
		TransitionNotice.Contains(TEXT("Continuing outside the editor"), ESearchCase::CaseSensitive));

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest LoadedRequest;
	TestTrue(TEXT("prepared request should load after auto-start preparation"), FUnrealClaudeRestartSurvivalManager::LoadPreparedRestoreRequest(LoadedRequest, Error));
	TestTrue(
		TEXT("prepared request detail should record the blocker family"),
		LoadedRequest.Detail.Contains(TEXT("ubt_log_backup_access_denied"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("prepared request continuation prompt should mention closed-editor build/preflight completion"),
		LoadedRequest.PostReattachCompletionText.Contains(TEXT("build/preflight"), ESearchCase::IgnoreCase));
	TestEqual(TEXT("prepared request should preserve originating run id"), LoadedRequest.OriginTask.OriginatingRunId, OriginTask.OriginatingRunId);
	TestEqual(TEXT("prepared request should preserve originating user prompt"), LoadedRequest.OriginTask.OriginatingUserPrompt, OriginTask.OriginatingUserPrompt);
	TestEqual(TEXT("prepared request should preserve originating task mode"), LoadedRequest.OriginTask.OriginatingTaskMode, OriginTask.OriginatingTaskMode);
	TestTrue(
		TEXT("loaded prepared request should still resolve to a prompt carrying the original task intent"),
		FUnrealClaudeRestartSurvivalManager::ResolvePreparedRequestPostReattachCompletionText(LoadedRequest).Contains(
			OriginTask.OriginatingUserPrompt,
			ESearchCase::CaseSensitive));
	FUnrealClaudeRestartSurvivalManager::ClearPreparedRequestAutoStartArm();
	return true;
}

bool FRestartSurvival_ClosedEditorBuildBlockerAutoStartPreservesOriginalTaskIntent::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ClosedEditorBuildBlockerAutoStartPreservesOriginalTaskIntent"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeClosedEditorBuildBlocker Blocker;
	Blocker.bDetected = true;
	Blocker.Family = EUnrealClaudeClosedEditorBuildBlockerFamily::UhtGeneratedRenameLock;
	Blocker.FamilyLabel = UnrealClaudeClosedEditorBuildBlockerFamilyToString(Blocker.Family);
	Blocker.EscalationReason = TEXT("Generated-file rename lock requires Unreal to close before bounded build work can continue.");
	Blocker.MatchedEvidence = TEXT("Failed to rename exported file MyThing.generated.h.tmp");

	FUnrealClaudeRestartSurvivalOriginTaskContext OriginTask;
	OriginTask.OriginatingRunId = TEXT("run_mutating_origin");
	OriginTask.OriginatingUserPrompt = TEXT("Make the moving cubes fly faster toward enemies and keep the same gameplay behavior.");
	OriginTask.OriginatingTaskMode = TEXT("workspace_file_build");
	OriginTask.OriginatingRequestedToolFamily = TEXT("workspace_file_build");
	OriginTask.OriginatingPrimaryMutationToolFamily = TEXT("workspace_file_build");

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest PreparedRequest;
	FString TransitionNotice;
	FString Error;
	TestTrue(
		TEXT("mutating origin task should arm one autonomous prepared request"),
		FUnrealClaudeRestartSurvivalManager::TryPrepareClosedEditorBuildBlockerAutoStart(
			EUnrealClaudeProviderBackend::CodexCli,
			FPaths::Combine(TestRoot, TEXT("Poligon1")),
			TEXT("thread_mutating_origin"),
			Blocker,
			OriginTask,
			PreparedRequest,
			TransitionNotice,
			Error));

	const FString ResolvedPrompt = FUnrealClaudeRestartSurvivalManager::ResolvePreparedRequestPostReattachCompletionText(PreparedRequest);
	TestTrue(TEXT("resolved prompt should carry the exact original task prompt"), ResolvedPrompt.Contains(OriginTask.OriginatingUserPrompt, ESearchCase::CaseSensitive));
	TestTrue(TEXT("resolved prompt should keep the originating run id"), ResolvedPrompt.Contains(OriginTask.OriginatingRunId, ESearchCase::CaseSensitive));
	TestTrue(TEXT("resolved prompt should forbid generic re-analysis"), ResolvedPrompt.Contains(TEXT("Do not replace the original task with generic workspace inspection"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("resolved prompt should keep the task mode visible"), ResolvedPrompt.Contains(OriginTask.OriginatingTaskMode, ESearchCase::CaseSensitive));
	FUnrealClaudeRestartSurvivalManager::ClearPreparedRequestAutoStartArm();
	return true;
}

bool FRestartSurvival_ClosedEditorBuildBlockerAutoStartReadOnlyIntentStaysReadOnly::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ClosedEditorBuildBlockerAutoStartReadOnlyIntentStaysReadOnly"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeClosedEditorBuildBlocker Blocker;
	Blocker.bDetected = true;
	Blocker.Family = EUnrealClaudeClosedEditorBuildBlockerFamily::LiveCodingActive;
	Blocker.FamilyLabel = UnrealClaudeClosedEditorBuildBlockerFamilyToString(Blocker.Family);
	Blocker.EscalationReason = TEXT("Live Coding is active while the bounded compile check needs the editor closed.");
	Blocker.MatchedEvidence = TEXT("Unable to build while Live Coding is active.");

	FUnrealClaudeRestartSurvivalOriginTaskContext OriginTask;
	OriginTask.OriginatingRunId = TEXT("run_read_only_origin");
	OriginTask.OriginatingUserPrompt = TEXT("Verify whether the bounded compile check completed after relaunch.");
	OriginTask.OriginatingTaskMode = TEXT("read_only_analysis");
	OriginTask.OriginatingRequestedToolFamily = TEXT("workspace_inspection");

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest PreparedRequest;
	FString TransitionNotice;
	FString Error;
	TestTrue(
		TEXT("read-only origin task should still arm one autonomous prepared request"),
		FUnrealClaudeRestartSurvivalManager::TryPrepareClosedEditorBuildBlockerAutoStart(
			EUnrealClaudeProviderBackend::CodexCli,
			FPaths::Combine(TestRoot, TEXT("Poligon1")),
			TEXT("thread_read_only_origin"),
			Blocker,
			OriginTask,
			PreparedRequest,
			TransitionNotice,
			Error));

	const FString ResolvedPrompt = FUnrealClaudeRestartSurvivalManager::ResolvePreparedRequestPostReattachCompletionText(PreparedRequest);
	TestTrue(TEXT("resolved prompt should keep the exact read-only task prompt"), ResolvedPrompt.Contains(OriginTask.OriginatingUserPrompt, ESearchCase::CaseSensitive));
	TestTrue(TEXT("resolved prompt should explicitly keep the resumed task read-only"), ResolvedPrompt.Contains(TEXT("remain read-only"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("read-only prompt should not insist on mutation-only continuation"), ResolvedPrompt.Contains(TEXT("Do not replace the original task with generic workspace inspection"), ESearchCase::CaseSensitive));
	FUnrealClaudeRestartSurvivalManager::ClearPreparedRequestAutoStartArm();
	return true;
}

bool FRestartSurvival_ClosedEditorBuildBlockerAutoStartRejectsMissingOriginPrompt::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ClosedEditorBuildBlockerAutoStartRejectsMissingOriginPrompt"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeClosedEditorBuildBlocker Blocker;
	Blocker.bDetected = true;
	Blocker.Family = EUnrealClaudeClosedEditorBuildBlockerFamily::UhtGeneratedRenameLock;
	Blocker.FamilyLabel = UnrealClaudeClosedEditorBuildBlockerFamilyToString(Blocker.Family);
	Blocker.EscalationReason = TEXT("Generated-file rename lock requires Unreal to close before bounded build work can continue.");
	Blocker.MatchedEvidence = TEXT("Failed to rename exported file MyThing.generated.h.tmp");

	FUnrealClaudeRestartSurvivalOriginTaskContext OriginTask;
	OriginTask.OriginatingRunId = TEXT("run_missing_prompt");
	OriginTask.OriginatingTaskMode = TEXT("workspace_file_build");
	OriginTask.OriginatingRequestedToolFamily = TEXT("workspace_file_build");

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest PreparedRequest;
	FString TransitionNotice;
	FString Error;
	TestFalse(
		TEXT("missing original prompt must refuse restart-survival auto-start"),
		FUnrealClaudeRestartSurvivalManager::TryPrepareClosedEditorBuildBlockerAutoStart(
			EUnrealClaudeProviderBackend::CodexCli,
			FPaths::Combine(TestRoot, TEXT("Poligon1")),
			TEXT("thread_missing_prompt"),
			Blocker,
			OriginTask,
			PreparedRequest,
			TransitionNotice,
			Error));
	TestTrue(TEXT("refusal should explain prompt capture"), Error.Contains(TEXT("original user prompt"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("auto-start should not be armed without origin prompt"), FUnrealClaudeRestartSurvivalManager::HasPreparedRequestAutoStartArm());
	return true;
}

bool FRestartSurvival_ClosedEditorBuildBlockerAutoStartCarriesVisualProofRequirement::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ClosedEditorBuildBlockerAutoStartCarriesVisualProofRequirement"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeClosedEditorBuildBlocker Blocker;
	Blocker.bDetected = true;
	Blocker.Family = EUnrealClaudeClosedEditorBuildBlockerFamily::UhtGeneratedRenameLock;
	Blocker.FamilyLabel = UnrealClaudeClosedEditorBuildBlockerFamilyToString(Blocker.Family);
	Blocker.EscalationReason = TEXT("Generated-file rename lock requires Unreal to close before bounded build work can continue.");
	Blocker.MatchedEvidence = TEXT("Failed to rename exported file MyThing.generated.h.tmp");

	FUnrealClaudeRestartSurvivalOriginTaskContext OriginTask;
	OriginTask.OriginatingRunId = TEXT("run_visual_origin");
	OriginTask.OriginatingUserPrompt = TEXT("Create combat inventory visually как на картинке and prove it with screenshot.");
	OriginTask.OriginatingPromptHash = TEXT("hash_visual_origin");
	OriginTask.OriginatingTaskMode = TEXT("feature_slice");
	OriginTask.OriginatingRequestedToolFamily = TEXT("bounded_unreal_mutation");
	OriginTask.bOriginatingHasAttachments = true;
	OriginTask.bOriginatingHasVisualReference = true;
	OriginTask.bVisualProofRequired = true;
	OriginTask.bVisualQaManifestRequired = true;
	OriginTask.OriginatingAttachedImagePaths = { TEXT("D:/References/combat_inventory.png") };
	OriginTask.OriginatingAttachmentNames = { TEXT("combat_inventory.png") };

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest PreparedRequest;
	FString TransitionNotice;
	FString Error;
	TestTrue(
		TEXT("visual-reference origin task should arm with metadata"),
		FUnrealClaudeRestartSurvivalManager::TryPrepareClosedEditorBuildBlockerAutoStart(
			EUnrealClaudeProviderBackend::CodexCli,
			FPaths::Combine(TestRoot, TEXT("Poligon1")),
			TEXT("thread_visual_origin"),
			Blocker,
			OriginTask,
			PreparedRequest,
			TransitionNotice,
			Error));

	const FString ResolvedPrompt = FUnrealClaudeRestartSurvivalManager::ResolvePreparedRequestPostReattachCompletionText(PreparedRequest);
	TestEqual(TEXT("visual origin hash should be preserved"), PreparedRequest.OriginTask.OriginatingPromptHash, OriginTask.OriginatingPromptHash);
	TestTrue(TEXT("visual proof should remain required"), PreparedRequest.OriginTask.bVisualProofRequired);
	TestTrue(TEXT("visual QA manifest should remain required"), PreparedRequest.OriginTask.bVisualQaManifestRequired);
	TestTrue(TEXT("continuation should mention prompt hash"), ResolvedPrompt.Contains(OriginTask.OriginatingPromptHash, ESearchCase::CaseSensitive));
	TestTrue(TEXT("continuation should mention visual proof requirement"), ResolvedPrompt.Contains(TEXT("Visual proof requirement"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("continuation should mention visual QA manifest requirement"), ResolvedPrompt.Contains(TEXT("visual_qa_manifest"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("continuation should mention original image path"), ResolvedPrompt.Contains(TEXT("combat_inventory.png"), ESearchCase::CaseSensitive));
	FUnrealClaudeRestartSurvivalManager::ClearPreparedRequestAutoStartArm();
	return true;
}

bool FRestartSurvival_MarkReattachValidatedDoesNotClearPendingDispatch::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("MarkReattachValidatedDoesNotClearPendingDispatch"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("session_pending_dispatch");
	State.TaskId = TEXT("task_pending_dispatch");
	State.ProjectRoot = TEXT("D:/PendingDispatchProject");
	State.UProjectPath = TEXT("D:/PendingDispatchProject/PendingDispatchProject.uproject");
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::Reattached;
	State.bProviderThreadResumePending = true;
	State.bPostReattachCompletionPending = true;
	State.bPostReattachCompletionDispatched = false;
	State.PostReattachCompletionText = TEXT("Continue after reattach.");

	FString Error;
	TestTrue(TEXT("state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));
	TestFalse(
		TEXT("reattach validation must not clear resume before pending continuation dispatch"),
		FUnrealClaudeRestartSurvivalManager::MarkReattachValidated(Error));

	FUnrealClaudeRestartSurvivalState ReloadedState;
	TestTrue(TEXT("state should reload after blocked validation"), FUnrealClaudeRestartSurvivalManager::DescribeCurrentState(ReloadedState));
	TestTrue(TEXT("provider resume should remain pending"), ReloadedState.bProviderThreadResumePending);
	TestTrue(TEXT("post-reattach completion should remain pending"), ReloadedState.bPostReattachCompletionPending);
	TestFalse(TEXT("post-reattach completion should remain undispatched"), ReloadedState.bPostReattachCompletionDispatched);
	return true;
}

bool FRestartSurvival_OpenEditorBuildLockCloseSafetyDecisionTruth::RunTest(const FString& Parameters)
{
	FOpenEditorBuildLockCloseSafetyInputs Inputs;
	Inputs.DirtyPackageCount = 2;
	Inputs.DirtyPackageSummary = TEXT("/Game/UI/WBP_Inventory, /Game/Maps/L_Main");

	FOpenEditorBuildLockCloseSafetyDecision Decision =
		SClaudeEditorWidget::EvaluateOpenEditorBuildLockCloseSafety(Inputs);
	TestFalse(TEXT("unsaved packages should block auto-close"), Decision.bCanAutoClose);
	TestEqual(TEXT("unsaved packages should use the expected block code"), Decision.BlockReasonCode, FString(TEXT("unsaved_packages")));
	TestTrue(TEXT("unsaved package blocker should surface package names"), Decision.UserFacingMessage.Contains(TEXT("/Game/UI/WBP_Inventory"), ESearchCase::CaseSensitive));

	Inputs = FOpenEditorBuildLockCloseSafetyInputs();
	Inputs.bPlaySessionActive = true;
	Decision = SClaudeEditorWidget::EvaluateOpenEditorBuildLockCloseSafety(Inputs);
	TestFalse(TEXT("active PIE should block auto-close"), Decision.bCanAutoClose);
	TestEqual(TEXT("PIE should use the expected block code"), Decision.BlockReasonCode, FString(TEXT("play_session_active")));
	TestTrue(TEXT("PIE blocker should mention play session"), Decision.UserFacingMessage.Contains(TEXT("PIE"), ESearchCase::CaseSensitive));

	Inputs = FOpenEditorBuildLockCloseSafetyInputs();
	Inputs.bModalWindowActive = true;
	Inputs.ModalWindowTitle = TEXT("Save Content");
	Decision = SClaudeEditorWidget::EvaluateOpenEditorBuildLockCloseSafety(Inputs);
	TestFalse(TEXT("modal dialog should block auto-close"), Decision.bCanAutoClose);
	TestEqual(TEXT("modal dialog should use the expected block code"), Decision.BlockReasonCode, FString(TEXT("modal_dialog_active")));
	TestTrue(TEXT("modal blocker should mention dialog title"), Decision.UserFacingMessage.Contains(TEXT("Save Content"), ESearchCase::CaseSensitive));

	Inputs = FOpenEditorBuildLockCloseSafetyInputs();
	Inputs.bRestartSurvivalStateActive = true;
	Inputs.RestartSurvivalPhase = TEXT("DetachedRunning");
	Decision = SClaudeEditorWidget::EvaluateOpenEditorBuildLockCloseSafety(Inputs);
	TestFalse(TEXT("active restart-survival should block auto-close"), Decision.bCanAutoClose);
	TestEqual(TEXT("restart-survival blocker should use the expected code"), Decision.BlockReasonCode, FString(TEXT("restart_survival_state_active")));
	TestTrue(TEXT("restart-survival blocker should mention phase"), Decision.UserFacingMessage.Contains(TEXT("DetachedRunning"), ESearchCase::CaseSensitive));

	Inputs = FOpenEditorBuildLockCloseSafetyInputs();
	Decision = SClaudeEditorWidget::EvaluateOpenEditorBuildLockCloseSafety(Inputs);
	TestTrue(TEXT("clean editor state should allow auto-close"), Decision.bCanAutoClose);
	TestTrue(TEXT("clean editor state should not need a block code"), Decision.BlockReasonCode.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ReattachRequiresMatchingToken,
	"UnrealClaude.RestartSurvival.ReattachRequiresMatchingToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_FailedDetachedBuildRelaunchesEditor,
	"UnrealClaude.RestartSurvival.FailedDetachedBuildRelaunchesEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_RelayAgentSuccessRelaunchesEditorAndInjectsContinuation,
	"UnrealClaude.RestartSurvival.RelayAgentSuccessRelaunchesEditorAndInjectsContinuation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_RelayAgentAdoptsExternalTerminalResultAndRelaunchesEditor,
	"UnrealClaude.RestartSurvival.RelayAgentAdoptsExternalTerminalResultAndRelaunchesEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_ReattachRequiresMatchingToken::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ReattachRequiresMatchingToken"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("session_token");
	State.TaskId = TEXT("task_token");
	State.ProjectRoot = TEXT("D:/TokenProject");
	State.UProjectPath = TEXT("D:/TokenProject/TokenProject.uproject");
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::AwaitingReattach;
	State.ReattachToken = TEXT("token_expected");

	FString Error;
	TestTrue(TEXT("state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	FString Notice;
	FString ReattachError;
	TestFalse(
		TEXT("reattach should reject a mismatched token"),
		FUnrealClaudeRestartSurvivalManager::TryMarkReattached(
			EUnrealClaudeProviderBackend::CodexCli,
			TEXT("token_wrong"),
			Notice,
			ReattachError));

	FUnrealClaudeRestartSurvivalState StillAwaitingState;
	TestTrue(TEXT("state should still load after mismatched token"), FUnrealClaudeRestartSurvivalManager::DescribeCurrentState(StillAwaitingState));
	TestEqual(TEXT("mismatched token must not advance phase"), StillAwaitingState.Phase, EUnrealClaudeRestartSurvivalPhase::AwaitingReattach);

	Notice.Reset();
	ReattachError.Reset();
	TestTrue(
		TEXT("reattach should accept the matching token"),
		FUnrealClaudeRestartSurvivalManager::TryMarkReattached(
			EUnrealClaudeProviderBackend::CodexCli,
			TEXT("token_expected"),
			Notice,
			ReattachError));

	FUnrealClaudeRestartSurvivalState ReattachedState;
	TestTrue(TEXT("state should load after matching token"), FUnrealClaudeRestartSurvivalManager::DescribeCurrentState(ReattachedState));
	TestEqual(TEXT("matching token should advance phase"), ReattachedState.Phase, EUnrealClaudeRestartSurvivalPhase::Reattached);
	TestTrue(TEXT("matching token should preserve pending resume until validation"), ReattachedState.bProviderThreadResumePending);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PrematureReopenDoesNotMarkReattached,
	"UnrealClaude.RestartSurvival.PrematureReopenDoesNotMarkReattached",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ReattachedDispatchedStateCanBeReplacedForFreshStart,
	"UnrealClaude.RestartSurvival.ReattachedDispatchedStateCanBeReplacedForFreshStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_DetachedOwnerStateRoundTripPersistsObjective,
	"UnrealClaude.RestartSurvival.DetachedOwnerStateRoundTripPersistsObjective",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_PreparedBuildBlockerContinuationDoesNotRewrapExistingContinuationPrompt,
	"UnrealClaude.RestartSurvival.PreparedBuildBlockerContinuationDoesNotRewrapExistingContinuationPrompt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ManualReopenAwaitingReattachCanAdoptSameState,
	"UnrealClaude.RestartSurvival.ManualReopenAwaitingReattachCanAdoptSameState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ProjectEditorAlreadyReopenedBeforePreflightReturnsAwaitingReattach,
	"UnrealClaude.RestartSurvival.ProjectEditorAlreadyReopenedBeforePreflightReturnsAwaitingReattach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_RepeatedNormalizedBlockerSignatureReachesTruthfulTerminalFail,
	"UnrealClaude.RestartSurvival.RepeatedNormalizedBlockerSignatureReachesTruthfulTerminalFail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_SupervisorPreservesUtf8NoBomPayloadBytes,
	"UnrealClaude.RestartSurvival.SupervisorPreservesUtf8NoBomPayloadBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_PrematureReopenDoesNotMarkReattached::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PrematureReopenDoesNotMarkReattached"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("session_phase");
	State.TaskId = TEXT("task_phase");
	State.ProjectRoot = TEXT("D:/PhaseProject");
	State.UProjectPath = TEXT("D:/PhaseProject/PhaseProject.uproject");
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::Relaunching;
	State.ReattachToken = TEXT("token_phase");

	FString Error;
	TestTrue(TEXT("state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	FString Notice;
	FString ReattachError;
	TestFalse(
		TEXT("premature reopen must not mark reattached from Relaunching"),
		FUnrealClaudeRestartSurvivalManager::TryMarkReattached(
			EUnrealClaudeProviderBackend::CodexCli,
			TEXT("token_phase"),
			Notice,
			ReattachError));

	FUnrealClaudeRestartSurvivalState ReloadedState;
	TestTrue(TEXT("state should still load after premature reopen"), FUnrealClaudeRestartSurvivalManager::DescribeCurrentState(ReloadedState));
	TestEqual(TEXT("premature reopen must keep phase unchanged"), ReloadedState.Phase, EUnrealClaudeRestartSurvivalPhase::Relaunching);
	TestTrue(TEXT("premature reopen must keep pending resume unchanged"), ReloadedState.bProviderThreadResumePending);
	return true;
}

bool FRestartSurvival_ReattachedDispatchedStateCanBeReplacedForFreshStart::RunTest(const FString& Parameters)
{
	FUnrealClaudeRestartSurvivalState ReattachedPendingState;
	ReattachedPendingState.Phase = EUnrealClaudeRestartSurvivalPhase::Reattached;
	ReattachedPendingState.bProviderThreadResumePending = true;
	ReattachedPendingState.bPostReattachCompletionDispatched = false;
	TestFalse(
		TEXT("replaced start must stay blocked while reattached continuation is still pending and undispatched"),
		FUnrealClaudeRestartSurvivalManager::CanReplaceExistingStateForFreshStart(ReattachedPendingState));

	ReattachedPendingState.bPostReattachCompletionDispatched = true;
	TestTrue(
		TEXT("replaced start should be allowed once the reattached continuation was already dispatched"),
		FUnrealClaudeRestartSurvivalManager::CanReplaceExistingStateForFreshStart(ReattachedPendingState));

	ReattachedPendingState.bProviderThreadResumePending = false;
	TestTrue(
		TEXT("replaced start should still be allowed after the reattached continuation fully clears"),
		FUnrealClaudeRestartSurvivalManager::CanReplaceExistingStateForFreshStart(ReattachedPendingState));

	FUnrealClaudeRestartSurvivalState FailedState;
	FailedState.Phase = EUnrealClaudeRestartSurvivalPhase::FailedTerminal;
	TestTrue(
		TEXT("failed terminal state should remain replaceable"),
		FUnrealClaudeRestartSurvivalManager::CanReplaceExistingStateForFreshStart(FailedState));

	return true;
}

bool FRestartSurvival_DetachedOwnerStateRoundTripPersistsObjective::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("DetachedOwnerStateRoundTripPersistsObjective"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("session_owner_roundtrip");
	State.TaskId = TEXT("task_owner_roundtrip");
	State.ProjectRoot = TEXT("D:/DetachedOwnerProject");
	State.UProjectPath = TEXT("D:/DetachedOwnerProject/DetachedOwnerProject.uproject");
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::DetachedRunning;
	State.bProviderThreadResumePending = true;
	State.DetachedObjective = TEXT("closed_editor_task_owner_continuity_v2");
	State.DetachedObjectiveDetail = TEXT("Bounded local-only detached owner loop.");
	State.DetachedStepIndex = 2;
	State.DetachedStepBudget = 3;
	State.DetachedCurrentStep = TEXT("build");
	State.DetachedPendingStep = TEXT("mitigate_uht_generated_rename_lock");
	State.DetachedLastStepOutcome = TEXT("mitigation_retry_pending");
	State.DetachedLastBlockerFamily = TEXT("uht_generated_rename_lock");
	State.DetachedLastBlockerSignature = TEXT("uht_generated_rename_lock|failed to rename exported file");
	State.DetachedTerminalOutcome = TEXT("none");
	State.bDetachedFileWriteCompleted = true;
	State.bDetachedRestoreCompleted = false;
	State.bDetachedBuildCompleted = false;
	State.DetachedOwnerProcessId = 4242;
	State.bDetachedOwnerActive = true;
	State.bDetachedOwnerManualReopenDetected = true;

	FString Error;
	TestTrue(TEXT("state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	FUnrealClaudeRestartSurvivalState ReloadedState;
	TestTrue(TEXT("state should load"), FUnrealClaudeRestartSurvivalManager::LoadState(ReloadedState, Error));
	TestEqual(TEXT("detached objective should persist"), ReloadedState.DetachedObjective, State.DetachedObjective);
	TestEqual(TEXT("detached objective detail should persist"), ReloadedState.DetachedObjectiveDetail, State.DetachedObjectiveDetail);
	TestEqual(TEXT("detached step index should persist"), ReloadedState.DetachedStepIndex, State.DetachedStepIndex);
	TestEqual(TEXT("detached step budget should persist"), ReloadedState.DetachedStepBudget, State.DetachedStepBudget);
	TestEqual(TEXT("detached current step should persist"), ReloadedState.DetachedCurrentStep, State.DetachedCurrentStep);
	TestEqual(TEXT("detached pending step should persist"), ReloadedState.DetachedPendingStep, State.DetachedPendingStep);
	TestEqual(TEXT("detached blocker signature should persist"), ReloadedState.DetachedLastBlockerSignature, State.DetachedLastBlockerSignature);
	TestTrue(TEXT("detached owner active flag should persist"), ReloadedState.bDetachedOwnerActive);
	TestTrue(TEXT("detached manual reopen flag should persist"), ReloadedState.bDetachedOwnerManualReopenDetected);
	return true;
}

bool FRestartSurvival_PreparedBuildBlockerContinuationDoesNotRewrapExistingContinuationPrompt::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PreparedBuildBlockerContinuationDoesNotRewrapExistingContinuationPrompt"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(TestRoot, TEXT("RelayContinuationProject")));
	IFileManager::Get().MakeDirectory(*ProjectRoot, true);

	FUnrealClaudeClosedEditorBuildBlocker Blocker;
	Blocker.bDetected = true;
	Blocker.Family = EUnrealClaudeClosedEditorBuildBlockerFamily::UhtGeneratedRenameLock;
	Blocker.FamilyLabel = TEXT("uht_generated_rename_lock");
	Blocker.EscalationReason = TEXT("UHT/generated-file rename lock shows that editor-open generated outputs cannot be replaced while Unreal is still running.");
	Blocker.MatchedEvidence = TEXT("Failed to rename exported file");

	FUnrealClaudeRestartSurvivalOriginTaskContext OriginTask;
	OriginTask.OriginatingRunId = TEXT("run_original");
	OriginTask.OriginatingUserPrompt = TEXT("Continue the same ordinary bounded project-local task after restart-survival reattach.\nPreviously injected continuation text should not be rewrapped.");
	OriginTask.OriginatingTaskMode = TEXT("workspace_file_build");
	OriginTask.OriginatingRequestedToolFamily = TEXT("workspace_file_build");
	OriginTask.OriginatingPrimaryMutationToolFamily = TEXT("workspace_file_build");

	FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request;
	FString TransitionNotice;
	FString Error;
	TestTrue(
		TEXT("build-blocker auto-start should prepare a request"),
		FUnrealClaudeRestartSurvivalManager::TryPrepareClosedEditorBuildBlockerAutoStart(
			EUnrealClaudeProviderBackend::CodexCli,
			ProjectRoot,
			TEXT("provider_session_original"),
			Blocker,
			OriginTask,
			Request,
			TransitionNotice,
			Error));

	TestFalse(
		TEXT("prepared continuation should not embed a nested original prompt wrapper when the source prompt is already a continuation"),
		Request.ContinuationIntentPrompt.Contains(TEXT("Original task prompt follows exactly:"), ESearchCase::CaseSensitive));
	TestTrue(
		TEXT("prepared continuation should explain that the original prompt is already preserved"),
		Request.ContinuationIntentPrompt.Contains(TEXT("do not recurse another continuation wrapper"), ESearchCase::IgnoreCase));
	return true;
}

bool FRestartSurvival_ManualReopenAwaitingReattachCanAdoptSameState::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ManualReopenAwaitingReattachCanAdoptSameState"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("session_manual_reopen");
	State.TaskId = TEXT("task_manual_reopen");
	State.ProjectRoot = TEXT("D:/ManualReopenProject");
	State.UProjectPath = TEXT("D:/ManualReopenProject/ManualReopenProject.uproject");
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::AwaitingReattach;
	State.bProviderThreadResumePending = true;
	State.ReattachNotice = TEXT("Manual reopen reattached.");
	State.bDetachedOwnerManualReopenDetected = true;
	State.bDetachedOwnerActive = false;
	State.DetachedOwnerProcessId = 0;

	FString Error;
	TestTrue(TEXT("state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	FString Notice;
	TestTrue(
		TEXT("manual reopen should be able to adopt the same awaiting reattach state"),
		FUnrealClaudeRestartSurvivalManager::TryMarkReattachedFromManualReopen(
			EUnrealClaudeProviderBackend::CodexCli,
			Notice,
			Error));
	TestEqual(TEXT("manual reopen should surface the stored reattach notice"), Notice, State.ReattachNotice);

	FUnrealClaudeRestartSurvivalState ReloadedState;
	TestTrue(TEXT("state should reload after manual reopen adoption"), FUnrealClaudeRestartSurvivalManager::LoadState(ReloadedState, Error));
	TestEqual(TEXT("manual reopen should advance phase to reattached"), ReloadedState.Phase, EUnrealClaudeRestartSurvivalPhase::Reattached);
	TestFalse(TEXT("manual reopen should clear detached owner active flag"), ReloadedState.bDetachedOwnerActive);
	return true;
}

bool FRestartSurvival_ProjectEditorAlreadyReopenedBeforePreflightReturnsAwaitingReattach::RunTest(const FString& Parameters)
{
#if !PLATFORM_WINDOWS
	AddInfo(TEXT("Detached preflight/manual-reopen coverage is Windows-only."));
	return true;
#else
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ProjectEditorAlreadyReopenedBeforePreflightReturnsAwaitingReattach"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(TestRoot, TEXT("Poligon1")));
	const FString UProjectPath = FPaths::Combine(ProjectRoot, TEXT("Poligon1.uproject"));
	const FString SavedRoot = FPaths::Combine(ProjectRoot, TEXT("Saved"));
	const FString BuildRoot = FPaths::Combine(SavedRoot, TEXT("UnrealBuildTool"));
	const FString BuildLogPath = FPaths::Combine(BuildRoot, TEXT("Poligon1Editor-current-build.log"));
	const FString FakeEditorPath = FPaths::Combine(ProjectRoot, TEXT("FakeEditorManualReopen.cmd"));
	const FString FakePreflightPath = FPaths::Combine(ProjectRoot, TEXT("Launch-OsvayderPlugin-WithPreflight.ps1"));

	TestTrue(TEXT("fake project descriptor should be written"), SaveUtf8TextFile(UProjectPath, TEXT("{}")));

	const FString FakeEditorScript = TEXT("@echo off\r\nexit /b 0\r\n");
	TestTrue(TEXT("fake editor script should be written"), SaveUtf8TextFile(FakeEditorPath, FakeEditorScript));

	FString PreflightProjectPath = UProjectPath;
	PreflightProjectPath.ReplaceInline(TEXT("/"), TEXT("\\"));
	const FString FakePreflightScript = FString::Printf(
		TEXT("$ErrorActionPreference = 'Stop'\r\n")
		TEXT("Write-Output 'Project editor is still running for %s (pid(s): 1234). Close that editor before running the preflight rebuild launcher.'\r\n")
		TEXT("exit 1\r\n"),
		*PreflightProjectPath.ReplaceCharWithEscapedChar());
	TestTrue(TEXT("fake preflight script should be written"), SaveUtf8TextFile(FakePreflightPath, FakePreflightScript));

	uint32 EditorProcessId = 0;
	const FString SleepCommand = TEXT("-NoProfile -Command \"Start-Sleep -Seconds 1\"");
	FProcHandle FakeRunningEditor = FPlatformProcess::CreateProc(
		TEXT("powershell.exe"),
		*SleepCommand,
		true,
		false,
		false,
		&EditorProcessId,
		0,
		nullptr,
		nullptr);
	TestTrue(TEXT("fake running editor process should start"), FakeRunningEditor.IsValid());
	if (!FakeRunningEditor.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("fake running editor should exit before detached supervisor takes over"), WaitForProcessExit(FakeRunningEditor, 10.0));
	FPlatformProcess::CloseProc(FakeRunningEditor);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("restart_survival_manual_reopen_preflight");
	State.TaskId = TEXT("task_manual_reopen_preflight");
	State.ProjectRoot = ProjectRoot;
	State.UProjectPath = UProjectPath;
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.BackendDisplayName = TEXT("Codex CLI [default]");
	State.ExecutionControlProfileId = TEXT("workspace_write_project");
	State.ExecutionTransportLabel = TEXT("persistent_app_server");
	State.ProviderSessionId = TEXT("provider_manual_reopen_preflight");
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::Detaching;
	State.PhaseDetail = TEXT("Detaching into fake manual-reopen preflight coverage.");
	State.ReattachToken = TEXT("reattach_manual_reopen_preflight");
	State.ReattachNotice = TEXT("Restart-survival reattached after manual reopen preflight return.");
	State.EditorProcessId = static_cast<int32>(EditorProcessId);
	State.EditorExecutablePath = FakeEditorPath;
	State.PreflightLauncherPath = FakePreflightPath;
	State.BuildTarget = TEXT("Poligon1Editor");
	State.CreatedAtUtc = TEXT("2026-04-16T00:00:00Z");
	State.PostReattachCompletionText = TEXT("Base continuation prompt.");
	State.bPostReattachCompletionPending = true;

	FString Error;
	TestTrue(TEXT("restart-survival state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	const FString SupervisorScriptPath = FUnrealClaudeRestartSurvivalManager::GetSupervisorScriptPath();
	const FString StatePath = FUnrealClaudeRestartSurvivalManager::GetStatePath();
	const FString SupervisorParams = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -StatePath \"%s\" -DisableStatusMonitor"),
		*SupervisorScriptPath,
		*StatePath);

	int32 SupervisorReturnCode = INDEX_NONE;
	FString SupervisorStdOut;
	FString SupervisorStdErr;
	const bool bSupervisorExecuted = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*SupervisorParams,
		&SupervisorReturnCode,
		&SupervisorStdOut,
		&SupervisorStdErr);
	TestTrue(TEXT("restart-survival supervisor should execute"), bSupervisorExecuted);
	TestEqual(TEXT("restart-survival supervisor should exit cleanly"), SupervisorReturnCode, 0);

	FUnrealClaudeRestartSurvivalState UpdatedState;
	TestTrue(TEXT("updated state should load"), FUnrealClaudeRestartSurvivalManager::LoadState(UpdatedState, Error));

	const FString NormalizedContinuationText = UpdatedState.PostReattachCompletionText.Replace(TEXT("\\"), TEXT("/"));
	const FString NormalizedBuildLogPath = BuildLogPath.Replace(TEXT("\\"), TEXT("/"));

	TestEqual(TEXT("manual reopen preflight should return to awaiting reattach"), UpdatedState.Phase, EUnrealClaudeRestartSurvivalPhase::AwaitingReattach);
	TestTrue(TEXT("provider-thread resume should remain pending"), UpdatedState.bProviderThreadResumePending);
	TestTrue(TEXT("manual reopen flag should be raised"), UpdatedState.bDetachedOwnerManualReopenDetected);
	TestFalse(TEXT("detached owner should no longer stay active"), UpdatedState.bDetachedOwnerActive);
	TestEqual(TEXT("terminal outcome should be manual_reopen"), UpdatedState.DetachedTerminalOutcome, FString(TEXT("manual_reopen")));
	TestTrue(TEXT("continuation text should explain the reopened editor return"),
		UpdatedState.PostReattachCompletionText.Contains(TEXT("same project editor was already reopened"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("continuation text should include the detached build log anchor"),
		NormalizedContinuationText.Contains(NormalizedBuildLogPath, ESearchCase::IgnoreCase));
	TestTrue(TEXT("detached current-build log should exist after the preflight manual-reopen return"),
		IFileManager::Get().FileExists(*BuildLogPath));

	return true;
#endif
}

bool FRestartSurvival_RepeatedNormalizedBlockerSignatureReachesTruthfulTerminalFail::RunTest(const FString& Parameters)
{
#if !PLATFORM_WINDOWS
	AddInfo(TEXT("Detached repeated-blocker supervisor coverage is Windows-only."));
	return true;
#else
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("RepeatedNormalizedBlockerSignatureReachesTruthfulTerminalFail"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(TestRoot, TEXT("Poligon1")));
	const FString UProjectPath = FPaths::Combine(ProjectRoot, TEXT("Poligon1.uproject"));
	const FString SavedRoot = FPaths::Combine(ProjectRoot, TEXT("Saved"));
	const FString BuildRoot = FPaths::Combine(SavedRoot, TEXT("UnrealBuildTool"));
	const FString BuildLogPath = FPaths::Combine(BuildRoot, TEXT("Poligon1Editor-current-build.log"));
	const FString RelaunchMarkerPath = FPaths::Combine(SavedRoot, TEXT("UnrealClaude"), TEXT("Automation"), TEXT("repeated_blocker_relaunch_marker.txt"));
	const FString FakeEditorPath = FPaths::Combine(ProjectRoot, TEXT("FakeEditorRepeatedBlocker.cmd"));
	const FString FakeBuildPath = FPaths::Combine(ProjectRoot, TEXT("Build.bat"));

	TestTrue(TEXT("fake project descriptor should be written"), SaveUtf8TextFile(UProjectPath, TEXT("{}")));

	auto ToBatchPath = [](const FString& InPath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		Path.ReplaceInline(TEXT("/"), TEXT("\\"));
		return Path;
	};

	const FString RelaunchMarkerBatch = ToBatchPath(RelaunchMarkerPath);
	const FString BuildRootBatch = ToBatchPath(BuildRoot);
	const FString BuildLogBatch = ToBatchPath(BuildLogPath);

	TestTrue(
		TEXT("fake editor relaunch script should be written"),
		SaveUtf8TextFile(
			FakeEditorPath,
			FString::Printf(
				TEXT("@echo off\r\nif not exist \"%s\" mkdir \"%s\"\r\n> \"%s\" echo relaunched_after_terminal_fail\r\nexit /b 0\r\n"),
				*ToBatchPath(FPaths::GetPath(RelaunchMarkerPath)),
				*ToBatchPath(FPaths::GetPath(RelaunchMarkerPath)),
				*RelaunchMarkerBatch)));

	TestTrue(
		TEXT("fake build script should be written"),
		SaveUtf8TextFile(
			FakeBuildPath,
			FString::Printf(
				TEXT("@echo off\r\n")
				TEXT("if not exist \"%s\" mkdir \"%s\"\r\n")
				TEXT("> \"%s\" echo Result: Failed (OtherCompilationError)\r\n")
				TEXT(">> \"%s\" echo Failed to rename exported file: 'D:\\Fake\\Intermediate\\Build\\Win64\\UnrealEditor\\Inc\\Poligon1\\UHT\\CombatCharacter.generated.h.tmp'\r\n")
				TEXT("exit /b 1\r\n"),
				*BuildRootBatch,
				*BuildRootBatch,
				*BuildLogBatch,
				*BuildLogBatch)));

	uint32 EditorProcessId = 0;
	FProcHandle FakeRunningEditor = FPlatformProcess::CreateProc(
		TEXT("powershell.exe"),
		TEXT("-NoProfile -Command \"Start-Sleep -Seconds 1\""),
		true,
		false,
		false,
		&EditorProcessId,
		0,
		nullptr,
		nullptr);
	TestTrue(TEXT("fake running editor should start"), FakeRunningEditor.IsValid());
	TestTrue(TEXT("fake running editor should exit"), WaitForProcessExit(FakeRunningEditor, 10.0));
	FPlatformProcess::CloseProc(FakeRunningEditor);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("restart_survival_repeated_signature");
	State.TaskId = TEXT("task_repeated_signature");
	State.ProjectRoot = ProjectRoot;
	State.UProjectPath = UProjectPath;
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.BackendDisplayName = TEXT("Codex CLI [default]");
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::Detaching;
	State.EditorProcessId = static_cast<int32>(EditorProcessId);
	State.EditorExecutablePath = FakeEditorPath;
	State.BuildBatchPath = FakeBuildPath;
	State.BuildTarget = TEXT("Poligon1Editor");
	State.PostReattachCompletionText = TEXT("Original continuation prompt.");
	State.bPostReattachCompletionPending = true;
	State.DetachedStepBudget = 3;
	State.Proof.bEnabled = true;
	State.Proof.BuildLogPath = BuildLogPath;

	FString Error;
	TestTrue(TEXT("state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	const FString SupervisorScriptPath = FUnrealClaudeRestartSurvivalManager::GetSupervisorScriptPath();
	const FString StatePath = FUnrealClaudeRestartSurvivalManager::GetStatePath();
	const FString SupervisorParams = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -StatePath \"%s\" -DisableStatusMonitor"),
		*SupervisorScriptPath,
		*StatePath);

	int32 SupervisorReturnCode = INDEX_NONE;
	TestTrue(
		TEXT("restart-survival supervisor should execute"),
		FPlatformProcess::ExecProcess(
			TEXT("powershell.exe"),
			*SupervisorParams,
			&SupervisorReturnCode,
			nullptr,
			nullptr));
	TestEqual(TEXT("restart-survival supervisor should exit cleanly"), SupervisorReturnCode, 0);

	FUnrealClaudeRestartSurvivalState UpdatedState;
	TestTrue(TEXT("updated state should load"), FUnrealClaudeRestartSurvivalManager::LoadState(UpdatedState, Error));
	TestEqual(TEXT("repeated signature should reach truthful terminal fail"), UpdatedState.Phase, EUnrealClaudeRestartSurvivalPhase::AwaitingReattach);
	TestEqual(TEXT("repeated signature should persist one bounded mitigation retry within budget"), UpdatedState.DetachedStepIndex, 3);
	TestEqual(TEXT("terminal outcome should be failed"), UpdatedState.DetachedTerminalOutcome, FString(TEXT("failed")));
	TestTrue(TEXT("failure reason should mention repeated normalized blocker"), UpdatedState.FailureReason.Contains(TEXT("same normalized blocker signature repeated"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("fake editor should still relaunch after terminal fail"), WaitForFileToExist(RelaunchMarkerPath, 5.0));
	return true;
#endif
}

bool FRestartSurvival_SupervisorPreservesUtf8NoBomPayloadBytes::RunTest(const FString& Parameters)
{
#if !PLATFORM_WINDOWS
	AddInfo(TEXT("UTF-8 no-BOM payload supervisor coverage is Windows-only."));
	return true;
#else
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("SupervisorPreservesUtf8NoBomPayloadBytes"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(TestRoot, TEXT("Utf8Project")));
	const FString UProjectPath = FPaths::Combine(ProjectRoot, TEXT("Utf8Project.uproject"));
	const FString SavedRoot = FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"));
	const FString ProofDir = FPaths::Combine(SavedRoot, TEXT("Automation"));
	const FString SourcePath = FPaths::Combine(SavedRoot, TEXT("UnicodePayloadSource.h"));
	const FString TargetPath = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("Utf8Project"), TEXT("UnicodePayloadTarget.h"));
	const FString BackupPath = FPaths::Combine(SavedRoot, TEXT("UnicodePayloadTarget.backup.h"));
	const FString ProofTargetPath = FPaths::Combine(ProofDir, TEXT("UnicodeProofTarget.h"));
	const FString RelaunchMarkerPath = FPaths::Combine(ProofDir, TEXT("utf8_payload_relaunch_marker.txt"));
	const FString FakeEditorPath = FPaths::Combine(ProjectRoot, TEXT("FakeEditorUtf8Payload.cmd"));

	TestTrue(TEXT("fake project descriptor should be written"), SaveUtf8TextFile(UProjectPath, TEXT("{}")));

	const FString HeaderPayload =
		TEXT("// \u2500\u2500 Test \u2500\u2500\r\n")
		TEXT("// \u041F\u0440\u0438\u0432\u0435\u0442 \u0438\u0437 restart_survival \u2014 UTF-8 payload must stay exact\r\n")
		TEXT("UCLASS()\r\n")
		TEXT("class UUnicodePayloadHeader : public UObject\r\n")
		TEXT("{\r\n")
		TEXT("\tGENERATED_BODY()\r\n")
		TEXT("};\r\n");
	const FString OriginalTargetPayload = TEXT("// original target payload before detached overwrite\r\n");

	TestTrue(TEXT("unicode payload source should be written"), SaveUtf8TextFile(SourcePath, HeaderPayload));
	TestTrue(TEXT("original target payload should be written"), SaveUtf8TextFile(TargetPath, OriginalTargetPayload));

	auto ToBatchPath = [](const FString& InPath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		Path.ReplaceInline(TEXT("/"), TEXT("\\"));
		return Path;
	};

	const FString MarkerDirBatch = ToBatchPath(FPaths::GetPath(RelaunchMarkerPath));
	const FString RelaunchMarkerBatch = ToBatchPath(RelaunchMarkerPath);
	TestTrue(
		TEXT("fake editor relaunch script should be written"),
		SaveUtf8TextFile(
			FakeEditorPath,
			FString::Printf(
				TEXT("@echo off\r\n")
				TEXT("if not exist \"%s\" mkdir \"%s\"\r\n")
				TEXT("> \"%s\" echo relaunched_after_utf8_payload_proof\r\n")
				TEXT("exit /b 0\r\n"),
				*MarkerDirBatch,
				*MarkerDirBatch,
				*RelaunchMarkerBatch)));

	TArray<uint8> SourceBytes;
	TArray<uint8> OriginalTargetBytes;
	TestTrue(TEXT("source bytes should load"), LoadBinaryFile(SourcePath, SourceBytes));
	TestTrue(TEXT("original target bytes should load"), LoadBinaryFile(TargetPath, OriginalTargetBytes));
	TestTrue(TEXT("source payload should not be empty"), SourceBytes.Num() > 0);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("restart_survival_utf8_payload");
	State.TaskId = TEXT("task_utf8_payload");
	State.ProjectRoot = ProjectRoot;
	State.UProjectPath = UProjectPath;
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.BackendDisplayName = TEXT("Codex CLI [default]");
	State.ExecutionControlProfileId = TEXT("workspace_write_project");
	State.ExecutionTransportLabel = TEXT("persistent_app_server");
	State.ProviderSessionId = TEXT("provider_utf8_payload");
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::Detaching;
	State.PhaseDetail = TEXT("Detaching into UTF-8 payload preservation coverage.");
	State.ReattachToken = TEXT("reattach_utf8_payload");
	State.ReattachNotice = TEXT("Restart-survival reattached after UTF-8 payload preservation proof.");
	State.EditorExecutablePath = FakeEditorPath;
	State.CreatedAtUtc = TEXT("2026-04-22T00:00:00Z");
	State.PostReattachCompletionText = TEXT("Base continuation prompt.");
	State.bPostReattachCompletionPending = true;
	State.FileWriteIntent.bEnabled = true;
	State.FileWriteIntent.SourcePath = SourcePath;
	State.FileWriteIntent.TargetPath = TargetPath;
	State.FileWriteIntent.BackupPath = BackupPath;
	State.FileWriteIntent.Detail = TEXT("exact_project_local_file_write");
	State.Proof.bEnabled = true;
	State.Proof.DetachedFileTargetPath = ProofTargetPath;
	State.Proof.DetachedFileExpectedText = HeaderPayload;
	State.DetachedStepBudget = 3;

	FString Error;
	TestTrue(TEXT("restart-survival state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("utf8 state save detail: %s"), *Error));
	}

	const FString SupervisorScriptPath = FUnrealClaudeRestartSurvivalManager::GetSupervisorScriptPath();
	const FString StatePath = FUnrealClaudeRestartSurvivalManager::GetStatePath();
	const FString SupervisorParams = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -StatePath \"%s\" -DisableStatusMonitor"),
		*SupervisorScriptPath,
		*StatePath);

	int32 SupervisorReturnCode = INDEX_NONE;
	FString SupervisorStdOut;
	FString SupervisorStdErr;
	const bool bSupervisorExecuted = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*SupervisorParams,
		&SupervisorReturnCode,
		&SupervisorStdOut,
		&SupervisorStdErr);
	TestTrue(TEXT("restart-survival supervisor should execute"), bSupervisorExecuted);
	TestEqual(TEXT("restart-survival supervisor should exit cleanly"), SupervisorReturnCode, 0);
	if (!SupervisorStdOut.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("utf8 supervisor stdout: %s"), *SupervisorStdOut));
	}
	if (!SupervisorStdErr.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("utf8 supervisor stderr: %s"), *SupervisorStdErr));
	}

	FUnrealClaudeRestartSurvivalState UpdatedState;
	TestTrue(TEXT("updated UTF-8 state should load"), FUnrealClaudeRestartSurvivalManager::LoadState(UpdatedState, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("utf8 load-state detail: %s"), *Error));
	}

	TestEqual(TEXT("UTF-8 payload run should return to awaiting reattach"),
		UpdatedState.Phase,
		EUnrealClaudeRestartSurvivalPhase::AwaitingReattach);
	TestEqual(TEXT("UTF-8 payload run should finish successfully"),
		UpdatedState.DetachedTerminalOutcome,
		FString(TEXT("success")));
	TestEqual(TEXT("UTF-8 payload run should consume exactly the proof + file_write detached steps"),
		UpdatedState.DetachedStepIndex,
		2);
	TestTrue(TEXT("file-write completion flag should be set"), UpdatedState.bDetachedFileWriteCompleted);
	TestTrue(TEXT("proof detached file-write completion flag should be set"), UpdatedState.Proof.bDetachedFileWriteCompleted);

	TArray<uint8> TargetBytes;
	TArray<uint8> BackupBytes;
	TArray<uint8> ProofBytes;
	TestTrue(TEXT("written target bytes should load"), LoadBinaryFile(TargetPath, TargetBytes));
	TestTrue(TEXT("backup bytes should load"), LoadBinaryFile(BackupPath, BackupBytes));
	TestTrue(TEXT("proof target bytes should load"), LoadBinaryFile(ProofTargetPath, ProofBytes));

	TestTrue(TEXT("file-write target bytes should match the UTF-8 source exactly"), ByteArraysEqual(TargetBytes, SourceBytes));
	TestTrue(TEXT("proof target bytes should match the UTF-8 source exactly"), ByteArraysEqual(ProofBytes, SourceBytes));
	TestTrue(TEXT("backup bytes should preserve the pre-overwrite target exactly"), ByteArraysEqual(BackupBytes, OriginalTargetBytes));
	TestTrue(TEXT("fake editor should relaunch after UTF-8 payload success"), WaitForFileToExist(RelaunchMarkerPath, 5.0));
	return true;
#endif
}

bool FRestartSurvival_PluginSettingsReadbackTruth::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("PluginSettingsReadbackTruth"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);
	FUnrealClaudeRestartSurvivalSupportBundle SupportBundle;
	FString SupportBundleError;
	TestTrue(TEXT("support bundle should resolve for readback truth"), FUnrealClaudeRestartSurvivalManager::TryResolveSupportBundle(SupportBundle, SupportBundleError));

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("session_readback");
	State.TaskId = TEXT("task_readback");
	State.ProjectRoot = TEXT("D:/ReadbackProject");
	State.UProjectPath = TEXT("D:/ReadbackProject/ReadbackProject.uproject");
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.BackendDisplayName = TEXT("Codex CLI");
	State.ExecutionControlProfileId = TEXT("workspace_write_default_runtime_v1");
	State.ExecutionTransportLabel = TEXT("persistent_app_server");
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::AwaitingReattach;
	State.ProviderSessionId = TEXT("thread_readback");
	State.ProviderThreadStatePath = TEXT("D:/ReadbackProject/Saved/UnrealClaude/codex_persistent_thread.json");
	State.RestoreIntent.bEnabled = true;
	State.RestoreIntent.TargetPath = TEXT("D:/ReadbackProject/Content/Proof.txt");
	State.SupportBundleResolution = SupportBundle.ResolutionLabel;
	State.SupportBundleRoot = SupportBundle.BundleRoot;
	State.SupervisorScriptPath = SupportBundle.SupervisorScriptPath;
	State.MonitorScriptPath = SupportBundle.MonitorScriptPath;
	State.PreflightLauncherPath = SupportBundle.PreflightScriptPath;
	State.Proof.bEnabled = true;
	State.Proof.RunTag = TEXT("readback_truth");

	FString Error;
	TestTrue(TEXT("state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> BackendObject = GetObjectFieldOrNull(Result.Data, TEXT("assistant_backend"));
	const TSharedPtr<FJsonObject> RestartObject = GetObjectFieldOrNull(BackendObject, TEXT("restart_survival"));
	TestTrue(TEXT("assistant_backend should exist"), BackendObject.IsValid());
	TestTrue(TEXT("restart_survival should exist"), RestartObject.IsValid());
	if (!BackendObject.IsValid() || !RestartObject.IsValid())
	{
		return false;
	}

	FString Phase;
	FString TaskId;
	FString ProviderSessionId;
	FString StatePath;
	FString SupportBundleResolution;
	FString SupportBundleRoot;
	FString SupportBundleSupervisorPath;
	FString SupportBundlePreflightPath;
	FString StateSupportBundleResolution;
	FString StatePreflightPath;
	bool bStatePresent = false;
	bool bResumePending = false;
	bool bSupportBundleResolved = false;
	TestTrue(TEXT("restart_survival.state_present should exist"), RestartObject->TryGetBoolField(TEXT("state_present"), bStatePresent));
	TestTrue(TEXT("restart_survival.provider_thread_resume_pending should exist"), RestartObject->TryGetBoolField(TEXT("provider_thread_resume_pending"), bResumePending));
	TestTrue(TEXT("restart_survival.phase should exist"), RestartObject->TryGetStringField(TEXT("phase"), Phase));
	TestTrue(TEXT("restart_survival.task_id should exist"), RestartObject->TryGetStringField(TEXT("task_id"), TaskId));
	TestTrue(TEXT("restart_survival.provider_session_id should exist"), RestartObject->TryGetStringField(TEXT("provider_session_id"), ProviderSessionId));
	TestTrue(TEXT("restart_survival.state_path should exist"), RestartObject->TryGetStringField(TEXT("state_path"), StatePath));
	TestTrue(TEXT("restart_survival.support_bundle_resolved should exist"), RestartObject->TryGetBoolField(TEXT("support_bundle_resolved"), bSupportBundleResolved));
	TestTrue(TEXT("restart_survival.support_bundle_resolution should exist"), RestartObject->TryGetStringField(TEXT("support_bundle_resolution"), SupportBundleResolution));
	TestTrue(TEXT("restart_survival.support_bundle_root should exist"), RestartObject->TryGetStringField(TEXT("support_bundle_root"), SupportBundleRoot));
	TestTrue(TEXT("restart_survival.support_bundle_supervisor_script_path should exist"), RestartObject->TryGetStringField(TEXT("support_bundle_supervisor_script_path"), SupportBundleSupervisorPath));
	TestTrue(TEXT("restart_survival.support_bundle_preflight_script_path should exist"), RestartObject->TryGetStringField(TEXT("support_bundle_preflight_script_path"), SupportBundlePreflightPath));
	TestTrue(TEXT("restart_survival.state_support_bundle_resolution should exist"), RestartObject->TryGetStringField(TEXT("state_support_bundle_resolution"), StateSupportBundleResolution));
	TestTrue(TEXT("restart_survival.state_preflight_script_path should exist"), RestartObject->TryGetStringField(TEXT("state_preflight_script_path"), StatePreflightPath));

	TestTrue(TEXT("restart_survival should report a present state"), bStatePresent);
	TestTrue(TEXT("restart_survival should report pending resume"), bResumePending);
	TestTrue(TEXT("restart_survival should report a resolved portable support bundle"), bSupportBundleResolved);
	TestEqual(TEXT("restart_survival phase should be truthful"), Phase, FString(TEXT("AwaitingReattach")));
	TestEqual(TEXT("restart_survival task_id should be truthful"), TaskId, FString(TEXT("task_readback")));
	TestEqual(TEXT("restart_survival provider_session_id should be truthful"), ProviderSessionId, FString(TEXT("thread_readback")));
	TestEqual(TEXT("restart_survival support bundle resolution should prefer plugin-owned assets"), SupportBundleResolution, FString(TEXT("plugin_owned")));
	TestEqual(TEXT("restart_survival support bundle root should match the resolved bundle"), NormalizePathForTest(SupportBundleRoot), NormalizePathForTest(SupportBundle.BundleRoot));
	TestEqual(TEXT("restart_survival support bundle supervisor path should match the resolved bundle"), NormalizePathForTest(SupportBundleSupervisorPath), NormalizePathForTest(SupportBundle.SupervisorScriptPath));
	TestEqual(TEXT("restart_survival support bundle preflight path should match the resolved bundle"), NormalizePathForTest(SupportBundlePreflightPath), NormalizePathForTest(SupportBundle.PreflightScriptPath));
	TestEqual(TEXT("restart_survival saved state should preserve the bundle resolution actually used"), StateSupportBundleResolution, SupportBundle.ResolutionLabel);
	TestEqual(TEXT("restart_survival saved state should preserve the resolved preflight script path"), NormalizePathForTest(StatePreflightPath), NormalizePathForTest(SupportBundle.PreflightScriptPath));
	TestEqual(TEXT("restart_survival state path should match the project-local override"),
		NormalizePathForTest(StatePath),
		NormalizePathForTest(FUnrealClaudeRestartSurvivalManager::GetStatePath()));
	return true;
}

bool FRestartSurvival_FailedDetachedBuildRelaunchesEditor::RunTest(const FString& Parameters)
{
#if !PLATFORM_WINDOWS
	AddInfo(TEXT("Restart-survival supervisor relaunch coverage is Windows-only."));
	return true;
#else
	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("FailedDetachedBuildRelaunchesEditor"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(TestRoot, TEXT("Poligon1")));
	const FString UProjectPath = FPaths::Combine(ProjectRoot, TEXT("Poligon1.uproject"));
	const FString SavedRoot = FPaths::Combine(ProjectRoot, TEXT("Saved"));
	const FString BuildRoot = FPaths::Combine(SavedRoot, TEXT("UnrealBuildTool"));
	const FString BuildLogPath = FPaths::Combine(BuildRoot, TEXT("Poligon1Editor-current-build.log"));
	const FString BuildJsonPath = FPaths::Combine(BuildRoot, TEXT("Poligon1Editor-current-build.json"));
	const FString MarkerDir = FPaths::Combine(SavedRoot, TEXT("UnrealClaude"), TEXT("Automation"));
	const FString RelaunchMarkerPath = FPaths::Combine(MarkerDir, TEXT("failed_build_relaunch_marker.txt"));
	const FString FakeEditorPath = FPaths::Combine(ProjectRoot, TEXT("FakeEditorRelaunch.cmd"));
	const FString FakeBuildPath = FPaths::Combine(ProjectRoot, TEXT("Build.bat"));

	TestTrue(TEXT("fake project descriptor should be written"), SaveUtf8TextFile(UProjectPath, TEXT("{}")));

	auto ToBatchPath = [](const FString& InPath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		Path.ReplaceInline(TEXT("/"), TEXT("\\"));
		return Path;
	};

	const FString MarkerDirBatch = ToBatchPath(MarkerDir);
	const FString RelaunchMarkerBatch = ToBatchPath(RelaunchMarkerPath);
	const FString BuildRootBatch = ToBatchPath(BuildRoot);
	const FString BuildLogBatch = ToBatchPath(BuildLogPath);
	const FString BuildJsonBatch = ToBatchPath(BuildJsonPath);

	const FString FakeEditorScript = FString::Printf(
		TEXT("@echo off\r\n")
		TEXT("if not exist \"%s\" mkdir \"%s\"\r\n")
		TEXT("> \"%s\" echo relaunched_after_failed_build\r\n")
		TEXT("exit /b 0\r\n"),
		*MarkerDirBatch,
		*MarkerDirBatch,
		*RelaunchMarkerBatch);
	TestTrue(TEXT("fake editor relaunch script should be written"), SaveUtf8TextFile(FakeEditorPath, FakeEditorScript));

	const FString FakeBuildScript = FString::Printf(
		TEXT("@echo off\r\n")
		TEXT("if not exist \"%s\" mkdir \"%s\"\r\n")
		TEXT("> \"%s\" echo Log started at 04/15/2026 17:38:42\r\n")
		TEXT(">> \"%s\" echo   Running Internal UnrealHeaderTool FakeProject.uproject FakeManifest -WarningsAsErrors -installed\r\n")
		TEXT(">> \"%s\" echo D:\\Fake\\Intermediate\\Build\\Win64\\UnrealEditor\\Inc\\Poligon1\\UHT\\CombatCharacter.generated.h^(1^): Error: Failed to rename exported file: 'D:\\Fake\\Intermediate\\Build\\Win64\\UnrealEditor\\Inc\\Poligon1\\UHT\\CombatCharacter.generated.h.tmp'\r\n")
		TEXT(">> \"%s\" echo Result: Failed ^(OtherCompilationError^)\r\n")
		TEXT("> \"%s\" echo {\"result\":\"Failed (OtherCompilationError)\",\"current_blocker\":\"Failed to rename exported file\"}\r\n")
		TEXT("exit /b 1\r\n"),
		*BuildRootBatch,
		*BuildRootBatch,
		*BuildLogBatch,
		*BuildLogBatch,
		*BuildLogBatch,
		*BuildLogBatch,
		*BuildJsonBatch);
	TestTrue(TEXT("fake build script should be written"), SaveUtf8TextFile(FakeBuildPath, FakeBuildScript));

	uint32 EditorProcessId = 0;
	const FString SleepCommand = TEXT("-NoProfile -Command \"Start-Sleep -Seconds 1\"");
	FProcHandle FakeRunningEditor = FPlatformProcess::CreateProc(
		TEXT("powershell.exe"),
		*SleepCommand,
		true,
		false,
		false,
		&EditorProcessId,
		0,
		nullptr,
		nullptr);
	TestTrue(TEXT("fake running editor process should start"), FakeRunningEditor.IsValid());
	if (!FakeRunningEditor.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("fake running editor should exit before detached supervisor takes over"), WaitForProcessExit(FakeRunningEditor, 10.0));
	FPlatformProcess::CloseProc(FakeRunningEditor);

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("restart_survival_failed_build");
	State.TaskId = TEXT("task_failed_build");
	State.ProjectRoot = ProjectRoot;
	State.UProjectPath = UProjectPath;
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.BackendDisplayName = TEXT("Codex CLI [default]");
	State.ExecutionControlProfileId = TEXT("workspace_write_project");
	State.ExecutionTransportLabel = TEXT("persistent_app_server");
	State.ProviderSessionId = TEXT("provider_failed_build");
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::Detaching;
	State.PhaseDetail = TEXT("Detaching into fake supervisor coverage.");
	State.ReattachToken = TEXT("reattach_failed_build");
	State.ReattachNotice = TEXT("Restart-survival reattached after failed detached build.");
	State.EditorProcessId = static_cast<int32>(EditorProcessId);
	State.EditorExecutablePath = FakeEditorPath;
	State.BuildBatchPath = FakeBuildPath;
	State.BuildTarget = TEXT("Poligon1Editor");
	State.CreatedAtUtc = TEXT("2026-04-15T00:00:00Z");
	State.PostReattachCompletionText = TEXT("Base continuation prompt.");
	State.bPostReattachCompletionPending = true;

	FString Error;
	TestTrue(TEXT("restart-survival state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("save-state detail: %s"), *Error));
	}

	const FString SupervisorScriptPath = FUnrealClaudeRestartSurvivalManager::GetSupervisorScriptPath();
	const FString StatePath = FUnrealClaudeRestartSurvivalManager::GetStatePath();
	const FString SupervisorParams = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -StatePath \"%s\" -DisableStatusMonitor"),
		*SupervisorScriptPath,
		*StatePath);

	int32 SupervisorReturnCode = INDEX_NONE;
	FString SupervisorStdOut;
	FString SupervisorStdErr;
	const bool bSupervisorExecuted = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*SupervisorParams,
		&SupervisorReturnCode,
		&SupervisorStdOut,
		&SupervisorStdErr);
	TestTrue(TEXT("restart-survival supervisor should execute"), bSupervisorExecuted);
	TestEqual(TEXT("restart-survival supervisor should exit cleanly"), SupervisorReturnCode, 0);
	if (!SupervisorStdOut.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("supervisor stdout: %s"), *SupervisorStdOut));
	}
	if (!SupervisorStdErr.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("supervisor stderr: %s"), *SupervisorStdErr));
	}

	FUnrealClaudeRestartSurvivalState UpdatedState;
	TestTrue(TEXT("updated state should load"), FUnrealClaudeRestartSurvivalManager::LoadState(UpdatedState, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("load-state detail: %s"), *Error));
	}

	const FString NormalizedPostReattachCompletionText =
		UpdatedState.PostReattachCompletionText.Replace(TEXT("\\"), TEXT("/"));
	const FString NormalizedBuildLogPath = BuildLogPath.Replace(TEXT("\\"), TEXT("/"));
	const FString NormalizedBuildJsonPath = BuildJsonPath.Replace(TEXT("\\"), TEXT("/"));

	TestEqual(TEXT("failed detached build should still relaunch into awaiting reattach"),
		UpdatedState.Phase,
		EUnrealClaudeRestartSurvivalPhase::AwaitingReattach);
	TestTrue(TEXT("provider-thread resume should remain pending after failed detached build relaunch"),
		UpdatedState.bProviderThreadResumePending);
	TestEqual(TEXT("failed detached build should now record a bounded detached-owner terminal outcome"),
		UpdatedState.DetachedTerminalOutcome,
		FString(TEXT("failed")));
	TestEqual(TEXT("failed detached build should use the full detached step budget"), UpdatedState.DetachedStepIndex, 3);
	TestTrue(TEXT("failure reason should capture the repeated normalized blocker terminal fail"),
		UpdatedState.FailureReason.Contains(TEXT("same normalized blocker signature repeated"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("post-reattach completion text should include the detached build log anchor"),
		NormalizedPostReattachCompletionText.Contains(NormalizedBuildLogPath, ESearchCase::IgnoreCase));
	TestTrue(TEXT("post-reattach completion text should include the detached build json anchor"),
		NormalizedPostReattachCompletionText.Contains(NormalizedBuildJsonPath, ESearchCase::IgnoreCase));
	TestTrue(TEXT("post-reattach completion text should include the failed result line"),
		UpdatedState.PostReattachCompletionText.Contains(TEXT("Result: Failed"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("post-reattach completion text should include blocker evidence"),
		UpdatedState.PostReattachCompletionText.Contains(TEXT("Failed to rename exported file"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("failed detached build should still relaunch the editor surrogate"),
		WaitForFileToExist(RelaunchMarkerPath, 5.0));
	TestTrue(TEXT("detached current-build log should exist after the failed build"),
		IFileManager::Get().FileExists(*BuildLogPath));
	TestTrue(TEXT("detached current-build json should exist after the failed build"),
		IFileManager::Get().FileExists(*BuildJsonPath));
	return true;
#endif
}

bool FRestartSurvival_RelayAgentSuccessRelaunchesEditorAndInjectsContinuation::RunTest(const FString& Parameters)
{
#if !PLATFORM_WINDOWS
	AddInfo(TEXT("Restart-survival relay-agent supervisor coverage is Windows-only."));
	return true;
#else
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		MakeFreshRestartSurvivalTestRoot(TEXT("RelayAgentSuccessRelaunchesEditorAndInjectsContinuation")),
		TEXT("RelayProject")));
	const FString RelayRoot = FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"));
	FScopedRestartSurvivalRootOverride Override(RelayRoot);

	const FString UProjectPath = FPaths::Combine(ProjectRoot, TEXT("RelayProject.uproject"));
	const FString RelaunchMarkerPath = FPaths::Combine(RelayRoot, TEXT("relay_success_relaunch_marker.txt"));
	const FString FakeEditorPath = FPaths::Combine(ProjectRoot, TEXT("FakeRelayEditor.cmd"));
	const FString FakeCodexPath = FPaths::Combine(ProjectRoot, TEXT("FakeRelayCodex.cmd"));
	TestTrue(TEXT("relay project descriptor should be written"), SaveUtf8TextFile(UProjectPath, TEXT("{}")));

	auto ToBatchPath = [](const FString& InPath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		Path.ReplaceInline(TEXT("/"), TEXT("\\"));
		return Path;
	};

	const FString MarkerDirBatch = ToBatchPath(FPaths::GetPath(RelaunchMarkerPath));
	const FString RelaunchMarkerBatch = ToBatchPath(RelaunchMarkerPath);
	const FString FakeEditorScript = FString::Printf(
		TEXT("@echo off\r\n")
		TEXT("if not exist \"%s\" mkdir \"%s\"\r\n")
		TEXT("> \"%s\" echo relaunched_after_relay_success\r\n")
		TEXT("exit /b 0\r\n"),
		*MarkerDirBatch,
		*MarkerDirBatch,
		*RelaunchMarkerBatch);
	TestTrue(TEXT("relay fake editor relaunch script should be written"), SaveUtf8TextFile(FakeEditorPath, FakeEditorScript));

	const FString RelayResultPath = FUnrealClaudeRelayAgentManager::GetRelayResultPath();
	const FString FakeCodexScript = FString::Printf(
		TEXT("@echo off\r\n")
		TEXT("echo {\"type\":\"thread.started\",\"thread_id\":\"relay_session_success\"}\r\n")
		TEXT("echo {\"type\":\"turn.started\"}\r\n")
		TEXT("echo {\"type\":\"item.started\",\"item\":{\"id\":\"item_1\",\"type\":\"command_execution\",\"tool_name\":\"command_execution\",\"status\":\"in_progress\",\"command\":\"Get-Content build.log\"}}\r\n")
		TEXT("powershell -NoProfile -ExecutionPolicy Bypass -Command \"Start-Sleep -Seconds 3\"\r\n")
		TEXT("echo {\"type\":\"item.completed\",\"item\":{\"id\":\"item_1\",\"type\":\"command_execution\",\"tool_name\":\"command_execution\",\"status\":\"completed\",\"exit_code\":0,\"command\":\"Get-Content build.log\"}}\r\n")
		TEXT("echo {\"type\":\"item.started\",\"item\":{\"id\":\"item_2\",\"type\":\"command_execution\",\"tool_name\":\"command_execution\",\"status\":\"in_progress\",\"command\":\"Build.bat RelayProjectEditor Win64 Development\"}}\r\n")
		TEXT("powershell -NoProfile -ExecutionPolicy Bypass -Command \"Start-Sleep -Seconds 3\"\r\n")
		TEXT("echo {\"type\":\"item.completed\",\"item\":{\"id\":\"item_2\",\"type\":\"command_execution\",\"tool_name\":\"command_execution\",\"status\":\"completed\",\"exit_code\":0,\"command\":\"Build.bat RelayProjectEditor Win64 Development\"}}\r\n")
		TEXT("echo {\"type\":\"item.completed\",\"item\":{\"id\":\"item_3\",\"type\":\"agent_message\",\"text\":\"relay live probe ok\"}}\r\n")
		TEXT("echo {\"type\":\"turn.completed\"}\r\n")
		TEXT("exit /b 0\r\n"));
	TestTrue(TEXT("relay fake codex script should be written"), SaveUtf8TextFile(FakeCodexPath, FakeCodexScript));

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("restart_survival_relay_success");
	State.TaskId = TEXT("task_relay_success");
	State.ProjectRoot = ProjectRoot;
	State.UProjectPath = UProjectPath;
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.BackendDisplayName = TEXT("Codex CLI [default]");
	State.ExecutionControlProfileId = TEXT("workspace_write_project");
	State.ExecutionTransportLabel = TEXT("persistent_app_server");
	State.ProviderSessionId = TEXT("provider_relay_success");
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::Detaching;
	State.PhaseDetail = TEXT("Detaching into relay-agent supervisor coverage.");
	State.ReattachToken = TEXT("reattach_relay_success");
	State.ReattachNotice = TEXT("Restart-survival reattached after relay-agent success.");
	State.EditorProcessId = 0;
	State.EditorExecutablePath = FakeEditorPath;
	State.CreatedAtUtc = TEXT("2026-04-16T00:00:00Z");
	State.PostReattachCompletionText = TEXT("Base continuation prompt.");
	State.bPostReattachCompletionPending = true;
	State.DetachedObjective = TEXT("closed_editor_complex_build_relay_v1");
	State.DetachedObjectiveDetail = TEXT("Synthetic relay-agent supervisor proof.");

	FString Error;
	TestTrue(TEXT("relay restart-survival state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("relay save-state detail: %s"), *Error));
	}

	FUnrealClaudeRelayHandoffContext Handoff;
	Handoff.TaskId = State.TaskId;
	Handoff.RelaySessionId = TEXT("relay_session_success");
	Handoff.ProjectRoot = ProjectRoot;
	Handoff.UProjectPath = UProjectPath;
	Handoff.ReattachToken = State.ReattachToken;
	Handoff.ReattachNotice = State.ReattachNotice;
	Handoff.CreatedAtUtc = TEXT("2026-04-16T00:00:00Z");
	Handoff.OriginalUserPrompt = TEXT("Reply exactly: relay live probe ok");
	Handoff.EditorAgentSummary = TEXT("Synthetic relay-agent supervisor proof.");
	Handoff.LastKnownBlockerFamily = TEXT("synthetic_probe");
	Handoff.LastKnownBlockerSignature = TEXT("synthetic_probe_signature");
	Handoff.KnownFacts = { TEXT("probe") };
	Handoff.NextAttemptHypothesis = TEXT("Reply exactly with the probe string.");
	Handoff.BoundedObjective = TEXT("closed_editor_complex_build_relay_v1");
	Handoff.BoundedObjectiveDetail = TEXT("Synthetic relay-agent supervisor proof.");
	Handoff.ReasoningIterationBudget = 10;
	Handoff.WallClockBudgetSeconds = 120;
	Handoff.Settings = FUnrealClaudeRelayAgentManager::BuildCurrentCodexSettingsSnapshot();

	TestTrue(TEXT("relay handoff context should save"), FUnrealClaudeRelayAgentManager::SaveHandoffContext(Handoff, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("relay handoff detail: %s"), *Error));
	}

	const FString SupervisorScriptPath = FUnrealClaudeRestartSurvivalManager::GetSupervisorScriptPath();
	const FString StatePath = FUnrealClaudeRestartSurvivalManager::GetStatePath();
	const FString SupervisorParams = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -StatePath \"%s\" -DisableStatusMonitor"),
		*SupervisorScriptPath,
		*StatePath);

	const FString PreviousRelayExecutable = FPlatformMisc::GetEnvironmentVariable(TEXT("UNREALCLAUDE_RELAY_CODEX_EXECUTABLE"));
	FPlatformMisc::SetEnvironmentVar(TEXT("UNREALCLAUDE_RELAY_CODEX_EXECUTABLE"), *FakeCodexPath);

	int32 SupervisorReturnCode = INDEX_NONE;
	FString SupervisorStdOut;
	FString SupervisorStdErr;
	const bool bSupervisorExecuted = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*SupervisorParams,
		&SupervisorReturnCode,
		&SupervisorStdOut,
		&SupervisorStdErr);

	FPlatformMisc::SetEnvironmentVar(TEXT("UNREALCLAUDE_RELAY_CODEX_EXECUTABLE"), *PreviousRelayExecutable);
	TestTrue(TEXT("relay restart-survival supervisor should execute"), bSupervisorExecuted);
	TestEqual(TEXT("relay restart-survival supervisor should exit cleanly"), SupervisorReturnCode, 0);
	if (!SupervisorStdOut.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("relay supervisor stdout: %s"), *SupervisorStdOut));
	}
	if (!SupervisorStdErr.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("relay supervisor stderr: %s"), *SupervisorStdErr));
	}

	FUnrealClaudeRestartSurvivalState UpdatedState;
	TestTrue(TEXT("relay updated state should load"), FUnrealClaudeRestartSurvivalManager::LoadState(UpdatedState, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("relay load-state detail: %s"), *Error));
	}

	TestEqual(TEXT("relay success should relaunch into awaiting reattach"),
		UpdatedState.Phase,
		EUnrealClaudeRestartSurvivalPhase::AwaitingReattach);
	TestEqual(TEXT("relay success should record a successful detached terminal outcome"),
		UpdatedState.DetachedTerminalOutcome,
		FString(TEXT("success")));
	TestEqual(TEXT("relay success should use exactly one detached step"), UpdatedState.DetachedStepIndex, 1);
	TestTrue(TEXT("provider-thread resume should remain pending after relay success relaunch"),
		UpdatedState.bProviderThreadResumePending);
	TestTrue(TEXT("post-reattach continuation should include relay success outcome"),
		UpdatedState.PostReattachCompletionText.Contains(TEXT("Relay terminal outcome: success"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("post-reattach continuation should preserve the original user prompt"),
		UpdatedState.PostReattachCompletionText.Contains(Handoff.OriginalUserPrompt, ESearchCase::CaseSensitive));
	TestTrue(TEXT("post-reattach continuation should include the relay summary"),
		UpdatedState.PostReattachCompletionText.Contains(TEXT("relay live probe ok"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("post-reattach continuation should require tool surface validation"),
		UpdatedState.PostReattachCompletionText.Contains(TEXT("blocked_on_tool_surface"), ESearchCase::IgnoreCase)
		&& UpdatedState.PostReattachCompletionText.Contains(TEXT("capture_viewport"), ESearchCase::IgnoreCase));

	FUnrealClaudeRelayResult RelayResult;
	TestTrue(TEXT("relay result should load after supervisor run"), FUnrealClaudeRelayAgentManager::LoadRelayResult(RelayResult, Error));
	TestEqual(TEXT("relay result should report success"),
		RelayResult.TerminalOutcome,
		EUnrealClaudeRelayTerminalOutcome::Success);
	TestEqual(TEXT("relay result summary should match the probe prompt"), RelayResult.Summary, FString(TEXT("relay live probe ok")));
	TestTrue(TEXT("relay result should stay within the accepted reasoning budget"), RelayResult.IterationsUsed <= 10);

	FString ProgressJsonl;
	TestTrue(TEXT("relay progress log should exist"), FFileHelper::LoadFileToString(ProgressJsonl, *FUnrealClaudeRelayAgentManager::GetRelayProgressPath()));
	TestTrue(TEXT("relay progress should include summary events"), ProgressJsonl.Contains(TEXT("\"entry_kind\":\"summary\"")));
	TestTrue(TEXT("relay progress should include tool_start events"), ProgressJsonl.Contains(TEXT("\"entry_kind\":\"tool_start\"")));
	TestTrue(TEXT("relay progress should include tool_result events"), ProgressJsonl.Contains(TEXT("\"entry_kind\":\"tool_result\"")));
	TestTrue(TEXT("relay progress should include decision events"), ProgressJsonl.Contains(TEXT("\"entry_kind\":\"decision\"")));
	TestTrue(TEXT("relay progress should include heartbeat events"), ProgressJsonl.Contains(TEXT("\"entry_kind\":\"heartbeat\"")));
	TestTrue(TEXT("relay progress should include terminal events"), ProgressJsonl.Contains(TEXT("\"entry_kind\":\"terminal\"")));

	TestTrue(TEXT("relay success should relaunch the editor surrogate"), WaitForFileToExist(RelaunchMarkerPath, 5.0));
	return true;
#endif
}

bool FRestartSurvival_RelayAgentAdoptsExternalTerminalResultAndRelaunchesEditor::RunTest(const FString& Parameters)
{
#if !PLATFORM_WINDOWS
	AddInfo(TEXT("Restart-survival relay-agent external terminal result coverage is Windows-only."));
	return true;
#else
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		MakeFreshRestartSurvivalTestRoot(TEXT("RelayAgentAdoptsExternalTerminalResultAndRelaunchesEditor")),
		TEXT("RelayProject")));
	const FString RelayRoot = FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"));
	FScopedRestartSurvivalRootOverride Override(RelayRoot);

	const FString UProjectPath = FPaths::Combine(ProjectRoot, TEXT("RelayProject.uproject"));
	const FString RelaunchMarkerPath = FPaths::Combine(RelayRoot, TEXT("relay_external_result_relaunch_marker.txt"));
	const FString FakeEditorPath = FPaths::Combine(ProjectRoot, TEXT("FakeRelayEditor.cmd"));
	const FString FakeCodexPath = FPaths::Combine(ProjectRoot, TEXT("FakeRelayCodex.cmd"));
	TestTrue(TEXT("relay project descriptor should be written"), SaveUtf8TextFile(UProjectPath, TEXT("{}")));

	auto ToBatchPath = [](const FString& InPath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		Path.ReplaceInline(TEXT("/"), TEXT("\\"));
		return Path;
	};

	const FString MarkerDirBatch = ToBatchPath(FPaths::GetPath(RelaunchMarkerPath));
	const FString RelaunchMarkerBatch = ToBatchPath(RelaunchMarkerPath);
	const FString FakeEditorScript = FString::Printf(
		TEXT("@echo off\r\n")
		TEXT("if not exist \"%s\" mkdir \"%s\"\r\n")
		TEXT("> \"%s\" echo relaunched_after_external_relay_result\r\n")
		TEXT("exit /b 0\r\n"),
		*MarkerDirBatch,
		*MarkerDirBatch,
		*RelaunchMarkerBatch);
	TestTrue(TEXT("relay fake editor relaunch script should be written"), SaveUtf8TextFile(FakeEditorPath, FakeEditorScript));

	const FString RelayResultPath = FUnrealClaudeRelayAgentManager::GetRelayResultPath();
	const FString RelayResultPathBatch = ToBatchPath(RelayResultPath);
	const FString FakeCodexScript = FString::Printf(
		TEXT("@echo off\r\n")
		TEXT("echo {\"type\":\"thread.started\",\"thread_id\":\"relay_external_result\"}\r\n")
		TEXT("> \"%s\" echo {\"task_status\":\"completed\",\"bounded_objective\":\"closed_editor_complex_build_relay_v1\",\"current_blocker_family\":\"source_compile_error\",\"current_blocker_signature\":\"CombatDamageableBox.cpp writes private AActor::bReplicateMovement in UE 5.7\",\"final_build_target\":\"Poligon1Editor Win64 Development\",\"final_build_result\":\"Succeeded\",\"final_build_truth\":\"Fresh closed-editor rebuild after source fix completed successfully for the current source state.\"}\r\n")
		TEXT("powershell -NoProfile -ExecutionPolicy Bypass -Command \"Start-Sleep -Seconds 30\"\r\n")
		TEXT("exit /b 0\r\n"),
		*RelayResultPathBatch);
	TestTrue(TEXT("relay fake codex script should be written"), SaveUtf8TextFile(FakeCodexPath, FakeCodexScript));

	FUnrealClaudeRestartSurvivalState State;
	State.SessionId = TEXT("restart_survival_relay_external");
	State.TaskId = TEXT("task_relay_external");
	State.ProjectRoot = ProjectRoot;
	State.UProjectPath = UProjectPath;
	State.Backend = EUnrealClaudeProviderBackend::CodexCli;
	State.BackendDisplayName = TEXT("Codex CLI [default]");
	State.ExecutionControlProfileId = TEXT("workspace_write_project");
	State.ExecutionTransportLabel = TEXT("persistent_app_server");
	State.ProviderSessionId = TEXT("provider_relay_external");
	State.bProviderThreadResumePending = true;
	State.Phase = EUnrealClaudeRestartSurvivalPhase::Detaching;
	State.PhaseDetail = TEXT("Detaching into relay-agent external-result supervisor coverage.");
	State.ReattachToken = TEXT("reattach_relay_external");
	State.ReattachNotice = TEXT("Restart-survival reattached after relay-agent external result.");
	State.EditorProcessId = 0;
	State.EditorExecutablePath = FakeEditorPath;
	State.CreatedAtUtc = TEXT("2026-04-16T00:00:00Z");
	State.PostReattachCompletionText = TEXT("Base continuation prompt.");
	State.bPostReattachCompletionPending = true;
	State.DetachedObjective = TEXT("closed_editor_complex_build_relay_v1");
	State.DetachedObjectiveDetail = TEXT("Synthetic relay-agent external terminal result proof.");

	FString Error;
	TestTrue(TEXT("relay restart-survival state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	FUnrealClaudeRelayHandoffContext Handoff;
	Handoff.TaskId = State.TaskId;
	Handoff.RelaySessionId = TEXT("relay_session_external");
	Handoff.ProjectRoot = ProjectRoot;
	Handoff.UProjectPath = UProjectPath;
	Handoff.ReattachToken = State.ReattachToken;
	Handoff.ReattachNotice = State.ReattachNotice;
	Handoff.CreatedAtUtc = TEXT("2026-04-16T00:00:00Z");
	Handoff.OriginalUserPrompt = TEXT("Find the current blocker and report the truthful final build result.");
	Handoff.EditorAgentSummary = TEXT("Synthetic relay-agent external terminal result proof.");
	Handoff.LastKnownBlockerFamily = TEXT("source_compile_error");
	Handoff.LastKnownBlockerSignature = TEXT("CombatDamageableBox.cpp writes private AActor::bReplicateMovement in UE 5.7");
	Handoff.KnownFacts = { TEXT("relay writes a legacy result before process exit") };
	Handoff.NextAttemptHypothesis = TEXT("Accept the terminal result artifact and relaunch the editor.");
	Handoff.BoundedObjective = TEXT("closed_editor_complex_build_relay_v1");
	Handoff.BoundedObjectiveDetail = TEXT("Synthetic relay-agent external terminal result proof.");
	Handoff.ReasoningIterationBudget = 10;
	Handoff.WallClockBudgetSeconds = 120;
	Handoff.Settings = FUnrealClaudeRelayAgentManager::BuildCurrentCodexSettingsSnapshot();

	TestTrue(TEXT("relay handoff context should save"), FUnrealClaudeRelayAgentManager::SaveHandoffContext(Handoff, Error));

	const FString PreviousRelayExecutable = FPlatformMisc::GetEnvironmentVariable(TEXT("UNREALCLAUDE_RELAY_CODEX_EXECUTABLE"));
	FPlatformMisc::SetEnvironmentVar(TEXT("UNREALCLAUDE_RELAY_CODEX_EXECUTABLE"), *FakeCodexPath);

	const FString SupervisorScriptPath = FUnrealClaudeRestartSurvivalManager::GetSupervisorScriptPath();
	const FString StatePath = FUnrealClaudeRestartSurvivalManager::GetStatePath();
	const FString SupervisorParams = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -StatePath \"%s\" -DisableStatusMonitor"),
		*SupervisorScriptPath,
		*StatePath);

	int32 SupervisorReturnCode = INDEX_NONE;
	FString SupervisorStdOut;
	FString SupervisorStdErr;
	const bool bSupervisorExecuted = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*SupervisorParams,
		&SupervisorReturnCode,
		&SupervisorStdOut,
		&SupervisorStdErr);

	FPlatformMisc::SetEnvironmentVar(TEXT("UNREALCLAUDE_RELAY_CODEX_EXECUTABLE"), *PreviousRelayExecutable);

	TestTrue(TEXT("relay restart-survival supervisor should execute"), bSupervisorExecuted);
	TestEqual(TEXT("relay restart-survival supervisor should exit cleanly"), SupervisorReturnCode, 0);

	FUnrealClaudeRestartSurvivalState UpdatedState;
	TestTrue(TEXT("relay updated state should load"), FUnrealClaudeRestartSurvivalManager::LoadState(UpdatedState, Error));
	TestEqual(TEXT("relay external result should relaunch into awaiting reattach"),
		UpdatedState.Phase,
		EUnrealClaudeRestartSurvivalPhase::AwaitingReattach);
	TestEqual(TEXT("relay external result should record a successful detached terminal outcome"),
		UpdatedState.DetachedTerminalOutcome,
		FString(TEXT("success")));
	TestEqual(TEXT("relay external result should use exactly one detached step"), UpdatedState.DetachedStepIndex, 1);
	TestTrue(TEXT("post-reattach continuation should include relay success outcome"),
		UpdatedState.PostReattachCompletionText.Contains(TEXT("Relay terminal outcome: success"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("post-reattach continuation should include the normalized legacy summary"),
		UpdatedState.PostReattachCompletionText.Contains(TEXT("Fresh closed-editor rebuild after source fix completed successfully"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("post-reattach continuation should preserve the original user prompt"),
		UpdatedState.PostReattachCompletionText.Contains(Handoff.OriginalUserPrompt, ESearchCase::CaseSensitive));
	TestTrue(TEXT("post-reattach continuation should require tool surface validation after external relay result"),
		UpdatedState.PostReattachCompletionText.Contains(TEXT("blocked_on_tool_surface"), ESearchCase::IgnoreCase)
		&& UpdatedState.PostReattachCompletionText.Contains(TEXT("map_runtime_proof"), ESearchCase::IgnoreCase));

	FUnrealClaudeRelayResult RelayResult;
	TestTrue(TEXT("relay result should load after external-result supervisor run"), FUnrealClaudeRelayAgentManager::LoadRelayResult(RelayResult, Error));
	TestEqual(TEXT("relay result should report success"),
		RelayResult.TerminalOutcome,
		EUnrealClaudeRelayTerminalOutcome::Success);
	TestEqual(TEXT("relay result summary should use the legacy final build truth"),
		RelayResult.Summary,
		FString(TEXT("Fresh closed-editor rebuild after source fix completed successfully for the current source state.")));
	TestEqual(TEXT("relay result blocker family should be normalized from the legacy artifact"),
		RelayResult.FinalBlockerFamily,
		FString(TEXT("source_compile_error")));

	TestTrue(TEXT("relay external result should relaunch the editor surrogate"), WaitForFileToExist(RelaunchMarkerPath, 5.0));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ReadbackAndDebugSummarySurfaceActivePlanRelayProgressAndCancel,
	"UnrealClaude.RestartSurvival.ReadbackAndDebugSummarySurfaceActivePlanRelayProgressAndCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_ReadbackAndDebugSummarySurfaceActivePlanRelayProgressAndCancel::RunTest(const FString& Parameters)
{
	const FString RelayRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ReadbackAndDebugSummarySurfaceActivePlanRelayProgressAndCancel"));
	FScopedRestartSurvivalRootOverride Override(RelayRoot);

	FUnrealClaudeRestartSurvivalState State;
	State.TaskId = TEXT("task_monitor");
	State.ProjectRoot = TEXT("D:/MonitorProject");
	State.UProjectPath = TEXT("D:/MonitorProject/MonitorProject.uproject");
	State.Phase = EUnrealClaudeRestartSurvivalPhase::DetachedRunning;
	State.PhaseDetail = TEXT("Detached task owner step 1/3: relay_agent");
	State.LastUpdatedAtUtc = TEXT("2026-04-16T00:00:10Z");

	FString Error;
	TestTrue(TEXT("restart-survival state should save"), FUnrealClaudeRestartSurvivalManager::SaveState(State, Error));

	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_monitor");
	Plan.OriginalUserTask = TEXT("Monitor the bounded closed-editor build task.");
	Plan.Status = TEXT("active");
	Plan.SummaryRu = TEXT("Agent 2 выполняет закрытый build lane.");
	Plan.CurrentMechanicId = TEXT("perform_bounded_work");
	Plan.CurrentToolCallId = TEXT("tool_call_monitor");
	Plan.CurrentActionRu = TEXT("Выполняется закрытая сборка");
	Plan.CurrentTechnicalDetail = TEXT("Build.bat MonitorProjectEditor Win64 Development");
	Plan.Settings = FUnrealClaudeRelayAgentManager::BuildCurrentCodexSettingsSnapshot();
	Plan.CompletedMechanicIds = { TEXT("inspect_current_state") };

	FUnrealClaudePlanMechanicEntry WorkMechanic;
	WorkMechanic.MechanicId = TEXT("perform_bounded_work");
	WorkMechanic.LabelRu = TEXT("Закрытая сборка");
	WorkMechanic.Status = TEXT("in_progress");
	WorkMechanic.LastSummaryRu = TEXT("Сборка еще идет.");
	Plan.Mechanics.Add(WorkMechanic);
	TestTrue(TEXT("active plan should save"), FUnrealClaudeRelayAgentManager::SaveActivePlan(Plan, Error));

	FUnrealClaudeRelayProgressEntry Progress;
	Progress.TaskId = State.TaskId;
	Progress.PlanId = Plan.PlanId;
	Progress.MechanicId = Plan.CurrentMechanicId;
	Progress.ToolCallId = Plan.CurrentToolCallId;
	Progress.EntryKind = TEXT("tool_result");
	Progress.SummaryRu = TEXT("Agent 2 обработал command_execution.");
	Progress.TechnicalDetail = TEXT("Build.bat MonitorProjectEditor Win64 Development -> exit_code=0");
	Progress.CurrentActionRu = TEXT("Завершен command_execution");
	Progress.CurrentToolName = TEXT("command_execution");
	Progress.IterationIndex = 4;
	Progress.ElapsedSeconds = 18.5;
	Progress.HeartbeatAgeSeconds = 0.4;
	TestTrue(TEXT("relay progress should append"), FUnrealClaudeRelayAgentManager::AppendProgressEntry(Progress, Error));

	FUnrealClaudeRelayResult Result;
	Result.TaskId = State.TaskId;
	Result.PlanId = Plan.PlanId;
	Result.CurrentMechanicId = Plan.CurrentMechanicId;
	Result.CurrentToolCallId = Plan.CurrentToolCallId;
	Result.TerminalOutcome = EUnrealClaudeRelayTerminalOutcome::Success;
	Result.SummaryRu = TEXT("Agent 2 завершил закрытый этап.");
	Result.TechnicalDetail = TEXT("Target is up to date; Result: Succeeded");
	Result.PlanStatus = TEXT("awaiting_resume");
	Result.bRequiresPostReattachVerification = true;
	Result.CompletedMechanicIds = { TEXT("inspect_current_state"), TEXT("perform_bounded_work") };
	TestTrue(TEXT("relay result should save"), FUnrealClaudeRelayAgentManager::SaveRelayResult(Result, Error));

	TestTrue(TEXT("cancel request should save"), FUnrealClaudeRelayAgentManager::WriteCancelRequest(TEXT("monitor test"), Error));

	const TSharedPtr<FJsonObject> Readback = FUnrealClaudeRestartSurvivalManager::BuildReadbackJson();
	TestTrue(TEXT("readback should be valid"), Readback.IsValid());
	if (Readback.IsValid())
	{
		const TSharedPtr<FJsonObject> ActivePlanObject = GetObjectFieldOrNull(Readback, TEXT("active_plan"));
		TestTrue(TEXT("readback should surface active plan"), ActivePlanObject.IsValid());
		if (ActivePlanObject.IsValid())
		{
			FString SummaryRu;
			ActivePlanObject->TryGetStringField(TEXT("summary_ru"), SummaryRu);
			TestEqual(TEXT("readback should preserve localized plan summary"), SummaryRu, Plan.SummaryRu);
		}

		const TSharedPtr<FJsonObject> RelayResultObject = GetObjectFieldOrNull(Readback, TEXT("relay_result"));
		TestTrue(TEXT("readback should surface relay result"), RelayResultObject.IsValid());
		if (RelayResultObject.IsValid())
		{
			FString TechnicalDetail;
			RelayResultObject->TryGetStringField(TEXT("technical_detail"), TechnicalDetail);
			TestEqual(TEXT("readback should preserve relay technical detail"), TechnicalDetail, Result.TechnicalDetail);
		}

		bool bCancelPresent = false;
		Readback->TryGetBoolField(TEXT("relay_cancel_request_present"), bCancelPresent);
		TestTrue(TEXT("readback should surface cancel request presence"), bCancelPresent);
	}

	const FString DebugSummary = FUnrealClaudeRestartSurvivalManager::BuildWidgetDebugSummary();
	TestTrue(TEXT("debug summary should surface active plan current mechanic"), DebugSummary.Contains(TEXT("active_plan_current_mechanic = perform_bounded_work")));
	TestTrue(TEXT("debug summary should surface localized summary"), DebugSummary.Contains(TEXT("active_plan_summary_ru = Agent 2")));
	TestTrue(TEXT("debug summary should surface cancel request presence"), DebugSummary.Contains(TEXT("relay_cancel_request_present = true")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ArmEmissionSpawnsOwnerBeforeTransportReset,
	"UnrealClaude.RestartSurvival.ArmEmissionSpawnsOwnerBeforeTransportReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_ArmEmissionSpawnsOwnerBeforeTransportReset::RunTest(const FString& Parameters)
{
	// 624 race-fix regression guard. The production escalation path in
	// ClaudeEditorWidget.cpp::TryHandleClosedEditorBuildBlocker routes through
	// FUnrealClaudeRestartSurvivalManager::ExecuteSpawnBeforeTransportResetSequence
	// to guarantee that the detached-owner spawn-request artifact (in the real
	// path: restart_survival_state.json written by StartRestartSurvivalForCurrentTask)
	// is observable BEFORE the Codex transport reset (in the real path:
	// FClaudeCodeSubsystem::CancelCurrentRequest -> TerminateProc on the
	// persistent app-server) fires. This test feeds stand-in lambdas into the
	// same ordering helper the widget uses; the in-memory ordering flag and
	// the on-disk stand-in spawn artifact together prove the contract.

	const FString TestRoot = MakeFreshRestartSurvivalTestRoot(TEXT("ArmEmissionSpawnsOwnerBeforeTransportReset"));
	FScopedRestartSurvivalRootOverride Override(TestRoot);

	const FString SpawnRequestPath = FPaths::Combine(TestRoot, TEXT("stand_in_restart_survival_state.json"));

	// SUCCESS PATH: ordering flag must be flipped AFTER the spawn artifact
	// exists on disk, never before.
	{
		bool bTransportResetObserved = false;
		bool bSpawnArtifactExistedWhenResetFired = false;

		const bool bSequenceExecuted =
			FUnrealClaudeRestartSurvivalManager::ExecuteSpawnBeforeTransportResetSequence(
				[&]() -> bool
				{
					return SaveUtf8TextFile(SpawnRequestPath, TEXT("{\"stand_in_spawn_request\":true}"));
				},
				[&]()
				{
					bSpawnArtifactExistedWhenResetFired = IFileManager::Get().FileExists(*SpawnRequestPath);
					bTransportResetObserved = true;
				});

		TestTrue(TEXT("sequence should report executed on a successful spawn write"), bSequenceExecuted);
		TestTrue(TEXT("stand-in spawn-request artifact should exist on disk after the sequence"), IFileManager::Get().FileExists(*SpawnRequestPath));
		TestTrue(TEXT("transport reset must run after the spawn artifact exists, never before"), bSpawnArtifactExistedWhenResetFired);
		TestTrue(TEXT("transport reset stand-in must have fired once the spawn succeeded"), bTransportResetObserved);
	}

	// FAIL PATH: when the spawn-request write fails, the ordering helper must
	// NOT fire the transport reset. Otherwise the pre-624 race re-opens (arm
	// signal without an owning supervisor).
	{
		IFileManager::Get().Delete(*SpawnRequestPath, false, true, true);

		bool bTransportResetObserved = false;
		const bool bSequenceExecuted =
			FUnrealClaudeRestartSurvivalManager::ExecuteSpawnBeforeTransportResetSequence(
				[]() -> bool
				{
					// Simulate a spawn-request write that failed before producing
					// any on-disk artifact (for example, the real path's
					// SaveState returning false).
					return false;
				},
				[&]()
				{
					bTransportResetObserved = true;
				});

		TestFalse(TEXT("sequence should report not-executed when the spawn write failed"), bSequenceExecuted);
		TestFalse(TEXT("transport reset must NOT fire when the spawn-request write failed"), bTransportResetObserved);
		TestFalse(TEXT("no spawn artifact should exist after a failed write"), IFileManager::Get().FileExists(*SpawnRequestPath));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_ArmEmissionPreservesEditorExitIntent,
	"UnrealClaude.RestartSurvival.ArmEmissionPreservesEditorExitIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_ArmEmissionPreservesEditorExitIntent::RunTest(const FString& Parameters)
{
	// 624 v3 regression guard. The closed-editor build-blocker arm path MUST
	// pass FClaudeRestartSurvivalStartOptions::bRequestEditorExit = true into
	// StartRestartSurvivalForCurrentTask. If it is false, the 0.2 s ticker at
	// ClaudeEditorWidget.cpp:2069 never schedules FPlatformMisc::RequestExit,
	// the editor never exits, and the PS supervisor
	// (UnrealClaude-RestartSurvival.ps1) sits forever in
	// phase=DetachedRunning / phase_detail="waiting for UnrealEditor.exe to exit".
	// That is the exact v2→v3 regression the 2026-04-18 user cycle surfaced.
	//
	// Observing the intent: v3 extracts the Options construction into the pure
	// static helper SClaudeEditorWidget::BuildArmOptionsForClosedEditorBuildBlocker.
	// This test calls that helper directly and asserts the two flags the arm
	// path depends on. Any future edit that flips bRequestEditorExit back to
	// false (whether as an explicit assignment or as a default struct change)
	// must fail this test.
	const FClaudeRestartSurvivalStartOptions ArmOptions =
		SClaudeEditorWidget::BuildArmOptionsForClosedEditorBuildBlocker();

	TestTrue(
		TEXT("closed-editor blocker arm path must set bRequestEditorExit=true so the editor-exit ticker schedules"),
		ArmOptions.bRequestEditorExit);
	TestTrue(
		TEXT("closed-editor blocker arm path must set bBypassWaitingForResponseGuard=true so the mid-turn spawn is allowed"),
		ArmOptions.bBypassWaitingForResponseGuard);

	// Structural tripwire: if a future refactor drops the explicit editor-exit
	// intent on the struct default (today default=true at ClaudeEditorWidget.h:26),
	// the previous two assertions still cover it via the helper's return value.
	// This third assertion pins the struct default separately so a silent
	// default flip does not escape both layers of coverage.
	FClaudeRestartSurvivalStartOptions DefaultOptions;
	TestTrue(
		TEXT("FClaudeRestartSurvivalStartOptions default must keep bRequestEditorExit=true; flipping the default is a silent regression vector"),
		DefaultOptions.bRequestEditorExit);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_MonitorCloseSemanticShellIntegration,
	"UnrealClaude.RestartSurvival.MonitorCloseSemanticShellIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_MonitorCloseSemanticShellIntegration::RunTest(const FString& Parameters)
{
	// 606 sub-stage (2026-04-18): shell-level integration guard for the monitor
	// window's Close button semantic. The in-editor side cannot directly exercise
	// the WinForms button click (GUI only), so the pure decision is factored into
	// the PowerShell helper Resolve-MonitorCloseCancelPayload and the companion
	// PS test script UnrealClaude-RestartSurvivalMonitor-CloseSemantic.Tests.ps1
	// drives that helper across all phase-classification cases. This C++ test
	// spawns powershell.exe to run that script, asserts exit code 0, and logs
	// the stdout so a regression (e.g. someone drops a terminal phase from the
	// classification, or rewires the close handler to skip the cancel write in
	// a non-terminal phase) surfaces inside the normal UE automation run.

	const FString MonitorScriptPath = FUnrealClaudeRestartSurvivalManager::GetMonitorScriptPath();
	if (!IFileManager::Get().FileExists(*MonitorScriptPath))
	{
		AddError(FString::Printf(TEXT("monitor script not found at %s"), *MonitorScriptPath));
		return false;
	}

	const FString TestScriptPath = FPaths::Combine(
		FPaths::GetPath(MonitorScriptPath),
		TEXT("UnrealClaude-RestartSurvivalMonitor-CloseSemantic.Tests.ps1"));
	if (!IFileManager::Get().FileExists(*TestScriptPath))
	{
		AddError(FString::Printf(TEXT("close-semantic PS test script not found at %s"), *TestScriptPath));
		return false;
	}

	const FString PowerShellParams = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\""),
		*TestScriptPath);

	int32 TestReturnCode = INDEX_NONE;
	FString TestStdOut;
	FString TestStdErr;
	const bool bTestExecuted = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*PowerShellParams,
		&TestReturnCode,
		&TestStdOut,
		&TestStdErr);
	TestTrue(TEXT("close-semantic PS test should execute"), bTestExecuted);
	if (!bTestExecuted)
	{
		return false;
	}

	// Mirror PS script stdout into the UE automation log so assertion-level
	// detail survives in Saved/Logs even if this test later fails on a specific
	// phase case. The script prints one "[PASS]" / "[FAIL]" line per assertion.
	TArray<FString> StdOutLines;
	TestStdOut.ParseIntoArrayLines(StdOutLines);
	for (const FString& Line : StdOutLines)
	{
		AddInfo(Line);
	}
	if (!TestStdErr.IsEmpty())
	{
		AddError(FString::Printf(TEXT("close-semantic PS test stderr: %s"), *TestStdErr));
	}

	TestEqual(TEXT("close-semantic PS test should exit cleanly (0 failures)"), TestReturnCode, 0);
	TestTrue(TEXT("close-semantic PS test stdout should contain the summary line"),
		TestStdOut.Contains(TEXT("assertions, 0 failures")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvival_DetachedBuildCaptureShellIntegration,
	"UnrealClaude.RestartSurvival.DetachedBuildCaptureShellIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestartSurvival_DetachedBuildCaptureShellIntegration::RunTest(const FString& Parameters)
{
	// 639 shell-level integration guard: run the real supervisor script against
	// synthetic detached-build scenarios that would previously freeze the
	// supervisor loop when Peek()+ReadLine() blocked on redirected output without
	// a newline. The companion PowerShell script drives two deterministic cases:
	// delayed stdout newline with >5 s heartbeat gap, and mixed stdout/stderr
	// bursts with delayed newline completion. This wrapper only checks process
	// execution + logs stdout/stderr into the UE automation log.

	const FString SupervisorScriptPath = FUnrealClaudeRestartSurvivalManager::GetSupervisorScriptPath();
	if (!IFileManager::Get().FileExists(*SupervisorScriptPath))
	{
		AddError(FString::Printf(TEXT("supervisor script not found at %s"), *SupervisorScriptPath));
		return false;
	}

	const FString TestScriptPath = FPaths::Combine(
		FPaths::GetPath(SupervisorScriptPath),
		TEXT("UnrealClaude-RestartSurvival-Capture.Tests.ps1"));
	if (!IFileManager::Get().FileExists(*TestScriptPath))
	{
		AddError(FString::Printf(TEXT("detached-build capture PS test script not found at %s"), *TestScriptPath));
		return false;
	}

	const FString PowerShellParams = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\""),
		*TestScriptPath);

	int32 TestReturnCode = INDEX_NONE;
	FString TestStdOut;
	FString TestStdErr;
	const bool bTestExecuted = FPlatformProcess::ExecProcess(
		TEXT("powershell.exe"),
		*PowerShellParams,
		&TestReturnCode,
		&TestStdOut,
		&TestStdErr);
	TestTrue(TEXT("detached-build capture PS test should execute"), bTestExecuted);
	if (!bTestExecuted)
	{
		return false;
	}

	TArray<FString> StdOutLines;
	TestStdOut.ParseIntoArrayLines(StdOutLines);
	for (const FString& Line : StdOutLines)
	{
		AddInfo(Line);
	}
	if (!TestStdErr.IsEmpty())
	{
		AddError(FString::Printf(TEXT("detached-build capture PS test stderr: %s"), *TestStdErr));
	}

	TestEqual(TEXT("detached-build capture PS test should exit cleanly (0 failures)"), TestReturnCode, 0);
	TestTrue(TEXT("detached-build capture PS test stdout should contain the summary line"),
		TestStdOut.Contains(TEXT("assertions, 0 failures")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidget_Agent2BackgroundBannerVisibilityDecision,
	"UnrealClaude.Widget.Agent2BackgroundBannerVisibilityDecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidget_Agent2BackgroundBannerVisibilityDecision::RunTest(const FString& Parameters)
{
	// 606 Part B guard: the Agent 2 background banner must (a) appear while a
	// supervisor is alive in any non-terminal phase, (b) disappear on terminal
	// phases, (c) disappear when the owner process has died (crash-detect
	// agreement with PollRestartSurvivalDetachedOwnerState), (d) default to
	// visible when DetachedOwnerProcessId is not yet populated so the arm-time
	// window is covered.

	auto MakeState = [](EUnrealClaudeRestartSurvivalPhase Phase, int32 Pid)
	{
		FUnrealClaudeRestartSurvivalState State;
		State.Phase = Phase;
		State.DetachedOwnerProcessId = Pid;
		State.TaskId = TEXT("task_banner_visibility_test");
		return State;
	};

	// Non-terminal phases with pid>0 and process alive → visible.
	for (const EUnrealClaudeRestartSurvivalPhase Phase : {
		EUnrealClaudeRestartSurvivalPhase::Detaching,
		EUnrealClaudeRestartSurvivalPhase::DetachedRunning,
		EUnrealClaudeRestartSurvivalPhase::AwaitingRelaunch,
		EUnrealClaudeRestartSurvivalPhase::Relaunching,
		EUnrealClaudeRestartSurvivalPhase::AwaitingReattach})
	{
		const FUnrealClaudeRestartSurvivalState State = MakeState(Phase, 12345);
		TestTrue(
			FString::Printf(TEXT("non-terminal phase %s + pid alive → visible"),
				UnrealClaudeRestartSurvivalPhaseToString(Phase)),
			SClaudeEditorWidget::ShouldShowAgent2BackgroundBanner(State, true));
	}

	// Terminal phases → hidden regardless of liveness.
	for (const EUnrealClaudeRestartSurvivalPhase Phase : {
		EUnrealClaudeRestartSurvivalPhase::Reattached,
		EUnrealClaudeRestartSurvivalPhase::FailedTerminal,
		EUnrealClaudeRestartSurvivalPhase::AttachedInEditor})
	{
		const FUnrealClaudeRestartSurvivalState State = MakeState(Phase, 12345);
		TestFalse(
			FString::Printf(TEXT("terminal phase %s → hidden"),
				UnrealClaudeRestartSurvivalPhaseToString(Phase)),
			SClaudeEditorWidget::ShouldShowAgent2BackgroundBanner(State, true));
	}

	// Non-terminal phase + pid>0 + process dead → hidden (crash-detect agreement).
	{
		const FUnrealClaudeRestartSurvivalState State = MakeState(
			EUnrealClaudeRestartSurvivalPhase::DetachedRunning, 98765);
		TestFalse(TEXT("dead-owner non-terminal phase → hidden"),
			SClaudeEditorWidget::ShouldShowAgent2BackgroundBanner(State, false));
	}

	// Non-terminal phase with pid=0 (early Detaching arm window) → visible
	// regardless of liveness bool — the banner must cover the narrow window
	// between arm-emission and supervisor CreateProc returning a real pid.
	{
		const FUnrealClaudeRestartSurvivalState State = MakeState(
			EUnrealClaudeRestartSurvivalPhase::Detaching, 0);
		TestTrue(TEXT("early detaching (pid=0) → visible even if liveness bool is false"),
			SClaudeEditorWidget::ShouldShowAgent2BackgroundBanner(State, false));
		TestTrue(TEXT("early detaching (pid=0) → visible when liveness bool is true"),
			SClaudeEditorWidget::ShouldShowAgent2BackgroundBanner(State, true));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidget_Agent2BannerCancelPayloadShape,
	"UnrealClaude.Widget.Agent2BannerCancelPayloadShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidget_Agent2BannerCancelPayloadShape::RunTest(const FString& Parameters)
{
	// 606 Part B guard: the banner's Cancel button writes a payload with the
	// same schema as the monitor window's Cancel button + the 606 Part A
	// close-on-non-terminal handler — only `reason` may differ for audit.

	const FString Payload = SClaudeEditorWidget::BuildAgent2BannerCancelRequestPayload();
	TestFalse(TEXT("cancel payload must not be empty"), Payload.IsEmpty());

	// Schema version field present and equals 1.
	TestTrue(TEXT("cancel payload must declare schema_version: 1"),
		Payload.Contains(TEXT("\"schema_version\": 1")));

	// Reason identifies the widget-banner source for audit.
	TestTrue(TEXT("cancel payload must set reason to user_requested_cancel_from_widget_banner"),
		Payload.Contains(TEXT("\"reason\": \"user_requested_cancel_from_widget_banner\"")));

	// Timestamp field present and non-empty (ISO 8601 via FDateTime::UtcNow).
	TestTrue(TEXT("cancel payload must include requested_at_utc"),
		Payload.Contains(TEXT("\"requested_at_utc\"")));

	// Round-trip through FJsonObject to confirm it's valid JSON with the expected keys.
	TSharedPtr<FJsonObject> Parsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	TestTrue(TEXT("cancel payload must be valid JSON"),
		FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid());
	if (Parsed.IsValid())
	{
		int32 SchemaVersion = 0;
		TestTrue(TEXT("JSON must carry schema_version as number 1"),
			Parsed->TryGetNumberField(TEXT("schema_version"), SchemaVersion) && SchemaVersion == 1);

		FString Reason;
		TestTrue(TEXT("JSON must carry reason string"),
			Parsed->TryGetStringField(TEXT("reason"), Reason));
		TestEqual(TEXT("JSON reason must be user_requested_cancel_from_widget_banner"),
			Reason, FString(TEXT("user_requested_cancel_from_widget_banner")));

		FString RequestedAtUtc;
		TestTrue(TEXT("JSON must carry requested_at_utc string"),
			Parsed->TryGetStringField(TEXT("requested_at_utc"), RequestedAtUtc));
		TestFalse(TEXT("requested_at_utc must not be empty"),
			RequestedAtUtc.IsEmpty());
	}

	return true;
}

#endif
