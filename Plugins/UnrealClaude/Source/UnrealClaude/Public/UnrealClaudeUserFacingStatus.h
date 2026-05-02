// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FUnrealClaudeActivePlan;
struct FUnrealClaudeActivePlanCloseoutDecision;
struct FUnrealClaudeRunCloseoutContext;

struct UNREALCLAUDE_API FUnrealClaudeUserFacingStatus
{
	FString StatusId;
	FString Headline;
	FString Detail;
	FString BlockerFamily;
	FString BlockerDetail;
	FString GateReasonCode;
	FString RunId;
	FString PlanId;
	FString FeatureWorkflowId;
	FString RecipeId;
	FString RoleId;
	int32 EvidenceSchemaVersion = 0;
	FString RawCurrentPhase;
	FString RawPlanStatus;
	FString RawResultStatus;

	TSharedPtr<FJsonObject> ToJsonObject() const;
};

namespace UnrealClaudeUserFacingStatus
{
	UNREALCLAUDE_API const FString& WorkRunningStatusId();
	UNREALCLAUDE_API const FString& RuntimeProofPassedStatusId();
	UNREALCLAUDE_API const FString& CloseoutPassedStatusId();
	UNREALCLAUDE_API const FString& CloseoutFailedStatusId();
	UNREALCLAUDE_API const FString& ManualVerificationRequiredStatusId();
	UNREALCLAUDE_API const FString& BlockedByExactCauseStatusId();

	UNREALCLAUDE_API bool IsRawInternalPhaseLabel(const FString& Text);

	UNREALCLAUDE_API FUnrealClaudeUserFacingStatus BuildStatus(
		const FUnrealClaudeActivePlan& Plan);

	UNREALCLAUDE_API FUnrealClaudeUserFacingStatus BuildStatus(
		const FUnrealClaudeActivePlan& Plan,
		const FUnrealClaudeActivePlanCloseoutDecision& Decision);

	UNREALCLAUDE_API FUnrealClaudeUserFacingStatus BuildStatus(
		const FUnrealClaudeActivePlan& Plan,
		const FUnrealClaudeActivePlanCloseoutDecision& Decision,
		const FUnrealClaudeRunCloseoutContext& Context);
}
