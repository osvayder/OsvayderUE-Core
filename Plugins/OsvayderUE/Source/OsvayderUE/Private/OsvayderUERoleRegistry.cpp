// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUERoleRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> MakeStringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		Array.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}

	bool ContainsStringCaseSensitive(const TArray<FString>& Values, const FString& Needle)
	{
		return Values.ContainsByPredicate([&Needle](const FString& Value)
		{
			return Value.Equals(Needle, ESearchCase::CaseSensitive);
		});
	}

	FString JoinListOrNone(const TArray<FString>& Values)
	{
		return Values.Num() > 0 ? FString::Join(Values, TEXT(", ")) : FString(TEXT("none"));
	}

	FOsvayderUERoleActionDecision MakeUnknownRoleActionDecision(
		const FString& RoleId,
		const FString& ActionId)
	{
		FOsvayderUERoleActionDecision Decision;
		Decision.RoleId = RoleId;
		Decision.ActionId = ActionId;
		Decision.GateReasonCode = TEXT("feature_workflow_unknown_role_contract");
		Decision.BlockerFamily = TEXT("role_contract_unknown");
		Decision.BlockerDetail = FString::Printf(
			TEXT("No registered role contract for role_id=%s"),
			*RoleId);
		return Decision;
	}

	FOsvayderUERoleTransitionDecision MakeUnknownRoleTransitionDecision(
		const FString& SourceRoleId,
		const FString& TargetRoleId,
		const FString& MissingRoleId)
	{
		FOsvayderUERoleTransitionDecision Decision;
		Decision.SourceRoleId = SourceRoleId;
		Decision.TargetRoleId = TargetRoleId;
		Decision.GateReasonCode = TEXT("role_contract_unknown_transition_role");
		Decision.BlockerFamily = TEXT("role_contract_unknown");
		Decision.BlockerDetail = FString::Printf(
			TEXT("No registered role contract for role_id=%s"),
			*MissingRoleId);
		return Decision;
	}
}

bool FOsvayderUERoleContract::AllowsNextRole(const FString& NextRoleId) const
{
	return ContainsStringCaseSensitive(AllowedNextRoles, NextRoleId);
}

bool FOsvayderUERoleContract::AllowsOutputOrEvidence(const FString& OutputId) const
{
	return ContainsStringCaseSensitive(AllowedOutputs, OutputId)
		|| ContainsStringCaseSensitive(RequiredOutputEvidence, OutputId);
}

bool FOsvayderUERoleContract::ProhibitsAction(const FString& ActionId) const
{
	return ContainsStringCaseSensitive(ProhibitedActions, ActionId);
}

TSharedPtr<FJsonObject> FOsvayderUERoleContract::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("role_id"), RoleId);
	Object->SetArrayField(TEXT("responsibilities"), MakeStringArrayJson(Responsibilities));
	Object->SetArrayField(TEXT("allowed_outputs"), MakeStringArrayJson(AllowedOutputs));
	Object->SetArrayField(TEXT("prohibited_actions"), MakeStringArrayJson(ProhibitedActions));
	Object->SetArrayField(TEXT("required_input_contracts"), MakeStringArrayJson(RequiredInputContracts));
	Object->SetArrayField(TEXT("required_output_evidence"), MakeStringArrayJson(RequiredOutputEvidence));
	Object->SetArrayField(TEXT("allowed_next_roles"), MakeStringArrayJson(AllowedNextRoles));
	return Object;
}

TSharedPtr<FJsonObject> FOsvayderUERoleActionDecision::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("allowed"), bAllowed);
	Object->SetBoolField(TEXT("role_contract_resolved"), bRoleContractResolved);
	Object->SetStringField(TEXT("role_id"), RoleId);
	Object->SetStringField(TEXT("action_id"), ActionId);
	Object->SetStringField(TEXT("gate_reason_code"), GateReasonCode);
	Object->SetStringField(TEXT("blocker_family"), BlockerFamily);
	Object->SetStringField(TEXT("blocker_detail"), BlockerDetail);
	return Object;
}

TSharedPtr<FJsonObject> FOsvayderUERoleTransitionDecision::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("allowed"), bAllowed);
	Object->SetBoolField(TEXT("source_role_resolved"), bSourceRoleResolved);
	Object->SetBoolField(TEXT("target_role_resolved"), bTargetRoleResolved);
	Object->SetStringField(TEXT("source_role_id"), SourceRoleId);
	Object->SetStringField(TEXT("target_role_id"), TargetRoleId);
	Object->SetStringField(TEXT("gate_reason_code"), GateReasonCode);
	Object->SetStringField(TEXT("blocker_family"), BlockerFamily);
	Object->SetStringField(TEXT("blocker_detail"), BlockerDetail);
	return Object;
}

namespace OsvayderUERoleRegistry
{
	const FString& ArchitectRoleId()
	{
		static const FString RoleId(TEXT("architect"));
		return RoleId;
	}

	const FString& WorkerRoleId()
	{
		static const FString RoleId(TEXT("worker"));
		return RoleId;
	}

