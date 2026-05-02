// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeRecipeRegistry.h"

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
}

TSharedPtr<FJsonObject> FUnrealClaudeRecipeEvidenceContract::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("recipe_id"), RecipeId);
	Object->SetNumberField(TEXT("evidence_schema_version"), EvidenceSchemaVersion);
	Object->SetArrayField(TEXT("required_positive_evidence_facts"), MakeStringArrayJson(RequiredPositiveEvidenceFacts));
	Object->SetArrayField(TEXT("allowed_tool_families"), MakeStringArrayJson(AllowedToolFamilies));
	Object->SetArrayField(TEXT("negative_evidence_conditions"), MakeStringArrayJson(NegativeEvidenceConditions));
	Object->SetArrayField(TEXT("required_focused_tests"), MakeStringArrayJson(RequiredFocusedTests));
	Object->SetArrayField(TEXT("optional_live_smoke_requirements"), MakeStringArrayJson(OptionalLiveSmokeRequirements));
	return Object;
}

namespace UnrealClaudeRecipeRegistry
{
	const FString& InteractionAccessRecipeId()
	{
		static const FString RecipeId(TEXT("feature.interaction_access_slice_v1"));
		return RecipeId;
	}

	const FString& InventoryBasicUiRecipeId()
	{
		static const FString RecipeId(TEXT("feature.inventory_basic_ui_v1"));
		return RecipeId;
	}

	FUnrealClaudeRecipeEvidenceContract GetInteractionAccessRecipeEvidenceContract()
	{
		FUnrealClaudeRecipeEvidenceContract Contract;
		Contract.RecipeId = InteractionAccessRecipeId();
		Contract.EvidenceSchemaVersion = 1;
		Contract.RequiredPositiveEvidenceFacts = {
			TEXT("known_proof_map_available"),
			TEXT("proof_input_mapping_available"),
			TEXT("placed_runtime_actors_available"),
			TEXT("attempt_resolver_source_observed"),
			TEXT("event_subsystem_source_observed"),
			TEXT("runtime_smoke_success_observed"),
			TEXT("prison_access_event_observed"),
			TEXT("bounded_prison_access_automation_proof")
		};
		Contract.AllowedToolFamilies = {
			TEXT("asset_search"),
			TEXT("enhanced_input"),
			TEXT("open_level"),
			TEXT("get_level_actors"),
			TEXT("map_runtime_proof"),
			TEXT("runtime_proof"),
			TEXT("automation_test"),
			TEXT("source_inspection"),
			TEXT("command_execution_read_only")
		};
		Contract.NegativeEvidenceConditions = {
			TEXT("managed_state_manual_write"),
			TEXT("stale_or_out_of_run_evidence"),
			TEXT("unsafe_or_unknown_command_evidence"),
			TEXT("zero_test_automation"),
			TEXT("missing_required_recipe_obligation")
		};
		Contract.RequiredFocusedTests = {
			TEXT("UnrealClaude.RecipeRegistry.InteractionAccessContract"),
			TEXT("UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RecipeRegistryUnknownRecipeFailsClosed"),
			TEXT("UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RecipeRegistryMissingObligationNamesMissingFact"),
			TEXT("UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.ExecutionTruthManagedStateCommandRejectsWithoutLegacyMutationFlag"),
			TEXT("UnrealClaude.ActivePlanCloseout.SystemicVerifierJudge.RunScopedOldTraceManualWriteDoesNotPoisonCleanRun")
		};
		Contract.OptionalLiveSmokeRequirements = {
			TEXT("Alternative.PrisonAccess"),
			TEXT("ProofFixtureSmoke"),
			TEXT("PrisonAccessEvent")
		};
		return Contract;
	}

	FUnrealClaudeRecipeEvidenceContract GetInventoryBasicUiRecipeEvidenceContract()
	{
		FUnrealClaudeRecipeEvidenceContract Contract;
		Contract.RecipeId = InventoryBasicUiRecipeId();
		Contract.EvidenceSchemaVersion = 1;
		Contract.RequiredPositiveEvidenceFacts = {
			TEXT("runtime_proof_state_passed")
		};
		Contract.AllowedToolFamilies = {
			TEXT("workspace_file_build"),
			TEXT("runtime_proof"),
			TEXT("automation_test")
		};
		Contract.NegativeEvidenceConditions = {
			TEXT("managed_state_manual_write"),
			TEXT("stale_or_out_of_run_evidence"),
			TEXT("missing_required_recipe_obligation")
		};
		Contract.RequiredFocusedTests = {
			TEXT("UnrealClaude.ActivePlanCloseout.FeatureWorkflowCompileAndRuntimeCanBeFull")
		};
		return Contract;
	}

	bool TryGetRecipeEvidenceContract(
		const FString& RecipeId,
		FUnrealClaudeRecipeEvidenceContract& OutContract)
	{
		if (RecipeId.Equals(InteractionAccessRecipeId(), ESearchCase::CaseSensitive))
		{
			OutContract = GetInteractionAccessRecipeEvidenceContract();
			return true;
		}
		if (RecipeId.Equals(InventoryBasicUiRecipeId(), ESearchCase::CaseSensitive))
		{
			OutContract = GetInventoryBasicUiRecipeEvidenceContract();
			return true;
		}

		OutContract = FUnrealClaudeRecipeEvidenceContract();
		return false;
	}

	TArray<FUnrealClaudeRecipeEvidenceContract> GetAllRecipeEvidenceContracts()
	{
		return {
			GetInteractionAccessRecipeEvidenceContract(),
			GetInventoryBasicUiRecipeEvidenceContract()
		};
	}
}
