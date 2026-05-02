// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct UNREALCLAUDE_API FUnrealClaudeRecipeEvidenceContract
{
	FString RecipeId;
	int32 EvidenceSchemaVersion = 0;
	TArray<FString> RequiredPositiveEvidenceFacts;
	TArray<FString> AllowedToolFamilies;
	TArray<FString> NegativeEvidenceConditions;
	TArray<FString> RequiredFocusedTests;
	TArray<FString> OptionalLiveSmokeRequirements;

	bool IsValid() const
	{
		return !RecipeId.IsEmpty() && EvidenceSchemaVersion > 0;
	}

	bool RequiresPositiveEvidence(const FString& EvidenceFact) const
	{
		return RequiredPositiveEvidenceFacts.Contains(EvidenceFact);
	}

	TSharedPtr<FJsonObject> ToJsonObject() const;
};

namespace UnrealClaudeRecipeRegistry
{
	UNREALCLAUDE_API const FString& InteractionAccessRecipeId();

	UNREALCLAUDE_API const FString& InventoryBasicUiRecipeId();

	UNREALCLAUDE_API bool TryGetRecipeEvidenceContract(
		const FString& RecipeId,
		FUnrealClaudeRecipeEvidenceContract& OutContract);

	UNREALCLAUDE_API FUnrealClaudeRecipeEvidenceContract GetInteractionAccessRecipeEvidenceContract();

	UNREALCLAUDE_API FUnrealClaudeRecipeEvidenceContract GetInventoryBasicUiRecipeEvidenceContract();

	UNREALCLAUDE_API TArray<FUnrealClaudeRecipeEvidenceContract> GetAllRecipeEvidenceContracts();
}
