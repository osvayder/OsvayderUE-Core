// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "AgentOrchestrator.h"
#include "ClaudeEditorWidget.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClaudeRecipeRegistry.h"
#include "UnrealClaudeRoleRegistry.h"
#include "UnrealClaudeUserFacingStatus.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FUnrealClaudeActivePlan MakeCppMutationPlan()
	{
		FUnrealClaudeActivePlan Plan;
		Plan.PlanId = TEXT("plan_638");
		Plan.Status = TEXT("active");
		Plan.ResultStatus = TEXT("incomplete");
		Plan.CompileProof.bCompiledModuleMutationObserved = true;
		Plan.CompileProof.MutationToolFamily = TEXT("workspace_file_build");
		Plan.CompileProof.LastMutationAtUtc = TEXT("2026-04-22T08:45:00Z");
		Plan.CompileProof.LastMutationToolCallId = TEXT("tool_call_edit_cpp");
		Plan.CompileProof.LastMutationToolName = TEXT("write");
		return Plan;
	}

	FUnrealClaudeActivePlan MakeFeatureWorkflowPlan()
	{
		FUnrealClaudeActivePlan Plan = MakeCppMutationPlan();
		Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_closeout_1");
		Plan.FeatureWorkflow.RecipeId = TEXT("feature.inventory_basic_ui_v1");
		Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
		Plan.FeatureWorkflow.bCompileProofRequired = true;
		Plan.FeatureWorkflow.CompileProofState = TEXT("passed");
		Plan.FeatureWorkflow.bRuntimeProofRequired = true;
		Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");
		Plan.FeatureWorkflow.CompletedPhaseIds = {
			TEXT("data_model"),
			TEXT("runtime_owner"),
			TEXT("input_controller"),
			TEXT("ui_widget"),
			TEXT("compile_gate")
		};
		return Plan;
	}

	FUnrealClaudeActivePlan MakeInteractionAccessFeatureWorkflowPlan()
	{
		FUnrealClaudeActivePlan Plan = MakeCppMutationPlan();
		Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_closeout_interaction_1");
		Plan.FeatureWorkflow.RecipeId = TEXT("feature.interaction_access_slice_v1");
		Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
		Plan.FeatureWorkflow.bCompileProofRequired = true;
		Plan.FeatureWorkflow.CompileProofState = TEXT("passed");
		Plan.FeatureWorkflow.bRuntimeProofRequired = true;
		Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");
		Plan.FeatureWorkflow.AuthoringLaneState = TEXT("pending");
		Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("pending");
		Plan.FeatureWorkflow.CompletedPhaseIds = {
			TEXT("project_context_preflight"),
			TEXT("interaction_contract"),
			TEXT("input_asset_authoring"),
			TEXT("runtime_actor_state"),
			TEXT("attempt_resolver_and_logging"),
			TEXT("proof_context_setup"),
			TEXT("compile_gate")
		};
		return Plan;
	}

	FUnrealClaudeActivePlan MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan()
	{
		FUnrealClaudeActivePlan Plan = MakeInteractionAccessFeatureWorkflowPlan();
		Plan.CompileProof = FUnrealClaudeCompileProofState();
		Plan.FeatureWorkflow.bCompileProofRequired = false;
		Plan.FeatureWorkflow.CompileProofState = TEXT("not_required");
		return Plan;
	}

	void PrimeCompileSuccess(FUnrealClaudeActivePlan& Plan)
	{
		Plan.CompileProof.LastCompileProofAtUtc = TEXT("2026-04-22T08:46:00Z");
		Plan.CompileProof.LastCompileProofToolCallId = TEXT("tool_call_compile");
		Plan.CompileProof.LastCompileProofToolName = TEXT("livecoding_compile");
		Plan.CompileProof.LastCompileProofOutcome = TEXT("success");
	}

	void PrimeBoundedInteractionAccessCloseoutFacts(FUnrealClaudeCloseoutFactSnapshot& Facts)
	{
		Facts.bKnownProofMapAvailable = true;
		Facts.bProofInputMappingAvailable = true;
		Facts.bPlacedRuntimeActorsAvailable = true;
		Facts.bAttemptResolverSourceObserved = true;
		Facts.bEventSubsystemSourceObserved = true;
		Facts.bRuntimeSmokeSuccessObserved = true;
		Facts.bPrisonAccessEventObserved = true;
		Facts.AutomationDiscoveryCount = 7;
		Facts.AutomationExecutedCount = 7;
		Facts.AutomationPassedCount = 7;
		Facts.AutomationFailedCount = 0;
		Facts.AddFactId(TEXT("test.known_proof_map"));
		Facts.AddFactId(TEXT("test.proof_input_mapping"));
		Facts.AddFactId(TEXT("test.placed_runtime_actors"));
		Facts.AddFactId(TEXT("test.complete_observation"));
		Facts.AddFactId(TEXT("test.bounded_automation"));
	}

	void PrimeProofInputMappingObservation(FAgentFeatureWorkflowState& Workflow)
	{
		Workflow.InteractionAccessReuseObservation.bPersistentInputAssetObserved = true;
		Workflow.InteractionAccessReuseObservation.bInteractionActionAssetObserved = true;
		Workflow.InteractionAccessReuseObservation.bReadOnlyEnhancedInputQueryObserved = true;
	}

	FString MakeActivePlanCloseoutTestRoot(const FString& TestName)
	{
		FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UnrealClaude"),
			TEXT("Automation"),
			TEXT("ActivePlanCloseout"),
			TestName);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	FString WriteVisualQaManifestForTest(
		const FString& TestRoot,
		const FString& Verdict,
		const bool bIncludeActualScreenshotPath,
		const FString& PlanId,
		const bool bIncludeManualMechanicsChecklist = false)
	{
		FString ScreenshotPath = FPaths::Combine(TestRoot, TEXT("actual_combat_inventory.png"));
		FPaths::NormalizeFilename(ScreenshotPath);
		if (bIncludeActualScreenshotPath)
		{
			FFileHelper::SaveStringToFile(TEXT("fake png bytes for manifest path test"), *ScreenshotPath);
		}

		FString ManifestPath = FPaths::Combine(TestRoot, FString::Printf(TEXT("visual_qa_manifest_%s.json"), *Verdict));
		FPaths::NormalizeFilename(ManifestPath);
		const FString ActualScreenshots = bIncludeActualScreenshotPath
			? FString::Printf(TEXT("\"%s\""), *ScreenshotPath)
			: FString();
		const FString ManualMechanicsFields = bIncludeManualMechanicsChecklist
			? TEXT("  \"visual_verdict\": \"passed\",\n")
				TEXT("  \"mechanics_not_required_for_visual_closeout\": true,\n")
				TEXT("  \"manual_mechanics_checklist\": [\"open inventory with I\", \"move an item between cells\", \"verify identical items stack\"],\n")
			: FString();
		const FString ManifestText = FString::Printf(
			TEXT("{\n")
			TEXT("  \"schema_version\": 1,\n")
			TEXT("  \"artifact_type\": \"visual_qa_manifest\",\n")
			TEXT("  \"run_id\": \"run_visual_qa_test\",\n")
			TEXT("  \"plan_id\": \"%s\",\n")
			TEXT("  \"task_id\": \"task_visual_qa_test\",\n")
			TEXT("  \"reference_artifact_paths\": [\"D:/References/combat_inventory.png\"],\n")
			TEXT("  \"actual_screenshot_paths\": [%s],\n")
			TEXT("  \"target_surface\": \"Poligon1 combat inventory HUD\",\n")
			TEXT("  \"target_widget_or_level\": \"Variant_Combat inventory widget\",\n")
			TEXT("  \"reviewer\": \"agent\",\n")
			TEXT("%s")
			TEXT("  \"checklist\": [\"matches reference layout\", \"three pickups visible\", \"stacking observed\"],\n")
			TEXT("  \"verdict\": \"%s\",\n")
			TEXT("  \"summary\": \"Focused visual QA manifest fixture.\",\n")
			TEXT("  \"created_at_utc\": \"2026-04-30T00:00:00Z\"\n")
			TEXT("}\n"),
			*PlanId,
			*ActualScreenshots,
			*ManualMechanicsFields,
			*Verdict);
		FFileHelper::SaveStringToFile(ManifestText, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return ManifestPath;
	}

	TSharedPtr<FJsonObject> MakeCloseoutTraceToolResult(
		const FString& EventId,
		const FString& ToolName,
		const FString& ToolInput,
		const FString& ToolResult,
		const FString& RunId = FString(),
		const FString& TimestampUtc = FString())
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("tool_name"), ToolName);
		Payload->SetStringField(TEXT("tool_input"), ToolInput);
		Payload->SetStringField(TEXT("tool_result"), ToolResult);

		TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("event_id"), EventId);
		Record->SetStringField(TEXT("event_type"), TEXT("tool_result"));
		if (!RunId.IsEmpty())
		{
			Record->SetStringField(TEXT("run_id"), RunId);
		}
		if (!TimestampUtc.IsEmpty())
		{
			Record->SetStringField(TEXT("timestamp"), TimestampUtc);
		}
		Record->SetObjectField(TEXT("payload"), Payload);
		return Record;
	}

	TSharedPtr<FJsonObject> MakeCloseoutTraceToolUse(
		const FString& EventId,
		const FString& ToolName,
		const FString& ToolInput,
		const bool bClassifiedMutatingTool,
		const FString& ClassifiedToolFamily,
		const FString& RunId = FString(),
		const FString& TimestampUtc = FString())
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("tool_name"), ToolName);
		Payload->SetStringField(TEXT("tool_input"), ToolInput);
		Payload->SetBoolField(TEXT("classified_mutating_tool"), bClassifiedMutatingTool);
		Payload->SetStringField(TEXT("classified_tool_family"), ClassifiedToolFamily);

		TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("event_id"), EventId);
		Record->SetStringField(TEXT("event_type"), TEXT("tool_use"));
		if (!RunId.IsEmpty())
		{
			Record->SetStringField(TEXT("run_id"), RunId);
		}
		if (!TimestampUtc.IsEmpty())
		{
			Record->SetStringField(TEXT("timestamp"), TimestampUtc);
		}
		Record->SetObjectField(TEXT("payload"), Payload);
		return Record;
	}

	void AppendCleanInteractionAccessTraceRecords(
		TArray<TSharedPtr<FJsonObject>>& Records,
		const FString& RunId,
		const FString& TimestampPrefix)
	{
		Records.Add(MakeCloseoutTraceToolResult(
			TEXT("trace_input_mapping_clean"),
			TEXT("unrealclaude/enhanced_input"),
			TEXT("{\"operation\":\"query_context\",\"context\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof\"}"),
			TEXT("IMC_PrisonAccessProof contains IA_Interact input mapping; read_only_enhanced_input_query_observed=true"),
			RunId,
			TimestampPrefix + TEXT("00Z")));
		Records.Add(MakeCloseoutTraceToolResult(
			TEXT("trace_open_level_clean"),
			TEXT("unrealclaude/open_level"),
			TEXT("{\"level\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}"),
			TEXT("Opened /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof"),
			RunId,
			TimestampPrefix + TEXT("01Z")));
		Records.Add(MakeCloseoutTraceToolResult(
			TEXT("trace_get_actors_clean"),
			TEXT("unrealclaude/get_level_actors"),
			TEXT("{}"),
			TEXT("BP_PrisonAccessProof placed in Lvl_PrisonAccessProof"),
			RunId,
			TimestampPrefix + TEXT("02Z")));
		Records.Add(MakeCloseoutTraceToolResult(
			TEXT("trace_sources_clean"),
			TEXT("command_execution"),
			TEXT("Select-String Source/Alternative/PrisonAccess"),
			TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp FAlternativePrisonAccessAttemptResolver ResolveDoorAttempt\nSource/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp UAlternativePrisonAccessEventSubsystem RecordEvent FAlternativePrisonAccessEventRecord PrisonAccessEvent order="),
			RunId,
			TimestampPrefix + TEXT("03Z")));
		Records.Add(MakeCloseoutTraceToolResult(
			TEXT("trace_automation_clean"),
			TEXT("command_execution"),
			TEXT("Automation RunTests Alternative.PrisonAccess"),
			TEXT("Found 7 automation tests based on 'Alternative.PrisonAccess'\nProofFixtureSmoke Result={Success}\nAuthorizedRoute Result={Success}\nForceRoute Result={Success}\nInputPathTruth Result={Success}\nProofFixturePresence Result={Success}\nTechnicalRoute Result={Success}\nUnskilledRoute Result={Success}\nPrisonAccessEvent order=1 tag=attempt.technical.override_success\nPrisonAccessEvent order=2 tag=attempt.access.open_existing\n**** TEST COMPLETE. EXIT CODE: 0 ****"),
			RunId,
			TimestampPrefix + TEXT("04Z")));
	}

	void AppendInteractionAccessSourceOnlyTraceRecords(
		TArray<TSharedPtr<FJsonObject>>& Records,
		const FString& RunId,
		const FString& TimestampPrefix)
	{
		Records.Add(MakeCloseoutTraceToolResult(
			TEXT("trace_packet662_input_mapping"),
			TEXT("unrealclaude/enhanced_input"),
			TEXT("{\"operation\":\"query_context\",\"context\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof\"}"),
			TEXT("IMC_PrisonAccessProof contains IA_Interact input mapping; read_only_enhanced_input_query_observed=true"),
			RunId,
			TimestampPrefix + TEXT("00Z")));
		Records.Add(MakeCloseoutTraceToolResult(
			TEXT("trace_packet662_open_level"),
			TEXT("unrealclaude/open_level"),
			TEXT("{\"level\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}"),
			TEXT("Opened /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof"),
			RunId,
			TimestampPrefix + TEXT("01Z")));
		Records.Add(MakeCloseoutTraceToolResult(
			TEXT("trace_packet662_get_actors"),
			TEXT("unrealclaude/get_level_actors"),
			TEXT("{}"),
			TEXT("Proof_PrisonAccess_PlayerStart Proof_PrisonAccess_Floor Proof_PrisonAccess_Door Proof_PrisonAccess_ControlBox"),
			RunId,
			TimestampPrefix + TEXT("02Z")));
		Records.Add(MakeCloseoutTraceToolResult(
			TEXT("trace_packet662_source_observation"),
			TEXT("command_execution"),
			TEXT("Select-String Source/Alternative/PrisonAccess"),
			TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp FAlternativePrisonAccessAttemptResolver ResolveDoorAttempt ResolveTechnicalBoxAttempt\nSource/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp UAlternativePrisonAccessEventSubsystem RecordEvent FAlternativePrisonAccessEventRecord PrisonAccessEvent order="),
			RunId,
			TimestampPrefix + TEXT("03Z")));
	}

	FUnrealClaudeActivePlan MakePacket662ExternalRuntimeProofPlan(const FString& Suffix)
	{
		FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
		Plan.PlanId = TEXT("plan_packet662_external_runtime_proof_") + Suffix;
		Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_packet662_external_runtime_proof_") + Suffix;
		Plan.OriginalUserTask =
			TEXT("Final integrated E2E acceptance. Use prefix packet659_global_e2e_v2.");
		Plan.CreatedAtUtc = TEXT("2026-04-27T16:44:39Z");
		Plan.FeatureWorkflow.CurrentPhase = TEXT("interaction_contract");
		Plan.FeatureWorkflow.TerminalStatus = TEXT("stop_loss");
		Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");
		Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("missing");
		Plan.FeatureWorkflow.bStopLossTriggered = true;
		Plan.FeatureWorkflow.StopLossReason =
			TEXT("command_execution_without_phase_advance_gt_5:proof_prerequisites_missing");
		Plan.FeatureWorkflow.BlockerFamily = TEXT("proof_prerequisites_missing");
		Plan.FeatureWorkflow.BlockerDetail =
			TEXT("known_proof_map=false; placed_runtime_actors=false; automation_discovery_count=-1");
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance = 6;
		Plan.FeatureWorkflow.CompletedPhaseIds = { TEXT("project_context_preflight") };
		Plan.FeatureWorkflow.FailedPhaseIds.Reset();
		Plan.FeatureWorkflow.bKnownProofMapAvailable = false;
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = false;
		Plan.FeatureWorkflow.AutomationDiscoveryCount = INDEX_NONE;
		Plan.FeatureWorkflow.AutomationExecutedCount = INDEX_NONE;
		Plan.FeatureWorkflow.AutomationPassedCount = INDEX_NONE;
		Plan.FeatureWorkflow.AutomationFailedCount = INDEX_NONE;
		return Plan;
	}

	FString MakePacket662ExternalRuntimeProofProjectRoot(const FString& Suffix)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("UnrealClaude"),
				TEXT("Packet662ExternalRuntimeProofLogs"),
				Suffix));
	}

	FString MakePacket662ExternalRuntimeProofLog(
		const int32 SuccessLineCount = 7,
		const bool bIncludeProofFixtureSmokeSuccess = true,
		const int32 PrisonAccessEventCount = 2,
		const int32 ExitCode = 0)
	{
		FString LogText =
			TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.AuthorizedRoute\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.ForceRoute\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.InputPathTruth\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.ProofFixturePresence\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.ProofFixtureSmoke\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.TechnicalRoute\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.UnskilledRoute\n");

		const TArray<FString> TestNames = {
			TEXT("AuthorizedRoute"),
			TEXT("ForceRoute"),
			TEXT("InputPathTruth"),
			TEXT("ProofFixturePresence"),
			TEXT("ProofFixtureSmoke"),
			TEXT("TechnicalRoute"),
			TEXT("UnskilledRoute")
		};

		int32 AddedSuccessLines = 0;
		for (const FString& TestName : TestNames)
		{
			if (TestName == TEXT("ProofFixtureSmoke") && !bIncludeProofFixtureSmokeSuccess)
			{
				continue;
			}
			if (AddedSuccessLines >= SuccessLineCount)
			{
				break;
			}
			LogText += FString::Printf(
				TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={%s} Path={Alternative.PrisonAccess.%s}\n"),
				*TestName,
				*TestName);
			++AddedSuccessLines;
		}
		while (AddedSuccessLines < SuccessLineCount)
		{
			LogText += TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={AuthorizedRoute} Path={Alternative.PrisonAccess.AuthorizedRoute}\n");
			++AddedSuccessLines;
		}

		for (int32 EventIndex = 1; EventIndex <= PrisonAccessEventCount; ++EventIndex)
		{
			LogText += FString::Printf(
				TEXT("LogAlternative: Display: PrisonAccessEvent order=%d actor=subject.player.technician target=ProofActor attempt=%d result=3 noisy=false tag=attempt.access.proof\n"),
				EventIndex,
				EventIndex - 1);
		}

		LogText += FString::Printf(
			TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: %d ****"),
			ExitCode);
		return LogText;
	}

	FString WritePacket662ExternalRuntimeProofLogFixture(
		const FString& ProjectRoot,
		const FString& FileName,
		const FString& Content,
		const FDateTime& Timestamp)
	{
		const FString LogDir = FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("Logs"));
		IFileManager::Get().MakeDirectory(*LogDir, true);
		const FString ProofLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(LogDir, FileName));
		FFileHelper::SaveStringToFile(Content, *ProofLogPath);
		IFileManager::Get().SetTimeStamp(*ProofLogPath, Timestamp);
		return ProofLogPath;
	}

	void WritePacket662ProjectSourceFixture(const FString& ProjectRoot)
	{
		const FString SourceDir =
			FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("Alternative"), TEXT("PrisonAccess"));
		IFileManager::Get().MakeDirectory(*SourceDir, true);

		FFileHelper::SaveStringToFile(
			TEXT("FAlternativePrisonAccessAttemptResolver\n")
			TEXT("ResolveDoorAttempt\n")
			TEXT("ResolveTechnicalBoxAttempt\n")
			TEXT("FAlternativePrisonAccessAttemptOutcome\n"),
			*FPaths::Combine(SourceDir, TEXT("AlternativePrisonAccessAttemptResolver.cpp")));
		FFileHelper::SaveStringToFile(
			TEXT("UAlternativePrisonAccessEventSubsystem\n")
			TEXT("FAlternativePrisonAccessEventRecord\n")
			TEXT("RecordEvent\n")
			TEXT("PrisonAccessEvent order=\n"),
			*FPaths::Combine(SourceDir, TEXT("AlternativePrisonAccessEventSubsystem.cpp")));
	}

	bool HasExternalRuntimeProofLogFact(const FUnrealClaudeCloseoutFactSnapshot& Facts)
	{
		for (const FString& FactId : Facts.ConsumedFactIds)
		{
			if (FactId.Contains(TEXT("external_runtime_proof_log."), ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_CppMutationWithoutCompileProofNotAchieved,
	"UnrealClaude.ActivePlanCloseout.CppMutationWithoutCompileProofNotAchieved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_CompileFailureAfterLatestMutationNotAchieved,
	"UnrealClaude.ActivePlanCloseout.CompileFailureAfterLatestMutationNotAchieved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_CompileSuccessWithoutVerificationStaysPartial,
	"UnrealClaude.ActivePlanCloseout.CompileSuccessWithoutVerificationStaysPartial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_CompileSuccessWithVerificationCanBeFull,
	"UnrealClaude.ActivePlanCloseout.CompileSuccessWithVerificationCanBeFull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_NonCppTasksRemainFullOnSuccess,
	"UnrealClaude.ActivePlanCloseout.NonCppTasksRemainFullOnSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_StaleCompileProofAfterLaterMutationNotAchieved,
	"UnrealClaude.ActivePlanCloseout.StaleCompileProofAfterLaterMutationNotAchieved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_MissingCompileWarningShownEvenForPartialResponse,
	"UnrealClaude.ActivePlanCloseout.MissingCompileWarningShownEvenForPartialResponse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_VerificationFailureAfterCompileNotAchieved,
	"UnrealClaude.ActivePlanCloseout.VerificationFailureAfterCompileNotAchieved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowStopLossNotAchieved,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowStopLossNotAchieved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowRuntimeProofMissingStaysPartial,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowRuntimeProofMissingStaysPartial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ManualAssetDependencyBlockerIsPrecise,
	"UnrealClaude.ActivePlanCloseout.ManualAssetDependencyBlockerIsPrecise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_BlockedOnToolSurfaceIsPrecise,
	"UnrealClaude.ActivePlanCloseout.BlockedOnToolSurfaceIsPrecise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_MechanicInputConflictIsPrecise,
	"UnrealClaude.ActivePlanCloseout.MechanicInputConflictIsPrecise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowCompileAndRuntimeCanBeFull,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowCompileAndRuntimeCanBeFull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowAuthoringLaneDeniedNotAchieved,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowAuthoringLaneDeniedNotAchieved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowAutomationDiscoveryZeroNotAchieved,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowAutomationDiscoveryZeroNotAchieved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowAutomationExecutionSatisfiedCanBeFull,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowAutomationExecutionSatisfiedCanBeFull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowVerificationOnlyAutomationAndSmokeCanBeFull,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowVerificationOnlyAutomationAndSmokeCanBeFull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_InteractionAccessRuntimeProofRequiresAttemptResolverObservation,
	"UnrealClaude.ActivePlanCloseout.InteractionAccessRuntimeProofRequiresAttemptResolverObservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_Review10PluginManagedArchiveRequiresAllEvidence,
	"UnrealClaude.ActivePlanCloseout.Review10PluginManagedArchiveRequiresAllEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowProofPrerequisitesMissingNotAchieved,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowProofPrerequisitesMissingNotAchieved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowStopLossRewritesFalseFullCloseout,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowStopLossRewritesFalseFullCloseout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowStopLossRewritesBulletStatusFullCloseout,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowStopLossRewritesBulletStatusFullCloseout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FeatureWorkflowRuntimeProofMissingRewritesFalseFullCloseout,
	"UnrealClaude.ActivePlanCloseout.FeatureWorkflowRuntimeProofMissingRewritesFalseFullCloseout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicJudgeFactsOverrideStaleProofPrerequisiteStopLoss,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.FactsOverrideStaleProofPrerequisiteStopLoss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicJudgeDocsProseAloneDoNotCompleteRuntimeProof,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.DocsProseAloneDoNotCompleteRuntimeProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicJudgeMissingAutomationRuntimeProofStaysNotFull,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.MissingAutomationRuntimeProofStaysNotFull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicJudgeArbitraryShellDriftStillStopLosses,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ArbitraryShellDriftStillStopLosses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicAgentTraceFactExtractorConsumesTypedProofFacts,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.AgentTraceFactExtractorConsumesTypedProofFacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicManagedStateManualWriteRejectsAcceptance,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ManagedStateManualWriteRejectsAcceptance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExecutionTruthManagedStateCommandRejectsWithoutLegacyMutationFlag,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExecutionTruthManagedStateCommandRejectsWithoutLegacyMutationFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicReadOnlyTraceAuditDoesNotTriggerManagedStateWrite,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ReadOnlyTraceAuditDoesNotTriggerManagedStateWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExternalRuntimeProofLogCurrentPrefixRecoversStopLoss,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExternalRuntimeProofLogCurrentPrefixRecoversStopLoss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExternalRuntimeProofLogStaleOrWrongPrefixRejected,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExternalRuntimeProofLogStaleOrWrongPrefixRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExternalRuntimeProofLogMalformedRejected,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExternalRuntimeProofLogMalformedRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExternalRuntimeProofRetryRecoversSameIdentity,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExternalRuntimeProofRetryRecoversSameIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExternalRuntimeProofLaterPlanRejectsOlderValidLog,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExternalRuntimeProofLaterPlanRejectsOlderValidLog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExternalRuntimeProofStopLossPlanCanResumeSameWorkflow,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExternalRuntimeProofStopLossPlanCanResumeSameWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExternalRuntimeProofAdHocStopLossRecoversCurrentPrefix,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExternalRuntimeProofAdHocStopLossRecoversCurrentPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExternalRuntimeProofLogReadsProjectSourceForCurrentPrefix,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExternalRuntimeProofLogReadsProjectSourceForCurrentPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_AttemptResolverStopLossRecoversFromCurrentPrefixProof,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.AttemptResolverStopLossRecoversFromCurrentPrefixProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ContinuitySchemaLabelsDoNotBecomeRefs,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ContinuitySchemaLabelsDoNotBecomeRefs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicCleanPluginOwnedCloseoutAccepted,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.CleanPluginOwnedCloseoutAccepted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_RecipeRegistryUnknownRecipeFailsClosed,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RecipeRegistryUnknownRecipeFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_RecipeRegistryMissingObligationNamesMissingFact,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RecipeRegistryMissingObligationNamesMissingFact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_RecipeRegistryDecisionJsonSerializesContract,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RecipeRegistryDecisionJsonSerializesContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_RecipeRegistryOldRunEvidenceDoesNotSatisfyCurrentRecipe,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RecipeRegistryOldRunEvidenceDoesNotSatisfyCurrentRecipe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_RoleContractsUnknownRoleFailsClosed,
	"UnrealClaude.ActivePlanCloseout.RoleContracts.UnknownRoleFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_RoleContractsArchitectCannotCloseRuntimeProof,
	"UnrealClaude.ActivePlanCloseout.RoleContracts.ArchitectCannotCloseRuntimeProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_RoleContractsWorkerManagedStateWriteStillRejectsAcceptance,
	"UnrealClaude.ActivePlanCloseout.RoleContracts.WorkerManagedStateWriteStillRejectsAcceptance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_RoleContractsVerifierDecisionJsonCitesCloseoutContract,
	"UnrealClaude.ActivePlanCloseout.RoleContracts.VerifierDecisionJsonCitesCloseoutContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicRunScopedOldTraceManualWriteDoesNotPoisonCleanRun,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RunScopedOldTraceManualWriteDoesNotPoisonCleanRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicRunScopedCurrentRunManualWriteRejectsAcceptance,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RunScopedCurrentRunManualWriteRejectsAcceptance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicRunScopedDecisionJsonCarriesRunPlanIdentity,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RunScopedDecisionJsonCarriesRunPlanIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicCleanPostProofMemoryUpdateCommandDriftLatchesAchieved,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.CleanPostProofMemoryUpdateCommandDriftLatchesAchieved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicPostProofMemoryUpdateCommandDriftWithoutRuntimeProofStillFails,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.PostProofMemoryUpdateCommandDriftWithoutRuntimeProofStillFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicJudgeMissingProofInputMappingRejectsRuntimeProof,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.MissingProofInputMappingRejectsRuntimeProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicJudgePayloadJsonEvidenceConsumesProofInputMapping,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.PayloadJsonEvidenceConsumesProofInputMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_SystemicCloseoutDecisionIdentityIsNonContradictoryAcrossRepeatedBuilds,
	"UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.CloseoutDecisionIdentityIsNonContradictoryAcrossRepeatedBuilds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_CppMutationWithoutCompileProofNotAchieved::RunTest(const FString& Parameters)
{
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(MakeCppMutationPlan(), true);

	TestTrue(TEXT("compile proof should be required"), Decision.bCompileProofRequired);
	TestEqual(TEXT("plan status should be failed"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("result status should be not_achieved"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(
		TEXT("gate reason should surface missing compile truth"),
		Decision.GateReasonCode,
		FString(TEXT("missing_compile_truth_after_cpp_mutation")));
	return true;
}

bool FActivePlanCloseout_CompileFailureAfterLatestMutationNotAchieved::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeCppMutationPlan();
	Plan.CompileProof.LastCompileProofAtUtc = TEXT("2026-04-22T08:46:00Z");
	Plan.CompileProof.LastCompileProofToolCallId = TEXT("tool_call_compile");
	Plan.CompileProof.LastCompileProofToolName = TEXT("livecoding_compile");
	Plan.CompileProof.LastCompileProofOutcome = TEXT("failed");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("fresh compile failure should be observed"), Decision.bFreshCompileFailureObserved);
	TestEqual(TEXT("plan status should be failed"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("result status should be not_achieved"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(
		TEXT("gate reason should surface compile failure"),
		Decision.GateReasonCode,
		FString(TEXT("compile_failed_after_cpp_mutation")));
	return true;
}

bool FActivePlanCloseout_CompileSuccessWithoutVerificationStaysPartial::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeCppMutationPlan();
	Plan.CompileProof.LastCompileProofAtUtc = TEXT("2026-04-22T08:46:00Z");
	Plan.CompileProof.LastCompileProofToolCallId = TEXT("tool_call_compile");
	Plan.CompileProof.LastCompileProofToolName = TEXT("livecoding_compile");
	Plan.CompileProof.LastCompileProofOutcome = TEXT("success");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("fresh compile proof should be observed"), Decision.bFreshCompileProofObserved);
	TestEqual(TEXT("plan status should remain done"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("result status should remain partial"), Decision.ResultStatus, FString(TEXT("achieved_partially")));
	TestEqual(
		TEXT("gate reason should surface missing post-compile verification"),
		Decision.GateReasonCode,
		FString(TEXT("compile_succeeded_without_post_compile_verification")));
	return true;
}

bool FActivePlanCloseout_CompileSuccessWithVerificationCanBeFull::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeCppMutationPlan();
	Plan.CompileProof.LastCompileProofAtUtc = TEXT("2026-04-22T08:46:00Z");
	Plan.CompileProof.LastCompileProofToolCallId = TEXT("tool_call_compile");
	Plan.CompileProof.LastCompileProofToolName = TEXT("livecoding_compile");
	Plan.CompileProof.LastCompileProofOutcome = TEXT("success");
	Plan.CompileProof.LastPostCompileVerificationAtUtc = TEXT("2026-04-22T08:47:00Z");
	Plan.CompileProof.LastPostCompileVerificationToolCallId = TEXT("tool_call_verify");
	Plan.CompileProof.LastPostCompileVerificationToolName = TEXT("execute_script");
	Plan.CompileProof.LastPostCompileVerificationOutcome = TEXT("pass");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("post-compile verification pass should be observed"), Decision.bPostCompileVerificationPassed);
	TestEqual(TEXT("plan status should be done"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("result status should be achieved_fully"), Decision.ResultStatus, FString(TEXT("achieved_fully")));
	TestTrue(TEXT("gate reason should be empty once verification passed"), Decision.GateReasonCode.IsEmpty());
	return true;
}

bool FActivePlanCloseout_NonCppTasksRemainFullOnSuccess::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_non_cpp");
	Plan.Status = TEXT("active");
	Plan.ResultStatus = TEXT("incomplete");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestFalse(TEXT("compile proof should not be required"), Decision.bCompileProofRequired);
	TestEqual(TEXT("plan status should be done"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("result status should be achieved_fully"), Decision.ResultStatus, FString(TEXT("achieved_fully")));
	TestTrue(TEXT("gate reason should stay empty"), Decision.GateReasonCode.IsEmpty());
	return true;
}

bool FActivePlanCloseout_StaleCompileProofAfterLaterMutationNotAchieved::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeCppMutationPlan();
	Plan.CompileProof.LastCompileProofAtUtc = TEXT("2026-04-22T08:44:59Z");
	Plan.CompileProof.LastCompileProofToolCallId = TEXT("tool_call_compile_old");
	Plan.CompileProof.LastCompileProofToolName = TEXT("livecoding_compile");
	Plan.CompileProof.LastCompileProofOutcome = TEXT("success");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestEqual(TEXT("stale compile proof must fail closeout"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("stale compile proof must not achieve"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(
		TEXT("stale compile proof should map to missing compile truth"),
		Decision.GateReasonCode,
		FString(TEXT("missing_compile_truth_after_cpp_mutation")));
	return true;
}

bool FActivePlanCloseout_MissingCompileWarningShownEvenForPartialResponse::RunTest(const FString& Parameters)
{
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(MakeCppMutationPlan(), true);
	const FString Warning = SClaudeEditorWidget::BuildActivePlanCloseoutWarningText(
		Decision,
		TEXT("Status: partial. I changed the code but did not rerun compile."));

	TestTrue(TEXT("warning should be surfaced for missing compile truth"), !Warning.IsEmpty());
	TestTrue(TEXT("warning should mention fresh compile proof"), Warning.Contains(TEXT("fresh compile proof")));
	return true;
}

bool FActivePlanCloseout_VerificationFailureAfterCompileNotAchieved::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeCppMutationPlan();
	Plan.CompileProof.LastCompileProofAtUtc = TEXT("2026-04-22T08:46:00Z");
	Plan.CompileProof.LastCompileProofToolCallId = TEXT("tool_call_compile");
	Plan.CompileProof.LastCompileProofToolName = TEXT("livecoding_compile");
	Plan.CompileProof.LastCompileProofOutcome = TEXT("success");
	Plan.CompileProof.LastPostCompileVerificationAtUtc = TEXT("2026-04-22T08:47:00Z");
	Plan.CompileProof.LastPostCompileVerificationToolCallId = TEXT("tool_call_verify");
	Plan.CompileProof.LastPostCompileVerificationToolName = TEXT("execute_script");
	Plan.CompileProof.LastPostCompileVerificationOutcome = TEXT("fail");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("failed verification should be observed"), Decision.bPostCompileVerificationFailed);
	TestEqual(TEXT("plan status should be failed"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("result status should be not_achieved"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(
		TEXT("gate reason should surface verification failure"),
		Decision.GateReasonCode,
		FString(TEXT("post_compile_verification_failed_after_cpp_mutation")));
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowStopLossNotAchieved::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeFeatureWorkflowPlan();
	Plan.FeatureWorkflow.bStopLossTriggered = true;
	Plan.FeatureWorkflow.StopLossReason = TEXT("command_execution_without_phase_advance_gt_5");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("feature workflow should be marked required"), Decision.bFeatureWorkflowRequired);
	TestTrue(TEXT("stop loss should surface on decision"), Decision.bStopLossTriggered);
	TestEqual(TEXT("stop loss should fail closeout"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("stop loss should not achieve the task"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(TEXT("stop loss gate reason should be feature-specific"), Decision.GateReasonCode, FString(TEXT("feature_workflow_stop_loss_triggered")));
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowRuntimeProofMissingStaysPartial::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("feature workflow should require runtime proof"), Decision.bRuntimeProofRequired);
	TestEqual(TEXT("missing runtime proof should stay done"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("missing runtime proof should downgrade to partial"), Decision.ResultStatus, FString(TEXT("achieved_partially")));
	TestEqual(TEXT("gate reason should surface missing runtime proof"), Decision.GateReasonCode, FString(TEXT("feature_workflow_runtime_proof_missing")));
	return true;
}

bool FActivePlanCloseout_ManualAssetDependencyBlockerIsPrecise::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("blocked");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("manual_asset_dependency_blocker");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("local Marketplace/Fab animation pack is absent; code-only placeholder path remains available");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	const FUnrealClaudeUserFacingStatus Status = UnrealClaudeUserFacingStatus::BuildStatus(Plan, Decision);
	const FString Warning = SClaudeEditorWidget::BuildActivePlanCloseoutWarningText(
		Decision,
		TEXT("Status: failed\nmanual_asset_dependency_blocker: missing local Fab asset."));

	TestEqual(TEXT("asset dependency blocker should keep exact gate reason"),
		Decision.GateReasonCode,
		FString(TEXT("manual_asset_dependency_blocker")));
	TestEqual(TEXT("asset dependency blocker should not collapse to generic assistant failure"),
		Decision.BlockerFamily,
		FString(TEXT("manual_asset_dependency_blocker")));
	TestEqual(TEXT("asset dependency blocker should be user-facing manual verification"),
		Status.StatusId,
		UnrealClaudeUserFacingStatus::ManualVerificationRequiredStatusId());
	TestTrue(TEXT("warning should name external asset dependency"),
		Warning.Contains(TEXT("external asset dependency"), ESearchCase::IgnoreCase));
	return true;
}

bool FActivePlanCloseout_BlockedOnToolSurfaceIsPrecise::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("blocked");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("blocked_on_tool_surface");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("missing tools: map_runtime_proof, capture_viewport, open_level");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	const FUnrealClaudeUserFacingStatus Status = UnrealClaudeUserFacingStatus::BuildStatus(Plan, Decision);

	TestEqual(TEXT("tool-surface blocker should keep exact gate reason"),
		Decision.GateReasonCode,
		FString(TEXT("blocked_on_tool_surface")));
	TestEqual(TEXT("tool-surface blocker detail should preserve exact missing tools"),
		Decision.BlockerDetail,
		FString(TEXT("missing tools: map_runtime_proof, capture_viewport, open_level")));
	TestEqual(TEXT("tool-surface blocker should be exact cause user status"),
		Status.StatusId,
		UnrealClaudeUserFacingStatus::BlockedByExactCauseStatusId());
	TestTrue(TEXT("tool-surface user detail should name missing capture tool"),
		Status.Detail.Contains(TEXT("capture_viewport"), ESearchCase::IgnoreCase));
	return true;
}

bool FActivePlanCloseout_MechanicInputConflictIsPrecise::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("blocked");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("mechanic_input_conflict_unresolved");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("Space+Shift conflicts with existing Combat flight and sprint wallrun input owners");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	const FUnrealClaudeUserFacingStatus Status = UnrealClaudeUserFacingStatus::BuildStatus(Plan, Decision);
	const FString Warning = SClaudeEditorWidget::BuildActivePlanCloseoutWarningText(
		Decision,
		TEXT("Status: failed\nmechanic_input_conflict_unresolved: Space+Shift input conflict."));

	TestEqual(TEXT("mechanic conflict blocker should keep exact gate reason"),
		Decision.GateReasonCode,
		FString(TEXT("mechanic_input_conflict_unresolved")));
	TestEqual(TEXT("mechanic conflict blocker should not collapse to generic assistant failure"),
		Decision.BlockerFamily,
		FString(TEXT("mechanic_input_conflict_unresolved")));
	TestEqual(TEXT("mechanic conflict blocker should be exact cause user status"),
		Status.StatusId,
		UnrealClaudeUserFacingStatus::BlockedByExactCauseStatusId());
	TestTrue(TEXT("mechanic conflict warning should mention input ownership"),
		Warning.Contains(TEXT("mechanic/input conflict"), ESearchCase::IgnoreCase));
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowCompileAndRuntimeCanBeFull::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.CompileProof.LastPostCompileVerificationAtUtc = TEXT("2026-04-22T08:47:00Z");
	Plan.CompileProof.LastPostCompileVerificationToolCallId = TEXT("tool_call_verify");
	Plan.CompileProof.LastPostCompileVerificationToolName = TEXT("execute_script");
	Plan.CompileProof.LastPostCompileVerificationOutcome = TEXT("pass");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("runtime proof pass should surface"), Decision.bRuntimeProofPassed);
	TestEqual(TEXT("compile + runtime proof should allow done"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("compile + runtime proof should allow achieved_fully"), Decision.ResultStatus, FString(TEXT("achieved_fully")));
	TestTrue(TEXT("feature workflow gate reason should be empty when fully satisfied"), Decision.GateReasonCode.IsEmpty());
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowAuthoringLaneDeniedNotAchieved::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeInteractionAccessFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.FeatureWorkflow.AuthoringLaneState = TEXT("denied");
	Plan.FeatureWorkflow.AuthoringPolicyRuleId = TEXT("workspace_write_project.broad_authoring_mutation_surface_denied");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("authoring_lane_denied");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("workspace_write_project.broad_authoring_mutation_surface_denied");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("authoring-lane denial should surface"), Decision.bAuthoringLaneDenied);
	TestEqual(TEXT("authoring-lane denial should fail closeout"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("authoring-lane denial should not achieve the task"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(TEXT("authoring-lane denial should map to a feature-specific gate"), Decision.GateReasonCode, FString(TEXT("feature_workflow_authoring_lane_denied")));
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowAutomationDiscoveryZeroNotAchieved::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeInteractionAccessFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.FeatureWorkflow.CurrentPhase = TEXT("automation_discovery_gate");
	Plan.FeatureWorkflow.AutomationDiscoveryCommand = TEXT("Automation RunTests Alternative.PrisonAccess");
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 0;
	Plan.FeatureWorkflow.AutomationExecutedCount = 0;
	Plan.FeatureWorkflow.BlockerFamily = TEXT("automation_discovery_failed");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("discoverable_automation_tests_eq_0");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("zero-test automation should surface as discovery failure"), Decision.bAutomationDiscoveryFailed);
	TestEqual(TEXT("zero-test automation should fail closeout"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("zero-test automation should not achieve the task"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(TEXT("zero-test automation should map to the automation discovery gate"), Decision.GateReasonCode, FString(TEXT("feature_workflow_automation_discovery_failed")));
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowAutomationExecutionSatisfiedCanBeFull::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeInteractionAccessFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.CompileProof.LastPostCompileVerificationAtUtc = TEXT("2026-04-22T08:47:00Z");
	Plan.CompileProof.LastPostCompileVerificationToolCallId = TEXT("tool_call_runtime_proof");
	Plan.CompileProof.LastPostCompileVerificationToolName = TEXT("execute_terminal");
	Plan.CompileProof.LastPostCompileVerificationOutcome = TEXT("pass");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.AutomationDiscoveryCommand = TEXT("Automation RunTests Alternative.PrisonAccess");
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 7;
	Plan.FeatureWorkflow.AutomationPassedCount = 7;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	PrimeProofInputMappingObservation(Plan.FeatureWorkflow);
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = true;
	Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("automation_discovery_gate"));

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestFalse(TEXT("positive automation execution should not surface a discovery failure"), Decision.bAutomationDiscoveryFailed);
	TestEqual(TEXT("truthful automation execution with runtime proof should allow done"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("truthful automation execution with runtime proof should allow achieved_fully"), Decision.ResultStatus, FString(TEXT("achieved_fully")));
	TestTrue(TEXT("no automation discovery gate reason should remain"), Decision.GateReasonCode.IsEmpty());
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowVerificationOnlyAutomationAndSmokeCanBeFull::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.AutomationDiscoveryCommand = TEXT("Automation RunTests Alternative.PrisonAccess");
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 7;
	Plan.FeatureWorkflow.AutomationPassedCount = 7;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	PrimeProofInputMappingObservation(Plan.FeatureWorkflow);
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = true;
	Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("compile_gate"));
	Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("automation_discovery_gate"));

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestFalse(TEXT("verification-only rerun should not require compile proof"), Decision.bCompileProofRequired);
	TestFalse(TEXT("verification-only automation truth should not surface discovery failure"), Decision.bAutomationDiscoveryFailed);
	TestEqual(TEXT("verification-only automation plus smoke should allow done"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("verification-only automation plus smoke should allow achieved_fully"), Decision.ResultStatus, FString(TEXT("achieved_fully")));
	TestTrue(TEXT("verification-only positive closeout should keep gate reason empty"), Decision.GateReasonCode.IsEmpty());
	return true;
}

bool FActivePlanCloseout_InteractionAccessRuntimeProofRequiresAttemptResolverObservation::RunTest(
	const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.AutomationDiscoveryCommand = TEXT("Automation RunTests Alternative.PrisonAccess");
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 7;
	Plan.FeatureWorkflow.AutomationPassedCount = 7;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	PrimeProofInputMappingObservation(Plan.FeatureWorkflow);
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("compile_gate"));
	Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("automation_discovery_gate"));

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestEqual(TEXT("runtime proof without attempt resolver observation should fail closeout"),
		Decision.PlanStatus,
		FString(TEXT("failed")));
	TestEqual(TEXT("runtime proof without attempt resolver observation should not achieve"),
		Decision.ResultStatus,
		FString(TEXT("not_achieved")));
	TestEqual(TEXT("missing attempt resolver observation should have an explicit gate reason"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_attempt_resolver_observation_missing")));
	return true;
}

bool FActivePlanCloseout_Review10PluginManagedArchiveRequiresAllEvidence::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");
	Plan.FeatureWorkflow.AutomationDiscoveryCommand = TEXT("observed Alternative.PrisonAccess automation log");
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 7;
	Plan.FeatureWorkflow.AutomationPassedCount = 7;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	PrimeProofInputMappingObservation(Plan.FeatureWorkflow);
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = true;
	Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("compile_gate"));
	Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("automation_discovery_gate"));
	Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("runtime_proof"));

	const FUnrealClaudeActivePlanCloseoutDecision MissingSourceDecision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestEqual(TEXT("review10 closeout must reject runtime-only observation without source attribution"),
		MissingSourceDecision.PlanStatus,
		FString(TEXT("failed")));
	TestEqual(TEXT("review10 closeout should use the attempt-resolver observation gate"),
		MissingSourceDecision.GateReasonCode,
		FString(TEXT("feature_workflow_attempt_resolver_observation_missing")));

	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved = true;
	const FUnrealClaudeActivePlanCloseoutDecision CompleteEvidenceDecision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestEqual(TEXT("review10 closeout should accept archive with source and runtime observation"),
		CompleteEvidenceDecision.PlanStatus,
		FString(TEXT("done")));
	TestEqual(TEXT("review10 closeout with all evidence should be full"),
		CompleteEvidenceDecision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestTrue(TEXT("review10 complete evidence closeout should not keep a gate reason"),
		CompleteEvidenceDecision.GateReasonCode.IsEmpty());
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowProofPrerequisitesMissingNotAchieved::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeInteractionAccessFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("missing");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("blocked");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("proof_prerequisites_missing");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("known_proof_map=false; placed_runtime_actors=false; automation_discovery_count=-1; reduced_proof_mode_allowed=false");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestTrue(TEXT("missing proof prerequisites should surface"), Decision.bProofPrerequisitesMissing);
	TestEqual(TEXT("missing proof prerequisites should fail closeout"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("missing proof prerequisites should not achieve the task"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(TEXT("missing proof prerequisites should map to the proof gate"), Decision.GateReasonCode, FString(TEXT("feature_workflow_proof_prerequisites_missing")));
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowStopLossRewritesFalseFullCloseout::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.FeatureWorkflow.bStopLossTriggered = true;
	Plan.FeatureWorkflow.StopLossReason = TEXT("command_execution_without_phase_advance_gt_5");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("proof_prerequisites_missing");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("runtime_fixture_missing");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	const FString Rewritten = SClaudeEditorWidget::RewriteResponseForTruthfulActivePlanCloseout(
		Plan,
		Decision,
		TEXT("**Result**\n\n`full`\n\n**Exact Next Blocker If Not Complete**\n\n- None."));

	TestEqual(TEXT("stop-loss rewrite should change result to fail"), Rewritten.Contains(TEXT("`fail`")), true);
	TestEqual(TEXT("stop-loss rewrite should surface feature terminal truth"), Rewritten.Contains(TEXT("feature_workflow_terminal_status: stop_loss")), true);
	TestEqual(TEXT("stop-loss rewrite should surface gate reason"), Rewritten.Contains(TEXT("gate_reason: feature_workflow_stop_loss_triggered")), true);
	TestEqual(TEXT("stop-loss rewrite should surface blocker family"), Rewritten.Contains(TEXT("blocker_family: proof_prerequisites_missing")), true);
	TestEqual(TEXT("stop-loss rewrite should preserve runtime-proof diagnostics"), Rewritten.Contains(TEXT("runtime_proof_state: pending")), true);
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowStopLossRewritesBulletStatusFullCloseout::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.FeatureWorkflow.bStopLossTriggered = true;
	Plan.FeatureWorkflow.StopLossReason = TEXT("command_execution_without_phase_advance_gt_5");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("proof_prerequisites_missing");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("runtime_fixture_missing");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	const FString Rewritten = SClaudeEditorWidget::RewriteResponseForTruthfulActivePlanCloseout(
		Plan,
		Decision,
		TEXT("- result status: `full`\n- current phase: `memory_update`\n- exact next blocker if not complete: none"));

	TestEqual(TEXT("bullet-style full status should still be downgraded to fail"), Rewritten.Contains(TEXT("`fail`")), true);
	TestEqual(TEXT("bullet-style full status rewrite should surface feature terminal truth"), Rewritten.Contains(TEXT("feature_workflow_terminal_status: stop_loss")), true);
	TestEqual(TEXT("bullet-style full status rewrite should surface gate reason"), Rewritten.Contains(TEXT("gate_reason: feature_workflow_stop_loss_triggered")), true);
	TestEqual(TEXT("bullet-style full status rewrite should replace the original response with truthful closeout"), Rewritten.Contains(TEXT("**Truthful Closeout**")), true);
	return true;
}

bool FActivePlanCloseout_FeatureWorkflowRuntimeProofMissingRewritesFalseFullCloseout::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeFeatureWorkflowPlan();
	PrimeCompileSuccess(Plan);
	Plan.CompileProof.LastPostCompileVerificationAtUtc = TEXT("2026-04-22T08:47:00Z");
	Plan.CompileProof.LastPostCompileVerificationToolCallId = TEXT("tool_call_verify");
	Plan.CompileProof.LastPostCompileVerificationToolName = TEXT("execute_script");
	Plan.CompileProof.LastPostCompileVerificationOutcome = TEXT("pass");

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	const FString Rewritten = SClaudeEditorWidget::RewriteResponseForTruthfulActivePlanCloseout(
		Plan,
		Decision,
		TEXT("Status: full\n\nRuntime smoke was enough."));

	TestEqual(TEXT("runtime-proof-missing rewrite should downgrade to partial"), Rewritten.Contains(TEXT("`partial`")), true);
	TestEqual(TEXT("runtime-proof-missing rewrite should surface gate reason"), Rewritten.Contains(TEXT("gate_reason: feature_workflow_runtime_proof_missing")), true);
	TestEqual(TEXT("runtime-proof-missing rewrite should keep runtime proof pending"), Rewritten.Contains(TEXT("runtime proof: pending")), true);
	TestEqual(TEXT("runtime-proof-missing rewrite should keep compile proof passed"), Rewritten.Contains(TEXT("compile proof: passed")), true);
	return true;
}

bool FActivePlanCloseout_SystemicJudgeFactsOverrideStaleProofPrerequisiteStopLoss::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("missing");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("blocked");
	Plan.FeatureWorkflow.bStopLossTriggered = true;
	Plan.FeatureWorkflow.StopLossReason =
		TEXT("command_execution_without_phase_advance_gt_5:proof_prerequisites_missing");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("proof_prerequisites_missing");
	Plan.FeatureWorkflow.BlockerDetail =
		TEXT("known_proof_map=false; placed_runtime_actors=false; automation_discovery_count=-1");

	FUnrealClaudeCloseoutFactSnapshot Facts;
	PrimeBoundedInteractionAccessCloseoutFacts(Facts);
	const FAgentFeatureWorkflowState ReducedWorkflow =
		SClaudeEditorWidget::ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);

	TestTrue(TEXT("closeout reducer should restore known proof map from typed facts"),
		ReducedWorkflow.bKnownProofMapAvailable);
	TestTrue(TEXT("closeout reducer should restore placed runtime actors from typed facts"),
		ReducedWorkflow.bPlacedRuntimeActorsAvailable);
	TestEqual(TEXT("closeout reducer should restore automation discovery count"),
		ReducedWorkflow.AutomationDiscoveryCount,
		7);
	TestEqual(TEXT("closeout reducer should pass runtime proof"),
		ReducedWorkflow.RuntimeProofState,
		FString(TEXT("passed")));
	TestEqual(TEXT("closeout reducer should advance to memory_update"),
		ReducedWorkflow.CurrentPhase,
		FString(TEXT("memory_update")));
	TestFalse(TEXT("bounded facts should clear stale proof-prerequisite stop-loss"),
		ReducedWorkflow.bStopLossTriggered);
	TestEqual(TEXT("bounded facts should let judge close full"),
		Decision.PlanStatus,
		FString(TEXT("done")));
	TestEqual(TEXT("bounded facts should let judge achieve fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestFalse(TEXT("bounded facts should not surface proof prerequisite missing"),
		Decision.bProofPrerequisitesMissing);
	TestTrue(TEXT("judge should record consumed fact ids"),
		Decision.ConsumedFactIds.Contains(TEXT("test.bounded_automation")));
	return true;
}

bool FActivePlanCloseout_SystemicJudgeDocsProseAloneDoNotCompleteRuntimeProof::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("pending");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");

	FUnrealClaudeCloseoutFactSnapshot Facts;
	Facts.AddFactId(TEXT("test.docs_memory_prose_only"));
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);

	TestEqual(TEXT("docs/prose alone should not be full"),
		Decision.ResultStatus,
		FString(TEXT("achieved_partially")));
	TestEqual(TEXT("docs/prose alone should keep runtime-proof missing gate"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_runtime_proof_missing")));
	TestFalse(TEXT("docs/prose alone should not pass runtime proof"),
		Decision.bRuntimeProofPassed);
	return true;
}

bool FActivePlanCloseout_SystemicJudgeMissingAutomationRuntimeProofStaysNotFull::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("missing");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("blocked");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("proof_prerequisites_missing");

	FUnrealClaudeCloseoutFactSnapshot Facts;
	Facts.bKnownProofMapAvailable = true;
	Facts.bProofInputMappingAvailable = true;
	Facts.bPlacedRuntimeActorsAvailable = true;
	Facts.bAttemptResolverSourceObserved = true;
	Facts.bEventSubsystemSourceObserved = true;
	Facts.AddFactId(TEXT("test.prereqs_without_runtime_proof"));

	const FAgentFeatureWorkflowState ReducedWorkflow =
		SClaudeEditorWidget::ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);

	TestEqual(TEXT("prereq facts should repair proof prerequisite state"),
		ReducedWorkflow.ProofPrerequisiteState,
		FString(TEXT("satisfied")));
	TestFalse(TEXT("missing automation/runtime proof should not pass runtime proof"),
		ReducedWorkflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("missing automation/runtime proof should stay not full"),
		Decision.ResultStatus,
		FString(TEXT("achieved_partially")));
	TestEqual(TEXT("missing automation/runtime proof should map to runtime proof missing"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_runtime_proof_missing")));
	return true;
}

bool FActivePlanCloseout_SystemicJudgeArbitraryShellDriftStillStopLosses::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");
	Plan.FeatureWorkflow.bStopLossTriggered = true;
	Plan.FeatureWorkflow.StopLossReason = TEXT("command_execution_without_phase_advance_gt_5");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("command_drift");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("unrelated shell commands");

	FUnrealClaudeCloseoutFactSnapshot Facts;
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);

	TestTrue(TEXT("arbitrary shell drift should keep stop-loss"),
		Decision.bStopLossTriggered);
	TestEqual(TEXT("arbitrary shell drift should fail closeout"),
		Decision.PlanStatus,
		FString(TEXT("failed")));
	TestEqual(TEXT("arbitrary shell drift should keep stop-loss gate"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_stop_loss_triggered")));
	return true;
}

bool FActivePlanCloseout_SystemicAgentTraceFactExtractorConsumesTypedProofFacts::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonObject>> Records;
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_open_level"),
		TEXT("unrealclaude/open_level"),
		TEXT("{\"level\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}"),
		TEXT("Opened /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_input_mapping"),
		TEXT("unrealclaude/enhanced_input"),
		TEXT("{\"operation\":\"query_context\",\"context\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof\"}"),
		TEXT("IMC_PrisonAccessProof contains IA_Interact input mapping; read_only_enhanced_input_query_observed=true")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_get_actors"),
		TEXT("unrealclaude/get_level_actors"),
		TEXT("{}"),
		TEXT("BP_PrisonAccessProof placed in Lvl_PrisonAccessProof")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_source_attempt"),
		TEXT("command_execution"),
		TEXT("Select-String Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp"),
		TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp: FAlternativePrisonAccessAttemptResolver ResolveDoorAttempt FAlternativePrisonAccessAttemptOutcome")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_source_event"),
		TEXT("command_execution"),
		TEXT("Select-String Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp"),
		TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp: UAlternativePrisonAccessEventSubsystem RecordEvent FAlternativePrisonAccessEventRecord PrisonAccessEvent order=")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_automation"),
		TEXT("command_execution"),
		TEXT("Automation RunTests Alternative.PrisonAccess"),
		TEXT("Found 7 automation tests based on 'Alternative.PrisonAccess'\nProofFixtureSmoke Result={Success}\nTechnicalRoute Result={Success}\nUnskilledRoute Result={Success}\nAccessDenied Result={Success}\nAccessGranted Result={Success}\nFixtureState Result={Success}\nRegressionGuard Result={Success}\nPrisonAccessEvent order=1 tag=Attempt.Access.Granted\nPrisonAccessEvent order=2 tag=Attempt.Technical.Success\n**** TEST COMPLETE. EXIT CODE: 0 ****")));

	const FUnrealClaudeCloseoutFactSnapshot Facts =
		SClaudeEditorWidget::ExtractAgentTraceFactsForCloseout(Records);
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);
	FUnrealClaudeActivePlanCloseoutDecision DecisionWithArchive = Decision;
	DecisionWithArchive.SourceArchivePath =
		TEXT("D:/VibeCode/Unreal/Alternative/Alternative/Saved/UnrealClaude/PlanArchives/test.active_plan.json");
	const TSharedPtr<FJsonObject> DecisionJson =
		SClaudeEditorWidget::BuildCloseoutDecisionJson(Plan, DecisionWithArchive, Facts);

	FString JsonPlanId;
	DecisionJson->TryGetStringField(TEXT("source_plan_id"), JsonPlanId);
	FString JsonArchivePath;
	DecisionJson->TryGetStringField(TEXT("source_archive_path"), JsonArchivePath);

	TestTrue(TEXT("fact extractor should consume proof-map fact"),
		Facts.bKnownProofMapAvailable);
	TestTrue(TEXT("fact extractor should consume proof input mapping fact"),
		Facts.bProofInputMappingAvailable);
	TestTrue(TEXT("fact extractor should consume placed-runtime-actors fact"),
		Facts.bPlacedRuntimeActorsAvailable);
	TestTrue(TEXT("fact extractor should consume complete observation"),
		Facts.HasCompleteInteractionAccessObservation());
	TestTrue(TEXT("fact extractor should consume bounded automation proof"),
		Facts.HasBoundedPrisonAccessAutomationProof());
	TestEqual(TEXT("trace facts should let judge close full"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("closeout decision json should include source plan id"),
		JsonPlanId,
		Plan.PlanId);
	TestEqual(TEXT("closeout decision json should include archive path"),
		JsonArchivePath,
		DecisionWithArchive.SourceArchivePath);
	TestTrue(TEXT("closeout decision json should include facts"),
		DecisionJson->HasTypedField<EJson::Object>(TEXT("facts_consumed")));
	return true;
}

bool FActivePlanCloseout_SystemicManagedStateManualWriteRejectsAcceptance::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");

	TArray<TSharedPtr<FJsonObject>> Records;
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_open_level_clean"),
		TEXT("unrealclaude/open_level"),
		TEXT("{\"level\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}"),
		TEXT("Opened /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_input_mapping_clean"),
		TEXT("unrealclaude/enhanced_input"),
		TEXT("{\"operation\":\"query_context\",\"context\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof\"}"),
		TEXT("IMC_PrisonAccessProof contains IA_Interact input mapping; read_only_enhanced_input_query_observed=true")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_get_actors_clean"),
		TEXT("unrealclaude/get_level_actors"),
		TEXT("{}"),
		TEXT("BP_PrisonAccessProof placed in Lvl_PrisonAccessProof")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_sources_clean"),
		TEXT("command_execution"),
		TEXT("Select-String Source/Alternative/PrisonAccess"),
		TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp FAlternativePrisonAccessAttemptResolver ResolveDoorAttempt\nSource/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp UAlternativePrisonAccessEventSubsystem RecordEvent FAlternativePrisonAccessEventRecord PrisonAccessEvent order=")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_automation_clean"),
		TEXT("command_execution"),
		TEXT("Automation RunTests Alternative.PrisonAccess"),
		TEXT("Found 7 automation tests based on 'Alternative.PrisonAccess'\nProofFixtureSmoke Result={Success}\nAuthorizedRoute Result={Success}\nForceRoute Result={Success}\nInputPathTruth Result={Success}\nProofFixturePresence Result={Success}\nTechnicalRoute Result={Success}\nUnskilledRoute Result={Success}\nPrisonAccessEvent order=1 tag=attempt.technical.override_success\nPrisonAccessEvent order=2 tag=attempt.access.open_existing\n**** TEST COMPLETE. EXIT CODE: 0 ****")));
	Records.Add(MakeCloseoutTraceToolUse(
		TEXT("trace_manual_state_write"),
		TEXT("command_execution"),
		TEXT("Set-Content Saved/UnrealClaude/active_plan.json; Copy-Item Saved/UnrealClaude/active_plan.json Saved/UnrealClaude/PlanArchives/manual.active_plan.json; Set-Content Saved/UnrealClaude/CloseoutDecisions/manual.closeout_decision.json; Set-Content Saved/UnrealClaude/visible_session_codex_cli.json"),
		true,
		TEXT("workspace_file_build")));

	const FUnrealClaudeCloseoutFactSnapshot Facts =
		SClaudeEditorWidget::ExtractAgentTraceFactsForCloseout(Records);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);
	const TSharedPtr<FJsonObject> DecisionJson =
		SClaudeEditorWidget::BuildCloseoutDecisionJson(Plan, Decision, Facts);
	const TSharedPtr<FJsonObject>* FactsJson = nullptr;
	DecisionJson->TryGetObjectField(TEXT("facts_consumed"), FactsJson);

	TestTrue(TEXT("manual managed-state write should be detected"),
		Facts.bManagedStateManualWriteDetected);
	TestTrue(TEXT("manual managed-state write should preserve runtime facts"),
		Facts.HasBoundedInteractionAccessRuntimeProof());
	TestEqual(TEXT("manual managed-state write should fail closeout"),
		Decision.PlanStatus,
		FString(TEXT("failed")));
	TestEqual(TEXT("manual managed-state write should be not achieved"),
		Decision.ResultStatus,
		FString(TEXT("not_achieved")));
	TestEqual(TEXT("manual managed-state write should use integrity gate"),
		Decision.GateReasonCode,
		FString(TEXT("managed_state_manual_write_detected")));
	TestEqual(TEXT("manual managed-state write should surface blocker family"),
		Decision.BlockerFamily,
		FString(TEXT("managed_state_integrity")));
	TestTrue(TEXT("manual managed-state write should be recorded in decision"),
		Decision.bManagedStateManualWriteDetected);
	TestTrue(TEXT("decision json should expose manual managed-state write fact"),
		FactsJson && FactsJson->IsValid()
		&& (*FactsJson)->HasTypedField<EJson::Array>(TEXT("managed_state_manual_write_fact_ids")));
	return true;
}

