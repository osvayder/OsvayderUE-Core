// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "OsvayderEditorWidget.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "OsvayderUECanonRouting.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FAgentCanonExecution MakeInteractionAccessExecution()
	{
		TArray<FAgentPromptContextBlock> ContextBlocks;
		return OsvayderUECanonRouting::BuildInitialCanonExecution(
			TEXT("Continue the prison-access slice on the existing proof map and persistent input assets without recreating them."),
			EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
			ContextBlocks);
	}

	void PrimeWorkflowToPhase(FAgentFeatureWorkflowState& Workflow, const FString& CurrentPhase)
	{
		Workflow.CurrentPhase = CurrentPhase;
		Workflow.CompletedPhaseIds.Reset();
		for (FAgentFeatureWorkflowPhaseState& Phase : Workflow.Phases)
		{
			if (Phase.PhaseId == CurrentPhase)
			{
				Phase.Status = TEXT("pending");
				continue;
			}

			const bool bBeforeCurrentPhase =
				Workflow.FindPhaseIndex(Phase.PhaseId) < Workflow.FindPhaseIndex(CurrentPhase);
			Phase.Status = bBeforeCurrentPhase ? TEXT("completed") : TEXT("pending");
			if (bBeforeCurrentPhase)
			{
				Workflow.CompletedPhaseIds.AddUnique(Phase.PhaseId);
			}
		}
	}

	FOsvayderUEActivePlan MakeInteractionAccessPlan(FAgentCanonExecution& OutExecution, const FString& CurrentPhase)
	{
		OutExecution = MakeInteractionAccessExecution();

		FOsvayderUEActivePlan Plan;
		Plan.PlanId = TEXT("plan_650_feature_workflow");
		Plan.Status = TEXT("active");
		Plan.ResultStatus = TEXT("incomplete");
		Plan.OriginalUserTask =
			TEXT("Reuse /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof and /Game/PrisonAccess/Input/IMC_PrisonAccessProof.");
		Plan.FeatureWorkflow = OutExecution.FeatureWorkflow;
		PrimeWorkflowToPhase(Plan.FeatureWorkflow, CurrentPhase);
		Plan.CurrentMechanicId = CurrentPhase;
		return Plan;
	}

	FAgentRunEvent MakeToolEvent(
		const FString& ToolName,
		const FString& ToolCallId,
		const FString& ToolInput,
		const FString& ToolResultContent = FString())
	{
		FAgentRunEvent Event;
		Event.Backend = EOsvayderUEProviderBackend::CodexCli;
		Event.Type = ToolResultContent.IsEmpty() ? EAgentRunEventType::ToolUse : EAgentRunEventType::ToolResult;
		Event.ToolName = ToolName;
		Event.ToolCallId = ToolCallId;
		Event.ToolInput = ToolInput;
		Event.ToolResultContent = ToolResultContent;
		return Event;
	}

	FAgentRunEvent MakeAutomationRunTestsEvent(
		const FString& ToolCallId,
		const FString& ToolResultContent = FString(),
		const bool bIsError = false)
	{
		const FString Command = TEXT("Automation RunTests Alternative.PrisonAccess");
		FAgentRunEvent Event = MakeToolEvent(TEXT("execute_terminal"), ToolCallId, Command, ToolResultContent);
		Event.RawJson = TEXT("{\"command\":\"Automation RunTests Alternative.PrisonAccess\"}");
		Event.bIsError = bIsError;
		return Event;
	}

	FAgentRunEvent MakeCommandExecutionEvent(
		const FString& ToolCallId,
		const FString& Command,
		const FString& ToolResultContent = FString(),
		const bool bIsError = false)
	{
		FAgentRunEvent Event = MakeToolEvent(TEXT("command_execution"), ToolCallId, Command, ToolResultContent);
		Event.RawJson = FString::Printf(TEXT("{\"command\":\"%s\"}"), *Command.ReplaceCharWithEscapedChar());
		Event.bIsError = bIsError;
		return Event;
	}

	FAgentRunEvent MakeCommandExecutionResultWithEmptyInputEvent(
		const FString& ToolCallId,
		const FString& RawJson,
		const FString& ToolResultContent,
		const bool bIsError = false)
	{
		FAgentRunEvent Event = MakeToolEvent(TEXT("command_execution"), ToolCallId, FString(), ToolResultContent);
		Event.RawJson = RawJson;
		Event.bIsError = bIsError;
		return Event;
	}

	void ApplyToolRoundTrip(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FAgentRunEvent& StartEvent,
		const FAgentRunEvent& ResultEvent)
	{
		SOsvayderEditorWidget::ApplyFeatureWorkflowToolBoundaryForTest(Plan, Execution, StartEvent, true);
		SOsvayderEditorWidget::ApplyFeatureWorkflowToolBoundaryForTest(Plan, Execution, ResultEvent, false);
	}

	void ApplyInteractionAccessAttemptResolverSourceInspection(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FString& ToolCallId)
	{
		const FString SourceCommand =
			TEXT("Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp; Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp");
		const FString SourceResult =
			TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp: FAlternativePrisonAccessAttemptResolver FAlternativePrisonAccessAttemptOutcome ResolveDoorAttempt ResolveTechnicalBoxAttempt\n")
			TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp: UAlternativePrisonAccessEventSubsystem FAlternativePrisonAccessEventRecord RecordEvent PrisonAccessEvent order=");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(ToolCallId, SourceCommand),
			MakeCommandExecutionEvent(ToolCallId, SourceCommand, SourceResult));
	}

	void ApplyLiveReadOnlyInputAuthoringContextBurst(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution)
	{
		auto ApplyProbe = [&Plan, &Execution](
			const FString& ToolCallId,
			const FString& Command,
			const FString& Result)
		{
			ApplyToolRoundTrip(
				Plan,
				Execution,
				MakeCommandExecutionEvent(ToolCallId, Command),
				MakeCommandExecutionEvent(ToolCallId, Command, Result));
		};

		ApplyProbe(
			TEXT("call_A73NK4DYjh5vKALdEellpcAG"),
			TEXT("Get-Content Saved/OsvayderUE/active_plan.json"),
			TEXT("\"recipe_id\":\"feature.interaction_access_slice_v1\",\"current_phase\":\"input_asset_authoring\",\"completed_phase_ids\":[\"project_context_preflight\",\"interaction_contract\"]"));
		ApplyProbe(
			TEXT("call_7L3nzgtIdQ9NAB0uReg5jPTg"),
			TEXT("Get-Content Docs/OsvayderUE/prison_access_slice_v2.md"),
			TEXT("Use existing proof map /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof and existing input IMC_PrisonAccessProof."));
		ApplyProbe(
			TEXT("call_kfmvZ2vcKJeCmL38drmOpSpE"),
			TEXT("Select-String -Path Docs/OsvayderUE/*.md -Pattern \"Alternative.PrisonAccess|ProofFixtureSmoke|PrisonAccessEvent\""),
			TEXT("Alternative.PrisonAccess has ProofFixtureSmoke success and PrisonAccessEvent runtime smoke lines."));
		ApplyProbe(
			TEXT("call_w9NXqva16jInqisuXDrIUwIb"),
			TEXT("Get-ChildItem Source/Alternative/PrisonAccess -Filter *.cpp"),
			TEXT("AlternativePrisonAccessAttemptResolver.cpp AlternativePrisonAccessEventSubsystem.cpp AlternativePrisonAccessRuntimeInput.cpp"));
		ApplyProbe(
			TEXT("call_GsSpwHCNfN32XSW7n7HR2hfD"),
			TEXT("Select-String -Path Source/Alternative/PrisonAccess/*.cpp -Pattern \"IMC_PrisonAccessProof|IA_Interact|PrisonAccess\""),
			TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessRuntimeInput.cpp: /Game/PrisonAccess/Input/IMC_PrisonAccessProof and /Game/Variant_SideScrolling/Input/Actions/IA_Interact"));
		ApplyProbe(
			TEXT("call_jTHU61ivnDR3bp1bubTplnAj"),
			TEXT("Get-ChildItem Saved/Logs -Filter \"*packet654*\""),
			TEXT("packet654_review5_acceptance_stdout_20260425_131341.log contains Found 7 automation tests based on Alternative.PrisonAccess"));
	}

	void ApplyInteractionAccessContractCompletion(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution)
	{
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeToolEvent(TEXT("asset_search"), TEXT("call_live_interaction_contract"), TEXT("{\"query\":\"Lvl_PrisonAccessProof\"}")),
			MakeToolEvent(
				TEXT("asset_search"),
				TEXT("call_live_interaction_contract"),
				TEXT("{\"query\":\"Lvl_PrisonAccessProof\"}"),
				TEXT("{\"assets\":[{\"path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof.Lvl_PrisonAccessProof\",\"name\":\"Lvl_PrisonAccessProof\"}]}")));
	}

	void ApplyInputReuseEvidence(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FString& ToolPrefix = FString())
	{
		const FString AssetSearchTool = ToolPrefix + TEXT("asset_search");
		const FString EnhancedInputTool = ToolPrefix + TEXT("enhanced_input");

		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeToolEvent(AssetSearchTool, TEXT("call_live_map_search"), TEXT("{\"path_filter\":\"/Game/PrisonAccess/Maps\",\"name_pattern\":\"Lvl_PrisonAccessProof\",\"limit\":10}")),
			MakeToolEvent(
				AssetSearchTool,
				TEXT("call_live_map_search"),
				TEXT("{\"path_filter\":\"/Game/PrisonAccess/Maps\",\"name_pattern\":\"Lvl_PrisonAccessProof\",\"limit\":10}"),
				TEXT("{\"assets\":[{\"path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof.Lvl_PrisonAccessProof\",\"name\":\"Lvl_PrisonAccessProof\"}]}")));
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeToolEvent(AssetSearchTool, TEXT("call_live_imc_prefixed"), TEXT("{\"path_filter\":\"/Game/PrisonAccess/Input\",\"name_pattern\":\"IMC_PrisonAccessProof\",\"limit\":10}")),
			MakeToolEvent(
				AssetSearchTool,
				TEXT("call_live_imc_prefixed"),
				TEXT("{\"path_filter\":\"/Game/PrisonAccess/Input\",\"name_pattern\":\"IMC_PrisonAccessProof\",\"limit\":10}"),
				TEXT("{\"assets\":[{\"path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\",\"name\":\"IMC_PrisonAccessProof\"}]}")));
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeToolEvent(AssetSearchTool, TEXT("call_live_ia_prefixed"), TEXT("{\"path_filter\":\"/Game/Variant_SideScrolling/Input/Actions\",\"name_pattern\":\"IA_Interact\",\"limit\":10}")),
			MakeToolEvent(
				AssetSearchTool,
				TEXT("call_live_ia_prefixed"),
				TEXT("{\"path_filter\":\"/Game/Variant_SideScrolling/Input/Actions\",\"name_pattern\":\"IA_Interact\",\"limit\":10}"),
				TEXT("{\"assets\":[{\"path\":\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\",\"name\":\"IA_Interact\"}]}")));

		const FString QueryInput =
			TEXT("{\"operation\":\"query_context\",\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"}");
		const FString QueryResult =
			TEXT("{\"tool_name\":\"enhanced_input\",\"status\":\"completed\",\"persistent_project_input\":true,\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\",\"mappings\":[{\"action_path\":\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\",\"key\":\"E\"}]}");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeToolEvent(EnhancedInputTool, TEXT("call_live_query_prefixed"), QueryInput),
			MakeToolEvent(EnhancedInputTool, TEXT("call_live_query_prefixed"), QueryInput, QueryResult));
	}

	void ApplyRuntimeActorEvidence(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FString& ToolPrefix = FString(),
		const FString& OpenCallId = TEXT("call_live_open_map"),
		const FString& ActorCallId = TEXT("call_live_actor_observation"))
	{
		const FString OpenLevelTool = ToolPrefix + TEXT("open_level");
		const FString GetLevelActorsTool = ToolPrefix + TEXT("get_level_actors");
		const FString OpenLevelInput =
			TEXT("{\"action\":\"open\",\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeToolEvent(OpenLevelTool, OpenCallId, OpenLevelInput),
			MakeToolEvent(OpenLevelTool, OpenCallId, OpenLevelInput, TEXT("Opened level: Lvl_PrisonAccessProof")));

		const FString ActorQueryInput = TEXT("{\"brief\":false,\"limit\":100,\"include_hidden\":true}");
		const FString ActorQueryResult =
			TEXT("Found 11 actors: WorldSettings, Proof_PrisonAccess_Door, Proof_PrisonAccess_ControlBox, Proof_PrisonAccess_EscapeItem, AlternativeStorageDoor_0, AlternativeTechnicalBox_0");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeToolEvent(GetLevelActorsTool, ActorCallId, ActorQueryInput),
			MakeToolEvent(GetLevelActorsTool, ActorCallId, ActorQueryInput, ActorQueryResult));
	}

	void ApplyReview13RuntimeActorSourceInspectionError(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FString& ToolCallId)
	{
		const FString SourceProbeCommand =
			TEXT("rg -n \"PrisonAccess|Proof_PrisonAccess\" Source/Alternative/PrisonAccess/*.h Source/Alternative/PrisonAccess/*.cpp Source/Alternative/PrisonAccess/Tests/*.cpp");
		const FString SourceProbeResult =
			TEXT("rg: Source/Alternative/PrisonAccess/*.h: syntax error in file name. (os error 123)\n")
			TEXT("rg: Source/Alternative/PrisonAccess/*.cpp: syntax error in file name. (os error 123)\n")
			TEXT("rg: Source/Alternative/PrisonAccess/Tests/*.cpp: syntax error in file name. (os error 123)");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(ToolCallId, SourceProbeCommand),
			MakeCommandExecutionEvent(ToolCallId, SourceProbeCommand, SourceProbeResult, true));
	}

	void ApplyAttemptResolverSourceEvidence(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution)
	{
		const FString SourceCommand =
			TEXT("Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.h; Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp; Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.h; Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp");
		const FString SourceRawJson =
			TEXT("{\"type\":\"commandExecution\",\"id\":\"call_live_source_shape\",\"command\":\"Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.h; Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp\",\"status\":\"completed\"}");
		const FString SourceResult =
			TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.h:9: struct ALTERNATIVE_API FAlternativePrisonAccessAttemptResolver\n")
			TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.cpp:16: FAlternativePrisonAccessAttemptOutcome FAlternativePrisonAccessAttemptResolver::ResolveDoorAttempt(\n")
			TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.h:12: class UAlternativePrisonAccessEventSubsystem\n")
			TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.cpp:18: void UAlternativePrisonAccessEventSubsystem::RecordEvent(const FAlternativePrisonAccessEventRecord& Record)\n")
			TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.cpp:29: TEXT(\"PrisonAccessEvent order=%d actor=%s target=%s attempt=%d result=%d noisy=%s tag=%s\")");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(TEXT("call_live_source_shape"), SourceCommand),
			MakeCommandExecutionResultWithEmptyInputEvent(
				TEXT("call_live_source_shape"),
				SourceRawJson,
				SourceResult));
	}

	void ApplyAutomationDiscoveryEvidence(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution)
	{
		const FString AutomationResult =
			TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={AuthorizedRoute} Path={Alternative.PrisonAccess.AuthorizedRoute}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={InputPathTruth} Path={Alternative.PrisonAccess.InputPathTruth}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
			TEXT("LogAlternative: Display: PrisonAccessEvent order=1 actor=subject.player.technician target=AlternativeTechnicalBox_0 attempt=1 result=3 noisy=false tag=attempt.technical.override_success\n")
			TEXT("LogAlternative: Display: PrisonAccessEvent order=2 actor=subject.player.technician target=AlternativeStorageDoor_0 attempt=0 result=4 noisy=false tag=attempt.access.open_existing\n")
			TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeAutomationRunTestsEvent(TEXT("call_live_automation")),
			MakeAutomationRunTestsEvent(TEXT("call_live_automation"), AutomationResult));
	}

	void ApplyRuntimeProofEvidence(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution)
	{
		const FString RuntimeProofInput =
			TEXT("{\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\",\"proof\":\"Alternative.PrisonAccess\"}");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeToolEvent(TEXT("map_runtime_proof"), TEXT("call_live_runtime_proof"), RuntimeProofInput),
			MakeToolEvent(
				TEXT("map_runtime_proof"),
				TEXT("call_live_runtime_proof"),
				RuntimeProofInput,
				TEXT("Runtime proof passed for /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof with Alternative.PrisonAccess")));
	}

	void ApplyProjectContextPreflight(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FString& ToolCallId = TEXT("call_project_context_preflight"))
	{
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(ToolCallId, TEXT("Write-Output project_context_preflight_ready")),
			MakeCommandExecutionEvent(
				ToolCallId,
				TEXT("Write-Output project_context_preflight_ready"),
				TEXT("Project context preflight completed for feature.interaction_access_slice_v1")));
	}

	FAgentRunEvent MakeReview10SourceReadStartEvent(
		const FString& ToolCallId,
		const FString& SourcePath)
	{
		const FString Command = FString::Printf(
			TEXT("\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -Command \"Get-Content -LiteralPath '%s'\""),
			*SourcePath);
		return MakeCommandExecutionEvent(ToolCallId, Command);
	}

	FAgentRunEvent MakeReview10SourceReadResultEvent(
		const FString& ToolCallId,
		const FString& SourceText)
	{
		FAgentRunEvent Event = MakeCommandExecutionResultWithEmptyInputEvent(
			ToolCallId,
			FString::Printf(TEXT("{\"type\":\"commandExecution\",\"id\":\"%s\",\"status\":\"completed\",\"exitCode\":0}"), *ToolCallId),
			SourceText);
		Event.bIsError = false;
		return Event;
	}

	void StartReview10DelayedSourceReads(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution)
	{
		const TPair<FString, FString> Calls[] = {
			{ TEXT("call_review10_attempt_h"), TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.h") },
			{ TEXT("call_review10_attempt_cpp"), TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp") },
			{ TEXT("call_review10_event_cpp"), TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp") },
			{ TEXT("call_review10_event_h"), TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.h") },
			{ TEXT("call_review10_attempt_cpp_confirm"), TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp") },
			{ TEXT("call_review10_event_cpp_confirm"), TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp") }
		};

		for (const TPair<FString, FString>& Call : Calls)
		{
			SOsvayderEditorWidget::ApplyFeatureWorkflowToolBoundaryForTest(
				Plan,
				Execution,
				MakeReview10SourceReadStartEvent(Call.Key, Call.Value),
				true);
		}
	}

	void CompleteReview10DelayedSourceReads(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution)
	{
		const TPair<FString, FString> Results[] = {
			{ TEXT("call_review10_attempt_h"), TEXT("struct ALTERNATIVE_API FAlternativePrisonAccessAttemptResolver { static FAlternativePrisonAccessAttemptOutcome ResolveDoorAttempt(const AActor* Interactor); static FAlternativePrisonAccessAttemptOutcome ResolveTechnicalBoxAttempt(const AActor* Interactor); };") },
			{ TEXT("call_review10_attempt_cpp"), TEXT("FAlternativePrisonAccessAttemptOutcome FAlternativePrisonAccessAttemptResolver::ResolveDoorAttempt(...) { Outcome.ResultTag = TEXT(\"attempt.access.open_existing\"); } FAlternativePrisonAccessAttemptOutcome FAlternativePrisonAccessAttemptResolver::ResolveTechnicalBoxAttempt(...) { Outcome.ResultTag = TEXT(\"attempt.technical.override_success\"); }") },
			{ TEXT("call_review10_event_cpp"), TEXT("void UAlternativePrisonAccessEventSubsystem::RecordEvent(const FAlternativePrisonAccessEventRecord& Record) { UE_LOG(LogAlternative, Display, TEXT(\"PrisonAccessEvent order=%d tag=%s\")); }") },
			{ TEXT("call_review10_event_h"), TEXT("class UAlternativePrisonAccessEventSubsystem : public UWorldSubsystem { void RecordEvent(const FAlternativePrisonAccessEventRecord& Record); const TArray<FAlternativePrisonAccessEventRecord>& GetRecordedEvents() const; };") },
			{ TEXT("call_review10_attempt_cpp_confirm"), TEXT("BuildCharacterAccessContext ResolveDoorAttemptFromContext ResolveTechnicalBoxAttemptFromContext FAlternativePrisonAccessAttemptOutcome") },
			{ TEXT("call_review10_event_cpp_confirm"), TEXT("FAlternativePrisonAccessEventRecord RecordEvent PrisonAccessEvent ResultTag") }
		};

		for (const TPair<FString, FString>& Result : Results)
		{
			SOsvayderEditorWidget::ApplyFeatureWorkflowToolBoundaryForTest(
				Plan,
				Execution,
				MakeReview10SourceReadResultEvent(Result.Key, Result.Value),
				false);
		}
	}

	FString MakeReview9AutomationLogResult()
	{
		return TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={AuthorizedRoute} Path={Alternative.PrisonAccess.AuthorizedRoute}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={InputPathTruth} Path={Alternative.PrisonAccess.InputPathTruth}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={TechnicalRoute} Path={Alternative.PrisonAccess.TechnicalRoute}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={UnskilledRoute} Path={Alternative.PrisonAccess.UnskilledRoute}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={NoisyFailure} Path={Alternative.PrisonAccess.NoisyFailure}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={FixtureIntegrity} Path={Alternative.PrisonAccess.FixtureIntegrity}\n")
			TEXT("LogAlternative: Display: PrisonAccessEvent order=1 actor=subject.player.technician target=AlternativeTechnicalBox_0 attempt=1 result=3 noisy=false tag=attempt.technical.override_success\n")
			TEXT("LogAlternative: Display: PrisonAccessEvent order=2 actor=subject.player.technician target=AlternativeStorageDoor_0 attempt=0 result=4 noisy=false tag=attempt.access.open_existing\n")
			TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");
	}

	FString MakeReview11NoisyAbsLogResult(const bool bIncludePassingProofBlock)
	{
		FString LogText =
			TEXT("LogInit: Command Line: -DDC-ForceMemoryCache -ExecCmds=\"Automation RunTests Alternative.PrisonAccess;Quit\"\n")
			TEXT("LogDerivedDataCache: Warning: writable node unavailable before automation boot\n")
			TEXT("LogAutomationTest: Error: Condition failed\n")
			TEXT("LogHttpListener: Error: failed to bind port 3000\n")
			TEXT("LogAssetRegistry: Error: failed to delete cached asset registry\n");
		if (!bIncludePassingProofBlock)
		{
			LogText += TEXT("LogAutomationCommandLine: Display: Ready to start automation\n")
				TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");
			return LogText;
		}

		LogText +=
			TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.AuthorizedRoute\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.ForceRoute\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.InputPathTruth\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.ProofFixturePresence\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.ProofFixtureSmoke\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.TechnicalRoute\n")
			TEXT("LogAutomationCommandLine: Display: \tAlternative.PrisonAccess.UnskilledRoute\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={AuthorizedRoute} Path={Alternative.PrisonAccess.AuthorizedRoute}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ForceRoute} Path={Alternative.PrisonAccess.ForceRoute}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={InputPathTruth} Path={Alternative.PrisonAccess.InputPathTruth}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixturePresence} Path={Alternative.PrisonAccess.ProofFixturePresence}\n")
			TEXT("LogAutomationController: Display: Test Started. Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
			TEXT("LogAlternative: Display: PrisonAccessEvent order=1 actor=subject.player.technician target=AlternativeTechnicalBox_0 attempt=1 result=3 noisy=false tag=attempt.technical.override_success\n")
			TEXT("LogAlternative: Display: PrisonAccessEvent order=2 actor=subject.player.technician target=AlternativeStorageDoor_0 attempt=0 result=4 noisy=false tag=attempt.access.open_existing\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={TechnicalRoute} Path={Alternative.PrisonAccess.TechnicalRoute}\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={UnskilledRoute} Path={Alternative.PrisonAccess.UnskilledRoute}\n")
			TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");
		return LogText;
	}

	FString WriteReview11AutomationLogFixture(const FString& Suffix, const FString& Content)
	{
		const FString LogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"));
		IFileManager::Get().MakeDirectory(*LogDir, true);
		const FString FixtureLogPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(LogDir, FString::Printf(TEXT("packet654_review11_%s.log"), *Suffix)));
		FFileHelper::SaveStringToFile(Content, *FixtureLogPath);
		return FixtureLogPath;
	}

	void ApplyReview11AbsLogAutomationEvidence(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FString& ToolCallId,
		const FString& AbsLogPath)
	{
		const FString Command = FString::Printf(
			TEXT("& 'D:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' Alternative.uproject -DDC-ForceMemoryCache -stdout -FullStdOutLogOutput -ExecCmds='Automation RunTests Alternative.PrisonAccess;Quit' -TestExit='Automation Test Queue Empty' -AbsLog=\"%s\""),
			*AbsLogPath);
		const FString Result = FString::Printf(
			TEXT("ExitCode : 0\nAutomationLog : %s\nStdoutLog : Saved/Logs/packet654_review11_stdout.log\nExitCodeLog : Saved/Logs/packet654_review11_exitcode.txt"),
			*AbsLogPath);
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(ToolCallId, Command),
			MakeCommandExecutionEvent(ToolCallId, Command, Result));
	}

	void PrimeInteractionAccessProofPrereqsAndObservation(FOsvayderUEActivePlan& Plan)
	{
		Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
		Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bPersistentInputAssetObserved = true;
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bInteractionActionAssetObserved = true;
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bReadOnlyEnhancedInputQueryObserved = true;
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved = true;
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved = true;
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = true;
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = true;
	}

	void ApplyReview9AutomationLogEvidence(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FString& ToolCallId = TEXT("call_review9_automation_log"))
	{
		const FString Command =
			TEXT("Get-Content Saved/Logs/packet654_review9_live_acceptance_automation_20260425_153437.log");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(ToolCallId, Command),
			MakeCommandExecutionEvent(ToolCallId, Command, MakeReview9AutomationLogResult()));
	}

	void ApplyStdoutOnlyReview10AutomationEvidence(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FString& ToolCallId = TEXT("call_review10_stdout_automation"))
	{
		const FString Command =
			TEXT("& UnrealEditor-Cmd.exe Alternative.uproject -DDC-ForceMemoryCache -stdout ")
			TEXT("-ExecCmds='Automation RunTests Alternative.PrisonAccess;Quit' -log=Saved/Logs/missing_review10_abslog.log");
		const FString Result =
			TEXT("Get-Content: Cannot find path 'Saved/Logs/missing_review10_abslog.log' because it does not exist.\n")
			+ MakeReview9AutomationLogResult();
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(ToolCallId, Command),
			MakeCommandExecutionEvent(ToolCallId, Command, Result));
	}

	void ApplyCompileGateNoMutationInspection(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution,
		const FString& ToolCallId = TEXT("call_compile_gate_no_mutation"))
	{
		const FString Command =
			TEXT("Get-Content Saved/OsvayderUE/active_plan.json; git status --short; ")
			TEXT("Get-Content Plugins/OsvayderUE/Source/OsvayderUE/Private/OsvayderEditorWidget.cpp -TotalCount 1");
		const FString Result =
			TEXT("{\"feature_workflow\":{\"current_phase\":\"compile_gate\",\"compile_proof_required\":false,\"compile_proof_state\":\"pending\"},")
			TEXT("\"compile_proof\":{\"compiled_module_mutation_observed\":false}}\n")
			TEXT("No C++ mutation was performed in packet654_review9_live_acceptance; compile proof is not required for this verification-only rerun. ")
			TEXT("Source status is read-only evidence, not a patch.");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(ToolCallId, Command),
			MakeCommandExecutionEvent(ToolCallId, Command, Result));
	}

	void ApplyUbtLogBackupUnauthorizedResult(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& Execution)
	{
		const FString Command =
			TEXT("& 'D:/Program Files/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat' AlternativeEditor Win64 Development -Project='X:/PublicExample/Unreal/SampleProject/SampleProject.uproject'");
		const FString Result =
			TEXT("Unhandled exception: System.UnauthorizedAccessException: Access to the path 'C:/Users/ExampleUser/AppData/Local/UnrealBuildTool/Log.txt' is denied.\n")
			TEXT("   at EpicGames.Core.Log.BackupLogFile(FileReference LogFile)\n")
			TEXT("Result: Failed (OtherCompilationError)\n")
			TEXT("No C++ compiler diagnostics were emitted before the UBT log backup failure.");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(TEXT("call_ubt_log_backup_unauthorized"), Command),
			MakeCommandExecutionEvent(TEXT("call_ubt_log_backup_unauthorized"), Command, Result, true));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_RoleContractRoundTripsThroughJson,
	"OsvayderUE.FeatureWorkflowState.RoleContractRoundTripsThroughJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InputAssetAuthoringAllowsPersistentReuseVerification,
	"OsvayderUE.FeatureWorkflowState.InputAssetAuthoringAllowsPersistentReuseVerification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InputAssetAuthoringAccumulatesReadOnlyReuseAcrossMultipleTools,
	"OsvayderUE.FeatureWorkflowState.InputAssetAuthoringAccumulatesReadOnlyReuseAcrossMultipleTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InputAssetAuthoringRejectsWeakReuseEvidence,
	"OsvayderUE.FeatureWorkflowState.InputAssetAuthoringRejectsWeakReuseEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InputAssetAuthoringRejectsPartialAccumulatedReuseEvidence,
	"OsvayderUE.FeatureWorkflowState.InputAssetAuthoringRejectsPartialAccumulatedReuseEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InputAssetAuthoringIgnoresEarlyCommandExecutionNoiseAndReachesAttemptResolver,
	"OsvayderUE.FeatureWorkflowState.InputAssetAuthoringIgnoresEarlyCommandExecutionNoiseAndReachesAttemptResolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InteractionContractIgnoresOptionalContextFailuresAndReachesMemoryUpdate,
	"OsvayderUE.FeatureWorkflowState.InteractionContractIgnoresOptionalContextFailuresAndReachesMemoryUpdate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InteractionContractPreflightProofContextDoesNotSelfStop,
	"OsvayderUE.FeatureWorkflowState.InteractionContractPreflightProofContextDoesNotSelfStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InteractionContractUnrelatedReadOnlyDriftStillStopLosses,
	"OsvayderUE.FeatureWorkflowState.InteractionContractUnrelatedReadOnlyDriftStillStopLosses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InteractionContractMissingProofContextStillReportsPrerequisites,
	"OsvayderUE.FeatureWorkflowState.InteractionContractMissingProofContextStillReportsPrerequisites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InteractionAccessReview5LiveTraceReplayReachesMemoryUpdate,
	"OsvayderUE.FeatureWorkflowState.InteractionAccessReview5LiveTraceReplayReachesMemoryUpdate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InputAssetAuthoringDoesNotStopLossOnLiveReadOnlyContextBurst,
	"OsvayderUE.FeatureWorkflowState.InputAssetAuthoringDoesNotStopLossOnLiveReadOnlyContextBurst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InputAssetAuthoringStillStopsOnUnboundedIrrelevantCommandDrift,
	"OsvayderUE.FeatureWorkflowState.InputAssetAuthoringStillStopsOnUnboundedIrrelevantCommandDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_LiveProofEvidenceAfterContextBurstLatchesPrerequisites,
	"OsvayderUE.FeatureWorkflowState.LiveProofEvidenceAfterContextBurstLatchesPrerequisites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AttemptResolverEvidenceAfterContextBurstSerializesObservation,
	"OsvayderUE.FeatureWorkflowState.AttemptResolverEvidenceAfterContextBurstSerializesObservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_LiveMcpToolNamesCompleteInputAssetAuthoringBeforeDriftStopLoss,
	"OsvayderUE.FeatureWorkflowState.LiveMcpToolNamesCompleteInputAssetAuthoringBeforeDriftStopLoss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_RuntimeProofToolDoesNotBypassAttemptResolverObservation,
	"OsvayderUE.FeatureWorkflowState.RuntimeProofToolDoesNotBypassAttemptResolverObservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_OpenLevelAloneDoesNotMarkPlacedRuntimeActorsAvailable,
	"OsvayderUE.FeatureWorkflowState.OpenLevelAloneDoesNotMarkPlacedRuntimeActorsAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review13RuntimeActorStateHydratesAfterSourceInspectionErrors,
	"OsvayderUE.FeatureWorkflowState.Review13.RuntimeActorStateHydratesAfterSourceInspectionErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review13RuntimeActorStateRejectsNarrativeAndShellDrift,
	"OsvayderUE.FeatureWorkflowState.Review13.RuntimeActorStateRejectsNarrativeAndShellDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review14RuntimeProofCompletesFromBoundedAutomationProof,
	"OsvayderUE.FeatureWorkflowState.Review14.RuntimeProofCompletesFromBoundedAutomationProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review14AdHocRuntimeProofAttemptsStillStopLossWithoutAutomationProof,
	"OsvayderUE.FeatureWorkflowState.Review14.AdHocRuntimeProofAttemptsStillStopLossWithoutAutomationProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromAccumulatedSourceInspection,
	"OsvayderUE.FeatureWorkflowState.AttemptResolverAndLoggingCompletesFromAccumulatedSourceInspection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromRelativeCommandExecutionSourceInspection,
	"OsvayderUE.FeatureWorkflowState.AttemptResolverAndLoggingCompletesFromRelativeCommandExecutionSourceInspection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromLiveCommandExecutionResultShape,
	"OsvayderUE.FeatureWorkflowState.AttemptResolverAndLoggingCompletesFromLiveCommandExecutionResultShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromRuntimeSmokeLogReview,
	"OsvayderUE.FeatureWorkflowState.AttemptResolverAndLoggingCompletesFromRuntimeSmokeLogReview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromBoundedAutomationCommandExecution,
	"OsvayderUE.FeatureWorkflowState.AttemptResolverAndLoggingCompletesFromBoundedAutomationCommandExecution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AttemptResolverAndLoggingRejectsDocsAndArbitraryCommandDrift,
	"OsvayderUE.FeatureWorkflowState.AttemptResolverAndLoggingRejectsDocsAndArbitraryCommandDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AttemptResolverAndLoggingRejectsMutatingCommandExecutionDrift,
	"OsvayderUE.FeatureWorkflowState.AttemptResolverAndLoggingRejectsMutatingCommandExecutionDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AttemptResolverAndLoggingReadOnlySourceProgressAvoidsPrematureStopLoss,
	"OsvayderUE.FeatureWorkflowState.AttemptResolverAndLoggingReadOnlySourceProgressAvoidsPrematureStopLoss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_ProofContextObservationAdvancesWithoutMutation,
	"OsvayderUE.FeatureWorkflowState.ProofContextObservationAdvancesWithoutMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AutomationDiscoveryAcceptsRealSuccessMarkers,
	"OsvayderUE.FeatureWorkflowState.AutomationDiscoveryAcceptsRealSuccessMarkers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AutomationDiscoveryAcceptsSummaryCountsWithCleanExit,
	"OsvayderUE.FeatureWorkflowState.AutomationDiscoveryAcceptsSummaryCountsWithCleanExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AutomationDiscoveryZeroDiscoveryStillFails,
	"OsvayderUE.FeatureWorkflowState.AutomationDiscoveryZeroDiscoveryStillFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_CompileGateVerificationOnlyAutomationAdvances,
	"OsvayderUE.FeatureWorkflowState.CompileGateVerificationOnlyAutomationAdvances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_CompileGateDoesNotAdvanceOnArbitraryCommandExecution,
	"OsvayderUE.FeatureWorkflowState.CompileGateDoesNotAdvanceOnArbitraryCommandExecution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_CompileGateRequiredCompileBlocksAutomationAdvance,
	"OsvayderUE.FeatureWorkflowState.CompileGateRequiredCompileBlocksAutomationAdvance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review9VerificationOnlyFlowDoesNotParkAtCompileGate,
	"OsvayderUE.FeatureWorkflowState.Review9VerificationOnlyFlowDoesNotParkAtCompileGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_CompileGateNoCompileRequiredAdvancesWithoutUBT,
	"OsvayderUE.FeatureWorkflowState.CompileGateNoCompileRequiredAdvancesWithoutUBT",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_AutomationProofBeforeCompileGateIsCarriedForward,
	"OsvayderUE.FeatureWorkflowState.AutomationProofBeforeCompileGateIsCarriedForward",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review10FullLiveNewSessionTraceReachesMemoryUpdate,
	"OsvayderUE.FeatureWorkflowState.Review10FullLiveNewSessionTraceReachesMemoryUpdate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review10DelayedSourceResultsDoNotSpendInputPhaseStopLoss,
	"OsvayderUE.FeatureWorkflowState.Review10DelayedSourceResultsDoNotSpendInputPhaseStopLoss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_SourceInspectionEvidenceHydratesAttemptResolverObservationAcrossPhaseBoundaries,
	"OsvayderUE.FeatureWorkflowState.SourceInspectionEvidenceHydratesAttemptResolverObservationAcrossPhaseBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_InputAssetAuthoringUsesAccumulatedSeparateAssetAndEnhancedInputEvidence,
	"OsvayderUE.FeatureWorkflowState.InputAssetAuthoringUsesAccumulatedSeparateAssetAndEnhancedInputEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_StdoutOnlyAutomationLogCountsWhenAbsLogFileMissing,
	"OsvayderUE.FeatureWorkflowState.StdoutOnlyAutomationLogCountsWhenAbsLogFileMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review11AutomationDiscoveryGateAcceptsPassingAbsLogAfterNoise,
	"OsvayderUE.FeatureWorkflowState.Review11AutomationDiscoveryGateAcceptsPassingAbsLogAfterNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review11AutomationDiscoveryHydratesRuntimeSmokeAndPrisonAccessEvent,
	"OsvayderUE.FeatureWorkflowState.Review11AutomationDiscoveryHydratesRuntimeSmokeAndPrisonAccessEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review11AutomationNoiseDoesNotCauseFalseDiscoveryFailure,
	"OsvayderUE.FeatureWorkflowState.Review11AutomationNoiseDoesNotCauseFalseDiscoveryFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_Review11AutomationNoiseWithoutPassingProofStillFails,
	"OsvayderUE.FeatureWorkflowState.Review11AutomationNoiseWithoutPassingProofStillFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_ProjectLocalReadOnlyEvidenceDoesNotSpendStopLossBudgetWhenItAddsOrConfirmsEvidence,
	"OsvayderUE.FeatureWorkflowState.ProjectLocalReadOnlyEvidenceDoesNotSpendStopLossBudgetWhenItAddsOrConfirmsEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_CompileGateReadOnlyPlanInspectionDoesNotConsumeStopLossWhenCompileNotRequired,
	"OsvayderUE.FeatureWorkflowState.CompileGateReadOnlyPlanInspectionDoesNotConsumeStopLossWhenCompileNotRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_CompileGateArbitraryCommandDriftStillStopLosses,
	"OsvayderUE.FeatureWorkflowState.CompileGateArbitraryCommandDriftStillStopLosses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_UbtLogBackupUnauthorizedIsInfrastructureBlockerNotCompileFailure,
	"OsvayderUE.FeatureWorkflowState.UbtLogBackupUnauthorizedIsInfrastructureBlockerNotCompileFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_VerificationArchiveWriteWithSourceEvidenceDoesNotRequireCompileProof,
	"OsvayderUE.FeatureWorkflowState.VerificationArchiveWriteWithSourceEvidenceDoesNotRequireCompileProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeatureWorkflowState_RealSourceWriteStillRequiresCompileProof,
	"OsvayderUE.FeatureWorkflowState.RealSourceWriteStillRequiresCompileProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFeatureWorkflowState_RoleContractRoundTripsThroughJson::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution = MakeInteractionAccessExecution();
	Execution.FeatureWorkflow.RoleId = TEXT("worker");
	Execution.FeatureWorkflow.EvidenceSchemaVersion = 1;

	const TSharedPtr<FJsonObject> Json = Execution.FeatureWorkflow.ToJsonObject();
	const FAgentFeatureWorkflowState RoundTrip =
		FAgentFeatureWorkflowState::FromJsonObject(Json);

	TestEqual(TEXT("role id should serialize"), RoundTrip.RoleId, FString(TEXT("worker")));
	TestEqual(TEXT("recipe id should serialize"), RoundTrip.RecipeId, FString(TEXT("feature.interaction_access_slice_v1")));
	TestEqual(TEXT("evidence schema version should serialize"), RoundTrip.EvidenceSchemaVersion, 1);
	TestTrue(TEXT("role contract fields should keep workflow signal"), RoundTrip.HasAnySignal());
	return true;
}

bool FFeatureWorkflowState_InputAssetAuthoringAllowsPersistentReuseVerification::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	const FString ToolInput =
		TEXT("{\"operation\":\"query_context\",\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"}");
	const FString ToolResult =
		TEXT("{\"persistent_project_input\":true,\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\",\"mappings\":[{\"action_path\":\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\",\"key\":\"E\"}]}");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_input_reuse"), ToolInput),
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_input_reuse"), ToolInput, ToolResult));

	TestTrue(TEXT("input asset authoring should complete on truthful persistent reuse evidence"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("workflow should advance to runtime_actor_state"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_actor_state")));
	TestEqual(TEXT("authoring lane state should mark the reuse path explicitly"),
		Plan.FeatureWorkflow.AuthoringLaneState,
		FString(TEXT("persistent_input_reuse_verified")));
	TestEqual(TEXT("authoring decision should explain the reuse evidence path"),
		Plan.FeatureWorkflow.AuthoringDecision,
		FString(TEXT("persistent_input_assets_reuse_verified_via_project_evidence")));
	return true;
}

bool FFeatureWorkflowState_InputAssetAuthoringAccumulatesReadOnlyReuseAcrossMultipleTools::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("asset_search"), TEXT("call_reuse_imc_search"), TEXT("{\"query\":\"IMC_PrisonAccessProof\"}")),
		MakeToolEvent(
			TEXT("asset_search"),
			TEXT("call_reuse_imc_search"),
			TEXT("{\"query\":\"IMC_PrisonAccessProof\"}"),
			TEXT("{\"assets\":[\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"]}")));

	TestTrue(TEXT("IMC asset search should accumulate persistent input asset evidence"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bPersistentInputAssetObserved);
	TestFalse(TEXT("IMC asset search alone should not accumulate interaction action evidence"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bInteractionActionAssetObserved);
	TestFalse(TEXT("IMC asset search alone should not accumulate the enhanced_input query observation"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bReadOnlyEnhancedInputQueryObserved);
	TestFalse(TEXT("IMC asset search alone must not complete input_asset_authoring"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("asset_search"), TEXT("call_reuse_ia_search"), TEXT("{\"query\":\"IA_Interact\"}")),
		MakeToolEvent(
			TEXT("asset_search"),
			TEXT("call_reuse_ia_search"),
			TEXT("{\"query\":\"IA_Interact\"}"),
			TEXT("{\"assets\":[\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\"]}")));

	TestTrue(TEXT("IA asset search should accumulate interaction action evidence"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bInteractionActionAssetObserved);
	TestFalse(TEXT("asset searches alone must still wait for truthful enhanced_input observation"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));

	const FString QueryInput =
		TEXT("{\"operation\":\"query_context\",\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"}");
	const FString QueryResult =
		TEXT("{\"persistent_project_input\":true,\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\",\"mappings\":[{\"action_path\":\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\",\"key\":\"E\"}]}");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_reuse_query_context"), QueryInput),
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_reuse_query_context"), QueryInput, QueryResult));

	TestTrue(TEXT("query_context should accumulate the read-only enhanced_input observation"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bReadOnlyEnhancedInputQueryObserved);
	TestTrue(TEXT("accumulated multi-tool reuse evidence should complete input_asset_authoring"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("workflow should advance to runtime_actor_state after accumulated reuse proof"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_actor_state")));
	return true;
}

bool FFeatureWorkflowState_InputAssetAuthoringRejectsWeakReuseEvidence::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	const FString ToolInput =
		TEXT("{\"operation\":\"query_context\",\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"}");
	const FString ToolResult =
		TEXT("{\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\",\"summary\":\"existing mapping context found\"}");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_weak_input_reuse"), ToolInput),
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_weak_input_reuse"), ToolInput, ToolResult));

	TestFalse(TEXT("weak reuse evidence should not complete input_asset_authoring"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("workflow should remain in input_asset_authoring without the action-path proof"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));
	TestFalse(TEXT("weak evidence must not synthesize the explicit reuse state"),
		Plan.FeatureWorkflow.AuthoringLaneState.Equals(TEXT("persistent_input_reuse_verified"), ESearchCase::CaseSensitive));
	return true;
}

bool FFeatureWorkflowState_InputAssetAuthoringRejectsPartialAccumulatedReuseEvidence::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("asset_search"), TEXT("call_partial_imc_search"), TEXT("{\"query\":\"IMC_PrisonAccessProof\"}")),
		MakeToolEvent(
			TEXT("asset_search"),
			TEXT("call_partial_imc_search"),
			TEXT("{\"query\":\"IMC_PrisonAccessProof\"}"),
			TEXT("{\"assets\":[\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"]}")));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("asset_search"), TEXT("call_partial_ia_search"), TEXT("{\"query\":\"IA_Interact\"}")),
		MakeToolEvent(
			TEXT("asset_search"),
			TEXT("call_partial_ia_search"),
			TEXT("{\"query\":\"IA_Interact\"}"),
			TEXT("{\"assets\":[\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\"]}")));

	TestTrue(TEXT("partial accumulated evidence may observe the persistent IMC asset"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bPersistentInputAssetObserved);
	TestTrue(TEXT("partial accumulated evidence may observe the IA asset"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bInteractionActionAssetObserved);
	TestFalse(TEXT("without truthful enhanced_input query observation the reuse proof must stay incomplete"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.bReadOnlyEnhancedInputQueryObserved);
	TestFalse(TEXT("partial accumulated evidence must not complete input_asset_authoring"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("partial accumulated evidence should keep the workflow parked in input_asset_authoring"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));
	return true;
}

bool FFeatureWorkflowState_InputAssetAuthoringIgnoresEarlyCommandExecutionNoiseAndReachesAttemptResolver::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FString NoiseCommand = FString::Printf(
			TEXT("Select-String -Path Docs/OsvayderUE/*.md -Pattern \"IMC_PrisonAccessProof\" # early context probe %d"),
			Index);
		const FString NoiseResult =
			TEXT("Select-String : Cannot find path 'Docs/OsvayderUE/*.md' because it does not exist.");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(FString::Printf(TEXT("call_input_noise_%d"), Index), NoiseCommand),
			MakeCommandExecutionEvent(
				FString::Printf(TEXT("call_input_noise_%d"), Index),
				NoiseCommand,
				NoiseResult,
				true));
	}

	const FAgentFeatureWorkflowPhaseState* InputPhase =
		Plan.FeatureWorkflow.FindPhase(TEXT("input_asset_authoring"));
	TestTrue(TEXT("early command_execution noise must not fail input_asset_authoring twice"),
		InputPhase != nullptr && InputPhase->FailureCount == 0);
	TestFalse(TEXT("early command_execution noise must not trigger phase_failed_twice stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("noise should leave the workflow parked in input_asset_authoring until phase-local evidence arrives"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("asset_search"), TEXT("call_live_imc_search"), TEXT("{\"query\":\"IMC_PrisonAccessProof\"}")),
		MakeToolEvent(
			TEXT("asset_search"),
			TEXT("call_live_imc_search"),
			TEXT("{\"query\":\"IMC_PrisonAccessProof\"}"),
			TEXT("{\"assets\":[\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"]}")));
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("asset_search"), TEXT("call_live_ia_search"), TEXT("{\"query\":\"IA_Interact\"}")),
		MakeToolEvent(
			TEXT("asset_search"),
			TEXT("call_live_ia_search"),
			TEXT("{\"query\":\"IA_Interact\"}"),
			TEXT("{\"assets\":[\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\"]}")));

	const FString QueryInput =
		TEXT("{\"operation\":\"query_context\",\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"}");
	const FString QueryResult =
		TEXT("{\"persistent_project_input\":true,\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\",\"mappings\":[{\"action_path\":\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\",\"key\":\"E\"}]}");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_live_query_context"), QueryInput),
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_live_query_context"), QueryInput, QueryResult));

	TestTrue(TEXT("valid live reuse evidence should complete input_asset_authoring after early noise"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("valid input reuse evidence should advance to runtime_actor_state"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_actor_state")));
	TestTrue(TEXT("reuse observation should be serialized as a real signal"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.HasAnySignal());

	const FString OpenLevelInput =
		TEXT("{\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}");
	const FString OpenLevelResult =
		TEXT("Opened level /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("open_level"), TEXT("call_live_open_map"), OpenLevelInput),
		MakeToolEvent(TEXT("open_level"), TEXT("call_live_open_map"), OpenLevelInput, OpenLevelResult));

	const FString ActorQueryInput =
		TEXT("{\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\",\"include_tags\":true}");
	const FString ActorQueryResult =
		TEXT("Actors: Proof_PrisonAccess_Door, Proof_PrisonAccess_ControlBox, Proof_PrisonAccess_EscapeItem");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("get_level_actors"), TEXT("call_live_actor_observation"), ActorQueryInput),
		MakeToolEvent(TEXT("get_level_actors"), TEXT("call_live_actor_observation"), ActorQueryInput, ActorQueryResult));

	TestTrue(TEXT("placed proof actors should complete runtime_actor_state"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_actor_state")));
	TestEqual(TEXT("actor observation should advance to attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("attempt_resolver_and_logging")));

	const FString AutomationWrapperCommand =
		TEXT("\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -Command '$stdoutLog = Join-Path (Get-Location) \"Saved/Logs/packet654_rerun_stdout_20260424_011743.log\"; $automationLog = Join-Path (Get-Location) \"Saved/Logs/packet654_rerun_automation_20260424_011743.log\"; & \"D:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe\" Alternative.uproject -unattended -nop4 -nosplash -NullRHI -NoSound -DDC-ForceMemoryCache -stdout -FullStdOutLogOutput \"-ExecCmds=Automation RunTests Alternative.PrisonAccess;Quit\" \"-TestExit=Automation Test Queue Empty\" (\"-AbsLog={0}\" -f $automationLog) 2>&1 | Tee-Object -FilePath $stdoutLog'");
	const FString AutomationWrapperResult =
		TEXT("LogInit: Command Line: -unattended -nop4 -nosplash -NullRHI -NoSound -DDC-ForceMemoryCache -stdout -FullStdOutLogOutput -ExecCmds=\"Automation RunTests Alternative.PrisonAccess;Quit\" -TestExit=\"Automation Test Queue Empty\" -AbsLog=X:/PublicExample/Unreal/SampleProject/Saved/Logs/packet654_rerun_automation_20260424_011743.log\n")
		TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={TechnicalRoute} Path={Alternative.PrisonAccess.TechnicalRoute}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={UnskilledRoute} Path={Alternative.PrisonAccess.UnskilledRoute}\n")
		TEXT("LogAlternative: Display: PrisonAccessEvent order=1 actor=subject.player.technician target=AlternativeTechnicalBox_0 attempt=1 result=3 noisy=false tag=attempt.technical.override_success\n")
		TEXT("LogAlternative: Display: PrisonAccessEvent order=2 actor=subject.player.technician target=AlternativeStorageDoor_0 attempt=0 result=4 noisy=false tag=attempt.access.open_existing\n")
		TEXT("STDOUT_LOG=X:/PublicExample/Unreal/SampleProject/Saved/Logs/packet654_rerun_stdout_20260424_011743.log\n")
		TEXT("AUTOMATION_LOG=X:/PublicExample/Unreal/SampleProject/Saved/Logs/packet654_rerun_automation_20260424_011743.log");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_live_attempt_runtime_wrapper"), AutomationWrapperCommand),
		MakeCommandExecutionEvent(
			TEXT("call_live_attempt_runtime_wrapper"),
			AutomationWrapperCommand,
			AutomationWrapperResult));
	ApplyInteractionAccessAttemptResolverSourceInspection(
		Plan,
		Execution,
		TEXT("call_live_attempt_source_inspection"));

	TestTrue(TEXT("bounded automation should serialize runtime smoke attempt observation"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved);
	TestTrue(TEXT("bounded automation should serialize prison access event observation"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved);
	TestTrue(TEXT("attempt_resolver_and_logging should complete after live-style evidence"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestEqual(TEXT("workflow should advance to proof_context_setup after attempt resolver evidence"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("proof_context_setup")));
	TestFalse(TEXT("live-style evidence sequence should not stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	return true;
}

bool FFeatureWorkflowState_InteractionContractIgnoresOptionalContextFailuresAndReachesMemoryUpdate::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("interaction_contract"));

	const FString MissingProtocolCommand =
		TEXT("\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -Command \"Get-Content -LiteralPath 'AgentBridge\\PROTOCOL.md'\"");
	const FString MissingProtocolRawJson =
		TEXT("{\"type\":\"commandExecution\",\"id\":\"call_missing_protocol\",\"command\":\"Get-Content -LiteralPath 'AgentBridge\\\\PROTOCOL.md'\",\"cwd\":\"X:/PublicExample/Unreal/SampleProject\",\"status\":\"failed\",\"exitCode\":1}");
	const FString MissingProtocolResult =
		TEXT("Get-Content : Cannot find path 'AgentBridge\\PROTOCOL.md' because it does not exist.\n")
		TEXT("+ Get-Content -LiteralPath 'AgentBridge\\PROTOCOL.md'\n")
		TEXT("+ CategoryInfo : ObjectNotFound: (AgentBridge\\PROTOCOL.md:String) [Get-Content], ItemNotFoundException\n")
		TEXT("+ FullyQualifiedErrorId : PathNotFound,Microsoft.PowerShell.Commands.GetContentCommand");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_missing_protocol"), MissingProtocolCommand),
		MakeCommandExecutionResultWithEmptyInputEvent(
			TEXT("call_missing_protocol"),
			MissingProtocolRawJson,
			MissingProtocolResult,
			true));

	const FString RgContextCommand =
		TEXT("\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -Command \"rg -n \\\"packet654|Alternative.PrisonAccess|ProofFixtureSmoke|PrisonAccessEvent|Result=\\{Success\\}\\\" AgentBridge Docs Saved -g \\\"*.md\\\" -g \\\"*.json\\\" -g \\\"*.log\\\"\"");
	const FString RgContextRawJson =
		TEXT("{\"type\":\"commandExecution\",\"id\":\"call_rg_context\",\"command\":\"rg -n \\\"packet654|Alternative.PrisonAccess|ProofFixtureSmoke|PrisonAccessEvent|Result=\\\\{Success\\\\}\\\" AgentBridge Docs Saved -g \\\"*.md\\\" -g \\\"*.json\\\" -g \\\"*.log\\\"\",\"cwd\":\"X:/PublicExample/Unreal/SampleProject\",\"status\":\"failed\",\"exitCode\":1}");
	const FString RgContextResult =
		TEXT("Docs\\OsvayderUE\\prison_access_slice_v2.md:61:- discovery result: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
		TEXT("Docs\\OsvayderUE\\prison_access_slice_v2.md:63:- Alternative.PrisonAccess.ProofFixtureSmoke completed with Result={Success}\n")
		TEXT("Docs\\OsvayderUE\\prison_access_slice_v2.md:64:- PrisonAccessEvent order=1 tag=attempt.technical.override_success");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_rg_context"), RgContextCommand),
		MakeCommandExecutionResultWithEmptyInputEvent(
			TEXT("call_rg_context"),
			RgContextRawJson,
			RgContextResult,
			true));

	const FAgentFeatureWorkflowPhaseState* ContractPhase =
		Plan.FeatureWorkflow.FindPhase(TEXT("interaction_contract"));
	TestTrue(TEXT("optional read-only contract discovery failures must not fail interaction_contract"),
		ContractPhase != nullptr && ContractPhase->FailureCount == 0);
	TestFalse(TEXT("optional read-only contract discovery failures must not trigger stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("optional failures should leave command_execution drift budget untouched"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);
	TestEqual(TEXT("workflow should wait for phase-local contract evidence"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("interaction_contract")));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("asset_search"), TEXT("call_contract_proof_map"), TEXT("{\"query\":\"Lvl_PrisonAccessProof\"}")),
		MakeToolEvent(
			TEXT("asset_search"),
			TEXT("call_contract_proof_map"),
			TEXT("{\"query\":\"Lvl_PrisonAccessProof\"}"),
			TEXT("{\"assets\":[\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof.Lvl_PrisonAccessProof\"]}")));

	TestTrue(TEXT("proof-map evidence should complete interaction_contract after optional context noise"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("interaction_contract")));
	TestEqual(TEXT("workflow should advance from interaction_contract to input_asset_authoring"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("asset_search"), TEXT("call_memory_imc_search"), TEXT("{\"query\":\"IMC_PrisonAccessProof\"}")),
		MakeToolEvent(
			TEXT("asset_search"),
			TEXT("call_memory_imc_search"),
			TEXT("{\"query\":\"IMC_PrisonAccessProof\"}"),
			TEXT("{\"assets\":[\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"]}")));
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("asset_search"), TEXT("call_memory_ia_search"), TEXT("{\"query\":\"IA_Interact\"}")),
		MakeToolEvent(
			TEXT("asset_search"),
			TEXT("call_memory_ia_search"),
			TEXT("{\"query\":\"IA_Interact\"}"),
			TEXT("{\"assets\":[\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\"]}")));

	const FString QueryInput =
		TEXT("{\"operation\":\"query_context\",\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"}");
	const FString QueryResult =
		TEXT("{\"persistent_project_input\":true,\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\",\"mappings\":[{\"action_path\":\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\",\"key\":\"E\"}]}");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_memory_query_context"), QueryInput),
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_memory_query_context"), QueryInput, QueryResult));

	const FString OpenLevelInput =
		TEXT("{\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}");
	const FString OpenLevelResult =
		TEXT("Opened level /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("open_level"), TEXT("call_memory_open_map"), OpenLevelInput),
		MakeToolEvent(TEXT("open_level"), TEXT("call_memory_open_map"), OpenLevelInput, OpenLevelResult));

	const FString ActorQueryInput =
		TEXT("{\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\",\"include_tags\":true}");
	const FString ActorQueryResult =
		TEXT("Actors: Proof_PrisonAccess_Door, Proof_PrisonAccess_ControlBox, Proof_PrisonAccess_EscapeItem");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("get_level_actors"), TEXT("call_memory_runtime_actors"), ActorQueryInput),
		MakeToolEvent(TEXT("get_level_actors"), TEXT("call_memory_runtime_actors"), ActorQueryInput, ActorQueryResult));

	const FString SourceRawJson =
		TEXT("{\"type\":\"commandExecution\",\"id\":\"call_memory_source_shape\",\"command\":\"Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.h; Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp\",\"status\":\"completed\"}");
	const FString SourceResult =
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.h:9: struct ALTERNATIVE_API FAlternativePrisonAccessAttemptResolver\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.h:16: static FAlternativePrisonAccessAttemptOutcome ResolveDoorAttempt(\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.cpp:18: void UAlternativePrisonAccessEventSubsystem::RecordEvent(const FAlternativePrisonAccessEventRecord& Record)\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.cpp:29: TEXT(\"PrisonAccessEvent order=%d actor=%s target=%s attempt=%d result=%d noisy=%s tag=%s\")");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(
			TEXT("call_memory_source_shape"),
			TEXT("Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.h; Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp")),
		MakeCommandExecutionResultWithEmptyInputEvent(
			TEXT("call_memory_source_shape"),
			SourceRawJson,
			SourceResult));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("get_level_actors"), TEXT("call_memory_proof_context"), ActorQueryInput),
		MakeToolEvent(TEXT("get_level_actors"), TEXT("call_memory_proof_context"), ActorQueryInput, ActorQueryResult));

	const FString AutomationResult =
		TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={InputPathTruth} Path={Alternative.PrisonAccess.InputPathTruth}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
		TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeAutomationRunTestsEvent(TEXT("call_memory_automation")),
		MakeAutomationRunTestsEvent(TEXT("call_memory_automation"), AutomationResult));

	const FString RuntimeProofInput =
		TEXT("{\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\",\"proof\":\"Alternative.PrisonAccess\"}");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("map_runtime_proof"), TEXT("call_memory_runtime_proof"), RuntimeProofInput),
		MakeToolEvent(
			TEXT("map_runtime_proof"),
			TEXT("call_memory_runtime_proof"),
			RuntimeProofInput,
			TEXT("Runtime proof passed for /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof")));

	TestTrue(TEXT("workflow should complete runtime_proof"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_proof")));
	TestEqual(TEXT("actual feature workflow state should reach memory_update"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("memory_update")));
	TestFalse(TEXT("interaction contract recovery path should not stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);

	const TSharedPtr<FJsonObject> WorkflowJson = Plan.FeatureWorkflow.ToJsonObject();
	const TSharedPtr<FJsonObject>* ObservationObject = nullptr;
	TestTrue(TEXT("memory_update archive JSON should serialize non-empty attempt resolver observation"),
		WorkflowJson->TryGetObjectField(TEXT("interaction_access_attempt_resolver_observation"), ObservationObject)
		&& ObservationObject != nullptr
		&& ObservationObject->IsValid());
	return true;
}

bool FFeatureWorkflowState_InteractionContractPreflightProofContextDoesNotSelfStop::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("interaction_contract"));

	TArray<FAgentRunEvent> Starts;
	TArray<FAgentRunEvent> Results;
	auto AddPreflight = [&Starts, &Results](
		const FString& ToolCallId,
		const FString& Command,
		const FString& Result)
	{
		Starts.Add(MakeCommandExecutionEvent(ToolCallId, Command));
		Results.Add(MakeCommandExecutionEvent(ToolCallId, Command, Result));
	};

	AddPreflight(
		TEXT("call_v2_preflight_docs"),
		TEXT("Get-ChildItem -Path Docs/OsvayderUE -Force"),
		TEXT("prison_access_slice_v2.md memory.md"));
	AddPreflight(
		TEXT("call_v2_preflight_saved"),
		TEXT("Get-ChildItem -Path Saved/OsvayderUE -Force"),
		TEXT("active_plan.json closeout_decision.json PlanArchives"));
	AddPreflight(
		TEXT("call_v2_preflight_active_plan"),
		TEXT("Get-Content -Path Saved/OsvayderUE/active_plan.json -Raw"),
		TEXT("{\"recipe_id\":\"feature.interaction_access_slice_v1\",\"original_user_task\":\"Verify /Game/PrisonAccess/Maps/LvL_PrisonAccessProof, /Game/PrisonAccess/Input/IMC_PrisonAccessProof, /Game/Variant_SideScrolling/Input/Actions/IA_Interact, Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp, Alternative.PrisonAccess ProofFixtureSmoke PrisonAccessEvent\"}"));
	AddPreflight(
		TEXT("call_v2_preflight_closeout"),
		TEXT("Get-Content -Path Saved/OsvayderUE/closeout_decision.json -Raw"),
		TEXT("{\"source_project_root\":\"X:/PublicExample/Unreal/SampleProject\",\"facts_consumed\":{\"bounded_interaction_access_runtime_proof\":true},\"recipe_id\":\"feature.interaction_access_slice_v1\"}"));
	AddPreflight(
		TEXT("call_v2_preflight_archives"),
		TEXT("Get-ChildItem -Path Saved/OsvayderUE/PlanArchives -Force"),
		TEXT("20260427-093943-plan_20260427_093711_9F9EC602499BDD62F4984BA08967D3B2.active_plan.json"));
	AddPreflight(
		TEXT("call_v2_preflight_docs_search"),
		TEXT("Select-String -Path Docs/OsvayderUE/prison_access_slice_v2.md -Pattern 'Alternative.PrisonAccess|ProofFixtureSmoke|PrisonAccessEvent'"),
		TEXT("Alternative.PrisonAccess ProofFixtureSmoke Result={Success} PrisonAccessEvent order=1"));

	for (const FAgentRunEvent& Start : Starts)
	{
		SOsvayderEditorWidget::ApplyFeatureWorkflowToolBoundaryForTest(Plan, Execution, Start, true);
	}

	TestFalse(TEXT("bounded proof-context preflight starts must not self-stop interaction_contract"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("bounded proof-context preflight starts must not consume command no-advance budget"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);
	TestEqual(TEXT("preflight starts should leave workflow waiting for result evidence"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("interaction_contract")));

	for (const FAgentRunEvent& Result : Results)
	{
		SOsvayderEditorWidget::ApplyFeatureWorkflowToolBoundaryForTest(Plan, Execution, Result, false);
	}

	TestFalse(TEXT("bounded proof-context preflight results must not stop-loss before UE proof"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("active-plan proof context should complete interaction_contract"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("interaction_contract")));
	TestEqual(TEXT("workflow should advance to input asset authoring after proof-context contract evidence"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));

	ApplyInputReuseEvidence(Plan, Execution);
	ApplyRuntimeActorEvidence(Plan, Execution);

	TestFalse(TEXT("UE proof context after preflight should not inherit a stale stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("runtime actor proof should mark known proof map available"),
		Plan.FeatureWorkflow.bKnownProofMapAvailable);
	TestTrue(TEXT("runtime actor proof should mark placed runtime actors available"),
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable);
	TestEqual(TEXT("runtime actor proof should satisfy proof prerequisites"),
		Plan.FeatureWorkflow.ProofPrerequisiteState,
		FString(TEXT("satisfied")));
	return true;
}

bool FFeatureWorkflowState_InteractionContractUnrelatedReadOnlyDriftStillStopLosses::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("interaction_contract"));

	for (int32 Index = 0; Index < 6; ++Index)
	{
		const FString Command = FString::Printf(TEXT("Write-Output unrelated_interaction_contract_probe_%d"), Index);
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(FString::Printf(TEXT("call_interaction_contract_unrelated_%d"), Index), Command),
			MakeCommandExecutionEvent(
				FString::Printf(TEXT("call_interaction_contract_unrelated_%d"), Index),
				Command,
				TEXT("unrelated output")));
	}

	TestTrue(TEXT("unrelated read-only command drift should still stop-loss interaction_contract"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("unrelated read-only drift should use command no-advance stop-loss"),
		Plan.FeatureWorkflow.StopLossReason.Contains(TEXT("command_execution_without_phase_advance_gt_5")));
	TestEqual(TEXT("unrelated read-only drift with no proof facts should report missing prerequisites"),
		Plan.FeatureWorkflow.BlockerFamily,
		FString(TEXT("proof_prerequisites_missing")));
	return true;
}

bool FFeatureWorkflowState_InteractionContractMissingProofContextStillReportsPrerequisites::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("interaction_contract"));

	TArray<FAgentRunEvent> Starts;
	TArray<FAgentRunEvent> Results;
	for (int32 Index = 0; Index < 6; ++Index)
	{
		const FString ToolCallId = FString::Printf(TEXT("call_missing_proof_preflight_%d"), Index);
		const FString Command = FString::Printf(
			TEXT("Get-Content -Path Docs/OsvayderUE/missing_preflight_%d.md"),
			Index);
		Starts.Add(MakeCommandExecutionEvent(ToolCallId, Command));
		Results.Add(MakeCommandExecutionEvent(ToolCallId, Command, TEXT("No relevant proof context in this file.")));
	}

	for (const FAgentRunEvent& Start : Starts)
	{
		SOsvayderEditorWidget::ApplyFeatureWorkflowToolBoundaryForTest(Plan, Execution, Start, true);
	}

	TestFalse(TEXT("resultless docs preflight starts should not stop-loss before contents are known"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("resultless docs preflight starts should not spend the no-advance budget"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);

	for (const FAgentRunEvent& Result : Results)
	{
		SOsvayderEditorWidget::ApplyFeatureWorkflowToolBoundaryForTest(Plan, Execution, Result, false);
	}

	TestTrue(TEXT("missing proof-context results should still stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("missing proof context should report missing proof prerequisites"),
		Plan.FeatureWorkflow.BlockerFamily,
		FString(TEXT("proof_prerequisites_missing")));
	TestEqual(TEXT("missing proof context should mark prerequisite state missing"),
		Plan.FeatureWorkflow.ProofPrerequisiteState,
		FString(TEXT("missing")));
	TestFalse(TEXT("missing proof context must not fabricate proof map availability"),
		Plan.FeatureWorkflow.bKnownProofMapAvailable);
	TestFalse(TEXT("missing proof context must not fabricate persistent input reuse"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.HasSufficientReuseEvidence());
	return true;
}

bool FFeatureWorkflowState_InteractionAccessReview5LiveTraceReplayReachesMemoryUpdate::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("project_context_preflight"));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_tZxzdSbs8w50OjBuIRIUDJl6"), TEXT("Write-Output project_context_preflight_ready")),
		MakeCommandExecutionEvent(
			TEXT("call_tZxzdSbs8w50OjBuIRIUDJl6"),
			TEXT("Write-Output project_context_preflight_ready"),
			TEXT("Project context preflight completed for feature.interaction_access_slice_v1")));
	TestTrue(TEXT("project preflight should complete before interaction contract"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("project_context_preflight")));
	TestEqual(TEXT("workflow should advance to interaction_contract"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("interaction_contract")));

	ApplyInteractionAccessContractCompletion(Plan, Execution);
	TestTrue(TEXT("interaction contract should complete before the live input burst"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("interaction_contract")));
	TestEqual(TEXT("workflow should advance to input_asset_authoring"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));

	ApplyLiveReadOnlyInputAuthoringContextBurst(Plan, Execution);
	TestFalse(TEXT("review5 read-only input context burst must not trigger stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("read-only input context burst should leave the phase parked for structured proof tools"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));

	ApplyInputReuseEvidence(Plan, Execution, TEXT("osvayderue/"));
	ApplyRuntimeActorEvidence(Plan, Execution, TEXT("osvayderue/"));
	ApplyAttemptResolverSourceEvidence(Plan, Execution);
	ApplyRuntimeActorEvidence(
		Plan,
		Execution,
		TEXT("osvayderue/"),
		TEXT("call_live_proof_context_open"),
		TEXT("call_live_proof_context_actors"));
	ApplyAutomationDiscoveryEvidence(Plan, Execution);
	ApplyRuntimeProofEvidence(Plan, Execution);

	TestFalse(TEXT("full review5 replay should not stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("input_asset_authoring should complete"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestTrue(TEXT("runtime_actor_state should complete"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_actor_state")));
	TestTrue(TEXT("attempt_resolver_and_logging should complete"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestTrue(TEXT("proof_context_setup should complete"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("proof_context_setup")));
	TestTrue(TEXT("compile_gate should complete as not_required before automation discovery"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("compile_gate")));
	TestTrue(TEXT("automation_discovery_gate should complete"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestTrue(TEXT("runtime_proof should complete"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_proof")));
	TestEqual(TEXT("review5 live replay should reach memory_update"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("memory_update")));
	TestTrue(TEXT("attempt resolver observation should be sufficient"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.HasSufficientEvidence());

	const TSharedPtr<FJsonObject> WorkflowJson = Plan.FeatureWorkflow.ToJsonObject();
	const TSharedPtr<FJsonObject>* ObservationObject = nullptr;
	TestTrue(TEXT("memory_update JSON should include non-empty interaction_access_attempt_resolver_observation"),
		WorkflowJson->TryGetObjectField(TEXT("interaction_access_attempt_resolver_observation"), ObservationObject)
		&& ObservationObject != nullptr
		&& ObservationObject->IsValid());
	return true;
}

bool FFeatureWorkflowState_InputAssetAuthoringDoesNotStopLossOnLiveReadOnlyContextBurst::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	ApplyLiveReadOnlyInputAuthoringContextBurst(Plan, Execution);

	const FAgentFeatureWorkflowPhaseState* InputPhase =
		Plan.FeatureWorkflow.FindPhase(TEXT("input_asset_authoring"));
	TestTrue(TEXT("live read-only context burst must keep input_asset_authoring phase available"),
		InputPhase != nullptr && InputPhase->FailureCount == 0);
	TestFalse(TEXT("live read-only context burst must not stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestFalse(TEXT("live read-only context burst must not mark proof prerequisites missing early"),
		Plan.FeatureWorkflow.BlockerFamily.Equals(TEXT("proof_prerequisites_missing"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("input_asset_authoring must not be failed by read-only context probes"),
		Plan.FeatureWorkflow.FailedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("workflow should remain in input_asset_authoring until enhanced_input proof arrives"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));

	ApplyInputReuseEvidence(Plan, Execution);

	TestTrue(TEXT("IMC/IA plus enhanced_input query should complete after the read-only burst"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("input proof should advance to runtime_actor_state"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_actor_state")));
	TestFalse(TEXT("input proof after context burst must still avoid stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	return true;
}

bool FFeatureWorkflowState_InputAssetAuthoringStillStopsOnUnboundedIrrelevantCommandDrift::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	for (int32 Index = 0; Index < 6; ++Index)
	{
		const FString Command = FString::Printf(TEXT("Get-ChildItem Saved/Temp/Unrelated_%d"), Index);
		const FString Result = FString::Printf(TEXT("Directory: Saved/Temp/Unrelated_%d\nnotes.txt"), Index);
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(FString::Printf(TEXT("call_irrelevant_drift_%d"), Index), Command),
			MakeCommandExecutionEvent(FString::Printf(TEXT("call_irrelevant_drift_%d"), Index), Command, Result));
	}

	TestTrue(TEXT("irrelevant command drift should still trip stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestFalse(TEXT("irrelevant drift must not complete input_asset_authoring"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("irrelevant drift should remain not past input_asset_authoring"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));
	return true;
}

bool FFeatureWorkflowState_LiveProofEvidenceAfterContextBurstLatchesPrerequisites::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	ApplyLiveReadOnlyInputAuthoringContextBurst(Plan, Execution);
	ApplyInputReuseEvidence(Plan, Execution);
	ApplyRuntimeActorEvidence(Plan, Execution);

	TestTrue(TEXT("open_level should latch the known proof map"),
		Plan.FeatureWorkflow.bKnownProofMapAvailable);
	TestTrue(TEXT("get_level_actors should latch placed runtime actors"),
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable);
	TestEqual(TEXT("proof prerequisites should be satisfied after map and actors are observed"),
		Plan.FeatureWorkflow.ProofPrerequisiteState,
		FString(TEXT("satisfied")));
	TestTrue(TEXT("runtime_actor_state should complete after actor evidence"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_actor_state")));
	TestEqual(TEXT("workflow should advance out of runtime_actor_state"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("attempt_resolver_and_logging")));
	TestFalse(TEXT("proof evidence after context burst should not stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	return true;
}

bool FFeatureWorkflowState_AttemptResolverEvidenceAfterContextBurstSerializesObservation::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	ApplyLiveReadOnlyInputAuthoringContextBurst(Plan, Execution);
	ApplyInputReuseEvidence(Plan, Execution);
	ApplyRuntimeActorEvidence(Plan, Execution);
	ApplyAttemptResolverSourceEvidence(Plan, Execution);

	TestTrue(TEXT("source evidence should observe attempt resolver"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved);
	TestTrue(TEXT("source evidence should observe event subsystem"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved);
	TestTrue(TEXT("attempt resolver source pair should be sufficient"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.HasSufficientEvidence());
	TestTrue(TEXT("attempt_resolver_and_logging should complete after context burst"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));

	const TSharedPtr<FJsonObject> WorkflowJson = Plan.FeatureWorkflow.ToJsonObject();
	const TSharedPtr<FJsonObject>* ObservationObject = nullptr;
	TestTrue(TEXT("observation JSON should serialize after context burst"),
		WorkflowJson->TryGetObjectField(TEXT("interaction_access_attempt_resolver_observation"), ObservationObject)
		&& ObservationObject != nullptr
		&& ObservationObject->IsValid());
	TestFalse(TEXT("attempt resolver evidence after context burst should not stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	return true;
}

bool FFeatureWorkflowState_LiveMcpToolNamesCompleteInputAssetAuthoringBeforeDriftStopLoss::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	ApplyLiveReadOnlyInputAuthoringContextBurst(Plan, Execution);
	ApplyInputReuseEvidence(Plan, Execution, TEXT("osvayderue/"));

	TestFalse(TEXT("prefixed live MCP tool names must not arrive after stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("prefixed asset_search and enhanced_input should complete input_asset_authoring"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("prefixed input evidence should advance to runtime_actor_state"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_actor_state")));

	ApplyRuntimeActorEvidence(Plan, Execution, TEXT("osvayderue/"));
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("osvayderue/capture_viewport"), TEXT("call_live_capture_viewport"), TEXT("{\"reason\":\"proof actor visibility\"}")),
		MakeToolEvent(
			TEXT("osvayderue/capture_viewport"),
			TEXT("call_live_capture_viewport"),
			TEXT("{\"reason\":\"proof actor visibility\"}"),
			TEXT("Viewport shows Proof_PrisonAccess_Door, Proof_PrisonAccess_ControlBox, and Proof_PrisonAccess_EscapeItem in Lvl_PrisonAccessProof")));

	TestTrue(TEXT("prefixed open_level/get_level_actors should latch known map"),
		Plan.FeatureWorkflow.bKnownProofMapAvailable);
	TestTrue(TEXT("prefixed actor/capture evidence should latch placed actors"),
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable);
	TestEqual(TEXT("live prefixed actor proof should advance to attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("attempt_resolver_and_logging")));
	return true;
}

bool FFeatureWorkflowState_RuntimeProofToolDoesNotBypassAttemptResolverObservation::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("attempt_resolver_and_logging"));
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");

	ApplyRuntimeProofEvidence(Plan, Execution);

	TestFalse(TEXT("runtime proof must not complete while attempt resolver observation is empty"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_proof")));
	TestFalse(TEXT("runtime proof must not be marked passed without attempt resolver observation"),
		Plan.FeatureWorkflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("attempt_resolver_and_logging should remain required"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("attempt_resolver_and_logging")));
	TestFalse(TEXT("empty attempt resolver observation should stay empty"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.HasAnySignal());
	return true;
}

bool FFeatureWorkflowState_OpenLevelAloneDoesNotMarkPlacedRuntimeActorsAvailable::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("proof_context_setup"));

	const FString ToolInput =
		TEXT("{\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}");
	const FString ToolResult =
		TEXT("Opened level /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("open_level"), TEXT("call_open_proof_map"), ToolInput),
		MakeToolEvent(TEXT("open_level"), TEXT("call_open_proof_map"), ToolInput, ToolResult));

	TestTrue(TEXT("opening the proof map should mark the known map as available"),
		Plan.FeatureWorkflow.bKnownProofMapAvailable);
	TestFalse(TEXT("opening the map alone must not claim placed actors are available"),
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable);
	TestEqual(TEXT("proof_context_setup should remain active until actor observation arrives"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("proof_context_setup")));
	return true;
}

bool FFeatureWorkflowState_Review13RuntimeActorStateHydratesAfterSourceInspectionErrors::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	ApplyInputReuseEvidence(Plan, Execution, TEXT("osvayderue/"));
	TestEqual(TEXT("input reuse evidence should place review13 replay at runtime_actor_state"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_actor_state")));

	ApplyReview13RuntimeActorSourceInspectionError(
		Plan,
		Execution,
		TEXT("call_review13_runtime_actor_bad_glob_1"));
	ApplyReview13RuntimeActorSourceInspectionError(
		Plan,
		Execution,
		TEXT("call_review13_runtime_actor_bad_glob_2"));

	ApplyRuntimeActorEvidence(
		Plan,
		Execution,
		TEXT("osvayderue/"),
		TEXT("call_review13_runtime_open_level"),
		TEXT("call_review13_runtime_get_level_actors"));

	TestFalse(TEXT("source inspection errors must not stop-loss before live Unreal actor evidence"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestFalse(TEXT("source inspection errors must not fail runtime_actor_state"),
		Plan.FeatureWorkflow.FailedPhaseIds.Contains(TEXT("runtime_actor_state")));
	TestTrue(TEXT("open_level should hydrate known proof map after inspection noise"),
		Plan.FeatureWorkflow.bKnownProofMapAvailable);
	TestTrue(TEXT("get_level_actors should hydrate placed runtime actors after inspection noise"),
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable);
	TestTrue(TEXT("runtime_actor_state should complete from live Unreal actor evidence"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_actor_state")));
	TestEqual(TEXT("runtime actor evidence should advance to attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("attempt_resolver_and_logging")));
	return true;
}

bool FFeatureWorkflowState_Review13RuntimeActorStateRejectsNarrativeAndShellDrift::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("runtime_actor_state"));

	const FString DocsCommand = TEXT("Get-Content Docs/OsvayderUE/prison_access_slice_v2.md");
	const FString DocsResult =
		TEXT("Docs/OsvayderUE/prison_access_slice_v2.md says Lvl_PrisonAccessProof contains ")
		TEXT("Proof_PrisonAccess_Door, Proof_PrisonAccess_ControlBox, Proof_PrisonAccess_EscapeItem, ")
		TEXT("AlternativeStorageDoor, AlternativeTechnicalBox, and AlternativeEscapeItem.");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_review13_docs_narrative"), DocsCommand),
		MakeCommandExecutionEvent(TEXT("call_review13_docs_narrative"), DocsCommand, DocsResult));

	TestFalse(TEXT("docs narrative must not mark known proof map as observed"),
		Plan.FeatureWorkflow.bKnownProofMapAvailable);
	TestFalse(TEXT("docs narrative must not mark placed runtime actors as observed"),
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable);
	TestFalse(TEXT("docs narrative must not complete runtime_actor_state"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_actor_state")));
	TestEqual(TEXT("runtime_actor_state should remain active without Unreal actor receipts"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_actor_state")));

	for (int32 Index = 0; Index < 6; ++Index)
	{
		const FString CallId = FString::Printf(TEXT("call_review13_arbitrary_shell_drift_%d"), Index);
		const FString Command = FString::Printf(TEXT("Write-Output unrelated-shell-drift-%d"), Index);
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(CallId, Command),
			MakeCommandExecutionEvent(CallId, Command, TEXT("unrelated shell output")));
	}

	TestFalse(TEXT("arbitrary shell drift must not mark known proof map as observed"),
		Plan.FeatureWorkflow.bKnownProofMapAvailable);
	TestFalse(TEXT("arbitrary shell drift must not mark placed runtime actors as observed"),
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable);
	TestFalse(TEXT("arbitrary shell drift must not complete runtime_actor_state"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_actor_state")));
	TestTrue(TEXT("arbitrary shell drift should retain stop-loss protection"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	return true;
}

bool FFeatureWorkflowState_Review14RuntimeProofCompletesFromBoundedAutomationProof::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("runtime_proof"));
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);

	const FString AbsLogPath = WriteReview11AutomationLogFixture(
		TEXT("review14_runtime_proof_bounded_automation"),
		MakeReview11NoisyAbsLogResult(true));
	ApplyReview11AbsLogAutomationEvidence(
		Plan,
		Execution,
		TEXT("call_review14_runtime_proof_bounded_automation"),
		AbsLogPath);

	TestTrue(TEXT("review14 replay starts after automation_discovery_gate is already complete"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestTrue(TEXT("review14 bounded automation should preserve all source and runtime observation booleans"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.HasCompleteEvidence());
	TestEqual(TEXT("review14 bounded automation should preserve discovery count"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		7);
	TestEqual(TEXT("review14 bounded automation should preserve pass count"),
		Plan.FeatureWorkflow.AutomationPassedCount,
		7);
	TestEqual(TEXT("review14 bounded automation should preserve zero failures"),
		Plan.FeatureWorkflow.AutomationFailedCount,
		0);
	TestTrue(TEXT("review14 bounded automation should pass runtime_proof"),
		Plan.FeatureWorkflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("review14 bounded automation should complete runtime_proof"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_proof")));
	TestEqual(TEXT("review14 bounded automation should advance to memory_update"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("memory_update")));
	TestFalse(TEXT("review14 bounded automation must not trip ad-hoc runtime proof stop-loss"),
		Plan.FeatureWorkflow.StopLossReason.Contains(TEXT("ad_hoc_runtime_proof_attempts_gt_3")));
	TestFalse(TEXT("review14 bounded automation must not stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	return true;
}

bool FFeatureWorkflowState_Review14AdHocRuntimeProofAttemptsStillStopLossWithoutAutomationProof::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("runtime_proof"));
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);

	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FString CallId = FString::Printf(TEXT("call_review14_ad_hoc_runtime_probe_%d"), Index);
		const FString Command = FString::Printf(TEXT("Write-Output unrelated_runtime_proof_probe_%d"), Index);
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(CallId, Command),
			MakeCommandExecutionEvent(CallId, Command, TEXT("unrelated runtime proof output")));
	}

	TestTrue(TEXT("review14 arbitrary ad-hoc runtime proof attempts must still stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("review14 ad-hoc stop-loss should keep its specific reason"),
		Plan.FeatureWorkflow.StopLossReason.Contains(TEXT("ad_hoc_runtime_proof_attempts_gt_3")));
	TestFalse(TEXT("review14 ad-hoc commands must not pass runtime_proof without automation truth"),
		Plan.FeatureWorkflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("review14 ad-hoc commands must not complete runtime_proof without automation truth"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_proof")));
	return true;
}

bool FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromAccumulatedSourceInspection::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("attempt_resolver_and_logging"));

	const FString AttemptResolverPath =
		TEXT("X:/PublicExample/Unreal/SampleProject/Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp");
	const FString AttemptResolverSource =
		TEXT("FAlternativePrisonAccessAttemptOutcome FAlternativePrisonAccessAttemptResolver::ResolveDoorAttempt(const AActor* Interactor, const FAlternativeDoorState& DoorState, const FName FacilityId, const FName AccessPointTag)");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("read"), TEXT("call_attempt_resolver_source"), AttemptResolverPath),
		MakeToolEvent(TEXT("read"), TEXT("call_attempt_resolver_source"), AttemptResolverPath, AttemptResolverSource));

	TestTrue(TEXT("direct attempt resolver source inspection should be accumulated"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved);
	TestFalse(TEXT("event subsystem source still needs its own direct observation"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved);
	TestFalse(TEXT("attempt resolver source alone must not complete the phase"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));

	const FString EventSubsystemPath =
		TEXT("X:/PublicExample/Unreal/SampleProject/Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp");
	const FString EventSubsystemSource =
		TEXT("void UAlternativePrisonAccessEventSubsystem::RecordEvent(const FAlternativePrisonAccessEventRecord& Record)\nTEXT(\"PrisonAccessEvent order=%d actor=%s target=%s attempt=%d result=%d noisy=%s tag=%s\")");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("read"), TEXT("call_event_subsystem_source"), EventSubsystemPath),
		MakeToolEvent(TEXT("read"), TEXT("call_event_subsystem_source"), EventSubsystemPath, EventSubsystemSource));

	TestTrue(TEXT("event subsystem source inspection should also be accumulated"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved);
	TestTrue(TEXT("accumulated direct source inspection should complete attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestEqual(TEXT("workflow should advance to proof_context_setup after direct source inspection"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("proof_context_setup")));
	return true;
}

bool FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromRelativeCommandExecutionSourceInspection::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("attempt_resolver_and_logging"));

	const FString AttemptResolverCommand =
		TEXT("Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp");
	const FString AttemptResolverSource =
		TEXT("FAlternativePrisonAccessAttemptOutcome FAlternativePrisonAccessAttemptResolver::ResolveDoorAttempt(const AActor* Interactor, const FAlternativeDoorState& DoorState, const FName FacilityId, const FName AccessPointTag)");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_relative_attempt_resolver_source"), AttemptResolverCommand),
		MakeCommandExecutionEvent(
			TEXT("call_relative_attempt_resolver_source"),
			AttemptResolverCommand,
			AttemptResolverSource));

	TestTrue(TEXT("relative command_execution source inspection should mark attempt resolver evidence"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved);
	TestEqual(TEXT("progressing relative source inspection should not consume stop-loss budget"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);
	TestFalse(TEXT("relative attempt resolver source alone must not complete the phase"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));

	const FString EventSubsystemCommand =
		TEXT("Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp");
	const FString EventSubsystemSource =
		TEXT("void UAlternativePrisonAccessEventSubsystem::RecordEvent(const FAlternativePrisonAccessEventRecord& Record)\nTEXT(\"PrisonAccessEvent order=%d actor=%s target=%s attempt=%d result=%d noisy=%s tag=%s\")");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_relative_event_subsystem_source"), EventSubsystemCommand),
		MakeCommandExecutionEvent(
			TEXT("call_relative_event_subsystem_source"),
			EventSubsystemCommand,
			EventSubsystemSource));

	TestTrue(TEXT("relative command_execution source inspection should also mark event subsystem evidence"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved);
	TestTrue(TEXT("relative project-local source inspection should complete attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestEqual(TEXT("workflow should advance to proof_context_setup after relative source inspection"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("proof_context_setup")));
	return true;
}

bool FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromLiveCommandExecutionResultShape::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("attempt_resolver_and_logging"));

	const FString SourceInspectionCommand =
		TEXT("\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -Command '$sourceFiles = @(\"Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.h\", \"Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp\", \"Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.h\", \"Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp\"); foreach ($file in $sourceFiles) { Select-String -Path $file -Pattern \"AlternativePrisonAccess|ResolveDoorAttempt|RecordEvent|PrisonAccessEvent\" }'");
	const FString LiveRawJson =
		TEXT("{\"type\":\"commandExecution\",\"id\":\"call_live_source_shape\",\"command\":\"Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.h; Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp\",\"status\":\"completed\"}");
	const FString LiveResult =
		TEXT("=== Source evidence ===\n")
		TEXT("-- Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.h\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.h:9: struct ALTERNATIVE_API FAlternativePrisonAccessAttemptResolver\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.h:16: static FAlternativePrisonAccessAttemptOutcome ResolveDoorAttempt(\n")
		TEXT("-- Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessAttemptResolver.cpp:1: #include \"PrisonAccess/AlternativePrisonAccessAttemptResolver.h\"\n")
		TEXT("-- Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.h\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.h:12: class UAlternativePrisonAccessEventSubsystem\n")
		TEXT("-- Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.cpp:18: void UAlternativePrisonAccessEventSubsystem::RecordEvent(const FAlternativePrisonAccessEventRecord& Record)\n")
		TEXT("Source\\Alternative\\PrisonAccess\\AlternativePrisonAccessEventSubsystem.cpp:29: TEXT(\"PrisonAccessEvent order=%d actor=%s target=%s attempt=%d result=%d noisy=%s tag=%s\")");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_live_source_shape"), SourceInspectionCommand),
		MakeCommandExecutionResultWithEmptyInputEvent(
			TEXT("call_live_source_shape"),
			LiveRawJson,
			LiveResult));

	TestTrue(TEXT("live command_execution result shape should mark attempt resolver source evidence"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved);
	TestTrue(TEXT("live command_execution result shape should mark event subsystem source evidence"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved);
	TestTrue(TEXT("live command_execution result shape should complete attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestFalse(TEXT("live command_execution result shape should not trigger stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("source evidence progress should reset command_execution drift budget"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);

	const TSharedPtr<FJsonObject> WorkflowJson = Plan.FeatureWorkflow.ToJsonObject();
	const TSharedPtr<FJsonObject>* ObservationObject = nullptr;
	TestTrue(TEXT("archive JSON should serialize non-empty attempt resolver observation"),
		WorkflowJson->TryGetObjectField(TEXT("interaction_access_attempt_resolver_observation"), ObservationObject)
		&& ObservationObject != nullptr
		&& ObservationObject->IsValid());
	TestEqual(TEXT("workflow should advance after live command_execution source evidence"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("proof_context_setup")));
	return true;
}

bool FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromRuntimeSmokeLogReview::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("attempt_resolver_and_logging"));

	const FString LogReviewCommand =
		TEXT("Get-Content X:\\PublicExample\\Unreal\\SampleProject\\Saved\\Logs\\packet653_rerun_stdout_20260424_003729.log");
	const FString LogReviewResult =
		TEXT("X:/PublicExample/Unreal/SampleProject/Saved/Logs/packet653_rerun_stdout_20260424_003729.log\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={TechnicalRoute} Path={Alternative.PrisonAccess.TechnicalRoute}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={UnskilledRoute} Path={Alternative.PrisonAccess.UnskilledRoute}\n")
		TEXT("LogAlternative: Display: PrisonAccessEvent order=1 actor=subject.player.technician target=AlternativeTechnicalBox_0 attempt=1 result=3 noisy=false tag=attempt.technical.override_success\n")
		TEXT("LogAlternative: Display: PrisonAccessEvent order=2 actor=subject.player.technician target=AlternativeStorageDoor_0 attempt=0 result=4 noisy=false tag=attempt.access.open_existing");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_attempt_runtime_smoke_review"), LogReviewCommand),
		MakeCommandExecutionEvent(TEXT("call_attempt_runtime_smoke_review"), LogReviewCommand, LogReviewResult));
	ApplyInteractionAccessAttemptResolverSourceInspection(
		Plan,
		Execution,
		TEXT("call_attempt_runtime_smoke_source_inspection"));

	TestTrue(TEXT("runtime smoke log review should accumulate bounded success markers"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved);
	TestTrue(TEXT("runtime smoke log review should accumulate prison access event markers"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved);
	TestTrue(TEXT("bounded runtime smoke evidence should complete attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestEqual(TEXT("workflow should advance to proof_context_setup after bounded runtime smoke log review"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("proof_context_setup")));
	return true;
}

bool FFeatureWorkflowState_AttemptResolverAndLoggingCompletesFromBoundedAutomationCommandExecution::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("attempt_resolver_and_logging"));

	const FString AutomationWrapperCommand =
		TEXT("\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -Command '$stdoutLog = Join-Path (Get-Location) \"Saved/Logs/packet654_rerun_stdout_20260424_011743.log\"; $automationLog = Join-Path (Get-Location) \"Saved/Logs/packet654_rerun_automation_20260424_011743.log\"; & \"D:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe\" Alternative.uproject -unattended -nop4 -nosplash -NullRHI -NoSound -DDC-ForceMemoryCache -stdout -FullStdOutLogOutput \"-ExecCmds=Automation RunTests Alternative.PrisonAccess;Quit\" \"-TestExit=Automation Test Queue Empty\" (\"-AbsLog={0}\" -f $automationLog) 2>&1 | Tee-Object -FilePath $stdoutLog'");
	const FString AutomationWrapperResult =
		TEXT("LogInit: Command Line: -unattended -nop4 -nosplash -NullRHI -NoSound -DDC-ForceMemoryCache -stdout -FullStdOutLogOutput -ExecCmds=\"Automation RunTests Alternative.PrisonAccess;Quit\" -TestExit=\"Automation Test Queue Empty\" -AbsLog=X:/PublicExample/Unreal/SampleProject/Saved/Logs/packet654_rerun_automation_20260424_011743.log\n")
		TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={TechnicalRoute} Path={Alternative.PrisonAccess.TechnicalRoute}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={UnskilledRoute} Path={Alternative.PrisonAccess.UnskilledRoute}\n")
		TEXT("LogAlternative: Display: PrisonAccessEvent order=1 actor=subject.player.technician target=AlternativeTechnicalBox_0 attempt=1 result=3 noisy=false tag=attempt.technical.override_success\n")
		TEXT("LogAlternative: Display: PrisonAccessEvent order=2 actor=subject.player.technician target=AlternativeStorageDoor_0 attempt=0 result=4 noisy=false tag=attempt.access.open_existing\n")
		TEXT("STDOUT_LOG=X:/PublicExample/Unreal/SampleProject/Saved/Logs/packet654_rerun_stdout_20260424_011743.log\n")
		TEXT("AUTOMATION_LOG=X:/PublicExample/Unreal/SampleProject/Saved/Logs/packet654_rerun_automation_20260424_011743.log");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_attempt_runtime_wrapper"), AutomationWrapperCommand),
		MakeCommandExecutionEvent(TEXT("call_attempt_runtime_wrapper"), AutomationWrapperCommand, AutomationWrapperResult));
	ApplyInteractionAccessAttemptResolverSourceInspection(
		Plan,
		Execution,
		TEXT("call_attempt_runtime_wrapper_source_inspection"));

	TestTrue(TEXT("bounded automation wrapper should accumulate runtime smoke success markers"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved);
	TestTrue(TEXT("bounded automation wrapper should accumulate prison access event markers"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved);
	TestTrue(TEXT("bounded automation wrapper should complete attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestEqual(TEXT("workflow should advance after bounded automation wrapper evidence"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("proof_context_setup")));
	return true;
}

bool FFeatureWorkflowState_AttemptResolverAndLoggingRejectsDocsAndArbitraryCommandDrift::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("attempt_resolver_and_logging"));

	const FString DocsPath =
		TEXT("X:/PublicExample/Unreal/SampleProject/Docs/OsvayderUE/attempt_resolver_notes.md");
	const FString DocsContent =
		TEXT("Reviewer notes: AlternativePrisonAccessAttemptResolver and AlternativePrisonAccessEventSubsystem exist. ProofFixtureSmoke and PrisonAccessEvent were mentioned in planning notes.");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("read"), TEXT("call_attempt_docs_read"), DocsPath),
		MakeToolEvent(TEXT("read"), TEXT("call_attempt_docs_read"), DocsPath, DocsContent));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_attempt_log_listing"), TEXT("Get-ChildItem Saved\\Logs")),
		MakeCommandExecutionEvent(
			TEXT("call_attempt_log_listing"),
			TEXT("Get-ChildItem Saved\\Logs"),
			TEXT("Directory: X:/PublicExample/Unreal/SampleProject/Saved/Logs\npacket653_rerun_stdout_20260424_003729.log")));

	TestFalse(TEXT("docs must not count as direct attempt resolver source inspection"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved);
	TestFalse(TEXT("docs must not count as direct event subsystem source inspection"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved);
	TestFalse(TEXT("listing logs without bounded success markers must not count as runtime smoke proof"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved);
	TestFalse(TEXT("docs and arbitrary command drift must not complete attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestEqual(TEXT("docs and arbitrary command drift should keep the phase parked"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("attempt_resolver_and_logging")));
	return true;
}

bool FFeatureWorkflowState_AttemptResolverAndLoggingRejectsMutatingCommandExecutionDrift::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("attempt_resolver_and_logging"));

	const FString DriftCommand =
		TEXT("\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -Command 'Get-Content Docs/OsvayderUE/attempt_resolver_notes.md | Tee-Object -FilePath Saved/Logs/attempt_resolver_notes_copy.log'");
	const FString DriftResult =
		TEXT("Saved/Logs/attempt_resolver_notes_copy.log\n")
		TEXT("Reviewer notes: AlternativePrisonAccessAttemptResolver and AlternativePrisonAccessEventSubsystem exist.\n")
		TEXT("ProofFixtureSmoke and PrisonAccessEvent were mentioned in planning notes.");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_attempt_mutating_drift"), DriftCommand),
		MakeCommandExecutionEvent(TEXT("call_attempt_mutating_drift"), DriftCommand, DriftResult));

	TestFalse(TEXT("mutating drift wrapper must not count as runtime smoke proof"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved);
	TestFalse(TEXT("mutating drift wrapper must not count as prison access event proof"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved);
	TestFalse(TEXT("mutating drift wrapper must not complete attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestEqual(TEXT("mutating drift wrapper should leave the phase parked"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("attempt_resolver_and_logging")));
	return true;
}

bool FFeatureWorkflowState_AttemptResolverAndLoggingReadOnlySourceProgressAvoidsPrematureStopLoss::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("attempt_resolver_and_logging"));

	for (int32 Index = 0; Index < 5; ++Index)
	{
		const FString SearchCommand = FString::Printf(
			TEXT("Select-String -Path Source/Alternative/PrisonAccess/*.cpp -Pattern \"PrisonAccess\" | Select-Object -First 1 # %d"),
			Index);
		const FString SearchResult =
			TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessWorldSubsystem.cpp:42: FString View = TEXT(\"PrisonAccess preview\")");
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(FString::Printf(TEXT("call_attempt_source_probe_%d"), Index), SearchCommand),
			MakeCommandExecutionEvent(
				FString::Printf(TEXT("call_attempt_source_probe_%d"), Index),
				SearchCommand,
				SearchResult));
	}

	TestFalse(TEXT("five read-only probes alone must not trigger stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("five read-only probes should consume the existing command_execution budget"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		5);

	const FString AttemptResolverCommand =
		TEXT("Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp");
	const FString AttemptResolverSource =
		TEXT("FAlternativePrisonAccessAttemptOutcome FAlternativePrisonAccessAttemptResolver::ResolveDoorAttempt(const AActor* Interactor, const FAlternativeDoorState& DoorState, const FName FacilityId, const FName AccessPointTag)");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_attempt_source_progress"), AttemptResolverCommand),
		MakeCommandExecutionEvent(TEXT("call_attempt_source_progress"), AttemptResolverCommand, AttemptResolverSource));

	TestTrue(TEXT("the valid relative resolver source read should still be recorded"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved);
	TestFalse(TEXT("valid relative source progress must not trip stop-loss at the budget edge"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("progressing source evidence should reset the command_execution drift budget"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);

	const FString EventSubsystemCommand =
		TEXT("Get-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp");
	const FString EventSubsystemSource =
		TEXT("void UAlternativePrisonAccessEventSubsystem::RecordEvent(const FAlternativePrisonAccessEventRecord& Record)\nTEXT(\"PrisonAccessEvent order=%d actor=%s target=%s attempt=%d result=%d noisy=%s tag=%s\")");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_attempt_source_completion"), EventSubsystemCommand),
		MakeCommandExecutionEvent(TEXT("call_attempt_source_completion"), EventSubsystemCommand, EventSubsystemSource));

	TestTrue(TEXT("the second relative source read should complete the phase before stop-loss"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestFalse(TEXT("the sufficient source-inspection sequence must finish without stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("workflow should still advance to proof_context_setup after the sequence completes"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("proof_context_setup")));
	return true;
}

bool FFeatureWorkflowState_ProofContextObservationAdvancesWithoutMutation::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("proof_context_setup"));

	const FString OpenLevelInput =
		TEXT("{\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\"}");
	const FString OpenLevelResult =
		TEXT("Opened level /Game/PrisonAccess/Maps/Lvl_PrisonAccessProof");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("open_level"), TEXT("call_open_fixture_map"), OpenLevelInput),
		MakeToolEvent(TEXT("open_level"), TEXT("call_open_fixture_map"), OpenLevelInput, OpenLevelResult));

	const FString ActorQueryInput =
		TEXT("{\"level_path\":\"/Game/PrisonAccess/Maps/Lvl_PrisonAccessProof\",\"include_tags\":true}");
	const FString ActorQueryResult =
		TEXT("Actors: Proof_PrisonAccess_Door, Proof_PrisonAccess_ControlBox, Proof_PrisonAccess_EscapeItem");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("get_level_actors"), TEXT("call_observe_fixture"), ActorQueryInput),
		MakeToolEvent(TEXT("get_level_actors"), TEXT("call_observe_fixture"), ActorQueryInput, ActorQueryResult));

	TestTrue(TEXT("actor observation should mark placed runtime actors available"),
		Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable);
	TestEqual(TEXT("proof context setup should advance after truthful observation"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("compile_gate")));
	TestTrue(TEXT("proof_context_setup should be recorded as completed"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("proof_context_setup")));
	return true;
}

bool FFeatureWorkflowState_AutomationDiscoveryAcceptsRealSuccessMarkers::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("automation_discovery_gate"));
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;

	const FString ToolResult =
		TEXT("LogAutomationController: 5400 tests available on Worker01\n")
		TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={InputPathTruth} Path={Alternative.PrisonAccess.InputPathTruth}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={TechnicalRoute} Path={Alternative.PrisonAccess.TechnicalRoute}\n")
		TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeAutomationRunTestsEvent(TEXT("call_automation_real_success")),
		MakeAutomationRunTestsEvent(TEXT("call_automation_real_success"), ToolResult));

	TestEqual(TEXT("discovery count should come from the UE discovery line"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		7);
	TestEqual(TEXT("executed count should come from completed result markers, not the 5400-tests substring"),
		Plan.FeatureWorkflow.AutomationExecutedCount,
		3);
	TestEqual(TEXT("passed count should fall back to successful completion markers"),
		Plan.FeatureWorkflow.AutomationPassedCount,
		3);
	TestTrue(TEXT("automation discovery gate should complete on real success markers"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestEqual(TEXT("workflow should advance to runtime_proof after truthful automation execution"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_proof")));
	TestEqual(TEXT("proof prerequisites should become satisfied"),
		Plan.FeatureWorkflow.ProofPrerequisiteState,
		FString(TEXT("satisfied")));
	TestTrue(TEXT("automation discovery blocker should be cleared after success"),
		Plan.FeatureWorkflow.BlockerFamily.IsEmpty());
	return true;
}

bool FFeatureWorkflowState_AutomationDiscoveryAcceptsSummaryCountsWithCleanExit::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("automation_discovery_gate"));

	const FString ToolResult =
		TEXT("LogAutomationCommandLine: Display: Found 4 automation tests based on 'Alternative.PrisonAccess'\n")
		TEXT("LogAutomationCommandLine: Display: Passed: 4\n")
		TEXT("LogAutomationCommandLine: Display: Failed: 0\n")
		TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeAutomationRunTestsEvent(TEXT("call_automation_summary_success")),
		MakeAutomationRunTestsEvent(TEXT("call_automation_summary_success"), ToolResult));

	TestEqual(TEXT("discovery count should be parsed from summary output"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		4);
	TestEqual(TEXT("executed count should fall back to passed+failed summary counts on clean exit"),
		Plan.FeatureWorkflow.AutomationExecutedCount,
		4);
	TestEqual(TEXT("passed count should keep the parsed summary count"),
		Plan.FeatureWorkflow.AutomationPassedCount,
		4);
	TestEqual(TEXT("failed count should keep the parsed summary count"),
		Plan.FeatureWorkflow.AutomationFailedCount,
		0);
	TestTrue(TEXT("summary counts with clean exit should complete automation_discovery_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	return true;
}

bool FFeatureWorkflowState_AutomationDiscoveryZeroDiscoveryStillFails::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("automation_discovery_gate"));

	const FString ToolResult =
		TEXT("LogAutomationCommandLine: Display: Found 0 automation tests based on 'Alternative.PrisonAccess'\n")
		TEXT("LogAutomationCommandLine: Display: No automation tests matched\n")
		TEXT("LogAutomationCommandLine: Display: Ran 0 tests");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeAutomationRunTestsEvent(TEXT("call_automation_zero")),
		MakeAutomationRunTestsEvent(TEXT("call_automation_zero"), ToolResult));

	TestEqual(TEXT("zero-test discovery must stay zero"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		0);
	TestEqual(TEXT("zero-test execution must stay zero"),
		Plan.FeatureWorkflow.AutomationExecutedCount,
		0);
	TestTrue(TEXT("automation discovery gate should fail for true zero-discovery"),
		Plan.FeatureWorkflow.FailedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestEqual(TEXT("zero-discovery failure should keep the automation blocker family"),
		Plan.FeatureWorkflow.BlockerFamily,
		FString(TEXT("automation_discovery_failed")));
	TestEqual(TEXT("zero-discovery failure should preserve the existing blocker detail"),
		Plan.FeatureWorkflow.BlockerDetail,
		FString(TEXT("discoverable_automation_tests_eq_0")));
	TestEqual(TEXT("runtime proof should remain blocked when no automation is available"),
		Plan.FeatureWorkflow.RuntimeProofState,
		FString(TEXT("blocked")));
	return true;
}

bool FFeatureWorkflowState_CompileGateVerificationOnlyAutomationAdvances::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("compile_gate"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("pending");
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;

	const FString ToolResult =
		TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={InputPathTruth} Path={Alternative.PrisonAccess.InputPathTruth}\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
		TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeAutomationRunTestsEvent(TEXT("call_compile_gate_verification_only")),
		MakeAutomationRunTestsEvent(TEXT("call_compile_gate_verification_only"), ToolResult));

	TestEqual(TEXT("verification-only rerun should mark compile proof as not required"),
		Plan.FeatureWorkflow.CompileProofState,
		FString(TEXT("not_required")));
	TestTrue(TEXT("verification-only automation should complete compile_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("compile_gate")));
	TestTrue(TEXT("verification-only automation should complete automation_discovery_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestEqual(TEXT("workflow should advance to runtime_proof after compile and automation gates"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_proof")));
	return true;
}

bool FFeatureWorkflowState_CompileGateDoesNotAdvanceOnArbitraryCommandExecution::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("compile_gate"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("pending");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_compile_gate_non_automation"), TEXT("Get-ChildItem Saved\\Logs")),
		MakeCommandExecutionEvent(
			TEXT("call_compile_gate_non_automation"),
			TEXT("Get-ChildItem Saved\\Logs"),
			TEXT("Directory: Saved\\Logs\npacket651_full_build.log")));

	TestFalse(TEXT("arbitrary command execution must not complete compile_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("compile_gate")));
	TestEqual(TEXT("compile_gate should remain current on non-automation commands"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("compile_gate")));
	TestEqual(TEXT("compile proof state should stay pending on non-automation commands"),
		Plan.FeatureWorkflow.CompileProofState,
		FString(TEXT("pending")));
	TestTrue(TEXT("automation discovery should stay untouched on non-automation commands"),
		Plan.FeatureWorkflow.AutomationDiscoveryCommand.IsEmpty());
	return true;
}

bool FFeatureWorkflowState_CompileGateRequiredCompileBlocksAutomationAdvance::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("compile_gate"));
	Plan.CompileProof.bCompiledModuleMutationObserved = true;
	Plan.FeatureWorkflow.bCompileProofRequired = true;
	Plan.FeatureWorkflow.CompileProofState = TEXT("pending");
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;

	const FString ToolResult =
		TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
		TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
		TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeAutomationRunTestsEvent(TEXT("call_compile_gate_required_compile")),
		MakeAutomationRunTestsEvent(TEXT("call_compile_gate_required_compile"), ToolResult));

	TestFalse(TEXT("required compile path must not complete compile_gate from automation alone"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("compile_gate")));
	TestFalse(TEXT("required compile path must not enter automation_discovery_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestEqual(TEXT("required compile path should remain parked in compile_gate"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("compile_gate")));
	TestEqual(TEXT("automation truth may still be parsed while compile gate stays blocked"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		7);
	return true;
}

bool FFeatureWorkflowState_Review9VerificationOnlyFlowDoesNotParkAtCompileGate::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("project_context_preflight"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("pending");
	Plan.CompileProof.bCompiledModuleMutationObserved = false;

	ApplyReview9AutomationLogEvidence(Plan, Execution);
	TestEqual(TEXT("review9 automation log evidence should be captured before compile_gate"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		7);
	TestEqual(TEXT("review9 automation log should retain executed count before compile_gate"),
		Plan.FeatureWorkflow.AutomationExecutedCount,
		7);

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_review9_preflight"), TEXT("Write-Output project_context_preflight_ready")),
		MakeCommandExecutionEvent(
			TEXT("call_review9_preflight"),
			TEXT("Write-Output project_context_preflight_ready"),
			TEXT("Project context preflight completed for feature.interaction_access_slice_v1")));
	ApplyInteractionAccessContractCompletion(Plan, Execution);
	ApplyInputReuseEvidence(Plan, Execution, TEXT("osvayderue/"));
	ApplyRuntimeActorEvidence(Plan, Execution, TEXT("osvayderue/"));
	ApplyAttemptResolverSourceEvidence(Plan, Execution);
	ApplyRuntimeActorEvidence(
		Plan,
		Execution,
		TEXT("osvayderue/"),
		TEXT("call_review9_proof_context_open"),
		TEXT("call_review9_proof_context_actors"));

	TestEqual(TEXT("review9 replay should arrive at compile_gate before the no-mutation inspection"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("compile_gate")));
	ApplyCompileGateNoMutationInspection(Plan, Execution);
	ApplyRuntimeProofEvidence(Plan, Execution);

	TestFalse(TEXT("review9 verification-only replay must not stop-loss at compile_gate"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("compile_gate should complete as not_required"),
		Plan.FeatureWorkflow.CompileProofState,
		FString(TEXT("not_required")));
	TestTrue(TEXT("compile_gate should complete after no-mutation evidence"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("compile_gate")));
	TestTrue(TEXT("already-observed automation should complete automation_discovery_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestTrue(TEXT("runtime proof should complete after carried-forward automation"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_proof")));
	TestEqual(TEXT("review9 verification-only replay should reach memory_update"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("memory_update")));
	return true;
}

bool FFeatureWorkflowState_CompileGateNoCompileRequiredAdvancesWithoutUBT::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("compile_gate"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("pending");
	Plan.CompileProof.bCompiledModuleMutationObserved = false;
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);

	ApplyCompileGateNoMutationInspection(Plan, Execution);

	TestFalse(TEXT("no-compile-required inspection must not trigger stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestFalse(TEXT("no-compile-required inspection must not request compile proof"),
		Plan.FeatureWorkflow.bCompileProofRequired);
	TestEqual(TEXT("compile proof state should become not_required without UBT"),
		Plan.FeatureWorkflow.CompileProofState,
		FString(TEXT("not_required")));
	TestTrue(TEXT("compile_gate should complete from no-mutation read-only evidence"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("compile_gate")));
	TestEqual(TEXT("workflow should advance to automation_discovery_gate when no automation is cached"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("automation_discovery_gate")));
	TestTrue(TEXT("compile completion must not fabricate a UBT compile outcome"),
		Plan.CompileProof.LastCompileProofOutcome.IsEmpty());
	return true;
}

bool FFeatureWorkflowState_AutomationProofBeforeCompileGateIsCarriedForward::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("pending");
	Plan.CompileProof.bCompiledModuleMutationObserved = false;

	ApplyReview9AutomationLogEvidence(Plan, Execution);
	TestEqual(TEXT("automation log read before compile_gate should preserve discovery count"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		7);
	TestEqual(TEXT("automation log read before compile_gate should preserve executed count"),
		Plan.FeatureWorkflow.AutomationExecutedCount,
		7);
	TestEqual(TEXT("automation log read before compile_gate should preserve pass count"),
		Plan.FeatureWorkflow.AutomationPassedCount,
		7);
	TestEqual(TEXT("automation log read before compile_gate should preserve failed count"),
		Plan.FeatureWorkflow.AutomationFailedCount,
		0);

	PrimeWorkflowToPhase(Plan.FeatureWorkflow, TEXT("compile_gate"));
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);
	ApplyCompileGateNoMutationInspection(Plan, Execution);

	TestTrue(TEXT("cached automation should complete automation_discovery_gate after compile_gate closes"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestEqual(TEXT("carried-forward automation should advance workflow to runtime_proof"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_proof")));
	TestEqual(TEXT("carried-forward automation count should not be blanked"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		7);
	return true;
}

bool FFeatureWorkflowState_Review10FullLiveNewSessionTraceReachesMemoryUpdate::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("project_context_preflight"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("pending");
	Plan.CompileProof.bCompiledModuleMutationObserved = false;

	ApplyProjectContextPreflight(Plan, Execution, TEXT("call_review10_preflight"));
	StartReview10DelayedSourceReads(Plan, Execution);
	ApplyInteractionAccessContractCompletion(Plan, Execution);
	CompleteReview10DelayedSourceReads(Plan, Execution);
	ApplyInputReuseEvidence(Plan, Execution, TEXT("osvayderue/"));
	ApplyRuntimeActorEvidence(Plan, Execution, TEXT("osvayderue/"));
	ApplyRuntimeActorEvidence(
		Plan,
		Execution,
		TEXT("osvayderue/"),
		TEXT("call_review10_proof_context_open"),
		TEXT("call_review10_proof_context_actors"));
	ApplyStdoutOnlyReview10AutomationEvidence(Plan, Execution);
	ApplyRuntimeProofEvidence(Plan, Execution);

	TestFalse(TEXT("review10 full replay must not stop-loss before delayed evidence is attributed"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("review10 source replay should observe attempt resolver source"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved);
	TestTrue(TEXT("review10 source replay should observe event subsystem source"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved);
	TestTrue(TEXT("review10 stdout automation should observe runtime smoke success"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved);
	TestTrue(TEXT("review10 stdout automation should observe prison access events"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved);
	TestTrue(TEXT("review10 replay should complete input_asset_authoring"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestTrue(TEXT("review10 replay should complete attempt_resolver_and_logging"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestTrue(TEXT("review10 replay should complete automation_discovery_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestEqual(TEXT("review10 full replay should reach memory_update"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("memory_update")));
	return true;
}

bool FFeatureWorkflowState_Review10DelayedSourceResultsDoNotSpendInputPhaseStopLoss::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("project_context_preflight"));

	ApplyProjectContextPreflight(Plan, Execution, TEXT("call_review10_delayed_preflight"));
	StartReview10DelayedSourceReads(Plan, Execution);
	ApplyInteractionAccessContractCompletion(Plan, Execution);
	TestEqual(TEXT("delayed source results should now arrive while current phase is input_asset_authoring"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));

	CompleteReview10DelayedSourceReads(Plan, Execution);

	TestFalse(TEXT("delayed source results from the previous phase must not stop-loss input_asset_authoring"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("delayed source evidence must not spend the input phase command no-advance budget"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);
	TestTrue(TEXT("delayed attempt resolver source should hydrate the cross-phase observation"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved);
	TestTrue(TEXT("delayed event subsystem source should hydrate the cross-phase observation"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved);
	return true;
}

bool FFeatureWorkflowState_SourceInspectionEvidenceHydratesAttemptResolverObservationAcrossPhaseBoundaries::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("project_context_preflight"));

	ApplyProjectContextPreflight(Plan, Execution, TEXT("call_review10_hydrate_preflight"));
	StartReview10DelayedSourceReads(Plan, Execution);
	ApplyInteractionAccessContractCompletion(Plan, Execution);
	CompleteReview10DelayedSourceReads(Plan, Execution);
	ApplyInputReuseEvidence(Plan, Execution);
	ApplyRuntimeActorEvidence(Plan, Execution);

	TestFalse(TEXT("source hydration should not stop-loss before attempt phase"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("attempt resolver phase should complete from source evidence cached earlier"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging")));
	TestEqual(TEXT("cached attempt resolver evidence should move workflow to proof_context_setup"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("proof_context_setup")));
	return true;
}

bool FFeatureWorkflowState_InputAssetAuthoringUsesAccumulatedSeparateAssetAndEnhancedInputEvidence::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	const FString AssetSearchTool = TEXT("asset_search");
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(AssetSearchTool, TEXT("call_review10_imc_only"), TEXT("{\"path_filter\":\"/Game/PrisonAccess/Input\",\"name_pattern\":\"IMC_PrisonAccessProof\"}")),
		MakeToolEvent(
			AssetSearchTool,
			TEXT("call_review10_imc_only"),
			TEXT("{\"path_filter\":\"/Game/PrisonAccess/Input\",\"name_pattern\":\"IMC_PrisonAccessProof\"}"),
			TEXT("{\"assets\":[{\"path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"}]}")));
	TestEqual(TEXT("IMC-only asset evidence should not complete input phase yet"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("input_asset_authoring")));

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(AssetSearchTool, TEXT("call_review10_ia_only"), TEXT("{\"path_filter\":\"/Game/Variant_SideScrolling/Input/Actions\",\"name_pattern\":\"IA_Interact\"}")),
		MakeToolEvent(
			AssetSearchTool,
			TEXT("call_review10_ia_only"),
			TEXT("{\"path_filter\":\"/Game/Variant_SideScrolling/Input/Actions\",\"name_pattern\":\"IA_Interact\"}"),
			TEXT("{\"assets\":[{\"path\":\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\"}]}")));
	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeToolEvent(TEXT("enhanced_input"), TEXT("call_review10_query_context"), TEXT("{\"operation\":\"query_context\",\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"}")),
		MakeToolEvent(
			TEXT("enhanced_input"),
			TEXT("call_review10_query_context"),
			TEXT("{\"operation\":\"query_context\",\"context_path\":\"/Game/PrisonAccess/Input/IMC_PrisonAccessProof.IMC_PrisonAccessProof\"}"),
			TEXT("{\"persistent_project_input\":true,\"mappings\":[{\"action_path\":\"/Game/Variant_SideScrolling/Input/Actions/IA_Interact.IA_Interact\",\"key\":\"E\"}]}")));

	TestTrue(TEXT("separate asset and enhanced input evidence should accumulate to sufficient reuse"),
		Plan.FeatureWorkflow.InteractionAccessReuseObservation.HasSufficientReuseEvidence());
	TestTrue(TEXT("accumulated input evidence should complete input_asset_authoring"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring")));
	TestEqual(TEXT("input phase should advance after accumulated evidence"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_actor_state")));
	return true;
}

bool FFeatureWorkflowState_StdoutOnlyAutomationLogCountsWhenAbsLogFileMissing::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("automation_discovery_gate"));

	ApplyStdoutOnlyReview10AutomationEvidence(Plan, Execution);

	TestEqual(TEXT("stdout-only automation evidence should preserve discovery count"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		7);
	TestEqual(TEXT("stdout-only automation evidence should preserve executed count"),
		Plan.FeatureWorkflow.AutomationExecutedCount,
		7);
	TestEqual(TEXT("stdout-only automation evidence should preserve failed count"),
		Plan.FeatureWorkflow.AutomationFailedCount,
		0);
	TestTrue(TEXT("stdout-only automation evidence should complete automation_discovery_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestTrue(TEXT("stdout-only automation should record runtime smoke markers"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved);
	TestTrue(TEXT("stdout-only automation should record prison access events"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved);
	return true;
}

bool FFeatureWorkflowState_Review11AutomationDiscoveryGateAcceptsPassingAbsLogAfterNoise::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("automation_discovery_gate"));
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = false;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = false;

	const FString AbsLogPath = WriteReview11AutomationLogFixture(
		TEXT("passing_abslog_after_noise"),
		MakeReview11NoisyAbsLogResult(true));
	ApplyReview11AbsLogAutomationEvidence(
		Plan,
		Execution,
		TEXT("call_review11_passing_abslog"),
		AbsLogPath);

	TestFalse(TEXT("review11 noisy passing AbsLog should not stop-loss automation discovery"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("review11 noisy passing AbsLog should complete automation_discovery_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestEqual(TEXT("review11 noisy AbsLog should preserve discovery count"),
		Plan.FeatureWorkflow.AutomationDiscoveryCount,
		7);
	TestEqual(TEXT("review11 noisy AbsLog should preserve executed count"),
		Plan.FeatureWorkflow.AutomationExecutedCount,
		7);
	TestEqual(TEXT("review11 noisy AbsLog should preserve pass count"),
		Plan.FeatureWorkflow.AutomationPassedCount,
		7);
	TestEqual(TEXT("review11 noisy AbsLog should preserve zero failures"),
		Plan.FeatureWorkflow.AutomationFailedCount,
		0);
	TestEqual(TEXT("review11 noisy passing AbsLog should advance to runtime_proof"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("runtime_proof")));
	return true;
}

bool FFeatureWorkflowState_Review11AutomationDiscoveryHydratesRuntimeSmokeAndPrisonAccessEvent::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("automation_discovery_gate"));
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = false;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = false;

	const FString AbsLogPath = WriteReview11AutomationLogFixture(
		TEXT("hydrate_runtime_observation"),
		MakeReview11NoisyAbsLogResult(true));
	ApplyReview11AbsLogAutomationEvidence(
		Plan,
		Execution,
		TEXT("call_review11_hydrate_runtime"),
		AbsLogPath);

	TestTrue(TEXT("review11 AbsLog should hydrate runtime smoke success from ProofFixtureSmoke success"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved);
	TestTrue(TEXT("review11 AbsLog should hydrate prison access event observation"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved);
	TestTrue(TEXT("review11 complete observation should satisfy source and runtime evidence"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.HasCompleteEvidence());
	return true;
}

bool FFeatureWorkflowState_Review11AutomationNoiseDoesNotCauseFalseDiscoveryFailure::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("automation_discovery_gate"));
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = false;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = false;

	const FString AbsLogPath = WriteReview11AutomationLogFixture(
		TEXT("noise_then_passing_block"),
		MakeReview11NoisyAbsLogResult(true));
	ApplyReview11AbsLogAutomationEvidence(
		Plan,
		Execution,
		TEXT("call_review11_noise_then_pass"),
		AbsLogPath);

	TestFalse(TEXT("startup noise before a passing proof block must not fail automation_discovery_gate"),
		Plan.FeatureWorkflow.FailedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestTrue(TEXT("startup noise before a passing proof block should clear automation blocker"),
		Plan.FeatureWorkflow.BlockerFamily.IsEmpty());
	TestEqual(TEXT("runtime proof should remain pending until the runtime proof tool runs"),
		Plan.FeatureWorkflow.RuntimeProofState,
		FString(TEXT("pending")));
	return true;
}

bool FFeatureWorkflowState_Review11AutomationNoiseWithoutPassingProofStillFails::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("automation_discovery_gate"));
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved = false;
	Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved = false;

	const FString AbsLogPath = WriteReview11AutomationLogFixture(
		TEXT("noise_without_passing_block"),
		MakeReview11NoisyAbsLogResult(false));
	ApplyReview11AbsLogAutomationEvidence(
		Plan,
		Execution,
		TEXT("call_review11_noise_without_pass"),
		AbsLogPath);

	TestFalse(TEXT("noise-only AbsLog must not complete automation_discovery_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestTrue(TEXT("noise-only AbsLog should fail automation_discovery_gate"),
		Plan.FeatureWorkflow.FailedPhaseIds.Contains(TEXT("automation_discovery_gate")));
	TestEqual(TEXT("noise-only AbsLog should keep runtime proof blocked"),
		Plan.FeatureWorkflow.RuntimeProofState,
		FString(TEXT("blocked")));
	TestFalse(TEXT("noise-only AbsLog must not fabricate runtime smoke"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved);
	TestFalse(TEXT("noise-only AbsLog must not fabricate prison access event"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved);
	return true;
}

bool FFeatureWorkflowState_ProjectLocalReadOnlyEvidenceDoesNotSpendStopLossBudgetWhenItAddsOrConfirmsEvidence::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("input_asset_authoring"));

	StartReview10DelayedSourceReads(Plan, Execution);
	CompleteReview10DelayedSourceReads(Plan, Execution);
	CompleteReview10DelayedSourceReads(Plan, Execution);

	TestFalse(TEXT("project-local read-only source evidence should not stop-loss while confirming known evidence"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("project-local evidence confirmations should not spend no-advance budget"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);
	TestTrue(TEXT("project-local confirmations should retain attempt resolver observation"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved);
	TestTrue(TEXT("project-local confirmations should retain event subsystem observation"),
		Plan.FeatureWorkflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved);
	return true;
}

bool FFeatureWorkflowState_CompileGateReadOnlyPlanInspectionDoesNotConsumeStopLossWhenCompileNotRequired::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("compile_gate"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("pending");
	Plan.CompileProof.bCompiledModuleMutationObserved = false;
	Plan.FeatureWorkflow.bKnownProofMapAvailable = true;
	Plan.FeatureWorkflow.bPlacedRuntimeActorsAvailable = true;
	Plan.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");

	for (int32 Index = 0; Index < 6; ++Index)
	{
		ApplyCompileGateNoMutationInspection(
			Plan,
			Execution,
			FString::Printf(TEXT("call_compile_gate_no_mutation_probe_%d"), Index));
	}

	TestFalse(TEXT("bounded no-mutation compile_gate inspections must not trigger stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestEqual(TEXT("bounded compile_gate inspections without attempt observation should remain parked"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("compile_gate")));
	TestEqual(TEXT("bounded no-mutation compile_gate inspections must not consume no-advance budget"),
		Plan.FeatureWorkflow.CommandExecutionCallsWithoutPhaseAdvance,
		0);
	return true;
}

bool FFeatureWorkflowState_CompileGateArbitraryCommandDriftStillStopLosses::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("compile_gate"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("pending");
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);

	for (int32 Index = 0; Index < 6; ++Index)
	{
		const FString Command = FString::Printf(TEXT("Write-Output unrelated_compile_gate_probe_%d"), Index);
		ApplyToolRoundTrip(
			Plan,
			Execution,
			MakeCommandExecutionEvent(FString::Printf(TEXT("call_compile_gate_irrelevant_%d"), Index), Command),
			MakeCommandExecutionEvent(
				FString::Printf(TEXT("call_compile_gate_irrelevant_%d"), Index),
				Command,
				TEXT("unrelated output")));
	}

	TestTrue(TEXT("arbitrary compile_gate command drift should still stop-loss"),
		Plan.FeatureWorkflow.bStopLossTriggered);
	TestTrue(TEXT("arbitrary compile_gate drift should use the command no-advance stop-loss reason"),
		Plan.FeatureWorkflow.StopLossReason.Contains(TEXT("command_execution_without_phase_advance_gt_5")));
	TestFalse(TEXT("arbitrary compile_gate drift must not complete compile_gate"),
		Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("compile_gate")));
	return true;
}

bool FFeatureWorkflowState_UbtLogBackupUnauthorizedIsInfrastructureBlockerNotCompileFailure::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("compile_gate"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("not_required");
	Plan.CompileProof.bCompiledModuleMutationObserved = false;
	PrimeInteractionAccessProofPrereqsAndObservation(Plan);

	ApplyUbtLogBackupUnauthorizedResult(Plan, Execution);

	TestFalse(TEXT("UBT log backup UnauthorizedAccessException must not mark compiled source mutation"),
		Plan.CompileProof.bCompiledModuleMutationObserved);
	TestFalse(TEXT("UBT log backup failure must not request compile proof on verification-only truth"),
		Plan.FeatureWorkflow.bCompileProofRequired);
	TestEqual(TEXT("UBT log backup failure must not overwrite no-compile-required truth"),
		Plan.FeatureWorkflow.CompileProofState,
		FString(TEXT("not_required")));
	TestFalse(TEXT("UBT log backup failure before compilation must not record compile_failed outcome"),
		Plan.CompileProof.LastCompileProofOutcome.Equals(TEXT("failed"), ESearchCase::IgnoreCase));
	return true;
}

bool FFeatureWorkflowState_VerificationArchiveWriteWithSourceEvidenceDoesNotRequireCompileProof::RunTest(
	const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("memory_update"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("not_required");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");

	const FString ArchiveWriteCommand =
		TEXT("$p = Get-Content Saved/OsvayderUE/active_plan.json | ConvertFrom-Json; ")
		TEXT("$p.feature_workflow.source_evidence = \"Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp\"; ")
		TEXT("$p | ConvertTo-Json -Depth 32 | Set-Content Saved/OsvayderUE/PlanArchives/20260425-141636-packet654-review7-live-acceptance.active_plan.json");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_archive_write_source_evidence"), ArchiveWriteCommand),
		MakeCommandExecutionEvent(
			TEXT("call_archive_write_source_evidence"),
			ArchiveWriteCommand,
			TEXT("ARCHIVE_PATH=Saved/OsvayderUE/PlanArchives/20260425-141636-packet654-review7-live-acceptance.active_plan.json")));

	TestFalse(TEXT("manual archive writes under Saved must not be treated as compiled source mutation"),
		Plan.CompileProof.bCompiledModuleMutationObserved);
	TestFalse(TEXT("verification-only archive writes must not request compile proof"),
		Plan.FeatureWorkflow.bCompileProofRequired);
	TestEqual(TEXT("compile proof state should remain not_required"),
		Plan.FeatureWorkflow.CompileProofState,
		FString(TEXT("not_required")));
	TestEqual(TEXT("archive bookkeeping should not move the terminal workflow phase"),
		Plan.FeatureWorkflow.CurrentPhase,
		FString(TEXT("memory_update")));
	return true;
}

bool FFeatureWorkflowState_RealSourceWriteStillRequiresCompileProof::RunTest(const FString& Parameters)
{
	FAgentCanonExecution Execution;
	FOsvayderUEActivePlan Plan = MakeInteractionAccessPlan(Execution, TEXT("memory_update"));
	Plan.FeatureWorkflow.bCompileProofRequired = false;
	Plan.FeatureWorkflow.CompileProofState = TEXT("not_required");

	const FString SourceWriteCommand =
		TEXT("Set-Content Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp \"// changed\"");

	ApplyToolRoundTrip(
		Plan,
		Execution,
		MakeCommandExecutionEvent(TEXT("call_real_source_write"), SourceWriteCommand),
		MakeCommandExecutionEvent(
			TEXT("call_real_source_write"),
			SourceWriteCommand,
			TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp written")));

	TestTrue(TEXT("real writes to Source/*.cpp must still record compiled module mutation"),
		Plan.CompileProof.bCompiledModuleMutationObserved);
	TestTrue(TEXT("real compiled source writes must still require compile proof"),
		Plan.FeatureWorkflow.bCompileProofRequired);
	TestEqual(TEXT("compile proof state should become pending after source mutation"),
		Plan.FeatureWorkflow.CompileProofState,
		FString(TEXT("pending")));
	TestEqual(TEXT("mutation tool should be command_execution"),
		Plan.CompileProof.LastMutationToolName,
		FString(TEXT("command_execution")));
	return true;
}

#endif
