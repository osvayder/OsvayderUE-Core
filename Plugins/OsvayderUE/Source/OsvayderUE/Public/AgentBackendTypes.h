// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "AgentBackendTypes.generated.h"

UENUM()
enum class EOsvayderUEProviderBackend : uint8
{
	ClaudeCli UMETA(DisplayName="Claude CLI (Legacy)"),
	CodexCli UMETA(DisplayName="Codex CLI"),
};

inline const TCHAR* OsvayderUEProviderBackendToString(const EOsvayderUEProviderBackend Backend)
{
	switch (Backend)
	{
	case EOsvayderUEProviderBackend::CodexCli:
		return TEXT("CodexCli");

	case EOsvayderUEProviderBackend::ClaudeCli:
	default:
		return TEXT("ClaudeCli");
	}
}

inline const TCHAR* OsvayderUEProviderBackendToSessionSlug(const EOsvayderUEProviderBackend Backend)
{
	switch (Backend)
	{
	case EOsvayderUEProviderBackend::CodexCli:
		return TEXT("codex_cli");

	case EOsvayderUEProviderBackend::ClaudeCli:
	default:
		return TEXT("claude_cli");
	}
}

DECLARE_DELEGATE_TwoParams(FOnAgentResponse, const FString& /*Response*/, bool /*bSuccess*/);
DECLARE_DELEGATE_OneParam(FOnAgentProgress, const FString& /*PartialOutput*/);

enum class EAgentRunEventType : uint8
{
	SessionInit,
	TextContent,
	ToolUse,
	ToolResult,
	Result,
	AssistantMessage,
	Unknown
};

struct OSVAYDERUE_API FAgentRunEvent
{
	EAgentRunEventType Type = EAgentRunEventType::Unknown;
	EOsvayderUEProviderBackend Backend = EOsvayderUEProviderBackend::CodexCli;
	FString Text;
	FString ToolName;
	FString ToolInput;
	FString ToolCallId;
	FString ToolResultContent;
	FString SessionId;
	FString ItemId;
	FString ResultText;
	FString RawJson;
	bool bIsError = false;
	int32 ExitCode = INDEX_NONE;
	int32 DurationMs = 0;
	int32 NumTurns = 0;
	float TotalCostUsd = 0.0f;
};

DECLARE_DELEGATE_OneParam(FOnAgentStreamEvent, const FAgentRunEvent& /*Event*/);

struct OSVAYDERUE_API FAgentPromptContextBlock
{
	FString Label;
	FString Content;
};

struct OSVAYDERUE_API FAgentPromptContract
{
	FString RoleId;
	FString RecipeId;
	int32 EvidenceSchemaVersion = 0;
	FString AgentIdentity;
	FString EnvironmentRules;
	FString ToolRules;
	FString MutationPolicy;
	FString CompletionPolicy;
	TArray<FAgentPromptContextBlock> ContextBlocks;

	bool HasAnyContent() const
	{
		if (!RoleId.IsEmpty() ||
			!RecipeId.IsEmpty() ||
			EvidenceSchemaVersion > 0 ||
			!AgentIdentity.IsEmpty() ||
			!EnvironmentRules.IsEmpty() ||
			!ToolRules.IsEmpty() ||
			!MutationPolicy.IsEmpty() ||
			!CompletionPolicy.IsEmpty())
		{
			return true;
		}

		for (const FAgentPromptContextBlock& Block : ContextBlocks)
		{
			if (!Block.Content.IsEmpty())
			{
				return true;
			}
		}

		return false;
	}
};

struct OSVAYDERUE_API FAgentFeatureWorkflowPhaseState
{
	FString PhaseId;
	FString Label;
	FString Status = TEXT("pending");
	int32 AttemptCount = 0;
	int32 FailureCount = 0;
	FString StartedAtUtc;
	FString CompletedAtUtc;
	FString LastToolCallId;
	FString LastToolName;
	FString LastFailureReason;

	bool HasAnySignal() const
	{
		return !PhaseId.IsEmpty()
			|| !Label.IsEmpty()
			|| !Status.Equals(TEXT("pending"), ESearchCase::CaseSensitive)
			|| AttemptCount > 0
			|| FailureCount > 0
			|| !StartedAtUtc.IsEmpty()
			|| !CompletedAtUtc.IsEmpty()
			|| !LastToolCallId.IsEmpty()
			|| !LastToolName.IsEmpty()
			|| !LastFailureReason.IsEmpty();
	}

	TSharedPtr<FJsonObject> ToJsonObject() const
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		if (!PhaseId.IsEmpty())
		{
			Object->SetStringField(TEXT("phase_id"), PhaseId);
		}
		if (!Label.IsEmpty())
		{
			Object->SetStringField(TEXT("label"), Label);
		}
		Object->SetStringField(TEXT("status"), Status);
		Object->SetNumberField(TEXT("attempt_count"), AttemptCount);
		Object->SetNumberField(TEXT("failure_count"), FailureCount);
		if (!StartedAtUtc.IsEmpty())
		{
			Object->SetStringField(TEXT("started_at_utc"), StartedAtUtc);
		}
		if (!CompletedAtUtc.IsEmpty())
		{
			Object->SetStringField(TEXT("completed_at_utc"), CompletedAtUtc);
		}
		if (!LastToolCallId.IsEmpty())
		{
			Object->SetStringField(TEXT("last_tool_call_id"), LastToolCallId);
		}
		if (!LastToolName.IsEmpty())
		{
			Object->SetStringField(TEXT("last_tool_name"), LastToolName);
		}
		if (!LastFailureReason.IsEmpty())
		{
			Object->SetStringField(TEXT("last_failure_reason"), LastFailureReason);
		}
		return Object;
	}

	static FAgentFeatureWorkflowPhaseState FromJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		FAgentFeatureWorkflowPhaseState State;
		if (!Object.IsValid())
		{
			return State;
		}

		Object->TryGetStringField(TEXT("phase_id"), State.PhaseId);
		Object->TryGetStringField(TEXT("label"), State.Label);
		Object->TryGetStringField(TEXT("status"), State.Status);
		double NumberValue = 0.0;
		if (Object->TryGetNumberField(TEXT("attempt_count"), NumberValue))
		{
			State.AttemptCount = static_cast<int32>(NumberValue);
		}
		if (Object->TryGetNumberField(TEXT("failure_count"), NumberValue))
		{
			State.FailureCount = static_cast<int32>(NumberValue);
		}
		Object->TryGetStringField(TEXT("started_at_utc"), State.StartedAtUtc);
		Object->TryGetStringField(TEXT("completed_at_utc"), State.CompletedAtUtc);
		Object->TryGetStringField(TEXT("last_tool_call_id"), State.LastToolCallId);
		Object->TryGetStringField(TEXT("last_tool_name"), State.LastToolName);
		Object->TryGetStringField(TEXT("last_failure_reason"), State.LastFailureReason);
		if (State.Status.IsEmpty())
		{
			State.Status = TEXT("pending");
		}
		return State;
	}
};