bool FActivePlanCloseout_ExecutionTruthManagedStateCommandRejectsWithoutLegacyMutationFlag::RunTest(
	const FString& Parameters)
{
	const FString CurrentRunId = TEXT("run_packet655_command_text_managed_write");
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.PlanId = TEXT("plan_packet655_command_text_managed_write");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendCleanInteractionAccessTraceRecords(Records, CurrentRunId, TEXT("2026-04-27T15:20:"));
	Records.Add(MakeCloseoutTraceToolUse(
		TEXT("trace_command_text_state_write_no_legacy_flag"),
		TEXT("command_execution"),
		TEXT("Set-Content Saved/UnrealClaude/active_plan.json '{}'; Out-File Saved/UnrealClaude/closeout_decision.json"),
		false,
		TEXT("workspace_inspection"),
		CurrentRunId,
		TEXT("2026-04-27T15:20:05Z")));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(
			Plan,
			Records,
			CurrentRunId,
			TEXT("D:/VibeCode/Unreal/Alternative/Alternative"));
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);
	const TSharedPtr<FJsonObject> FactsJson = Context.Facts.ToJsonObject();
	const TArray<TSharedPtr<FJsonValue>>* ExecutionTruthDecisions = nullptr;
	FactsJson->TryGetArrayField(TEXT("execution_truth_decisions"), ExecutionTruthDecisions);

	bool bSawManagedStateWriteDecision = false;
	if (ExecutionTruthDecisions)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ExecutionTruthDecisions)
		{
			if (Value.IsValid()
				&& Value->AsString().Contains(TEXT("category=managed_state_write"), ESearchCase::CaseSensitive))
			{
				bSawManagedStateWriteDecision = true;
				break;
			}
		}
	}

	TestTrue(TEXT("command-text managed state write should be detected without classified_mutating_tool"),
		Context.Facts.bManagedStateManualWriteDetected);
	TestTrue(TEXT("command-text managed state write should record command mutation"),
		Context.Facts.bCommandMutationDetected);
	TestTrue(TEXT("facts should expose execution-truth decision summary"),
		bSawManagedStateWriteDecision);
	TestEqual(TEXT("command-text managed write should reject closeout"),
		Decision.ResultStatus,
		FString(TEXT("not_achieved")));
	TestEqual(TEXT("command-text managed write should use integrity gate"),
		Decision.GateReasonCode,
		FString(TEXT("managed_state_manual_write_detected")));
	return true;
}

