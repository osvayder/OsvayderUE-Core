// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "OsvayderUERoleRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOsvayderUERoleContracts_RegistryResolvesInitialRoles,
	"OsvayderUE.RoleContracts.RegistryResolvesInitialRoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOsvayderUERoleContracts_UnknownRoleFailsClosed,
	"OsvayderUE.RoleContracts.UnknownRoleFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOsvayderUERoleContracts_GuardsRejectIllegalRoleActions,
	"OsvayderUE.RoleContracts.GuardsRejectIllegalRoleActions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOsvayderUERoleContracts_TransitionGuardFailsClosed,
	"OsvayderUE.RoleContracts.TransitionGuardFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOsvayderUERoleContracts_RegistryResolvesInitialRoles::RunTest(const FString& Parameters)
{
	const TArray<FOsvayderUERoleContract> Contracts = OsvayderUERoleRegistry::GetAllRoleContracts();
	TestEqual(TEXT("registry should expose exactly three initial role contracts"), Contracts.Num(), 3);

	const TArray<FString> RoleIds = {
		OsvayderUERoleRegistry::ArchitectRoleId(),
		OsvayderUERoleRegistry::WorkerRoleId(),
		OsvayderUERoleRegistry::VerifierRoleId()
	};
	for (const FString& RoleId : RoleIds)
	{
		FOsvayderUERoleContract Contract;
		TestTrue(
			FString::Printf(TEXT("%s role should resolve"), *RoleId),
			OsvayderUERoleRegistry::TryGetRoleContract(RoleId, Contract));
		TestEqual(TEXT("role id should round-trip"), Contract.RoleId, RoleId);
		TestTrue(TEXT("role should have responsibilities"), Contract.Responsibilities.Num() > 0);
		TestTrue(TEXT("role should have allowed outputs"), Contract.AllowedOutputs.Num() > 0);
		TestTrue(TEXT("role should have prohibited actions"), Contract.ProhibitedActions.Num() > 0);
		TestTrue(TEXT("role should have required input contracts"), Contract.RequiredInputContracts.Num() > 0);
		TestTrue(TEXT("role should have required output evidence"), Contract.RequiredOutputEvidence.Num() > 0);
		TestTrue(TEXT("role should have allowed next roles"), Contract.AllowedNextRoles.Num() > 0);
		TestTrue(TEXT("role should reference recipe evidence contracts"),
			Contract.RequiredInputContracts.Contains(TEXT("recipe_evidence_contract_v1")));
	}

	return true;
}

bool FOsvayderUERoleContracts_UnknownRoleFailsClosed::RunTest(const FString& Parameters)
{
	FOsvayderUERoleContract Contract;
	TestFalse(
		TEXT("unknown role should not resolve"),
		OsvayderUERoleRegistry::TryGetRoleContract(TEXT("operator"), Contract));
	TestFalse(TEXT("unknown role should not return a valid contract"), Contract.IsValid());

	const FOsvayderUERoleActionDecision Decision =
		OsvayderUERoleRegistry::EvaluateRoleAction(TEXT("operator"), TEXT("runtime_evidence"));
	TestFalse(TEXT("unknown role action should fail closed"), Decision.bAllowed);
	TestEqual(
		TEXT("unknown role should name missing role contract"),
		Decision.BlockerFamily,
		FString(TEXT("role_contract_unknown")));
	TestTrue(
		TEXT("unknown role detail should include role id"),
		Decision.BlockerDetail.Contains(TEXT("operator"), ESearchCase::CaseSensitive));
	return true;
}

