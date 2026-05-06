// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"
#include "Dom/JsonObject.h"

enum class EOsvayderUERestartSurvivalPhase : uint8
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

OSVAYDERUE_API const TCHAR* OsvayderUERestartSurvivalPhaseToString(EOsvayderUERestartSurvivalPhase Phase);
OSVAYDERUE_API bool ParseOsvayderUERestartSurvivalPhase(const FString& Value, EOsvayderUERestartSurvivalPhase& OutPhase);

enum class EOsvayderUEClosedEditorBuildBlockerFamily : uint8
{
	None,
	UbtLogBackupAccessDenied,
	UhtGeneratedRenameLock,
	LiveCodingActive,
	IntermediateBuildArtifactAccessDenied,
	EditorBinaryLinkerLock,
	EditorOpenFileLock
};

OSVAYDERUE_API const TCHAR* OsvayderUEClosedEditorBuildBlockerFamilyToString(EOsvayderUEClosedEditorBuildBlockerFamily Family);

struct OSVAYDERUE_API FOsvayderUEClosedEditorBuildBlocker
{
	bool bDetected = false;
	EOsvayderUEClosedEditorBuildBlockerFamily Family = EOsvayderUEClosedEditorBuildBlockerFamily::None;
	FString ClassificationLabel;
	FString FamilyLabel;
	FString EscalationReason;
	FString MatchedEvidence;
};

struct OSVAYDERUE_API FOsvayderUERestartSurvivalRestoreIntent
{
	bool bEnabled = false;
	FString AutosaveSourcePath;
	FString TargetPath;
	FString BackupPath;
	FString Detail;
};

struct OSVAYDERUE_API FOsvayderUERestartSurvivalFileWriteIntent
{
	bool bEnabled = false;
	FString SourcePath;
	FString TargetPath;
	FString BackupPath;
	FString Detail;
};

struct OSVAYDERUE_API FOsvayderUERestartSurvivalOriginTaskContext
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

struct OSVAYDERUE_API FOsvayderUERestartSurvivalPreparedRestoreRequest
{
	int32 SchemaVersion = 1;
	FString RequestId;
	FString TaskId;
	FString SessionId;
	EOsvayderUEProviderBackend Backend = EOsvayderUEProviderBackend::CodexCli;
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
	FOsvayderUERestartSurvivalOriginTaskContext OriginTask;
};

struct OSVAYDERUE_API FOsvayderUERestartSurvivalProofState
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

struct OSVAYDERUE_API FOsvayderUERestartSurvivalPreparedStartOverride
{
	FString TaskId;
	FString SessionId;
	FString ReattachTokenOverride;
	FString ReattachNoticeOverride;
	FString AdditionalRelaunchArguments;
	FOsvayderUERestartSurvivalProofState Proof;
};

struct OSVAYDERUE_API FOsvayderUERestartSurvivalSupportBundle
{
	FString ResolutionLabel;
	FString BundleRoot;
	FString SupervisorScriptPath;
	FString MonitorScriptPath;
	FString PreflightScriptPath;
};

struct OSVAYDERUE_API FOsvayderUERestartSurvivalState
{
	int32 SchemaVersion = 1;
	FString SessionId;
	FString TaskId;
	FString ProjectRoot;
	FString UProjectPath;
	EOsvayderUEProviderBackend Backend = EOsvayderUEProviderBackend::CodexCli;
	FString BackendDisplayName;
	FString ExecutionControlProfileId;
	FString ExecutionTransportLabel;
	FString DetachedSupervisorKind = TEXT("powershell_local_task_owner_v2");
	FString ProviderSessionId;
	FString ProviderThreadStatePath;
	bool bProviderThreadResumePending = false;
	EOsvayderUERestartSurvivalPhase Phase = EOsvayderUERestartSurvivalPhase::AttachedInEditor;
	FString PhaseDetail;
	FOsvayderUETaskLaneState LaneState;
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
	FOsvayderUERestartSurvivalOriginTaskContext OriginTask;
	FOsvayderUERestartSurvivalRestoreIntent RestoreIntent;
	FOsvayderUERestartSurvivalFileWriteIntent FileWriteIntent;
	FOsvayderUERestartSurvivalProofState Proof;
};

