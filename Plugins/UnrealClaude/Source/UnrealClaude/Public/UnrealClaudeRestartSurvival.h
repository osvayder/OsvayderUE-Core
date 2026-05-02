// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"
#include "Dom/JsonObject.h"

enum class EUnrealClaudeRestartSurvivalPhase : uint8
{
	AttachedInEditor,
	Detaching,
	DetachedRunning,
	AwaitingRelaunch,
	Relaunching,
	AwaitingReattach,
	Reattached,
	FailedTerminal
};

UNREALCLAUDE_API const TCHAR* UnrealClaudeRestartSurvivalPhaseToString(EUnrealClaudeRestartSurvivalPhase Phase);
UNREALCLAUDE_API bool ParseUnrealClaudeRestartSurvivalPhase(const FString& Value, EUnrealClaudeRestartSurvivalPhase& OutPhase);

enum class EUnrealClaudeClosedEditorBuildBlockerFamily : uint8
{
	None,
	UbtLogBackupAccessDenied,
	UhtGeneratedRenameLock,
	LiveCodingActive,
	IntermediateBuildArtifactAccessDenied,
	EditorBinaryLinkerLock,
	EditorOpenFileLock
};

UNREALCLAUDE_API const TCHAR* UnrealClaudeClosedEditorBuildBlockerFamilyToString(EUnrealClaudeClosedEditorBuildBlockerFamily Family);

struct UNREALCLAUDE_API FUnrealClaudeClosedEditorBuildBlocker
{
	bool bDetected = false;
	EUnrealClaudeClosedEditorBuildBlockerFamily Family = EUnrealClaudeClosedEditorBuildBlockerFamily::None;
	FString ClassificationLabel;
	FString FamilyLabel;
	FString EscalationReason;
	FString MatchedEvidence;
};

struct UNREALCLAUDE_API FUnrealClaudeRestartSurvivalRestoreIntent
{
	bool bEnabled = false;
	FString AutosaveSourcePath;
	FString TargetPath;
	FString BackupPath;
	FString Detail;
};

struct UNREALCLAUDE_API FUnrealClaudeRestartSurvivalFileWriteIntent
{
	bool bEnabled = false;
	FString SourcePath;
	FString TargetPath;
	FString BackupPath;
	FString Detail;
};

struct UNREALCLAUDE_API FUnrealClaudeRestartSurvivalOriginTaskContext
{
	FString OriginatingRunId;
	FString OriginatingUserPrompt;
	FString OriginatingPromptHash;
	FString OriginatingTaskMode;
	FString OriginatingRequestedToolFamily;
	FString OriginatingPrimaryMutationToolFamily;
	bool bOriginatingHasAttachments = false;
	bool bOriginatingHasVisualReference = false;
	bool bVisualProofRequired = false;
	bool bVisualQaManifestRequired = false;
	TArray<FString> OriginatingAttachedImagePaths;
	TArray<FString> OriginatingAttachmentNames;
	FString VisualReferenceRequirement;
};

struct UNREALCLAUDE_API FUnrealClaudeRestartSurvivalPreparedRestoreRequest
{
	int32 SchemaVersion = 1;
	FString RequestId;
	FString TaskId;
	FString SessionId;
	EUnrealClaudeProviderBackend Backend = EUnrealClaudeProviderBackend::CodexCli;
	FString LinkedProviderSessionId;
	FString ProjectRoot;
	bool bTaskDrivenHandoff = false;
	bool bAutoStartAfterResponse = false;
	bool bRestoreEnabled = false;
	bool bFileWriteEnabled = false;
	bool bAutonomousClosedEditorEscalation = false;
	bool bUseRelayAgent = false;
	FString ContinuationIntentPrompt;
	FString AutosaveSourcePath;
	FString TargetPath;
	FString BackupPath;
	FString FileWriteSourcePath;
	FString FileWriteTargetPath;
	FString FileWriteBackupPath;
	FString FileWriteDetail;
	FString Detail;
	FString CreatedAtUtc;
	FString PostReattachCompletionText;
	FUnrealClaudeRestartSurvivalOriginTaskContext OriginTask;
};

struct UNREALCLAUDE_API FUnrealClaudeRestartSurvivalProofState
{
	bool bEnabled = false;
	FString RunTag;
	FString ProofOutputPath;
	FString DetachedLogPath;
	FString BuildLogPath;
	FString DetachedFileTargetPath;
	FString DetachedFileExpectedText;
	FString RestoreExpectedText;
	FString FinalizeCommand;
	bool bDetachedFileWriteCompleted = false;
	bool bRestoreCompleted = false;
	bool bBuildCompleted = false;
	bool bRelaunchStarted = false;
	bool bReattachValidated = false;
};

struct UNREALCLAUDE_API FUnrealClaudeRestartSurvivalPreparedStartOverride
{
	FString TaskId;
	FString SessionId;
	FString ReattachTokenOverride;
	FString ReattachNoticeOverride;
	FString AdditionalRelaunchArguments;
	FUnrealClaudeRestartSurvivalProofState Proof;
};

