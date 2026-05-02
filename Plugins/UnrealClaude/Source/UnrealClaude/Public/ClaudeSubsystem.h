// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentExecutionControl.h"
#include "IClaudeRunner.h"

class FAgentOrchestrator;

/**
 * Options for sending a prompt to the configured backend
 * Reduces parameter count in SendPrompt method
 */
struct UNREALCLAUDE_API FClaudePromptOptions
{
	/** Include UE5.7 engine context in system prompt */
	bool bIncludeEngineContext = true;

	/** Include project-specific context in system prompt */
	bool bIncludeProjectContext = true;

	/** Execution profile for this run */
	EAgentExecutionRunProfile ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;

	/** Optional callback for streaming output progress */
	FOnClaudeProgress OnProgress;

	/** Optional callback for structured NDJSON stream events */
	FOnClaudeStreamEvent OnStreamEvent;

	/** Optional paths to attached clipboard images (PNG) */
	TArray<FString> AttachedImagePaths;

	/** Marks a bounded replay of the exact last prompt after a transport reset */
	bool bIsTransportRetryReplay = false;

	/** Source run id for the transport-reset replay, when known */
	FString TransportRetrySourceRunId;

	/** Failure family that triggered the transport-reset replay */
	FString TransportRetryFailureFamily;

	/** Optional visible-history prompt marker when the dispatched turn is system-owned */
	FString VisibleHistoryPromptOverride;

	/** Default constructor with sensible defaults */
	FClaudePromptOptions() = default;

	/** Convenience constructor for common case */
	FClaudePromptOptions(bool bEngineContext, bool bProjectContext)
		: bIncludeEngineContext(bEngineContext)
		, bIncludeProjectContext(bProjectContext)
	{}
};

/**
 * Legacy compatibility subsystem for the editor assistant runtime.
 * Orchestration now routes through a provider-neutral backend layer.
 */
class UNREALCLAUDE_API FClaudeCodeSubsystem
{
public:
	static FClaudeCodeSubsystem& Get();

	/** Destructor - must be defined in cpp where full types are available */
	~FClaudeCodeSubsystem();

	/** Send a prompt to Claude with optional context (new API with options struct) */
	void SendPrompt(
		const FString& Prompt,
		FOnClaudeResponse OnComplete,
		const FClaudePromptOptions& Options = FClaudePromptOptions()
	);

	/** Send a prompt to Claude with optional context (legacy API for backward compatibility) */
	void SendPrompt(
		const FString& Prompt,
		FOnClaudeResponse OnComplete,
		bool bIncludeUE57Context,
		FOnClaudeProgress OnProgress,
		bool bIncludeProjectContext = true
	);

	/** Get the default UE5.7 system prompt */
	FString GetUE57SystemPrompt() const;

	/** Get the project context prompt */
	FString GetProjectContextPrompt() const;

	/** Set custom system prompt additions */
	void SetCustomSystemPrompt(const FString& InCustomPrompt);

	/**
	 * 626 P6-prep wiring: stash a one-shot policy-routing advisory to be
	 * consumed into the NEXT turn's system prompt as a dedicated
	 * `[POLICY ROUTING ADVISORY]` context block. Called by the widget-side
	 * compile-intent policy gate hook when a non-Allow decision fires so
	 * the agent observes the decision in its next-turn context.
	 * Last-write-wins single slot.
	 */
	void SetPendingPolicyRoutingAdvisory(const FString& AdvisoryText);

	/**
	 * 632 Task Recovery & Rehydration: stash a one-shot `[TASK RECOVERY]`
	 * context block to be consumed into the NEXT turn's system prompt.
	 * Called by the widget-side recovery dialog's "Continue" button so the
	 * agent observes the interrupted task intent in its next-turn context.
	 * Last-write-wins single slot.
	 */
	void SetPendingTaskRecoveryContext(const FString& ContextText);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test-only readback for the single-shot task recovery context queue. */
	FString ConsumePendingTaskRecoveryContextForTests();
#endif

	/** Get conversation history (delegates to session manager) */
	const TArray<TPair<FString, FString>>& GetHistory() const;

	/** Clear conversation history */
	void ClearHistory();

	/** Cancel current request */
	void CancelCurrentRequest();

	/** Save current session to disk */
	bool SaveSession();

	/** Load previous session from disk */
	bool LoadSession();

	/** Load previous session from disk with detailed provider-safe result */
	FAgentSessionRestoreResult LoadSessionWithResult();

	/** Describe saved sessions for the current provider, the alternate provider, and the legacy shared file */
	FAgentSavedSessionIndex DescribeSavedSessions() const;

	/** Describe saved sessions as seen from a specific execution profile boundary */
	FAgentSavedSessionIndex DescribeSavedSessionsForProfile(EAgentExecutionRunProfile Profile) const;

	/** Check if a previous session exists */
	bool HasSavedSession() const;

	/** Get session file path */
	FString GetSessionFilePath() const;

	/** Get session file path for a specific execution profile boundary */
	FString GetSessionFilePathForProfile(EAgentExecutionRunProfile Profile) const;

	/** Get Claude runner interface when Claude backend is active (legacy/testing use only) */
	IClaudeRunner* GetRunner() const;

	/** Get the active backend interface */
	IAgentBackend* GetActiveBackend() const;

	/** Get the backend selected in settings */
	EUnrealClaudeProviderBackend GetConfiguredBackend() const;

	/** Get the status of the configured backend */
	FAgentBackendStatus GetConfiguredBackendStatus() const;

	/** Get the machine-readable execution control manifest for the configured backend */
	FAgentProviderExecutionControlManifest GetConfiguredBackendExecutionControlManifest() const;

	/** Get the machine-readable execution control manifest for a specific execution profile */
	FAgentProviderExecutionControlManifest GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile Profile) const;

	/** Get statuses for all known backends */
	TArray<FAgentBackendStatus> GetBackendStatuses() const;

private:
	FClaudeCodeSubsystem();

	TUniquePtr<FAgentOrchestrator> Orchestrator;
};
