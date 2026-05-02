// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"
#include "Dom/JsonObject.h"

enum class EUnrealClaudeRelayTerminalOutcome : uint8
{
	None,
	Success,
	TerminalFail,
	Cancelled
};

UNREALCLAUDE_API const TCHAR* UnrealClaudeRelayTerminalOutcomeToString(EUnrealClaudeRelayTerminalOutcome Outcome);
UNREALCLAUDE_API bool ParseUnrealClaudeRelayTerminalOutcome(const FString& Value, EUnrealClaudeRelayTerminalOutcome& OutOutcome);

struct UNREALCLAUDE_API FUnrealClaudeRelaySettingsSnapshot
{
	int32 SchemaVersion = 1;
	FString Backend = TEXT("CodexCli");
	FString BackendDisplayName;
	FString Model;
	FString Profile;
	FString RequestedSpeedMode;
	FString EffectiveSpeedMode;
	FString SpeedSupport;
	FString WorkMode;
	FString ReasoningEffort;
	FString Verbosity;
	FString AuthMode;
	FString AuthPath;
	FString AuthOwnership;
	FString CodexHomePath;
	FString CodexHomeResolutionSource;
	FString ExecutionTransport = TEXT("persistent_app_server");
	bool bPersistentAppServerEnabled = true;
	bool bClearProxyEnvForExec = false;
	bool bHasExplicitCodexHomeOverride = false;
};

struct UNREALCLAUDE_API FUnrealClaudeRelayHandoffContext
{
	int32 SchemaVersion = 1;
	FString TaskId;
	FString RelaySessionId;
	FString ProjectRoot;
	FString UProjectPath;
	FString ReattachToken;
	FString ReattachNotice;
	FString CreatedAtUtc;
	FString OriginalUserPrompt;
	FString OriginPromptHash;
	bool bVisualProofRequired = false;
	bool bVisualQaManifestRequired = false;
	TArray<FString> AttachedImagePaths;
	TArray<FString> AttachmentNames;
	FString EditorAgentSummary;
	FString LastKnownBlockerFamily;
	FString LastKnownBlockerSignature;
	TArray<FString> KnownFacts;
	TArray<FString> RelevantArtifactPaths;
	TArray<FString> RelevantToolReceipts;
	FString NextAttemptHypothesis;
	FString BoundedObjective;
	FString BoundedObjectiveDetail;
	int32 ReasoningIterationBudget = 10;
	int32 WallClockBudgetSeconds = 900;
	FUnrealClaudeRelaySettingsSnapshot Settings;
};

struct UNREALCLAUDE_API FUnrealClaudeRelayProgressEntry
{
	int32 SchemaVersion = 1;
	FString TaskId;
	FString PlanId;
	FString MechanicId;
	FString ToolCallId;
	FString RelaySessionId;
	FString TimestampUtc;
	FString EntryKind = TEXT("summary");
	FString Summary;
	FString SummaryRu;
	FString TechnicalDetail;
	FString CurrentAction;
	FString CurrentActionRu;
	FString CurrentToolName;
	int32 IterationIndex = 0;
	double ElapsedSeconds = 0.0;
	double HeartbeatAgeSeconds = 0.0;
	bool bIsStale = false;
	EUnrealClaudeRelayTerminalOutcome TerminalOutcome = EUnrealClaudeRelayTerminalOutcome::None;
};

struct UNREALCLAUDE_API FUnrealClaudeRelayResult
{
	int32 SchemaVersion = 1;
	FString ArtifactType;
	FString TaskId;
	FString PlanId;
	FString WorkflowId;
	FString CurrentMechanicId;
	FString CurrentToolCallId;
	FString RelaySessionId;
	FString CompletedAtUtc;
	EUnrealClaudeRelayTerminalOutcome TerminalOutcome = EUnrealClaudeRelayTerminalOutcome::None;
	FString Status;
	FString OriginPromptHash;
	FString Summary;
	FString SummaryRu;
	FString TechnicalDetail;
	FString FinalBlockerFamily;
	FString FinalBlockerSignature;
	FString PlanStatus;
	FString NextResumePhase;
	FString BuildCommand;
	FString BuildLogPath;
	FString TargetProofStatus;
	bool bRequiresPostReattachVerification = false;
	TArray<FString> CompletedMechanicIds;
	TArray<FString> RelevantArtifactPaths;
	TArray<FString> ChangedFiles;
	TArray<FString> RequiredLiveChecks;
	FString RelayProgressPath;
	int32 IterationsUsed = 0;
	double ElapsedSeconds = 0.0;
	FString ReattachSummary;
};