struct OSVAYDERUE_API FOsvayderUETaskLaneState
{
	FString CurrentLane;
	FString TargetLane;
	FString ExpectedReturnLane;
	FString TransitionKind;
	FString TransitionState;
	FString TransitionReason;
	FString BlockerFamily;
	FString ContinuityTaskId;
	FString ContinuityPlanId;
	FString ContinuityWorkflowId;
	FString ContinuityPhaseId;
	FString ContinuationIntent;
	FString ExpectedReturnCondition;

	bool HasAnySignal() const
	{
		return !CurrentLane.IsEmpty()
			|| !TargetLane.IsEmpty()
			|| !ExpectedReturnLane.IsEmpty()
			|| !TransitionKind.IsEmpty()
			|| !TransitionState.IsEmpty()
			|| !TransitionReason.IsEmpty()
			|| !BlockerFamily.IsEmpty()
			|| !ContinuityTaskId.IsEmpty()
			|| !ContinuityPlanId.IsEmpty()
			|| !ContinuityWorkflowId.IsEmpty()
			|| !ContinuityPhaseId.IsEmpty()
			|| !ContinuationIntent.IsEmpty()
			|| !ExpectedReturnCondition.IsEmpty();
	}

	FString GetEffectiveCurrentLane() const
	{
		return CurrentLane.IsEmpty()
			? FString(TEXT("live_editor"))
			: CurrentLane;
	}

	TSharedPtr<FJsonObject> ToJsonObject() const
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		if (!CurrentLane.IsEmpty())
		{
			Object->SetStringField(TEXT("current_lane"), CurrentLane);
		}
		if (!TargetLane.IsEmpty())
		{
			Object->SetStringField(TEXT("target_lane"), TargetLane);
		}
		if (!ExpectedReturnLane.IsEmpty())
		{
			Object->SetStringField(TEXT("expected_return_lane"), ExpectedReturnLane);
		}
		if (!TransitionKind.IsEmpty())
		{
			Object->SetStringField(TEXT("transition_kind"), TransitionKind);
		}
		if (!TransitionState.IsEmpty())
		{
			Object->SetStringField(TEXT("transition_state"), TransitionState);
		}
		if (!TransitionReason.IsEmpty())
		{
			Object->SetStringField(TEXT("transition_reason"), TransitionReason);
		}
		if (!BlockerFamily.IsEmpty())
		{
			Object->SetStringField(TEXT("blocker_family"), BlockerFamily);
		}
		if (!ContinuityTaskId.IsEmpty())
		{
			Object->SetStringField(TEXT("continuity_task_id"), ContinuityTaskId);
		}
		if (!ContinuityPlanId.IsEmpty())
		{
			Object->SetStringField(TEXT("continuity_plan_id"), ContinuityPlanId);
		}
		if (!ContinuityWorkflowId.IsEmpty())
		{
			Object->SetStringField(TEXT("continuity_workflow_id"), ContinuityWorkflowId);
		}
		if (!ContinuityPhaseId.IsEmpty())
		{
			Object->SetStringField(TEXT("continuity_phase_id"), ContinuityPhaseId);
		}
		if (!ContinuationIntent.IsEmpty())
		{
			Object->SetStringField(TEXT("continuation_intent"), ContinuationIntent);
		}
		if (!ExpectedReturnCondition.IsEmpty())
		{
			Object->SetStringField(TEXT("expected_return_condition"), ExpectedReturnCondition);
		}
		return Object;
	}

	static FOsvayderUETaskLaneState FromJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		FOsvayderUETaskLaneState State;
		if (!Object.IsValid())
		{
			return State;
		}

		Object->TryGetStringField(TEXT("current_lane"), State.CurrentLane);
		Object->TryGetStringField(TEXT("target_lane"), State.TargetLane);
		Object->TryGetStringField(TEXT("expected_return_lane"), State.ExpectedReturnLane);
		Object->TryGetStringField(TEXT("transition_kind"), State.TransitionKind);
		Object->TryGetStringField(TEXT("transition_state"), State.TransitionState);
		Object->TryGetStringField(TEXT("transition_reason"), State.TransitionReason);
		Object->TryGetStringField(TEXT("blocker_family"), State.BlockerFamily);
		Object->TryGetStringField(TEXT("continuity_task_id"), State.ContinuityTaskId);
		Object->TryGetStringField(TEXT("continuity_plan_id"), State.ContinuityPlanId);
		Object->TryGetStringField(TEXT("continuity_workflow_id"), State.ContinuityWorkflowId);
		Object->TryGetStringField(TEXT("continuity_phase_id"), State.ContinuityPhaseId);
		Object->TryGetStringField(TEXT("continuation_intent"), State.ContinuationIntent);
		Object->TryGetStringField(TEXT("expected_return_condition"), State.ExpectedReturnCondition);
		return State;
	}
};

