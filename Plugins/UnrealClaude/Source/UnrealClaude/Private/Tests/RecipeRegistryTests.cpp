// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealClaudeRecipeRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealClaudeRecipeRegistry_InteractionAccessContractResolves,
	"UnrealClaude.RecipeRegistry.InteractionAccessContract.ResolvesSchemaAndObligations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealClaudeRecipeRegistry_UnknownRecipeLookupFailsClosed,
	"UnrealClaude.RecipeRegistry.UnknownRecipeLookupFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealClaudeRecipeRegistry_InteractionAccessContractResolves::RunTest(const FString& Parameters)
{
	FUnrealClaudeRecipeEvidenceContract Contract;
	const bool bResolved = UnrealClaudeRecipeRegistry::TryGetRecipeEvidenceContract(
		TEXT("feature.interaction_access_slice_v1"),
		Contract);

	TestTrue(TEXT("known interaction-access recipe should resolve"), bResolved);
	TestEqual(TEXT("recipe id should match"), Contract.RecipeId, FString(TEXT("feature.interaction_access_slice_v1")));
	TestEqual(TEXT("schema version should be v1"), Contract.EvidenceSchemaVersion, 1);
	TestTrue(TEXT("contract should require proof map"),
		Contract.RequiredPositiveEvidenceFacts.Contains(TEXT("known_proof_map_available")));
	TestTrue(TEXT("contract should require proof input mapping"),
		Contract.RequiredPositiveEvidenceFacts.Contains(TEXT("proof_input_mapping_available")));
	TestTrue(TEXT("contract should require bounded automation"),
		Contract.RequiredPositiveEvidenceFacts.Contains(TEXT("bounded_prison_access_automation_proof")));
	TestTrue(TEXT("contract should allow enhanced input evidence"),
		Contract.AllowedToolFamilies.Contains(TEXT("enhanced_input")));
	TestTrue(TEXT("contract should allow read-only command evidence"),
		Contract.AllowedToolFamilies.Contains(TEXT("command_execution_read_only")));
	TestTrue(TEXT("contract should reject managed-state manual writes"),
		Contract.NegativeEvidenceConditions.Contains(TEXT("managed_state_manual_write")));
	TestTrue(TEXT("contract should reject stale evidence"),
		Contract.NegativeEvidenceConditions.Contains(TEXT("stale_or_out_of_run_evidence")));
	TestTrue(TEXT("contract should list focused registry test"),
		Contract.RequiredFocusedTests.Contains(TEXT("UnrealClaude.RecipeRegistry.InteractionAccessContract")));
	TestTrue(TEXT("contract should list optional live smoke"),
		Contract.OptionalLiveSmokeRequirements.Contains(TEXT("Alternative.PrisonAccess")));
	return true;
}

bool FUnrealClaudeRecipeRegistry_UnknownRecipeLookupFailsClosed::RunTest(const FString& Parameters)
{
	FUnrealClaudeRecipeEvidenceContract Contract;
	const bool bResolved = UnrealClaudeRecipeRegistry::TryGetRecipeEvidenceContract(
		TEXT("feature.unknown_recipe_v1"),
		Contract);

	TestFalse(TEXT("unknown recipe should not resolve"), bResolved);
	TestFalse(TEXT("unknown recipe should not return a valid contract"), Contract.IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
