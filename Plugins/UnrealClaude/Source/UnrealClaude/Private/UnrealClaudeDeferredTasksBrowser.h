// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealClaudeRelayAgent.h"
#include "UnrealClaudeTaskRecoverySummarizer.h"

DECLARE_DELEGATE_OneParam(FOnDeferredTaskResumed, const FString&);

struct FUnrealClaudeDeferredPlanEntry
{
	FString PlanId;
	FString ArchiveFilePath;
	FString OriginalUserTask;
	FString ShortTitle;
	FString UserClosedReason;
	FString Status;
	FString UpdatedAtUtc;
};

class FUnrealClaudeDeferredTasksEnumerator
{
public:
	static TArray<FUnrealClaudeDeferredPlanEntry> ListDeferredPlans();
	static int32 CountDeferredPlans();
};

class FUnrealClaudeDeferredTasksBrowser
{
public:
	using FOnPlanResumed = FOnDeferredTaskResumed;

	static void Show(const FOnPlanResumed& OnPlanResumed = FOnPlanResumed());
	static bool LoadArchivedPlan(const FUnrealClaudeDeferredPlanEntry& Entry, FUnrealClaudeActivePlan& OutPlan, FString& OutError);
	static bool ResumePlan(const FUnrealClaudeDeferredPlanEntry& Entry, FString& OutShortTitle, FString& OutError);
	static bool DeletePlan(const FUnrealClaudeDeferredPlanEntry& Entry, FString& OutError);
	static FString BuildStatusLabel(const FString& Status);
	static FString BuildReasonLabel(const FString& Reason);
	static FString BuildMetaLine(const FUnrealClaudeDeferredPlanEntry& Entry, const FTaskRecoverySummary& Summary);

private:
	static FString BuildBackupArchivePath(const FString& ExistingPlanId);
	static bool BackupCurrentActivePlanIfPresent(FString& OutBackupPath, FString& OutError);
};