struct OSVAYDERUE_API FInteractionAccessReuseObservationState
{
	bool bPersistentInputAssetObserved = false;
	bool bInteractionActionAssetObserved = false;
	bool bReadOnlyEnhancedInputQueryObserved = false;

	bool HasAnySignal() const
	{
		return bPersistentInputAssetObserved
			|| bInteractionActionAssetObserved
			|| bReadOnlyEnhancedInputQueryObserved;
	}

	bool HasSufficientReuseEvidence() const
	{
		return bPersistentInputAssetObserved
			&& bInteractionActionAssetObserved
			&& bReadOnlyEnhancedInputQueryObserved;
	}

	TSharedPtr<FJsonObject> ToJsonObject() const
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("persistent_input_asset_observed"), bPersistentInputAssetObserved);
		Object->SetBoolField(TEXT("interaction_action_asset_observed"), bInteractionActionAssetObserved);
		Object->SetBoolField(TEXT("read_only_enhanced_input_query_observed"), bReadOnlyEnhancedInputQueryObserved);
		return Object;
	}

	static FInteractionAccessReuseObservationState FromJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		FInteractionAccessReuseObservationState State;
		if (!Object.IsValid())
		{
			return State;
		}

		bool BoolValue = false;
		if (Object->TryGetBoolField(TEXT("persistent_input_asset_observed"), BoolValue))
		{
			State.bPersistentInputAssetObserved = BoolValue;
		}
		if (Object->TryGetBoolField(TEXT("interaction_action_asset_observed"), BoolValue))
		{
			State.bInteractionActionAssetObserved = BoolValue;
		}
		if (Object->TryGetBoolField(TEXT("read_only_enhanced_input_query_observed"), BoolValue))
		{
			State.bReadOnlyEnhancedInputQueryObserved = BoolValue;
		}
		return State;
	}
};

struct OSVAYDERUE_API FInteractionAccessAttemptResolverObservationState
{
	bool bAttemptResolverSourceObserved = false;
	bool bEventSubsystemSourceObserved = false;
	bool bRuntimeSmokeSuccessObserved = false;
	bool bPrisonAccessEventObserved = false;

	bool HasAnySignal() const
	{
		return bAttemptResolverSourceObserved
			|| bEventSubsystemSourceObserved
			|| bRuntimeSmokeSuccessObserved
			|| bPrisonAccessEventObserved;
	}

	bool HasSufficientSourceEvidence() const
	{
		return bAttemptResolverSourceObserved
			&& bEventSubsystemSourceObserved;
	}

	bool HasSufficientRuntimeEvidence() const
	{
		return bRuntimeSmokeSuccessObserved
			&& bPrisonAccessEventObserved;
	}

	bool HasSufficientEvidence() const
	{
		return HasSufficientSourceEvidence()
			|| HasSufficientRuntimeEvidence();
	}

	bool HasCompleteEvidence() const
	{
		return HasSufficientSourceEvidence()
			&& HasSufficientRuntimeEvidence();
	}

	TSharedPtr<FJsonObject> ToJsonObject() const
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("attempt_resolver_source_observed"), bAttemptResolverSourceObserved);
		Object->SetBoolField(TEXT("event_subsystem_source_observed"), bEventSubsystemSourceObserved);
		Object->SetBoolField(TEXT("runtime_smoke_success_observed"), bRuntimeSmokeSuccessObserved);
		Object->SetBoolField(TEXT("prison_access_event_observed"), bPrisonAccessEventObserved);
		return Object;
	}

	static FInteractionAccessAttemptResolverObservationState FromJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		FInteractionAccessAttemptResolverObservationState State;
		if (!Object.IsValid())
		{
			return State;
		}

		bool BoolValue = false;
		if (Object->TryGetBoolField(TEXT("attempt_resolver_source_observed"), BoolValue))
		{
			State.bAttemptResolverSourceObserved = BoolValue;
		}
		if (Object->TryGetBoolField(TEXT("event_subsystem_source_observed"), BoolValue))
		{
			State.bEventSubsystemSourceObserved = BoolValue;
		}
		if (Object->TryGetBoolField(TEXT("runtime_smoke_success_observed"), BoolValue))
		{
			State.bRuntimeSmokeSuccessObserved = BoolValue;
		}
		if (Object->TryGetBoolField(TEXT("prison_access_event_observed"), BoolValue))
		{
			State.bPrisonAccessEventObserved = BoolValue;
		}
		return State;
	}
};

struct OSVAYDERUE_API FAgentFeatureWorkflowState
{
	FString FeatureWorkflowId;
	FString RecipeId;
	FString RoleId;
	int32 EvidenceSchemaVersion = 0;
	FString CurrentPhase;
	FString TerminalStatus;
	bool bCompileProofRequired = false;
	FString CompileProofState;
	bool bRuntimeProofRequired = false;
	FString RuntimeProofState;
	bool bStopLossTriggered = false;
	FString StopLossReason;
	FString BlockerFamily;
	FString BlockerDetail;
	FOsvayderUETaskLaneState LaneState;
	FString AuthoringLaneState;
	FString AuthoringPolicyRuleId;
	FString AuthoringDecision;
	FString ProofPrerequisiteState;
	FInteractionAccessReuseObservationState InteractionAccessReuseObservation;
	FInteractionAccessAttemptResolverObservationState InteractionAccessAttemptResolverObservation;
	bool bKnownProofMapAvailable = false;
	bool bPlacedRuntimeActorsAvailable = false;
	bool bReducedProofModeAllowed = false;
	FString AutomationDiscoveryCommand;
	int32 AutomationDiscoveryCount = INDEX_NONE;
	int32 AutomationExecutedCount = INDEX_NONE;
	int32 AutomationPassedCount = INDEX_NONE;
	int32 AutomationFailedCount = INDEX_NONE;
	int32 AdHocProofAttemptCount = 0;
	int32 CommandExecutionCallsWithoutPhaseAdvance = 0;
	TMap<FString, FString> ToolUseStartPhaseByCallId;
	TMap<FString, FString> ToolUseInputByCallId;
	TMap<FString, FString> ToolUseRawJsonByCallId;
	TArray<FString> CompletedPhaseIds;
	TArray<FString> FailedPhaseIds;
	TArray<FAgentFeatureWorkflowPhaseState> Phases;

