// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct UNREALCLAUDE_API FUnrealClaudeRoleContract
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

struct UNREALCLAUDE_API FUnrealClaudeRoleActionDecision
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

struct UNREALCLAUDE_API FUnrealClaudeRoleTransitionDecision
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

namespace UnrealClaudeRoleRegistry
{
	UNREALCLAUDE_API const FString& ArchitectRoleId();
	UNREALCLAUDE_API const FString& WorkerRoleId();
	UNREALCLAUDE_API const FString& VerifierRoleId();

	UNREALCLAUDE_API bool TryGetRoleContract(
		const FString& RoleId,
		FUnrealClaudeRoleContract& OutContract);

	UNREALCLAUDE_API FUnrealClaudeRoleContract GetArchitectRoleContract();
	UNREALCLAUDE_API FUnrealClaudeRoleContract GetWorkerRoleContract();
	UNREALCLAUDE_API FUnrealClaudeRoleContract GetVerifierRoleContract();
	UNREALCLAUDE_API TArray<FUnrealClaudeRoleContract> GetAllRoleContracts();

	UNREALCLAUDE_API FUnrealClaudeRoleActionDecision EvaluateRoleAction(
		const FString& RoleId,
		const FString& ActionId);

	UNREALCLAUDE_API FUnrealClaudeRoleTransitionDecision EvaluateRoleTransition(
		const FString& SourceRoleId,
		const FString& TargetRoleId);

	UNREALCLAUDE_API FString BuildRoleContractPromptContext(
		const FString& RoleId,
		const FString& RecipeId,
		int32 EvidenceSchemaVersion);
}
