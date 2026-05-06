// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct OSVAYDERUE_API FOsvayderUERoleContract
{
	FString RoleId;
	TArray<FString> Responsibilities;
	TArray<FString> AllowedOutputs;
	TArray<FString> ProhibitedActions;
	TArray<FString> RequiredInputContracts;
	TArray<FString> RequiredOutputEvidence;
	TArray<FString> AllowedNextRoles;

	bool IsValid() const
	{
		return !RoleId.IsEmpty()
			&& Responsibilities.Num() > 0
			&& AllowedOutputs.Num() > 0
			&& ProhibitedActions.Num() > 0
			&& RequiredInputContracts.Num() > 0
			&& RequiredOutputEvidence.Num() > 0;
	}

	bool AllowsNextRole(const FString& NextRoleId) const;
	bool AllowsOutputOrEvidence(const FString& OutputId) const;
	bool ProhibitsAction(const FString& ActionId) const;
	TSharedPtr<FJsonObject> ToJsonObject() const;
};

struct OSVAYDERUE_API FOsvayderUERoleActionDecision
{
	bool bAllowed = false;
	bool bRoleContractResolved = false;
	FString RoleId;
	FString ActionId;
	FString GateReasonCode;
	FString BlockerFamily;
	FString BlockerDetail;

	TSharedPtr<FJsonObject> ToJsonObject() const;
};

struct OSVAYDERUE_API FOsvayderUERoleTransitionDecision
{
	bool bAllowed = false;
	bool bSourceRoleResolved = false;
	bool bTargetRoleResolved = false;
	FString SourceRoleId;
	FString TargetRoleId;
	FString GateReasonCode;
	FString BlockerFamily;
	FString BlockerDetail;

	TSharedPtr<FJsonObject> ToJsonObject() const;
};

namespace OsvayderUERoleRegistry
{
	OSVAYDERUE_API const FString& ArchitectRoleId();
	OSVAYDERUE_API const FString& WorkerRoleId();
	OSVAYDERUE_API const FString& VerifierRoleId();

	OSVAYDERUE_API bool TryGetRoleContract(
		const FString& RoleId,
		FOsvayderUERoleContract& OutContract);

	OSVAYDERUE_API FOsvayderUERoleContract GetArchitectRoleContract();
	OSVAYDERUE_API FOsvayderUERoleContract GetWorkerRoleContract();
	OSVAYDERUE_API FOsvayderUERoleContract GetVerifierRoleContract();
	OSVAYDERUE_API TArray<FOsvayderUERoleContract> GetAllRoleContracts();

	OSVAYDERUE_API FOsvayderUERoleActionDecision EvaluateRoleAction(
		const FString& RoleId,
		const FString& ActionId);

	OSVAYDERUE_API FOsvayderUERoleTransitionDecision EvaluateRoleTransition(
		const FString& SourceRoleId,
		const FString& TargetRoleId);

	OSVAYDERUE_API FString BuildRoleContractPromptContext(
		const FString& RoleId,
		const FString& RecipeId,
		int32 EvidenceSchemaVersion);
}