	bool HasAnySignal() const
	{
		return !FeatureWorkflowId.IsEmpty()
			|| !RecipeId.IsEmpty()
			|| !RoleId.IsEmpty()
			|| EvidenceSchemaVersion > 0
			|| !CurrentPhase.IsEmpty()
			|| !TerminalStatus.IsEmpty()
			|| bCompileProofRequired
			|| !CompileProofState.IsEmpty()
			|| bRuntimeProofRequired
			|| !RuntimeProofState.IsEmpty()
			|| bStopLossTriggered
			|| !StopLossReason.IsEmpty()
			|| !BlockerFamily.IsEmpty()
			|| !BlockerDetail.IsEmpty()
			|| LaneState.HasAnySignal()
			|| !AuthoringLaneState.IsEmpty()
			|| !AuthoringPolicyRuleId.IsEmpty()
			|| !AuthoringDecision.IsEmpty()
			|| !ProofPrerequisiteState.IsEmpty()
			|| InteractionAccessReuseObservation.HasAnySignal()
			|| InteractionAccessAttemptResolverObservation.HasAnySignal()
			|| bKnownProofMapAvailable
			|| bPlacedRuntimeActorsAvailable
			|| bReducedProofModeAllowed
			|| !AutomationDiscoveryCommand.IsEmpty()
			|| AutomationDiscoveryCount != INDEX_NONE
			|| AutomationExecutedCount != INDEX_NONE
			|| AutomationPassedCount != INDEX_NONE
			|| AutomationFailedCount != INDEX_NONE
			|| AdHocProofAttemptCount > 0
			|| CommandExecutionCallsWithoutPhaseAdvance > 0
			|| CompletedPhaseIds.Num() > 0
			|| FailedPhaseIds.Num() > 0
			|| Phases.Num() > 0;
	}

	bool HasRuntimeProofPrerequisites() const
	{
		return bKnownProofMapAvailable
			|| bPlacedRuntimeActorsAvailable
			|| bReducedProofModeAllowed
			|| AutomationDiscoveryCount > 0;
	}