	const FString& VerifierRoleId()
	{
		static const FString RoleId(TEXT("verifier"));
		return RoleId;
	}

	FOsvayderUERoleContract GetArchitectRoleContract()
	{
		FOsvayderUERoleContract Contract;
		Contract.RoleId = ArchitectRoleId();
		Contract.Responsibilities = {
			TEXT("partition ambiguous work into bounded implementation packets"),
			TEXT("define risks and acceptance criteria"),
			TEXT("select recipe/evidence contracts without claiming proof")
		};
		Contract.AllowedOutputs = {
			TEXT("implementation_brief"),
			TEXT("task_packet"),
			TEXT("risk_assessment"),
			TEXT("acceptance_criteria")
		};
		Contract.ProhibitedActions = {
			TEXT("implementation_claim"),
			TEXT("runtime_proof_claim"),
			TEXT("final_acceptance_claim"),
			TEXT("managed_state_closeout_write")
		};
		Contract.RequiredInputContracts = {
			TEXT("request_context"),
			TEXT("recipe_evidence_contract_v1"),
			TEXT("execution_truth_contract_v1"),
			TEXT("run_isolated_closeout_contract_v1")
		};
		Contract.RequiredOutputEvidence = {
			TEXT("architect.implementation_brief"),
			TEXT("architect.acceptance_criteria"),
			TEXT("architect.risk_register")
		};
		Contract.AllowedNextRoles = {
			WorkerRoleId(),
			VerifierRoleId()
		};
		return Contract;
	}

	FOsvayderUERoleContract GetWorkerRoleContract()
	{
		FOsvayderUERoleContract Contract;
		Contract.RoleId = WorkerRoleId();
		Contract.Responsibilities = {
			TEXT("implement bounded source/content changes"),
			TEXT("produce current-run compile/runtime evidence"),
			TEXT("feed facts into deterministic closeout without self-acceptance")
		};
		Contract.AllowedOutputs = {
			TEXT("source_patch"),
			TEXT("test_log"),
			TEXT("runtime_evidence"),
			TEXT("closeout_evidence")
		};
		Contract.ProhibitedActions = {
			TEXT("closeout_manager_bypass"),
			TEXT("reviewer_acceptance_claim"),
			TEXT("accepted_closeout_verdict_direct_write"),
			TEXT("prose_only_runtime_proof")
		};
		Contract.RequiredInputContracts = {
			TEXT("architect_task_packet"),
			TEXT("recipe_evidence_contract_v1"),
			TEXT("execution_truth_contract_v1"),
			TEXT("run_isolated_closeout_contract_v1")
		};
		Contract.RequiredOutputEvidence = {
			TEXT("worker.change_summary"),
			TEXT("worker.build_or_test_evidence"),
			TEXT("worker.execution_truth_receipt")
		};
		Contract.AllowedNextRoles = {
			VerifierRoleId()
		};
		return Contract;
	}

	FOsvayderUERoleContract GetVerifierRoleContract()
	{
		FOsvayderUERoleContract Contract;
		Contract.RoleId = VerifierRoleId();
		Contract.Responsibilities = {
			TEXT("consume recipe/evidence/closeout facts"),
			TEXT("compare role output against deterministic closeout"),
			TEXT("emit verification verdicts without replacing the judge")
		};
		Contract.AllowedOutputs = {
			TEXT("verification_verdict"),
			TEXT("blocker_report"),
			TEXT("acceptance_recommendation")
		};
		Contract.ProhibitedActions = {
			TEXT("implementation_mutation"),
			TEXT("deterministic_judge_override"),
			TEXT("prose_only_judge_override"),
			TEXT("closeout_manager_bypass")
		};
		Contract.RequiredInputContracts = {
			TEXT("recipe_evidence_contract_v1"),
			TEXT("execution_truth_contract_v1"),
			TEXT("run_isolated_closeout_contract_v1"),
			TEXT("deterministic_closeout_decision")
		};
		Contract.RequiredOutputEvidence = {
			TEXT("verifier.recipe_schema_citation"),
			TEXT("verifier.deterministic_closeout_decision_citation"),
			TEXT("verifier.verification_log")
		};
		Contract.AllowedNextRoles = {
			ArchitectRoleId(),
			WorkerRoleId()
		};
		return Contract;
	}

	bool TryGetRoleContract(
		const FString& RoleId,
		FOsvayderUERoleContract& OutContract)
	{
		if (RoleId.Equals(ArchitectRoleId(), ESearchCase::CaseSensitive))
		{
			OutContract = GetArchitectRoleContract();
			return true;
		}
		if (RoleId.Equals(WorkerRoleId(), ESearchCase::CaseSensitive))
		{
			OutContract = GetWorkerRoleContract();
			return true;
		}
		if (RoleId.Equals(VerifierRoleId(), ESearchCase::CaseSensitive))
		{
			OutContract = GetVerifierRoleContract();
			return true;
		}

		OutContract = FOsvayderUERoleContract();
		return false;
	}

