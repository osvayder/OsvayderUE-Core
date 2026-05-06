// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"

class FJsonObject;

struct FOsvayderUECanonLedgerEntry
{
	FString PatternKey;
	FString Subsystem;
	FString ShortTitle;
	FString ChosenPathSummary;
	FString WhyPreferred;
	FString ProofReference;
	FString BadPathToAvoid;
	FString LastConfirmedEngineContext;

	bool IsValid() const
	{
		return !PatternKey.IsEmpty()
			&& !Subsystem.IsEmpty()
			&& !ShortTitle.IsEmpty()
			&& !ChosenPathSummary.IsEmpty()
			&& !WhyPreferred.IsEmpty()
			&& !ProofReference.IsEmpty();
	}
};

struct FOsvayderUECanonPromotionRequest
{
	FString ProvenRunReceiptPath;
	FString PatternKey;
	FString ExpectedSubsystem;
	FString ShortTitle;
	FString ChosenPathSummary;
	FString WhyPreferred;
	FString BadPathToAvoid;
	FString LastConfirmedEngineContext;

	bool IsValid() const
	{
		return !ProvenRunReceiptPath.IsEmpty()
			&& !PatternKey.IsEmpty()
			&& !ShortTitle.IsEmpty()
			&& !ChosenPathSummary.IsEmpty()
			&& !WhyPreferred.IsEmpty();
	}
};

class FOsvayderUECanonLedger
{
public:
	static FString GetLedgerPath();
	static bool LoadEntries(TArray<FOsvayderUECanonLedgerEntry>& OutEntries, FString& OutError);
	static bool SaveEntries(const TArray<FOsvayderUECanonLedgerEntry>& Entries, FString& OutError);
	static bool PromoteEntry(const FOsvayderUECanonLedgerEntry& Entry, FString& OutError);
	static bool PromoteFromProvenRun(
		const FOsvayderUECanonPromotionRequest& Request,
		FString& OutError,
		FOsvayderUECanonLedgerEntry* OutPromotedEntry = nullptr);
	static FString BuildPromptContext(
		const FString& Prompt,
		const FString& DetectedSubsystem,
		bool& bOutApprovedPatternFound,
		FString& OutApprovedPatternKey);

	static void SetTestLedgerPathOverride(const FString& InPath);
	static void ClearTestLedgerPathOverride();
};

namespace OsvayderUECanonRouting
{
	FAgentCanonExecution BuildInitialCanonExecution(
		const FString& Prompt,
		EAgentExecutionRunProfile Profile,
		TArray<FAgentPromptContextBlock>& InOutContextBlocks);

	void ApplyRuntimeToolExposure(FAgentRequestConfig& Config);
	void UpdateFromToolUse(
		FAgentCanonExecution& Execution,
		const FString& ToolName,
		const FString& ToolInput = FString(),
		const FString& PreMutationAssistantText = FString());
	void MarkPolicyDeny(FAgentCanonExecution& Execution);
	void Finalize(FAgentCanonExecution& Execution, bool bSuccess, bool bTimedOut);
	FString DetermineToolFamily(
		const FAgentCanonExecution& Execution,
		const FString& ToolName,
		const FString& ToolInput = FString());
	bool IsMutatingToolUse(const FString& ToolName, const FString& ToolInput = FString());
	TSharedPtr<FJsonObject> MakeCanonExecutionJson(const FAgentCanonExecution& Execution);
	TSharedPtr<FJsonObject> ExtractCanonExecutionFromTraceRecords(const TArray<TSharedPtr<FJsonObject>>& TraceRecords);
}
