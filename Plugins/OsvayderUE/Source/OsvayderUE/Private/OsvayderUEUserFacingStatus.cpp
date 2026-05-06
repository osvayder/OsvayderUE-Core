// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUEUserFacingStatus.h"

#include "OsvayderEditorWidget.h"
#include "Dom/JsonObject.h"
#include "OsvayderUERelayAgent.h"

namespace
{
	const FString& EmptyString()
	{
		static const FString Value;
		return Value;
	}

	bool ContainsAnyToken(const FString& Value, const TArray<FString>& Tokens)
	{
		for (const FString& Token : Tokens)
		{
			if (Value.Contains(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool HasDecisionSignal(const FOsvayderUEActivePlanCloseoutDecision& Decision)
	{
		return !Decision.PlanStatus.IsEmpty()
			|| !Decision.ResultStatus.IsEmpty()
			|| !Decision.GateReasonCode.IsEmpty()
			|| !Decision.BlockerFamily.IsEmpty()
			|| !Decision.BlockerDetail.IsEmpty()
			|| Decision.bRuntimeProofPassed
			|| Decision.bRuntimeProofFailed
			|| Decision.bStopLossTriggered
			|| Decision.bProofPrerequisitesMissing;
	}

	bool IsFullCloseoutPassed(const FOsvayderUEUserFacingStatus& Status)
	{
		return Status.RawPlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive)
			&& Status.RawResultStatus.Equals(TEXT("achieved_fully"), ESearchCase::CaseSensitive)
			&& Status.GateReasonCode.IsEmpty();
	}

	bool IsManualBoundary(const FOsvayderUEActivePlanCloseoutDecision& Decision)
	{
		if (Decision.GateReasonCode.Equals(TEXT("managed_state_manual_write_detected"), ESearchCase::CaseSensitive))
		{
			return false;
		}

		const FString Combined = FString::Printf(
			TEXT("%s %s %s"),
			*Decision.GateReasonCode,
			*Decision.BlockerFamily,
			*Decision.BlockerDetail);
		return ContainsAnyToken(
			Combined,
			{
				TEXT("manual_verification_required"),
				TEXT("manual_required"),
				TEXT("manual_ui_boundary"),
				TEXT("manual_boundary"),
				TEXT("manual_asset_dependency_blocker"),
				TEXT("blocked_on_manual"),
				TEXT("ui_boundary")
			});
	}

	bool IsExactCauseBlocker(const FOsvayderUEActivePlanCloseoutDecision& Decision)
	{
		if (Decision.bStopLossTriggered || Decision.bProofPrerequisitesMissing)
		{
			return true;
		}

		const FString Combined = FString::Printf(
			TEXT("%s %s %s %s"),
			*Decision.GateReasonCode,
			*Decision.BlockerFamily,
			*Decision.BlockerDetail,
			*Decision.StopLossReason);
		return ContainsAnyToken(
			Combined,
			{
				TEXT("stop_loss"),
				TEXT("blocked_on_tool_surface"),
				TEXT("mechanic_input_conflict_unresolved"),
				TEXT("tool_surface_unavailable"),
				TEXT("missing tool"),
				TEXT("proof_prerequisite"),
				TEXT("proof_prerequisites")
			});
	}

	bool IsCloseoutFailure(const FOsvayderUEUserFacingStatus& Status)
	{
		return Status.RawPlanStatus.Equals(TEXT("failed"), ESearchCase::CaseSensitive)
			|| Status.RawResultStatus.Equals(TEXT("not_achieved"), ESearchCase::CaseSensitive)
			|| !Status.GateReasonCode.IsEmpty();
	}

	bool HasRuntimeProofPassedSignal(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision* Decision)
	{
		return Plan.FeatureWorkflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase)
			|| Plan.FeatureWorkflow.CompletedPhaseIds.Contains(TEXT("runtime_proof"))
			|| (Decision && Decision->bRuntimeProofPassed);
	}

	void AddIfPresent(TArray<FString>& Parts, const FString& Label, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Parts.Add(FString::Printf(TEXT("%s=%s"), *Label, *Value));
		}
	}

	void AddIfPositive(TArray<FString>& Parts, const FString& Label, const int32 Value)
	{
		if (Value > 0)
		{
			Parts.Add(FString::Printf(TEXT("%s=%d"), *Label, Value));
		}
	}

	FString BuildIdentifierDetail(const FOsvayderUEUserFacingStatus& Status)
	{
		TArray<FString> Parts;
		AddIfPresent(Parts, TEXT("run_id"), Status.RunId);
		AddIfPresent(Parts, TEXT("plan_id"), Status.PlanId);
		AddIfPresent(Parts, TEXT("feature_workflow_id"), Status.FeatureWorkflowId);
		AddIfPresent(Parts, TEXT("recipe_id"), Status.RecipeId);
		AddIfPresent(Parts, TEXT("role_id"), Status.RoleId);
		AddIfPositive(Parts, TEXT("evidence_schema_version"), Status.EvidenceSchemaVersion);
		return FString::Join(Parts, TEXT("; "));
	}

	FString BuildBlockerDetail(const FOsvayderUEUserFacingStatus& Status, const FString& StopLossReason)
	{
		TArray<FString> Parts;
		AddIfPresent(Parts, TEXT("gate_reason_code"), Status.GateReasonCode);
		AddIfPresent(Parts, TEXT("blocker_family"), Status.BlockerFamily);
		AddIfPresent(Parts, TEXT("blocker_detail"), Status.BlockerDetail);
		AddIfPresent(Parts, TEXT("stop_loss_reason"), StopLossReason);
		return FString::Join(Parts, TEXT("; "));
	}

	void SetOutcome(
		FOsvayderUEUserFacingStatus& Status,
		const FString& StatusId,
		const FString& Headline,
		const FString& Detail)
	{
		Status.StatusId = StatusId;
		Status.Headline = Headline;
		Status.Detail = Detail;
	}

	FOsvayderUEUserFacingStatus BuildBaseStatus(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision* Decision,
		const FOsvayderUERunCloseoutContext* Context)
	{
		FOsvayderUEUserFacingStatus Status;
		Status.PlanId = Decision && !Decision->SourcePlanId.IsEmpty()
			? Decision->SourcePlanId
			: Plan.PlanId;
		Status.RunId = Decision && !Decision->SourceRunId.IsEmpty()
			? Decision->SourceRunId
			: (Context ? Context->RunId : EmptyString());
		Status.FeatureWorkflowId = Decision && !Decision->SourceFeatureWorkflowId.IsEmpty()
			? Decision->SourceFeatureWorkflowId
			: (Context && !Context->FeatureWorkflowId.IsEmpty()
				? Context->FeatureWorkflowId
				: Plan.FeatureWorkflow.FeatureWorkflowId);
		Status.RecipeId = Decision && !Decision->SourceRecipeId.IsEmpty()
			? Decision->SourceRecipeId
			: (Context && !Context->RecipeId.IsEmpty()
				? Context->RecipeId
				: Plan.FeatureWorkflow.RecipeId);
		Status.RoleId = Decision && !Decision->SourceRoleId.IsEmpty()
			? Decision->SourceRoleId
			: (Context && !Context->RoleId.IsEmpty()
				? Context->RoleId
				: Plan.FeatureWorkflow.RoleId);
		Status.EvidenceSchemaVersion = Decision && Decision->EvidenceSchemaVersion > 0
			? Decision->EvidenceSchemaVersion
			: (Context ? Context->EvidenceSchemaVersion : Plan.FeatureWorkflow.EvidenceSchemaVersion);
		Status.RawCurrentPhase = Plan.FeatureWorkflow.CurrentPhase;
		Status.RawPlanStatus = Decision && !Decision->PlanStatus.IsEmpty()
			? Decision->PlanStatus
			: Plan.Status;
		Status.RawResultStatus = Decision && !Decision->ResultStatus.IsEmpty()
			? Decision->ResultStatus
			: Plan.ResultStatus;
		if (Decision)
		{
			Status.GateReasonCode = Decision->GateReasonCode;
			Status.BlockerFamily = Decision->BlockerFamily.IsEmpty()
				? Plan.FeatureWorkflow.BlockerFamily
				: Decision->BlockerFamily;
			Status.BlockerDetail = Decision->BlockerDetail.IsEmpty()
				? Plan.FeatureWorkflow.BlockerDetail
				: Decision->BlockerDetail;
		}
		else
		{
			Status.BlockerFamily = Plan.FeatureWorkflow.BlockerFamily;
			Status.BlockerDetail = Plan.FeatureWorkflow.BlockerDetail;
		}

		return Status;
	}

	FOsvayderUEUserFacingStatus BuildStatusInternal(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision* Decision,
		const FOsvayderUERunCloseoutContext* Context)
	{
		FOsvayderUEUserFacingStatus Status = BuildBaseStatus(Plan, Decision, Context);
		const FString IdentifierDetail = BuildIdentifierDetail(Status);
		const FString IdentifierSuffix = IdentifierDetail.IsEmpty()
			? FString()
			: FString::Printf(TEXT(" Identifiers: %s."), *IdentifierDetail);

		if ((!Decision || HasDecisionSignal(*Decision)) && IsFullCloseoutPassed(Status))
		{
			SetOutcome(
				Status,
				OsvayderUEUserFacingStatus::CloseoutPassedStatusId(),
				TEXT("Closeout passed"),
				TEXT("Final closeout reached achieved_fully.") + IdentifierSuffix);
			return Status;
		}

		if (Decision && HasDecisionSignal(*Decision) && IsManualBoundary(*Decision))
		{
			const FString Cause = BuildBlockerDetail(Status, Decision->StopLossReason);
			SetOutcome(
				Status,
				OsvayderUEUserFacingStatus::ManualVerificationRequiredStatusId(),
				TEXT("Manual verification required"),
				Cause.IsEmpty()
					? TEXT("A manual UI/editor boundary must be verified before closeout can continue.") + IdentifierSuffix
					: FString::Printf(TEXT("Manual boundary: %s.%s"), *Cause, *IdentifierSuffix));
			return Status;
		}

		if (Decision && HasDecisionSignal(*Decision) && IsExactCauseBlocker(*Decision))
		{
			const FString Cause = BuildBlockerDetail(Status, Decision->StopLossReason);
			SetOutcome(
				Status,
				OsvayderUEUserFacingStatus::BlockedByExactCauseStatusId(),
				TEXT("Blocked by exact cause"),
				Cause.IsEmpty()
					? TEXT("Closeout is blocked by a named workflow cause.") + IdentifierSuffix
					: FString::Printf(TEXT("Exact blocker: %s.%s"), *Cause, *IdentifierSuffix));
			return Status;
		}

		if (Decision && HasDecisionSignal(*Decision) && IsCloseoutFailure(Status))
		{
			const FString Cause = BuildBlockerDetail(Status, Decision->StopLossReason);
			SetOutcome(
				Status,
				OsvayderUEUserFacingStatus::CloseoutFailedStatusId(),
				TEXT("Closeout failed"),
				Cause.IsEmpty()
					? TEXT("Closeout did not reach achieved_fully.") + IdentifierSuffix
					: FString::Printf(TEXT("Closeout failure: %s.%s"), *Cause, *IdentifierSuffix));
			return Status;
		}

		if (HasRuntimeProofPassedSignal(Plan, Decision))
		{
			SetOutcome(
				Status,
				OsvayderUEUserFacingStatus::RuntimeProofPassedStatusId(),
				TEXT("Runtime proof passed"),
				TEXT("Runtime proof is green, but final closeout has not reached achieved_fully yet.") + IdentifierSuffix);
			return Status;
		}

		if (Status.RawPlanStatus.Equals(TEXT("failed"), ESearchCase::CaseSensitive))
		{
			SetOutcome(
				Status,
				OsvayderUEUserFacingStatus::CloseoutFailedStatusId(),
				TEXT("Closeout failed"),
				TEXT("The active plan is terminal failed without a full closeout decision.") + IdentifierSuffix);
			return Status;
		}

		SetOutcome(
			Status,
			OsvayderUEUserFacingStatus::WorkRunningStatusId(),
			TEXT("Work is running"),
			Status.RawCurrentPhase.IsEmpty()
				? TEXT("The active plan is still in progress.") + IdentifierSuffix
				: FString::Printf(TEXT("The active plan is still in progress. Debug phase: %s.%s"), *Status.RawCurrentPhase, *IdentifierSuffix));
		return Status;
	}
}

TSharedPtr<FJsonObject> FOsvayderUEUserFacingStatus::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("status_id"), StatusId);
	Object->SetStringField(TEXT("headline"), Headline);
	Object->SetStringField(TEXT("detail"), Detail);
	Object->SetStringField(TEXT("blocker_family"), BlockerFamily);
	Object->SetStringField(TEXT("blocker_detail"), BlockerDetail);
	Object->SetStringField(TEXT("gate_reason_code"), GateReasonCode);
	Object->SetStringField(TEXT("run_id"), RunId);
	Object->SetStringField(TEXT("plan_id"), PlanId);
	Object->SetStringField(TEXT("feature_workflow_id"), FeatureWorkflowId);
	Object->SetStringField(TEXT("recipe_id"), RecipeId);
	Object->SetStringField(TEXT("role_id"), RoleId);
	Object->SetNumberField(TEXT("evidence_schema_version"), EvidenceSchemaVersion);
	Object->SetStringField(TEXT("raw_current_phase"), RawCurrentPhase);
	Object->SetStringField(TEXT("raw_plan_status"), RawPlanStatus);
	Object->SetStringField(TEXT("raw_result_status"), RawResultStatus);
	return Object;
}

