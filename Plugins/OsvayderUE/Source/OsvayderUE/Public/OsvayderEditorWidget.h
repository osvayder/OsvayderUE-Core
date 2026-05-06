// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IOsvayderRunner.h"
#include "OsvayderUERelayAgent.h"
#include "OsvayderUERestartSurvival.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SMultiLineEditableTextBox;
class SMultiLineEditableText;
class SScrollBox;
class SVerticalBox;
class SOsvayderInputArea;
class SExpandableArea;
class STextBlock;
class FOsvayderUEVoiceDictationService;
struct FOsvayderUEVoiceDictationStatus;
struct FClaudeRestartSurvivalStartOptions
{
	FString TaskIdOverride;
	FString SessionIdOverride;
	FString ReattachTokenOverride;
	FString ReattachNoticeOverride;
	FString AdditionalRelaunchArguments;
	bool bRequestEditorExit = true;
	/**
	 * 624 fix: when true the start path skips the "already waiting for response"
	 * early return so a blocker escalation can spawn the detached owner
	 * synchronously from inside the Codex turn itself, before the transport is
	 * reset. Normal toolbar / post-response callers leave this false.
	 */
	bool bBypassWaitingForResponseGuard = false;
	FOsvayderUERestartSurvivalRestoreIntent RestoreIntent;
	FOsvayderUERestartSurvivalFileWriteIntent FileWriteIntent;
	FOsvayderUERestartSurvivalProofState Proof;
};

struct OSVAYDERUE_API FOsvayderUEActivePlanCloseoutDecision
{
	FString PlanStatus;
	FString ResultStatus;
	FString GateReasonCode;
	FString SourcePlanId;
	FString SourceArchivePath;
	FString SourceRunId;
	FString SourceFeatureWorkflowId;
	FString SourceProjectRoot;
	FString SourceRecipeId;
	FString SourceRoleId;
	int32 EvidenceSchemaVersion = 0;
	bool bFeatureWorkflowRequired = false;
	bool bRecipeContractResolved = false;
	bool bRoleContractResolved = false;
	bool bRuntimeProofRequired = false;
	bool bRuntimeProofPassed = false;
	bool bRuntimeProofFailed = false;
	bool bStopLossTriggered = false;
	bool bProofPrerequisitesMissing = false;
	bool bAutomationDiscoveryFailed = false;
	bool bAuthoringLaneDenied = false;
	bool bCompileProofRequired = false;
	bool bFreshCompileProofObserved = false;
	bool bFreshCompileFailureObserved = false;
	bool bCurrentClosedEditorProofObserved = false;
	bool bPostCompileVerificationPassed = false;
	bool bPostCompileVerificationFailed = false;
	bool bManagedStateManualWriteDetected = false;
	FString StopLossReason;
	FString BlockerFamily;
	FString BlockerDetail;
	TArray<FString> ConsumedFactIds;
	TArray<FString> MissingRecipeObligations;
};

struct OSVAYDERUE_API FOsvayderUECloseoutFactSnapshot
{
	bool bKnownProofMapAvailable = false;
	bool bProofInputMappingAvailable = false;
	bool bPlacedRuntimeActorsAvailable = false;
	bool bReducedProofModeAllowed = false;
	bool bAttemptResolverSourceObserved = false;
	bool bEventSubsystemSourceObserved = false;
	bool bRuntimeSmokeSuccessObserved = false;
	bool bPrisonAccessEventObserved = false;
	int32 AutomationDiscoveryCount = INDEX_NONE;
	int32 AutomationExecutedCount = INDEX_NONE;
	int32 AutomationPassedCount = INDEX_NONE;
	int32 AutomationFailedCount = INDEX_NONE;
	bool bRuntimeProofPassed = false;
	bool bCompileProofPassed = false;
	bool bCommandMutationDetected = false;
	bool bManagedStateManualWriteDetected = false;
	bool bLocalAnimationPackIntakeRequired = false;
	bool bLocalAnimationPackIntakeSucceeded = false;
	bool bAnimationPreflightObserved = false;
	bool bAnimationPreflightSkeletonMismatchObserved = false;
	bool bAnimationRetargetFixupSucceeded = false;
	bool bPostRetargetCompatibleAnimationPreflightObserved = false;
	bool bAnimationLineageRoleEvidencePresent = false;
	bool bAnimationLineagePathEvidencePresent = false;
	bool bAnimationLineagePathCheckApplied = false;
	TArray<FString> RequiredAnimationRoles;
	TArray<FString> CompatiblePostRetargetAnimationRoles;
	TArray<FString> MissingAnimationLineageRoles;
	TArray<FString> AnimationLineagePathMismatchRoles;
	TArray<FString> GeneratedRetargetDestinationPaths;
	TMap<FString, FString> RequiredAnimationRoleSourcePaths;
	TMap<FString, FString> RetargetGeneratedDestinationsBySource;
	TMap<FString, FString> RetargetGeneratedDestinationsByRole;
	TMap<FString, FString> CompatibleAnimationRolePaths;
	TArray<FString> ConsumedFactIds;
	TArray<FString> CommandMutationFactIds;
	TArray<FString> ManagedStateManualWriteFactIds;
	TArray<FString> AnimationWorkflowFactIds;
	TArray<FString> ExecutionTruthDecisionSummaries;