bool FActivePlanCloseout_SystemicReadOnlyTraceAuditDoesNotTriggerManagedStateWrite::RunTest(
	const FString& Parameters)
{
	const FString CurrentRunId = TEXT("run_packet660_read_only_trace_audit");
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.PlanId = TEXT("plan_packet660_read_only_trace_audit");
	Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_packet660_read_only_trace_audit");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");

	const FString ReadOnlyAuditCommand =
		TEXT("$run = 'run_packet660_read_only_trace_audit'\n")
		TEXT("$lines = Get-Content -LiteralPath 'Saved/UnrealClaude/agent_trace.jsonl'\n")
		TEXT("$currentLines = $lines | Where-Object { $_ -match $run }\n")
		TEXT("$cmds = $currentLines | Where-Object { $_ -match 'command_execution' }\n")
		TEXT("$manualManagedWritePattern = '(?i)(Set-Content|Out-File|Add-Content|New-Item|Copy-Item|Move-Item|Remove-Item|\\\\s[12]?>>?\\\\s*)'\n")
		TEXT("$manualManagedWrites = $cmds | Where-Object { ($_ -match 'Saved[\\\\/]+UnrealClaude') -and ($_ -match $manualManagedWritePattern) }");

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendCleanInteractionAccessTraceRecords(Records, CurrentRunId, TEXT("2026-04-27T18:10:"));
	Records.Add(MakeCloseoutTraceToolUse(
		TEXT("trace_packet660_read_only_audit_use"),
		TEXT("command_execution"),
		ReadOnlyAuditCommand,
		true,
		TEXT("workspace_file_build"),
		CurrentRunId,
		TEXT("2026-04-27T18:10:05Z")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_packet660_read_only_audit_result"),
		TEXT("command_execution"),
		ReadOnlyAuditCommand,
		TEXT("{\"CommandExecutionManagedStateWriteFlagCount\":0,\"ManualShellSavedUnrealClaudeWritePatternCount\":0}"),
		CurrentRunId,
		TEXT("2026-04-27T18:10:06Z")));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(
			Plan,
			Records,
			CurrentRunId,
			TEXT("D:/VibeCode/Unreal/Alternative/Alternative"));
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);
	const TSharedPtr<FJsonObject> FactsJson = Context.Facts.ToJsonObject();
	const TArray<TSharedPtr<FJsonValue>>* ExecutionTruthDecisions = nullptr;
	FactsJson->TryGetArrayField(TEXT("execution_truth_decisions"), ExecutionTruthDecisions);

	bool bSawReadOnlyAuditDecision = false;
	bool bSawManagedStateWriteDecision = false;
	if (ExecutionTruthDecisions)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ExecutionTruthDecisions)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			const FString Summary = Value->AsString();
			if (Summary.Contains(TEXT("trace_packet660_read_only_audit"), ESearchCase::CaseSensitive)
				&& Summary.Contains(TEXT("category=read_only_inspection"), ESearchCase::CaseSensitive))
			{
				bSawReadOnlyAuditDecision = true;
			}
			if (Summary.Contains(TEXT("trace_packet660_read_only_audit"), ESearchCase::CaseSensitive)
				&& Summary.Contains(TEXT("category=managed_state_write"), ESearchCase::CaseSensitive))
			{
				bSawManagedStateWriteDecision = true;
			}
		}
	}

	TestTrue(TEXT("read-only trace audit should be classified as read-only"),
		bSawReadOnlyAuditDecision);
	TestFalse(TEXT("read-only trace audit should not be classified as managed-state write"),
		bSawManagedStateWriteDecision);
	TestFalse(TEXT("read-only trace audit should not mark managed-state manual write"),
		Context.Facts.bManagedStateManualWriteDetected);
	TestFalse(TEXT("read-only trace audit should not mark command mutation"),
		Context.Facts.bCommandMutationDetected);
	TestTrue(TEXT("read-only trace audit should preserve bounded runtime proof"),
		Context.Facts.HasBoundedInteractionAccessRuntimeProof());
	TestEqual(TEXT("read-only trace audit should allow achieved closeout"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("read-only trace audit should not keep integrity gate"),
		Decision.GateReasonCode,
		FString());
	return true;
}