	TArray<FOsvayderUERoleContract> GetAllRoleContracts()
	{
		return {
			GetArchitectRoleContract(),
			GetWorkerRoleContract(),
			GetVerifierRoleContract()
		};
	}

	FOsvayderUERoleActionDecision EvaluateRoleAction(
		const FString& RoleId,
		const FString& ActionId)
	{
		FOsvayderUERoleContract Contract;
		if (!TryGetRoleContract(RoleId, Contract))
		{
			return MakeUnknownRoleActionDecision(RoleId, ActionId);
		}

		FOsvayderUERoleActionDecision Decision;
		Decision.RoleId = Contract.RoleId;
		Decision.ActionId = ActionId;
		Decision.bRoleContractResolved = true;

		if (Contract.ProhibitsAction(ActionId))
		{
			Decision.GateReasonCode = TEXT("role_contract_prohibited_action");
			Decision.BlockerFamily = TEXT("role_contract_violation");
			Decision.BlockerDetail = FString::Printf(
				TEXT("role_id=%s prohibited_action=%s"),
				*Contract.RoleId,
				*ActionId);
			return Decision;
		}

		if (!Contract.AllowsOutputOrEvidence(ActionId))
		{
			Decision.GateReasonCode = TEXT("role_contract_unlisted_action");
			Decision.BlockerFamily = TEXT("role_contract_violation");
			Decision.BlockerDetail = FString::Printf(
				TEXT("role_id=%s action_id=%s is not an allowed output/evidence contract"),
				*Contract.RoleId,
				*ActionId);
			return Decision;
		}

		Decision.bAllowed = true;
		return Decision;
	}

	FOsvayderUERoleTransitionDecision EvaluateRoleTransition(
		const FString& SourceRoleId,
		const FString& TargetRoleId)
	{
		FOsvayderUERoleContract SourceContract;
		FOsvayderUERoleContract TargetContract;
		const bool bSourceResolved = TryGetRoleContract(SourceRoleId, SourceContract);
		const bool bTargetResolved = TryGetRoleContract(TargetRoleId, TargetContract);
		if (!bSourceResolved)
		{
			FOsvayderUERoleTransitionDecision Decision =
				MakeUnknownRoleTransitionDecision(SourceRoleId, TargetRoleId, SourceRoleId);
			Decision.bTargetRoleResolved = bTargetResolved;
			return Decision;
		}
		if (!bTargetResolved)
		{
			FOsvayderUERoleTransitionDecision Decision =
				MakeUnknownRoleTransitionDecision(SourceRoleId, TargetRoleId, TargetRoleId);
			Decision.bSourceRoleResolved = true;
			return Decision;
		}

		FOsvayderUERoleTransitionDecision Decision;
		Decision.SourceRoleId = SourceContract.RoleId;
		Decision.TargetRoleId = TargetContract.RoleId;
		Decision.bSourceRoleResolved = true;
		Decision.bTargetRoleResolved = true;

		if (!SourceContract.AllowsNextRole(TargetContract.RoleId))
		{
			Decision.GateReasonCode = TEXT("role_contract_illegal_transition");
			Decision.BlockerFamily = TEXT("role_transition_not_allowed");
			Decision.BlockerDetail = FString::Printf(
				TEXT("source_role_id=%s target_role_id=%s allowed_next_roles=%s"),
				*SourceContract.RoleId,
				*TargetContract.RoleId,
				*JoinListOrNone(SourceContract.AllowedNextRoles));
			return Decision;
		}

		Decision.bAllowed = true;
		return Decision;
	}

	FString BuildRoleContractPromptContext(
		const FString& RoleId,
		const FString& RecipeId,
		const int32 EvidenceSchemaVersion)
	{
		FOsvayderUERoleContract Contract;
		if (!TryGetRoleContract(RoleId, Contract))
		{
			return FString::Printf(
				TEXT("role_id = %s\nrole_contract_resolved = false\nblocker_family = role_contract_unknown\nblocker_detail = No registered role contract for role_id=%s"),
				*RoleId,
				*RoleId);
		}

		return FString::Printf(
			TEXT("role_id = %s\nrole_contract_resolved = true\nrecipe_id = %s\nevidence_schema_version = %d\nresponsibilities = %s\nallowed_outputs = %s\nprohibited_actions = %s\nrequired_input_contracts = %s\nrequired_output_evidence = %s\nallowed_next_roles = %s\ncloseout_authority = deterministic_closeout_judge_remains_source_of_truth"),
			*Contract.RoleId,
			*RecipeId,
			EvidenceSchemaVersion,
			*JoinListOrNone(Contract.Responsibilities),
			*JoinListOrNone(Contract.AllowedOutputs),
			*JoinListOrNone(Contract.ProhibitedActions),
			*JoinListOrNone(Contract.RequiredInputContracts),
			*JoinListOrNone(Contract.RequiredOutputEvidence),
			*JoinListOrNone(Contract.AllowedNextRoles));
	}
}