	void AddFactId(const FString& FactId);
	void MergeFrom(const FOsvayderUECloseoutFactSnapshot& Other);
	bool HasRuntimePrerequisiteFacts() const;
	bool HasCompleteInteractionAccessObservation() const;
	bool HasBoundedPrisonAccessAutomationProof() const;
	bool HasBoundedInteractionAccessRuntimeProof() const;
	TSharedPtr<FJsonObject> ToJsonObject() const;
};

struct OSVAYDERUE_API FOsvayderUERunCloseoutContext
{
	FString RunId;
	FString PlanId;
	FString FeatureWorkflowId;
	FString RecipeId;
	FString RoleId;
	int32 EvidenceSchemaVersion = 0;
	FString ProjectRoot;
	FString StartedAtUtc;
	FString CompletedAtUtc;
	FOsvayderUECloseoutFactSnapshot Facts;
	TArray<FString> IncludedTraceEventIds;
	TArray<FString> ExcludedTraceEventIds;

	TSharedPtr<FJsonObject> ToJsonObject() const;
};

struct OSVAYDERUE_API FOsvayderUEHeadlessAcceptanceRequest
{
	FString PromptFile;
	FString Prefix;
	int32 TimeoutSec = 900;
	FString OutputDir;
	FString TriggerPath;
	bool bLocalDevOptIn = false;
	bool bRequestEditorExitOnComplete = false;
	bool bVisibleManualEmulator = false;
	bool bRequireVisibleEditor = false;
};

struct OSVAYDERUE_API FOsvayderUEHeadlessAcceptanceReceiptContext
{
	FOsvayderUEHeadlessAcceptanceRequest Request;
	FString ReceiptPath;
	FString PromptHash;
	FString StartedAtUtc;
	FString CompletedAtUtc;
	FString DispatchPath;
	FString Status;
	FString FailureReason;
	FString ActiveRunIdFallback;
	FString LatestCloseoutPath;
	FString ArchivePath;
	FString VisibleSessionPath;
	FString TracePath;
	TArray<FString> LogPaths;
	bool bAssistantSuccess = false;
	bool bHasCloseoutDecision = false;
	FOsvayderUEActivePlanCloseoutDecision CloseoutDecision;
};

struct OSVAYDERUE_API FTaskRecoveryAutoDispatchDecision
{
	bool bCanAutoDispatch = false;
	FString BlockReasonCode;
	FString UserFacingMessage;
};

struct OSVAYDERUE_API FOpenEditorBuildLockCloseSafetyInputs
{
	int32 DirtyPackageCount = 0;
	FString DirtyPackageSummary;
	bool bPlaySessionActive = false;
	bool bModalWindowActive = false;
	FString ModalWindowTitle;
	bool bRestartSurvivalStateActive = false;
	FString RestartSurvivalPhase;
};

struct OSVAYDERUE_API FOpenEditorBuildLockCloseSafetyDecision
{
	bool bCanAutoClose = false;
	FString BlockReasonCode;
	FString UserFacingMessage;
};

/**
 * Chat message display widget
 */