bool FActivePlanCloseout_ExternalRuntimeProofLogCurrentPrefixRecoversStopLoss::RunTest(
	const FString& Parameters)
{
	const FString RunId = TEXT("run_packet662_external_runtime_proof_current");
	const FString ProjectRoot = MakePacket662ExternalRuntimeProofProjectRoot(TEXT("current"));
	FUnrealClaudeActivePlan Plan = MakePacket662ExternalRuntimeProofPlan(TEXT("current"));
	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v2_retry_ddc_automation_20260427_200925.log"),
		MakePacket662ExternalRuntimeProofLog(),
		FDateTime(2026, 4, 27, 17, 10, 0));

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendInteractionAccessSourceOnlyTraceRecords(Records, RunId, TEXT("2026-04-27T17:09:"));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(Plan, Records, RunId, ProjectRoot);
	const FAgentFeatureWorkflowState ReducedWorkflow =
		SClaudeEditorWidget::ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Context.Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestTrue(TEXT("current-prefix external proof log should be consumed"),
		HasExternalRuntimeProofLogFact(Context.Facts));
	TestTrue(TEXT("current-prefix external proof log should produce bounded runtime proof"),
		Context.Facts.HasBoundedInteractionAccessRuntimeProof());
	TestEqual(TEXT("recovered workflow should pass runtime proof"),
		ReducedWorkflow.RuntimeProofState,
		FString(TEXT("passed")));
	TestFalse(TEXT("recovered workflow should clear proof-prerequisite stop-loss"),
		ReducedWorkflow.bStopLossTriggered);
	TestEqual(TEXT("recovered workflow should advance to memory_update"),
		ReducedWorkflow.CurrentPhase,
		FString(TEXT("memory_update")));
	TestEqual(TEXT("current-prefix external proof should close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("current-prefix external proof should not keep stop-loss gate"),
		Decision.GateReasonCode,
		FString());
	return true;
}

