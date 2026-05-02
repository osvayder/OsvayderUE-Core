// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"
#include "Dom/JsonObject.h"

struct UNREALCLAUDE_API FAgentTraceQueryOptions
{
	FString RunId;
	FString EventType;
	FString Backend;
	int32 Count = 60;
	bool bLatestOnly = true;
	bool bIncludeRawJson = false;
	int32 PreviewChars = 800;
};

struct UNREALCLAUDE_API FAgentTransportFailureSummary
{
	FString RunId;
	FString FailureFamily;
	FString FailureMessage;
	bool bRetrySafe = false;
	bool bRetryAttempted = false;
	FString RetryBlockReason;
	int32 ObservedToolUseCount = 0;
	bool bObservedMutatingTool = false;
	bool bObservedVerificationTool = false;
	bool bObservedRestartSurvivalSignal = false;

	bool HasAnySignal() const
	{
		return !RunId.IsEmpty() || !FailureFamily.IsEmpty() || !FailureMessage.IsEmpty();
	}
};

/**
 * Persistent observable execution trace for backend chat runs and shared-widget session actions.
 *
 * Important: this stores only observable runtime/tool events and explicit UI/session actions.
 * It does not attempt to capture hidden model reasoning or private provider-internal logic.
 */
class UNREALCLAUDE_API FUnrealClaudeAgentTraceLog
{
public:
	static FUnrealClaudeAgentTraceLog& Get();

	FString BeginRun(
		const FAgentBackendStatus& Status,
		const FAgentRequestConfig& Config,
		const FString& UserPrompt,
		bool bIncludeEngineContext,
		bool bIncludeProjectContext);

	void AppendObservedEvent(const FString& RunId, const FAgentRunEvent& Event);

	void AppendEvent(
		const FString& EventType,
		EUnrealClaudeProviderBackend Backend,
		const TSharedPtr<FJsonObject>& Payload,
		const FString& RunId = FString());

	void LogBackendFailure(
		const FString& RunId,
		EUnrealClaudeProviderBackend Backend,
		const FString& Message,
		const FString& Stage);

	void MarkCancellation(EUnrealClaudeProviderBackend Backend, const FString& Detail);

	void CompleteRun(
		const FString& RunId,
		EUnrealClaudeProviderBackend Backend,
		const FString& Response,
		bool bSuccess);

	FString GetTraceLogPath() const;
	FString GetActiveRunIdForBackend(EUnrealClaudeProviderBackend Backend) const;
	bool TryGetActiveCanonExecutionForBackend(EUnrealClaudeProviderBackend Backend, FAgentCanonExecution& OutExecution) const;
	bool IsRunMarkedCancelled(const FString& RunId) const;
	bool TryGetLatestTransportFailureSummary(EUnrealClaudeProviderBackend Backend, FAgentTransportFailureSummary& OutSummary) const;
	bool MarkTransportRetryAttempt(EUnrealClaudeProviderBackend Backend, const FString& Trigger);

	TArray<TSharedPtr<FJsonObject>> QueryEvents(
		const FAgentTraceQueryOptions& Options,
		FString& OutResolvedRunId,
		int32& OutTotalLoaded) const;

private:
	struct FAgentTraceRecord
	{
		FString EventId;
		FString RunId;
		FString EventType;
		FDateTime Timestamp;
		EUnrealClaudeProviderBackend Backend = EUnrealClaudeProviderBackend::ClaudeCli;
		TSharedPtr<FJsonObject> Payload;
	};

	struct FAgentTraceActiveRunState
	{
		FString RunId;
		EUnrealClaudeProviderBackend Backend = EUnrealClaudeProviderBackend::ClaudeCli;
		FString BackendDisplayName;
		FDateTime StartedAt;
		double StartedSeconds = 0.0;
		FString ProviderSessionId;
		bool bCancelled = false;
		bool bFirstMutatingToolObserved = false;
		FString PreMutationAssistantText;
		FAgentCanonExecution CanonExecution;
		int32 ObservedToolUseCount = 0;
		bool bObservedMutatingTool = false;
		bool bObservedVerificationTool = false;
		bool bObservedRestartSurvivalSignal = false;
		bool bIsTransportRetryReplay = false;
		FString TransportRetrySourceRunId;
		FString TransportFailureFamily;
		FString TransportFailureMessage;
		bool bTransportRetrySafe = false;
		bool bTransportRetryAttempted = false;
		FString TransportRetryBlockReason;
	};

	FUnrealClaudeAgentTraceLog() = default;

	TSharedPtr<FJsonObject> BuildRunMetadataObject(
		const FAgentBackendStatus& Status,
		const FAgentRequestConfig& Config,
		bool bIncludeEngineContext,
		bool bIncludeProjectContext) const;

	TSharedPtr<FJsonObject> MakeTraceRecordObject(const FAgentTraceRecord& Record) const;
	void StoreAndPersistRecord(const FAgentTraceRecord& Record);
	void PersistRecord(const FAgentTraceRecord& Record) const;
	TArray<TSharedPtr<FJsonObject>> LoadPersistedRecords() const;
	FString MakeRunId() const;
	FString MakeEventId() const;
	static bool IsTimeoutMessage(const FString& Message);

	mutable FCriticalSection Mutex;
	TArray<FAgentTraceRecord> RecentRecords;
	TMap<FString, FAgentTraceActiveRunState> ActiveRuns;
	TMap<uint8, FString> ActiveRunIdsByBackend;
	TMap<uint8, FAgentTransportFailureSummary> LatestTransportFailureByBackend;
};
