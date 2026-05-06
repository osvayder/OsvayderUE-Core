// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct OSVAYDERUE_API FOsvayderUERecipeEvidenceContract
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

namespace OsvayderUERecipeRegistry
{
	OSVAYDERUE_API const FString& InteractionAccessRecipeId();

	OSVAYDERUE_API const FString& InventoryBasicUiRecipeId();

	OSVAYDERUE_API bool TryGetRecipeEvidenceContract(
		const FString& RecipeId,
		FOsvayderUERecipeEvidenceContract& OutContract);

	OSVAYDERUE_API FOsvayderUERecipeEvidenceContract GetInteractionAccessRecipeEvidenceContract();

	OSVAYDERUE_API FOsvayderUERecipeEvidenceContract GetInventoryBasicUiRecipeEvidenceContract();

	OSVAYDERUE_API TArray<FOsvayderUERecipeEvidenceContract> GetAllRecipeEvidenceContracts();
}
