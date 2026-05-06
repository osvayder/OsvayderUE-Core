// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OsvayderUERelayAgent.h"
#include "OsvayderUETaskRecoverySummarizer.h"

DECLARE_DELEGATE_OneParam(FOnDeferredTaskResumed, const FString&);

struct FOsvayderUEDeferredPlanEntry
{
	FString PlanId;
	FString ArchiveFilePath;
	FString OriginalUserTask;
	FString ShortTitle;
	FString UserClosedReason;
	FString Status;
	FString UpdatedAtUtc;
};

class FOsvayderUEDeferredTasksEnumerator
{
public:
	static TArray<FOsvayderUEDeferredPlanEntry> ListDeferredPlans();
	static int32 CountDeferredPlans();
};

class FOsvayderUEDeferredTasksBrowser
{
public:
	using FOnPlanResumed = FOnDeferredTaskResumed;

	static void Show(const FOnPlanResumed& OnPlanResumed = FOnPlanResumed());
	static bool LoadArchivedPlan(const FOsvayderUEDeferredPlanEntry& Entry, FOsvayderUEActivePlan& OutPlan, FString& OutError);
	static bool ResumePlan(const FOsvayderUEDeferredPlanEntry& Entry, FString& OutShortTitle, FString& OutError);
	static bool DeletePlan(const FOsvayderUEDeferredPlanEntry& Entry, FString& OutError);
	static FString BuildStatusLabel(const FString& Status);
	static FString BuildReasonLabel(const FString& Reason);
	static FString BuildMetaLine(const FOsvayderUEDeferredPlanEntry& Entry, const FTaskRecoverySummary& Summary);

private:
	static FString BuildBackupArchivePath(const FString& ExistingPlanId);
	static bool BackupCurrentActivePlanIfPresent(FString& OutBackupPath, FString& OutError);
};
