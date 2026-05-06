// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUEAgentTrace.h"

#include "Algo/Reverse.h"
#include "AgentExecutionControl.h"
#include "CodexCliRunner.h"
#include "OsvayderUECanonRouting.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "OsvayderUEConstants.h"
#include "OsvayderUECommandClassification.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEModule.h"
#include "OsvayderUERelayAgent.h"
#include "OsvayderUESettings.h"
#include "OsvayderUEStorageMigration.h"

namespace
{
	constexpr int32 MaxInMemoryTraceRecords = 600;
	constexpr int64 MaxTraceFileBytes = 10 * 1024 * 1024;

#if WITH_DEV_AUTOMATION_TESTS
	FString GTestTraceLogPathOverride;
#endif

	FString GetLegacyTraceLogPath()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!GTestTraceLogPathOverride.IsEmpty())
		{
			FString OverridePath = FPaths::ConvertRelativePathToFull(GTestTraceLogPathOverride);
			FPaths::NormalizeFilename(OverridePath);
			return FPaths::Combine(
				OsvayderUEStorageMigration::DeriveLegacyRootFromPreferred(FPaths::GetPath(OverridePath)),
				TEXT("agent_trace.jsonl"));
		}
#endif

		return FPaths::Combine(OsvayderUEStorageMigration::GetLegacySavedRoot(), TEXT("agent_trace.jsonl"));
	}

	bool ValidateTraceLogFile(const FString& CandidatePath, FString& OutValidationError)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *CandidatePath))
		{
			OutValidationError = FString::Printf(TEXT("Could not load trace log at %s"), *CandidatePath);
			return false;
		}

		return true;
	}

	FString SerializeJsonObjectCompact(const TSharedPtr<FJsonObject>& JsonObject)
	{
		if (!JsonObject.IsValid())
		{
			return FString();
		}

		FString JsonText;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		Writer->Close();
		return JsonText;
	}

	FString TruncateStringForPreview(const FString& Value, const int32 PreviewChars)
	{
		if (PreviewChars <= 0 || Value.Len() <= PreviewChars)
		{
			return Value;
		}

		return Value.Left(PreviewChars) + FString::Printf(TEXT("... [truncated %d chars]"), Value.Len() - PreviewChars);
	}

	TSharedPtr<FJsonValue> SanitizeValueForPreview(const TSharedPtr<FJsonValue>& Value, const int32 PreviewChars, const bool bIncludeRawJson);

	TSharedPtr<FJsonObject> SanitizeObjectForPreview(const TSharedPtr<FJsonObject>& Object, const int32 PreviewChars, const bool bIncludeRawJson)
	{
		TSharedPtr<FJsonObject> Sanitized = MakeShared<FJsonObject>();
		if (!Object.IsValid())
		{
			return Sanitized;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (!bIncludeRawJson &&
				(Pair.Key.Equals(TEXT("raw_provider_event"), ESearchCase::IgnoreCase) ||
				 Pair.Key.Equals(TEXT("raw_json"), ESearchCase::IgnoreCase)))
			{
				continue;
			}

			Sanitized->SetField(Pair.Key, SanitizeValueForPreview(Pair.Value, PreviewChars, bIncludeRawJson));
		}

		return Sanitized;
	}

	TSharedPtr<FJsonValue> SanitizeValueForPreview(const TSharedPtr<FJsonValue>& Value, const int32 PreviewChars, const bool bIncludeRawJson)
	{
		if (!Value.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}

		switch (Value->Type)
		{
		case EJson::String:
			return MakeShared<FJsonValueString>(TruncateStringForPreview(Value->AsString(), PreviewChars));

		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Value->TryGetObject(Object) && Object && Object->IsValid())
			{
				return MakeShared<FJsonValueObject>(SanitizeObjectForPreview(*Object, PreviewChars, bIncludeRawJson));
			}
			return MakeShared<FJsonValueNull>();
		}

		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> SanitizedArray;
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			SanitizedArray.Reserve(Array.Num());
			for (const TSharedPtr<FJsonValue>& Item : Array)
			{
				SanitizedArray.Add(SanitizeValueForPreview(Item, PreviewChars, bIncludeRawJson));
			}
			return MakeShared<FJsonValueArray>(SanitizedArray);
		}

		case EJson::Boolean:
			return MakeShared<FJsonValueBoolean>(Value->AsBool());

		case EJson::Number:
			return MakeShared<FJsonValueNumber>(Value->AsNumber());

		case EJson::Null:
		default:
			return MakeShared<FJsonValueNull>();
		}
	}

	FString BackendFilterLabel(const EOsvayderUEProviderBackend Backend)
	{
		return OsvayderUEProviderBackendToString(Backend);
	}

	FString GetCanonicalToolSlug(FString ToolName)
	{
		int32 LastSlashIdx = INDEX_NONE;
		if (ToolName.FindLastChar(TEXT('/'), LastSlashIdx))
		{
			ToolName = ToolName.Mid(LastSlashIdx + 1);
		}

		const int32 LastUnderscoreBlockIdx =
			ToolName.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (LastUnderscoreBlockIdx != INDEX_NONE)
		{
			ToolName = ToolName.Mid(LastUnderscoreBlockIdx + 2);
		}

		return ToolName;
	}

	bool IsVerificationToolName(const FString& ToolName)
	{
		static const TArray<FString> VerificationToolNames = {
			TEXT("livecoding_compile"),
			TEXT("execute_script"),
			TEXT("map_runtime_proof"),
			TEXT("multiplayer_audit"),
			TEXT("validate_blueprint"),
			TEXT("dependency_health"),
			TEXT("oss_session_proof"),
			TEXT("metadata_truth"),
			TEXT("cpp_reflection")
		};

		const FString NormalizedToolName = GetCanonicalToolSlug(ToolName);
		for (const FString& Known : VerificationToolNames)
		{
			if (NormalizedToolName.Equals(Known, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	bool IsRestartSurvivalSignalToolName(const FString& ToolName)
	{
		return GetCanonicalToolSlug(ToolName).Equals(TEXT("restart_survival"), ESearchCase::IgnoreCase);
	}

	FString DetermineTransportRetryBlockReason(
		const int32 ObservedToolUseCount,
		const bool bObservedRestartSurvivalSignal,
		const bool bObservedVerificationTool,
		const bool bObservedMutatingTool)
	{
		if (ObservedToolUseCount > 0)
		{
			return TEXT("tool_activity_observed");
		}
		if (bObservedRestartSurvivalSignal)
		{
			return TEXT("restart_survival_handoff_observed");
		}
		if (bObservedVerificationTool)
		{
			return TEXT("verification_side_effect_observed");
		}
		if (bObservedMutatingTool)
		{
			return TEXT("mutation_side_effect_observed");
		}
		return FString();
	}

	void CompleteFeatureWorkflowSummary(FAgentFeatureWorkflowState& Workflow, const FString& TimestampUtc, const bool bSuccess)
	{
		if (!Workflow.HasAnySignal())
		{
			return;
		}

		auto MarkPhaseCompleted = [&Workflow, &TimestampUtc](const FString& PhaseId)
		{
			if (FAgentFeatureWorkflowPhaseState* Phase = Workflow.FindMutablePhase(PhaseId))
			{
				Phase->Status = TEXT("completed");
				if (Phase->StartedAtUtc.IsEmpty())
				{
					Phase->StartedAtUtc = TimestampUtc;
				}
				if (Phase->CompletedAtUtc.IsEmpty())
				{
					Phase->CompletedAtUtc = TimestampUtc;
				}
			}
			Workflow.CompletedPhaseIds.AddUnique(PhaseId);
		};

		if (Workflow.bStopLossTriggered)
		{
			Workflow.TerminalStatus = TEXT("stop_loss");
			return;
		}

		const bool bCompilePassed = !Workflow.bCompileProofRequired
			|| Workflow.CompileProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase);
		const bool bRuntimePassed = !Workflow.bRuntimeProofRequired
			|| Workflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase);

		if (bRuntimePassed)
		{
			MarkPhaseCompleted(TEXT("memory_update"));
			Workflow.CurrentPhase = TEXT("memory_update");
		}

		if (!bSuccess || !bCompilePassed)
		{
			Workflow.TerminalStatus = TEXT("failed");
			return;
		}

		if (Workflow.AuthoringLaneState.Equals(TEXT("denied"), ESearchCase::IgnoreCase)
			&& !Workflow.bReducedProofModeAllowed)
		{
			Workflow.TerminalStatus = TEXT("failed");
			return;
		}

		if (Workflow.ProofPrerequisiteState.Equals(TEXT("missing"), ESearchCase::IgnoreCase)
			&& !Workflow.HasRuntimeProofPrerequisites())
		{
			Workflow.TerminalStatus = TEXT("failed");
			return;
		}

		if (Workflow.RuntimeProofState.Equals(TEXT("failed"), ESearchCase::IgnoreCase)
			|| Workflow.RuntimeProofState.Equals(TEXT("denied"), ESearchCase::IgnoreCase)
			|| Workflow.RuntimeProofState.Equals(TEXT("blocked"), ESearchCase::IgnoreCase))
		{
			Workflow.TerminalStatus = TEXT("failed");
			return;
		}

		Workflow.TerminalStatus = bRuntimePassed ? TEXT("completed_full") : TEXT("completed_partial");
	}
}

FOsvayderUEAgentTraceLog& FOsvayderUEAgentTraceLog::Get()
{
	static FOsvayderUEAgentTraceLog Instance;
	return Instance;
}

FString FOsvayderUEAgentTraceLog::BeginRun(
	const FAgentBackendStatus& Status,
	const FAgentRequestConfig& Config,
	const FString& UserPrompt,
	const bool bIncludeEngineContext,
	const bool bIncludeProjectContext)
{
	const FString RunId = MakeRunId();

	FAgentTraceActiveRunState ActiveRunState;
	ActiveRunState.RunId = RunId;
	ActiveRunState.Backend = Status.Backend;
	ActiveRunState.BackendDisplayName = Status.DisplayName;
	ActiveRunState.StartedAt = FDateTime::UtcNow();
	ActiveRunState.StartedSeconds = FPlatformTime::Seconds();
	ActiveRunState.CanonExecution = Config.CanonExecution;
	ActiveRunState.bIsTransportRetryReplay = Config.bIsTransportRetryReplay;
	ActiveRunState.TransportRetrySourceRunId = Config.TransportRetrySourceRunId;

	{
		FScopeLock Lock(&Mutex);
		ActiveRuns.Add(RunId, ActiveRunState);
		ActiveRunIdsByBackend.Add(static_cast<uint8>(Status.Backend), RunId);
	}

	TSharedPtr<FJsonObject> RunStartedPayload = BuildRunMetadataObject(Status, Config, bIncludeEngineContext, bIncludeProjectContext);
	AppendEvent(TEXT("run_started"), Status.Backend, RunStartedPayload, RunId);

	TSharedPtr<FJsonObject> SnapshotPayload = MakeShared<FJsonObject>();
	SnapshotPayload->SetStringField(TEXT("snapshot_source"), TEXT("orchestrator_preflight"));
	SnapshotPayload->SetStringField(TEXT("detail"), Status.Detail);
	SnapshotPayload->SetStringField(TEXT("auth_detail"), Status.AuthDetail);
	SnapshotPayload->SetStringField(TEXT("readiness"), AgentBackendReadinessToString(Status.Readiness));
	SnapshotPayload->SetStringField(TEXT("auth_state"), AgentBackendAuthStateToString(Status.AuthState));
	SnapshotPayload->SetBoolField(TEXT("available"), Status.bAvailable);
	SnapshotPayload->SetBoolField(TEXT("ready"), Status.bReady);
	AppendEvent(TEXT("backend_snapshot"), Status.Backend, SnapshotPayload, RunId);

	TSharedPtr<FJsonObject> PromptPayload = MakeShared<FJsonObject>();
	PromptPayload->SetStringField(TEXT("prompt_text"), UserPrompt);
	PromptPayload->SetNumberField(TEXT("prompt_length"), UserPrompt.Len());
	PromptPayload->SetBoolField(TEXT("include_engine_context"), bIncludeEngineContext);
	PromptPayload->SetBoolField(TEXT("include_project_context"), bIncludeProjectContext);
	PromptPayload->SetNumberField(TEXT("attached_image_count"), Config.AttachedImagePaths.Num());
	PromptPayload->SetNumberField(TEXT("bootstrap_context_length"), Config.ConversationBootstrapText.Len());
	AppendEvent(TEXT("user_prompt_submitted"), Status.Backend, PromptPayload, RunId);

	if (Config.CanonExecution.HasAnySignal())
	{
		TSharedPtr<FJsonObject> CanonTaskPayload = MakeShared<FJsonObject>();
		if (!Config.CanonExecution.DetectedSubsystem.IsEmpty())
		{
			CanonTaskPayload->SetStringField(TEXT("detected_subsystem"), Config.CanonExecution.DetectedSubsystem);
		}
		if (!Config.CanonExecution.TaskMode.IsEmpty())
		{
			CanonTaskPayload->SetStringField(TEXT("task_mode"), Config.CanonExecution.TaskMode);
		}
		CanonTaskPayload->SetBoolField(TEXT("canon_discovery_used"), Config.CanonExecution.bCanonDiscoveryUsed);
		CanonTaskPayload->SetBoolField(TEXT("approved_pattern_found"), Config.CanonExecution.bApprovedPatternFound);
		CanonTaskPayload->SetBoolField(TEXT("tool_exposure_adjusted"), Config.CanonExecution.bToolExposureAdjusted);
		CanonTaskPayload->SetBoolField(TEXT("brief_part_b_required"), Config.CanonExecution.bBriefPartBRequired);
		if (!Config.CanonExecution.RequestedToolFamily.IsEmpty())
		{
			CanonTaskPayload->SetStringField(TEXT("requested_tool_family"), Config.CanonExecution.RequestedToolFamily);
		}
		if (Config.CanonExecution.EnabledToolFamilyIds.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> EnabledFamilies;
			for (const FString& FamilyId : Config.CanonExecution.EnabledToolFamilyIds)
			{
				EnabledFamilies.Add(MakeShared<FJsonValueString>(FamilyId));
			}
			CanonTaskPayload->SetArrayField(TEXT("enabled_tool_family_ids"), EnabledFamilies);
		}
		if (Config.CanonExecution.MandatoryAnimationWorkflowSteps.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Step : Config.CanonExecution.MandatoryAnimationWorkflowSteps)
			{
				Values.Add(MakeShared<FJsonValueString>(Step));
			}
			CanonTaskPayload->SetArrayField(TEXT("mandatory_animation_workflow_steps"), Values);
		}
		if (Config.CanonExecution.ConditionalMandatoryAnimationWorkflowSteps.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Step : Config.CanonExecution.ConditionalMandatoryAnimationWorkflowSteps)
			{
				Values.Add(MakeShared<FJsonValueString>(Step));
			}
			CanonTaskPayload->SetArrayField(TEXT("conditional_mandatory_animation_workflow_steps"), Values);
		}
		if (Config.CanonExecution.CloseoutGateReasonCodes.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& ReasonCode : Config.CanonExecution.CloseoutGateReasonCodes)
			{
				Values.Add(MakeShared<FJsonValueString>(ReasonCode));
			}
			CanonTaskPayload->SetArrayField(TEXT("closeout_gate_reason_codes"), Values);
		}
		if (!Config.CanonExecution.LedgerPath.IsEmpty())
		{
			CanonTaskPayload->SetStringField(TEXT("ledger_path"), Config.CanonExecution.LedgerPath);
		}
		AppendEvent(TEXT("canon_task_routing"), Status.Backend, CanonTaskPayload, RunId);

		TSharedPtr<FJsonObject> CanonBriefPayload = MakeShared<FJsonObject>();
		CanonBriefPayload->SetBoolField(TEXT("brief_was_produced"), Config.CanonExecution.bBriefWasProduced);
		CanonBriefPayload->SetBoolField(TEXT("brief_part_b_required"), Config.CanonExecution.bBriefPartBRequired);
		if (Config.CanonExecution.ImplementationBriefLines.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> BriefValues;
			for (const FString& Line : Config.CanonExecution.ImplementationBriefLines)
			{
				BriefValues.Add(MakeShared<FJsonValueString>(Line));
			}
			CanonBriefPayload->SetArrayField(TEXT("implementation_brief_lines"), BriefValues);
		}
		AppendEvent(TEXT("canon_implementation_brief"), Status.Backend, CanonBriefPayload, RunId);

		if (Config.CanonExecution.FeatureWorkflow.HasAnySignal())
		{
			AppendEvent(TEXT("workflow_started"), Status.Backend, Config.CanonExecution.FeatureWorkflow.ToJsonObject(), RunId);

			TSharedPtr<FJsonObject> PhasePayload = MakeShared<FJsonObject>();
			PhasePayload->SetStringField(TEXT("feature_workflow_id"), Config.CanonExecution.FeatureWorkflow.FeatureWorkflowId);
			PhasePayload->SetStringField(TEXT("recipe_id"), Config.CanonExecution.FeatureWorkflow.RecipeId);
			PhasePayload->SetStringField(TEXT("role_id"), Config.CanonExecution.FeatureWorkflow.RoleId);
			PhasePayload->SetNumberField(TEXT("evidence_schema_version"), Config.CanonExecution.FeatureWorkflow.EvidenceSchemaVersion);
			PhasePayload->SetStringField(TEXT("phase_id"), Config.CanonExecution.FeatureWorkflow.CurrentPhase);
			AppendEvent(TEXT("phase_started"), Status.Backend, PhasePayload, RunId);
		}
	}

	return RunId;
}