bool FActivePlanCloseout_ExternalRuntimeProofLogStaleOrWrongPrefixRejected::RunTest(
	const FString& Parameters)
{
	const FString RunId = TEXT("run_packet662_external_runtime_proof_stale");
	const FString ProjectRoot = MakePacket662ExternalRuntimeProofProjectRoot(TEXT("stale"));
	FUnrealClaudeActivePlan Plan = MakePacket662ExternalRuntimeProofPlan(TEXT("stale"));
	const FString ValidProofLog = MakePacket662ExternalRuntimeProofLog();

	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v1_automation_20260427_200925.log"),
		ValidProofLog,
		FDateTime(2026, 4, 27, 17, 10, 0));
	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v2_retry_ddc_automation_20260425_200925.log"),
		ValidProofLog,
		FDateTime(2026, 4, 25, 17, 10, 0));

	const FString ArchiveDir =
		FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("UnrealClaude"), TEXT("PlanArchives"));
	IFileManager::Get().MakeDirectory(*ArchiveDir, true);
	const FString ArchivedLogPath =
		FPaths::Combine(ArchiveDir, TEXT("packet659_global_e2e_v2_retry_ddc_automation_20260427_201000.log"));
	FFileHelper::SaveStringToFile(ValidProofLog, *ArchivedLogPath);
	IFileManager::Get().SetTimeStamp(*ArchivedLogPath, FDateTime(2026, 4, 27, 17, 10, 0));

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendInteractionAccessSourceOnlyTraceRecords(Records, RunId, TEXT("2026-04-27T17:09:"));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(Plan, Records, RunId, ProjectRoot);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestFalse(TEXT("wrong-prefix or stale external proof logs must not be consumed"),
		HasExternalRuntimeProofLogFact(Context.Facts));
	TestFalse(TEXT("wrong-prefix or stale logs must not produce bounded automation proof"),
		Context.Facts.HasBoundedPrisonAccessAutomationProof());
	TestNotEqual(TEXT("wrong-prefix or stale logs must not close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("current recipe should still require runtime proof"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_runtime_proof_missing")));
	return true;
}

bool FActivePlanCloseout_ExternalRuntimeProofLogMalformedRejected::RunTest(
	const FString& Parameters)
{
	const FString RunId = TEXT("run_packet662_external_runtime_proof_malformed");
	const FString ProjectRoot = MakePacket662ExternalRuntimeProofProjectRoot(TEXT("malformed"));
	FUnrealClaudeActivePlan Plan = MakePacket662ExternalRuntimeProofPlan(TEXT("malformed"));

	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v2_retry_ddc_automation_missing_smoke.log"),
		MakePacket662ExternalRuntimeProofLog(7, false, 2, 0),
		FDateTime(2026, 4, 27, 17, 10, 0));
	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v2_retry_ddc_automation_one_event.log"),
		MakePacket662ExternalRuntimeProofLog(7, true, 1, 0),
		FDateTime(2026, 4, 27, 17, 11, 0));
	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v2_retry_ddc_automation_exit3.log"),
		MakePacket662ExternalRuntimeProofLog(7, true, 2, 3),
		FDateTime(2026, 4, 27, 17, 12, 0));
	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v2_retry_ddc_automation_six_successes.log"),
		MakePacket662ExternalRuntimeProofLog(6, true, 2, 0),
		FDateTime(2026, 4, 27, 17, 13, 0));
	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v2_retry_ddc_automation_startup_only.log"),
		TEXT("LogInit: Display: Running engine startup for Alternative.PrisonAccess without automation results\n"),
		FDateTime(2026, 4, 27, 17, 14, 0));
	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v2_retry_ddc_automation_partial_a.log"),
		MakePacket662ExternalRuntimeProofLog(7, false, 0, 0),
		FDateTime(2026, 4, 27, 17, 15, 0));
	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v2_retry_ddc_automation_partial_b.log"),
		MakePacket662ExternalRuntimeProofLog(0, true, 2, 0),
		FDateTime(2026, 4, 27, 17, 16, 0));

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendInteractionAccessSourceOnlyTraceRecords(Records, RunId, TEXT("2026-04-27T17:09:"));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(Plan, Records, RunId, ProjectRoot);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestFalse(TEXT("malformed external proof logs must not be consumed"),
		HasExternalRuntimeProofLogFact(Context.Facts));
	TestFalse(TEXT("malformed external proof logs must not produce bounded runtime proof"),
		Context.Facts.HasBoundedInteractionAccessRuntimeProof());
	TestNotEqual(TEXT("malformed external proof logs must not close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("malformed external proof should leave runtime proof missing"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_runtime_proof_missing")));
	return true;
}

bool FActivePlanCloseout_ExternalRuntimeProofRetryRecoversSameIdentity::RunTest(
	const FString& Parameters)
{
	const FString RunId = TEXT("run_84C7ACF14BA4C393676C2982F5BAD34D");
	const FString ProjectRoot = MakePacket662ExternalRuntimeProofProjectRoot(TEXT("retry_identity"));
	FUnrealClaudeActivePlan Plan = MakePacket662ExternalRuntimeProofPlan(TEXT("retry_identity"));
	Plan.PlanId = TEXT("plan_20260427_210827_48608346411C344D44885483014F0A0E");
	Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_1810fe4f");
	Plan.OriginalUserTask =
		TEXT("Continue existing packet run using D:/Project/Saved/Logs/packet659_global_e2e_v3_ingestion_runtime_automation_retry_20260428_002851.log.");
	Plan.CreatedAtUtc = TEXT("2026-04-27T21:08:27Z");

	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v3_ingestion_runtime_automation_retry_20260428_002851.log"),
		MakePacket662ExternalRuntimeProofLog(),
		FDateTime(2026, 4, 27, 21, 28, 51));

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendInteractionAccessSourceOnlyTraceRecords(Records, RunId, TEXT("2026-04-27T21:09:"));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(Plan, Records, RunId, ProjectRoot);
	const FAgentFeatureWorkflowState ReducedWorkflow =
		SClaudeEditorWidget::ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Context.Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestTrue(TEXT("retry proof should be consumed for the original plan prefix"),
		HasExternalRuntimeProofLogFact(Context.Facts));
	TestEqual(TEXT("closeout should retain source run id"),
		Decision.SourceRunId,
		RunId);
	TestEqual(TEXT("closeout should retain source plan id"),
		Decision.SourcePlanId,
		Plan.PlanId);
	TestEqual(TEXT("closeout should retain source workflow id"),
		Decision.SourceFeatureWorkflowId,
		Plan.FeatureWorkflow.FeatureWorkflowId);
	TestEqual(TEXT("recovered retry workflow should pass runtime proof"),
		ReducedWorkflow.RuntimeProofState,
		FString(TEXT("passed")));
	TestFalse(TEXT("recovered retry workflow should clear proof-prerequisite stop-loss"),
		ReducedWorkflow.bStopLossTriggered);
	TestEqual(TEXT("retry proof should close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("retry proof should not keep a gate reason"),
		Decision.GateReasonCode,
		FString());
	return true;
}

bool FActivePlanCloseout_ExternalRuntimeProofLaterPlanRejectsOlderValidLog::RunTest(
	const FString& Parameters)
{
	const FString RunId = TEXT("run_packet664_later_plan");
	const FString ProjectRoot = MakePacket662ExternalRuntimeProofProjectRoot(TEXT("later_plan"));
	FUnrealClaudeActivePlan Plan = MakePacket662ExternalRuntimeProofPlan(TEXT("later_plan"));
	Plan.OriginalUserTask =
		TEXT("New unrelated continuation. Use prefix packet659_global_e2e_v3_ingestion_runtime.");
	Plan.CreatedAtUtc = TEXT("2026-04-27T21:41:06Z");

	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet659_global_e2e_v3_ingestion_runtime_automation_retry_20260428_002851.log"),
		MakePacket662ExternalRuntimeProofLog(),
		FDateTime(2026, 4, 27, 21, 28, 51));

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendInteractionAccessSourceOnlyTraceRecords(Records, RunId, TEXT("2026-04-27T21:42:"));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(Plan, Records, RunId, ProjectRoot);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestFalse(TEXT("older valid proof must not be consumed by a later unrelated plan"),
		HasExternalRuntimeProofLogFact(Context.Facts));
	TestFalse(TEXT("older valid proof must not produce bounded runtime proof for later plan"),
		Context.Facts.HasBoundedInteractionAccessRuntimeProof());
	TestNotEqual(TEXT("later unrelated plan must not close fully from older proof"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	return true;
}

bool FActivePlanCloseout_ExternalRuntimeProofStopLossPlanCanResumeSameWorkflow::RunTest(
	const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakePacket662ExternalRuntimeProofPlan(TEXT("resume_same_workflow"));

	TestTrue(TEXT("recoverable external-proof stop-loss plan should remain resumable"),
		FAgentOrchestrator::ShouldResumeFeatureWorkflowFromActivePlan(Plan));

	Plan.FeatureWorkflow.StopLossReason = TEXT("phase_failed_twice:attempt_resolver_and_logging");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("attempt_resolver_observation_missing");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("attempt_resolver_source_observed=false");
	TestTrue(TEXT("attempt-resolver source-observation stop-loss should remain resumable for current-prefix proof ingestion"),
		FAgentOrchestrator::ShouldResumeFeatureWorkflowFromActivePlan(Plan));

	Plan.FeatureWorkflow.StopLossReason = TEXT("phase_failed_twice:interaction_contract");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("unrelated_stop_loss");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("not_proof_continuity");
	TestFalse(TEXT("unrelated stop-loss plan should still not resume as same workflow"),
		FAgentOrchestrator::ShouldResumeFeatureWorkflowFromActivePlan(Plan));
	return true;
}

bool FActivePlanCloseout_ExternalRuntimeProofAdHocStopLossRecoversCurrentPrefix::RunTest(
	const FString& Parameters)
{
	const FString RunId = TEXT("run_packet664_ad_hoc_stop_loss");
	const FString ProjectRoot = MakePacket662ExternalRuntimeProofProjectRoot(TEXT("ad_hoc_stop_loss"));
	FUnrealClaudeActivePlan Plan = MakePacket662ExternalRuntimeProofPlan(TEXT("ad_hoc_stop_loss"));
	Plan.PlanId = TEXT("plan_packet664_ad_hoc_stop_loss");
	Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_packet664_ad_hoc_stop_loss");
	Plan.OriginalUserTask =
		TEXT("Final live E2E acceptance for packet 664. Use prefix packet664_runtime_continuity_v1.");
	Plan.CreatedAtUtc = TEXT("2026-04-28T12:26:37Z");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.StopLossReason = TEXT("ad_hoc_runtime_proof_attempts_gt_3");
	Plan.FeatureWorkflow.BlockerFamily.Reset();
	Plan.FeatureWorkflow.BlockerDetail.Reset();
	Plan.FeatureWorkflow.AdHocProofAttemptCount = 4;
	Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance = 3;
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = true;
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 14;
	Plan.FeatureWorkflow.AutomationPassedCount = 14;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;
	Plan.FeatureWorkflow.CompletedPhaseIds = {
		TEXT("project_context_preflight"),
		TEXT("interaction_contract"),
		TEXT("input_asset_authoring"),
		TEXT("runtime_actor_state"),
		TEXT("attempt_resolver_and_logging"),
		TEXT("proof_context_setup"),
		TEXT("compile_gate"),
		TEXT("automation_discovery_gate")
	};

	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet664_runtime_continuity_v1_automation_20260428_105213.log"),
		MakePacket662ExternalRuntimeProofLog(),
		FDateTime(2026, 4, 28, 12, 53, 35));

	TArray<TSharedPtr<FJsonObject>> Records;
	// Deliberately omit the enhanced-input trace: this reproduces the live ad-hoc
	// stop-loss where only the current-prefix automation log supplied InputPathTruth.
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_packet664_open_level"),
		TEXT("unrealclaude/open_level"),
		TEXT("{\"level\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}"),
		TEXT("Opened /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof"),
		RunId,
		TEXT("2026-04-28T12:27:01Z")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_packet664_get_actors"),
		TEXT("unrealclaude/get_level_actors"),
		TEXT("{}"),
		TEXT("Proof_PrisonAccess_PlayerStart Proof_PrisonAccess_Floor Proof_PrisonAccess_Door Proof_PrisonAccess_ControlBox"),
		RunId,
		TEXT("2026-04-28T12:27:02Z")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_packet664_source_observation"),
		TEXT("command_execution"),
		TEXT("Select-String Source/Alternative/PrisonAccess"),
		TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp FAlternativePrisonAccessAttemptResolver ResolveDoorAttempt ResolveTechnicalBoxAttempt\nSource/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp UAlternativePrisonAccessEventSubsystem RecordEvent FAlternativePrisonAccessEventRecord PrisonAccessEvent order="),
		RunId,
		TEXT("2026-04-28T12:27:03Z")));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(Plan, Records, RunId, ProjectRoot);
	const FAgentFeatureWorkflowState ReducedWorkflow =
		SClaudeEditorWidget::ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Context.Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestTrue(TEXT("ad-hoc stop-loss current-prefix proof should be consumed"),
		HasExternalRuntimeProofLogFact(Context.Facts));
	TestTrue(TEXT("current-prefix proof should supply proof input mapping"),
		Context.Facts.bProofInputMappingAvailable);
	TestTrue(TEXT("current-prefix proof should produce bounded runtime proof"),
		Context.Facts.HasBoundedInteractionAccessRuntimeProof());
	TestEqual(TEXT("ad-hoc stop-loss recovery should pass runtime proof"),
		ReducedWorkflow.RuntimeProofState,
		FString(TEXT("passed")));
	TestFalse(TEXT("ad-hoc stop-loss recovery should clear stop-loss"),
		ReducedWorkflow.bStopLossTriggered);
	TestEqual(TEXT("ad-hoc proof attempt counter should be reset"),
		ReducedWorkflow.AdHocProofAttemptCount,
		0);
	TestEqual(TEXT("ad-hoc stop-loss proof should close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("ad-hoc stop-loss proof should not keep a gate reason"),
		Decision.GateReasonCode,
		FString());
	return true;
}

bool FActivePlanCloseout_ExternalRuntimeProofLogReadsProjectSourceForCurrentPrefix::RunTest(
	const FString& Parameters)
{
	const FString RunId = TEXT("run_packet664_project_source_ingestion");
	const FString ProjectRoot = MakePacket662ExternalRuntimeProofProjectRoot(TEXT("project_source_ingestion"));
	FUnrealClaudeActivePlan Plan = MakePacket662ExternalRuntimeProofPlan(TEXT("project_source_ingestion"));
	Plan.PlanId = TEXT("plan_packet664_project_source_ingestion");
	Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_packet664_project_source_ingestion");
	Plan.OriginalUserTask =
		TEXT("Continue packet 664 using current proof prefix packet664_runtime_trace_fix_v1.");
	Plan.CreatedAtUtc = TEXT("2026-04-28T16:34:00Z");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");
	Plan.FeatureWorkflow.StopLossReason = TEXT("ad_hoc_runtime_proof_attempts_gt_3");
	Plan.FeatureWorkflow.BlockerFamily.Reset();
	Plan.FeatureWorkflow.BlockerDetail.Reset();
	Plan.FeatureWorkflow.AdHocProofAttemptCount = 4;
	Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance = 3;
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation =
		FInteractionAccessAttemptResolverObservationState();
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 7;
	Plan.FeatureWorkflow.AutomationPassedCount = 7;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;
	Plan.FeatureWorkflow.CompletedPhaseIds = {
		TEXT("project_context_preflight"),
		TEXT("interaction_contract"),
		TEXT("input_asset_authoring"),
		TEXT("runtime_actor_state"),
		TEXT("attempt_resolver_and_logging"),
		TEXT("proof_context_setup"),
		TEXT("compile_gate"),
		TEXT("automation_discovery_gate")
	};

	WritePacket662ProjectSourceFixture(ProjectRoot);
	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet664_runtime_trace_fix_v1_automation_20260428_163443.log"),
		MakePacket662ExternalRuntimeProofLog(),
		FDateTime(2026, 4, 28, 16, 34, 43));

	TArray<TSharedPtr<FJsonObject>> Records;
	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(Plan, Records, RunId, ProjectRoot);
	const FAgentFeatureWorkflowState ReducedWorkflow =
		SClaudeEditorWidget::ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Context.Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestTrue(TEXT("current-prefix external proof log should be consumed"),
		HasExternalRuntimeProofLogFact(Context.Facts));
	TestTrue(TEXT("project source should supply attempt resolver source fact"),
		Context.Facts.bAttemptResolverSourceObserved);
	TestTrue(TEXT("project source should supply event subsystem source fact"),
		Context.Facts.bEventSubsystemSourceObserved);
	TestTrue(TEXT("project source plus current-prefix proof should produce bounded runtime proof"),
		Context.Facts.HasBoundedInteractionAccessRuntimeProof());
	TestEqual(TEXT("project-source ingestion should pass runtime proof"),
		ReducedWorkflow.RuntimeProofState,
		FString(TEXT("passed")));
	TestFalse(TEXT("project-source ingestion should clear ad-hoc stop-loss"),
		ReducedWorkflow.bStopLossTriggered);
	TestEqual(TEXT("project-source ingestion should reset ad-hoc proof attempts"),
		ReducedWorkflow.AdHocProofAttemptCount,
		0);
	TestEqual(TEXT("project-source ingestion should close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("project-source ingestion should not keep a gate reason"),
		Decision.GateReasonCode,
		FString());
	return true;
}