	int32 FindPhaseIndex(const FString& PhaseId) const
	{
		for (int32 Index = 0; Index < Phases.Num(); ++Index)
		{
			if (Phases[Index].PhaseId.Equals(PhaseId, ESearchCase::CaseSensitive))
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	FAgentFeatureWorkflowPhaseState* FindMutablePhase(const FString& PhaseId)
	{
		const int32 Index = FindPhaseIndex(PhaseId);
		return Index == INDEX_NONE ? nullptr : &Phases[Index];
	}

	const FAgentFeatureWorkflowPhaseState* FindPhase(const FString& PhaseId) const
	{
		const int32 Index = FindPhaseIndex(PhaseId);
		return Index == INDEX_NONE ? nullptr : &Phases[Index];
	}

	TSharedPtr<FJsonObject> ToJsonObject() const
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		if (!FeatureWorkflowId.IsEmpty())
		{
			Object->SetStringField(TEXT("feature_workflow_id"), FeatureWorkflowId);
		}
		if (!RecipeId.IsEmpty())
		{
			Object->SetStringField(TEXT("recipe_id"), RecipeId);
		}
		if (!RoleId.IsEmpty())
		{
			Object->SetStringField(TEXT("role_id"), RoleId);
		}
		if (EvidenceSchemaVersion > 0)
		{
			Object->SetNumberField(TEXT("evidence_schema_version"), EvidenceSchemaVersion);
		}
		if (!CurrentPhase.IsEmpty())
		{
			Object->SetStringField(TEXT("current_phase"), CurrentPhase);
		}
		if (!TerminalStatus.IsEmpty())
		{
			Object->SetStringField(TEXT("terminal_status"), TerminalStatus);
		}
		Object->SetBoolField(TEXT("compile_proof_required"), bCompileProofRequired);
		if (!CompileProofState.IsEmpty())
		{
			Object->SetStringField(TEXT("compile_proof_state"), CompileProofState);
		}
		Object->SetBoolField(TEXT("runtime_proof_required"), bRuntimeProofRequired);
		if (!RuntimeProofState.IsEmpty())
		{
			Object->SetStringField(TEXT("runtime_proof_state"), RuntimeProofState);
		}
		Object->SetBoolField(TEXT("stop_loss_triggered"), bStopLossTriggered);
		if (!StopLossReason.IsEmpty())
		{
			Object->SetStringField(TEXT("stop_loss_reason"), StopLossReason);
		}
		if (!BlockerFamily.IsEmpty())
		{
			Object->SetStringField(TEXT("blocker_family"), BlockerFamily);
		}
		if (!BlockerDetail.IsEmpty())
		{
			Object->SetStringField(TEXT("blocker_detail"), BlockerDetail);
		}
		if (LaneState.HasAnySignal())
		{
			Object->SetObjectField(TEXT("lane_state"), LaneState.ToJsonObject());
		}
		if (!AuthoringLaneState.IsEmpty())
		{
			Object->SetStringField(TEXT("authoring_lane_state"), AuthoringLaneState);
		}
		if (!AuthoringPolicyRuleId.IsEmpty())
		{
			Object->SetStringField(TEXT("authoring_policy_rule_id"), AuthoringPolicyRuleId);
		}
		if (!AuthoringDecision.IsEmpty())
		{
			Object->SetStringField(TEXT("authoring_decision"), AuthoringDecision);
		}
		if (!ProofPrerequisiteState.IsEmpty())
		{
			Object->SetStringField(TEXT("proof_prerequisite_state"), ProofPrerequisiteState);
		}
		if (InteractionAccessReuseObservation.HasAnySignal())
		{
			Object->SetObjectField(
				TEXT("interaction_access_reuse_observation"),
				InteractionAccessReuseObservation.ToJsonObject());
		}
		if (InteractionAccessAttemptResolverObservation.HasAnySignal())
		{
			Object->SetObjectField(
				TEXT("interaction_access_attempt_resolver_observation"),
				InteractionAccessAttemptResolverObservation.ToJsonObject());
		}
		Object->SetBoolField(TEXT("known_proof_map_available"), bKnownProofMapAvailable);
		Object->SetBoolField(TEXT("placed_runtime_actors_available"), bPlacedRuntimeActorsAvailable);
		Object->SetBoolField(TEXT("reduced_proof_mode_allowed"), bReducedProofModeAllowed);
		if (!AutomationDiscoveryCommand.IsEmpty())
		{
			Object->SetStringField(TEXT("automation_discovery_command"), AutomationDiscoveryCommand);
		}
		if (AutomationDiscoveryCount != INDEX_NONE)
		{
			Object->SetNumberField(TEXT("automation_discovery_count"), AutomationDiscoveryCount);
		}
		if (AutomationExecutedCount != INDEX_NONE)
		{
			Object->SetNumberField(TEXT("automation_executed_count"), AutomationExecutedCount);
		}
		if (AutomationPassedCount != INDEX_NONE)
		{
			Object->SetNumberField(TEXT("automation_passed_count"), AutomationPassedCount);
		}
		if (AutomationFailedCount != INDEX_NONE)
		{
			Object->SetNumberField(TEXT("automation_failed_count"), AutomationFailedCount);
		}
		Object->SetNumberField(TEXT("ad_hoc_proof_attempt_count"), AdHocProofAttemptCount);
		Object->SetNumberField(TEXT("command_execution_calls_without_phase_advance"), CommandExecutionCallsWithoutPhaseAdvance);
		if (CompletedPhaseIds.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Value : CompletedPhaseIds)
			{
				Values.Add(MakeShared<FJsonValueString>(Value));
			}
			Object->SetArrayField(TEXT("completed_phase_ids"), Values);
		}
		if (FailedPhaseIds.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Value : FailedPhaseIds)
			{
				Values.Add(MakeShared<FJsonValueString>(Value));
			}
			Object->SetArrayField(TEXT("failed_phase_ids"), Values);
		}
		if (Phases.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FAgentFeatureWorkflowPhaseState& Phase : Phases)
			{
				Values.Add(MakeShared<FJsonValueObject>(Phase.ToJsonObject()));
			}
			Object->SetArrayField(TEXT("phases"), Values);
		}
		return Object;
	}

	static FAgentFeatureWorkflowState FromJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		FAgentFeatureWorkflowState State;
		if (!Object.IsValid())
		{
			return State;
		}

		Object->TryGetStringField(TEXT("feature_workflow_id"), State.FeatureWorkflowId);
		Object->TryGetStringField(TEXT("recipe_id"), State.RecipeId);
		Object->TryGetStringField(TEXT("role_id"), State.RoleId);
		double NumberValue = 0.0;
		if (Object->TryGetNumberField(TEXT("evidence_schema_version"), NumberValue))
		{
			State.EvidenceSchemaVersion = static_cast<int32>(NumberValue);
		}
		Object->TryGetStringField(TEXT("current_phase"), State.CurrentPhase);
		Object->TryGetStringField(TEXT("terminal_status"), State.TerminalStatus);
		bool BoolValue = false;
		if (Object->TryGetBoolField(TEXT("compile_proof_required"), BoolValue))
		{
			State.bCompileProofRequired = BoolValue;
		}
		Object->TryGetStringField(TEXT("compile_proof_state"), State.CompileProofState);
		if (Object->TryGetBoolField(TEXT("runtime_proof_required"), BoolValue))
		{
			State.bRuntimeProofRequired = BoolValue;
		}
		Object->TryGetStringField(TEXT("runtime_proof_state"), State.RuntimeProofState);
		if (Object->TryGetBoolField(TEXT("stop_loss_triggered"), BoolValue))
		{
			State.bStopLossTriggered = BoolValue;
		}
		Object->TryGetStringField(TEXT("stop_loss_reason"), State.StopLossReason);
		Object->TryGetStringField(TEXT("blocker_family"), State.BlockerFamily);
		Object->TryGetStringField(TEXT("blocker_detail"), State.BlockerDetail);
		const TSharedPtr<FJsonObject>* LaneStateObject = nullptr;
		if (Object->TryGetObjectField(TEXT("lane_state"), LaneStateObject) && LaneStateObject && LaneStateObject->IsValid())
		{
			State.LaneState = FOsvayderUETaskLaneState::FromJsonObject(*LaneStateObject);
		}
		Object->TryGetStringField(TEXT("authoring_lane_state"), State.AuthoringLaneState);
		Object->TryGetStringField(TEXT("authoring_policy_rule_id"), State.AuthoringPolicyRuleId);
		Object->TryGetStringField(TEXT("authoring_decision"), State.AuthoringDecision);
		Object->TryGetStringField(TEXT("proof_prerequisite_state"), State.ProofPrerequisiteState);
		const TSharedPtr<FJsonObject>* ReuseObservationObject = nullptr;
		if (Object->TryGetObjectField(TEXT("interaction_access_reuse_observation"), ReuseObservationObject)
			&& ReuseObservationObject
			&& ReuseObservationObject->IsValid())
		{
			State.InteractionAccessReuseObservation =
				FInteractionAccessReuseObservationState::FromJsonObject(*ReuseObservationObject);
		}
		const TSharedPtr<FJsonObject>* AttemptResolverObservationObject = nullptr;
		if (Object->TryGetObjectField(
				TEXT("interaction_access_attempt_resolver_observation"),
				AttemptResolverObservationObject)
			&& AttemptResolverObservationObject
			&& AttemptResolverObservationObject->IsValid())
		{
			State.InteractionAccessAttemptResolverObservation =
				FInteractionAccessAttemptResolverObservationState::FromJsonObject(*AttemptResolverObservationObject);
		}
		if (Object->TryGetBoolField(TEXT("known_proof_map_available"), BoolValue))
		{
			State.bKnownProofMapAvailable = BoolValue;
		}
		if (Object->TryGetBoolField(TEXT("placed_runtime_actors_available"), BoolValue))
		{
			State.bPlacedRuntimeActorsAvailable = BoolValue;
		}
		if (Object->TryGetBoolField(TEXT("reduced_proof_mode_allowed"), BoolValue))
		{
			State.bReducedProofModeAllowed = BoolValue;
		}
		Object->TryGetStringField(TEXT("automation_discovery_command"), State.AutomationDiscoveryCommand);
		if (Object->TryGetNumberField(TEXT("automation_discovery_count"), NumberValue))
		{
			State.AutomationDiscoveryCount = static_cast<int32>(NumberValue);
		}
		if (Object->TryGetNumberField(TEXT("automation_executed_count"), NumberValue))
		{
			State.AutomationExecutedCount = static_cast<int32>(NumberValue);
		}
		if (Object->TryGetNumberField(TEXT("automation_passed_count"), NumberValue))
		{
			State.AutomationPassedCount = static_cast<int32>(NumberValue);
		}
		if (Object->TryGetNumberField(TEXT("automation_failed_count"), NumberValue))
		{
			State.AutomationFailedCount = static_cast<int32>(NumberValue);
		}
		if (Object->TryGetNumberField(TEXT("ad_hoc_proof_attempt_count"), NumberValue))
		{
			State.AdHocProofAttemptCount = static_cast<int32>(NumberValue);
		}
		if (Object->TryGetNumberField(TEXT("command_execution_calls_without_phase_advance"), NumberValue))
		{
			State.CommandExecutionCallsWithoutPhaseAdvance = static_cast<int32>(NumberValue);
		}

		const TArray<TSharedPtr<FJsonValue>>* CompletedValues = nullptr;
		if (Object->TryGetArrayField(TEXT("completed_phase_ids"), CompletedValues) && CompletedValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *CompletedValues)
			{
				if (Value.IsValid())
				{
					State.CompletedPhaseIds.Add(Value->AsString());
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* FailedValues = nullptr;
		if (Object->TryGetArrayField(TEXT("failed_phase_ids"), FailedValues) && FailedValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *FailedValues)
			{
				if (Value.IsValid())
				{
					State.FailedPhaseIds.Add(Value->AsString());
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* PhaseValues = nullptr;
		if (Object->TryGetArrayField(TEXT("phases"), PhaseValues) && PhaseValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *PhaseValues)
			{
				if (Value.IsValid())
				{
					State.Phases.Add(FAgentFeatureWorkflowPhaseState::FromJsonObject(Value->AsObject()));
				}
			}
		}

		return State;
	}
};

struct OSVAYDERUE_API FAgentCanonExecution
{
	FString DetectedSubsystem;
	FString TaskMode;
	bool bCanonDiscoveryUsed = false;
	bool bBriefWasProduced = false;
	bool bBriefPartBRequired = false;
	bool bBriefPartBProduced = false;
	bool bBriefPartBProducedBeforeFirstMutatingTool = false;
	bool bApprovedPatternFound = false;
	bool bToolExposureAdjusted = false;
	FString RequestedToolFamily;
	TArray<FString> EnabledToolFamilyIds;
	FString ActualToolFamily;
	bool bFallbackToolUsed = false;
	FString PrimaryMutationToolFamily;
	TArray<FString> AuxiliaryToolFamilies;
	bool bMutatingFallbackUsed = false;
	bool bPolicyDenyOccurred = false;
	FString VerificationOutcome;
	TArray<FString> ImplementationBriefLines;
	TArray<FString> ImplementationBriefPartBLines;
	TArray<FString> MandatoryAnimationWorkflowSteps;
	TArray<FString> ConditionalMandatoryAnimationWorkflowSteps;
	TArray<FString> CloseoutGateReasonCodes;
	FString ApprovedPatternKey;
	FString LedgerPath;

	/**
	 * 631 Agent self-retrospective: did this run exercise an empirical
	 * verification tool (PIE smoke, automation test, compile+link, runtime
	 * probe) before the agent reported a completion status? Set by the
	 * plugin-side validator based on agent_trace evidence, NOT by the agent's
	 * own claim. Mismatch (agent reports full but no verification-tool
	 * invocation observed) surfaces as a widget warning — see
	 * `OsvayderEditorWidget::SelfVerificationMismatchWarning`.
	 */
	bool bSelfVerificationAttempted = false;

	/**
	 * 631 Agent self-retrospective: outcome of the observed self-verification
	 * tool. Canonical values: "pass" / "fail" / "skipped" (empty when
	 * bSelfVerificationAttempted=false). Empty string means "no verification
	 * attempted per plugin-side trace inspection".
	 */
	FString SelfVerificationResult;
	FAgentFeatureWorkflowState FeatureWorkflow;

	bool HasAnySignal() const
	{
		return !DetectedSubsystem.IsEmpty()
			|| !TaskMode.IsEmpty()
			|| bCanonDiscoveryUsed
			|| bBriefWasProduced
			|| bBriefPartBRequired
			|| bBriefPartBProduced
			|| bBriefPartBProducedBeforeFirstMutatingTool
			|| bApprovedPatternFound
			|| bToolExposureAdjusted
			|| !RequestedToolFamily.IsEmpty()
			|| EnabledToolFamilyIds.Num() > 0
			|| !ActualToolFamily.IsEmpty()
			|| bFallbackToolUsed
			|| !PrimaryMutationToolFamily.IsEmpty()
			|| AuxiliaryToolFamilies.Num() > 0
			|| bMutatingFallbackUsed
			|| bPolicyDenyOccurred
			|| !VerificationOutcome.IsEmpty()
			|| ImplementationBriefLines.Num() > 0
			|| ImplementationBriefPartBLines.Num() > 0
			|| MandatoryAnimationWorkflowSteps.Num() > 0
			|| ConditionalMandatoryAnimationWorkflowSteps.Num() > 0
			|| CloseoutGateReasonCodes.Num() > 0
			|| !ApprovedPatternKey.IsEmpty()
			|| !LedgerPath.IsEmpty()
			|| bSelfVerificationAttempted
			|| !SelfVerificationResult.IsEmpty()
			|| FeatureWorkflow.HasAnySignal();
	}
};

enum class EAgentExecutionPowerClass : uint8
{
	ReadOnlyAnalysis,
	WorkspaceWriteProject,
	BoundedMutationCapable,
	HighRiskDirectFileShell
};

inline const TCHAR* AgentExecutionPowerClassToString(const EAgentExecutionPowerClass PowerClass)
{
	switch (PowerClass)
	{
	case EAgentExecutionPowerClass::WorkspaceWriteProject:
		return TEXT("workspace_write_project");

	case EAgentExecutionPowerClass::BoundedMutationCapable:
		return TEXT("bounded_mutation_capable");

	case EAgentExecutionPowerClass::HighRiskDirectFileShell:
		return TEXT("high_risk_direct_file_shell");

	case EAgentExecutionPowerClass::ReadOnlyAnalysis:
	default:
		return TEXT("read_only_analysis");
	}
}

enum class EAgentExecutionRunProfile : uint8
{
	ConfiguredDefaultRuntime,
	ExplicitExpertOptIn,
	BoundedPluginMutation,
	ReadOnlyDiagnostic
};

inline const TCHAR* AgentExecutionRunProfileToString(const EAgentExecutionRunProfile Profile)
{
	switch (Profile)
	{
	case EAgentExecutionRunProfile::ExplicitExpertOptIn:
		return TEXT("explicit_expert_opt_in");

	case EAgentExecutionRunProfile::BoundedPluginMutation:
		return TEXT("bounded_plugin_mutation");

	case EAgentExecutionRunProfile::ReadOnlyDiagnostic:
		return TEXT("read_only_diagnostic");

	case EAgentExecutionRunProfile::ConfiguredDefaultRuntime:
	default:
		return TEXT("configured_default_runtime");
	}
}

enum class EAgentExecutionGovernanceState : uint8
{
	DescribedOnly,
	PartialPlumbing,
	Enforced
};

inline const TCHAR* AgentExecutionGovernanceStateToString(const EAgentExecutionGovernanceState State)
{
	switch (State)
	{
	case EAgentExecutionGovernanceState::PartialPlumbing:
		return TEXT("partial_plumbing");

	case EAgentExecutionGovernanceState::Enforced:
		return TEXT("enforced");

	case EAgentExecutionGovernanceState::DescribedOnly:
	default:
		return TEXT("described_only");
	}
}

enum class EAgentSessionPersistenceMode : uint8
{
	NormalProviderSession,
	HelperIsolatedStore,
	NotPersisted
};

inline const TCHAR* AgentSessionPersistenceModeToString(const EAgentSessionPersistenceMode Mode)
{
	switch (Mode)
	{
	case EAgentSessionPersistenceMode::HelperIsolatedStore:
		return TEXT("helper_isolated_store");

	case EAgentSessionPersistenceMode::NotPersisted:
		return TEXT("not_persisted");

	case EAgentSessionPersistenceMode::NormalProviderSession:
	default:
		return TEXT("normal_provider_session");
	}
}

struct OSVAYDERUE_API FAgentRequestConfig
{
	FString Prompt;
	FString ConversationBootstrapText;
	FString SystemPrompt;
	FAgentPromptContract PromptContract;
	FString WorkingDirectory;
	EAgentExecutionRunProfile ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	FString ExecutionControlProfileId;
	FString ExecutionTransportLabel;
	FString CodexSpeedMode;
	FString CodexWorkMode;
	FString CodexReasoningEffort;
	FString CodexVerbosity;
	EAgentExecutionPowerClass CurrentEffectiveProviderPowerClass = EAgentExecutionPowerClass::HighRiskDirectFileShell;
	EAgentExecutionPowerClass DesiredFutureDefaultProviderPowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
	EAgentExecutionGovernanceState ExecutionControlPlumbingState = EAgentExecutionGovernanceState::PartialPlumbing;
	EAgentSessionPersistenceMode SessionPersistenceMode = EAgentSessionPersistenceMode::NormalProviderSession;
	bool bUseJsonOutput = false;
	bool bSkipPermissions = true;
	bool bExecutionProfileEnforcedNow = false;
	bool bToolAllowListEnforced = false;
	bool bEnableUnrealMcpBridge = true;
	bool bForceDisablePersistentConversationTransport = false;
	bool bUseRestartSurvivalProviderThreadState = false;
	bool bIsTransportRetryReplay = false;
	float TimeoutSeconds = 300.0f;
	FString TransportRetrySourceRunId;
	FString TransportRetryFailureFamily;
	TArray<FString> AllowedTools;
	TArray<FString> AttachedImagePaths;
	FAgentCanonExecution CanonExecution;
	FOnAgentStreamEvent OnStreamEvent;
};

enum class EAgentBackendAuthState : uint8
{
	Unknown,
	NotAuthenticated,
	Authenticated
};

enum class EAgentBackendReadiness : uint8
{
	NotAvailable,
	AvailableAuthUnknown,
	AvailableNotAuthenticated,
	Ready
};

inline const TCHAR* AgentBackendAuthStateToString(EAgentBackendAuthState State)
{
	switch (State)
	{
	case EAgentBackendAuthState::NotAuthenticated:
		return TEXT("not_authenticated");
	case EAgentBackendAuthState::Authenticated:
		return TEXT("authenticated");
	case EAgentBackendAuthState::Unknown:
	default:
		return TEXT("unknown");
	}
}

inline const TCHAR* AgentBackendReadinessToString(EAgentBackendReadiness State)
{
	switch (State)
	{
	case EAgentBackendReadiness::AvailableAuthUnknown:
		return TEXT("available_auth_unknown");
	case EAgentBackendReadiness::AvailableNotAuthenticated:
		return TEXT("available_not_authenticated");
	case EAgentBackendReadiness::Ready:
		return TEXT("ready");
	case EAgentBackendReadiness::NotAvailable:
	default:
		return TEXT("not_available");
	}
}

struct OSVAYDERUE_API FAgentBackendCapabilities
{
	EOsvayderUEProviderBackend Backend = EOsvayderUEProviderBackend::CodexCli;
	FString DisplayName;
	bool bSupportsStreamingEvents = false;
	bool bSupportsImages = false;
	bool bSupportsCancellation = true;
	bool bSupportsToolAllowList = false;
	bool bUsesStructuredOutput = false;
	bool bSupportsBrowserVerifyLogin = false;
	bool bSupportsProviderPersistentThreads = false;
	bool bSupportsReasoningEffortControl = false;
	bool bSupportsVerbosityControl = false;
	bool bSupportsSpeedModeControl = false;
	bool bSupportsProfileSelection = false;
	bool bSupportsExplicitAuthModeSelection = false;
};

struct OSVAYDERUE_API FAgentBackendStatus
{
	EOsvayderUEProviderBackend Backend = EOsvayderUEProviderBackend::CodexCli;
	FString DisplayName;
	FString ExecutablePath;
	FString Detail;
	bool bAvailable = false;
	EAgentBackendAuthState AuthState = EAgentBackendAuthState::Unknown;
	EAgentBackendReadiness Readiness = EAgentBackendReadiness::NotAvailable;
	FString AuthDetail;
	bool bReady = false;
	FAgentBackendCapabilities Capabilities;
};

inline bool AgentBackendCanExecutePrompt(const FAgentBackendStatus& Status)
{
	return Status.bAvailable && Status.Readiness != EAgentBackendReadiness::AvailableNotAuthenticated;
}

enum class EAgentSessionRestoreOutcome : uint8
{
	Loaded,
	NoSession,
	LegacySharedSessionBlocked,
	Failed
};

inline const TCHAR* AgentSessionRestoreOutcomeToString(const EAgentSessionRestoreOutcome Outcome)
{
	switch (Outcome)
	{
	case EAgentSessionRestoreOutcome::Loaded:
		return TEXT("loaded");
	case EAgentSessionRestoreOutcome::LegacySharedSessionBlocked:
		return TEXT("legacy_shared_session_blocked");
	case EAgentSessionRestoreOutcome::Failed:
		return TEXT("failed");
	case EAgentSessionRestoreOutcome::NoSession:
	default:
		return TEXT("no_session");
	}
}

struct OSVAYDERUE_API FAgentSessionMetadata
{
	bool bHasSession = false;
	bool bIsReadable = false;
	bool bIsLegacySharedFile = false;
	EOsvayderUEProviderBackend Backend = EOsvayderUEProviderBackend::CodexCli;
	FString BackendDisplayName;
	FString StoreKind;
	FString Model;
	FString Profile;
	FString AuthMode;
	FString SessionFilePath;
	FString LastUpdated;
	int32 MessageCount = 0;
	FString Detail;
};

struct OSVAYDERUE_API FAgentSavedSessionIndex
{
	FAgentSessionMetadata CurrentProviderSession;
	FAgentSessionMetadata OtherProviderSession;
	FAgentSessionMetadata LegacySharedSession;

	bool HasCurrentProviderSession() const
	{
		return CurrentProviderSession.bHasSession;
	}

	bool HasAnySavedSession() const
	{
		return CurrentProviderSession.bHasSession ||
			OtherProviderSession.bHasSession ||
			LegacySharedSession.bHasSession;
	}
};

struct OSVAYDERUE_API FAgentSessionRestoreResult
{
	EAgentSessionRestoreOutcome Outcome = EAgentSessionRestoreOutcome::NoSession;
	FAgentSessionMetadata RequestedSession;
	FAgentSavedSessionIndex SavedSessions;
	FString FailureReason;

	bool WasLoaded() const
	{
		return Outcome == EAgentSessionRestoreOutcome::Loaded;
	}
};
