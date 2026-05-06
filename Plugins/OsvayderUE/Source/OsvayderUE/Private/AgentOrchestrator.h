// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentExecutionControl.h"
#include "AgentBackendTypes.h"

class IAgentBackend;
class IOsvayderRunner;
class FOsvayderSessionManager;
struct FOsvayderUEActivePlan;

struct FAgentPromptOptions
{
	bool bIncludeEngineContext = true;
	bool bIncludeProjectContext = true;
	EAgentExecutionRunProfile ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	FOnAgentProgress OnProgress;
	FOnAgentStreamEvent OnStreamEvent;
	TArray<FString> AttachedImagePaths;
	bool bIsTransportRetryReplay = false;
	FString TransportRetrySourceRunId;
	FString TransportRetryFailureFamily;
	FString VisibleHistoryPromptOverride;
};

class FAgentOrchestrator
{
public:
	FAgentOrchestrator();
	~FAgentOrchestrator();

	void SendPrompt(
		const FString& Prompt,
		FOnAgentResponse OnComplete,
		const FAgentPromptOptions& Options
	);

	FString GetUE57SystemPrompt() const;
	FString GetProjectContextPrompt() const;
	void SetCustomSystemPrompt(const FString& InCustomPrompt);

	/**
	 * 626 P6-prep wiring: stash a one-shot policy-routing advisory that the
	 * NEXT BuildRequestConfig will consume into the system prompt as a
	 * dedicated `[POLICY ROUTING ADVISORY]` context block. Widget-side
	 * `TryHandleCompileIntentPolicy` calls this when the P3 policy gate
	 * returns a non-Allow classification so the agent observes the decision
	 * in the next turn's context rather than only in log output.
	 * Single-slot queue (last-write-wins); consumed + cleared on next Build.
	 */
	void SetPendingPolicyRoutingAdvisory(const FString& AdvisoryText);

	/**
	 * 626 P6-prep wiring: consume + clear the pending advisory. Returns empty
	 * string if no advisory queued. Called from BuildRequestConfig.
	 */
	FString ConsumePendingPolicyRoutingAdvisory();

	/**
	 * 632 Task Recovery & Rehydration: single-shot `[TASK RECOVERY]` context
	 * block queued for the NEXT BuildRequestConfig. Widget-side `Continue`
	 * button calls this with the deterministic recovery summary + verbatim
	 * original_user_task so the agent observes the interrupted task intent
	 * in its next turn's system prompt. Single-slot last-write-wins (rare
	 * double-write is safe — most-recent wins; older remains in agent_trace
	 * for forensic record). Consumed and cleared on next BuildRequestConfig.
	 */
	void SetPendingTaskRecoveryContext(const FString& ContextText);

	/** 632 wiring: consume + clear. Returns empty string if none queued. */
	FString ConsumePendingTaskRecoveryContext();

	const TArray<TPair<FString, FString>>& GetHistory() const;
	void ClearHistory();
	void CancelCurrentRequest();
	bool SaveSession();
	bool LoadSession();
	FAgentSessionRestoreResult LoadSessionWithResult();
	FAgentSavedSessionIndex DescribeSavedSessions() const;
	FAgentSavedSessionIndex DescribeSavedSessionsForProfile(EAgentExecutionRunProfile Profile) const;
	bool HasSavedSession() const;
	FString GetSessionFilePath() const;
	FString GetSessionFilePathForProfile(EAgentExecutionRunProfile Profile) const;

	EOsvayderUEProviderBackend GetConfiguredBackend() const;
	FAgentBackendStatus GetConfiguredBackendStatus() const;
	FAgentProviderExecutionControlManifest GetConfiguredBackendExecutionControlManifest() const;
	FAgentProviderExecutionControlManifest GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile Profile) const;
	TArray<FAgentBackendStatus> GetBackendStatuses() const;
	IAgentBackend* GetActiveBackend() const;
	IOsvayderRunner* GetClaudeRunner() const;
	static bool ShouldResumeFeatureWorkflowFromActivePlan(const FOsvayderUEActivePlan& ActivePlan);

private:
	IAgentBackend* GetOrCreateBackend(EOsvayderUEProviderBackend Backend) const;
	FAgentRequestConfig BuildExecutionControlBaselineConfig(EAgentExecutionRunProfile Profile) const;
	FAgentRequestConfig BuildRequestConfig(const FString& Prompt, const FAgentPromptOptions& Options) const;
	bool ProfileUsesNormalProviderSession(EAgentExecutionRunProfile Profile) const;
	bool ProfileUsesProjectLocalVisibleRestore(EAgentExecutionRunProfile Profile) const;
	FAgentSavedSessionIndex BuildProfileScopedSavedSessions(EAgentExecutionRunProfile Profile) const;
	FAgentSessionRestoreResult BuildProfileScopedRestoreResult(EAgentExecutionRunProfile Profile) const;
	FString BuildConversationBootstrapText(EAgentExecutionRunProfile Profile) const;
	FString BuildPromptWithHistory(const FString& NewPrompt, EAgentExecutionRunProfile Profile) const;

	mutable TUniquePtr<IAgentBackend> ClaudeBackend;
	mutable TUniquePtr<IAgentBackend> CodexBackend;
	TUniquePtr<FOsvayderSessionManager> SessionManager;
	FString CustomSystemPrompt;

	/** 626 P6-prep: one-shot policy-routing advisory cache (see SetPendingPolicyRoutingAdvisory). */
	mutable FString PendingPolicyRoutingAdvisory;

	/** 632: one-shot task-recovery context cache (see SetPendingTaskRecoveryContext). */
	mutable FString PendingTaskRecoveryContext;
};