bool FOsvayderUERoleContracts_GuardsRejectIllegalRoleActions::RunTest(const FString& Parameters)
{
	const FOsvayderUERoleActionDecision ArchitectBrief =
		OsvayderUERoleRegistry::EvaluateRoleAction(TEXT("architect"), TEXT("implementation_brief"));
	TestTrue(TEXT("architect can produce implementation briefs"), ArchitectBrief.bAllowed);

	const FOsvayderUERoleActionDecision ArchitectRuntimeClaim =
		OsvayderUERoleRegistry::EvaluateRoleAction(TEXT("architect"), TEXT("runtime_proof_claim"));
	TestFalse(TEXT("architect cannot claim runtime proof"), ArchitectRuntimeClaim.bAllowed);
	TestEqual(
		TEXT("architect runtime claim should be a role violation"),
		ArchitectRuntimeClaim.GateReasonCode,
		FString(TEXT("role_contract_prohibited_action")));

	const FOsvayderUERoleActionDecision WorkerEvidence =
		OsvayderUERoleRegistry::EvaluateRoleAction(TEXT("worker"), TEXT("runtime_evidence"));
	TestTrue(TEXT("worker can produce runtime evidence"), WorkerEvidence.bAllowed);

	const FOsvayderUERoleActionDecision WorkerAcceptance =
		OsvayderUERoleRegistry::EvaluateRoleAction(TEXT("worker"), TEXT("reviewer_acceptance_claim"));
	TestFalse(TEXT("worker cannot fabricate reviewer acceptance"), WorkerAcceptance.bAllowed);
	TestEqual(
		TEXT("worker reviewer acceptance should be a role violation"),
		WorkerAcceptance.GateReasonCode,
		FString(TEXT("role_contract_prohibited_action")));

	const FOsvayderUERoleActionDecision VerifierVerdict =
		OsvayderUERoleRegistry::EvaluateRoleAction(TEXT("verifier"), TEXT("verification_verdict"));
	TestTrue(TEXT("verifier can produce verification verdicts"), VerifierVerdict.bAllowed);

	const FOsvayderUERoleActionDecision VerifierJudgeOverride =
		OsvayderUERoleRegistry::EvaluateRoleAction(TEXT("verifier"), TEXT("prose_only_judge_override"));
	TestFalse(TEXT("verifier cannot replace deterministic judge with prose"), VerifierJudgeOverride.bAllowed);
	TestEqual(
		TEXT("verifier judge override should be a role violation"),
		VerifierJudgeOverride.GateReasonCode,
		FString(TEXT("role_contract_prohibited_action")));
	return true;
}

bool FOsvayderUERoleContracts_TransitionGuardFailsClosed::RunTest(const FString& Parameters)
{
	const FOsvayderUERoleTransitionDecision ArchitectToWorker =
		OsvayderUERoleRegistry::EvaluateRoleTransition(TEXT("architect"), TEXT("worker"));
	TestTrue(TEXT("architect can hand off to worker"), ArchitectToWorker.bAllowed);

	const FOsvayderUERoleTransitionDecision WorkerToVerifier =
		OsvayderUERoleRegistry::EvaluateRoleTransition(TEXT("worker"), TEXT("verifier"));
	TestTrue(TEXT("worker can hand off to verifier"), WorkerToVerifier.bAllowed);

	const FOsvayderUERoleTransitionDecision WorkerToArchitect =
		OsvayderUERoleRegistry::EvaluateRoleTransition(TEXT("worker"), TEXT("architect"));
	TestFalse(TEXT("worker cannot skip back to architect as an allowed next role"), WorkerToArchitect.bAllowed);
	TestEqual(
		TEXT("illegal transition should fail closed"),
		WorkerToArchitect.GateReasonCode,
		FString(TEXT("role_contract_illegal_transition")));

	const FOsvayderUERoleTransitionDecision UnknownToVerifier =
		OsvayderUERoleRegistry::EvaluateRoleTransition(TEXT("operator"), TEXT("verifier"));
	TestFalse(TEXT("unknown transition source should fail closed"), UnknownToVerifier.bAllowed);
	TestEqual(
		TEXT("unknown transition should name missing role contract"),
		UnknownToVerifier.BlockerFamily,
		FString(TEXT("role_contract_unknown")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