class OSVAYDERUE_API FOsvayderUERestartSurvivalManager
{
public:
	static FString GetStateRootDir();
	static FString GetStatePath();
	static FString GetPreparedRestoreRequestPath();
	static FString GetClosedEditorResultPath();
	static FString GetSupervisorScriptPath();
	static FString GetMonitorScriptPath();
	static FString GetPreflightScriptPath();
	static FString BuildPreparedRequestClosedEditorTransitionNotice(const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request);
	static bool TryResolveSupportBundle(FOsvayderUERestartSurvivalSupportBundle& OutBundle, FString& OutError);
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
		FOsvayderUEClosedEditorBuildBlocker& OutBlocker);
	static bool TryPrepareClosedEditorBuildBlockerAutoStart(
		EOsvayderUEProviderBackend Backend,
		const FString& ProjectRoot,
		const FString& ProviderSessionId,
		const FOsvayderUEClosedEditorBuildBlocker& Blocker,
		const FOsvayderUERestartSurvivalOriginTaskContext& OriginTask,
		FOsvayderUERestartSurvivalPreparedRestoreRequest& OutRequest,
		FString& OutTransitionNotice,
		FString& OutError);

	static bool LoadState(FOsvayderUERestartSurvivalState& OutState, FString& OutError);
	static bool SaveState(FOsvayderUERestartSurvivalState State, FString& OutError);
	static bool DeleteState(FString& OutError);
	static bool DescribeCurrentState(FOsvayderUERestartSurvivalState& OutState);
	static bool CanReplaceExistingStateForFreshStart(const FOsvayderUERestartSurvivalState& State);
	static bool IsDetachedOwnerPhaseActive(EOsvayderUERestartSurvivalPhase Phase);
	static bool HasPreparedRestoreRequest();
	static bool LoadPreparedRestoreRequest(FOsvayderUERestartSurvivalPreparedRestoreRequest& OutRequest, FString& OutError);
	static bool SavePreparedRestoreRequest(FOsvayderUERestartSurvivalPreparedRestoreRequest Request, FString& OutError);
	static FString ResolvePreparedRequestPostReattachCompletionText(const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request);
	static bool DeletePreparedRestoreRequest(FString& OutError);
	static bool ValidatePreparedRestoreRequestForStart(
		const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request,
		EOsvayderUEProviderBackend CurrentBackend,
		const FString& CurrentProjectRoot,
		const FString& CurrentProviderSessionId,
		FString& OutError);
	static void ArmPreparedRequestAutoStart(
		const FString& RequestId,
		EOsvayderUEProviderBackend Backend,
		const FString& ProjectRoot,
		const FString& ProviderSessionId);
	static void ClearPreparedRequestAutoStartArm();
	static bool HasPreparedRequestAutoStartArm();
	static bool TryConsumePreparedRequestAutoStart(
		EOsvayderUEProviderBackend CurrentBackend,
		const FString& CurrentProjectRoot,
		const FString& CurrentProviderSessionId,
		FOsvayderUERestartSurvivalPreparedRestoreRequest& OutRequest,
		FString& OutError);
	static void ArmPreparedRequestStartOverride(const FOsvayderUERestartSurvivalPreparedStartOverride& Override);
	static void ClearPreparedRequestStartOverride();
	static bool TryConsumePreparedRequestStartOverride(
		const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request,
		FOsvayderUERestartSurvivalPreparedStartOverride& OutOverride);

	static bool HasPendingResume(EOsvayderUEProviderBackend Backend);
	static bool TryMarkReattached(EOsvayderUEProviderBackend ExpectedBackend, const FString& PresentedToken, FString& OutNotice, FString& OutError);
	static bool TryMarkReattachedFromManualReopen(EOsvayderUEProviderBackend ExpectedBackend, FString& OutNotice, FString& OutError);
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