struct UNREALCLAUDE_API FUnrealClaudePlanMechanicEntry
{
	int32 SchemaVersion = 1;
	FString MechanicId;
	FString Label;
	FString LabelRu;
	FString Status = TEXT("pending");
	FString StartedAtUtc;
	FString CompletedAtUtc;
	FString LastSummary;
	FString LastSummaryRu;
	bool bRequiresClosedEditor = false;
	bool bRequiresPostReattachVerification = false;
};

struct UNREALCLAUDE_API FUnrealClaudePlanToolCallEntry
{
	int32 SchemaVersion = 1;
	FString ToolCallId;
	FString MechanicId;
	FString ToolName;
	FString Status = TEXT("pending");
	FString Summary;
	FString SummaryRu;
	FString TechnicalDetail;
	FString StartedAtUtc;
	FString CompletedAtUtc;
	FString ResultStatus;
};

struct UNREALCLAUDE_API FUnrealClaudeCompileProofState
{
	int32 SchemaVersion = 1;
	bool bCompiledModuleMutationObserved = false;
	FString MutationToolFamily;
	FString LastMutationAtUtc;
	FString LastMutationToolCallId;
	FString LastMutationToolName;
	FString LastCompileProofAtUtc;
	FString LastCompileProofToolCallId;
	FString LastCompileProofToolName;
	FString LastCompileProofOutcome; // "success" | "failed" | empty
	FString LastCompileProofDetail;
	FString LastPostCompileVerificationAtUtc;
	FString LastPostCompileVerificationToolCallId;
	FString LastPostCompileVerificationToolName;
	FString LastPostCompileVerificationOutcome; // "pass" | "fail" | empty
	FString LastCloseoutGateReason;
};

struct UNREALCLAUDE_API FUnrealClaudeActivePlan
{
	int32 SchemaVersion = 1;
	FString PlanId;
	FString ReviewerPlanReference;
	FString OriginalUserTask;
	FString CreatedAtUtc;
	FString UpdatedAtUtc;
	FString Status = TEXT("active");
	FString ResultStatus;
	FString Summary;
	FString SummaryRu;
	FString TechnicalDetail;
	FString CurrentMechanicId;
	FString CurrentToolCallId;
	FString CurrentAction;
	FString CurrentActionRu;
	FString CurrentTechnicalDetail;
	FString ResumeHint;
	FUnrealClaudeTaskLaneState LaneState;
	FString HandoffPolicy = TEXT("full_batch_default");
	FString HybridSplitReason;
	FString LastCompletedToolCallId;
	FString ArchiveRunTag;
	bool bHybridSplitTriggered = false;
	bool bPostReattachVerificationRequired = false;
	bool bVisualProofRequired = false;
	bool bVisualQaManifestRequired = false;
	FString VisualProofRequirement;
	FString VisualProofStatus;
	FString VisualProofArtifactPath;
	FString VisualProofBlocker;
	FString VisualQaManifestPath;
	FString VisualQaManifestVerdict;
	FUnrealClaudeRelaySettingsSnapshot Settings;
	FUnrealClaudeCompileProofState CompileProof;
	FAgentFeatureWorkflowState FeatureWorkflow;
	TArray<FUnrealClaudePlanMechanicEntry> Mechanics;
	TArray<FUnrealClaudePlanToolCallEntry> ToolCalls;
	TArray<FString> CompletedMechanicIds;
	TArray<FString> VerificationChecklist;
	TArray<FString> VisualReferenceArtifactPaths;

	// 632 Task Recovery & Rehydration: interruption detection + user choice.
	// Absent in pre-632 files (JSON read treats missing as empty / 0, write
	// always emits so writer is forward-compat but reader is back-compat).
	FString InterruptionDetectedAtUtc;                            // ISO-8601, empty if never detected
	FString InterruptionReason;                                    // "previous_session_crashed" | "editor_startup_after_quiet_period" | "previous_restart_survival_failed" | "previous_restart_survival_stuck_or_crashed" | empty
	int32 InterruptionElapsedSeconds = 0;                          // (NowUtc - UpdatedAtUtc) at detection time
	FString UserRecoveryChoice;                                    // "" | "continue" | "start_fresh" | "closed_as_irrelevant"
	FString UserClosedReason;                                      // free-form text, only set when UserRecoveryChoice == "closed_as_irrelevant"
};