struct UNREALCLAUDE_API FUnrealClaudeRestartSurvivalSupportBundle
{
	FString ResolutionLabel;
	FString BundleRoot;
	FString SupervisorScriptPath;
	FString MonitorScriptPath;
	FString PreflightScriptPath;
};

struct UNREALCLAUDE_API FUnrealClaudeRestartSurvivalState
{
	int32 SchemaVersion = 1;
	FString SessionId;
	FString TaskId;
	FString ProjectRoot;
	FString UProjectPath;
	EUnrealClaudeProviderBackend Backend = EUnrealClaudeProviderBackend::CodexCli;
	FString BackendDisplayName;
	FString ExecutionControlProfileId;
	FString ExecutionTransportLabel;
	FString DetachedSupervisorKind = TEXT("powershell_local_task_owner_v2");
	FString ProviderSessionId;
	FString ProviderThreadStatePath;
	bool bProviderThreadResumePending = false;
	EUnrealClaudeRestartSurvivalPhase Phase = EUnrealClaudeRestartSurvivalPhase::AttachedInEditor;
	FString PhaseDetail;
	FUnrealClaudeTaskLaneState LaneState;
	FString ReattachToken;
	FString ReattachNotice;
	int32 EditorProcessId = 0;
	FString EngineRoot;
	FString EditorExecutablePath;
	FString BuildBatchPath;
	FString BuildTarget;
	FString RelaunchArguments;
	FString SupportBundleResolution;
	FString SupportBundleRoot;
	FString SupervisorScriptPath;
	FString MonitorScriptPath;
	FString PreflightLauncherPath;
	FString CreatedAtUtc;
	FString LastUpdatedAtUtc;
	FString DetachedObjective = TEXT("closed_editor_task_owner_continuity_v1");
	FString DetachedObjectiveDetail;
	int32 DetachedStepIndex = 0;
	int32 DetachedStepBudget = 3;
	FString DetachedCurrentStep;
	FString DetachedPendingStep;
	FString DetachedLastStepOutcome;
	FString DetachedLastBlockerFamily;
	FString DetachedLastBlockerSignature;
	FString DetachedTerminalOutcome;
	bool bDetachedFileWriteCompleted = false;
	bool bDetachedRestoreCompleted = false;
	bool bDetachedBuildCompleted = false;
	int32 DetachedOwnerProcessId = 0;
	bool bDetachedOwnerActive = false;
	bool bDetachedOwnerManualReopenDetected = false;
	bool bDetachedOwnerCrashObserved = false;
	FString FailureReason;
	FString PreparedRestoreRequestId;
	FString PreparedRestoreRequestCreatedAtUtc;
	FString PostReattachCompletionText;
	bool bPostReattachCompletionPending = false;
	bool bPostReattachCompletionDispatched = false;
	FUnrealClaudeRestartSurvivalOriginTaskContext OriginTask;
	FUnrealClaudeRestartSurvivalRestoreIntent RestoreIntent;
	FUnrealClaudeRestartSurvivalFileWriteIntent FileWriteIntent;
	FUnrealClaudeRestartSurvivalProofState Proof;
};

class UNREALCLAUDE_API FUnrealClaudeRestartSurvivalManager
{
public:
	static FString GetStateRootDir();
	static FString GetStatePath();
	static FString GetPreparedRestoreRequestPath();
	static FString GetClosedEditorResultPath();
	static FString GetSupervisorScriptPath();
	static FString GetMonitorScriptPath();
	static FString GetPreflightScriptPath();
	static FString BuildPreparedRequestClosedEditorTransitionNotice(const FUnrealClaudeRestartSurvivalPreparedRestoreRequest& Request);
	static bool TryResolveSupportBundle(FUnrealClaudeRestartSurvivalSupportBundle& OutBundle, FString& OutError);
	/**
	 * Classifies a command_execution tool result as a closed-editor build
	 * blocker. Gated by 626 P2 Layers A/B:
	 *   - Layer A admits only if ToolInput is a recognized build invocation
	 *     (Build.bat, UnrealBuildTool, cl.exe, link.exe, msbuild, etc.) OR
	 *     the haystack carries a structured build-output signature (UBT/UHT/
	 *     Live Coding/compile-context). Known inspection commands
	 *     (Get-Content, rg, findstr, grep, git status/diff/log, etc.) are
	 *     rejected explicitly before matching.
	 *   - Layer B strips grep-output lines (path:line:content format) and
	 *     lines whose path markers point at repository source/docs/saved
	 *     artifacts from the haystack before blocker-family matching, so
	 *     self-reference (Ouroboros) quoting by grep/rg cannot escalate.
	 * ToolInput may be empty if the caller cannot surface it; in that case
	 * only the haystack-signature admission path is available. This is the
	 * legacy call shape used by existing 5.7-stage positive tests.
	 */
	static bool TryDetectClosedEditorBuildBlocker(
		const FString& ToolName,
		const FString& ToolInput,
		const FString& ToolResultContent,
		const FString& RawProviderEvent,
		FUnrealClaudeClosedEditorBuildBlocker& OutBlocker);
	static bool TryPrepareClosedEditorBuildBlockerAutoStart(
		EUnrealClaudeProviderBackend Backend,
		const FString& ProjectRoot,
		const FString& ProviderSessionId,
		const FUnrealClaudeClosedEditorBuildBlocker& Blocker,
		const FUnrealClaudeRestartSurvivalOriginTaskContext& OriginTask,
		FUnrealClaudeRestartSurvivalPreparedRestoreRequest& OutRequest,
		FString& OutTransitionNotice,
		FString& OutError);

