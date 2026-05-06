// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FOsvayderUEActivePlan;
struct FOsvayderUEActivePlanCloseoutDecision;
struct FOsvayderUERunCloseoutContext;

struct OSVAYDERUE_API FOsvayderUEUserFacingStatus
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

namespace OsvayderUEUserFacingStatus
{
	OSVAYDERUE_API const FString& WorkRunningStatusId();
	OSVAYDERUE_API const FString& RuntimeProofPassedStatusId();
	OSVAYDERUE_API const FString& CloseoutPassedStatusId();
	OSVAYDERUE_API const FString& CloseoutFailedStatusId();
	OSVAYDERUE_API const FString& ManualVerificationRequiredStatusId();
	OSVAYDERUE_API const FString& BlockedByExactCauseStatusId();

	OSVAYDERUE_API bool IsRawInternalPhaseLabel(const FString& Text);

	OSVAYDERUE_API FOsvayderUEUserFacingStatus BuildStatus(
		const FOsvayderUEActivePlan& Plan);

	OSVAYDERUE_API FOsvayderUEUserFacingStatus BuildStatus(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision& Decision);

	OSVAYDERUE_API FOsvayderUEUserFacingStatus BuildStatus(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision& Decision,
		const FOsvayderUERunCloseoutContext& Context);
}