void FOsvayderUEAgentTraceLog::AppendObservedEvent(const FString& RunId, const FAgentRunEvent& Event)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	if (!Event.ItemId.IsEmpty())
	{
		Payload->SetStringField(TEXT("item_id"), Event.ItemId);
	}

	switch (Event.Type)
	{
	case EAgentRunEventType::SessionInit:
	{
		{
			FScopeLock Lock(&Mutex);
			if (FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
			{
				ActiveRun->ProviderSessionId = Event.SessionId;
			}
		}

		Payload->SetStringField(TEXT("snapshot_source"), TEXT("provider_session_opened"));
		Payload->SetStringField(TEXT("provider_session_id"), Event.SessionId);
		if (!Event.RawJson.IsEmpty())
		{
			Payload->SetStringField(TEXT("raw_provider_event"), Event.RawJson);
		}
		AppendEvent(TEXT("backend_snapshot"), Event.Backend, Payload, RunId);
		return;
	}

	case EAgentRunEventType::TextContent:
		{
			FScopeLock Lock(&Mutex);
			if (FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
			{
				if (!ActiveRun->bFirstMutatingToolObserved)
				{
					ActiveRun->PreMutationAssistantText += Event.Text;
					if (ActiveRun->PreMutationAssistantText.Len() > 12000)
					{
						ActiveRun->PreMutationAssistantText = ActiveRun->PreMutationAssistantText.Right(12000);
					}
				}
			}
		}
		Payload->SetStringField(TEXT("text"), Event.Text);
		Payload->SetNumberField(TEXT("text_length"), Event.Text.Len());
		if (!Event.RawJson.IsEmpty())
		{
			Payload->SetStringField(TEXT("raw_provider_event"), Event.RawJson);
		}
		AppendEvent(TEXT("stream_text"), Event.Backend, Payload, RunId);
		return;

	case EAgentRunEventType::ToolUse:
		{
			bool bEmitPartBEvent = false;
			bool bMutatingTool = false;
			bool bPrimaryAssignedByThisTool = false;
			bool bBriefPartBProducedAfterUpdate = false;
			FString ClassifiedToolFamily;
			FString PrimaryMutationFamilyAfterUpdate;
			TArray<FString> PartBLines;
			{
				FScopeLock Lock(&Mutex);
				if (FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
				{
					const bool bHadPrimaryMutationFamily = !ActiveRun->CanonExecution.PrimaryMutationToolFamily.IsEmpty();
					ClassifiedToolFamily = OsvayderUECanonRouting::DetermineToolFamily(
						ActiveRun->CanonExecution,
						Event.ToolName,
						Event.ToolInput);
					bMutatingTool = OsvayderUECanonRouting::IsMutatingToolUse(Event.ToolName, Event.ToolInput);
					OsvayderUECanonRouting::UpdateFromToolUse(
						ActiveRun->CanonExecution,
						Event.ToolName,
						Event.ToolInput,
						ActiveRun->PreMutationAssistantText);
					PrimaryMutationFamilyAfterUpdate = ActiveRun->CanonExecution.PrimaryMutationToolFamily;
					bBriefPartBProducedAfterUpdate = ActiveRun->CanonExecution.bBriefPartBProduced;
					if (!bHadPrimaryMutationFamily && !ActiveRun->CanonExecution.PrimaryMutationToolFamily.IsEmpty())
					{
						ActiveRun->bFirstMutatingToolObserved = true;
						bEmitPartBEvent = true;
						bPrimaryAssignedByThisTool = true;
						PartBLines = ActiveRun->CanonExecution.ImplementationBriefPartBLines;
					}
					ActiveRun->ObservedToolUseCount++;
					ActiveRun->bObservedMutatingTool = ActiveRun->bObservedMutatingTool || bMutatingTool;
					ActiveRun->bObservedVerificationTool =
						ActiveRun->bObservedVerificationTool || IsVerificationToolName(Event.ToolName);
					ActiveRun->bObservedRestartSurvivalSignal =
						ActiveRun->bObservedRestartSurvivalSignal || IsRestartSurvivalSignalToolName(Event.ToolName);
				}
			}

			if (bEmitPartBEvent)
			{
				TSharedPtr<FJsonObject> PartBPayload = MakeShared<FJsonObject>();
				PartBPayload->SetBoolField(TEXT("brief_part_b_produced"), PartBLines.Num() >= 3);
				PartBPayload->SetBoolField(TEXT("before_first_mutating_tool"), PartBLines.Num() >= 3);
				if (PartBLines.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> BriefValues;
					for (const FString& Line : PartBLines)
					{
						BriefValues.Add(MakeShared<FJsonValueString>(Line));
					}
					PartBPayload->SetArrayField(TEXT("implementation_brief_part_b_lines"), BriefValues);
				}
				AppendEvent(TEXT("canon_implementation_brief_part_b"), Event.Backend, PartBPayload, RunId);
			}
			Payload->SetStringField(TEXT("tool_name"), Event.ToolName);
			Payload->SetStringField(TEXT("tool_call_id"), Event.ToolCallId);
			Payload->SetStringField(TEXT("tool_input"), Event.ToolInput);
			if (!ClassifiedToolFamily.IsEmpty())
			{
				Payload->SetStringField(TEXT("classified_tool_family"), ClassifiedToolFamily);
			}
			Payload->SetBoolField(TEXT("classified_mutating_tool"), bMutatingTool);
			Payload->SetBoolField(TEXT("primary_mutation_assigned_by_this_tool"), bPrimaryAssignedByThisTool);
			Payload->SetBoolField(TEXT("brief_part_b_produced_after_update"), bBriefPartBProducedAfterUpdate);
			if (!PrimaryMutationFamilyAfterUpdate.IsEmpty())
			{
				Payload->SetStringField(TEXT("primary_mutation_tool_family_after_update"), PrimaryMutationFamilyAfterUpdate);
			}
			if (!Event.RawJson.IsEmpty())
			{
				Payload->SetStringField(TEXT("raw_provider_event"), Event.RawJson);
			}
			{
				OsvayderUE::CommandClassification::FOsvayderUEExecutionTruthInputs TruthInputs;
				TruthInputs.RunId = RunId;
				TruthInputs.ToolName = Event.ToolName;
				TruthInputs.CommandInput = Event.ToolInput;
				TruthInputs.ToolFamily = ClassifiedToolFamily;
				TruthInputs.RawJson = Event.RawJson;
				TruthInputs.bClassifiedMutatingTool = bMutatingTool;
				TruthInputs.bPrimaryMutationAssigned = bPrimaryAssignedByThisTool;
				Payload->SetObjectField(
					TEXT("execution_truth"),
					OsvayderUE::CommandClassification::ClassifyExecutionTruth(TruthInputs).ToJsonObject());
			}
			AppendEvent(TEXT("tool_use"), Event.Backend, Payload, RunId);
			return;
		}

	case EAgentRunEventType::ToolResult:
		Payload->SetStringField(TEXT("tool_name"), Event.ToolName);
		Payload->SetStringField(TEXT("tool_call_id"), Event.ToolCallId);
		Payload->SetStringField(TEXT("tool_result"), Event.ToolResultContent);
		Payload->SetBoolField(TEXT("is_error"), Event.bIsError);
		if (Event.ExitCode != INDEX_NONE)
		{
			Payload->SetNumberField(TEXT("exit_code"), Event.ExitCode);
		}
		if (!Event.RawJson.IsEmpty())
		{
			Payload->SetStringField(TEXT("raw_provider_event"), Event.RawJson);
		}
		{
			FString ClassifiedToolFamily;
			bool bMutatingTool = false;
			{
				FScopeLock Lock(&Mutex);
				if (const FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
				{
					ClassifiedToolFamily = OsvayderUECanonRouting::DetermineToolFamily(
						ActiveRun->CanonExecution,
						Event.ToolName,
						Event.ToolInput);
					bMutatingTool = OsvayderUECanonRouting::IsMutatingToolUse(Event.ToolName, Event.ToolInput);
				}
			}
			OsvayderUE::CommandClassification::FOsvayderUEExecutionTruthInputs TruthInputs;
			TruthInputs.RunId = RunId;
			TruthInputs.ToolName = Event.ToolName;
			TruthInputs.CommandInput = Event.ToolInput;
			TruthInputs.ToolFamily = ClassifiedToolFamily;
			TruthInputs.ToolResult = Event.ToolResultContent;
			TruthInputs.RawJson = Event.RawJson;
			TruthInputs.ExitCode = Event.ExitCode;
			TruthInputs.bIsError = Event.bIsError;
			TruthInputs.bClassifiedMutatingTool = bMutatingTool;
			Payload->SetObjectField(
				TEXT("execution_truth"),
				OsvayderUE::CommandClassification::ClassifyExecutionTruth(TruthInputs).ToJsonObject());
		}
		if (IsRestartSurvivalSignalToolName(Event.ToolName))
		{
			FScopeLock Lock(&Mutex);
			if (FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
			{
				ActiveRun->bObservedRestartSurvivalSignal = true;
			}
		}

		// 631 Agent self-retrospective: plugin-side validation that the agent
		// actually exercised an empirical verification tool during this run.
		// Sets CanonExecution.bSelfVerificationAttempted + SelfVerificationResult
		// based on OBSERVED tool invocations, NOT agent self-claim. This is the
		// trust-calibration gate — when the agent reports `status: full` in its
		// final message, the plugin can cross-reference this canon_execution
		// state + surface a warning if mismatch. See 631 dispatch + audit doc.
		//
		// Canonical verification tool set: compile/link/runtime probes that
		// produce empirical truth about the agent's work, NOT read-only
		// discovery tools.
		{
			const bool bIsVerificationTool = IsVerificationToolName(Event.ToolName);
			if (bIsVerificationTool)
			{
				FScopeLock Lock(&Mutex);
				if (FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
				{
					ActiveRun->CanonExecution.bSelfVerificationAttempted = true;
					// Last verification tool wins for the result label — if
					// agent re-runs after a fix, the more recent outcome is
					// the one that matters for the final status call.
					ActiveRun->CanonExecution.SelfVerificationResult = Event.bIsError
						? TEXT("fail")
						: TEXT("pass");
					ActiveRun->bObservedVerificationTool = true;
					ActiveRun->bObservedRestartSurvivalSignal =
						ActiveRun->bObservedRestartSurvivalSignal || IsRestartSurvivalSignalToolName(Event.ToolName);
				}
			}
		}

		AppendEvent(TEXT("tool_result"), Event.Backend, Payload, RunId);
		return;

	case EAgentRunEventType::Result:
	{
		FString ProviderSessionId;
		{
			FScopeLock Lock(&Mutex);
			if (const FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
			{
				ProviderSessionId = ActiveRun->ProviderSessionId;
			}
		}

		Payload->SetStringField(TEXT("result_text"), Event.ResultText);
		Payload->SetBoolField(TEXT("is_error"), Event.bIsError);
		Payload->SetNumberField(TEXT("duration_ms"), Event.DurationMs);
		Payload->SetNumberField(TEXT("num_turns"), Event.NumTurns);
		Payload->SetNumberField(TEXT("total_cost_usd"), Event.TotalCostUsd);
		if (!ProviderSessionId.IsEmpty())
		{
			Payload->SetStringField(TEXT("provider_session_id"), ProviderSessionId);
		}
		if (!Event.RawJson.IsEmpty())
		{
			Payload->SetStringField(TEXT("raw_provider_event"), Event.RawJson);
		}
		AppendEvent(TEXT("result"), Event.Backend, Payload, RunId);
		return;
	}

	case EAgentRunEventType::AssistantMessage:
	case EAgentRunEventType::Unknown:
	default:
		return;
	}
}

void FOsvayderUEAgentTraceLog::AppendEvent(
	const FString& EventType,
	const EOsvayderUEProviderBackend Backend,
	const TSharedPtr<FJsonObject>& Payload,
	const FString& RunId)
{
	FAgentTraceRecord Record;
	Record.EventId = MakeEventId();
	Record.RunId = RunId;
	Record.EventType = EventType;
	Record.Timestamp = FDateTime::UtcNow();
	Record.Backend = Backend;
	Record.Payload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();

	if (EventType == TEXT("policy_denied"))
	{
		FScopeLock Lock(&Mutex);
		if (FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
		{
			OsvayderUECanonRouting::MarkPolicyDeny(ActiveRun->CanonExecution);
		}
	}

	StoreAndPersistRecord(Record);
}

void FOsvayderUEAgentTraceLog::LogBackendFailure(
	const FString& RunId,
	const EOsvayderUEProviderBackend Backend,
	const FString& Message,
	const FString& Stage)
{
	const FString TransportFailureFamily =
		Backend == EOsvayderUEProviderBackend::CodexCli
			? FCodexCliRunner::ClassifyPersistentTransportFailureMessage(Message)
			: FString();
	const bool bTimedOut = IsTimeoutMessage(Message);
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("message"), Message);
	Payload->SetStringField(TEXT("stage"), Stage);
	Payload->SetStringField(
		TEXT("classification_source"),
		!TransportFailureFamily.IsEmpty()
			? TEXT("persistent_transport_classifier")
			: (bTimedOut ? TEXT("message_text_match") : TEXT("explicit_failure_path")));
	if (!TransportFailureFamily.IsEmpty())
	{
		Payload->SetStringField(TEXT("failure_family"), TransportFailureFamily);
		Payload->SetBoolField(TEXT("fresh_backend_session_on_next_request"), true);
	}

	FString ProviderSessionId;
	{
		FScopeLock Lock(&Mutex);
		if (FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
		{
			ProviderSessionId = ActiveRun->ProviderSessionId;
			if (!TransportFailureFamily.IsEmpty())
			{
				ActiveRun->TransportFailureFamily = TransportFailureFamily;
				ActiveRun->TransportFailureMessage = Message;
			}
		}
	}

	if (!ProviderSessionId.IsEmpty())
	{
		Payload->SetStringField(TEXT("provider_session_id"), ProviderSessionId);
	}

	AppendEvent(
		!TransportFailureFamily.IsEmpty()
			? TEXT("transport_reset_detected")
			: (bTimedOut ? TEXT("timeout") : TEXT("backend_error")),
		Backend,
		Payload,
		RunId);
}

void FOsvayderUEAgentTraceLog::MarkCancellation(const EOsvayderUEProviderBackend Backend, const FString& Detail)
{
	FString RunId;
	FString ProviderSessionId;
	{
		FScopeLock Lock(&Mutex);
		if (const FString* ActiveRunId = ActiveRunIdsByBackend.Find(static_cast<uint8>(Backend)))
		{
			RunId = *ActiveRunId;
			if (FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
			{
				ActiveRun->bCancelled = true;
				ProviderSessionId = ActiveRun->ProviderSessionId;
			}
		}
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("detail"), Detail);
	if (!ProviderSessionId.IsEmpty())
	{
		Payload->SetStringField(TEXT("provider_session_id"), ProviderSessionId);
	}
	AppendEvent(TEXT("cancellation"), Backend, Payload, RunId);
}

void FOsvayderUEAgentTraceLog::CompleteRun(
	const FString& RunId,
	const EOsvayderUEProviderBackend Backend,
	const FString& Response,
	const bool bSuccess)
{
	FAgentTraceActiveRunState ActiveRunState;
	bool bHadActiveRun = false;
	{
		FScopeLock Lock(&Mutex);
		if (FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
		{
			ActiveRunState = *ActiveRun;
			bHadActiveRun = true;
		}

		ActiveRuns.Remove(RunId);

		if (const FString* ActiveRunId = ActiveRunIdsByBackend.Find(static_cast<uint8>(Backend)))
		{
			if (*ActiveRunId == RunId)
			{
				ActiveRunIdsByBackend.Remove(static_cast<uint8>(Backend));
			}
		}
	}

	const double DurationSeconds = bHadActiveRun
		? FMath::Max(0.0, FPlatformTime::Seconds() - ActiveRunState.StartedSeconds)
		: 0.0;
	const bool bTimedOut = IsTimeoutMessage(Response);
	const bool bTransportFailure = !ActiveRunState.TransportFailureFamily.IsEmpty();
	ActiveRunState.bTransportRetrySafe =
		bTransportFailure
		&& DetermineTransportRetryBlockReason(
			ActiveRunState.ObservedToolUseCount,
			ActiveRunState.bObservedRestartSurvivalSignal,
			ActiveRunState.bObservedVerificationTool,
			ActiveRunState.bObservedMutatingTool).IsEmpty();
	ActiveRunState.TransportRetryBlockReason =
		bTransportFailure
			? DetermineTransportRetryBlockReason(
				ActiveRunState.ObservedToolUseCount,
				ActiveRunState.bObservedRestartSurvivalSignal,
				ActiveRunState.bObservedVerificationTool,
				ActiveRunState.bObservedMutatingTool)
			: FString();
	const FString Outcome = ActiveRunState.bCancelled
		? TEXT("cancelled")
		: (bSuccess
			? TEXT("success")
			: (bTransportFailure
				? TEXT("transport_reset")
				: (bTimedOut ? TEXT("timeout") : TEXT("failed"))));
	FOsvayderUEActivePlan ActivePlan;
	FString ActivePlanError;
	if (FOsvayderUERelayAgentManager::LoadActivePlan(ActivePlan, ActivePlanError)
		&& ActivePlan.FeatureWorkflow.HasAnySignal())
	{
		ActiveRunState.CanonExecution.FeatureWorkflow = ActivePlan.FeatureWorkflow;
		CompleteFeatureWorkflowSummary(
			ActiveRunState.CanonExecution.FeatureWorkflow,
			FDateTime::UtcNow().ToIso8601(),
			bSuccess && !ActiveRunState.bCancelled && !bTimedOut);
	}
	OsvayderUECanonRouting::Finalize(ActiveRunState.CanonExecution, bSuccess, bTimedOut);
	const TSharedPtr<FJsonObject> CanonExecutionObject = OsvayderUECanonRouting::MakeCanonExecutionJson(ActiveRunState.CanonExecution);

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("outcome"), Outcome);
	Payload->SetStringField(
		TEXT("outcome_source"),
		ActiveRunState.bCancelled
			? TEXT("explicit_cancellation")
			: (bSuccess
				? TEXT("explicit_success")
				: (bTransportFailure
					? TEXT("persistent_transport_classifier")
					: (bTimedOut ? TEXT("message_text_match") : TEXT("explicit_failure_path")))));
	Payload->SetStringField(TEXT("response_text"), Response);
	Payload->SetNumberField(TEXT("response_length"), Response.Len());
	Payload->SetNumberField(TEXT("duration_ms"), DurationSeconds * 1000.0);
	if (!ActiveRunState.ProviderSessionId.IsEmpty())
	{
		Payload->SetStringField(TEXT("provider_session_id"), ActiveRunState.ProviderSessionId);
	}
	if (bTransportFailure)
	{
		Payload->SetStringField(TEXT("transport_failure_family"), ActiveRunState.TransportFailureFamily);
		Payload->SetBoolField(TEXT("transport_retry_safe"), ActiveRunState.bTransportRetrySafe);
		Payload->SetBoolField(TEXT("transport_retry_attempted"), ActiveRunState.bTransportRetryAttempted);
		Payload->SetStringField(TEXT("current_task_lane"), TEXT("live_editor"));
		Payload->SetStringField(TEXT("recovery_kind"), TEXT("same_lane_recovery"));
		Payload->SetBoolField(TEXT("detached_lane_escalation"), false);
		Payload->SetNumberField(TEXT("observed_tool_use_count"), ActiveRunState.ObservedToolUseCount);
		Payload->SetBoolField(TEXT("observed_mutating_tool"), ActiveRunState.bObservedMutatingTool);
		Payload->SetBoolField(TEXT("observed_verification_tool"), ActiveRunState.bObservedVerificationTool);
		Payload->SetBoolField(TEXT("observed_restart_survival_signal"), ActiveRunState.bObservedRestartSurvivalSignal);
		if (!ActiveRunState.TransportRetryBlockReason.IsEmpty())
		{
			Payload->SetStringField(TEXT("transport_retry_block_reason"), ActiveRunState.TransportRetryBlockReason);
		}
	}
	if (ActiveRunState.bIsTransportRetryReplay)
	{
		Payload->SetBoolField(TEXT("transport_retry_replay"), true);
		if (!ActiveRunState.TransportRetrySourceRunId.IsEmpty())
		{
			Payload->SetStringField(TEXT("transport_retry_source_run_id"), ActiveRunState.TransportRetrySourceRunId);
		}
	}
	if (CanonExecutionObject.IsValid())
	{
		Payload->SetObjectField(TEXT("canon_execution"), CanonExecutionObject);
		if (ActiveRunState.CanonExecution.FeatureWorkflow.HasAnySignal())
		{
			Payload->SetObjectField(TEXT("feature_workflow"), ActiveRunState.CanonExecution.FeatureWorkflow.ToJsonObject());
		}
		AppendEvent(TEXT("canon_execution_summary"), Backend, CanonExecutionObject, RunId);
	}
	if (bTransportFailure)
	{
		TSharedPtr<FJsonObject> RetryEligibilityPayload = MakeShared<FJsonObject>();
		RetryEligibilityPayload->SetStringField(TEXT("failure_family"), ActiveRunState.TransportFailureFamily);
		RetryEligibilityPayload->SetBoolField(TEXT("retry_safe"), ActiveRunState.bTransportRetrySafe);
		RetryEligibilityPayload->SetStringField(TEXT("current_task_lane"), TEXT("live_editor"));
		RetryEligibilityPayload->SetStringField(TEXT("recovery_kind"), TEXT("same_lane_recovery"));
		RetryEligibilityPayload->SetBoolField(TEXT("detached_lane_escalation"), false);
		RetryEligibilityPayload->SetNumberField(TEXT("observed_tool_use_count"), ActiveRunState.ObservedToolUseCount);
		RetryEligibilityPayload->SetBoolField(TEXT("observed_mutating_tool"), ActiveRunState.bObservedMutatingTool);
		RetryEligibilityPayload->SetBoolField(TEXT("observed_verification_tool"), ActiveRunState.bObservedVerificationTool);
		RetryEligibilityPayload->SetBoolField(TEXT("observed_restart_survival_signal"), ActiveRunState.bObservedRestartSurvivalSignal);
		if (!ActiveRunState.TransportRetryBlockReason.IsEmpty())
		{
			RetryEligibilityPayload->SetStringField(TEXT("retry_block_reason"), ActiveRunState.TransportRetryBlockReason);
		}
		if (!ActiveRunState.ProviderSessionId.IsEmpty())
		{
			RetryEligibilityPayload->SetStringField(TEXT("provider_session_id"), ActiveRunState.ProviderSessionId);
		}
		AppendEvent(TEXT("transport_retry_eligibility"), Backend, RetryEligibilityPayload, RunId);

		if (!ActiveRunState.bTransportRetrySafe)
		{
			TSharedPtr<FJsonObject> RetryBlockedPayload = MakeShared<FJsonObject>();
			RetryBlockedPayload->SetStringField(TEXT("failure_family"), ActiveRunState.TransportFailureFamily);
			RetryBlockedPayload->SetStringField(TEXT("block_reason"), ActiveRunState.TransportRetryBlockReason);
			RetryBlockedPayload->SetStringField(TEXT("current_task_lane"), TEXT("live_editor"));
			RetryBlockedPayload->SetStringField(TEXT("recovery_kind"), TEXT("same_lane_recovery"));
			RetryBlockedPayload->SetBoolField(TEXT("detached_lane_escalation"), false);
			AppendEvent(TEXT("transport_retry_blocked"), Backend, RetryBlockedPayload, RunId);
		}
	}
	AppendEvent(TEXT("run_completed"), Backend, Payload, RunId);

	if (bTransportFailure)
	{
		FAgentTransportFailureSummary Summary;
		Summary.RunId = RunId;
		Summary.FailureFamily = ActiveRunState.TransportFailureFamily;
		Summary.FailureMessage = ActiveRunState.TransportFailureMessage.IsEmpty()
			? Response
			: ActiveRunState.TransportFailureMessage;
		Summary.bRetrySafe = ActiveRunState.bTransportRetrySafe;
		Summary.bRetryAttempted = ActiveRunState.bTransportRetryAttempted;
		Summary.RetryBlockReason = ActiveRunState.TransportRetryBlockReason;
		Summary.ObservedToolUseCount = ActiveRunState.ObservedToolUseCount;
		Summary.bObservedMutatingTool = ActiveRunState.bObservedMutatingTool;
		Summary.bObservedVerificationTool = ActiveRunState.bObservedVerificationTool;
		Summary.bObservedRestartSurvivalSignal = ActiveRunState.bObservedRestartSurvivalSignal;

		{
			FScopeLock Lock(&Mutex);
			LatestTransportFailureByBackend.Add(static_cast<uint8>(Backend), Summary);
		}
	}

	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("assistant_backend_run");
	Receipt.bSuccess = bSuccess;
	Receipt.TargetType = TEXT("assistant_run");
	Receipt.Status = Outcome;
	Receipt.DurationMs = DurationSeconds * 1000.0;
	Receipt.Summary = bSuccess
		? TEXT("Assistant backend run completed successfully.")
		: (bTransportFailure
			? TEXT("Assistant backend run ended because the persistent transport was reset.")
			: TEXT("Assistant backend run failed."));
	Receipt.ErrorOrDenialReason = bSuccess ? FString() : Response;
	Receipt.CanonExecution = ActiveRunState.CanonExecution;
	Receipt.TransportFailureFamily = ActiveRunState.TransportFailureFamily;
	Receipt.bTransportRetrySafe = ActiveRunState.bTransportRetrySafe;
	Receipt.bTransportRetryAttempted = ActiveRunState.bTransportRetryAttempted;
	Receipt.TransportRetryBlockReason = ActiveRunState.TransportRetryBlockReason;
	Receipt.TransportRetrySourceRunId = ActiveRunState.TransportRetrySourceRunId;
	FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
}

FString FOsvayderUEAgentTraceLog::GetTraceLogPath() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (!GTestTraceLogPathOverride.IsEmpty())
	{
		FString OverridePath = FPaths::ConvertRelativePathToFull(GTestTraceLogPathOverride);
		FPaths::NormalizeFilename(OverridePath);
		return OverridePath;
	}
#endif

	return FPaths::Combine(OsvayderUEStorageMigration::GetPreferredSavedRoot(), TEXT("agent_trace.jsonl"));
}

FString FOsvayderUEAgentTraceLog::GetActiveRunIdForBackend(const EOsvayderUEProviderBackend Backend) const
{
	FScopeLock Lock(&Mutex);
	if (const FString* ActiveRunId = ActiveRunIdsByBackend.Find(static_cast<uint8>(Backend)))
	{
		return *ActiveRunId;
	}
	return FString();
}

bool FOsvayderUEAgentTraceLog::TryGetActiveCanonExecutionForBackend(
	const EOsvayderUEProviderBackend Backend,
	FAgentCanonExecution& OutExecution) const
{
	FScopeLock Lock(&Mutex);
	const FString* ActiveRunId = ActiveRunIdsByBackend.Find(static_cast<uint8>(Backend));
	if (!ActiveRunId)
	{
		return false;
	}

	const FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(*ActiveRunId);
	if (!ActiveRun)
	{
		return false;
	}

	OutExecution = ActiveRun->CanonExecution;
	return true;
}

bool FOsvayderUEAgentTraceLog::IsRunMarkedCancelled(const FString& RunId) const
{
	FScopeLock Lock(&Mutex);
	if (const FAgentTraceActiveRunState* ActiveRun = ActiveRuns.Find(RunId))
	{
		return ActiveRun->bCancelled;
	}
	return false;
}

bool FOsvayderUEAgentTraceLog::TryGetLatestTransportFailureSummary(
	const EOsvayderUEProviderBackend Backend,
	FAgentTransportFailureSummary& OutSummary) const
{
	FScopeLock Lock(&Mutex);
	if (const FAgentTransportFailureSummary* Summary =
		LatestTransportFailureByBackend.Find(static_cast<uint8>(Backend)))
	{
		OutSummary = *Summary;
		return Summary->HasAnySignal();
	}

	OutSummary = FAgentTransportFailureSummary();
	return false;
}

bool FOsvayderUEAgentTraceLog::MarkTransportRetryAttempt(
	const EOsvayderUEProviderBackend Backend,
	const FString& Trigger)
{
	FAgentTransportFailureSummary Summary;
	{
		FScopeLock Lock(&Mutex);
		FAgentTransportFailureSummary* StoredSummary =
			LatestTransportFailureByBackend.Find(static_cast<uint8>(Backend));
		if (StoredSummary == nullptr || !StoredSummary->HasAnySignal())
		{
			return false;
		}

		StoredSummary->bRetryAttempted = true;
		Summary = *StoredSummary;
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("failure_family"), Summary.FailureFamily);
	Payload->SetStringField(TEXT("trigger"), Trigger);
	Payload->SetBoolField(TEXT("retry_safe"), Summary.bRetrySafe);
	if (!Summary.RetryBlockReason.IsEmpty())
	{
		Payload->SetStringField(TEXT("retry_block_reason"), Summary.RetryBlockReason);
	}
	AppendEvent(TEXT("transport_retry_attempted"), Backend, Payload, Summary.RunId);

	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("assistant_backend_transport_retry");
	Receipt.bSuccess = true;
	Receipt.TargetType = TEXT("assistant_run");
	Receipt.Status = TEXT("retry_attempted");
	Receipt.Summary = TEXT("Transport-safe retry replay was requested for the exact last prompt.");
	Receipt.TransportFailureFamily = Summary.FailureFamily;
	Receipt.bTransportRetrySafe = Summary.bRetrySafe;
	Receipt.bTransportRetryAttempted = true;
	Receipt.TransportRetryBlockReason = Summary.RetryBlockReason;
	Receipt.TransportRetrySourceRunId = Summary.RunId;
	FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
	return true;
}

TArray<TSharedPtr<FJsonObject>> FOsvayderUEAgentTraceLog::QueryEvents(
	const FAgentTraceQueryOptions& Options,
	FString& OutResolvedRunId,
	int32& OutTotalLoaded) const
{
	TArray<TSharedPtr<FJsonObject>> LoadedRecords = LoadPersistedRecords();
	OutTotalLoaded = LoadedRecords.Num();
	OutResolvedRunId = Options.RunId.TrimStartAndEnd();

	if (OutResolvedRunId.IsEmpty() && Options.bLatestOnly)
	{
		for (int32 Index = LoadedRecords.Num() - 1; Index >= 0; --Index)
		{
			const TSharedPtr<FJsonObject>& Record = LoadedRecords[Index];
			if (Record.IsValid())
			{
				FString CandidateRunId;
				if (Record->TryGetStringField(TEXT("run_id"), CandidateRunId) && !CandidateRunId.IsEmpty())
				{
					OutResolvedRunId = CandidateRunId;
					break;
				}
			}
		}
	}

	const FString EventTypeFilter = Options.EventType.TrimStartAndEnd();
	const FString BackendFilter = Options.Backend.TrimStartAndEnd();
	const int32 ClampedCount = FMath::Clamp(Options.Count, 1, 2000);

	TArray<TSharedPtr<FJsonObject>> Filtered;
	for (int32 Index = LoadedRecords.Num() - 1; Index >= 0; --Index)
	{
		const TSharedPtr<FJsonObject>& Record = LoadedRecords[Index];
		if (!Record.IsValid())
		{
			continue;
		}

		FString RecordRunId;
		Record->TryGetStringField(TEXT("run_id"), RecordRunId);
		if (!OutResolvedRunId.IsEmpty() && RecordRunId != OutResolvedRunId)
		{
			continue;
		}

		FString RecordEventType;
		Record->TryGetStringField(TEXT("event_type"), RecordEventType);
		if (!EventTypeFilter.IsEmpty() && !RecordEventType.Equals(EventTypeFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		FString RecordBackend;
		Record->TryGetStringField(TEXT("backend"), RecordBackend);
		if (!BackendFilter.IsEmpty() && !RecordBackend.Contains(BackendFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		Filtered.Add(Record);
		if (Filtered.Num() >= ClampedCount)
		{
			break;
		}
	}

	Algo::Reverse(Filtered);

	TArray<TSharedPtr<FJsonObject>> Sanitized;
	Sanitized.Reserve(Filtered.Num());
	for (const TSharedPtr<FJsonObject>& Record : Filtered)
	{
		Sanitized.Add(SanitizeObjectForPreview(Record, FMath::Clamp(Options.PreviewChars, 80, 12000), Options.bIncludeRawJson));
	}

	return Sanitized;
}

TSharedPtr<FJsonObject> FOsvayderUEAgentTraceLog::BuildRunMetadataObject(
	const FAgentBackendStatus& Status,
	const FAgentRequestConfig& Config,
	const bool bIncludeEngineContext,
	const bool bIncludeProjectContext) const
{
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("backend_display_name"), Status.DisplayName);
	Payload->SetStringField(TEXT("model"), Settings ? Settings->GetConfiguredModelForBackend(Status.Backend) : FString());
	Payload->SetStringField(TEXT("profile"), Settings ? Settings->GetConfiguredProfileLabelForBackend(Status.Backend) : FString());
	Payload->SetStringField(TEXT("auth_mode"), Settings ? Settings->GetConfiguredAuthModeLabelForBackend(Status.Backend) : FString());
	Payload->SetStringField(TEXT("auth_effective_path"),
		Status.Backend == EOsvayderUEProviderBackend::CodexCli ? FCodexCliRunner::GetEffectiveAuthEntryPath() : FString());
	Payload->SetStringField(TEXT("auth_ownership_model"),
		Status.Backend == EOsvayderUEProviderBackend::CodexCli ? FCodexCliRunner::GetEffectiveAuthOwnershipModel() : FString());
	Payload->SetStringField(TEXT("speed_mode_requested"),
		Settings ? Settings->GetConfiguredSpeedModeLabelForBackend(Status.Backend) : FString());
	Payload->SetStringField(TEXT("speed_mode_effective"),
		Status.Backend == EOsvayderUEProviderBackend::CodexCli ? FCodexCliRunner::GetEffectiveSpeedModeName() : FString());
	Payload->SetStringField(TEXT("speed_mode_support"),
		Status.Backend == EOsvayderUEProviderBackend::CodexCli ? FCodexCliRunner::GetSpeedModeSupportLabel() : FString());
	Payload->SetStringField(TEXT("work_mode"),
		(Status.Backend == EOsvayderUEProviderBackend::CodexCli && Settings)
			? Settings->GetConfiguredCodexWorkModeName()
			: FString());
	Payload->SetStringField(TEXT("reasoning_effort"),
		(Status.Backend == EOsvayderUEProviderBackend::CodexCli && Settings)
			? Settings->GetConfiguredCodexReasoningEffortName()
			: FString());
	Payload->SetStringField(TEXT("verbosity"),
		(Status.Backend == EOsvayderUEProviderBackend::CodexCli && Settings)
			? Settings->GetConfiguredCodexVerbosityName()
			: FString());
	Payload->SetStringField(TEXT("execution_transport"),
		!Config.ExecutionTransportLabel.IsEmpty()
			? Config.ExecutionTransportLabel
			: (Status.Backend == EOsvayderUEProviderBackend::CodexCli
				? (FCodexCliRunner::ShouldUsePersistentConversationTransport() ? TEXT("persistent_app_server") : TEXT("exec_per_message"))
				: TEXT("cli_process")));
	Payload->SetStringField(TEXT("execution_control_profile_id"), Config.ExecutionControlProfileId);
	Payload->SetStringField(TEXT("session_persistence_mode"), AgentSessionPersistenceModeToString(Config.SessionPersistenceMode));
	Payload->SetStringField(TEXT("current_effective_provider_power"), AgentExecutionPowerClassToString(Config.CurrentEffectiveProviderPowerClass));
	Payload->SetStringField(TEXT("desired_future_default_provider_power"), AgentExecutionPowerClassToString(Config.DesiredFutureDefaultProviderPowerClass));
	Payload->SetStringField(TEXT("execution_control_plumbing_state"), AgentExecutionGovernanceStateToString(Config.ExecutionControlPlumbingState));
	const FAgentProviderExecutionControlManifest Manifest = BuildAgentProviderExecutionControlManifest(Status, Config);
	Payload->SetBoolField(TEXT("normal_provider_session_history_touched"), Manifest.bTouchesNormalProviderSessionHistory);
	Payload->SetBoolField(TEXT("provider_session_file_updated_on_success"), Manifest.bWritesProviderSessionFileOnSuccess);
	Payload->SetStringField(TEXT("working_directory"), Config.WorkingDirectory);
	Payload->SetBoolField(TEXT("include_engine_context"), bIncludeEngineContext);
	Payload->SetBoolField(TEXT("include_project_context"), bIncludeProjectContext);
	Payload->SetNumberField(TEXT("bootstrap_context_length"), Config.ConversationBootstrapText.Len());
	Payload->SetNumberField(TEXT("system_prompt_length"), Config.SystemPrompt.Len());
	Payload->SetNumberField(TEXT("prompt_length"), Config.Prompt.Len());
	Payload->SetBoolField(TEXT("uses_structured_output"), Config.bUseJsonOutput);
	Payload->SetNumberField(TEXT("attached_image_count"), Config.AttachedImagePaths.Num());
	Payload->SetStringField(TEXT("readiness"), AgentBackendReadinessToString(Status.Readiness));
	Payload->SetStringField(TEXT("auth_state"), AgentBackendAuthStateToString(Status.AuthState));
	Payload->SetBoolField(TEXT("available"), Status.bAvailable);
	Payload->SetBoolField(TEXT("ready"), Status.bReady);
	Payload->SetStringField(TEXT("detail"), Status.Detail);
	Payload->SetStringField(TEXT("auth_detail"), Status.AuthDetail);
	const TSharedPtr<FJsonObject> CanonObject = OsvayderUECanonRouting::MakeCanonExecutionJson(Config.CanonExecution);
	if (CanonObject.IsValid())
	{
		Payload->SetObjectField(TEXT("canon_execution"), CanonObject);
	}
	if (Config.CanonExecution.FeatureWorkflow.HasAnySignal())
	{
		Payload->SetObjectField(TEXT("feature_workflow"), Config.CanonExecution.FeatureWorkflow.ToJsonObject());
	}
	if (Config.bIsTransportRetryReplay)
	{
		TSharedPtr<FJsonObject> RetryObject = MakeShared<FJsonObject>();
		RetryObject->SetBoolField(TEXT("exact_last_prompt_replay"), true);
		RetryObject->SetStringField(TEXT("source_run_id"), Config.TransportRetrySourceRunId);
		RetryObject->SetStringField(TEXT("failure_family"), Config.TransportRetryFailureFamily);
		Payload->SetObjectField(TEXT("transport_retry_replay"), RetryObject);
	}

	TArray<TSharedPtr<FJsonValue>> AllowedToolsArray;
	for (const FString& ToolName : Config.AllowedTools)
	{
		AllowedToolsArray.Add(MakeShared<FJsonValueString>(ToolName));
	}
	Payload->SetArrayField(TEXT("allowed_tools"), AllowedToolsArray);
	Payload->SetObjectField(TEXT("provider_execution_control"), MakeAgentProviderExecutionControlJson(Manifest));

	return Payload;
}

TSharedPtr<FJsonObject> FOsvayderUEAgentTraceLog::MakeTraceRecordObject(const FAgentTraceRecord& Record) const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("event_id"), Record.EventId);
	Object->SetStringField(TEXT("run_id"), Record.RunId);
	Object->SetStringField(TEXT("event_type"), Record.EventType);
	Object->SetStringField(TEXT("timestamp"), Record.Timestamp.ToIso8601());
	Object->SetStringField(TEXT("backend"), BackendFilterLabel(Record.Backend));
	Object->SetObjectField(TEXT("payload"), Record.Payload.IsValid() ? Record.Payload : MakeShared<FJsonObject>());
	return Object;
}

void FOsvayderUEAgentTraceLog::StoreAndPersistRecord(const FAgentTraceRecord& Record)
{
	{
		FScopeLock Lock(&Mutex);
		RecentRecords.Add(Record);
		if (RecentRecords.Num() > MaxInMemoryTraceRecords)
		{
			RecentRecords.RemoveAt(0, RecentRecords.Num() - MaxInMemoryTraceRecords);
		}
	}

	PersistRecord(Record);
}

void FOsvayderUEAgentTraceLog::PersistRecord(const FAgentTraceRecord& Record) const
{
	const FString TraceLogFile = GetTraceLogPath();
	bool bHydrated = false;
	FString HydrationError;
	if (!OsvayderUEStorageMigration::EnsurePreferredFileHydrated(
		TraceLogFile,
		GetLegacyTraceLogPath(),
		TEXT("agent_trace_log"),
		ValidateTraceLogFile,
		bHydrated,
		HydrationError))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Trace log hydration failed: %s"), *HydrationError);
	}

	const FString TraceLogDir = FPaths::GetPath(TraceLogFile);
	IFileManager::Get().MakeDirectory(*TraceLogDir, true);

	const FString JsonLine = SerializeJsonObjectCompact(MakeTraceRecordObject(Record)) + TEXT("\n");
	FFileHelper::SaveStringToFile(
		JsonLine,
		*TraceLogFile,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(),
		EFileWrite::FILEWRITE_Append);

	const int64 FileSize = IFileManager::Get().FileSize(*TraceLogFile);
	if (FileSize > MaxTraceFileBytes)
	{
		const FString RotatedFile = TraceLogFile + TEXT(".old");
		IFileManager::Get().Delete(*RotatedFile);
		IFileManager::Get().Move(*RotatedFile, *TraceLogFile);
	}
}

TArray<TSharedPtr<FJsonObject>> FOsvayderUEAgentTraceLog::LoadPersistedRecords() const
{
	TArray<TSharedPtr<FJsonObject>> Records;
	const FString TraceLogFile = GetTraceLogPath();
	FString ResolveError;
	OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
	if (!OsvayderUEStorageMigration::ResolveManagedReadPath(
		TraceLogFile,
		GetLegacyTraceLogPath(),
		TEXT("agent_trace_log"),
		ValidateTraceLogFile,
		ManagedRead,
		ResolveError))
	{
		return Records;
	}

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *ManagedRead.ResolvedPath))
	{
		return Records;
	}

	Records.Reserve(Lines.Num());
	for (const FString& Line : Lines)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
		{
			Records.Add(JsonObject);
		}
	}

	return Records;
}

#if WITH_DEV_AUTOMATION_TESTS
void FOsvayderUEAgentTraceLog::SetTestTraceLogPathOverride(const FString& InPath)
{
	GTestTraceLogPathOverride = InPath;
}

void FOsvayderUEAgentTraceLog::ClearTestTraceLogPathOverride()
{
	GTestTraceLogPathOverride.Empty();
}
#endif

FString FOsvayderUEAgentTraceLog::MakeRunId() const
{
	return FString::Printf(TEXT("run_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString FOsvayderUEAgentTraceLog::MakeEventId() const
{
	return FString::Printf(TEXT("evt_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

bool FOsvayderUEAgentTraceLog::IsTimeoutMessage(const FString& Message)
{
	return Message.Contains(TEXT("timed out"), ESearchCase::IgnoreCase) ||
		Message.Contains(TEXT("timeout"), ESearchCase::IgnoreCase);
}
