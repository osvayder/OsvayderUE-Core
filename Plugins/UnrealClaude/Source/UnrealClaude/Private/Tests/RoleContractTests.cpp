// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealClaudeRoleRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealClaudeRoleContracts_RegistryResolvesInitialRoles,
	"UnrealClaude.RoleContracts.RegistryResolvesInitialRoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealClaudeRoleContracts_UnknownRoleFailsClosed,
	"UnrealClaude.RoleContracts.UnknownRoleFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealClaudeRoleContracts_GuardsRejectIllegalRoleActions,
	"UnrealClaude.RoleContracts.GuardsRejectIllegalRoleActions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealClaudeRoleContracts_TransitionGuardFailsClosed,
	"UnrealClaude.RoleContracts.TransitionGuardFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealClaudeRoleContracts_RegistryResolvesInitialRoles::RunTest(const FString& Parameters)
{
	const TArray<FUnrealClaudeRoleContract> Contracts = UnrealClaudeRoleRegistry::GetAllRoleContracts();
	TestEqual(TEXT("registry should expose exactly three initial role contracts"), Contracts.Num(), 3);

	const TArray<FString> RoleIds = {
		UnrealClaudeRoleRegistry::ArchitectRoleId(),
		UnrealClaudeRoleRegistry::WorkerRoleId(),
		UnrealClaudeRoleRegistry::VerifierRoleId()
	};
	for (const FString& RoleId : RoleIds)
	{
		FUnrealClaudeRoleContract Contract;
		TestTrue(
			FString::Printf(TEXT("%s role should resolve"), *RoleId),
			UnrealClaudeRoleRegistry::TryGetRoleContract(RoleId, Contract));
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

bool FUnrealClaudeRoleContracts_UnknownRoleFailsClosed::RunTest(const FString& Parameters)
{
	FUnrealClaudeRoleContract Contract;
	TestFalse(
		TEXT("unknown role should not resolve"),
		UnrealClaudeRoleRegistry::TryGetRoleContract(TEXT("operator"), Contract));
	TestFalse(TEXT("unknown role should not return a valid contract"), Contract.IsValid());

	const FUnrealClaudeRoleActionDecision Decision =
		UnrealClaudeRoleRegistry::EvaluateRoleAction(TEXT("operator"), TEXT("runtime_evidence"));
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

bool FUnrealClaudeRoleContracts_GuardsRejectIllegalRoleActions::RunTest(const FString& Parameters)
{
	const FUnrealClaudeRoleActionDecision ArchitectBrief =
		UnrealClaudeRoleRegistry::EvaluateRoleAction(TEXT("architect"), TEXT("implementation_brief"));
	TestTrue(TEXT("architect can produce implementation briefs"), ArchitectBrief.bAllowed);

	const FUnrealClaudeRoleActionDecision ArchitectRuntimeClaim =
		UnrealClaudeRoleRegistry::EvaluateRoleAction(TEXT("architect"), TEXT("runtime_proof_claim"));
	TestFalse(TEXT("architect cannot claim runtime proof"), ArchitectRuntimeClaim.bAllowed);
	TestEqual(
		TEXT("architect runtime claim should be a role violation"),
		ArchitectRuntimeClaim.GateReasonCode,
		FString(TEXT("role_contract_prohibited_action")));

	const FUnrealClaudeRoleActionDecision WorkerEvidence =
		UnrealClaudeRoleRegistry::EvaluateRoleAction(TEXT("worker"), TEXT("runtime_evidence"));
	TestTrue(TEXT("worker can produce runtime evidence"), WorkerEvidence.bAllowed);

	const FUnrealClaudeRoleActionDecision WorkerAcceptance =
		UnrealClaudeRoleRegistry::EvaluateRoleAction(TEXT("worker"), TEXT("reviewer_acceptance_claim"));
	TestFalse(TEXT("worker cannot fabricate reviewer acceptance"), WorkerAcceptance.bAllowed);
	TestEqual(
		TEXT("worker reviewer acceptance should be a role violation"),
		WorkerAcceptance.GateReasonCode,
		FString(TEXT("role_contract_prohibited_action")));

	const FUnrealClaudeRoleActionDecision VerifierVerdict =
		UnrealClaudeRoleRegistry::EvaluateRoleAction(TEXT("verifier"), TEXT("verification_verdict"));
	TestTrue(TEXT("verifier can produce verification verdicts"), VerifierVerdict.bAllowed);

	const FUnrealClaudeRoleActionDecision VerifierJudgeOverride =
		UnrealClaudeRoleRegistry::EvaluateRoleAction(TEXT("verifier"), TEXT("prose_only_judge_override"));
	TestFalse(TEXT("verifier cannot replace deterministic judge with prose"), VerifierJudgeOverride.bAllowed);
	TestEqual(
		TEXT("verifier judge override should be a role violation"),
		VerifierJudgeOverride.GateReasonCode,
		FString(TEXT("role_contract_prohibited_action")));
	return true;
}

bool FUnrealClaudeRoleContracts_TransitionGuardFailsClosed::RunTest(const FString& Parameters)
{
	const FUnrealClaudeRoleTransitionDecision ArchitectToWorker =
		UnrealClaudeRoleRegistry::EvaluateRoleTransition(TEXT("architect"), TEXT("worker"));
	TestTrue(TEXT("architect can hand off to worker"), ArchitectToWorker.bAllowed);

	const FUnrealClaudeRoleTransitionDecision WorkerToVerifier =
		UnrealClaudeRoleRegistry::EvaluateRoleTransition(TEXT("worker"), TEXT("verifier"));
	TestTrue(TEXT("worker can hand off to verifier"), WorkerToVerifier.bAllowed);

	const FUnrealClaudeRoleTransitionDecision WorkerToArchitect =
		UnrealClaudeRoleRegistry::EvaluateRoleTransition(TEXT("worker"), TEXT("architect"));
	TestFalse(TEXT("worker cannot skip back to architect as an allowed next role"), WorkerToArchitect.bAllowed);
	TestEqual(
		TEXT("illegal transition should fail closed"),
		WorkerToArchitect.GateReasonCode,
		FString(TEXT("role_contract_illegal_transition")));

	const FUnrealClaudeRoleTransitionDecision UnknownToVerifier =
		UnrealClaudeRoleRegistry::EvaluateRoleTransition(TEXT("operator"), TEXT("verifier"));
	TestFalse(TEXT("unknown transition source should fail closed"), UnknownToVerifier.bAllowed);
	TestEqual(
		TEXT("unknown transition should name missing role contract"),
		UnknownToVerifier.BlockerFamily,
		FString(TEXT("role_contract_unknown")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