class SChatMessage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SChatMessage)
		: _IsUser(true)
		, _AssistantLabel(FString(TEXT("Assistant")))
	{}
		SLATE_ARGUMENT(FString, Message)
		SLATE_ARGUMENT(bool, IsUser)
		SLATE_ARGUMENT(FString, AssistantLabel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

/**
 * Main Claude chat widget for the editor
 */
class OSVAYDERUE_API SOsvayderEditorWidget : public SCompoundWidget
{
public:
	enum class EBackendRuntimeConnectionState : uint8
	{
		Unknown,
		Connecting,
		Connected,
		Failed,
		TransportReset
	};

	SLATE_BEGIN_ARGS(SOsvayderEditorWidget)
	{}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SOsvayderEditorWidget();
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	static void RegisterLiveWidget(const TSharedRef<SOsvayderEditorWidget>& Widget);
	static TSharedPtr<SOsvayderEditorWidget> GetLiveWidget();

	bool DebugStartDictation();
	bool DebugStopDictation();
	bool DebugToggleDictation();
	void DebugClearInputText();
	void DebugSetInputText(const FString& NewText);
	bool DebugSendCurrentInput();
	bool DebugIsWaitingForResponse() const;
	void DebugForceBackendSnapshotRefresh(bool bForce = true);
	void DebugProcessBackendStateNow();
	int32 DebugGetVisibleMessageCount() const;
	bool DebugVisibleChatContains(const FString& Needle) const;
	EBackendRuntimeConnectionState DebugGetLastRuntimeConnectionState() const;
	FString DebugGetLastRuntimeConnectionDetail() const;
	FString DebugGetLastResponseText() const;
	bool StartRestartSurvivalForCurrentTask(const FClaudeRestartSurvivalStartOptions& Options, FString& OutError);
	bool StartHeadlessNewSessionAcceptanceBridge(const FOsvayderUEHeadlessAcceptanceRequest& Request, FString& OutError);
	static FString NormalizeHeadlessAcceptancePathArgument(const FString& PathArgument);
	static void NormalizeHeadlessAcceptanceRequestPaths(FOsvayderUEHeadlessAcceptanceRequest& Request);
	static bool ValidateHeadlessAcceptanceRequest(const FOsvayderUEHeadlessAcceptanceRequest& Request, FString& OutError);
	static FString ComputeHeadlessAcceptancePromptHash(const FString& Prompt);
	static FString ResolveHeadlessAcceptanceReceiptPath(const FOsvayderUEHeadlessAcceptanceRequest& Request);
	static void NormalizeMandatoryContextFlagsForNormalAssistantRun(
		bool& bInOutIncludeUE57Context,
		bool& bInOutIncludeProjectContext);
	static TSharedPtr<FJsonObject> BuildHeadlessAcceptanceReceiptJson(
		const FOsvayderUEHeadlessAcceptanceReceiptContext& Context);
	static bool SaveHeadlessAcceptanceReceiptArtifact(
		const FOsvayderUEHeadlessAcceptanceReceiptContext& Context,
		FString& OutError);

	/**
	 * 624 v3: pure helper that returns the FClaudeRestartSurvivalStartOptions the
	 * closed-editor build-blocker arm path must use. Exposed as a public static
	 * so the regression test OsvayderUE.RestartSurvival.ArmEmissionPreservesEditorExitIntent
	 * can call it directly and assert bRequestEditorExit == true without needing
	 * a live widget instance. MUST keep bRequestEditorExit=true so the
	 * FPlatformMisc::RequestExit ticker inside StartRestartSurvivalForCurrentTask
	 * at :2069 runs — the PS supervisor polls editor_process_id and waits for the
	 * editor to exit; nothing else in the product asks the editor to exit.
	 */
	static FClaudeRestartSurvivalStartOptions BuildArmOptionsForClosedEditorBuildBlocker();

	/**
	 * 606 Part B (2026-04-18): pure helper for the outside-editor continuation
	 * banner visibility decision. The banner warns the user that the PS
	 * supervisor is still alive for a task even though the monitor window is
	 * hidden (closed, minimized, off-screen) — addresses the "мы так сможем
	 * забыть его и он будет жрать память" user concern journaled 2026-04-18 07:50.
	 *
	 * Returns true when all the following hold:
	 *   1. The provided RestartState came from a successful LoadState (caller
	 *      pre-condition — this helper does not re-read disk).
	 *   2. RestartState.Phase is NOT terminal — rejects Reattached, FailedTerminal,
	 *      AttachedInEditor.
	 *   3. Either DetachedOwnerProcessId is not yet populated (early Detaching
	 *      window before CreateProc returns an owner pid) OR the caller confirms
	 *      the owner process is alive (bDetachedOwnerProcessRunning).
	 *
	 * Exposed as a public static so the regression test
	 * OsvayderUE.Widget.Agent2BackgroundBannerVisibilityDecision can exercise
	 * the decision table across phases without a live widget.
	 */
	static bool ShouldShowAgent2BackgroundBanner(
		const FOsvayderUERestartSurvivalState& RestartState,
		bool bDetachedOwnerProcessRunning);

	/**
	 * 606 Part B: pure helper that produces the Cancel-button payload the
	 * Agent 2 banner writes to relay_cancel_request.json. Same schema as the
	 * monitor-window Cancel (schema_version=1, requested_at_utc, reason), only
	 * the reason string differs for audit (user_requested_cancel_from_widget_banner).
	 * Exposed static for the automation test that exercises the click path.
	 */
	static FString BuildAgent2BannerCancelRequestPayload();
	static FString DescribeTransportRetryBlockReason(const FString& RetryBlockReason);
	static FString BuildTransportRetryStatusLabel(bool bRetrySafe);
	static FString BuildTransportRetryNotice(const FString& BackendDisplayName, bool bRetrySafe, const FString& RetryBlockReason);
	static FString BuildTaskRecoveryAutoResumePrompt();
	static FString BuildTaskRecoveryAutoResumeVisibleMarker();
	static FString DescribeTaskLaneStatus(const FOsvayderUETaskLaneState& LaneState);
	static FOpenEditorBuildLockCloseSafetyDecision EvaluateOpenEditorBuildLockCloseSafety(
		const FOpenEditorBuildLockCloseSafetyInputs& Inputs);
	static FTaskRecoveryAutoDispatchDecision EvaluateTaskRecoveryAutoDispatch(
		bool bIsWaitingForResponse,
		bool bBackendCanSendPrompt,
		const FString& BackendBlockedDetail,
		bool bUnsafeTransportRetry,
		const FString& TransportRetryBlockReason);
#if WITH_DEV_AUTOMATION_TESTS
	static bool PromptHasExplicitContinuityRefsForTests(const FString& Prompt);
	static FString DescribeExplicitContinuityRefsForTests(const FString& Prompt);
#endif

private:
	/** UI Construction */
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildCodexAuthDiagnosticsPanel();
	TSharedRef<SWidget> BuildChatArea();
	TSharedRef<SWidget> BuildInputArea();
	TSharedRef<SWidget> BuildStatusBar();
	
	/** Add a message to the chat display */
	void AddMessage(const FString& Message, bool bIsUser);
	int32 GetDeferredTasksCount() const;
	void OpenDeferredTasksBrowser();

	/** Mirror a committed visible message into the bounded proof/readback store */
	void TrackVisibleChatMessage(const FString& Message);

	/** Replace the active streaming bubble with one final assistant message */
	void ReplaceStreamingResponseWithFinalText(const FString& Message);
	
	/** Add streaming response (appends to last assistant message) */
	void AppendToLastResponse(const FString& Text);
	
	/** Send the current input to Claude */
	void SendMessage();

	/** Dispatch one prepared prompt to the backend with optional visible-history override */
	bool DispatchPreparedPrompt(
		const FString& Prompt,
		const TArray<FString>& ImagePaths,
		bool bIsTransportRetryReplay,
		const FString& TransportRetrySourceRunId,
		const FString& TransportRetryFailureFamily,
		const FString& VisibleHistoryPromptOverride,
		bool bClearInputWidget,
		FString& OutError);

	/** Start or stop the shared dictation flow bound to the input area */
	void ToggleDictation();

	/** Cancel current request */
	void CancelRequest();

	/** Copy selected text or last response */
	void CopyToClipboard();

	/** Restore prior saved session history from disk */
	void RestoreSession();

	/** Start a fresh session by clearing visible chat, saved history, and backend conversation state */
	void NewSession();

	/** Clear the bounded transport-failure replay state after success, reset, or provider switch */
	void ClearTransportRetryState();

	/** Restore current-provider history from memory or disk with provider-safe messaging */
	bool RestoreCurrentBackendSession(bool bAutomatic, const FString& LeadingNotice = FString());

	/** Restore visible chat history automatically when the widget opens, if configured. */
	bool TryRestoreSessionOnOpen();

	/** Reset visible chat widgets and local response state without mutating provider history */
	void ResetVisibleChatState();

	/** Rebind visible chat/session state after the configured provider changes */
	void HandleConfiguredBackendChanged(EOsvayderUEProviderBackend PreviousBackend);

	/** Render a saved conversation history into the chat area with optional wrapper notices. */
	void RenderConversationHistory(const TArray<TPair<FString, FString>>& History, const FString& IntroMessage, const FString& OutroMessage);

	/** Launch Codex Browser Verify login flow from the editor UI */
	void LaunchBrowserVerifyLogin();

	/** Open the effective Codex auth folder used by plugin launches */
	void OpenCodexAuthFolder();

	/** Launch Codex relogin using the same effective CODEX_HOME as plugin launches */
	void LaunchCodexRelogin();

	/** Explicitly back up and clear known stale Codex auth artifacts */
	void BackupAndClearCodexAuth();

	/** Probe Codex backend auth without sending a user prompt */
	void ProbeCodexBackendAuth();

	/** Start restart survival from the ordinary toolbar product path */
	void StartRestartSurvival();

	/** Auto-dispatch one bounded post-reattach completion round if restart survival prepared it */
	void TryDispatchPendingRestartSurvivalCompletion();

	/** Poll one active detached owner so manual reopen and crash/user-return truth stay inside the same product loop. */
	void PollRestartSurvivalDetachedOwnerState();

	/** 606 Part B: build the outside-editor continuation banner Slate sub-tree. */
	TSharedRef<SWidget> BuildAgent2BackgroundBanner();

	/** 606 Part B: visibility binding — reflects Agent2BannerState.bVisible. */
	EVisibility GetAgent2BackgroundBannerVisibility() const;

	/** 606 Part B: banner text binding for the current lane-centric outside-editor status. */
	FText GetAgent2BackgroundBannerText() const;

	/** 606 Part B: "Show Monitor" button click handler — CreateProc the monitor script. */
	FReply HandleAgent2BannerShowMonitor();

	/** 606 Part B: "Cancel" button click handler — write relay_cancel_request.json. */
	FReply HandleAgent2BannerCancel();

	/**
	 * 606 Part B: update the cached banner display data from the latest restart-survival
	 * state snapshot. Called once per polling tick from PollRestartSurvivalDetachedOwnerState.
	 * Passing nullptr clears the state (banner becomes invisible).
	 */
	void UpdateAgent2BannerStateFromRestartState(
		const FOsvayderUERestartSurvivalState* RestartState,
		bool bDetachedOwnerProcessRunning);

	/** Consume one structured task-driven restart-survival handoff after a successful normal response */
	void TryStartPendingRestartSurvivalAfterResponse(EOsvayderUEProviderBackend RequestBackend);

	/**
	 * 624 fix: spawn the detached restart-survival owner synchronously from inside
	 * TryHandleClosedEditorBuildBlocker, before the Codex transport is reset, so the
	 * supervisor is already alive when the arm signal propagates. Returns true on
	 * spawn success and writes a detail message into OutError on failure.
	 */
	bool StartRestartSurvivalForClosedEditorBuildBlocker(FString& OutError);

	/** Detect one truthful closed-editor build blocker from streamed tool results and interrupt before timeout. */
	void TryHandleClosedEditorBuildBlocker(const FOsvayderStreamEvent& Event, EOsvayderUEProviderBackend RequestBackend);

	/**
	 * 631 Agent self-retrospective policy: plugin-side trust-calibration check.
	 *
	 * Inspects the final agent response for a `status: full` claim + cross-
	 * references the run's `FAgentCanonExecution.bSelfVerificationAttempted`
	 * flag (set by `FOsvayderUEAgentTraceLog::AppendObservedEvent` on the
	 * ToolResult path when an empirical verification tool was invoked).
	 *
	 * Mismatch (agent claims `full` but no verification-tool invocation was
	 * observed in the run's trace) triggers a warning chat-bubble so the user
	 * sees the trust gap before trusting the claim.
	 *
	 * Pure check — NEVER overrides the agent's text, NEVER fabricates evidence.
	 */
	void CheckSelfVerificationMismatchAndWarn(const FString& FinalResponse, EOsvayderUEProviderBackend RequestBackend);

public:
	/**
	 * 631/649 closeout trust policy: true if `Response` contains an explicit
	 * full-completion claim such as `status: full`, `result: full`, or the
	 * markdown section form `**Result**` + `full`.
	 */
	static bool DoesResponseClaimStatusFull(const FString& Response);

	/** Pure closeout policy for active_plan terminal status after Agent 1 responds. */
	static FOsvayderUEActivePlanCloseoutDecision EvaluateActivePlanCloseout(
		const FOsvayderUEActivePlan& Plan,
		bool bResponseSuccess);

	static FOsvayderUECloseoutFactSnapshot ExtractCloseoutFactsFromPlan(
		const FOsvayderUEActivePlan& Plan);

	static FOsvayderUECloseoutFactSnapshot ExtractAgentTraceFactsForCloseout(
		const TArray<TSharedPtr<FJsonObject>>& TraceRecords);

	static FOsvayderUECloseoutFactSnapshot ExtractCurrentAgentTraceFactsForCloseout();

	static FOsvayderUERunCloseoutContext BuildRunCloseoutContext(
		const FOsvayderUEActivePlan& Plan,
		const TArray<TSharedPtr<FJsonObject>>& TraceRecords,
		const FString& RunId,
		const FString& ProjectRoot);

	static FOsvayderUERunCloseoutContext BuildCurrentRunCloseoutContext(
		const FOsvayderUEActivePlan& Plan,
		const FString& PreferredRunId = FString());

	static FAgentFeatureWorkflowState ReduceFeatureWorkflowForCloseout(
		const FAgentFeatureWorkflowState& Workflow,
		const FOsvayderUECloseoutFactSnapshot& Facts);

	static FOsvayderUEActivePlanCloseoutDecision EvaluateActivePlanCloseoutWithContext(
		const FOsvayderUEActivePlan& Plan,
		bool bResponseSuccess,
		const FOsvayderUERunCloseoutContext& Context);

	static FOsvayderUEActivePlanCloseoutDecision EvaluateActivePlanCloseoutWithFacts(
		const FOsvayderUEActivePlan& Plan,
		bool bResponseSuccess,
		const FOsvayderUECloseoutFactSnapshot& Facts);

	static FOsvayderUEActivePlanCloseoutDecision EvaluateActivePlanCloseoutFromCurrentArtifacts(
		const FOsvayderUEActivePlan& Plan,
		bool bResponseSuccess);

	static TSharedPtr<FJsonObject> BuildCloseoutDecisionJson(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision& Decision,
		const FOsvayderUECloseoutFactSnapshot& Facts);

	static TSharedPtr<FJsonObject> BuildCloseoutDecisionJson(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision& Decision,
		const FOsvayderUERunCloseoutContext& Context);

	/** Pure warning surface for compile-gated closeout mismatches. */
	static FString BuildActivePlanCloseoutWarningText(
		const FOsvayderUEActivePlanCloseoutDecision& Decision,
		const FString& FinalResponse);

	static bool DoesPromptRequireVisualProof(
		const FString& Prompt,
		const TArray<FString>& ImagePaths = TArray<FString>());

	static bool DoesFinalResponseProvideVisualProof(const FString& Response);
	static bool DoesFinalResponseDeclareFailureOrBlocker(const FString& Response);

	static FOsvayderUEActivePlanCloseoutDecision ApplyActivePlanCloseoutSafetyGates(
		const FOsvayderUEActivePlan& Plan,
		const FString& Response,
		bool bResponseSuccess,
		bool bHasCurrentClosedEditorProof,
		FOsvayderUEActivePlanCloseoutDecision Decision);

	/**
	 * 649 truthful closeout policy: when plugin-tracked active-plan truth
	 * contradicts a final `full` claim, replace that closeout with a bounded,
	 * deterministic summary before it is rendered/saved.
	 */
	static FString RewriteResponseForTruthfulActivePlanCloseout(
		const FOsvayderUEActivePlan& Plan,
		const FOsvayderUEActivePlanCloseoutDecision& Decision,
		const FString& FinalResponse);

	/**
	 * 650 test seam: apply one feature-workflow tool boundary without a live widget.
	 * Used by focused automation coverage for reuse-aware phase advancement truth.
	 */
	static void ApplyFeatureWorkflowToolBoundaryForTest(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& ActiveCanon,
		const FOsvayderStreamEvent& Event,
		bool bStarting);

private:

	/**
	 * 626 P3 compile-intent policy gate (annotation-layer hook).
	 *
	 * Observes `command_execution` tool-result events and runs
	 * `FCompileIntentPolicyGate::EvaluateCompileIntent` with the current
	 * editor state / lane profile / user-intent text. Emits the policy
	 * routing decision to the UE log for transcript visibility when the
	 * decision is non-Allow. Does NOT rewrite tool calls; Codex owns
	 * command_execution dispatch entirely. See CompileIntentPolicyGate.h
	 * for rationale on the annotation-layer hook choice.
	 */
	void TryHandleCompileIntentPolicy(const FOsvayderStreamEvent& Event);

	/** Persist or resume the current active plan before dispatching the next request. */
	bool PrepareActivePlanForCurrentSend(const FString& Prompt, FString& OutError);

	/** Persist one tool-boundary update into the current active plan. */
	void UpdateActivePlanForToolBoundary(const FOsvayderStreamEvent& Event, bool bStarting);

	/** Mark the active plan as a justified hybrid split at the closed-editor blocker boundary. */
	void MarkActivePlanAwaitingClosedEditorRelay(
		const FOsvayderUEClosedEditorBuildBlocker& Blocker,
		const FString& ContinuationTaskId,
		const FString& EscalationReason,
		const FString& TechnicalDetail);

	/** Record that a closed-editor continuation was considered but blocked before it could arm. */
	void MarkActivePlanBlockedOnClosedEditorRelay(
		const FOsvayderUEClosedEditorBuildBlocker& Blocker,
		const FString& EscalationReason,
		const FString& TechnicalDetail);

	/** Mark the active plan for exact post-reattach verification resume. */
	void MarkActivePlanForPostReattachVerification(const FOsvayderUERestartSurvivalState& RestartState);

	/** Finalize, archive, and clear the active plan once the current task reaches a terminal user answer. */
	FOsvayderUEActivePlanCloseoutDecision FinalizeActivePlan(const FString& Response, bool bSuccess, bool bArchiveRelayArtifacts);

	/** Surface one truthful resume notice when a persisted active plan is waiting on startup. */
	void TrySurfacePersistedActivePlanNotice();

	/** Clear one pending closed-editor build blocker intervention after the active request settles. */
	void ClearPendingClosedEditorBuildBlockerIntervention();
	
	/** Handle response from Claude */
	void OnClaudeResponse(const FString& Response, bool bSuccess, uint64 RequestGeneration, EOsvayderUEProviderBackend RequestBackend);

	/** Handle streamed text only if it belongs to the still-active request generation */
	void OnClaudeProgress(const FString& PartialOutput, uint64 RequestGeneration, EOsvayderUEProviderBackend RequestBackend);

	/** Handle structured stream events only if they belong to the still-active request generation */
	void OnClaudeStreamEvent(const FOsvayderStreamEvent& Event, uint64 RequestGeneration, EOsvayderUEProviderBackend RequestBackend);
	
	/** Check if Claude CLI is available */
	bool IsClaudeAvailable() const;

	/** Check whether the configured backend can accept a prompt right now */
	bool CanSendPrompt() const;

	/** Whether the toolbar should expose the Browser Verify button */
	bool ShouldShowBrowserVerifyButton() const;

	/** Whether the Codex auth diagnostics panel should be visible */
	EVisibility GetCodexAuthDiagnosticsVisibility() const;

	/** Compact Codex auth diagnostics text shown before prompt send */
	FText GetCodexAuthDiagnosticsText() const;

	/** Detailed Codex auth diagnostics tooltip */
	FText GetCodexAuthDiagnosticsToolTip() const;

	/** Whether the backend auth probe can start now */
	bool CanProbeCodexBackendAuth() const;

	/** Whether Browser Verify can currently be launched from the widget */
	bool CanLaunchBrowserVerifyButton() const;

	/** Tooltip text for the Browser Verify toolbar button */
	FText GetBrowserVerifyToolTip() const;

	/** Whether the ordinary restart-survival product action can start right now */
	bool CanStartRestartSurvival() const;

	/** Tooltip text for the ordinary restart-survival product action */
	FText GetRestartSurvivalToolTip() const;

	/** Toolbar title text with active provider identity */
	FText GetToolbarTitleText() const;

	/** Compact provider-specific control summary for the shared toolbar */
	FText GetProviderSummaryText() const;

	/** Detailed provider-specific control tooltip for the shared toolbar */
	FText GetProviderSummaryToolTip() const;

	/** Tooltip text for provider-safe session restore */
	FText GetRestoreSessionToolTip() const;
	
	/** Get status text */
	FText GetStatusText() const;
	
	/** Get status color */
	FSlateColor GetStatusColor() const;

	/** Get runtime connection indicator color */
	FSlateColor GetConnectionIndicatorColor() const;

	/** Get runtime connection indicator tooltip */
	FText GetConnectionIndicatorToolTip() const;

	/** Whether the transport-reset Retry Last action is currently safe and available */
	bool CanRetryLastTransportRequest() const;

	/** Visibility for the bounded Retry Last action after a transport-reset failure */
	EVisibility GetRetryLastTransportVisibility() const;

	/** Tooltip for the bounded Retry Last action */
	FText GetRetryLastTransportToolTip() const;

	/** Replay the exact last prompt on a fresh backend session when the prior failure was transport-safe */
	FReply HandleRetryLastTransportRequest();

	/** Add the initial welcome/debug snapshot for the currently configured backend */
	void AddInitialStatusMessage();

	/** Refresh the backend snapshot trace when the configured status changes without injecting ordinary chat noise */
	void RefreshBackendSnapshotIfChanged(bool bForce = false);

	/** Handle status changes from the bounded voice dictation service */
	void HandleDictationStatusChanged(const FOsvayderUEVoiceDictationStatus& Status);

	/** Insert a completed transcript into the shared prompt input */
	void HandleDictationTranscriptReady(const FString& Transcript);

	/** Dictation status line shown under the input area controls */
	FText GetDictationStatusText() const;

	/** Build a compact backend debug snapshot for the chat area */
	FString BuildBackendDebugSnapshot() const;

	/** Build a stable backend-status signature for polling without volatile session counters */
	FString BuildBackendSnapshotSignature() const;

	/** Build a compact MCP status summary for the backend debug snapshot */
	FString GenerateMCPCompactStatusSummary() const;

	/** Determine the effective runtime connection state shown by the widget indicator */
	EBackendRuntimeConnectionState GetRuntimeConnectionState() const;

	/** Invalidate callbacks from any older in-flight request so they cannot render into a reset shell */
	void InvalidateActiveRequestCallbacks();

	/** Check whether a callback belongs to the still-active request generation and backend */
	bool IsActiveRequestCallback(uint64 RequestGeneration, EOsvayderUEProviderBackend RequestBackend, const TCHAR* CallbackKind) const;
	
private:
	/** Chat message container */
	TSharedPtr<SVerticalBox> ChatMessagesBox;

	/** Scroll box for chat */
	TSharedPtr<SScrollBox> ChatScrollBox;

	/** Input area widget */
	TSharedPtr<SOsvayderInputArea> InputArea;

	/** Bounded voice dictation service owned by the editor widget */
	TSharedPtr<FOsvayderUEVoiceDictationService> VoiceDictationService;

	/** Current input text */
	FString CurrentInputText;

	/** Exact prompt text for the currently active request, preserved until the request settles. */
	FString CurrentRequestPromptText;

	/** Exact image/reference attachments for the currently active request. */
	TArray<FString> CurrentRequestImagePaths;

	/** Latest user-facing dictation status string */
	FString LastDictationStatusDetail;

	/** Last user-facing Codex auth probe status emitted by the diagnostics panel */
	FString LastCodexAuthProbeStatus;

	/** True while the async Codex auth probe is running */
	bool bCodexAuthProbeRunning = false;
	
	/** Is currently waiting for response */
	bool bIsWaitingForResponse = false;

	/**
	 * 626 P1 crash-safety one-shot guard. Set to true the first time
	 * `FinalizeStreamingResponse()` runs for the current request generation;
	 * subsequent calls no-op with a Warning log. Reset to false inside
	 * `InvalidateActiveRequestCallbacks()` so the next request starts clean.
	 * Prevents a double-release crash on shared_ptr controllers if both a
	 * streaming completion path and a final response path race into
	 * `FinalizeStreamingResponse`. See `observed_failures.md` rows #26/#28
	 * and 626 P1 dispatch crash stack (`ParseAndRenderCodeBlocks:4721`).
	 */
	bool bHasCompletedResponseDelivery = false;

	/** Timestamp when the current streaming request started (FPlatformTime::Seconds) */
	double StreamingStartTime = 0.0;

	/** Number of tool calls observed during current streaming response */
	int32 StreamingToolCallCount = 0;

	/** Final stats from the Result event (persists after streaming ends until next request) */
	FString LastResultStats;

	/** Last response for copying */
	FString LastResponse;

	/** Accumulated streaming response */
	FString StreamingResponse;

	/** Current streaming message widget (for updating in place) */
	TSharedPtr<SMultiLineEditableText> StreamingTextBlock;

	/** Inner content box for streaming bubble (holds text segments + tool indicators) */
	TSharedPtr<SVerticalBox> StreamingContentBox;

	/** Text accumulated for the current segment only (reset on each tool use) */
	FString CurrentSegmentText;

	/** Tool call status labels by call ID */
	TMap<FString, TSharedPtr<STextBlock>> ToolCallStatusLabels;

	/** Selectable tool call result text blocks by call ID */
	TMap<FString, TSharedPtr<SMultiLineEditableText>> ToolCallResultTexts;

	/** Tool call expandable areas by call ID */
	TMap<FString, TSharedPtr<SExpandableArea>> ToolCallExpandables;

	/** Tool names by call ID */
	TMap<FString, FString> ToolCallNames;

	/** All text segments in order (frozen when tool events arrive) */
	TArray<FString> AllTextSegments;

	/** Text block widgets for each segment (for code block post-processing) */
	TArray<TSharedPtr<SMultiLineEditableText>> TextSegmentBlocks;

	/** Container vertical boxes wrapping each text segment (for code block replacement) */
	TArray<TSharedPtr<SVerticalBox>> TextSegmentContainers;

	/** Current tool group expandable area */
	TSharedPtr<SExpandableArea> ToolGroupExpandArea;

	/** Inner box holding tool entries in current group */
	TSharedPtr<SVerticalBox> ToolGroupInnerBox;

	/** Summary text for current tool group header */
	TSharedPtr<STextBlock> ToolGroupSummaryText;

	/** Number of tools in current group */
	int32 ToolGroupCount = 0;

	/** Number of completed tools in current group */
	int32 ToolGroupDoneCount = 0;

	/** Tool call IDs in current group (for showing/hiding labels on transition) */
	TArray<FString> ToolGroupCallIds;

	/** Include UE5.7 context in prompts */
	bool bIncludeUE57Context = true;

	/** Include project context in prompts */
	bool bIncludeProjectContext = true;

	/** Last backend snapshot signature observed by widget polling */
	FString LastBackendSnapshotSignature;

	/** Mirror of visible chat messages for bounded proof/readback helpers */
	TArray<FString> VisibleChatMessages;

	/** Provider currently rendered into the shared chat shell */
	EOsvayderUEProviderBackend LastRenderedBackend = EOsvayderUEProviderBackend::CodexCli;

	/** Last time the widget polled backend state for chat snapshot updates */
	double LastBackendSnapshotPollTime = 0.0;

	/**
	 * 632 Task Recovery & Rehydration: set to true once the startup-triggered
	 * recovery dialog has either been shown (user made a choice) or skipped
	 * (plan not interrupted, or user already chose). Prevents re-triggering
	 * on every Tick.
	 */
	bool bTaskRecoveryStartupCheckCompleted = false;

	/**
	 * 632 wiring: check once on first Tick whether Saved/OsvayderUE/active_plan.json
	 * carries interruption metadata with no prior user_recovery_choice, and
	 * show the modal recovery dialog if so. Persists the user's choice back
	 * to the plan + routes the chosen path (Continue arms a [TASK RECOVERY]
	 * context block and attempts one-shot auto-resume; Start Fresh / Close as
	 * Irrelevant archive the plan).
	 * No-op if WITH_EDITOR is unavailable or the slate app isn't active.
	 */
	void TryRunTaskRecoveryStartupCheck();

	/** Last live runtime connection result observed by the widget */
	EBackendRuntimeConnectionState LastRuntimeConnectionState = EBackendRuntimeConnectionState::Unknown;

	/** Short detail for the last runtime connection outcome */
	FString LastRuntimeConnectionDetail;

	struct FLastTransportFailureState
	{
		bool bActive = false;
		bool bRetrySafe = false;
		FString Prompt;
		FString SourceRunId;
		FString FailureFamily;
		FString FailureMessage;
		FString RetryBlockReason;
	};

	FLastTransportFailureState LastTransportFailureState;

	struct FPendingTransportRetryReplayContext
	{
		bool bActive = false;
		FString SourceRunId;
		FString FailureFamily;
	};

	FPendingTransportRetryReplayContext PendingTransportRetryReplayContext;

	/** Provider that owned the currently active request when it was dispatched */
	EOsvayderUEProviderBackend ActiveRequestBackend = EOsvayderUEProviderBackend::CodexCli;

	/** Monotonic request generation used to invalidate stale callbacks after provider/session resets */
	uint64 ActiveRequestGeneration = 0;

	/** Monotonic counter for issuing unique request generations */
	uint64 NextRequestGeneration = 0;

	struct FPendingClosedEditorBuildBlockerIntervention
	{
		bool bActive = false;
		bool bRestartSurvivalArmed = false;
		FString BlockerFamily;
		FString EscalationReason;
		FString UserFacingMessage;
	};

	/** One pending blocker intervention that should replace generic interrupt/timeout UX on completion. */
	FPendingClosedEditorBuildBlockerIntervention PendingClosedEditorBuildBlockerIntervention;

	/** Prevent repeated manual-wait notices for the same detached owner task. */
	FString LastDetachedOwnerWaitNoticeTaskId;

	struct FPendingHeadlessAcceptanceBridge
	{
		bool bActive = false;
		FOsvayderUEHeadlessAcceptanceRequest Request;
		FString ReceiptPath;
		FString PromptHash;
		FString StartedAtUtc;
		FDateTime StartedAtUtcDateTime = FDateTime::MinValue();
		double StartedSeconds = 0.0;
		uint64 RequestGeneration = 0;
		EOsvayderUEProviderBackend Backend = EOsvayderUEProviderBackend::CodexCli;
	};

	FPendingHeadlessAcceptanceBridge PendingHeadlessAcceptanceBridge;

	/** 606 Part B cached banner display state updated once per polling tick. */
	struct FAgent2BackgroundBannerState
	{
		bool bVisible = false;
		FString PhaseLabel;
		FString LaneStatusLabel;
		FString TaskIdShort;
		FString StateFilePath;
		FString MonitorScriptPath;
		FString CancelRequestPath;
	};
	FAgent2BackgroundBannerState Agent2BannerState;

	/** Reuse the persisted active plan on the next SendMessage instead of creating a fresh plan. */
	bool bResumeExistingActivePlanOnNextSend = false;

	/** When true, the next resumed send should use the post-reattach verification policy instead of exact-boundary resume. */
	bool bUsePostReattachResumePolicyOnNextSend = false;

	/** Reset all streaming and tool tracking state to defaults */
	void ResetStreamingState();
	void TickHeadlessAcceptanceBridgeTimeout(double InCurrentTime);
	void CompletePendingHeadlessAcceptanceBridge(
		const FString& Response,
		bool bAssistantSuccess,
		const FOsvayderUEActivePlanCloseoutDecision* CloseoutDecision,
		const FString& FailureReason);
	void WriteHeadlessAcceptanceReceipt(
		const FString& Status,
		bool bAssistantSuccess,
		const FOsvayderUEActivePlanCloseoutDecision* CloseoutDecision,
		const FString& FailureReason,
		const FString& ResponsePreview);

	/** Start a new streaming response message */
	void StartStreamingResponse();

	/** Finalize streaming response */
	void FinalizeStreamingResponse();

	/** Handle a ToolUse stream event (insert tool indicator, start new text segment) */
	void HandleToolUseEvent(const FOsvayderStreamEvent& Event);

	/** Handle a ToolResult stream event (update tool indicator with completion + result) */
	void HandleToolResultEvent(const FOsvayderStreamEvent& Event, EOsvayderUEProviderBackend RequestBackend);

	/** Handle a Result stream event (append stats footer) */
	void HandleResultEvent(const FOsvayderStreamEvent& Event);

	/** Update tool group summary text based on pending/completed state */
	void UpdateToolGroupSummary();

	/** Get display-friendly tool name (strips MCP server prefix) */
	static FString GetDisplayToolName(const FString& FullToolName);

	/** Post-process text segments to render code blocks with monospace styling */
	void ParseAndRenderCodeBlocks();

public:
	/**
	 * Parse text into alternating plain/code sections split on triple-backtick fences.
	 * Exposed publicly for 626 P1 crash-safety automation tests
	 * (OsvayderUE.Widget.CrashSafety.MalformedCodeFences_DoesNotCrash).
	 * Pure static function; no widget state dependency.
	 */
	static void ParseCodeFences(const FString& Input, TArray<TPair<FString, bool>>& OutSections);

private:
	/** Refresh project context */
	void RefreshProjectContext();

	/** Generate MCP tool status message for greeting */
	FString GenerateMCPStatusMessage() const;
};