bool FActivePlanCloseout_AttemptResolverStopLossRecoversFromCurrentPrefixProof::RunTest(
	const FString& Parameters)
{
	const FString RunId = TEXT("run_packet670_attempt_resolver_stop_loss");
	const FString ProjectRoot = MakePacket662ExternalRuntimeProofProjectRoot(TEXT("attempt_resolver_stop_loss"));
	FUnrealClaudeActivePlan Plan = MakePacket662ExternalRuntimeProofPlan(TEXT("attempt_resolver_stop_loss"));
	Plan.PlanId = TEXT("plan_packet670_attempt_resolver_stop_loss");
	Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_packet670_attempt_resolver_stop_loss");
	Plan.OriginalUserTask =
		TEXT("Packet 670 control rerun. Use current proof prefix packet670_control_bridge_fix_v1.");
	Plan.CreatedAtUtc = TEXT("2026-04-29T00:15:00Z");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("attempt_resolver_and_logging");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");
	Plan.FeatureWorkflow.StopLossReason = TEXT("phase_failed_twice:attempt_resolver_and_logging");
	Plan.FeatureWorkflow.BlockerFamily = TEXT("attempt_resolver_observation_missing");
	Plan.FeatureWorkflow.BlockerDetail = TEXT("attempt_resolver_source_observed=false");
	Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance = 0;
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation =
		FInteractionAccessAttemptResolverObservationState();
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 7;
	Plan.FeatureWorkflow.AutomationPassedCount = 7;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;
	Plan.FeatureWorkflow.CompletedPhaseIds = {
		TEXT("project_context_preflight"),
		TEXT("interaction_contract"),
		TEXT("input_asset_authoring"),
		TEXT("runtime_actor_state")
	};
	Plan.FeatureWorkflow.FailedPhaseIds = { TEXT("attempt_resolver_and_logging") };
	if (FAgentFeatureWorkflowPhaseState* Phase =
		Plan.FeatureWorkflow.FindMutablePhase(TEXT("attempt_resolver_and_logging")))
	{
		Phase->Status = TEXT("failed");
		Phase->FailureCount = 2;
		Phase->LastFailureReason = TEXT("attempt_resolver_source_observed=false");
	}

	WritePacket662ExternalRuntimeProofLogFixture(
		ProjectRoot,
		TEXT("packet670_control_bridge_fix_v1_automation_20260429_001700.log"),
		MakePacket662ExternalRuntimeProofLog(),
		FDateTime(2026, 4, 29, 0, 17, 0));

	TArray<TSharedPtr<FJsonObject>> Records;
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_packet670_open_level"),
		TEXT("unrealclaude/open_level"),
		TEXT("{\"level\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}"),
		TEXT("Opened /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof"),
		RunId,
		TEXT("2026-04-29T00:15:01Z")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_packet670_get_actors"),
		TEXT("unrealclaude/get_level_actors"),
		TEXT("{}"),
		TEXT("Proof_PrisonAccess_PlayerStart Proof_PrisonAccess_Floor Proof_PrisonAccess_Door Proof_PrisonAccess_ControlBox"),
		RunId,
		TEXT("2026-04-29T00:15:02Z")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_packet670_failed_attempt_resolver_probe"),
		TEXT("command_execution"),
		TEXT("rg Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.*"),
		TEXT("rg: Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.*: IO error for operation on Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.*"),
		RunId,
		TEXT("2026-04-29T00:15:03Z")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_packet670_failed_event_subsystem_probe"),
		TEXT("command_execution"),
		TEXT("rg Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.*"),
		TEXT("rg: Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.*: IO error for operation on Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.*"),
		RunId,
		TEXT("2026-04-29T00:15:04Z")));
	Records.Add(MakeCloseoutTraceToolResult(
		TEXT("trace_packet670_backslash_source_observation"),
		TEXT("command_execution"),
		TEXT("Select-String Source\\Alternative\\PrisonAccess"),
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.h:9: struct ALTERNATIVE_API FAlternativePrisonAccessAttemptResolver\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.h:16: static FAlternativePrisonAccessAttemptOutcome ResolveDoorAttempt(\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.h:12: class UAlternativePrisonAccessEventSubsystem\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.cpp:18: void UAlternativePrisonAccessEventSubsystem::RecordEvent(const FAlternativePrisonAccessEventRecord& Record)\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.cpp:29: TEXT(\"PrisonAccessEvent order=%d actor=%s target=%s attempt=%d result=%d noisy=%s tag=%s\")"),
		RunId,
		TEXT("2026-04-29T00:15:05Z")));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(Plan, Records, RunId, ProjectRoot);
	const FAgentFeatureWorkflowState ReducedWorkflow =
		SClaudeEditorWidget::ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Context.Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestTrue(TEXT("attempt resolver source should be recovered from Windows-style source paths"),
		Context.Facts.bAttemptResolverSourceObserved);
	TestTrue(TEXT("event subsystem source should be recovered from Windows-style source paths"),
		Context.Facts.bEventSubsystemSourceObserved);
	TestTrue(TEXT("current-prefix runtime proof log should be consumed after attempt-resolver stop-loss"),
		HasExternalRuntimeProofLogFact(Context.Facts));
	TestTrue(TEXT("same-run trace plus current-prefix proof should produce bounded runtime proof"),
		Context.Facts.HasBoundedInteractionAccessRuntimeProof());
	TestEqual(TEXT("attempt-resolver stop-loss recovery should pass runtime proof"),
		ReducedWorkflow.RuntimeProofState,
		FString(TEXT("passed")));
	TestFalse(TEXT("attempt-resolver stop-loss should be cleared only after bounded proof"),
		ReducedWorkflow.bStopLossTriggered);
	TestFalse(TEXT("attempt-resolver failed phase should be cleared after complete proof"),
		ReducedWorkflow.FailedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestTrue(TEXT("attempt-resolver phase should be completed after recovered source observation"),
		ReducedWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestEqual(TEXT("recovered attempt-resolver proof should close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("recovered attempt-resolver proof should not keep a gate reason"),
		Decision.GateReasonCode,
		FString());
	return true;
}

bool FActivePlanCloseout_ContinuitySchemaLabelsDoNotBecomeRefs::RunTest(const FString& Parameters)
{
	const FString SchemaOnlyPrompt =
		TEXT("Final answer must include result status, run_id, plan_id, workflow_id, archive path, and closeout path.");
	TestFalse(TEXT("schema-only final answer labels must not be treated as continuity references"),
		SClaudeEditorWidget::PromptHasExplicitContinuityRefsForTests(SchemaOnlyPrompt));
	TestEqual(TEXT("schema-only final answer labels should describe no refs"),
		SClaudeEditorWidget::DescribeExplicitContinuityRefsForTests(SchemaOnlyPrompt),
		FString());

	const FString RealIdPrompt =
		TEXT("Resume run_15B62F64457C357C16F7C5B2FFA56570 / plan_20260428_122637_CB131E474A091606F6C071B5E61C028E / feature_18a8ec25.");
	const FString RealRefs = SClaudeEditorWidget::DescribeExplicitContinuityRefsForTests(RealIdPrompt);
	TestTrue(TEXT("real run id should still be parsed"),
		RealRefs.Contains(TEXT("run_id=run_15B62F64457C357C16F7C5B2FFA56570"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("real plan id should still be parsed"),
		RealRefs.Contains(TEXT("plan_id=plan_20260428_122637_CB131E474A091606F6C071B5E61C028E"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("real feature workflow id should still be parsed"),
		RealRefs.Contains(TEXT("feature_workflow_id=feature_18a8ec25"), ESearchCase::CaseSensitive));

	const FString MixedPrompt =
		TEXT("Report fields: run_id, plan_id, feature_workflow_id. Then continue plan_20260428_122637_CB131E474A091606F6C071B5E61C028E.");
	TestEqual(TEXT("reserved labels should be skipped so a later real plan id remains bindable"),
		SClaudeEditorWidget::DescribeExplicitContinuityRefsForTests(MixedPrompt),
		FString(TEXT("plan_id=plan_20260428_122637_CB131E474A091606F6C071B5E61C028E")));
	return true;
}

bool FActivePlanCloseout_SystemicCleanPluginOwnedCloseoutAccepted::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");

	FUnrealClaudeCloseoutFactSnapshot Facts;
	PrimeBoundedInteractionAccessCloseoutFacts(Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);
	FUnrealClaudeActivePlanCloseoutDecision DecisionWithArchive = Decision;
	DecisionWithArchive.SourceArchivePath =
		TEXT("Saved/UnrealClaude/PlanArchives/plugin_owned.active_plan.json");
	const TSharedPtr<FJsonObject> DecisionJson =
		SClaudeEditorWidget::BuildCloseoutDecisionJson(Plan, DecisionWithArchive, Facts);

	TestFalse(TEXT("clean plugin-owned facts should not detect manual managed-state writes"),
		Facts.bManagedStateManualWriteDetected);
	TestEqual(TEXT("clean plugin-owned facts should close done"),
		Decision.PlanStatus,
		FString(TEXT("done")));
	TestEqual(TEXT("clean plugin-owned facts should achieve fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("clean plugin-owned closeout should not have gate reason"),
		Decision.GateReasonCode,
		FString());
	TestTrue(TEXT("clean plugin-owned closeout should pass runtime proof"),
		Decision.bRuntimeProofPassed);
	TestTrue(TEXT("clean closeout decision json should include archive path"),
		DecisionJson->HasField(TEXT("source_archive_path")));
	TestFalse(TEXT("clean closeout decision json should not include blocker"),
		DecisionJson->HasField(TEXT("blocker")));
	return true;
}

bool FActivePlanCloseout_RecipeRegistryUnknownRecipeFailsClosed::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.RecipeId = TEXT("feature.unregistered_recipe_v1");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");

	FUnrealClaudeCloseoutFactSnapshot Facts;
	PrimeBoundedInteractionAccessCloseoutFacts(Facts);

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);

	TestEqual(TEXT("unknown recipe should fail closed"),
		Decision.ResultStatus,
		FString(TEXT("not_achieved")));
	TestEqual(TEXT("unknown recipe should surface contract blocker"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_unknown_recipe_contract")));
	TestEqual(TEXT("unknown recipe should surface blocker family"),
		Decision.BlockerFamily,
		FString(TEXT("recipe_contract_unknown")));
	TestTrue(TEXT("unknown recipe detail should name recipe id"),
		Decision.BlockerDetail.Contains(TEXT("feature.unregistered_recipe_v1")));
	TestFalse(TEXT("unknown recipe should not claim resolved contract"),
		Decision.bRecipeContractResolved);
	return true;
}

bool FActivePlanCloseout_RecipeRegistryMissingObligationNamesMissingFact::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.InteractionAccessReuseObservation.bPersistentInputAssetObserved = true;
	Plan.FeatureWorkflow.InteractionAccessReuseObservation.bInteractionActionAssetObserved = true;
	Plan.FeatureWorkflow.InteractionAccessReuseObservation.bReadOnlyEnhancedInputQueryObserved = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = true;
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 7;
	Plan.FeatureWorkflow.AutomationPassedCount = 7;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestEqual(TEXT("missing proof map should not achieve"),
		Decision.ResultStatus,
		FString(TEXT("not_achieved")));
	TestEqual(TEXT("missing contract obligation should use recipe gate"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_recipe_obligation_missing")));
	TestTrue(TEXT("missing obligation should name proof map"),
		Decision.MissingRecipeObligations.Contains(TEXT("known_proof_map_available")));
	TestTrue(TEXT("blocker detail should name missing obligation"),
		Decision.BlockerDetail.Contains(TEXT("known_proof_map_available")));
	TestEqual(TEXT("contract schema should be v1"),
		Decision.EvidenceSchemaVersion,
		1);
	return true;
}

bool FActivePlanCloseout_RecipeRegistryDecisionJsonSerializesContract::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");

	FUnrealClaudeCloseoutFactSnapshot Facts;
	PrimeBoundedInteractionAccessCloseoutFacts(Facts);

	FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);
	const TSharedPtr<FJsonObject> DecisionJson =
		SClaudeEditorWidget::BuildCloseoutDecisionJson(Plan, Decision, Facts);
	const TSharedPtr<FJsonObject>* DecisionObject = nullptr;
	const TSharedPtr<FJsonObject>* ContractObject = nullptr;
	DecisionJson->TryGetObjectField(TEXT("decision"), DecisionObject);
	DecisionJson->TryGetObjectField(TEXT("recipe_contract"), ContractObject);

	FString RootRecipeId;
	double RootSchemaVersion = 0.0;
	DecisionJson->TryGetStringField(TEXT("recipe_id"), RootRecipeId);
	DecisionJson->TryGetNumberField(TEXT("evidence_schema_version"), RootSchemaVersion);

	FString DecisionRecipeId;
	double DecisionSchemaVersion = 0.0;
	if (DecisionObject && DecisionObject->IsValid())
	{
		(*DecisionObject)->TryGetStringField(TEXT("recipe_id"), DecisionRecipeId);
		(*DecisionObject)->TryGetNumberField(TEXT("evidence_schema_version"), DecisionSchemaVersion);
	}

	TestEqual(TEXT("decision should carry recipe id"),
		Decision.SourceRecipeId,
		UnrealClaudeRecipeRegistry::InteractionAccessRecipeId());
	TestEqual(TEXT("decision should carry schema version"),
		Decision.EvidenceSchemaVersion,
		1);
	TestEqual(TEXT("root json should carry recipe id"),
		RootRecipeId,
		UnrealClaudeRecipeRegistry::InteractionAccessRecipeId());
	TestEqual(TEXT("root json should carry schema version"),
		static_cast<int32>(RootSchemaVersion),
		1);
	TestEqual(TEXT("nested decision should carry recipe id"),
		DecisionRecipeId,
		UnrealClaudeRecipeRegistry::InteractionAccessRecipeId());
	TestEqual(TEXT("nested decision should carry schema version"),
		static_cast<int32>(DecisionSchemaVersion),
		1);
	TestTrue(TEXT("closeout json should include recipe contract"),
		ContractObject && ContractObject->IsValid());
	return true;
}

bool FActivePlanCloseout_RecipeRegistryOldRunEvidenceDoesNotSatisfyCurrentRecipe::RunTest(const FString& Parameters)
{
	const FString OldRunId = TEXT("run_packet656_old_clean_evidence");
	const FString CurrentRunId = TEXT("run_packet656_current_without_evidence");
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendCleanInteractionAccessTraceRecords(Records, OldRunId, TEXT("2026-04-26T23:40:"));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(
			Plan,
			Records,
			CurrentRunId,
			TEXT("D:/VibeCode/Unreal/Alternative/Alternative"));
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestFalse(TEXT("old-run clean proof should not be included for current run"),
		Context.IncludedTraceEventIds.Contains(TEXT("trace_automation_clean")));
	TestTrue(TEXT("old-run clean proof should be excluded"),
		Context.ExcludedTraceEventIds.Contains(TEXT("trace_automation_clean")));
	TestFalse(TEXT("current run should not inherit bounded proof"),
		Context.Facts.HasBoundedInteractionAccessRuntimeProof());
	TestEqual(TEXT("stale evidence should not satisfy current recipe"),
		Decision.ResultStatus,
		FString(TEXT("achieved_partially")));
	TestEqual(TEXT("current recipe should still require runtime proof"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_runtime_proof_missing")));
	return true;
}

bool FActivePlanCloseout_SystemicRunScopedOldTraceManualWriteDoesNotPoisonCleanRun::RunTest(const FString& Parameters)
{
	const FString OldRunId = TEXT("run_packet654_v1_tainted");
	const FString CleanRunId = TEXT("run_packet654_v3_clean");
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.PlanId = TEXT("plan_packet654_clean");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");

	TArray<TSharedPtr<FJsonObject>> Records;
	Records.Add(MakeCloseoutTraceToolUse(
		TEXT("trace_old_manual_state_write"),
		TEXT("command_execution"),
		TEXT("Set-Content Saved/UnrealClaude/active_plan.json; Set-Content Saved/UnrealClaude/CloseoutDecisions/old.closeout_decision.json"),
		true,
		TEXT("workspace_file_build"),
		OldRunId,
		TEXT("2026-04-26T08:40:42Z")));
	AppendCleanInteractionAccessTraceRecords(Records, CleanRunId, TEXT("2026-04-27T00:10:"));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(
			Plan,
			Records,
			CleanRunId,
			TEXT("D:/VibeCode/Unreal/Alternative/Alternative"));
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);

	TestEqual(TEXT("run context should target clean run"),
		Context.RunId,
		CleanRunId);
	TestFalse(TEXT("old-run managed state write should be excluded from clean context"),
		Context.IncludedTraceEventIds.Contains(TEXT("trace_old_manual_state_write")));
	TestTrue(TEXT("old-run event should be listed as excluded"),
		Context.ExcludedTraceEventIds.Contains(TEXT("trace_old_manual_state_write")));
	TestFalse(TEXT("old-run managed state write should not taint clean facts"),
		Context.Facts.bManagedStateManualWriteDetected);
	TestTrue(TEXT("clean run should still consume bounded runtime proof"),
		Context.Facts.HasBoundedInteractionAccessRuntimeProof());
	TestEqual(TEXT("clean run should close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("clean run should not carry managed state gate"),
		Decision.GateReasonCode,
		FString());
	return true;
}

bool FActivePlanCloseout_SystemicRunScopedCurrentRunManualWriteRejectsAcceptance::RunTest(const FString& Parameters)
{
	const FString CurrentRunId = TEXT("run_packet654_v3_current_tainted");
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.PlanId = TEXT("plan_packet654_current_tainted");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.bStopLossTriggered = true;
	Plan.FeatureWorkflow.StopLossReason = TEXT("command_execution_without_phase_advance_gt_5");
	Plan.FeatureWorkflow.TerminalStatus = TEXT("stop_loss");
	Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance = 6;

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendCleanInteractionAccessTraceRecords(Records, CurrentRunId, TEXT("2026-04-27T00:20:"));
	Records.Add(MakeCloseoutTraceToolUse(
		TEXT("trace_current_manual_state_write"),
		TEXT("command_execution"),
		TEXT("Copy-Item Saved/UnrealClaude/active_plan.json Saved/UnrealClaude/PlanArchives/current.active_plan.json; Set-Content Saved/UnrealClaude/visible_session_codex_cli.json"),
		true,
		TEXT("workspace_file_build"),
		CurrentRunId,
		TEXT("2026-04-27T00:20:05Z")));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(
			Plan,
			Records,
			CurrentRunId,
			TEXT("D:/VibeCode/Unreal/Alternative/Alternative"));
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);
	const FString Rewritten = SClaudeEditorWidget::RewriteResponseForTruthfulActivePlanCloseout(
		Plan,
		Decision,
		TEXT("- result status: `full`\n- current phase: `memory_update`\n- exact next blocker if not complete: none"));

	TestTrue(TEXT("current-run manual managed-state write should be included"),
		Context.IncludedTraceEventIds.Contains(TEXT("trace_current_manual_state_write")));
	TestTrue(TEXT("current-run manual managed-state write should be detected"),
		Context.Facts.bManagedStateManualWriteDetected);
	TestTrue(TEXT("current-run mutating command should be detected"),
		Context.Facts.bCommandMutationDetected);
	TestEqual(TEXT("current-run manual write should reject closeout"),
		Decision.ResultStatus,
		FString(TEXT("not_achieved")));
	TestEqual(TEXT("current-run manual write should use integrity gate"),
		Decision.GateReasonCode,
		FString(TEXT("managed_state_manual_write_detected")));
	TestEqual(TEXT("current-run manual write should surface blocker family"),
		Decision.BlockerFamily,
		FString(TEXT("managed_state_integrity")));
	TestTrue(TEXT("rejected closeout should rewrite false full response to fail"),
		Rewritten.Contains(TEXT("`fail`")));
	TestTrue(TEXT("rewrite should cite managed state integrity gate"),
		Rewritten.Contains(TEXT("gate_reason: managed_state_manual_write_detected")));
	return true;
}

bool FActivePlanCloseout_SystemicRunScopedDecisionJsonCarriesRunPlanIdentity::RunTest(const FString& Parameters)
{
	const FString RunId = TEXT("run_packet654_identity_clean");
	const FString ProjectRoot = TEXT("D:/VibeCode/Unreal/Alternative/Alternative");
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.PlanId = TEXT("plan_packet654_identity");
	Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_packet654_identity");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendCleanInteractionAccessTraceRecords(Records, RunId, TEXT("2026-04-27T00:30:"));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(Plan, Records, RunId, ProjectRoot);
	FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);
	Decision.SourceArchivePath = TEXT("Saved/UnrealClaude/PlanArchives/run_scoped.active_plan.json");
	const TSharedPtr<FJsonObject> DecisionJson =
		SClaudeEditorWidget::BuildCloseoutDecisionJson(Plan, Decision, Context);
	const TSharedPtr<FJsonObject>* ContextJson = nullptr;
	DecisionJson->TryGetObjectField(TEXT("run_closeout_context"), ContextJson);

	FString JsonRunId;
	FString JsonPlanId;
	FString JsonWorkflowId;
	DecisionJson->TryGetStringField(TEXT("source_run_id"), JsonRunId);
	DecisionJson->TryGetStringField(TEXT("source_plan_id"), JsonPlanId);
	DecisionJson->TryGetStringField(TEXT("source_feature_workflow_id"), JsonWorkflowId);

	TestEqual(TEXT("decision should carry source run id"),
		Decision.SourceRunId,
		RunId);
	TestEqual(TEXT("decision json should carry source run id"),
		JsonRunId,
		RunId);
	TestEqual(TEXT("decision json should carry source plan id"),
		JsonPlanId,
		Plan.PlanId);
	TestEqual(TEXT("decision json should carry source workflow id"),
		JsonWorkflowId,
		Plan.FeatureWorkflow.FeatureWorkflowId);
	TestTrue(TEXT("decision json should include run closeout context"),
		ContextJson && ContextJson->IsValid());
	TestTrue(TEXT("run closeout context should include only current-run trace facts"),
		Context.IncludedTraceEventIds.Contains(TEXT("trace_automation_clean")));
	TestEqual(TEXT("identity clean run should close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	return true;
}

bool FActivePlanCloseout_SystemicCleanPostProofMemoryUpdateCommandDriftLatchesAchieved::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.PlanId = TEXT("plan_packet654_post_proof_read_only_drift");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.bStopLossTriggered = true;
	Plan.FeatureWorkflow.StopLossReason = TEXT("command_execution_without_phase_advance_gt_5");
	Plan.FeatureWorkflow.TerminalStatus = TEXT("stop_loss");
	Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance = 6;
	Plan.FeatureWorkflow.FailedPhaseIds.Add(TEXT("memory_update"));

	FUnrealClaudeCloseoutFactSnapshot Facts;
	PrimeBoundedInteractionAccessCloseoutFacts(Facts);
	Facts.AddFactId(TEXT("test.post_proof_read_only_inspection"));

	const FAgentFeatureWorkflowState ReducedWorkflow =
		SClaudeEditorWidget::ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);

	TestFalse(TEXT("clean post-proof read-only drift should clear stop-loss"),
		ReducedWorkflow.bStopLossTriggered);
	TestEqual(TEXT("clean post-proof read-only drift should clear terminal stop-loss"),
		ReducedWorkflow.TerminalStatus,
		FString());
	TestEqual(TEXT("clean post-proof read-only drift should reset no-advance counter"),
		ReducedWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);
	TestFalse(TEXT("clean post-proof read-only drift should clear failed memory_update phase"),
		ReducedWorkflow.FailedPhaseIds.Contains(TEXT("memory_update")));
	TestEqual(TEXT("clean post-proof read-only drift should close done"),
		Decision.PlanStatus,
		FString(TEXT("done")));
	TestEqual(TEXT("clean post-proof read-only drift should achieve fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	TestEqual(TEXT("clean post-proof read-only drift should not surface stop-loss gate"),
		Decision.GateReasonCode,
		FString());
	TestFalse(TEXT("clean post-proof read-only drift decision should not keep stop-loss"),
		Decision.bStopLossTriggered);
	TestTrue(TEXT("clean post-proof read-only drift decision should preserve runtime proof"),
		Decision.bRuntimeProofPassed);
	return true;
}

bool FActivePlanCloseout_SystemicPostProofMemoryUpdateCommandDriftWithoutRuntimeProofStillFails::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.PlanId = TEXT("plan_packet654_post_proof_missing_runtime");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");
	Plan.FeatureWorkflow.bStopLossTriggered = true;
	Plan.FeatureWorkflow.StopLossReason = TEXT("command_execution_without_phase_advance_gt_5");
	Plan.FeatureWorkflow.TerminalStatus = TEXT("stop_loss");
	Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance = 6;

	FUnrealClaudeCloseoutFactSnapshot Facts;
	Facts.bKnownProofMapAvailable = true;
	Facts.bProofInputMappingAvailable = true;
	Facts.bPlacedRuntimeActorsAvailable = true;
	Facts.AutomationDiscoveryCount = 7;
	Facts.AddFactId(TEXT("test.prereqs_without_runtime_proof_after_drift"));

	const FAgentFeatureWorkflowState ReducedWorkflow =
		SClaudeEditorWidget::ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Facts);
	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);

	TestTrue(TEXT("missing runtime proof must keep command-drift stop-loss"),
		ReducedWorkflow.bStopLossTriggered);
	TestFalse(TEXT("missing runtime proof must not pass runtime proof"),
		Decision.bRuntimeProofPassed);
	TestEqual(TEXT("missing runtime proof after command drift should fail"),
		Decision.ResultStatus,
		FString(TEXT("not_achieved")));
	TestEqual(TEXT("missing runtime proof after command drift should keep stop-loss gate"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_stop_loss_triggered")));
	return true;
}

bool FActivePlanCloseout_SystemicJudgeMissingProofInputMappingRejectsRuntimeProof::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 7;
	Plan.FeatureWorkflow.AutomationPassedCount = 7;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = true;

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestEqual(TEXT("runtime proof without proof input mapping should fail"),
		Decision.PlanStatus,
		FString(TEXT("failed")));
	TestEqual(TEXT("missing proof input mapping should not achieve"),
		Decision.ResultStatus,
		FString(TEXT("not_achieved")));
	TestEqual(TEXT("missing proof input mapping should have explicit gate reason"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_proof_input_mapping_missing")));
	TestEqual(TEXT("missing proof input mapping should surface blocker family"),
		Decision.BlockerFamily,
		FString(TEXT("proof_input_mapping_missing")));
	return true;
}

bool FActivePlanCloseout_SystemicJudgePayloadJsonEvidenceConsumesProofInputMapping::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("tool_name"), TEXT("unrealclaude/enhanced_input"));

	TSharedPtr<FJsonObject> ToolInputObject = MakeShared<FJsonObject>();
	ToolInputObject->SetStringField(TEXT("operation"), TEXT("query_context"));
	ToolInputObject->SetStringField(TEXT("context"), TEXT("/Game/PrisonAccess/Input/IMC_PrisonAccessProof"));
	Payload->SetObjectField(TEXT("tool_input"), ToolInputObject);

	TSharedPtr<FJsonObject> ToolResultObject = MakeShared<FJsonObject>();
	ToolResultObject->SetStringField(TEXT("context"), TEXT("IMC_PrisonAccessProof"));
	ToolResultObject->SetStringField(TEXT("action"), TEXT("IA_Interact"));
	ToolResultObject->SetStringField(TEXT("evidence"), TEXT("read_only_enhanced_input_query_observed input mapping"));
	Payload->SetObjectField(TEXT("tool_result"), ToolResultObject);

	TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
	Record->SetStringField(TEXT("event_id"), TEXT("trace_payload_json_input_mapping"));
	Record->SetStringField(TEXT("event_type"), TEXT("tool_result"));
	Record->SetObjectField(TEXT("payload"), Payload);

	TArray<TSharedPtr<FJsonObject>> Records;
	Records.Add(Record);

	const FUnrealClaudeCloseoutFactSnapshot Facts =
		SClaudeEditorWidget::ExtractAgentTraceFactsForCloseout(Records);
	const TSharedPtr<FJsonObject> FactsJson = Facts.ToJsonObject();
	bool bJsonInputMapping = false;
	FactsJson->TryGetBoolField(TEXT("proof_input_mapping_available"), bJsonInputMapping);

	TestTrue(TEXT("serialized structured tool payload should consume proof input mapping"),
		Facts.bProofInputMappingAvailable);
	TestTrue(TEXT("facts json should expose proof input mapping"),
		bJsonInputMapping);
	TestTrue(TEXT("proof input mapping fact id should be consumed"),
		Facts.ConsumedFactIds.Contains(TEXT("agent_trace.trace_payload_json_input_mapping.proof_input_mapping")));
	return true;
}

bool FActivePlanCloseout_SystemicCloseoutDecisionIdentityIsNonContradictoryAcrossRepeatedBuilds::RunTest(
	const FString& Parameters)
{
	const FString RunId = TEXT("run_packet654_idempotent_clean");
	const FString ArchivePath =
		TEXT("Saved/UnrealClaude/PlanArchives/20260427-run_packet654_idempotent_clean-plan_packet654_idempotent.active_plan.json");
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.PlanId = TEXT("plan_packet654_idempotent");
	Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_packet654_idempotent");
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");

	TArray<TSharedPtr<FJsonObject>> Records;
	AppendCleanInteractionAccessTraceRecords(Records, RunId, TEXT("2026-04-27T00:40:"));

	const FUnrealClaudeRunCloseoutContext Context =
		SClaudeEditorWidget::BuildRunCloseoutContext(
			Plan,
			Records,
			RunId,
			TEXT("D:/VibeCode/Unreal/Alternative/Alternative"));
	FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithContext(Plan, true, Context);
	Decision.SourceArchivePath = ArchivePath;

	const TSharedPtr<FJsonObject> FirstDecisionJson =
		SClaudeEditorWidget::BuildCloseoutDecisionJson(Plan, Decision, Context);
	const TSharedPtr<FJsonObject> SecondDecisionJson =
		SClaudeEditorWidget::BuildCloseoutDecisionJson(Plan, Decision, Context);

	FString FirstRunId;
	FString SecondRunId;
	FString FirstPlanId;
	FString SecondPlanId;
	FString FirstArchivePath;
	FString SecondArchivePath;
	FirstDecisionJson->TryGetStringField(TEXT("source_run_id"), FirstRunId);
	SecondDecisionJson->TryGetStringField(TEXT("source_run_id"), SecondRunId);
	FirstDecisionJson->TryGetStringField(TEXT("source_plan_id"), FirstPlanId);
	SecondDecisionJson->TryGetStringField(TEXT("source_plan_id"), SecondPlanId);
	FirstDecisionJson->TryGetStringField(TEXT("source_archive_path"), FirstArchivePath);
	SecondDecisionJson->TryGetStringField(TEXT("source_archive_path"), SecondArchivePath);

	const TSharedPtr<FJsonObject>* FirstDecisionObject = nullptr;
	const TSharedPtr<FJsonObject>* SecondDecisionObject = nullptr;
	FirstDecisionJson->TryGetObjectField(TEXT("decision"), FirstDecisionObject);
	SecondDecisionJson->TryGetObjectField(TEXT("decision"), SecondDecisionObject);

	FString FirstResultStatus;
	FString SecondResultStatus;
	if (FirstDecisionObject && FirstDecisionObject->IsValid())
	{
		(*FirstDecisionObject)->TryGetStringField(TEXT("result_status"), FirstResultStatus);
	}
	if (SecondDecisionObject && SecondDecisionObject->IsValid())
	{
		(*SecondDecisionObject)->TryGetStringField(TEXT("result_status"), SecondResultStatus);
	}

	TestEqual(TEXT("repeated closeout decision should keep same run id"), SecondRunId, FirstRunId);
	TestEqual(TEXT("repeated closeout decision should keep same plan id"), SecondPlanId, FirstPlanId);
	TestEqual(TEXT("repeated closeout decision should keep same archive path"), SecondArchivePath, FirstArchivePath);
	TestEqual(TEXT("repeated closeout decision should keep same result verdict"), SecondResultStatus, FirstResultStatus);
	TestEqual(TEXT("idempotent clean run should close fully"),
		Decision.ResultStatus,
		FString(TEXT("achieved_fully")));
	return true;
}

bool FActivePlanCloseout_RoleContractsUnknownRoleFailsClosed::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.RoleId = TEXT("operator");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	PrimeProofInputMappingObservation(Plan.FeatureWorkflow);
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = true;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = true;
	Plan.FeatureWorkflow.AutomationDiscoveryCount = 7;
	Plan.FeatureWorkflow.AutomationExecutedCount = 7;
	Plan.FeatureWorkflow.AutomationPassedCount = 7;
	Plan.FeatureWorkflow.AutomationFailedCount = 0;

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestEqual(TEXT("unknown role should fail plan"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("unknown role should not achieve"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(
		TEXT("unknown role should fail closed with role contract gate"),
		Decision.GateReasonCode,
		FString(TEXT("feature_workflow_unknown_role_contract")));
	TestEqual(
		TEXT("unknown role should name missing contract"),
		Decision.BlockerFamily,
		FString(TEXT("role_contract_unknown")));
	TestTrue(TEXT("unknown role blocker should include role id"),
		Decision.BlockerDetail.Contains(TEXT("operator"), ESearchCase::CaseSensitive));
	return true;
}

bool FActivePlanCloseout_RoleContractsArchitectCannotCloseRuntimeProof::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.RoleId = UnrealClaudeRoleRegistry::ArchitectRoleId();
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	PrimeProofInputMappingObservation(Plan.FeatureWorkflow);

	FUnrealClaudeCloseoutFactSnapshot Facts;
	PrimeBoundedInteractionAccessCloseoutFacts(Facts);

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);

	TestEqual(TEXT("architect runtime proof claim should fail"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("architect runtime proof claim should not achieve"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(
		TEXT("architect runtime proof claim should be denied by role contract"),
		Decision.GateReasonCode,
		FString(TEXT("role_contract_architect_runtime_proof_claim_denied")));
	TestEqual(
		TEXT("architect denial should surface role violation"),
		Decision.BlockerFamily,
		FString(TEXT("role_contract_violation")));
	TestEqual(TEXT("decision should cite architect role"), Decision.SourceRoleId, FString(TEXT("architect")));
	return true;
}

bool FActivePlanCloseout_RoleContractsWorkerManagedStateWriteStillRejectsAcceptance::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.RoleId = UnrealClaudeRoleRegistry::WorkerRoleId();
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	PrimeProofInputMappingObservation(Plan.FeatureWorkflow);

	FUnrealClaudeCloseoutFactSnapshot Facts;
	PrimeBoundedInteractionAccessCloseoutFacts(Facts);
	Facts.bManagedStateManualWriteDetected = true;
	Facts.ManagedStateManualWriteFactIds.Add(TEXT("agent_trace.current_run.managed_state_manual_write"));

	const FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);

	TestEqual(TEXT("worker managed-state direct write should fail"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("worker managed-state direct write should not achieve"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(
		TEXT("worker cannot bypass closeout manager with managed-state write"),
		Decision.GateReasonCode,
		FString(TEXT("managed_state_manual_write_detected")));
	TestTrue(TEXT("worker role should resolve"), Decision.bRoleContractResolved);
	TestEqual(TEXT("decision should cite worker role"), Decision.SourceRoleId, FString(TEXT("worker")));
	return true;
}

bool FActivePlanCloseout_RoleContractsVerifierDecisionJsonCitesCloseoutContract::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan = MakeVerificationOnlyInteractionAccessFeatureWorkflowPlan();
	Plan.FeatureWorkflow.RoleId = UnrealClaudeRoleRegistry::VerifierRoleId();
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	PrimeProofInputMappingObservation(Plan.FeatureWorkflow);

	FUnrealClaudeCloseoutFactSnapshot Facts;
	PrimeBoundedInteractionAccessCloseoutFacts(Facts);

	FUnrealClaudeActivePlanCloseoutDecision Decision =
		SClaudeEditorWidget::EvaluateActivePlanCloseoutWithFacts(Plan, true, Facts);
	const TSharedPtr<FJsonObject> DecisionJson =
		SClaudeEditorWidget::BuildCloseoutDecisionJson(Plan, Decision, Facts);

	FString RootRoleId;
	FString RootRecipeId;
	double RootEvidenceSchemaVersion = 0.0;
	DecisionJson->TryGetStringField(TEXT("role_id"), RootRoleId);
	DecisionJson->TryGetStringField(TEXT("recipe_id"), RootRecipeId);
	DecisionJson->TryGetNumberField(TEXT("evidence_schema_version"), RootEvidenceSchemaVersion);

	const TSharedPtr<FJsonObject>* DecisionObject = nullptr;
	TestTrue(TEXT("closeout JSON should include decision object"),
		DecisionJson->TryGetObjectField(TEXT("decision"), DecisionObject) && DecisionObject && (*DecisionObject).IsValid());
	if (!DecisionObject || !(*DecisionObject).IsValid())
	{
		return false;
	}

	FString DecisionRoleId;
	FString DecisionRecipeId;
	FString DecisionResultStatus;
	double DecisionEvidenceSchemaVersion = 0.0;
	bool bRoleContractResolved = false;
	(*DecisionObject)->TryGetStringField(TEXT("role_id"), DecisionRoleId);
	(*DecisionObject)->TryGetStringField(TEXT("recipe_id"), DecisionRecipeId);
	(*DecisionObject)->TryGetStringField(TEXT("result_status"), DecisionResultStatus);
	(*DecisionObject)->TryGetNumberField(TEXT("evidence_schema_version"), DecisionEvidenceSchemaVersion);
	(*DecisionObject)->TryGetBoolField(TEXT("role_contract_resolved"), bRoleContractResolved);

	TestEqual(TEXT("verifier decision should achieve through deterministic closeout"), Decision.ResultStatus, FString(TEXT("achieved_fully")));
	TestEqual(TEXT("root should cite verifier role"), RootRoleId, FString(TEXT("verifier")));
	TestEqual(TEXT("decision should cite verifier role"), DecisionRoleId, FString(TEXT("verifier")));
	TestEqual(TEXT("root should cite same recipe"), RootRecipeId, FString(TEXT("feature.interaction_access_slice_v1")));
	TestEqual(TEXT("decision should cite same recipe"), DecisionRecipeId, RootRecipeId);
	TestEqual(TEXT("root should cite recipe schema version"), static_cast<int32>(RootEvidenceSchemaVersion), 1);
	TestEqual(TEXT("decision should cite same schema version"), DecisionEvidenceSchemaVersion, RootEvidenceSchemaVersion);
	TestEqual(TEXT("decision object should cite deterministic result"), DecisionResultStatus, Decision.ResultStatus);
	TestTrue(TEXT("decision should resolve verifier role contract"), bRoleContractResolved);
	TestTrue(TEXT("closeout JSON should include role contract"),
		DecisionJson->HasTypedField<EJson::Object>(TEXT("role_contract")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_FallbackProviderSuccessRequiresCurrentProof,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.FallbackProviderSuccessRequiresCurrentProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_FallbackProviderSuccessRequiresCurrentProof::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_fallback_missing_current_proof");
	Plan.ReviewerPlanReference = TEXT("post_reattach_state_json_fallback_plan_v1");

	FUnrealClaudeActivePlanCloseoutDecision Decision;
	Decision.PlanStatus = TEXT("done");
	Decision.ResultStatus = TEXT("achieved_fully");

	const FUnrealClaudeActivePlanCloseoutDecision GatedDecision =
		SClaudeEditorWidget::ApplyActivePlanCloseoutSafetyGates(
			Plan,
			TEXT("status: full"),
			true,
			false,
			Decision);

	TestEqual(TEXT("fallback plan should not close done without typed proof"), GatedDecision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("fallback plan should not achieve without typed proof"), GatedDecision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(TEXT("fallback proof gate should explain the downgrade"), GatedDecision.GateReasonCode, FString(TEXT("fallback_closeout_proof_missing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExplicitFailureTextRejectsProviderSuccess,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.ExplicitFailureTextRejectsProviderSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_ExplicitFailureTextRejectsProviderSuccess::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_current");
	Plan.ReviewerPlanReference = TEXT("reviewer_plan_current");

	FUnrealClaudeActivePlanCloseoutDecision Decision;
	Decision.PlanStatus = TEXT("done");
	Decision.ResultStatus = TEXT("achieved_fully");

	const FUnrealClaudeActivePlanCloseoutDecision GatedDecision =
		SClaudeEditorWidget::ApplyActivePlanCloseoutSafetyGates(
			Plan,
			TEXT("Final result: failed because the visual proof could not be produced."),
			true,
			true,
			Decision);

	TestEqual(TEXT("explicit failure text should override provider success"), GatedDecision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("explicit failure text should not achieve"), GatedDecision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(TEXT("failure text gate should be recorded"), GatedDecision.GateReasonCode, FString(TEXT("final_response_declares_failure")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExplicitManualAssetBlockerTextStaysPrecise,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.ExplicitManualAssetBlockerTextStaysPrecise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_ExplicitMechanicInputConflictTextStaysPrecise,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.ExplicitMechanicInputConflictTextStaysPrecise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_ExplicitManualAssetBlockerTextStaysPrecise::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_current_asset_blocker");
	Plan.ReviewerPlanReference = TEXT("reviewer_plan_current");

	FUnrealClaudeActivePlanCloseoutDecision Decision;
	Decision.PlanStatus = TEXT("done");
	Decision.ResultStatus = TEXT("achieved_fully");

	const FUnrealClaudeActivePlanCloseoutDecision GatedDecision =
		SClaudeEditorWidget::ApplyActivePlanCloseoutSafetyGates(
			Plan,
			TEXT("Status: failed\nmanual_asset_dependency_blocker: requested Marketplace/Fab pack is not available locally."),
			true,
			true,
			Decision);

	TestEqual(TEXT("explicit manual asset blocker should override provider success"),
		GatedDecision.PlanStatus,
		FString(TEXT("failed")));
	TestEqual(TEXT("explicit manual asset blocker should preserve exact gate"),
		GatedDecision.GateReasonCode,
		FString(TEXT("manual_asset_dependency_blocker")));
	TestNotEqual(TEXT("explicit manual asset blocker should not collapse to assistant_reported_failure"),
		GatedDecision.BlockerFamily,
		FString(TEXT("assistant_reported_failure")));
	return true;
}

bool FActivePlanCloseout_ExplicitMechanicInputConflictTextStaysPrecise::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_current_input_conflict");
	Plan.ReviewerPlanReference = TEXT("reviewer_plan_current");

	FUnrealClaudeActivePlanCloseoutDecision Decision;
	Decision.PlanStatus = TEXT("done");
	Decision.ResultStatus = TEXT("achieved_fully");

	const FUnrealClaudeActivePlanCloseoutDecision GatedDecision =
		SClaudeEditorWidget::ApplyActivePlanCloseoutSafetyGates(
			Plan,
			TEXT("Status: failed\nmechanic_input_conflict_unresolved: Space+Shift conflicts with existing flight input."),
			true,
			true,
			Decision);

	TestEqual(TEXT("explicit mechanic conflict should override provider success"),
		GatedDecision.PlanStatus,
		FString(TEXT("failed")));
	TestEqual(TEXT("explicit mechanic conflict should preserve exact gate"),
		GatedDecision.GateReasonCode,
		FString(TEXT("mechanic_input_conflict_unresolved")));
	TestNotEqual(TEXT("explicit mechanic conflict should not collapse to assistant_reported_failure"),
		GatedDecision.BlockerFamily,
		FString(TEXT("assistant_reported_failure")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_VisualReferenceRequiresProof,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.VisualReferenceRequiresProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_VisualReferenceRequiresProof::RunTest(const FString& Parameters)
{
	TestTrue(
		TEXT("Russian visual-reference wording should require visual proof"),
		SClaudeEditorWidget::DoesPromptRequireVisualProof(TEXT("Создай combat inventory как на картинке"), TArray<FString>()));

	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_visual");
	Plan.bVisualProofRequired = true;
	Plan.bVisualQaManifestRequired = true;

	const FUnrealClaudeActivePlanCloseoutDecision GatedDecision = SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);

	TestEqual(TEXT("visual reference should require manifest proof"), GatedDecision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("visual reference should not achieve without manifest"), GatedDecision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(TEXT("visual manifest missing gate should be recorded"), GatedDecision.GateReasonCode, FString(TEXT("visual_qa_manifest_required_missing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_VisualReferenceRejectsBareScreenshotPath,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.VisualReferenceRejectsBareScreenshotPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_VisualReferenceRejectsBareScreenshotPath::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_visual_bare_screenshot");
	Plan.bVisualProofRequired = true;
	Plan.bVisualQaManifestRequired = true;

	FUnrealClaudeActivePlanCloseoutDecision Decision;
	Decision.PlanStatus = TEXT("done");
	Decision.ResultStatus = TEXT("achieved_fully");

	const FUnrealClaudeActivePlanCloseoutDecision GatedDecision =
		SClaudeEditorWidget::ApplyActivePlanCloseoutSafetyGates(
			Plan,
			TEXT("status: full. Visual proof artifact path: Saved/UnrealClaude/Screenshots/combat_inventory.png"),
			true,
			true,
			Decision);

	TestEqual(TEXT("bare screenshot path should not satisfy visual QA gate"), GatedDecision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("bare screenshot path should not keep achieved result"), GatedDecision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(TEXT("manifest gate should explain missing manifest"), GatedDecision.GateReasonCode, FString(TEXT("visual_qa_manifest_required_missing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_VisualReferenceAllowsPassedManifestWithScreenshotPath,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.VisualReferenceAllowsPassedManifestWithScreenshotPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_VisualReferenceAllowsPassedManifestWithScreenshotPath::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeActivePlanCloseoutTestRoot(TEXT("VisualReferenceAllowsPassedManifestWithScreenshotPath"));

	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_visual_manifest_passed");
	Plan.bVisualProofRequired = true;
	Plan.bVisualQaManifestRequired = true;
	const FString ManifestPath = WriteVisualQaManifestForTest(TestRoot, TEXT("passed"), true, Plan.PlanId);
	Plan.VisualQaManifestPath = ManifestPath;

	const FUnrealClaudeActivePlanCloseoutDecision Decision = SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	TestEqual(TEXT("passed manifest should satisfy visual QA gate"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("passed manifest should keep achieved result"), Decision.ResultStatus, FString(TEXT("achieved_fully")));

	FUnrealClaudeActivePlanCloseoutDecision TransportDecision;
	TransportDecision.PlanStatus = TEXT("done");
	TransportDecision.ResultStatus = TEXT("achieved_fully");
	const FUnrealClaudeActivePlanCloseoutDecision GatedDecision =
		SClaudeEditorWidget::ApplyActivePlanCloseoutSafetyGates(
			Plan,
			FString::Printf(TEXT("status: full. Visual QA manifest: %s"), *ManifestPath),
			true,
			true,
			TransportDecision);
	TestEqual(TEXT("passed manifest response should pass safety gate"), GatedDecision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("passed manifest response should remain fully achieved"), GatedDecision.ResultStatus, FString(TEXT("achieved_fully")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_VisualReferenceAllowsManualMechanicsChecklist,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.VisualReferenceAllowsManualMechanicsChecklist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_VisualReferenceAllowsManualMechanicsChecklist::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeActivePlanCloseoutTestRoot(TEXT("VisualReferenceAllowsManualMechanicsChecklist"));

	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_visual_manifest_manual_mechanics");
	Plan.bVisualProofRequired = true;
	Plan.bVisualQaManifestRequired = true;
	const FString ManifestPath = WriteVisualQaManifestForTest(TestRoot, TEXT("passed"), true, Plan.PlanId, true);
	Plan.VisualQaManifestPath = ManifestPath;

	const FUnrealClaudeActivePlanCloseoutDecision Decision = SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	TestEqual(TEXT("manual mechanics checklist should not block passed visual manifest"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("manual mechanics checklist should not downgrade visual closeout"), Decision.ResultStatus, FString(TEXT("achieved_fully")));

	FUnrealClaudeActivePlanCloseoutDecision TransportDecision;
	TransportDecision.PlanStatus = TEXT("done");
	TransportDecision.ResultStatus = TEXT("achieved_fully");
	const FUnrealClaudeActivePlanCloseoutDecision GatedDecision =
		SClaudeEditorWidget::ApplyActivePlanCloseoutSafetyGates(
			Plan,
			FString::Printf(
				TEXT("status: full. Visual QA manifest: %s. Mechanics checklist remains manual and is not visual closeout proof."),
				*ManifestPath),
			true,
			true,
			TransportDecision);
	TestEqual(TEXT("passed visual manifest with manual mechanics should pass safety gate"), GatedDecision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("passed visual manifest with manual mechanics should remain achieved"), GatedDecision.ResultStatus, FString(TEXT("achieved_fully")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_VisualReferenceFailedManifestBlocksCloseout,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.VisualReferenceFailedManifestBlocksCloseout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_VisualReferenceFailedManifestBlocksCloseout::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeActivePlanCloseoutTestRoot(TEXT("VisualReferenceFailedManifestBlocksCloseout"));

	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_visual_manifest_failed");
	Plan.bVisualProofRequired = true;
	Plan.bVisualQaManifestRequired = true;
	const FString ManifestPath = WriteVisualQaManifestForTest(TestRoot, TEXT("failed"), true, Plan.PlanId);
	Plan.VisualQaManifestPath = ManifestPath;

	const FUnrealClaudeActivePlanCloseoutDecision Decision = SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	TestEqual(TEXT("failed manifest should block closeout"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("failed manifest should not achieve"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(TEXT("failed manifest gate should be recorded"), Decision.GateReasonCode, FString(TEXT("visual_qa_manifest_failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_VisualReferenceRejectsStaleManifestPlanId,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.VisualReferenceRejectsStaleManifestPlanId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_VisualReferenceRejectsStaleManifestPlanId::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeActivePlanCloseoutTestRoot(TEXT("VisualReferenceRejectsStaleManifestPlanId"));
	const FString ManifestPath = WriteVisualQaManifestForTest(TestRoot, TEXT("passed"), true, TEXT("plan_visual_manifest_stale"));

	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_visual_manifest_current");
	Plan.bVisualProofRequired = true;
	Plan.bVisualQaManifestRequired = true;
	Plan.VisualQaManifestPath = ManifestPath;

	const FUnrealClaudeActivePlanCloseoutDecision Decision = SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	TestEqual(TEXT("stale manifest should block closeout"), Decision.PlanStatus, FString(TEXT("failed")));
	TestEqual(TEXT("stale manifest should not achieve"), Decision.ResultStatus, FString(TEXT("not_achieved")));
	TestEqual(
		TEXT("stale manifest gate should be recorded"),
		Decision.GateReasonCode,
		FString(TEXT("visual_qa_manifest_stale_plan_mismatch")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActivePlanCloseout_NonVisualTaskIgnoresVisualQaGate,
	"UnrealClaude.ActivePlanCloseout.RestartSurvival.NonVisualTaskIgnoresVisualQaGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActivePlanCloseout_NonVisualTaskIgnoresVisualQaGate::RunTest(const FString& Parameters)
{
	FUnrealClaudeActivePlan Plan;
	Plan.PlanId = TEXT("plan_non_visual");

	const FUnrealClaudeActivePlanCloseoutDecision Decision = SClaudeEditorWidget::EvaluateActivePlanCloseout(Plan, true);
	TestEqual(TEXT("non-visual task should still close on response success"), Decision.PlanStatus, FString(TEXT("done")));
	TestEqual(TEXT("non-visual task should remain fully achieved"), Decision.ResultStatus, FString(TEXT("achieved_fully")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