	static bool LoadState(FUnrealClaudeRestartSurvivalState& OutState, FString& OutError);
	static bool SaveState(FUnrealClaudeRestartSurvivalState State, FString& OutError);
	static bool DeleteState(FString& OutError);
	static bool DescribeCurrentState(FUnrealClaudeRestartSurvivalState& OutState);
	static bool CanReplaceExistingStateForFreshStart(const FUnrealClaudeRestartSurvivalState& State);
	static bool IsDetachedOwnerPhaseActive(EUnrealClaudeRestartSurvivalPhase Phase);
	static bool HasPreparedRestoreRequest();
	static bool LoadPreparedRestoreRequest(FUnrealClaudeRestartSurvivalPreparedRestoreRequest& OutRequest, FString& OutError);
	static bool SavePreparedRestoreRequest(FUnrealClaudeRestartSurvivalPreparedRestoreRequest Request, FString& OutError);
	static FString ResolvePreparedRequestPostReattachCompletionText(const FUnrealClaudeRestartSurvivalPreparedRestoreRequest& Request);
	static bool DeletePreparedRestoreRequest(FString& OutError);
	static bool ValidatePreparedRestoreRequestForStart(
		const FUnrealClaudeRestartSurvivalPreparedRestoreRequest& Request,
		EUnrealClaudeProviderBackend CurrentBackend,
		const FString& CurrentProjectRoot,
		const FString& CurrentProviderSessionId,
		FString& OutError);
	static void ArmPreparedRequestAutoStart(
		const FString& RequestId,
		EUnrealClaudeProviderBackend Backend,
		const FString& ProjectRoot,
		const FString& ProviderSessionId);
	static void ClearPreparedRequestAutoStartArm();
	static bool HasPreparedRequestAutoStartArm();
	static bool TryConsumePreparedRequestAutoStart(
		EUnrealClaudeProviderBackend CurrentBackend,
		const FString& CurrentProjectRoot,
		const FString& CurrentProviderSessionId,
		FUnrealClaudeRestartSurvivalPreparedRestoreRequest& OutRequest,
		FString& OutError);
	static void ArmPreparedRequestStartOverride(const FUnrealClaudeRestartSurvivalPreparedStartOverride& Override);
	static void ClearPreparedRequestStartOverride();
	static bool TryConsumePreparedRequestStartOverride(
		const FUnrealClaudeRestartSurvivalPreparedRestoreRequest& Request,
		FUnrealClaudeRestartSurvivalPreparedStartOverride& OutOverride);

	static bool HasPendingResume(EUnrealClaudeProviderBackend Backend);
	static bool TryMarkReattached(EUnrealClaudeProviderBackend ExpectedBackend, const FString& PresentedToken, FString& OutNotice, FString& OutError);
	static bool TryMarkReattachedFromManualReopen(EUnrealClaudeProviderBackend ExpectedBackend, FString& OutNotice, FString& OutError);
	static bool MarkReattachValidated(FString& OutError);
	static bool MarkFailed(const FString& Reason, FString& OutError);

	static TSharedPtr<FJsonObject> BuildReadbackJson();
	static FString BuildWidgetDebugSummary();

	/**
	 * Ordering helper for 624 race fix: invoke WriteSpawnRequestFn first and only
	 * call TransportResetFn after it succeeds. Guarantees the detached-owner
	 * spawn-request is observable on disk before the Codex persistent transport is
	 * torn down, preventing the "arm without execute" race.
	 *
	 * Returns true if WriteSpawnRequestFn succeeded and TransportResetFn was
	 * invoked; false if WriteSpawnRequestFn failed (in which case TransportResetFn
	 * is not called).
	 */
	static bool ExecuteSpawnBeforeTransportResetSequence(
		TFunctionRef<bool()> WriteSpawnRequestFn,
		TFunctionRef<void()> TransportResetFn);

#if WITH_DEV_AUTOMATION_TESTS
	static void SetTestStateRootOverride(const FString& InDir);
	static void ClearTestStateRootOverride();
#endif
};