class UNREALCLAUDE_API FUnrealClaudeRelayAgentManager
{
public:
	static FString GetRelayRootDir();
	static FString GetActivePlanPath();
	static FString GetPlanArchiveDir();
	static FString GetRelayArchiveDir();
	static FString GetHandoffContextPath();
	static FString GetRelayProgressPath();
	static FString GetRelayResultPath();
	static FString GetRelayCancelRequestPath();
	static FString GetRelayAgentScriptPath();

	static bool LoadActivePlan(FUnrealClaudeActivePlan& OutPlan, FString& OutError);
	static bool SaveActivePlan(const FUnrealClaudeActivePlan& Plan, FString& OutError);
	static bool DeleteActivePlan(FString& OutError);
	static bool LoadPlanFromPath(const FString& PlanPath, FUnrealClaudeActivePlan& OutPlan, FString& OutError);
	static bool SavePlanToPath(const FString& PlanPath, const FUnrealClaudeActivePlan& Plan, FString& OutError);

	/**
	 * 619 P3 Fix #3: tolerant post-reattach read that falls through a
	 * 3-step chain when active_plan.json is absent.
	 *
	 * Outcome values report which source the returned plan was synthesized
	 * from so callers (and tests) can log + branch on the source:
	 *   ActivePlanJson                   - normal happy path; active_plan.json on disk.
	 *   RestartSurvivalStateJsonFallback - active_plan.json missing; state.json.post_reattach_completion_text is non-empty; plan synthesized from default shape with the continuation text woven in.
	 *   NoneRecoverable                  - neither source carries a continuation; graceful-warning path. OutPlan is synthesized as a default plan for Prompt. OutError is empty.
	 *
	 * The `NoneRecoverable` case returns **true** (not false) because from
	 * the agent-contract POV we CAN continue (with a fresh plan); the widget
	 * just won't route the old [failed] error string. This is the critical
	 * semantic the spec is after: "No continuation plan recoverable,
	 * reverting to fresh session" plus a logged warning. No [failed] in
	 * widget, no error popup, user sees fresh session.
	 */
	enum class EActivePlanFallbackSource : uint8
	{
		ActivePlanJson,
		RestartSurvivalStateJsonFallback,
		NoneRecoverable
	};
	static bool LoadActivePlanWithFallback(
		const FString& Prompt,
		FUnrealClaudeActivePlan& OutPlan,
		EActivePlanFallbackSource& OutSource,
		FString& OutError);
	static bool LoadProgressEntriesForTask(const FString& TaskId, TArray<FUnrealClaudeRelayProgressEntry>& OutEntries, FString& OutError);
	static bool ArchiveTerminalArtifacts(const FUnrealClaudeActivePlan& Plan, TArray<FString>& OutArchivedPaths, FString& OutError);
	static bool PruneArchivedArtifacts(int32 MaxPlanSnapshots, int32 MaxRelayBundles, FString& OutError);

	static bool LoadHandoffContext(FUnrealClaudeRelayHandoffContext& OutContext, FString& OutError);
	static bool SaveHandoffContext(const FUnrealClaudeRelayHandoffContext& Context, FString& OutError);
	static bool DeleteHandoffContext(FString& OutError);

	static bool AppendProgressEntry(const FUnrealClaudeRelayProgressEntry& Entry, FString& OutError);
	static bool LoadLatestProgressEntry(FUnrealClaudeRelayProgressEntry& OutEntry, FString& OutError);
	static bool LoadLatestProgressEntryForTask(const FString& TaskId, FUnrealClaudeRelayProgressEntry& OutEntry, FString& OutError);
	static bool DeleteProgressLog(FString& OutError);

	static bool LoadRelayResult(FUnrealClaudeRelayResult& OutResult, FString& OutError);
	static bool LoadRelayResultForTask(const FString& TaskId, FUnrealClaudeRelayResult& OutResult, FString& OutError);
	static bool SaveRelayResult(const FUnrealClaudeRelayResult& Result, FString& OutError);
	static bool DeleteRelayResult(FString& OutError);

	static bool HasCancelRequest();
	static bool WriteCancelRequest(const FString& Reason, FString& OutError);
	static bool DeleteCancelRequest(FString& OutError);

	static FUnrealClaudeRelaySettingsSnapshot BuildCurrentCodexSettingsSnapshot();
};