namespace OsvayderUEUserFacingStatus
{
	const FString& WorkRunningStatusId()
	{
		static const FString Value(TEXT("work_running"));
		return Value;
	}

	const FString& RuntimeProofPassedStatusId()
	{
		static const FString Value(TEXT("runtime_proof_passed"));
		return Value;
	}

	const FString& CloseoutPassedStatusId()
	{
		static const FString Value(TEXT("closeout_passed"));
		return Value;
	}

	const FString& CloseoutFailedStatusId()
	{
		static const FString Value(TEXT("closeout_failed"));
		return Value;
	}

	const FString& ManualVerificationRequiredStatusId()
	{
		static const FString Value(TEXT("manual_verification_required"));
		return Value;
	}

	const FString& BlockedByExactCauseStatusId()
	{
		static const FString Value(TEXT("blocked_by_exact_cause"));
		return Value;
	}

	bool IsRawInternalPhaseLabel(const FString& Text)
	{
		return Text.Equals(TEXT("runtime_proof"), ESearchCase::IgnoreCase)
			|| Text.Equals(TEXT("memory_update"), ESearchCase::IgnoreCase)
			|| Text.Equals(TEXT("stop_loss"), ESearchCase::IgnoreCase)
			|| Text.Equals(TEXT("command_execution_without_phase_advance_gt_5"), ESearchCase::IgnoreCase)
			|| Text.Equals(TEXT("compile_gate"), ESearchCase::IgnoreCase)
			|| Text.Equals(TEXT("automation_discovery_gate"), ESearchCase::IgnoreCase)
			|| Text.Equals(TEXT("proof_context_setup"), ESearchCase::IgnoreCase)
			|| Text.Equals(TEXT("input_asset_authoring"), ESearchCase::IgnoreCase);
	}

	FOsvayderUEUserFacingStatus BuildStatus(const FOsvayderUEActivePlan& Plan)
	{
		return BuildStatusInternal(Plan, nullptr, nullptr);
	}

	FOsvayderUEUserFacingStatus BuildStatus(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision& Decision)
	{
		return BuildStatusInternal(Plan, &Decision, nullptr);
	}

	FOsvayderUEUserFacingStatus BuildStatus(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision& Decision,
		const FOsvayderUERunCloseoutContext& Context)
	{
		return BuildStatusInternal(Plan, &Decision, &Context);
	}
}
