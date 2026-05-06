// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderEditorWidget.h"
#include "OsvayderCodeRunner.h"
#include "OsvayderSubsystem.h"
#include "CodexCliRunner.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "OsvayderUEVoiceDictationService.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUEModule.h"
#include "OsvayderUERelayAgent.h"
#include "OsvayderUEConstants.h"
#include "OsvayderUETaskRecoveryDetector.h"   // 632 Task Recovery & Rehydration
#include "OsvayderUETaskRecoveryDialog.h"     // 632 Task Recovery & Rehydration
#include "OsvayderUETaskRecoverySummarizer.h" // 632 Task Recovery & Rehydration
#include "OsvayderUEDeferredTasksBrowser.h"   // 634 Deferred Tasks Browser & Resume
#include "OsvayderUERestartSurvival.h"
#include "OsvayderUECanonRouting.h"
#include "OsvayderUECommandClassification.h"
#include "OsvayderUERecipeRegistry.h"
#include "OsvayderUERoleRegistry.h"
#include "OsvayderUEUserFacingStatus.h"
#include "AgentExecutionControl.h"
#include "CompileIntentPolicyGate.h" // 626 P3 compile-intent policy gate (widget-side annotation hook)
#include "OsvayderUESettings.h"
#include "ProjectContext.h"
#include "MCP/OsvayderUEMCPServer.h"
#include "MCP/MCPToolRegistry.h"
#include "Widgets/SOsvayderToolbar.h"
#include "Widgets/SOsvayderInputArea.h"
#include "Widgets/OsvayderSlateStyle.h"
#include "Widgets/SOsvayderToolCallRow.h"

#include <initializer_list>

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/MessageDialog.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"
#include "Internationalization/Regex.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "OsvayderUE"

// Static text styles for selectable text (SMultiLineEditableText needs persistent style pointers)
namespace
{
	TWeakPtr<SOsvayderEditorWidget> GLiveOsvayderEditorWidget;

	const FTextBlockStyle& GetSelectableNormalStyle()
	{
		static FTextBlockStyle Style;
		static bool bInit = false;
		if (!bInit)
		{
			Style = FAppStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");
			Style.SetColorAndOpacity(OsvayderSlateStyle::SubtleText());
			bInit = true;
		}
		return Style;
	}

	const FTextBlockStyle& GetSelectableCodeStyle()
	{
		static FTextBlockStyle Style;
		static bool bInit = false;
		if (!bInit)
		{
			Style = FAppStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");
			Style.SetFont(FCoreStyle::GetDefaultFontStyle("Mono", 9));
			Style.SetColorAndOpacity(FSlateColor(FLinearColor(0.780f, 0.860f, 0.820f, 1.0f)));
			bInit = true;
		}
		return Style;
	}

	FAgentBackendStatus GetConfiguredBackendStatus()
	{
		return FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
	}

	FString GetConfiguredBackendDisplayName()
	{
		const FAgentBackendStatus Status = GetConfiguredBackendStatus();
		return Status.DisplayName.IsEmpty() ? TEXT("Assistant backend") : Status.DisplayName;
	}

	bool IsRestartSurvivalTaskRecoveryReason(const FString& Reason)
	{
		return Reason == TEXT("previous_restart_survival_failed")
			|| Reason == TEXT("previous_restart_survival_stuck_or_crashed");
	}

	FString BuildRestartSurvivalTaskRecoveryDiagnostic(const FString& InterruptionReason)
	{
		if (!IsRestartSurvivalTaskRecoveryReason(InterruptionReason))
		{
			return FString();
		}

		FOsvayderUERestartSurvivalState RestartState;
		FString LoadError;
		const bool bHasRestartState =
			FOsvayderUERestartSurvivalManager::LoadState(RestartState, LoadError);

		if (InterruptionReason == TEXT("previous_restart_survival_failed"))
		{
			FString FailureDetail = TEXT("failure");
			if (bHasRestartState)
			{
				if (!RestartState.FailureReason.IsEmpty())
				{
					FailureDetail =
						FString::Printf(TEXT("failure (%s)"), *RestartState.FailureReason);
				}
				else if (!RestartState.DetachedTerminalOutcome.IsEmpty())
				{
					FailureDetail = FString::Printf(
						TEXT("terminal outcome %s"),
						*RestartState.DetachedTerminalOutcome);
				}
			}

			return FString::Printf(
				TEXT("Note: the previous restart-survival cycle ended in %s. Your task was interrupted during closed-editor build."),
				*FailureDetail);
		}

		FString StuckDetail = TEXT("stuck or crashed before reattach");
		if (bHasRestartState)
		{
			const FString Phase = FString(OsvayderUERestartSurvivalPhaseToString(RestartState.Phase));
			StuckDetail = FString::Printf(TEXT("stuck in phase %s"), *Phase);
			if (!RestartState.LastUpdatedAtUtc.IsEmpty())
			{
				StuckDetail = FString::Printf(
					TEXT("%s (last update %s)"),
					*StuckDetail,
					*RestartState.LastUpdatedAtUtc);
			}
		}

		return FString::Printf(
			TEXT("Note: the previous restart-survival cycle appears %s. Your task was interrupted during closed-editor build."),
			*StuckDetail);
	}

	FAgentBackendCapabilities GetConfiguredBackendCapabilities()
	{
		return GetConfiguredBackendStatus().Capabilities;
	}

	FString GetBackendDisplayNameFromStatus(const FAgentBackendStatus& Status)
	{
		return Status.DisplayName.IsEmpty() ? TEXT("Assistant backend") : Status.DisplayName;
	}

	FString GetSessionMetadataDisplayName(const FAgentSessionMetadata& Metadata)
	{
		if (!Metadata.BackendDisplayName.IsEmpty())
		{
			return Metadata.BackendDisplayName;
		}

		return Metadata.Backend == EOsvayderUEProviderBackend::CodexCli
			? TEXT("Codex CLI")
			: TEXT("Claude CLI");
	}

	FString GetDictationStateLabel(const EOsvayderUEVoiceDictationState State)
	{
		switch (State)
		{
		case EOsvayderUEVoiceDictationState::Recording:
			return TEXT("recording");

		case EOsvayderUEVoiceDictationState::PreparingRuntime:
			return TEXT("preparing_runtime");

		case EOsvayderUEVoiceDictationState::Transcribing:
			return TEXT("transcribing");

		case EOsvayderUEVoiceDictationState::Failed:
			return TEXT("failed");

		case EOsvayderUEVoiceDictationState::Unavailable:
			return TEXT("unavailable");

		case EOsvayderUEVoiceDictationState::Idle:
		default:
			return TEXT("idle");
		}
	}

	TSharedPtr<FJsonObject> BuildSessionMetadataTraceObject(const FAgentSessionMetadata& Metadata)
	{
		TSharedPtr<FJsonObject> SessionObject = MakeShared<FJsonObject>();
		SessionObject->SetBoolField(TEXT("exists"), Metadata.bHasSession);
	SessionObject->SetBoolField(TEXT("readable"), Metadata.bIsReadable);
	SessionObject->SetBoolField(TEXT("legacy_shared_file"), Metadata.bIsLegacySharedFile);
	SessionObject->SetStringField(TEXT("backend"), OsvayderUEProviderBackendToString(Metadata.Backend));
	SessionObject->SetStringField(TEXT("backend_display_name"), GetSessionMetadataDisplayName(Metadata));
	SessionObject->SetStringField(TEXT("store_kind"), Metadata.StoreKind);
	SessionObject->SetStringField(TEXT("model"), Metadata.Model);
		SessionObject->SetStringField(TEXT("profile"), Metadata.Profile);
		SessionObject->SetStringField(TEXT("auth_mode"), Metadata.AuthMode);
		SessionObject->SetStringField(TEXT("path"), Metadata.SessionFilePath);
		SessionObject->SetStringField(TEXT("last_updated"), Metadata.LastUpdated);
		SessionObject->SetNumberField(TEXT("message_count"), Metadata.MessageCount);
		SessionObject->SetStringField(TEXT("detail"), Metadata.Detail);
		return SessionObject;
	}

	FString BuildProviderCapabilitySummary(const FAgentBackendCapabilities& Capabilities)
	{
		TArray<FString> Controls;

		if (Capabilities.bSupportsProfileSelection)
		{
			Controls.Add(TEXT("profile"));
		}
		if (Capabilities.bSupportsExplicitAuthModeSelection)
		{
			Controls.Add(TEXT("auth mode"));
		}
		if (Capabilities.bSupportsSpeedModeControl)
		{
			Controls.Add(TEXT("speed"));
		}
		if (Capabilities.bSupportsReasoningEffortControl)
		{
			Controls.Add(TEXT("reasoning"));
		}
		if (Capabilities.bSupportsVerbosityControl)
		{
			Controls.Add(TEXT("verbosity"));
		}
		if (Capabilities.bSupportsBrowserVerifyLogin)
		{
			Controls.Add(TEXT("browser verify"));
		}
		if (Capabilities.bSupportsProviderPersistentThreads)
		{
			Controls.Add(TEXT("persistent threads"));
		}

		return Controls.Num() > 0
			? FString::Join(Controls, TEXT(" + "))
			: TEXT("shared chat/session UX");
	}

	FString NormalizeStatusDetail(FString Detail)
	{
		Detail.ReplaceInline(TEXT("\r"), TEXT(" "));
		Detail.ReplaceInline(TEXT("\n"), TEXT(" "));
		while (Detail.Contains(TEXT("  ")))
		{
			Detail.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		Detail.TrimStartAndEndInline();
		return Detail;
	}

	FString BuildProviderSummaryText(const FAgentBackendStatus& Status)
	{
		const FString DisplayName = GetBackendDisplayNameFromStatus(Status);

		switch (Status.Readiness)
		{
		case EAgentBackendReadiness::Ready:
			return FString::Printf(TEXT("%s ready"), *DisplayName);

		case EAgentBackendReadiness::AvailableAuthUnknown:
			return FString::Printf(TEXT("%s auth check"), *DisplayName);

		case EAgentBackendReadiness::AvailableNotAuthenticated:
			return FString::Printf(TEXT("%s login required"), *DisplayName);

		case EAgentBackendReadiness::NotAvailable:
		default:
			return FString::Printf(TEXT("%s unavailable"), *DisplayName);
		}
	}

	FString GetConfiguredToolbarModelLabel()
	{
		const UOsvayderUESettings* Settings = GetDefault<UOsvayderUESettings>();
		if (Settings == nullptr)
		{
			return FString();
		}

		FString ModelLabel = Settings->GetConfiguredModelForBackend(GetConfiguredBackendStatus().Backend);
		ModelLabel = NormalizeStatusDetail(ModelLabel);
		return ModelLabel;
	}

	FString GetConfiguredToolbarProfileLabel()
	{
		const FAgentBackendStatus Status = GetConfiguredBackendStatus();
		if (!Status.Capabilities.bSupportsProfileSelection)
		{
			return FString();
		}

		const UOsvayderUESettings* Settings = GetDefault<UOsvayderUESettings>();
		if (Settings == nullptr)
		{
			return FString();
		}

		FString ProfileLabel = Settings->GetConfiguredProfileLabelForBackend(Status.Backend);
		ProfileLabel = NormalizeStatusDetail(ProfileLabel);
		return ProfileLabel;
	}

	FText BuildToolbarModelToolTip()
	{
		const FString ModelLabel = GetConfiguredToolbarModelLabel();
		if (ModelLabel.IsEmpty())
		{
			return FText::GetEmpty();
		}

		const FString DisplayName = GetBackendDisplayNameFromStatus(GetConfiguredBackendStatus());
		return FText::FromString(FString::Printf(TEXT("Configured %s model: %s"), *DisplayName, *ModelLabel));
	}

	FText BuildToolbarProfileToolTip()
	{
		const FString ProfileLabel = GetConfiguredToolbarProfileLabel();
		if (ProfileLabel.IsEmpty())
		{
			return FText::GetEmpty();
		}

		const FString DisplayName = GetBackendDisplayNameFromStatus(GetConfiguredBackendStatus());
		return FText::FromString(FString::Printf(TEXT("Configured %s profile: %s"), *DisplayName, *ProfileLabel));
	}

	bool CanSendPromptForStatus(const FAgentBackendStatus& Status)
	{
		return AgentBackendCanExecutePrompt(Status);
	}

	FText GetIdleBackendStatusText(const FAgentBackendStatus& Status)
	{
		const FString DisplayName = Status.DisplayName.IsEmpty() ? TEXT("Assistant backend") : Status.DisplayName;

		switch (Status.Readiness)
		{
		case EAgentBackendReadiness::Ready:
			return FText::FromString(FString::Printf(TEXT("[ready] %s ready"), *DisplayName));

		case EAgentBackendReadiness::AvailableAuthUnknown:
			return FText::FromString(FString::Printf(TEXT("[auth?] %s auth unconfirmed"), *DisplayName));

		case EAgentBackendReadiness::AvailableNotAuthenticated:
			return FText::FromString(FString::Printf(TEXT("[login] %s login required"), *DisplayName));

		case EAgentBackendReadiness::NotAvailable:
		default:
			return FText::FromString(FString::Printf(TEXT("[offline] %s unavailable"), *DisplayName));
		}
	}

	FString GetPluginBuildSyncSummary()
	{
		const FOsvayderUEBuildSyncStatus BuildSyncStatus = FOsvayderUEModule::GetBuildSyncStatus();
		if (BuildSyncStatus.bFresh)
		{
			return FString();
		}

		return FString::Printf(
			TEXT("Plugin build status: STALE\n%s"),
			*BuildSyncStatus.Detail);
	}

	FString GetRestartSurvivalPowerShellExecutablePath()
	{
#if PLATFORM_WINDOWS
		const FString SystemRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot"));
		if (!SystemRoot.IsEmpty())
		{
			const FString Candidate = FPaths::Combine(SystemRoot, TEXT("System32"), TEXT("WindowsPowerShell"), TEXT("v1.0"), TEXT("powershell.exe"));
			if (FPaths::FileExists(Candidate))
			{
				return Candidate;
			}
		}
#endif
		return TEXT("powershell.exe");
	}

	FString GetRestartSurvivalEditorExecutablePath()
	{
		return FPlatformProcess::ExecutablePath();
	}

	FString GetRestartSurvivalEngineRoot()
	{
		const FString EditorExecutablePath = GetRestartSurvivalEditorExecutablePath();
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(EditorExecutablePath), TEXT(".."), TEXT(".."), TEXT("..")));
	}

	FString GetRestartSurvivalProjectEditorTargetName()
	{
		return FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) + TEXT("Editor");
	}

	FString MakeRestartSurvivalStableId(const FString& Prefix)
	{
		return Prefix + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	FString BuildRestartSurvivalDefaultReattachNotice(const FString& TaskId)
	{
		return FString::Printf(
			TEXT("Reopening editor and reattaching to task %s. Detached file/build/recovery work completed while the editor was closed."),
			*TaskId);
	}

	bool IsRestartSurvivalOwnerProcessRunning(const int32 ProcessId)
	{
		if (ProcessId <= 0)
		{
			return false;
		}

		FProcHandle ProcessHandle = FPlatformProcess::OpenProcess(static_cast<uint32>(ProcessId));
		if (!ProcessHandle.IsValid())
		{
			return false;
		}

		const bool bIsRunning = FPlatformProcess::IsProcRunning(ProcessHandle);
		FPlatformProcess::CloseProc(ProcessHandle);
		return bIsRunning;
	}

	bool IsPreparedRequestFileWriteEnabled(const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request)
	{
		return Request.bFileWriteEnabled
			|| !Request.FileWriteSourcePath.IsEmpty()
			|| !Request.FileWriteTargetPath.IsEmpty()
			|| !Request.FileWriteBackupPath.IsEmpty();
	}

	FString BuildClosedEditorBuildBlockerFailFastMessage(
		const FOsvayderUEClosedEditorBuildBlocker& Blocker,
		const FString& EscalationFailureReason)
	{
		return FString::Printf(
			TEXT("Working in editor hit an open-editor build lock (%s). Continuing outside the editor could not be armed, so the task stopped truthfully. Need your review before continuing. %s"),
			*Blocker.FamilyLabel,
			*EscalationFailureReason);
	}

	FString SummarizeDirtyPackagesForUser(const TArray<UPackage*>& DirtyPackages, const int32 MaxPackages = 3)
	{
		TArray<FString> PackageNames;
		PackageNames.Reserve(FMath::Min(DirtyPackages.Num(), MaxPackages));
		for (UPackage* DirtyPackage : DirtyPackages)
		{
			if (DirtyPackage == nullptr)
			{
				continue;
			}

			const FString PackageName = DirtyPackage->GetName();
			if (PackageName.IsEmpty())
			{
				continue;
			}

			PackageNames.AddUnique(PackageName);
			if (PackageNames.Num() >= MaxPackages)
			{
				break;
			}
		}

		return PackageNames.Num() > 0
			? FString::Join(PackageNames, TEXT(", "))
			: FString();
	}

	FOpenEditorBuildLockCloseSafetyInputs BuildOpenEditorBuildLockCloseSafetyInputsFromEditorState()
	{
		FOpenEditorBuildLockCloseSafetyInputs Inputs;

		TArray<UPackage*> DirtyPackages;
		FEditorFileUtils::GetDirtyPackages(DirtyPackages);
		DirtyPackages.Remove(nullptr);
		Inputs.DirtyPackageCount = DirtyPackages.Num();
		Inputs.DirtyPackageSummary = SummarizeDirtyPackagesForUser(DirtyPackages);

		if (GEditor != nullptr)
		{
			Inputs.bPlaySessionActive =
				GEditor->IsPlaySessionInProgress()
				|| GEditor->PlayWorld != nullptr
				|| GEditor->bIsSimulatingInEditor;
		}

		if (FSlateApplication::IsInitialized())
		{
			const TSharedPtr<SWindow> ActiveModalWindow = FSlateApplication::Get().GetActiveModalWindow();
			Inputs.bModalWindowActive = ActiveModalWindow.IsValid();
			if (ActiveModalWindow.IsValid())
			{
				Inputs.ModalWindowTitle = ActiveModalWindow->GetTitle().ToString();
			}
		}

		FOsvayderUERestartSurvivalState ExistingRestartState;
		if (FOsvayderUERestartSurvivalManager::DescribeCurrentState(ExistingRestartState)
			&& !FOsvayderUERestartSurvivalManager::CanReplaceExistingStateForFreshStart(ExistingRestartState))
		{
			Inputs.bRestartSurvivalStateActive = true;
			Inputs.RestartSurvivalPhase = FString(OsvayderUERestartSurvivalPhaseToString(ExistingRestartState.Phase));
		}

		return Inputs;
	}

	bool ShouldRouteClosedEditorBuildBlockerToRelay(const FAgentCanonExecution& Execution)
	{
		return Execution.TaskMode.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase)
			|| Execution.TaskMode.Equals(TEXT("feature_slice"), ESearchCase::IgnoreCase)
			|| Execution.TaskMode.Equals(TEXT("directed_execution"), ESearchCase::IgnoreCase)
			|| Execution.TaskMode.Equals(TEXT("bounded_unreal_mutation"), ESearchCase::IgnoreCase)
			|| Execution.TaskMode.Equals(TEXT("restart_survival_eligible_blocker"), ESearchCase::IgnoreCase)
			|| Execution.RequestedToolFamily.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase);
	}

	bool ResetRelayArtifactsForFreshRun(FString& OutError)
	{
		TArray<FString> Errors;
		FString Error;
		if (!FOsvayderUERelayAgentManager::DeleteCancelRequest(Error) && !Error.IsEmpty())
		{
			Errors.Add(Error);
		}

		Error.Reset();
		if (!FOsvayderUERelayAgentManager::DeleteRelayResult(Error) && !Error.IsEmpty())
		{
			Errors.Add(Error);
		}

		Error.Reset();
		if (!FOsvayderUERelayAgentManager::DeleteProgressLog(Error) && !Error.IsEmpty())
		{
			Errors.Add(Error);
		}

		Error.Reset();
		if (!FOsvayderUERelayAgentManager::DeleteHandoffContext(Error) && !Error.IsEmpty())
		{
			Errors.Add(Error);
		}

		if (Errors.Num() == 0)
		{
			OutError.Reset();
			return true;
		}

		OutError = FString::Join(Errors, TEXT(" "));
		return false;
	}

	FOsvayderUERelayHandoffContext BuildClosedEditorRelayHandoffContext(
		const FOsvayderUERestartSurvivalPreparedRestoreRequest& PreparedRequest,
		const FString& ReattachToken,
		const FString& ReattachNotice)
	{
	FOsvayderUERelayHandoffContext Context;
	Context.TaskId = PreparedRequest.TaskId;
	Context.RelaySessionId = FString::Printf(TEXT("relay_%s"), *PreparedRequest.TaskId);
	Context.ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Context.UProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	Context.ReattachToken = ReattachToken;
	Context.ReattachNotice = ReattachNotice;
	Context.CreatedAtUtc = FDateTime::UtcNow().ToIso8601();
	Context.OriginalUserPrompt = PreparedRequest.OriginTask.OriginatingUserPrompt;
	Context.OriginPromptHash = PreparedRequest.OriginTask.OriginatingPromptHash;
	Context.bVisualProofRequired = PreparedRequest.OriginTask.bVisualProofRequired;
	Context.bVisualQaManifestRequired =
		PreparedRequest.OriginTask.bVisualQaManifestRequired || PreparedRequest.OriginTask.bVisualProofRequired;
	Context.AttachedImagePaths = PreparedRequest.OriginTask.OriginatingAttachedImagePaths;
	Context.AttachmentNames = PreparedRequest.OriginTask.OriginatingAttachmentNames;
	Context.EditorAgentSummary = TEXT("Editor-agent detected a closed-editor build blocker and is handing the same project-local build/fix objective to relay-agent while Unreal is closed.");
	Context.LastKnownBlockerFamily = TEXT("closed_editor_build_blocker");
	Context.LastKnownBlockerSignature = PreparedRequest.Detail;
	Context.KnownFacts = {
		FString::Printf(TEXT("originating_run_id=%s"), *PreparedRequest.OriginTask.OriginatingRunId),
		FString::Printf(TEXT("originating_prompt_hash=%s"), *PreparedRequest.OriginTask.OriginatingPromptHash),
		FString::Printf(TEXT("originating_task_mode=%s"), *PreparedRequest.OriginTask.OriginatingTaskMode),
		FString::Printf(TEXT("originating_requested_tool_family=%s"), *PreparedRequest.OriginTask.OriginatingRequestedToolFamily),
		FString::Printf(TEXT("visual_proof_required=%s"), PreparedRequest.OriginTask.bVisualProofRequired ? TEXT("true") : TEXT("false")),
		FString::Printf(TEXT("visual_qa_manifest_required=%s"), Context.bVisualQaManifestRequired ? TEXT("true") : TEXT("false")),
		FString::Printf(TEXT("attached_image_count=%d"), PreparedRequest.OriginTask.OriginatingAttachedImagePaths.Num()),
		FString::Printf(TEXT("prepared_request_detail=%s"), *PreparedRequest.Detail)
	};
	Context.RelevantArtifactPaths = {
		FOsvayderUERestartSurvivalManager::GetStatePath(),
		FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath(),
		FOsvayderUERestartSurvivalManager::GetClosedEditorResultPath(),
		FOsvayderUERelayAgentManager::GetHandoffContextPath(),
		FOsvayderUERelayAgentManager::GetRelayProgressPath(),
		FOsvayderUERelayAgentManager::GetRelayResultPath(),
			FPaths::Combine(Context.ProjectRoot, TEXT("Saved"), TEXT("Logs")),
			FPaths::Combine(Context.ProjectRoot, TEXT("Saved"), TEXT("UnrealBuildTool"), FString::Printf(TEXT("%s-current-build.log"), *GetRestartSurvivalProjectEditorTargetName())),
			FPaths::Combine(Context.ProjectRoot, TEXT("Saved"), TEXT("UnrealBuildTool"), FString::Printf(TEXT("%s-current-build.json"), *GetRestartSurvivalProjectEditorTargetName()))
		};
		Context.RelevantToolReceipts = {
			FString::Printf(TEXT("prepared_restore_request_id=%s"), *PreparedRequest.RequestId),
			FString::Printf(TEXT("linked_provider_session_id=%s"), *PreparedRequest.LinkedProviderSessionId),
			FString::Printf(TEXT("prepared_request_detail=%s"), *PreparedRequest.Detail)
		};
		Context.NextAttemptHypothesis = TEXT("While Unreal is closed, use only project-local source/generated/build artifacts to drive the same task to a truthful build terminal result instead of stopping at the first editor-open blocker.");
		Context.BoundedObjective = TEXT("closed_editor_complex_build_relay_v1");
		Context.BoundedObjectiveDetail = TEXT("Continue the same project-local build/fix task while Unreal is closed using project source files, generated intermediates, build scripts, and build/log artifacts only. No editor/MCP work until final relaunch.");
		Context.ReasoningIterationBudget = 10;
		Context.WallClockBudgetSeconds = 900;
		Context.Settings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
		return Context;
	}

	bool IsRestartSurvivalContinuationPromptForState(
		const FString& CurrentPrompt,
		const FOsvayderUERestartSurvivalState& State)
	{
		if (CurrentPrompt.IsEmpty() || State.PostReattachCompletionText.IsEmpty())
		{
			return false;
		}

		if (CurrentPrompt.Equals(State.PostReattachCompletionText, ESearchCase::CaseSensitive))
		{
			return true;
		}

		const FString ContinuationPrefix = TEXT("Continue the same ordinary bounded project-local task after restart-survival reattach.");
		return CurrentPrompt.StartsWith(ContinuationPrefix, ESearchCase::CaseSensitive)
			&& State.PostReattachCompletionText.StartsWith(ContinuationPrefix, ESearchCase::CaseSensitive);
	}

	void BuildEffectiveClosedEditorOriginTaskContext(
		const EOsvayderUEProviderBackend Backend,
		const FString& CurrentPrompt,
		const TArray<FString>& CurrentImagePaths,
		const FString& ActiveRunId,
		const FAgentCanonExecution& ActiveCanonExecution,
		FOsvayderUERestartSurvivalOriginTaskContext& OutOriginTask,
		FAgentCanonExecution& OutEffectiveCanonExecution)
	{
		OutOriginTask = FOsvayderUERestartSurvivalOriginTaskContext();
		OutOriginTask.OriginatingRunId = ActiveRunId;
		OutOriginTask.OriginatingUserPrompt = CurrentPrompt;
		OutOriginTask.OriginatingPromptHash = SOsvayderEditorWidget::ComputeHeadlessAcceptancePromptHash(CurrentPrompt);
		OutOriginTask.OriginatingTaskMode = ActiveCanonExecution.TaskMode;
		OutOriginTask.OriginatingRequestedToolFamily = ActiveCanonExecution.RequestedToolFamily;
		OutOriginTask.OriginatingPrimaryMutationToolFamily = ActiveCanonExecution.PrimaryMutationToolFamily;
		for (const FString& ImagePath : CurrentImagePaths)
		{
			const FString NormalizedPath = FPaths::ConvertRelativePathToFull(ImagePath);
			OutOriginTask.OriginatingAttachedImagePaths.Add(NormalizedPath);
			OutOriginTask.OriginatingAttachmentNames.Add(FPaths::GetCleanFilename(NormalizedPath));
		}
		OutOriginTask.bOriginatingHasAttachments = OutOriginTask.OriginatingAttachedImagePaths.Num() > 0;
		OutOriginTask.bOriginatingHasVisualReference =
			SOsvayderEditorWidget::DoesPromptRequireVisualProof(CurrentPrompt, CurrentImagePaths);
		OutOriginTask.bVisualProofRequired = OutOriginTask.bOriginatingHasVisualReference;
		OutOriginTask.bVisualQaManifestRequired = OutOriginTask.bVisualProofRequired;
		if (OutOriginTask.bVisualProofRequired)
		{
			OutOriginTask.VisualReferenceRequirement =
				TEXT("Final acceptance requires a visual_qa_manifest.json with verdict=passed and actual_screenshot_paths, or an explicit visual-proof blocker.");
		}
		OutEffectiveCanonExecution = ActiveCanonExecution;

		FOsvayderUERestartSurvivalState RestartState;
		FString LoadError;
		if (!FOsvayderUERestartSurvivalManager::LoadState(RestartState, LoadError)
			|| RestartState.Backend != Backend
			|| RestartState.Phase != EOsvayderUERestartSurvivalPhase::Reattached
			|| !RestartState.bPostReattachCompletionDispatched
			|| RestartState.OriginTask.OriginatingUserPrompt.IsEmpty()
			|| !IsRestartSurvivalContinuationPromptForState(CurrentPrompt, RestartState))
		{
			return;
		}

		OutOriginTask = RestartState.OriginTask;
		if (!RestartState.OriginTask.OriginatingTaskMode.IsEmpty())
		{
			OutEffectiveCanonExecution.TaskMode = RestartState.OriginTask.OriginatingTaskMode;
		}
		if (!RestartState.OriginTask.OriginatingRequestedToolFamily.IsEmpty())
		{
			OutEffectiveCanonExecution.RequestedToolFamily = RestartState.OriginTask.OriginatingRequestedToolFamily;
		}
		if (!RestartState.OriginTask.OriginatingPrimaryMutationToolFamily.IsEmpty())
		{
			OutEffectiveCanonExecution.PrimaryMutationToolFamily = RestartState.OriginTask.OriginatingPrimaryMutationToolFamily;
		}
	}

	FString BuildRestartSurvivalRelaunchArguments(const FString& ReattachToken, const FString& AdditionalArguments)
	{
		FString Arguments = FString::Printf(TEXT("-OsvayderUERestartReattachToken=\"%s\""), *ReattachToken);
		if (!AdditionalArguments.IsEmpty())
		{
			Arguments += TEXT(" ");
			Arguments += AdditionalArguments;
		}
		return Arguments;
	}

	bool TryGetRestartSurvivalTokenFromCommandLine(FString& OutToken)
	{
		OutToken.Reset();
		return FParse::Value(FCommandLine::Get(), TEXT("OsvayderUERestartReattachToken="), OutToken);
	}

	FString MakeUtcNowText();
	FString NormalizeSingleLineText(FString Value);
	bool IsVisualQaManifestRequiredForPlan(const FOsvayderUEActivePlan& Plan);
	void ApplyVisualQaManifestEvidenceFromResponse(FOsvayderUEActivePlan& Plan, const FString& Response);
	FString BuildPlanId();
	FOsvayderUEActivePlan BuildFeatureWorkflowPlan(const FString& OriginalUserTask, const FAgentFeatureWorkflowState& Workflow);
	FOsvayderUEActivePlan BuildDefaultActivePlan(const FString& OriginalUserTask);
	int32 FindMechanicIndex(const FOsvayderUEActivePlan& Plan, const FString& MechanicId);
	void SetMechanicStatus(
		FOsvayderUEActivePlan& Plan,
		const FString& MechanicId,
		const FString& Status,
		const FString& Summary,
		const FString& SummaryRu,
		bool bMarkCompleted);
	void SetCurrentMechanic(
		FOsvayderUEActivePlan& Plan,
		const FString& MechanicId,
		const FString& Summary,
		const FString& SummaryRu,
		const FString& TechnicalDetail);
	void MarkLeadingMechanicCompleted(FOsvayderUEActivePlan& Plan, const FString& MechanicId);
	void EnsureToolCallEntryExists(
		FOsvayderUEActivePlan& Plan,
		const FString& ToolCallId,
		const FString& MechanicId,
		const FString& ToolName);
	bool ToolNameMatches(const FString& ToolName, const FString& CanonicalName);
	bool IsCompileProofTool(const FString& ToolName);
	bool IsPostCompileVerificationTool(const FString& ToolName);
	bool ToolInputMentionsCompiledModuleSource(const FString& ToolInput);
	bool ToolInputTargetsCompiledModuleSourceMutation(const FString& ToolName, const FString& ToolInput);
	bool IsWorkspaceFileBuildExecution(const FAgentCanonExecution& Execution);
	bool IsWorkspaceFileBuildRestartOrigin(const FOsvayderUERestartSurvivalState& RestartState);
	int32 CompareIsoUtcTexts(const FString& Left, const FString& Right);
	bool IsIsoUtcAfter(const FString& Left, const FString& Right);
	void RecordCompileProof(
		FOsvayderUECompileProofState& State,
		const FString& TimestampUtc,
		const FString& ToolCallId,
		const FString& ToolName,
		const FString& Outcome,
		const FString& Detail);
	void RecordPostCompileVerification(
		FOsvayderUECompileProofState& State,
		const FString& TimestampUtc,
		const FString& ToolCallId,
		const FString& ToolName,
		const FString& Outcome);
	bool IsFeatureWorkflowPlan(const FOsvayderUEActivePlan& Plan);
	FString GetCurrentFeatureWorkflowPhaseId(const FOsvayderUEActivePlan& Plan);
	void UpdateFeatureWorkflowForToolBoundary(FOsvayderUEActivePlan& Plan, const FAgentCanonExecution& ActiveCanon, const FOsvayderStreamEvent& Event, bool bStarting);
	bool SaveCloseoutJsonObjectToFile(const TSharedPtr<FJsonObject>& JsonObject, const FString& FilePath, FString& OutError);
	bool SaveHeadlessAcceptanceReceiptObject(const TSharedPtr<FJsonObject>& JsonObject, const FString& FilePath, FString& OutError);
	bool TryLoadHeadlessAcceptanceReceiptStartedAt(const FString& ReceiptPath, FString& OutStartedAtUtc, FDateTime& OutStartedAtDateTime);
	FString GetHeadlessAcceptancePendingReceiptPath();
	bool SaveHeadlessAcceptancePendingReceiptObject(const TSharedPtr<FJsonObject>& JsonObject);
	void DeleteHeadlessAcceptancePendingReceipt();
	bool TryLoadPendingHeadlessAcceptanceReceiptContext(FOsvayderUEHeadlessAcceptanceReceiptContext& OutContext);
	bool TryCompleteRecoveredHeadlessAcceptanceReceipt(
		const FString& ResponsePreview,
		bool bAssistantSuccess,
		const FOsvayderUEActivePlanCloseoutDecision& CloseoutDecision,
		bool& bOutRequestEditorExit,
		FString& OutError);
	bool TryLoadLatestHeadlessCloseoutDecision(FOsvayderUEActivePlanCloseoutDecision& OutDecision, FString& OutRawJson);
	TArray<FString> CollectCurrentPrefixLogPaths(const FString& Prefix, const FDateTime& StartedAtUtc);
	TArray<TSharedPtr<FJsonValue>> MakeCloseoutStringArrayJson(const TArray<FString>& Values);
	FString SerializeCloseoutJsonObjectCompact(const TSharedPtr<FJsonObject>& Object);
	FString FindActivePlanArchivePath(const TArray<FString>& ArchivedPaths);
	FString MakeCloseoutSafeFileTag(const FString& Value);
	FString BuildRunIsolatedCloseoutArchiveRunTag(const FOsvayderUERunCloseoutContext& Context);
	FString GetCloseoutProjectRoot();
	bool ArchiveActivePlanContinuitySnapshotForNewSession(FString& OutArchivePath, FString& OutError);
	bool TryRestoreExplicitContinuityPlanForPrompt(
		const FString& Prompt,
		FOsvayderUEActivePlan& OutPlan,
		FString& OutError);
	void AppendContinuationPromptToPlanIfRelevant(FOsvayderUEActivePlan& Plan, const FString& Prompt);
	FString DescribeSettingsParity(const FOsvayderUERelaySettingsSnapshot& Expected, const FOsvayderUERelaySettingsSnapshot& Current);
	FString DescribeTaskLaneStatusInternal(const FOsvayderUETaskLaneState& LaneState)
	{
		const bool bBlocked =
			LaneState.TransitionState.Equals(TEXT("blocked"), ESearchCase::CaseSensitive);
		const bool bReturningToEditor =
			LaneState.TargetLane.Equals(TEXT("live_editor"), ESearchCase::CaseSensitive)
			|| (LaneState.TransitionKind.Equals(TEXT("lane_return"), ESearchCase::CaseSensitive)
				&& LaneState.TransitionState.Equals(TEXT("in_progress"), ESearchCase::CaseSensitive));
		const bool bDetachedLaneActive =
			LaneState.GetEffectiveCurrentLane().Equals(TEXT("closed_editor_detached"), ESearchCase::CaseSensitive)
			|| LaneState.TargetLane.Equals(TEXT("closed_editor_detached"), ESearchCase::CaseSensitive)
			|| (LaneState.TransitionKind.Equals(TEXT("lane_escalation"), ESearchCase::CaseSensitive)
				&& (LaneState.TransitionState.Equals(TEXT("armed"), ESearchCase::CaseSensitive)
					|| LaneState.TransitionState.Equals(TEXT("in_progress"), ESearchCase::CaseSensitive)));

		if (bBlocked)
		{
			return TEXT("Need your review before continuing");
		}
		if (bReturningToEditor)
		{
			return TEXT("Reopening editor and reattaching");
		}
		if (bDetachedLaneActive)
		{
			return TEXT("Continuing outside the editor");
		}

		return TEXT("Working in editor");
	}

	void MirrorPlanLaneStateToFeatureWorkflow(FOsvayderUEActivePlan& Plan)
	{
		if (!Plan.FeatureWorkflow.HasAnySignal())
		{
			return;
		}

		Plan.FeatureWorkflow.LaneState = Plan.LaneState;
	}

	void RefreshPlanLaneStateIdentity(FOsvayderUEActivePlan& Plan)
	{
		if (Plan.LaneState.CurrentLane.IsEmpty())
		{
			Plan.LaneState.CurrentLane = TEXT("live_editor");
		}
		if (Plan.LaneState.TransitionKind.IsEmpty())
		{
			Plan.LaneState.TransitionKind = TEXT("none");
		}
		if (Plan.LaneState.TransitionState.IsEmpty())
		{
			Plan.LaneState.TransitionState = TEXT("steady");
		}
		if (Plan.LaneState.ExpectedReturnCondition.IsEmpty())
		{
			Plan.LaneState.ExpectedReturnCondition =
				TEXT("Finish the current task in the editor unless a bounded closed-editor continuation is armed.");
		}

		Plan.LaneState.ContinuityPlanId = Plan.PlanId;
		if (IsFeatureWorkflowPlan(Plan))
		{
			Plan.LaneState.ContinuityWorkflowId = Plan.FeatureWorkflow.FeatureWorkflowId;
			Plan.LaneState.ContinuityPhaseId = GetCurrentFeatureWorkflowPhaseId(Plan);
		}
		else
		{
			Plan.LaneState.ContinuityWorkflowId.Reset();
			Plan.LaneState.ContinuityPhaseId = Plan.CurrentMechanicId;
		}

		if (!Plan.ResumeHint.IsEmpty())
		{
			Plan.LaneState.ContinuationIntent = Plan.ResumeHint;
		}

		MirrorPlanLaneStateToFeatureWorkflow(Plan);
	}

	void AssignPlanLaneState(
		FOsvayderUEActivePlan& Plan,
		const FString& CurrentLane,
		const FString& TargetLane,
		const FString& ExpectedReturnLane,
		const FString& TransitionKind,
		const FString& TransitionState,
		const FString& TransitionReason,
		const FString& BlockerFamily,
		const FString& ContinuityTaskId,
		const FString& ExpectedReturnCondition)
	{
		Plan.LaneState.CurrentLane = CurrentLane;
		Plan.LaneState.TargetLane = TargetLane;
		Plan.LaneState.ExpectedReturnLane = ExpectedReturnLane;
		Plan.LaneState.TransitionKind = TransitionKind;
		Plan.LaneState.TransitionState = TransitionState;
		Plan.LaneState.TransitionReason = TransitionReason;
		Plan.LaneState.BlockerFamily = BlockerFamily;
		if (!ContinuityTaskId.IsEmpty())
		{
			Plan.LaneState.ContinuityTaskId = ContinuityTaskId;
		}
		Plan.LaneState.ExpectedReturnCondition = ExpectedReturnCondition;
		Plan.LaneState.ContinuationIntent = Plan.ResumeHint;
		RefreshPlanLaneStateIdentity(Plan);
	}

	FString DescribeTransportFailureFamilyForUser(const FString& FailureFamily)
	{
		if (FailureFamily.Equals(TEXT("persistent_transport_timeout"), ESearchCase::CaseSensitive))
		{
			return TEXT("backend transport timeout");
		}
		if (FailureFamily.Equals(TEXT("persistent_transport_websocket_reset"), ESearchCase::CaseSensitive))
		{
			return TEXT("backend websocket reset");
		}
		if (FailureFamily.Equals(TEXT("persistent_transport_process_exit"), ESearchCase::CaseSensitive))
		{
			return TEXT("backend app-server exit");
		}
		if (FailureFamily.Equals(TEXT("persistent_transport_write_failure"), ESearchCase::CaseSensitive))
		{
			return TEXT("backend transport write failure");
		}
		if (FailureFamily.Equals(TEXT("persistent_transport_app_server_error"), ESearchCase::CaseSensitive))
		{
			return TEXT("backend app-server transport error");
		}

		return TEXT("backend transport reset");
	}

	FSlateColor GetToolRunningStatusColor()
	{
		return OsvayderSlateStyle::AccentCyanText();
	}

	FSlateColor GetToolCompletedStatusColor()
	{
		return FSlateColor(OsvayderSlateStyle::Color::Success());
	}

	FSlateColor GetToolBlockedStatusColor()
	{
		return FSlateColor(OsvayderSlateStyle::Color::Blocked());
	}

	FSlateColor GetToolFailedStatusColor()
	{
		return FSlateColor(OsvayderSlateStyle::Color::Error());
	}

	bool ToolResultLooksBlocked(const FString& ResultContent)
	{
		return ResultContent.Contains(TEXT("blocked"), ESearchCase::IgnoreCase)
			|| ResultContent.Contains(TEXT("blocker"), ESearchCase::IgnoreCase)
			|| ResultContent.Contains(TEXT("denied"), ESearchCase::IgnoreCase)
			|| ResultContent.Contains(TEXT("not allowed"), ESearchCase::IgnoreCase);
	}
}

// ============================================================================
// SChatMessage
// ============================================================================

void SChatMessage::Construct(const FArguments& InArgs)
{
	bool bIsUser = InArgs._IsUser;
	FString Message = InArgs._Message;
	const FString AssistantLabel = InArgs._AssistantLabel;

	const FLinearColor AccentColor = bIsUser
		? OsvayderSlateStyle::Color::Cyan()
		: OsvayderSlateStyle::Color::Violet();
	const FSlateBrush* MessageBrush = bIsUser
		? OsvayderSlateStyle::UserMessageBrush()
		: OsvayderSlateStyle::AssistantMessageBrush();
	const FSlateColor RoleLabelColor = bIsUser
		? OsvayderSlateStyle::AccentCyanText()
		: OsvayderSlateStyle::AccentVioletText();

	FString RoleLabel = bIsUser
		? TEXT("You")
		: (AssistantLabel.IsEmpty() ? TEXT("Assistant") : AssistantLabel);

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(AccentColor)
			.Padding(FMargin(2.0f, 0.0f))
			[
				SNullWidget::NullWidget
			]
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SBorder)
			.BorderImage(MessageBrush)
			.Padding(FMargin(12.0f, 9.0f, 12.0f, 10.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 7.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SBorder)
						.BorderImage(bIsUser ? OsvayderSlateStyle::CyanChipBrush() : OsvayderSlateStyle::VioletChipBrush())
						.Padding(OsvayderSlateStyle::ChipPadding())
						[
							SNew(STextBlock)
							.Text(FText::FromString(RoleLabel))
							.TextStyle(FAppStyle::Get(), "SmallText")
							.ColorAndOpacity(RoleLabelColor)
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(bIsUser ? LOCTEXT("UserMessageMeta", "Prompt") : LOCTEXT("AssistantMessageMeta", "Response"))
						.TextStyle(FAppStyle::Get(), "SmallText")
						.ColorAndOpacity(OsvayderSlateStyle::MutedText())
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SMultiLineEditableText)
					.Text(FText::FromString(Message))
					.TextStyle(&GetSelectableNormalStyle())
					.IsReadOnly(true)
					.AutoWrapText(true)
				]
			]
		]
	];
}

// ============================================================================
// SOsvayderEditorWidget
// ============================================================================

void SOsvayderEditorWidget::Construct(const FArguments& InArgs)
{
	VoiceDictationService = MakeShared<FOsvayderUEVoiceDictationService>();
	VoiceDictationService->SetStatusChangedHandler([this](const FOsvayderUEVoiceDictationStatus& Status)
	{
		HandleDictationStatusChanged(Status);
	});
	VoiceDictationService->SetTranscriptReadyHandler([this](const FString& Transcript)
	{
		HandleDictationTranscriptReady(Transcript);
	});

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildToolbar()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildCodexAuthDiagnosticsPanel()
		]
		// 606 Part B (2026-04-18): Agent 2 background banner — only visible when
		// supervisor is alive in a non-terminal phase. AutoHeight + per-tick
		// Visibility binding means zero space cost in the happy path.
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildAgent2BackgroundBanner()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			BuildChatArea()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f)
		[
			BuildInputArea()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildStatusBar()
		]
	];

	LastRenderedBackend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
	ActiveRequestBackend = LastRenderedBackend;
	LastBackendSnapshotPollTime = FPlatformTime::Seconds();
	LastBackendSnapshotSignature = BuildBackendSnapshotSignature();
	if (!TryRestoreSessionOnOpen())
	{
		AddInitialStatusMessage();
	}
}

void SOsvayderEditorWidget::RegisterLiveWidget(const TSharedRef<SOsvayderEditorWidget>& Widget)
{
	GLiveOsvayderEditorWidget = Widget;
}

TSharedPtr<SOsvayderEditorWidget> SOsvayderEditorWidget::GetLiveWidget()
{
	return GLiveOsvayderEditorWidget.Pin();
}

SOsvayderEditorWidget::~SOsvayderEditorWidget()
{
	// Cancel any pending requests
	FOsvayderCodeSubsystem::Get().CancelCurrentRequest();

	if (VoiceDictationService.IsValid())
	{
		VoiceDictationService->Shutdown();
		VoiceDictationService.Reset();
	}

	if (GLiveOsvayderEditorWidget.Pin().Get() == this)
	{
		GLiveOsvayderEditorWidget.Reset();
	}
}

void SOsvayderEditorWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (VoiceDictationService.IsValid())
	{
		VoiceDictationService->Tick();
	}
	TickHeadlessAcceptanceBridgeTimeout(InCurrentTime);

	// 632 Task Recovery & Rehydration: one-shot startup check. Runs on first
	// Tick after widget Construct (guaranteed after Slate app is live + modal
	// windows can be added). The check itself is fast (single JSON read) and
	// returns immediately if no interruption metadata is present.
	if (!bTaskRecoveryStartupCheckCompleted)
	{
		TryRunTaskRecoveryStartupCheck();
	}

	const EOsvayderUEProviderBackend CurrentBackend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
	if (CurrentBackend != LastRenderedBackend)
	{
		const EOsvayderUEProviderBackend PreviousBackend = LastRenderedBackend;
		LastRenderedBackend = CurrentBackend;
		LastBackendSnapshotSignature.Empty();
		HandleConfiguredBackendChanged(PreviousBackend);
	}

	if (InCurrentTime - LastBackendSnapshotPollTime < 1.0)
	{
		return;
	}

	LastBackendSnapshotPollTime = InCurrentTime;
	RefreshBackendSnapshotIfChanged();
	PollRestartSurvivalDetachedOwnerState();
}

void SOsvayderEditorWidget::TryRunTaskRecoveryStartupCheck()
{
	// Mark completed first so we never re-enter on subsequent Ticks — even if
	// the plan read fails or the dialog throws, we only try once per widget
	// lifetime.
	bTaskRecoveryStartupCheckCompleted = true;

	// Load plan. Missing / unparseable / no-interruption-metadata → silent
	// no-op; the detector probe at StartupModule tail already decided whether
	// to write interruption fields. The widget is just the UI surface.
	FOsvayderUEActivePlan Plan;
	FString LoadError;
	if (!FOsvayderUERelayAgentManager::LoadActivePlan(Plan, LoadError))
	{
		return;
	}

	// Two gating conditions: detector must have flagged interruption AND user
	// must not have already chosen. This lets the user re-enter the widget
	// many times without the dialog re-appearing (choice persists).
	if (Plan.InterruptionDetectedAtUtc.IsEmpty())
	{
		return;
	}
	if (!Plan.UserRecoveryChoice.IsEmpty())
	{
		return;
	}

	// Build summary + show dialog.
	const FString EvidencePath = FOsvayderUERelayAgentManager::GetActivePlanPath();
	const FTaskRecoverySummary Summary =
		FOsvayderUETaskRecoverySummarizer::BuildRecoverySummary(
			Plan, FDateTime::UtcNow(), EvidencePath);
	const FString DiagnosticNote =
		BuildRestartSurvivalTaskRecoveryDiagnostic(Plan.InterruptionReason);

	const FOsvayderUETaskRecoveryDialog::FDialogResult DialogResult =
		FOsvayderUETaskRecoveryDialog::Show(Summary, DiagnosticNote);

	// Record choice into the plan and route side-effect per choice.
	switch (DialogResult.Choice)
	{
	case EOsvayderUETaskRecoveryChoice::Continue:
	{
		Plan.UserRecoveryChoice = TEXT("continue");
		FString SaveError;
		FOsvayderUERelayAgentManager::SaveActivePlan(Plan, SaveError);

		const FString ContextText =
			FOsvayderUETaskRecoveryContextBuilder::BuildContextBlockText(Summary);
		FOsvayderCodeSubsystem::Get().SetPendingTaskRecoveryContext(ContextText);
		bResumeExistingActivePlanOnNextSend = true;
		bUsePostReattachResumePolicyOnNextSend = false;

		const FAgentBackendStatus Status = GetConfiguredBackendStatus();
		const bool bUnsafeTransportRetry =
			LastTransportFailureState.bActive && !LastTransportFailureState.bRetrySafe;
		const FTaskRecoveryAutoDispatchDecision AutoDispatchDecision =
			EvaluateTaskRecoveryAutoDispatch(
				bIsWaitingForResponse,
				CanSendPromptForStatus(Status),
				Status.Detail,
				bUnsafeTransportRetry,
				LastTransportFailureState.RetryBlockReason);

		// Forensic event.
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("plan_id"), Plan.PlanId);
		Payload->SetStringField(TEXT("choice"), TEXT("continue"));
		if (AutoDispatchDecision.bCanAutoDispatch)
		{
			AddMessage(TEXT("Task recovery: auto-resuming the interrupted task now."), false);

			FString DispatchError;
			if (DispatchPreparedPrompt(
				BuildTaskRecoveryAutoResumePrompt(),
				TArray<FString>(),
				false,
				FString(),
				FString(),
				BuildTaskRecoveryAutoResumeVisibleMarker(),
				false,
				DispatchError))
			{
				Payload->SetStringField(TEXT("auto_dispatch_outcome"), TEXT("started"));
			}
			else
			{
				Payload->SetStringField(TEXT("auto_dispatch_outcome"), TEXT("failed_to_start"));
				Payload->SetStringField(TEXT("auto_dispatch_block_reason"), TEXT("dispatch_start_failed"));
				Payload->SetStringField(TEXT("auto_dispatch_detail"), DispatchError);
				AddMessage(
					FString::Printf(
						TEXT("Task recovery Continue is armed, but auto-resume could not start: %s"),
						*DispatchError),
					false);
			}
		}
		else
		{
			Payload->SetStringField(TEXT("auto_dispatch_outcome"), TEXT("blocked"));
			Payload->SetStringField(TEXT("auto_dispatch_block_reason"), AutoDispatchDecision.BlockReasonCode);
			Payload->SetStringField(TEXT("auto_dispatch_detail"), AutoDispatchDecision.UserFacingMessage);
			AddMessage(AutoDispatchDecision.UserFacingMessage, false);
		}

		FOsvayderUEAgentTraceLog::Get().AppendEvent(
			TEXT("task_recovery_user_choice"),
			FOsvayderCodeSubsystem::Get().GetConfiguredBackend(),
			Payload);
		break;
	}
	case EOsvayderUETaskRecoveryChoice::StartFresh:
	{
		// Mark choice + archive + reset active plan. We don't implement a
		// full PlanArchives move here (dispatch scope is "archive" — we
		// persist the choice which is enough for forensic audit; existing
		// archive infrastructure can pick up stalled plans later).
		Plan.UserRecoveryChoice = TEXT("start_fresh");
		Plan.Status = TEXT("abandoned_for_fresh_session");
		FString SaveError;
		FOsvayderUERelayAgentManager::SaveActivePlan(Plan, SaveError);

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("plan_id"), Plan.PlanId);
		Payload->SetStringField(TEXT("choice"), TEXT("start_fresh"));
		FOsvayderUEAgentTraceLog::Get().AppendEvent(
			TEXT("task_recovery_abandoned"),
			FOsvayderCodeSubsystem::Get().GetConfiguredBackend(),
			Payload);

		AddMessage(TEXT("Task recovery: previous task archived, starting with a fresh session."), false);
		break;
	}
	case EOsvayderUETaskRecoveryChoice::CloseAsIrrelevant:
	{
		Plan.UserRecoveryChoice = TEXT("closed_as_irrelevant");
		Plan.UserClosedReason = DialogResult.UserClosedReason;
		Plan.Status = TEXT("abandoned_by_user");
		FString SaveError;
		FOsvayderUERelayAgentManager::SaveActivePlan(Plan, SaveError);

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("plan_id"), Plan.PlanId);
		Payload->SetStringField(TEXT("choice"), TEXT("closed_as_irrelevant"));
		Payload->SetStringField(TEXT("user_closed_reason"), Plan.UserClosedReason);
		FOsvayderUEAgentTraceLog::Get().AppendEvent(
			TEXT("task_recovery_closed"),
			FOsvayderCodeSubsystem::Get().GetConfiguredBackend(),
			Payload);

		AddMessage(TEXT("Task recovery: previous task marked irrelevant and closed."), false);
		break;
	}
	case EOsvayderUETaskRecoveryChoice::None:
	default:
		// User closed the dialog without picking a button. Leave plan state
		// as-is so the dialog shows again on next widget launch. No persisted
		// choice = no regression.
		UE_LOG(LogOsvayderUE, Verbose,
			TEXT("TaskRecoveryDialog: user dismissed without choosing; will re-prompt next widget construct."));
		break;
	}
}

TSharedRef<SWidget> SOsvayderEditorWidget::BuildToolbar()
{
	NormalizeMandatoryContextFlagsForNormalAssistantRun(bIncludeUE57Context, bIncludeProjectContext);

	return SNew(SOsvayderToolbar)
		.TitleText(this, &SOsvayderEditorWidget::GetToolbarTitleText)
		.ProviderSummaryText(this, &SOsvayderEditorWidget::GetProviderSummaryText)
		.ProviderSummaryToolTip(this, &SOsvayderEditorWidget::GetProviderSummaryToolTip)
		.ModelText_Lambda([]() { return FText::FromString(GetConfiguredToolbarModelLabel()); })
		.ModelToolTip_Lambda([]() { return BuildToolbarModelToolTip(); })
		.ProfileText_Lambda([]() { return FText::FromString(GetConfiguredToolbarProfileLabel()); })
		.ProfileToolTip_Lambda([]() { return BuildToolbarProfileToolTip(); })
		.bUE57ContextEnabled_Lambda([this]() { return bIncludeUE57Context; })
		.bProjectContextEnabled_Lambda([this]() { return bIncludeProjectContext; })
		.bRestoreEnabled_Lambda([this]()
		{
			const FAgentSavedSessionIndex SavedSessions = FOsvayderCodeSubsystem::Get().DescribeSavedSessions();
			const FAgentSavedSessionIndex ExpertSessions =
				FOsvayderCodeSubsystem::Get().DescribeSavedSessionsForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);
			return SavedSessions.HasAnySavedSession() || ExpertSessions.CurrentProviderSession.bHasSession;
		})
		.RestoreToolTip(this, &SOsvayderEditorWidget::GetRestoreSessionToolTip)
		.bRestartSurvivalEnabled_Lambda([this]() { return CanStartRestartSurvival(); })
		.RestartSurvivalToolTip(this, &SOsvayderEditorWidget::GetRestartSurvivalToolTip)
		.DeferredTasksCount_Lambda([this]() { return GetDeferredTasksCount(); })
		.bShowBrowserVerify_Lambda([this]() { return ShouldShowBrowserVerifyButton(); })
		.bBrowserVerifyEnabled_Lambda([this]() { return CanLaunchBrowserVerifyButton(); })
		.BrowserVerifyToolTip_Lambda([this]() { return GetBrowserVerifyToolTip(); })
		.OnUE57ContextChanged_Lambda([this](bool bEnabled) { bIncludeUE57Context = bEnabled; })
		.OnProjectContextChanged_Lambda([this](bool bEnabled) { bIncludeProjectContext = bEnabled; })
		.OnRefreshContext_Lambda([this]() { RefreshProjectContext(); })
		.OnRestoreSession_Lambda([this]() { RestoreSession(); })
		.OnRestartSurvival_Lambda([this]() { StartRestartSurvival(); })
		.OnOpenDeferredTasks_Lambda([this]() { OpenDeferredTasksBrowser(); })
		.OnBrowserVerify_Lambda([this]() { LaunchBrowserVerifyLogin(); })
		.OnNewSession_Lambda([this]() { NewSession(); })
		.OnCopyLast_Lambda([this]() { CopyToClipboard(); });
}

TSharedRef<SWidget> SOsvayderEditorWidget::BuildCodexAuthDiagnosticsPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.0f, 4.0f))
		.Visibility(this, &SOsvayderEditorWidget::GetCodexAuthDiagnosticsVisibility)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SOsvayderEditorWidget::GetCodexAuthDiagnosticsText)
				.ToolTipText(this, &SOsvayderEditorWidget::GetCodexAuthDiagnosticsToolTip)
				.TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.78f, 0.82f)))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SComboButton)
				.ToolTipText(FText::FromString(TEXT("Show Codex authentication diagnostics actions.")))
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Auth Actions")))
				]
				.MenuContent()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(8.0f, 6.0f, 8.0f, 2.0f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Advanced Auth")))
						.TextStyle(FAppStyle::Get(), "SmallText")
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(8.0f, 2.0f))
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Open Auth Folder")))
						.ToolTipText(FText::FromString(TEXT("Open the effective CODEX_HOME folder that plugin launches use.")))
						.OnClicked_Lambda([this]() { OpenCodexAuthFolder(); return FReply::Handled(); })
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(8.0f, 2.0f))
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Relogin Codex")))
						.ToolTipText(FText::FromString(TEXT("Open Codex browser login using the same effective CODEX_HOME as plugin launches.")))
						.OnClicked_Lambda([this]() { LaunchCodexRelogin(); return FReply::Handled(); })
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(8.0f, 2.0f))
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Backup/Clear Stale Auth")))
						.ToolTipText(FText::FromString(TEXT("Explicit action only: back up auth.json/credentials.json under Saved diagnostics, then remove only those known Codex auth artifacts from effective CODEX_HOME.")))
						.OnClicked_Lambda([this]() { BackupAndClearCodexAuth(); return FReply::Handled(); })
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(8.0f, 2.0f, 8.0f, 8.0f))
					[
						SNew(SButton)
						.Text_Lambda([this]()
						{
							return bCodexAuthProbeRunning
								? FText::FromString(TEXT("Probing..."))
								: FText::FromString(TEXT("Probe Backend Auth"));
						})
						.ToolTipText(FText::FromString(TEXT("Start codex app-server and a read-only thread/start probe to classify auth separately from transport failure.")))
						.IsEnabled(this, &SOsvayderEditorWidget::CanProbeCodexBackendAuth)
						.OnClicked_Lambda([this]() { ProbeCodexBackendAuth(); return FReply::Handled(); })
					]
				]
			]
		];
}

int32 SOsvayderEditorWidget::GetDeferredTasksCount() const
{
	return FOsvayderUEDeferredTasksEnumerator::CountDeferredPlans();
}

void SOsvayderEditorWidget::OpenDeferredTasksBrowser()
{
	FOsvayderUEDeferredTasksBrowser::Show(
		FOsvayderUEDeferredTasksBrowser::FOnPlanResumed::CreateLambda([this](const FString& ShortTitle)
		{
			bResumeExistingActivePlanOnNextSend = true;
			bUsePostReattachResumePolicyOnNextSend = false;
			AddMessage(FString::Printf(TEXT("Resumed deferred task: %s"), *ShortTitle), false);
		}));
}

TSharedRef<SWidget> SOsvayderEditorWidget::BuildChatArea()
{
	return SNew(SBorder)
		.BorderImage(OsvayderSlateStyle::ChatSurfaceBrush())
		.Padding(6.0f)
		[
			SAssignNew(ChatScrollBox, SScrollBox)
			+ SScrollBox::Slot()
			[
				SAssignNew(ChatMessagesBox, SVerticalBox)
			]
		];
}

TSharedRef<SWidget> SOsvayderEditorWidget::BuildInputArea()
{
	SAssignNew(InputArea, SOsvayderInputArea)
		.bIsWaiting_Lambda([this]() { return bIsWaitingForResponse; })
		.OnSend_Lambda([this]() { SendMessage(); })
		.OnCancel_Lambda([this]() { CancelRequest(); })
		.OnToggleDictation_Lambda([this]() { ToggleDictation(); })
		.DictationState_Lambda([this]()
		{
			return VoiceDictationService.IsValid()
				? VoiceDictationService->GetStatus().State
				: EOsvayderUEVoiceDictationState::Idle;
		})
		.DictationStatusText_Lambda([this]() { return GetDictationStatusText(); })
		.OnTextChanged_Lambda([this](const FString& Text) { CurrentInputText = Text; });

	return InputArea.ToSharedRef();
}

TSharedRef<SWidget> SOsvayderEditorWidget::BuildStatusBar()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.0f, 4.0f))
		[
			SNew(SHorizontalBox)
			
			// Status indicator
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SBox)
				.WidthOverride(10.0f)
				.HeightOverride(10.0f)
				.ToolTipText(this, &SOsvayderEditorWidget::GetConnectionIndicatorToolTip)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(this, &SOsvayderEditorWidget::GetConnectionIndicatorColor)
					.Padding(0.0f)
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SOsvayderEditorWidget::GetStatusText)
				.ColorAndOpacity(this, &SOsvayderEditorWidget::GetStatusColor)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Retry Last")))
				.Visibility(this, &SOsvayderEditorWidget::GetRetryLastTransportVisibility)
				.IsEnabled(this, &SOsvayderEditorWidget::CanRetryLastTransportRequest)
				.ToolTipText(this, &SOsvayderEditorWidget::GetRetryLastTransportToolTip)
				.OnClicked(this, &SOsvayderEditorWidget::HandleRetryLastTransportRequest)
			]
			
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNullWidget::NullWidget
			]
			
			// Project path (convert to absolute and shorten home dir to ~/)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([]() -> FText {
					FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
					FString HomeDir = FPlatformProcess::UserHomeDir();
					// Normalize: strip any trailing slashes so the replacement is consistent
					// regardless of whether UserHomeDir() returns "/Users/x" or "/Users/x/"
					while (HomeDir.EndsWith(TEXT("/")))
					{
						HomeDir.LeftChopInline(1);
					}
					if (ProjectPath.StartsWith(HomeDir))
					{
						ProjectPath = TEXT("~") + ProjectPath.RightChop(HomeDir.Len());
					}
					return FText::FromString(ProjectPath);
				})
				.TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
			]
		];
}

void SOsvayderEditorWidget::AddMessage(const FString& Message, bool bIsUser)
{
	if (ChatMessagesBox.IsValid())
	{
		// Add a thin separator line between messages for visual clarity
		if (ChatMessagesBox->NumSlots() > 0)
		{
			ChatMessagesBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(8.0f, 2.0f))
			[
			SNew(SSeparator)
			.ColorAndOpacity(FLinearColor(0.090f, 0.105f, 0.130f, 0.8f))
			];
		}

		ChatMessagesBox->AddSlot()
		.AutoHeight()
		.Padding(FMargin(4.0f, 6.0f, 4.0f, 6.0f))
		[
			SNew(SChatMessage)
			.Message(Message)
			.IsUser(bIsUser)
			.AssistantLabel(GetConfiguredBackendDisplayName())
		];

		// Scroll to bottom
		if (ChatScrollBox.IsValid())
		{
			ChatScrollBox->ScrollToEnd();
		}

		TrackVisibleChatMessage(Message);
	}
}

void SOsvayderEditorWidget::TrackVisibleChatMessage(const FString& Message)
{
	VisibleChatMessages.Add(Message);
}

void SOsvayderEditorWidget::ReplaceStreamingResponseWithFinalText(const FString& Message)
{
	if (!StreamingContentBox.IsValid())
	{
		return;
	}

	StreamingContentBox->ClearChildren();

	TSharedPtr<SVerticalBox> SegmentContainer;
	StreamingTextBlock.Reset();

	StreamingContentBox->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Text(FText::FromString(GetConfiguredBackendDisplayName()))
		.TextStyle(FAppStyle::Get(), "SmallText")
		.ColorAndOpacity(OsvayderSlateStyle::AccentVioletText())
	];

	StreamingContentBox->AddSlot()
	.AutoHeight()
	[
		SAssignNew(SegmentContainer, SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(StreamingTextBlock, SMultiLineEditableText)
			.Text(FText::FromString(Message))
			.TextStyle(&GetSelectableNormalStyle())
			.IsReadOnly(true)
			.AutoWrapText(true)
		]
	];

	StreamingResponse = Message;
	CurrentSegmentText = Message;
	ToolCallStatusLabels.Empty();
	ToolCallResultTexts.Empty();
	ToolCallExpandables.Empty();
	ToolCallNames.Empty();
	AllTextSegments.Reset();
	TextSegmentBlocks.Reset();
	TextSegmentContainers.Reset();
	ToolGroupExpandArea.Reset();
	ToolGroupInnerBox.Reset();
	ToolGroupSummaryText.Reset();
	ToolGroupCount = 0;
	ToolGroupDoneCount = 0;
	ToolGroupCallIds.Reset();
	StreamingToolCallCount = 0;
	LastResultStats.Empty();

	TextSegmentBlocks.Add(StreamingTextBlock);
	TextSegmentContainers.Add(SegmentContainer);

	if (ChatScrollBox.IsValid())
	{
		ChatScrollBox->ScrollToEnd();
	}
}

static FString GetTypedResultStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(FieldName, Value);
	}
	return Value;
}

static bool LoadJsonObjectForTypedResult(const FString& Path, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
{
	OutObject.Reset();
	if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path))
	{
		OutError = FString::Printf(TEXT("Typed closed-editor result is missing: %s"), *Path);
		return false;
	}

	FString RawJson;
	if (!FFileHelper::LoadFileToString(RawJson, *Path))
	{
		OutError = FString::Printf(TEXT("Could not read typed closed-editor result: %s"), *Path);
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
	if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not parse typed closed-editor result JSON: %s"), *Path);
		return false;
	}
	return true;
}

static bool IsTypedClosedEditorTerminalSuccess(const FString& TerminalOutcome, const FString& Status)
{
	return TerminalOutcome.Equals(TEXT("success"), ESearchCase::IgnoreCase)
		|| TerminalOutcome.Equals(TEXT("terminal_success"), ESearchCase::IgnoreCase)
		|| Status.Equals(TEXT("success"), ESearchCase::IgnoreCase)
		|| Status.Equals(TEXT("completed"), ESearchCase::IgnoreCase)
		|| Status.Equals(TEXT("succeeded"), ESearchCase::IgnoreCase);
}

static bool DoesTypedClosedEditorResultMatchState(
	const TSharedPtr<FJsonObject>& ResultObject,
	const FOsvayderUERestartSurvivalState& State,
	const FString& SourcePath,
	const bool bRequireTerminalSuccess,
	FString& OutDetail)
{
	const FString TaskId = GetTypedResultStringField(ResultObject, TEXT("task_id"));
	if (TaskId.IsEmpty() || !TaskId.Equals(State.TaskId, ESearchCase::CaseSensitive))
	{
		OutDetail = FString::Printf(
			TEXT("%s task_id mismatch: result task_id=%s; restart task_id=%s"),
			*SourcePath,
			*TaskId,
			*State.TaskId);
		return false;
	}

	const FString ExpectedPromptHash = State.OriginTask.OriginatingPromptHash;
	const FString ResultPromptHash = GetTypedResultStringField(ResultObject, TEXT("origin_prompt_hash"));
	if (!ExpectedPromptHash.IsEmpty()
		&& (ResultPromptHash.IsEmpty() || !ResultPromptHash.Equals(ExpectedPromptHash, ESearchCase::CaseSensitive)))
	{
		OutDetail = FString::Printf(
			TEXT("%s origin_prompt_hash mismatch or missing: result=%s; expected=%s"),
			*SourcePath,
			*ResultPromptHash,
			*ExpectedPromptHash);
		return false;
	}

	const FString TerminalOutcome = GetTypedResultStringField(ResultObject, TEXT("terminal_outcome"));
	const FString Status = GetTypedResultStringField(ResultObject, TEXT("status"));
	if (TerminalOutcome.IsEmpty() && Status.IsEmpty())
	{
		OutDetail = FString::Printf(TEXT("%s has no terminal_outcome/status."), *SourcePath);
		return false;
	}

	if (bRequireTerminalSuccess && !IsTypedClosedEditorTerminalSuccess(TerminalOutcome, Status))
	{
		OutDetail = FString::Printf(
			TEXT("%s is typed and current, but not a success proof: terminal_outcome=%s; status=%s"),
			*SourcePath,
			*TerminalOutcome,
			*Status);
		return false;
	}

	OutDetail = FString::Printf(
		TEXT("typed closed-editor result accepted: %s; terminal_outcome=%s; status=%s"),
		*SourcePath,
		*TerminalOutcome,
		*Status);
	return true;
}

static bool TryGetCurrentTypedClosedEditorResultProof(const bool bRequireTerminalSuccess, FString& OutDetail)
{
	FOsvayderUERestartSurvivalState State;
	FString StateError;
	if (!FOsvayderUERestartSurvivalManager::LoadState(State, StateError) || State.TaskId.IsEmpty())
	{
		OutDetail = StateError.IsEmpty()
			? TEXT("No restart-survival state/task_id is available for typed closed-editor result validation.")
			: StateError;
		return false;
	}

	TArray<FString> CandidatePaths;
	CandidatePaths.Add(FOsvayderUERestartSurvivalManager::GetClosedEditorResultPath());
	CandidatePaths.Add(FOsvayderUERelayAgentManager::GetRelayResultPath());

	FString LastDetail;
	for (const FString& CandidatePath : CandidatePaths)
	{
		TSharedPtr<FJsonObject> ResultObject;
		FString LoadError;
		if (!LoadJsonObjectForTypedResult(CandidatePath, ResultObject, LoadError))
		{
			LastDetail = LoadError;
			continue;
		}

		if (DoesTypedClosedEditorResultMatchState(ResultObject, State, CandidatePath, bRequireTerminalSuccess, LastDetail))
		{
			OutDetail = LastDetail;
			return true;
		}
	}

	OutDetail = LastDetail.IsEmpty()
		? TEXT("No current typed closed-editor result matched restart-survival state.")
		: LastDetail;
	return false;
}

static void ApplyVisualProofRequirementToPlan(
	FOsvayderUEActivePlan& Plan,
	const FString& Prompt,
	const TArray<FString>& ImagePaths)
{
	bool bVisualProofRequired = SOsvayderEditorWidget::DoesPromptRequireVisualProof(Prompt, ImagePaths);
	TArray<FString> ReferencePaths;
	for (const FString& ImagePath : ImagePaths)
	{
		if (!ImagePath.IsEmpty())
		{
			ReferencePaths.AddUnique(FPaths::ConvertRelativePathToFull(ImagePath));
		}
	}

	FOsvayderUERestartSurvivalState RestartState;
	FString RestartStateError;
	if (FOsvayderUERestartSurvivalManager::LoadState(RestartState, RestartStateError)
		&& RestartState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
		&& IsRestartSurvivalContinuationPromptForState(Prompt, RestartState))
	{
		bVisualProofRequired =
			bVisualProofRequired
			|| RestartState.OriginTask.bOriginatingHasVisualReference
			|| RestartState.OriginTask.bVisualProofRequired
			|| RestartState.OriginTask.bVisualQaManifestRequired;
		for (const FString& OriginImagePath : RestartState.OriginTask.OriginatingAttachedImagePaths)
		{
			if (!OriginImagePath.IsEmpty())
			{
				ReferencePaths.AddUnique(FPaths::ConvertRelativePathToFull(OriginImagePath));
			}
		}
		if (Plan.VisualProofRequirement.IsEmpty() && !RestartState.OriginTask.VisualReferenceRequirement.IsEmpty())
		{
			Plan.VisualProofRequirement = RestartState.OriginTask.VisualReferenceRequirement;
		}
	}

	if (!bVisualProofRequired)
	{
		return;
	}

	Plan.bVisualProofRequired = true;
	Plan.bVisualQaManifestRequired = true;
	if (Plan.VisualProofRequirement.IsEmpty())
	{
		Plan.VisualProofRequirement =
			TEXT("Final acceptance requires a visual_qa_manifest.json with verdict=passed and actual_screenshot_paths, or an explicit visual-proof blocker.");
	}
	if (Plan.VisualProofStatus.IsEmpty())
	{
		Plan.VisualProofStatus = TEXT("pending_visual_qa_manifest");
	}
	for (const FString& ReferencePath : ReferencePaths)
	{
		Plan.VisualReferenceArtifactPaths.AddUnique(ReferencePath);
	}
	Plan.VerificationChecklist.AddUnique(Plan.VisualProofRequirement);
}

bool SOsvayderEditorWidget::PrepareActivePlanForCurrentSend(const FString& Prompt, FString& OutError)
{
	FOsvayderUEActivePlan Plan;
	if (bResumeExistingActivePlanOnNextSend)
	{
		// 619 P3 Fix #3: tolerant active_plan.json read. Fallback chain:
		//   1. active_plan.json present -> use it (back-compat, unchanged).
		//   2. Missing -> read state.json.post_reattach_completion_text.
		//   3. Both missing -> synthesize fresh default plan for Prompt
		//      (no [failed] routed to widget; the render path at :3661 is
		//      avoided because this function returns true).
		FOsvayderUERelayAgentManager::EActivePlanFallbackSource FallbackSource
			= FOsvayderUERelayAgentManager::EActivePlanFallbackSource::ActivePlanJson;
		FString FallbackError;
		const bool bFallbackLoaded = FOsvayderUERelayAgentManager::LoadActivePlanWithFallback(
			Prompt, Plan, FallbackSource, FallbackError);

		if (!bFallbackLoaded)
		{
			// LoadActivePlanWithFallback is defined to always return true
			// (NoneRecoverable still returns a usable default plan). A
			// literal `false` here means something truly unexpected went
			// wrong (e.g. plugin teardown); keep the legacy failure path
			// in that specific case so the widget user sees an accurate
			// runtime-connection state.
			OutError = FallbackError.IsEmpty()
				? TEXT("LoadActivePlanWithFallback returned false unexpectedly; no continuation available.")
				: FallbackError;
			return false;
		}

		if (bUsePostReattachResumePolicyOnNextSend)
		{
			FString TypedResultDetail;
			if (!TryGetCurrentTypedClosedEditorResultProof(false, TypedResultDetail))
			{
				OutError = FString::Printf(
					TEXT("Restart-survival reattach continuation is blocked until a current typed closed-editor result is present and matches the original task. %s"),
					*TypedResultDetail);
				return false;
			}
			Plan.VerificationChecklist.AddUnique(FString::Printf(TEXT("Consumed typed closed-editor result: %s"), *TypedResultDetail));
			Plan.ResumeHint = Plan.ResumeHint.IsEmpty()
				? FString::Printf(TEXT("Resume after consuming typed closed-editor result: %s"), *TypedResultDetail)
				: Plan.ResumeHint + TEXT(" ") + FString::Printf(TEXT("Typed closed-editor result consumed: %s"), *TypedResultDetail);
		}

		if (FallbackSource == FOsvayderUERelayAgentManager::EActivePlanFallbackSource::ActivePlanJson)
		{
			if (bUsePostReattachResumePolicyOnNextSend)
			{
				if (IsFeatureWorkflowPlan(Plan))
				{
					const FString PhaseId = GetCurrentFeatureWorkflowPhaseId(Plan);
					SetCurrentMechanic(
						Plan,
						PhaseId,
						TEXT("Resume the persisted feature workflow after reattach."),
						TEXT("Продолжить сохранённый feature workflow после reattach."),
						FString::Printf(
							TEXT("feature_workflow_id=%s; current_phase=%s; resume_rule=same_workflow_same_phase"),
							*Plan.FeatureWorkflow.FeatureWorkflowId,
							*PhaseId));
					Plan.bPostReattachVerificationRequired = false;
					Plan.ResumeHint = FString::Printf(
						TEXT("Resume feature_workflow_id=%s at phase=%s; do not restart free-form re-analysis."),
						*Plan.FeatureWorkflow.FeatureWorkflowId,
						*PhaseId);
				}
				else
				{
					SetCurrentMechanic(
						Plan,
						TEXT("verify_and_report"),
						TEXT("Resume from the exact last tool-call boundary after reattach."),
						TEXT("Продолжить с точной границы последнего завершённого tool call после reattach."),
						TEXT("The reopened editor is resuming the persisted plan from the exact post-reattach verification boundary."));
					Plan.bPostReattachVerificationRequired = true;
					Plan.ResumeHint = TEXT("Continue from the persisted post-reattach verification boundary; do not redo detached work.");
				}
			}
			else if (IsFeatureWorkflowPlan(Plan))
			{
				const FString PhaseId = GetCurrentFeatureWorkflowPhaseId(Plan);
				Plan.bPostReattachVerificationRequired = false;
				Plan.ResumeHint = FString::Printf(
					TEXT("Resume feature_workflow_id=%s at phase=%s from the exact interrupted boundary; do not restart free-form re-analysis."),
					*Plan.FeatureWorkflow.FeatureWorkflowId,
					*PhaseId);
			}
			else
			{
				Plan.bPostReattachVerificationRequired = false;
				Plan.ResumeHint = TEXT("Continue from the exact interrupted boundary recorded in active_plan.json; do not restart the task from scratch.");
			}
		}
		else if (FallbackSource == FOsvayderUERelayAgentManager::EActivePlanFallbackSource::RestartSurvivalStateJsonFallback)
		{
			// State.json fallback: continuation text was recovered but no
			// structured plan exists. Keep the post-reattach verification
			// flag so the user sees a similar resume posture, but the plan
			// is synthesized (BuildFallbackPlan inside the helper).
			SetCurrentMechanic(
				Plan,
				TEXT("verify_and_report"),
				bUsePostReattachResumePolicyOnNextSend
					? TEXT("Resume from the state.json continuation text after reattach (active_plan.json absent).")
					: TEXT("Resume from the recovered continuation text (active_plan.json absent)."),
				bUsePostReattachResumePolicyOnNextSend
					? TEXT("Resume from the state.json continuation text after reattach (active_plan.json absent).")
					: TEXT("Resume from the recovered continuation text (active_plan.json absent)."),
				TEXT("active_plan.json was not present post-reattach; the continuation prompt was recovered from state.json."));
			Plan.bPostReattachVerificationRequired = bUsePostReattachResumePolicyOnNextSend;
		}
		else // NoneRecoverable
		{
			FOsvayderUEActivePlan ContinuityPlan;
			FString ContinuityError;
			if (TryRestoreExplicitContinuityPlanForPrompt(Prompt, ContinuityPlan, ContinuityError))
			{
				Plan = ContinuityPlan;
				AppendContinuationPromptToPlanIfRelevant(Plan, Prompt);
				const FString PhaseId = IsFeatureWorkflowPlan(Plan)
					? GetCurrentFeatureWorkflowPhaseId(Plan)
					: Plan.CurrentMechanicId;
				Plan.bPostReattachVerificationRequired = false;
				Plan.ResumeHint = FString::Printf(
					TEXT("Resume explicitly cited continuity plan_id=%s feature_workflow_id=%s at phase=%s; do not create a replacement feature scope."),
					*Plan.PlanId,
					*Plan.FeatureWorkflow.FeatureWorkflowId,
					*PhaseId);
			}
			else if (!ContinuityError.IsEmpty())
			{
				OutError = ContinuityError;
				return false;
			}
			else
			{
				if (bUsePostReattachResumePolicyOnNextSend)
				{
					OutError = TEXT("Restart-survival reattach continuation is blocked: active_plan.json is missing and no explicit continuity plan or typed closed-editor result can recover the task identity.");
					return false;
				}
				bResumeExistingActivePlanOnNextSend = false;
				bUsePostReattachResumePolicyOnNextSend = false;
			}
		}
	}
	else
	{
		FOsvayderUEActivePlan ContinuityPlan;
		FString ContinuityError;
		if (TryRestoreExplicitContinuityPlanForPrompt(Prompt, ContinuityPlan, ContinuityError))
		{
			Plan = ContinuityPlan;
			AppendContinuationPromptToPlanIfRelevant(Plan, Prompt);
			const FString PhaseId = IsFeatureWorkflowPlan(Plan)
				? GetCurrentFeatureWorkflowPhaseId(Plan)
				: Plan.CurrentMechanicId;
			Plan.ResumeHint = FString::Printf(
				TEXT("Resume explicitly cited continuity plan_id=%s feature_workflow_id=%s at phase=%s; do not create a replacement feature scope."),
				*Plan.PlanId,
				*Plan.FeatureWorkflow.FeatureWorkflowId,
				*PhaseId);
		}
		else if (!ContinuityError.IsEmpty())
		{
			OutError = ContinuityError;
			return false;
		}
		else
		{
			Plan = BuildDefaultActivePlan(Prompt);
		}
	}

	AppendContinuationPromptToPlanIfRelevant(Plan, Prompt);
	ApplyVisualProofRequirementToPlan(Plan, Prompt, CurrentRequestImagePaths);

	if (Plan.ReviewerPlanReference.IsEmpty() || Plan.OriginalUserTask.IsEmpty() || Plan.Mechanics.Num() == 0)
	{
		OutError = TEXT("Refusing to start execution without a detailed persisted reviewer-plan state.");
		return false;
	}

	Plan.Settings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
	RefreshPlanLaneStateIdentity(Plan);
	Plan.UpdatedAtUtc = MakeUtcNowText();
	if (!FOsvayderUERelayAgentManager::SaveActivePlan(Plan, OutError))
	{
		return false;
	}

	bResumeExistingActivePlanOnNextSend = false;
	bUsePostReattachResumePolicyOnNextSend = false;
	return true;
}

void SOsvayderEditorWidget::UpdateActivePlanForToolBoundary(const FOsvayderStreamEvent& Event, const bool bStarting)
{
	FOsvayderUEActivePlan Plan;
	FString Error;
	if (!FOsvayderUERelayAgentManager::LoadActivePlan(Plan, Error))
	{
		return;
	}

	FString EffectiveMechanicId = Plan.CurrentMechanicId;
	const bool bFeatureWorkflowPlan = IsFeatureWorkflowPlan(Plan);
	if (bFeatureWorkflowPlan)
	{
		EffectiveMechanicId = GetCurrentFeatureWorkflowPhaseId(Plan);
	}
	else if (EffectiveMechanicId.IsEmpty())
	{
		EffectiveMechanicId = TEXT("inspect_current_state");
	}
	if (!bFeatureWorkflowPlan && Plan.bHybridSplitTriggered && !Plan.bPostReattachVerificationRequired)
	{
		EffectiveMechanicId = TEXT("perform_bounded_work");
	}
	else if (!bFeatureWorkflowPlan && Plan.bPostReattachVerificationRequired)
	{
		EffectiveMechanicId = TEXT("verify_and_report");
	}

	const FString BoundaryTimestampUtc = MakeUtcNowText();
	const FString ToolBoundaryMechanicId = EffectiveMechanicId;
	if (bFeatureWorkflowPlan)
	{
		FAgentCanonExecution ActiveCanon;
		if (!FOsvayderUEAgentTraceLog::Get().TryGetActiveCanonExecutionForBackend(Event.Backend, ActiveCanon))
		{
			ActiveCanon.TaskMode = TEXT("feature_slice");
			ActiveCanon.RequestedToolFamily = Plan.FeatureWorkflow.RecipeId;
			ActiveCanon.FeatureWorkflow = Plan.FeatureWorkflow;
		}
		UpdateFeatureWorkflowForToolBoundary(Plan, ActiveCanon, Event, bStarting);
	}
	else if (bStarting)
	{
		FAgentCanonExecution ActiveCanon;
		if (FOsvayderUEAgentTraceLog::Get().TryGetActiveCanonExecutionForBackend(Event.Backend, ActiveCanon)
			&& IsWorkspaceFileBuildExecution(ActiveCanon)
			&& OsvayderUECanonRouting::IsMutatingToolUse(Event.ToolName, Event.ToolInput)
			&& !IsCompileProofTool(Event.ToolName)
			&& ToolInputTargetsCompiledModuleSourceMutation(Event.ToolName, Event.ToolInput))
		{
			Plan.CompileProof.bCompiledModuleMutationObserved = true;
			Plan.CompileProof.MutationToolFamily = !ActiveCanon.PrimaryMutationToolFamily.IsEmpty()
				? ActiveCanon.PrimaryMutationToolFamily
				: OsvayderUECanonRouting::DetermineToolFamily(ActiveCanon, Event.ToolName, Event.ToolInput);
			if (Plan.CompileProof.MutationToolFamily.IsEmpty())
			{
				Plan.CompileProof.MutationToolFamily = TEXT("workspace_file_build");
			}
			Plan.CompileProof.LastMutationAtUtc = BoundaryTimestampUtc;
			Plan.CompileProof.LastMutationToolCallId = Event.ToolCallId;
			Plan.CompileProof.LastMutationToolName = Event.ToolName;
		}
	}
	else
	{
		if (ToolNameMatches(Event.ToolName, TEXT("livecoding_compile")))
		{
			RecordCompileProof(
				Plan.CompileProof,
				BoundaryTimestampUtc,
				Event.ToolCallId,
				Event.ToolName,
				Event.bIsError ? TEXT("failed") : TEXT("success"),
				NormalizeSingleLineText(Event.ToolResultContent.Left(400)));
		}

		if (IsPostCompileVerificationTool(Event.ToolName))
		{
			RecordPostCompileVerification(
				Plan.CompileProof,
				BoundaryTimestampUtc,
				Event.ToolCallId,
				Event.ToolName,
				Event.bIsError ? TEXT("fail") : TEXT("pass"));
		}
	}

	EnsureToolCallEntryExists(Plan, Event.ToolCallId, ToolBoundaryMechanicId, Event.ToolName);
	for (FOsvayderUEPlanToolCallEntry& Entry : Plan.ToolCalls)
	{
		if (!Entry.ToolCallId.Equals(Event.ToolCallId, ESearchCase::CaseSensitive))
		{
			continue;
		}

		Entry.MechanicId = ToolBoundaryMechanicId;
		Entry.ToolName = Event.ToolName;
		Entry.Status = bStarting ? TEXT("running") : TEXT("completed");
		Entry.Summary = bStarting
			? FString::Printf(TEXT("Tool %s started."), *Event.ToolName)
			: FString::Printf(TEXT("Tool %s completed."), *Event.ToolName);
		Entry.SummaryRu = bStarting
            ? FString::Printf(TEXT("Запущен инструмент %s."), *GetDisplayToolName(Event.ToolName))
            : FString::Printf(TEXT("Инструмент %s завершён."), *GetDisplayToolName(Event.ToolName));
		Entry.TechnicalDetail = bStarting
			? FString::Printf(TEXT("tool_call_id=%s"), *Event.ToolCallId)
			: NormalizeSingleLineText(Event.ToolResultContent.Left(400));
		if (bStarting)
		{
			if (Entry.StartedAtUtc.IsEmpty())
			{
				Entry.StartedAtUtc = BoundaryTimestampUtc;
			}
			Entry.ResultStatus = TEXT("in_progress");
		}
		else
		{
			Entry.CompletedAtUtc = BoundaryTimestampUtc;
			Entry.ResultStatus = TEXT("completed");
			Plan.LastCompletedToolCallId = Entry.ToolCallId;
		}
		break;
	}

	if (!bFeatureWorkflowPlan)
	{
		SetCurrentMechanic(
			Plan,
			EffectiveMechanicId,
			bStarting
				? FString::Printf(TEXT("Tool %s is running."), *Event.ToolName)
				: FString::Printf(TEXT("Tool %s finished."), *Event.ToolName),
			bStarting
	            ? FString::Printf(TEXT("Инструмент %s выполняется."), *GetDisplayToolName(Event.ToolName))
	            : FString::Printf(TEXT("Инструмент %s завершён."), *GetDisplayToolName(Event.ToolName)),
			bStarting
				? FString::Printf(TEXT("tool_call_id=%s"), *Event.ToolCallId)
				: NormalizeSingleLineText(Event.ToolResultContent.Left(400)));
	}
	Plan.CurrentToolCallId = Event.ToolCallId;
	Plan.UpdatedAtUtc = BoundaryTimestampUtc;
	RefreshPlanLaneStateIdentity(Plan);
	FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error);
}

void SOsvayderEditorWidget::MarkActivePlanAwaitingClosedEditorRelay(
	const FOsvayderUEClosedEditorBuildBlocker& Blocker,
	const FString& ContinuationTaskId,
	const FString& EscalationReason,
	const FString& TechnicalDetail)
{
	FOsvayderUEActivePlan Plan;
	FString Error;
	if (!FOsvayderUERelayAgentManager::LoadActivePlan(Plan, Error))
	{
		return;
	}

	if (IsFeatureWorkflowPlan(Plan))
	{
		const int32 PhaseIndex = FindMechanicIndex(Plan, GetCurrentFeatureWorkflowPhaseId(Plan));
		if (PhaseIndex != INDEX_NONE)
		{
			Plan.Mechanics[PhaseIndex].bRequiresClosedEditor = true;
		}
		SetCurrentMechanic(
			Plan,
			GetCurrentFeatureWorkflowPhaseId(Plan),
			TEXT("Continuing the current feature workflow phase outside the editor."),
			TEXT("Текущая фаза feature workflow продолжится вне редактора."),
			NormalizeSingleLineText(TechnicalDetail));
		Plan.ResumeHint = FString::Printf(
			TEXT("After reattach, resume feature_workflow_id=%s at phase=%s."),
			*Plan.FeatureWorkflow.FeatureWorkflowId,
			*GetCurrentFeatureWorkflowPhaseId(Plan));
	}
	else
	{
		MarkLeadingMechanicCompleted(Plan, TEXT("inspect_current_state"));
		const int32 WorkMechanicIndex = FindMechanicIndex(Plan, TEXT("perform_bounded_work"));
		if (WorkMechanicIndex != INDEX_NONE)
		{
			Plan.Mechanics[WorkMechanicIndex].bRequiresClosedEditor = true;
		}
		SetCurrentMechanic(
			Plan,
			TEXT("perform_bounded_work"),
			TEXT("Continuing the bounded task outside the editor."),
	        TEXT("Ограниченная задача продолжится вне редактора."),
			NormalizeSingleLineText(TechnicalDetail));
		Plan.ResumeHint = TEXT("After reattach, continue from the last closed-editor tool-call boundary instead of restarting the task.");
	}
	AssignPlanLaneState(
		Plan,
		TEXT("live_editor"),
		TEXT("closed_editor_detached"),
		TEXT("live_editor"),
		TEXT("lane_escalation"),
		TEXT("armed"),
		EscalationReason,
		Blocker.FamilyLabel,
		ContinuationTaskId,
		TEXT("Continue the same persisted task outside the editor, then reopen Unreal and resume from the saved boundary."));
	Plan.bHybridSplitTriggered = true;
	Plan.HybridSplitReason = Blocker.FamilyLabel.IsEmpty() ? EscalationReason : Blocker.FamilyLabel;
	Plan.UpdatedAtUtc = MakeUtcNowText();
	FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error);
}

void SOsvayderEditorWidget::MarkActivePlanBlockedOnClosedEditorRelay(
	const FOsvayderUEClosedEditorBuildBlocker& Blocker,
	const FString& EscalationReason,
	const FString& TechnicalDetail)
{
	FOsvayderUEActivePlan Plan;
	FString Error;
	if (!FOsvayderUERelayAgentManager::LoadActivePlan(Plan, Error))
	{
		return;
	}

	if (IsFeatureWorkflowPlan(Plan))
	{
		SetCurrentMechanic(
			Plan,
			GetCurrentFeatureWorkflowPhaseId(Plan),
			TEXT("Need your review before leaving the editor lane."),
			TEXT("Нужна ваша проверка, прежде чем выходить из editor-этапа."),
			NormalizeSingleLineText(TechnicalDetail));
	}
	else
	{
		SetCurrentMechanic(
			Plan,
			Plan.CurrentMechanicId.IsEmpty() ? TEXT("perform_bounded_work") : Plan.CurrentMechanicId,
			TEXT("Need your review before continuing the task outside the editor."),
			TEXT("Нужна ваша проверка, прежде чем продолжать задачу вне редактора."),
			NormalizeSingleLineText(TechnicalDetail));
	}

	AssignPlanLaneState(
		Plan,
		TEXT("live_editor"),
		TEXT("closed_editor_detached"),
		TEXT("live_editor"),
		TEXT("lane_escalation"),
		TEXT("blocked"),
		EscalationReason,
		Blocker.FamilyLabel,
		FString(),
		TEXT("Need your review before continuing."));
	Plan.UpdatedAtUtc = MakeUtcNowText();
	FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error);
}

void SOsvayderEditorWidget::MarkActivePlanForPostReattachVerification(const FOsvayderUERestartSurvivalState& RestartState)
{
	FOsvayderUEActivePlan Plan;
	FString Error;
	if (!FOsvayderUERelayAgentManager::LoadActivePlan(Plan, Error))
	{
		return;
	}

	if (IsFeatureWorkflowPlan(Plan))
	{
		if (IsWorkspaceFileBuildRestartOrigin(RestartState))
		{
			Plan.CompileProof.bCompiledModuleMutationObserved = true;
			Plan.FeatureWorkflow.bCompileProofRequired = true;
			if (Plan.FeatureWorkflow.CompileProofState.IsEmpty())
			{
				Plan.FeatureWorkflow.CompileProofState = TEXT("pending");
			}
		}
		if (RestartState.bDetachedBuildCompleted)
		{
			const FString CompileDetail = RestartState.DetachedTerminalOutcome.IsEmpty()
				? FString(TEXT("restart_survival detached build completed before reattach"))
				: RestartState.DetachedTerminalOutcome.Left(400);
			RecordCompileProof(
				Plan.CompileProof,
				RestartState.LastUpdatedAtUtc.IsEmpty() ? MakeUtcNowText() : RestartState.LastUpdatedAtUtc,
				Plan.LastCompletedToolCallId,
				TEXT("restart_survival"),
				TEXT("success"),
				NormalizeSingleLineText(CompileDetail));
			Plan.FeatureWorkflow.CompileProofState = TEXT("passed");
			const int32 CompilePhaseIndex = Plan.FeatureWorkflow.FindPhaseIndex(TEXT("compile_gate"));
			if (CompilePhaseIndex != INDEX_NONE)
			{
				Plan.FeatureWorkflow.Phases[CompilePhaseIndex].Status = TEXT("completed");
				Plan.FeatureWorkflow.Phases[CompilePhaseIndex].CompletedAtUtc =
					RestartState.LastUpdatedAtUtc.IsEmpty() ? MakeUtcNowText() : RestartState.LastUpdatedAtUtc;
				Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("compile_gate"));
			}
			Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
		}
		Plan.bPostReattachVerificationRequired = false;
		Plan.CurrentToolCallId = Plan.LastCompletedToolCallId;
		SetCurrentMechanic(
			Plan,
			GetCurrentFeatureWorkflowPhaseId(Plan),
			TEXT("Resume the same feature workflow phase after reattach."),
			TEXT("Продолжить тот же этап feature workflow после reattach."),
			FString::Printf(
				TEXT("restart_survival_task_id=%s; feature_workflow_id=%s; current_phase=%s"),
				*RestartState.TaskId,
				*Plan.FeatureWorkflow.FeatureWorkflowId,
				*GetCurrentFeatureWorkflowPhaseId(Plan)));
		Plan.ResumeHint = FString::Printf(
			TEXT("Resume feature_workflow_id=%s at phase=%s after reattach."),
			*Plan.FeatureWorkflow.FeatureWorkflowId,
			*GetCurrentFeatureWorkflowPhaseId(Plan));
	}
	else
	{
		MarkLeadingMechanicCompleted(Plan, TEXT("perform_bounded_work"));
		const int32 VerifyMechanicIndex = FindMechanicIndex(Plan, TEXT("verify_and_report"));
		if (VerifyMechanicIndex != INDEX_NONE)
		{
			Plan.Mechanics[VerifyMechanicIndex].bRequiresPostReattachVerification = true;
		}
		Plan.bPostReattachVerificationRequired = true;
		Plan.CurrentToolCallId = Plan.LastCompletedToolCallId;
		SetCurrentMechanic(
			Plan,
			TEXT("verify_and_report"),
			TEXT("Resume verification from the exact last closed-editor tool-call boundary."),
	        TEXT("Возобновить проверку с точной границы последнего завершённого tool call."),
			FString::Printf(
				TEXT("restart_survival_task_id=%s; last_completed_tool_call_id=%s"),
				*RestartState.TaskId,
				*Plan.LastCompletedToolCallId));
		Plan.ResumeHint = FString::Printf(
			TEXT("Resume from last_completed_tool_call_id=%s after reattach."),
			*Plan.LastCompletedToolCallId);

		if (IsWorkspaceFileBuildRestartOrigin(RestartState))
		{
			Plan.CompileProof.bCompiledModuleMutationObserved = true;
			if (Plan.CompileProof.MutationToolFamily.IsEmpty())
			{
				Plan.CompileProof.MutationToolFamily = !RestartState.OriginTask.OriginatingPrimaryMutationToolFamily.IsEmpty()
					? RestartState.OriginTask.OriginatingPrimaryMutationToolFamily
					: RestartState.OriginTask.OriginatingRequestedToolFamily;
			}
			if (Plan.CompileProof.MutationToolFamily.IsEmpty())
			{
				Plan.CompileProof.MutationToolFamily = TEXT("workspace_file_build");
			}
			if (Plan.CompileProof.LastMutationAtUtc.IsEmpty())
			{
				Plan.CompileProof.LastMutationAtUtc = Plan.CreatedAtUtc.IsEmpty()
					? MakeUtcNowText()
					: Plan.CreatedAtUtc;
				Plan.CompileProof.LastMutationToolName = TEXT("restart_survival_origin");
			}
			if (RestartState.bDetachedBuildCompleted)
			{
				const FString CompileDetail = RestartState.DetachedTerminalOutcome.IsEmpty()
					? FString(TEXT("restart_survival detached build completed before reattach"))
					: RestartState.DetachedTerminalOutcome.Left(400);
				RecordCompileProof(
					Plan.CompileProof,
					RestartState.LastUpdatedAtUtc.IsEmpty() ? MakeUtcNowText() : RestartState.LastUpdatedAtUtc,
					Plan.LastCompletedToolCallId,
					TEXT("restart_survival"),
					TEXT("success"),
					NormalizeSingleLineText(CompileDetail));
			}
		}
	}

	AssignPlanLaneState(
		Plan,
		TEXT("live_editor"),
		FString(),
		FString(),
		TEXT("lane_return"),
		(RestartState.bPostReattachCompletionPending && !RestartState.bPostReattachCompletionDispatched)
			? TEXT("in_progress")
			: TEXT("completed"),
		RestartState.ReattachNotice.IsEmpty()
			? TEXT("The task has returned to the editor lane.")
			: RestartState.ReattachNotice,
		RestartState.DetachedLastBlockerFamily,
		RestartState.TaskId,
		TEXT("Resume the same persisted task from the saved editor boundary after reattach."));

	Plan.UpdatedAtUtc = MakeUtcNowText();
	FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error);
}

FOsvayderUEActivePlanCloseoutDecision SOsvayderEditorWidget::FinalizeActivePlan(const FString& Response, const bool bSuccess, const bool bArchiveRelayArtifacts)
{
	FOsvayderUEActivePlanCloseoutDecision FallbackDecision;
	FallbackDecision.PlanStatus = TEXT("failed");
	FallbackDecision.ResultStatus = TEXT("not_achieved");
	FallbackDecision.GateReasonCode = TEXT("missing_active_plan_closeout_proof");
	FallbackDecision.BlockerFamily = TEXT("active_plan_missing");
	FallbackDecision.BlockerDetail = TEXT("Provider transport success is not task success; active_plan.json/current closeout proof is required before archiving as achieved.");

	FOsvayderUEActivePlan Plan;
	FString Error;
	if (!FOsvayderUERelayAgentManager::LoadActivePlan(Plan, Error))
	{
		return FallbackDecision;
	}

	ApplyVisualQaManifestEvidenceFromResponse(Plan, Response);

	const FString ActiveRunId = FOsvayderUEAgentTraceLog::Get().GetActiveRunIdForBackend(ActiveRequestBackend);
	FOsvayderUERunCloseoutContext CloseoutContext = BuildCurrentRunCloseoutContext(Plan, ActiveRunId);
	FOsvayderUEActivePlanCloseoutDecision Decision =
		EvaluateActivePlanCloseoutWithContext(Plan, bSuccess, CloseoutContext);
	if (IsFeatureWorkflowPlan(Plan))
	{
		Plan.FeatureWorkflow = ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, CloseoutContext.Facts);
	}
	FString TypedClosedEditorProofDetail;
	const bool bHasCurrentClosedEditorSuccessProof =
		TryGetCurrentTypedClosedEditorResultProof(true, TypedClosedEditorProofDetail);
	Decision = ApplyActivePlanCloseoutSafetyGates(
		Plan,
		Response,
		bSuccess,
		bHasCurrentClosedEditorSuccessProof,
		Decision);
	if (IsVisualQaManifestRequiredForPlan(Plan))
	{
		ApplyVisualQaManifestEvidenceFromResponse(Plan, Response);
	}
	const FString SummaryText = NormalizeSingleLineText(Response.Left(400));
	FString GatePrefix;
	if (!Decision.GateReasonCode.IsEmpty())
	{
		GatePrefix = FString::Printf(TEXT("closeout_gate_reason=%s; "), *Decision.GateReasonCode);
		if (!Decision.BlockerFamily.IsEmpty())
		{
			GatePrefix += FString::Printf(TEXT("blocker_family=%s; "), *Decision.BlockerFamily);
		}
		if (!Decision.BlockerDetail.IsEmpty())
		{
			GatePrefix += FString::Printf(TEXT("blocker_detail=%s; "), *Decision.BlockerDetail);
		}
		if (!Decision.StopLossReason.IsEmpty())
		{
			GatePrefix += FString::Printf(TEXT("stop_loss_reason=%s; "), *Decision.StopLossReason);
		}
	}

	const FOsvayderUEUserFacingStatus UserFacingStatus =
		OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision, CloseoutContext);
	const FString UserFacingTechnicalPrefix = FString::Printf(
		TEXT("user_status_id=%s; user_status=%s; user_detail=%s; "),
		*UserFacingStatus.StatusId,
		*UserFacingStatus.Headline,
		*NormalizeSingleLineText(UserFacingStatus.Detail));

	const FString CloseoutMechanicId = IsFeatureWorkflowPlan(Plan)
		? (Decision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive)
			? TEXT("memory_update")
			: GetCurrentFeatureWorkflowPhaseId(Plan))
		: FString(TEXT("verify_and_report"));
	SetMechanicStatus(
		Plan,
		CloseoutMechanicId,
		Decision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive) ? TEXT("completed") : TEXT("failed"),
		SummaryText,
		UserFacingStatus.Headline,
		Decision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive));
	Plan.Status = Decision.PlanStatus;
	Plan.ResultStatus = Decision.ResultStatus;
	Plan.CompileProof.LastCloseoutGateReason = Decision.GateReasonCode;
	if (IsFeatureWorkflowPlan(Plan))
	{
		Plan.FeatureWorkflow.CurrentPhase = CloseoutMechanicId;
		Plan.FeatureWorkflow.bStopLossTriggered = Decision.bStopLossTriggered;
		if (Decision.bStopLossTriggered)
		{
			Plan.FeatureWorkflow.TerminalStatus = TEXT("stop_loss");
		}
		else if (Decision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive))
		{
			Plan.FeatureWorkflow.TerminalStatus = Decision.ResultStatus.Equals(TEXT("achieved_fully"), ESearchCase::CaseSensitive)
				? TEXT("completed_full")
				: TEXT("completed_partial");
			Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("memory_update"));
		}
		else
		{
			Plan.FeatureWorkflow.TerminalStatus = TEXT("failed");
		}
	}
	Plan.Summary = SummaryText;
	Plan.SummaryRu = UserFacingStatus.Headline;
	Plan.TechnicalDetail = UserFacingTechnicalPrefix + GatePrefix + NormalizeSingleLineText(Response);
	Plan.CurrentAction = UserFacingStatus.StatusId;
	Plan.CurrentActionRu = UserFacingStatus.Headline;
	Plan.CurrentTechnicalDetail = UserFacingTechnicalPrefix + GatePrefix + SummaryText;
	Plan.UpdatedAtUtc = MakeUtcNowText();
	Plan.ArchiveRunTag = BuildRunIsolatedCloseoutArchiveRunTag(CloseoutContext);
	if (!FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error))
	{
		return Decision;
	}

	FString PlanArchivePath;
	if (bArchiveRelayArtifacts)
	{
		TArray<FString> ArchivedPaths;
		FString ArchiveError;
		FOsvayderUERelayAgentManager::ArchiveTerminalArtifacts(Plan, ArchivedPaths, ArchiveError);
		PlanArchivePath = FindActivePlanArchivePath(ArchivedPaths);
	}

	Decision.SourceArchivePath = PlanArchivePath;
	TSharedPtr<FJsonObject> CloseoutDecisionJson = BuildCloseoutDecisionJson(Plan, Decision, CloseoutContext);
	FString CloseoutArtifactError;
	const FString LatestCloseoutDecisionPath =
		FPaths::Combine(FOsvayderUERelayAgentManager::GetRelayRootDir(), TEXT("closeout_decision.json"));
	if (!SaveCloseoutJsonObjectToFile(CloseoutDecisionJson, LatestCloseoutDecisionPath, CloseoutArtifactError))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to persist closeout_decision artifact: %s"), *CloseoutArtifactError);
	}
	if (!PlanArchivePath.IsEmpty())
	{
		FString ArchivedCloseoutDecisionPath = PlanArchivePath;
		ArchivedCloseoutDecisionPath.RemoveFromEnd(TEXT(".active_plan.json"), ESearchCase::IgnoreCase);
		ArchivedCloseoutDecisionPath += TEXT(".closeout_decision.json");
		FString ArchivedCloseoutArtifactError;
		if (!SaveCloseoutJsonObjectToFile(CloseoutDecisionJson, ArchivedCloseoutDecisionPath, ArchivedCloseoutArtifactError))
		{
			UE_LOG(
				LogOsvayderUE,
				Warning,
				TEXT("Failed to persist archived closeout_decision artifact: %s"),
				*ArchivedCloseoutArtifactError);
		}
	}
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("closeout_decision"),
		ActiveRequestBackend,
		CloseoutDecisionJson,
		ActiveRunId.IsEmpty() ? CloseoutContext.RunId : ActiveRunId);

	return Decision;
}

void SOsvayderEditorWidget::TrySurfacePersistedActivePlanNotice()
{
	FOsvayderUEActivePlan Plan;
	FString Error;
	if (!FOsvayderUERelayAgentManager::LoadActivePlan(Plan, Error))
	{
		return;
	}

	if (Plan.Status.Equals(TEXT("done"), ESearchCase::CaseSensitive)
		|| Plan.Status.Equals(TEXT("failed"), ESearchCase::CaseSensitive)
		|| Plan.Status.Equals(TEXT("cancelled"), ESearchCase::CaseSensitive))
	{
		return;
	}

	FString MechanicLabel = Plan.CurrentMechanicId;
	const int32 MechanicIndex = FindMechanicIndex(Plan, Plan.CurrentMechanicId);
	if (MechanicIndex != INDEX_NONE && !Plan.Mechanics[MechanicIndex].LabelRu.IsEmpty())
	{
		MechanicLabel = Plan.Mechanics[MechanicIndex].LabelRu;
	}

	AddMessage(
		FString::Printf(
			TEXT("Active task plan found. Status: %s. Current step: %s. Resume hint: %s"),
			*DescribeTaskLaneStatusInternal(Plan.LaneState),
			*MechanicLabel,
			*Plan.ResumeHint),
		false);
}

bool SOsvayderEditorWidget::DispatchPreparedPrompt(
	const FString& Prompt,
	const TArray<FString>& ImagePaths,
	const bool bIsTransportRetryReplay,
	const FString& TransportRetrySourceRunId,
	const FString& TransportRetryFailureFamily,
	const FString& VisibleHistoryPromptOverride,
	const bool bClearInputWidget,
	FString& OutError)
{
	const bool bHasText = !Prompt.IsEmpty();
	const bool bHasImage = ImagePaths.Num() > 0;
	if ((!bHasText && !bHasImage) || bIsWaitingForResponse)
	{
		OutError = bIsWaitingForResponse
			? TEXT("Another request is already running.")
			: TEXT("Nothing is available to dispatch.");
		return false;
	}

	if (!CanSendPrompt())
	{
		const FAgentBackendStatus Status = GetConfiguredBackendStatus();
		LastRuntimeConnectionState = EBackendRuntimeConnectionState::Failed;
		LastRuntimeConnectionDetail = Status.Detail.IsEmpty()
			? TEXT("Configured backend is not available.")
			: Status.Detail;
		OutError = LastRuntimeConnectionDetail;
		return false;
	}

	CurrentRequestPromptText = Prompt;
	CurrentRequestImagePaths.Reset();
	for (const FString& ImagePath : ImagePaths)
	{
		if (!ImagePath.IsEmpty())
		{
			CurrentRequestImagePaths.Add(FPaths::ConvertRelativePathToFull(ImagePath));
		}
	}

	FString ActivePlanError;
	if (!PrepareActivePlanForCurrentSend(Prompt, ActivePlanError))
	{
		LastRuntimeConnectionState = EBackendRuntimeConnectionState::Failed;
		LastRuntimeConnectionDetail = ActivePlanError.IsEmpty()
			? TEXT("Could not persist active reviewer-plan state.")
			: ActivePlanError;
		CurrentRequestPromptText.Empty();
		CurrentRequestImagePaths.Reset();
		OutError = LastRuntimeConnectionDetail;
		return false;
	}

	ClearTransportRetryState();

	if (bClearInputWidget)
	{
		CurrentInputText.Empty();
		if (InputArea.IsValid())
		{
			InputArea->ClearText();
		}
	}

	bIsWaitingForResponse = true;
	ActiveRequestBackend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
	const EOsvayderUEProviderBackend RequestBackend = ActiveRequestBackend;
	const uint64 RequestGeneration = ++NextRequestGeneration;
	ActiveRequestGeneration = RequestGeneration;
	LastRuntimeConnectionState = EBackendRuntimeConnectionState::Connecting;
	LastRuntimeConnectionDetail = FString::Printf(
		TEXT("Connecting to %s..."),
		*GetConfiguredBackendDisplayName());

	StartStreamingResponse();

	TWeakPtr<SOsvayderEditorWidget> SelfWeak = SharedThis(this).ToWeakPtr();

	FOnOsvayderResponse OnComplete;
	OnComplete.BindLambda([SelfWeak, RequestGeneration, RequestBackend](const FString& Response, bool bSuccess)
	{
		TSharedPtr<SOsvayderEditorWidget> Pinned = SelfWeak.Pin();
		if (!Pinned.IsValid())
		{
			return;
		}
		Pinned->OnClaudeResponse(Response, bSuccess, RequestGeneration, RequestBackend);
	});

	NormalizeMandatoryContextFlagsForNormalAssistantRun(bIncludeUE57Context, bIncludeProjectContext);

	FOsvayderPromptOptions Options;
	Options.bIncludeEngineContext = bIncludeUE57Context;
	Options.bIncludeProjectContext = bIncludeProjectContext;
	Options.bIsTransportRetryReplay = bIsTransportRetryReplay;
	Options.TransportRetrySourceRunId = TransportRetrySourceRunId;
	Options.TransportRetryFailureFamily = TransportRetryFailureFamily;
	Options.VisibleHistoryPromptOverride = VisibleHistoryPromptOverride;
	Options.OnProgress.BindLambda([SelfWeak, RequestGeneration, RequestBackend](const FString& PartialOutput)
	{
		TSharedPtr<SOsvayderEditorWidget> Pinned = SelfWeak.Pin();
		if (!Pinned.IsValid())
		{
			return;
		}
		Pinned->OnClaudeProgress(PartialOutput, RequestGeneration, RequestBackend);
	});
	Options.OnStreamEvent.BindLambda([SelfWeak, RequestGeneration, RequestBackend](const FOsvayderStreamEvent& Event)
	{
		TSharedPtr<SOsvayderEditorWidget> Pinned = SelfWeak.Pin();
		if (!Pinned.IsValid())
		{
			return;
		}
		Pinned->OnClaudeStreamEvent(Event, RequestGeneration, RequestBackend);
	});
	Options.AttachedImagePaths = ImagePaths;

	FOsvayderCodeSubsystem::Get().SendPrompt(Prompt, OnComplete, Options);
	return true;
}

void SOsvayderEditorWidget::SendMessage()
{
	// Extract image paths before checking emptiness
	TArray<FString> ImagePaths;
	if (InputArea.IsValid())
	{
		ImagePaths = InputArea->GetAttachedImagePaths();
	}

	const bool bHasText = !CurrentInputText.IsEmpty();
	const bool bHasImage = ImagePaths.Num() > 0;
	const FPendingTransportRetryReplayContext TransportRetryReplayContext = PendingTransportRetryReplayContext;

	if ((!bHasText && !bHasImage) || bIsWaitingForResponse)
	{
		return;
	}

	if (!CanSendPrompt())
	{
		const FAgentBackendStatus Status = GetConfiguredBackendStatus();
		LastRuntimeConnectionState = EBackendRuntimeConnectionState::Failed;
		LastRuntimeConnectionDetail = Status.Detail.IsEmpty()
			? TEXT("Configured backend is not available.")
			: Status.Detail;
		AddMessage(Status.Detail.IsEmpty() ? TEXT("Configured backend is not available.") : Status.Detail, false);
		return;
	}

	// Build display message
	FString DisplayMessage = bHasText ? CurrentInputText : FString();
	if (bHasImage)
	{
		FString ImageLabel;
		if (ImagePaths.Num() == 1)
		{
			ImageLabel = FString::Printf(TEXT("[Attached image: %s]"), *FPaths::GetCleanFilename(ImagePaths[0]));
		}
		else
		{
			TArray<FString> FileNames;
			for (const FString& Path : ImagePaths)
			{
				FileNames.Add(FPaths::GetCleanFilename(Path));
			}
			ImageLabel = FString::Printf(TEXT("[Attached %d images: %s]"), ImagePaths.Num(), *FString::Join(FileNames, TEXT(" ")));
		}

		if (bHasText)
		{
			DisplayMessage += TEXT("\n") + ImageLabel;
		}
		else
		{
			DisplayMessage = ImageLabel;
		}
	}
	if (TransportRetryReplayContext.bActive)
	{
		DisplayMessage = FString::Printf(
			TEXT("[Retry Last on fresh backend session]\n%s"),
			*DisplayMessage);
	}

	// Add user message to chat
	AddMessage(DisplayMessage, true);

	// Build prompt - use default if image-only
	FString Prompt = bHasText ? CurrentInputText : TEXT("Please analyze this image.");
	FString DispatchError;
	if (!DispatchPreparedPrompt(
		Prompt,
		ImagePaths,
		TransportRetryReplayContext.bActive,
		TransportRetryReplayContext.SourceRunId,
		TransportRetryReplayContext.FailureFamily,
		FString(),
		true,
		DispatchError))
	{
		AddMessage(DispatchError, false);
	}
}

void SOsvayderEditorWidget::ToggleDictation()
{
	if (!VoiceDictationService.IsValid() || bIsWaitingForResponse)
	{
		return;
	}

	const EOsvayderUEVoiceDictationState DictationState = VoiceDictationService->GetStatus().State;
	if (DictationState == EOsvayderUEVoiceDictationState::Recording)
	{
		VoiceDictationService->StopDictation();
		return;
	}

	VoiceDictationService->StartDictation();
}

bool SOsvayderEditorWidget::DebugStartDictation()
{
	if (!VoiceDictationService.IsValid() || bIsWaitingForResponse)
	{
		return false;
	}

	const EOsvayderUEVoiceDictationState DictationState = VoiceDictationService->GetStatus().State;
	if (DictationState == EOsvayderUEVoiceDictationState::Recording)
	{
		return true;
	}

	if (DictationState == EOsvayderUEVoiceDictationState::PreparingRuntime ||
		DictationState == EOsvayderUEVoiceDictationState::Transcribing)
	{
		return false;
	}

	return VoiceDictationService->StartDictation();
}

bool SOsvayderEditorWidget::DebugStopDictation()
{
	if (!VoiceDictationService.IsValid())
	{
		return false;
	}

	if (VoiceDictationService->GetStatus().State != EOsvayderUEVoiceDictationState::Recording)
	{
		return false;
	}

	VoiceDictationService->StopDictation();
	return true;
}

bool SOsvayderEditorWidget::DebugToggleDictation()
{
	if (!VoiceDictationService.IsValid() || bIsWaitingForResponse)
	{
		return false;
	}

	const EOsvayderUEVoiceDictationState BeforeState = VoiceDictationService->GetStatus().State;
	ToggleDictation();
	return VoiceDictationService->GetStatus().State != BeforeState
		|| BeforeState == EOsvayderUEVoiceDictationState::Recording;
}

void SOsvayderEditorWidget::DebugClearInputText()
{
	CurrentInputText.Empty();
	if (InputArea.IsValid())
	{
		InputArea->ClearText();
	}
}

void SOsvayderEditorWidget::DebugSetInputText(const FString& NewText)
{
	CurrentInputText = NewText;
	if (InputArea.IsValid())
	{
		InputArea->SetText(NewText);
	}
}

bool SOsvayderEditorWidget::DebugSendCurrentInput()
{
	if (bIsWaitingForResponse)
	{
		return false;
	}

	const int32 MessageCountBefore = VisibleChatMessages.Num();
	SendMessage();
	return bIsWaitingForResponse || VisibleChatMessages.Num() > MessageCountBefore;
}

bool SOsvayderEditorWidget::DebugIsWaitingForResponse() const
{
	return bIsWaitingForResponse;
}

void SOsvayderEditorWidget::DebugForceBackendSnapshotRefresh(const bool bForce)
{
	RefreshBackendSnapshotIfChanged(bForce);
}

void SOsvayderEditorWidget::DebugProcessBackendStateNow()
{
	const EOsvayderUEProviderBackend CurrentBackend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
	if (CurrentBackend != LastRenderedBackend)
	{
		const EOsvayderUEProviderBackend PreviousBackend = LastRenderedBackend;
		LastRenderedBackend = CurrentBackend;
		LastBackendSnapshotSignature.Empty();
		HandleConfiguredBackendChanged(PreviousBackend);
	}

	RefreshBackendSnapshotIfChanged();
}

int32 SOsvayderEditorWidget::DebugGetVisibleMessageCount() const
{
	return VisibleChatMessages.Num();
}

bool SOsvayderEditorWidget::DebugVisibleChatContains(const FString& Needle) const
{
	if (Needle.IsEmpty())
	{
		return false;
	}

	for (const FString& Message : VisibleChatMessages)
	{
		if (Message.Contains(Needle, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return false;
}

SOsvayderEditorWidget::EBackendRuntimeConnectionState SOsvayderEditorWidget::DebugGetLastRuntimeConnectionState() const
{
	return LastRuntimeConnectionState;
}

FString SOsvayderEditorWidget::DebugGetLastRuntimeConnectionDetail() const
{
	return LastRuntimeConnectionDetail;
}

FString SOsvayderEditorWidget::DebugGetLastResponseText() const
{
	return LastResponse;
}

bool SOsvayderEditorWidget::ValidateHeadlessAcceptanceRequest(
	const FOsvayderUEHeadlessAcceptanceRequest& Request,
	FString& OutError)
{
	OutError.Reset();
	FOsvayderUEHeadlessAcceptanceRequest NormalizedRequest = Request;
	NormalizeHeadlessAcceptanceRequestPaths(NormalizedRequest);

	if (!NormalizedRequest.bLocalDevOptIn)
	{
		OutError = TEXT("local_dev_opt_in_required");
		return false;
	}
	if (NormalizedRequest.PromptFile.IsEmpty())
	{
		OutError = TEXT("prompt_file_required");
		return false;
	}
	if (!FPaths::FileExists(NormalizedRequest.PromptFile))
	{
		OutError = FString::Printf(TEXT("prompt_file_not_found: %s"), *NormalizedRequest.PromptFile);
		return false;
	}
	if (NormalizedRequest.Prefix.IsEmpty())
	{
		OutError = TEXT("prefix_required");
		return false;
	}
	if (NormalizedRequest.TimeoutSec <= 0)
	{
		OutError = TEXT("timeout_sec_must_be_positive");
		return false;
	}

	return true;
}

FString SOsvayderEditorWidget::NormalizeHeadlessAcceptancePathArgument(const FString& PathArgument)
{
	FString Result = PathArgument.TrimStartAndEnd();
	bool bStrippedQuotes = true;
	while (bStrippedQuotes && Result.Len() >= 2)
	{
		bStrippedQuotes = false;
		const TCHAR FirstChar = Result[0];
		const TCHAR LastChar = Result[Result.Len() - 1];
		const bool bDoubleQuoted = FirstChar == TCHAR('"') && LastChar == TCHAR('"');
		const bool bSingleQuoted = FirstChar == TCHAR('\'') && LastChar == TCHAR('\'');
		if (bDoubleQuoted || bSingleQuoted)
		{
			Result = Result.Mid(1, Result.Len() - 2).TrimStartAndEnd();
			bStrippedQuotes = true;
		}
	}

	if (!Result.IsEmpty())
	{
		FPaths::NormalizeFilename(Result);
	}
	return Result;
}

void SOsvayderEditorWidget::NormalizeHeadlessAcceptanceRequestPaths(FOsvayderUEHeadlessAcceptanceRequest& Request)
{
	Request.PromptFile = NormalizeHeadlessAcceptancePathArgument(Request.PromptFile);
	Request.OutputDir = NormalizeHeadlessAcceptancePathArgument(Request.OutputDir);
}

void SOsvayderEditorWidget::NormalizeMandatoryContextFlagsForNormalAssistantRun(
	bool& bInOutIncludeUE57Context,
	bool& bInOutIncludeProjectContext)
{
	bInOutIncludeUE57Context = true;
	bInOutIncludeProjectContext = true;
}

FString SOsvayderEditorWidget::ComputeHeadlessAcceptancePromptHash(const FString& Prompt)
{
	FTCHARToUTF8 Utf8Prompt(*Prompt);
	FMD5 Md5;
	Md5.Update(reinterpret_cast<const uint8*>(Utf8Prompt.Get()), Utf8Prompt.Length());

	uint8 Digest[16];
	Md5.Final(Digest);
	return FString::Printf(TEXT("md5:%s"), *BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower());
}

FString SOsvayderEditorWidget::ResolveHeadlessAcceptanceReceiptPath(
	const FOsvayderUEHeadlessAcceptanceRequest& Request)
{
	FString OutputDir = Request.OutputDir.IsEmpty()
		? FPaths::Combine(FOsvayderUERelayAgentManager::GetRelayRootDir(), TEXT("HeadlessAcceptance"))
		: Request.OutputDir;
	OutputDir = NormalizeHeadlessAcceptancePathArgument(OutputDir);
	const FString SafePrefix = MakeCloseoutSafeFileTag(
		Request.Prefix.IsEmpty() ? FString(TEXT("headless_acceptance")) : Request.Prefix);
	const FString FileName = FString::Printf(TEXT("%s.headless_acceptance_receipt.json"), *SafePrefix);
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDir, FileName));
}

namespace
{
	bool IsCurrentPrefixLogPathForHeadlessAcceptance(
		const FOsvayderUEHeadlessAcceptanceReceiptContext& Context,
		const FString& CandidateLogPath)
	{
		if (Context.Request.Prefix.IsEmpty() || CandidateLogPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		return FPaths::GetCleanFilename(CandidateLogPath).StartsWith(MakeCloseoutSafeFileTag(Context.Request.Prefix));
	}

	bool HasCurrentPrefixLogEvidenceForHeadlessAcceptance(
		const FOsvayderUEHeadlessAcceptanceReceiptContext& Context)
	{
		for (const FString& CandidateLogPath : Context.LogPaths)
		{
			if (IsCurrentPrefixLogPathForHeadlessAcceptance(Context, CandidateLogPath))
			{
				return true;
			}
		}
		return false;
	}

	bool HasCloseoutIdentityEvidenceForHeadlessAcceptance(
		const FOsvayderUEHeadlessAcceptanceReceiptContext& Context)
	{
		const FOsvayderUEActivePlanCloseoutDecision& Decision = Context.CloseoutDecision;
		const FString ArchivePath = !Context.ArchivePath.IsEmpty() ? Context.ArchivePath : Decision.SourceArchivePath;
		return Context.bHasCloseoutDecision
			&& !Decision.SourceRunId.IsEmpty()
			&& !Decision.SourcePlanId.IsEmpty()
			&& !Context.LatestCloseoutPath.IsEmpty()
			&& !ArchivePath.IsEmpty()
			&& !Context.VisibleSessionPath.IsEmpty()
			&& !Context.TracePath.IsEmpty();
	}

	FString GetHeadlessAcceptanceDispatchPath(const FOsvayderUEHeadlessAcceptanceRequest& Request)
	{
		if (!Request.TriggerPath.IsEmpty())
		{
			return Request.TriggerPath;
		}
		return Request.bVisibleManualEmulator
			? TEXT("visible_manual_emulator_widget_new_session_send_message")
			: TEXT("widget_new_session_send_message");
	}

	FString GetHeadlessAcceptanceMode(const FOsvayderUEHeadlessAcceptanceRequest& Request)
	{
		return Request.bVisibleManualEmulator
			? TEXT("visible_manual")
			: TEXT("headless_acceptance");
	}

	FString GetHeadlessAcceptanceVisibilityMode(const FOsvayderUEHeadlessAcceptanceRequest& Request)
	{
		return Request.bRequireVisibleEditor
			? TEXT("visible_editor_required")
			: TEXT("headless_or_unspecified");
	}
}

TSharedPtr<FJsonObject> SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(
	const FOsvayderUEHeadlessAcceptanceReceiptContext& Context)
{
	const FOsvayderUEActivePlanCloseoutDecision& Decision = Context.CloseoutDecision;
	const bool bDecisionPassed =
		Context.bAssistantSuccess
		&& Context.bHasCloseoutDecision
		&& Decision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive)
		&& Decision.ResultStatus.Equals(TEXT("achieved_fully"), ESearchCase::CaseSensitive)
		&& Decision.GateReasonCode.IsEmpty()
		&& !Decision.bStopLossTriggered
		&& !Decision.bManagedStateManualWriteDetected;
	const bool bManualVerificationRequired =
		Context.bAssistantSuccess
		&& Context.bHasCloseoutDecision
		&& Decision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive)
		&& Decision.ResultStatus.Equals(TEXT("achieved_partially"), ESearchCase::CaseSensitive)
		&& Decision.GateReasonCode.Equals(TEXT("gameplay_runtime_visual_proof_manual_required"), ESearchCase::CaseSensitive)
		&& Decision.BlockerFamily.Equals(TEXT("manual_verification_required"), ESearchCase::CaseSensitive)
		&& !Decision.bStopLossTriggered
		&& !Decision.bManagedStateManualWriteDetected;
	const bool bCurrentPrefixLogEvidencePresent =
		Context.Request.Prefix.IsEmpty() || HasCurrentPrefixLogEvidenceForHeadlessAcceptance(Context);
	const bool bCloseoutIdentityEvidencePresent =
		HasCloseoutIdentityEvidenceForHeadlessAcceptance(Context);
	const bool bRuntimeLogArtifactsRequired =
		Context.bHasCloseoutDecision && Decision.bRuntimeProofRequired;
	const bool bFreshPrefixEvidencePresent =
		Context.Request.Prefix.IsEmpty()
		|| bCurrentPrefixLogEvidencePresent
		|| (!bRuntimeLogArtifactsRequired && bCloseoutIdentityEvidencePresent);

	FString EffectiveStatus = Context.Status;
	FString EffectiveFailureReason = Context.FailureReason;
	if (EffectiveStatus.IsEmpty() || EffectiveStatus.Equals(TEXT("completed"), ESearchCase::CaseSensitive))
	{
		if (bDecisionPassed && bFreshPrefixEvidencePresent)
		{
			EffectiveStatus = TEXT("closeout_passed");
		}
		else if (bManualVerificationRequired && bFreshPrefixEvidencePresent)
		{
			EffectiveStatus = OsvayderUEUserFacingStatus::ManualVerificationRequiredStatusId();
		}
		else
		{
			EffectiveStatus = TEXT("closeout_failed");
		}
	}
	if (!bFreshPrefixEvidencePresent && EffectiveFailureReason.IsEmpty())
	{
		EffectiveFailureReason = TEXT("no_current_prefix_artifacts");
	}
	else if (!bDecisionPassed
		&& EffectiveFailureReason.IsEmpty()
		&& Context.bHasCloseoutDecision)
	{
		EffectiveFailureReason = Decision.GateReasonCode.IsEmpty()
			? TEXT("closeout_decision_not_passed")
			: Decision.GateReasonCode;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(TEXT("artifact_type"), TEXT("headless_new_session_acceptance_receipt"));
	Root->SetStringField(TEXT("status"), EffectiveStatus);
	Root->SetStringField(TEXT("failure_reason"), EffectiveFailureReason);
	Root->SetStringField(TEXT("dispatch_path"), Context.DispatchPath.IsEmpty()
		? GetHeadlessAcceptanceDispatchPath(Context.Request)
		: Context.DispatchPath);
	Root->SetStringField(TEXT("ui_path"), TEXT("widget_new_session_send_message"));
	Root->SetStringField(TEXT("emulator_mode"), GetHeadlessAcceptanceMode(Context.Request));
	Root->SetStringField(TEXT("process_visibility_mode"), GetHeadlessAcceptanceVisibilityMode(Context.Request));
	Root->SetBoolField(TEXT("visible_manual_emulator"), Context.Request.bVisibleManualEmulator);
	Root->SetBoolField(TEXT("visible_unreal_required"), Context.Request.bRequireVisibleEditor);
	Root->SetStringField(TEXT("prompt_file"), Context.Request.PromptFile);
	Root->SetStringField(TEXT("output_dir"), Context.Request.OutputDir);
	Root->SetStringField(TEXT("receipt_path"), Context.ReceiptPath);
	Root->SetStringField(TEXT("prompt_hash"), Context.PromptHash);
	Root->SetStringField(TEXT("prefix"), Context.Request.Prefix);
	Root->SetNumberField(TEXT("timeout_sec"), Context.Request.TimeoutSec);
	Root->SetStringField(TEXT("started_at_utc"), Context.StartedAtUtc);
	Root->SetStringField(TEXT("completed_at_utc"), Context.CompletedAtUtc);
	Root->SetBoolField(TEXT("assistant_success"), Context.bAssistantSuccess);
	Root->SetBoolField(TEXT("has_closeout_decision"), Context.bHasCloseoutDecision);
	Root->SetBoolField(TEXT("fresh_prefix_artifacts_present"), bFreshPrefixEvidencePresent);
	Root->SetBoolField(TEXT("current_prefix_log_artifacts_present"), bCurrentPrefixLogEvidencePresent);
	Root->SetBoolField(TEXT("closeout_identity_artifacts_present"), bCloseoutIdentityEvidencePresent);
	Root->SetBoolField(TEXT("runtime_log_artifacts_required"), bRuntimeLogArtifactsRequired);
	Root->SetBoolField(TEXT("managed_state_manual_write_detected"), Decision.bManagedStateManualWriteDetected);
	Root->SetBoolField(TEXT("request_editor_exit_on_complete"), Context.Request.bRequestEditorExitOnComplete);

	const FString RunId = !Decision.SourceRunId.IsEmpty() ? Decision.SourceRunId : Context.ActiveRunIdFallback;
	const FString PlanId = Decision.SourcePlanId;
	const FString WorkflowId = Decision.SourceFeatureWorkflowId;
	const FString ArchivePath = !Context.ArchivePath.IsEmpty() ? Context.ArchivePath : Decision.SourceArchivePath;
	Root->SetStringField(TEXT("run_id"), RunId);
	Root->SetStringField(TEXT("plan_id"), PlanId);
	Root->SetStringField(TEXT("workflow_id"), WorkflowId);
	Root->SetStringField(TEXT("recipe_id"), Decision.SourceRecipeId);
	Root->SetStringField(TEXT("role_id"), Decision.SourceRoleId);
	Root->SetStringField(TEXT("closeout_path"), Context.LatestCloseoutPath);
	Root->SetStringField(TEXT("archive_path"), ArchivePath);
	Root->SetStringField(TEXT("visible_session_path"), Context.VisibleSessionPath);
	Root->SetStringField(TEXT("trace_path"), Context.TracePath);
	Root->SetArrayField(TEXT("log_paths"), MakeCloseoutStringArrayJson(Context.LogPaths));

	TSharedPtr<FJsonObject> DecisionObject = MakeShared<FJsonObject>();
	DecisionObject->SetStringField(TEXT("plan_status"), Decision.PlanStatus);
	DecisionObject->SetStringField(TEXT("result_status"), Decision.ResultStatus);
	DecisionObject->SetStringField(TEXT("gate_reason_code"), Decision.GateReasonCode);
	DecisionObject->SetBoolField(TEXT("runtime_proof_passed"), Decision.bRuntimeProofPassed);
	DecisionObject->SetBoolField(TEXT("runtime_proof_required"), Decision.bRuntimeProofRequired);
	DecisionObject->SetBoolField(TEXT("stop_loss_triggered"), Decision.bStopLossTriggered);
	DecisionObject->SetStringField(TEXT("stop_loss_reason"), Decision.StopLossReason);
	DecisionObject->SetStringField(TEXT("blocker_family"), Decision.BlockerFamily);
	DecisionObject->SetStringField(TEXT("blocker_detail"), Decision.BlockerDetail);
	DecisionObject->SetBoolField(TEXT("managed_state_manual_write_detected"), Decision.bManagedStateManualWriteDetected);
	DecisionObject->SetArrayField(TEXT("missing_recipe_obligations"), MakeCloseoutStringArrayJson(Decision.MissingRecipeObligations));
	Root->SetObjectField(TEXT("closeout_decision_summary"), DecisionObject);

	return Root;
}

bool SOsvayderEditorWidget::SaveHeadlessAcceptanceReceiptArtifact(
	const FOsvayderUEHeadlessAcceptanceReceiptContext& Context,
	FString& OutError)
{
	return SaveHeadlessAcceptanceReceiptObject(
		BuildHeadlessAcceptanceReceiptJson(Context),
		Context.ReceiptPath,
		OutError);
}

bool SOsvayderEditorWidget::StartHeadlessNewSessionAcceptanceBridge(
	const FOsvayderUEHeadlessAcceptanceRequest& Request,
	FString& OutError)
{
	if (PendingHeadlessAcceptanceBridge.bActive || bIsWaitingForResponse)
	{
		OutError = TEXT("headless_acceptance_request_already_running");
		return false;
	}

	FOsvayderUEHeadlessAcceptanceRequest NormalizedRequest = Request;
	NormalizeHeadlessAcceptanceRequestPaths(NormalizedRequest);

	if (!ValidateHeadlessAcceptanceRequest(NormalizedRequest, OutError))
	{
		FOsvayderUEHeadlessAcceptanceReceiptContext RejectedContext;
		RejectedContext.Request = NormalizedRequest;
		RejectedContext.ReceiptPath = ResolveHeadlessAcceptanceReceiptPath(NormalizedRequest);
		RejectedContext.Status = TEXT("rejected");
		RejectedContext.FailureReason = OutError;
		RejectedContext.CompletedAtUtc = MakeUtcNowText();
		RejectedContext.DispatchPath = GetHeadlessAcceptanceDispatchPath(NormalizedRequest);
		FString SaveError;
		SaveHeadlessAcceptanceReceiptArtifact(RejectedContext, SaveError);
		return false;
	}

	FString Prompt;
	if (!FFileHelper::LoadFileToString(Prompt, *NormalizedRequest.PromptFile))
	{
		OutError = FString::Printf(TEXT("prompt_file_load_failed: %s"), *NormalizedRequest.PromptFile);
		FOsvayderUEHeadlessAcceptanceReceiptContext FailedContext;
		FailedContext.Request = NormalizedRequest;
		FailedContext.ReceiptPath = ResolveHeadlessAcceptanceReceiptPath(NormalizedRequest);
		FailedContext.Status = TEXT("failed");
		FailedContext.FailureReason = OutError;
		FailedContext.CompletedAtUtc = MakeUtcNowText();
		FailedContext.DispatchPath = GetHeadlessAcceptanceDispatchPath(NormalizedRequest);
		FString SaveError;
		SaveHeadlessAcceptanceReceiptArtifact(FailedContext, SaveError);
		return false;
	}

	const FString ReceiptPath = ResolveHeadlessAcceptanceReceiptPath(NormalizedRequest);
	const FString PromptHash = ComputeHeadlessAcceptancePromptHash(Prompt);

	FString PriorStartedAtUtc;
	FDateTime PriorStartedAtDateTime = FDateTime::MinValue();
	TryLoadHeadlessAcceptanceReceiptStartedAt(ReceiptPath, PriorStartedAtUtc, PriorStartedAtDateTime);

	FOsvayderUEActivePlanCloseoutDecision ExistingDecision;
	FString ExistingCloseoutJson;
	if (TryLoadLatestHeadlessCloseoutDecision(ExistingDecision, ExistingCloseoutJson)
		&& !Request.Prefix.IsEmpty()
		&& ExistingCloseoutJson.Contains(Request.Prefix)
		&& ExistingDecision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive)
		&& ExistingDecision.ResultStatus.Equals(TEXT("achieved_fully"), ESearchCase::CaseSensitive)
		&& ExistingDecision.GateReasonCode.IsEmpty()
		&& !ExistingDecision.bStopLossTriggered
		&& !ExistingDecision.bManagedStateManualWriteDetected)
	{
		FOsvayderUEHeadlessAcceptanceReceiptContext RecoveryContext;
		RecoveryContext.Request = NormalizedRequest;
		RecoveryContext.ReceiptPath = ReceiptPath;
		RecoveryContext.PromptHash = PromptHash;
		RecoveryContext.StartedAtUtc = PriorStartedAtUtc.IsEmpty() ? MakeUtcNowText() : PriorStartedAtUtc;
		RecoveryContext.CompletedAtUtc = MakeUtcNowText();
		RecoveryContext.DispatchPath = GetHeadlessAcceptanceDispatchPath(NormalizedRequest);
		RecoveryContext.Status = TEXT("completed");
		RecoveryContext.bAssistantSuccess = true;
		RecoveryContext.bHasCloseoutDecision = true;
		RecoveryContext.CloseoutDecision = ExistingDecision;
		RecoveryContext.LatestCloseoutPath =
			FPaths::Combine(FOsvayderUERelayAgentManager::GetRelayRootDir(), TEXT("closeout_decision.json"));
		RecoveryContext.ArchivePath = ExistingDecision.SourceArchivePath;
		RecoveryContext.VisibleSessionPath = FOsvayderCodeSubsystem::Get().GetSessionFilePath();
		RecoveryContext.TracePath = FOsvayderUEAgentTraceLog::Get().GetTraceLogPath();
		RecoveryContext.LogPaths = CollectCurrentPrefixLogPaths(Request.Prefix, PriorStartedAtDateTime);

		if (!ExistingCloseoutJson.TrimStartAndEnd().IsEmpty())
		{
			TSharedPtr<FJsonObject> RecoveryReceipt = BuildHeadlessAcceptanceReceiptJson(RecoveryContext);
			FString RecoveryStatus;
			RecoveryReceipt->TryGetStringField(TEXT("status"), RecoveryStatus);
			if (!RecoveryStatus.Equals(TEXT("closeout_passed"), ESearchCase::CaseSensitive))
			{
				UE_LOG(
					LogOsvayderUE,
					Display,
					TEXT("Existing closeout did not satisfy current headless receipt gate; dispatching a fresh acceptance run."));
			}
			else
			{
				RecoveryReceipt->SetStringField(
					TEXT("response_preview"),
					TEXT("Recovered receipt from existing plugin-owned closeout_decision.json."));

				FString SaveError;
				if (!SaveHeadlessAcceptanceReceiptObject(RecoveryReceipt, ReceiptPath, SaveError))
				{
					OutError = SaveError;
					return false;
				}

				UE_LOG(
					LogOsvayderUE,
					Display,
					TEXT("Recovered headless acceptance receipt from existing closeout: %s"),
					*ReceiptPath);
				if (Request.bRequestEditorExitOnComplete)
				{
					FPlatformMisc::RequestExit(false);
				}

				OutError.Reset();
				return true;
			}
		}
	}

	const FDateTime StartedAt = FDateTime::UtcNow();
	PendingHeadlessAcceptanceBridge = FPendingHeadlessAcceptanceBridge{};
	PendingHeadlessAcceptanceBridge.bActive = true;
	PendingHeadlessAcceptanceBridge.Request = NormalizedRequest;
	PendingHeadlessAcceptanceBridge.ReceiptPath = ReceiptPath;
	PendingHeadlessAcceptanceBridge.PromptHash = PromptHash;
	PendingHeadlessAcceptanceBridge.StartedAtUtc = StartedAt.ToIso8601();
	PendingHeadlessAcceptanceBridge.StartedAtUtcDateTime = StartedAt;
	PendingHeadlessAcceptanceBridge.StartedSeconds = FPlatformTime::Seconds();
	PendingHeadlessAcceptanceBridge.Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();

	WriteHeadlessAcceptanceReceipt(TEXT("running"), false, nullptr, FString(), FString());

	NewSession();
	DebugSetInputText(Prompt);
	const uint64 RequestGenerationBeforeSend = ActiveRequestGeneration;
	SendMessage();
	if (!bIsWaitingForResponse || ActiveRequestGeneration == RequestGenerationBeforeSend)
	{
		const FString FailureReason = LastRuntimeConnectionDetail.IsEmpty()
			? TEXT("dispatch_failed")
			: LastRuntimeConnectionDetail;
		CompletePendingHeadlessAcceptanceBridge(FailureReason, false, nullptr, FailureReason);
		OutError = FailureReason;
		return false;
	}

	PendingHeadlessAcceptanceBridge.RequestGeneration = ActiveRequestGeneration;
	PendingHeadlessAcceptanceBridge.Backend = ActiveRequestBackend;
	OutError.Reset();
	return true;
}

void SOsvayderEditorWidget::TickHeadlessAcceptanceBridgeTimeout(const double InCurrentTime)
{
	if (!PendingHeadlessAcceptanceBridge.bActive || PendingHeadlessAcceptanceBridge.Request.TimeoutSec <= 0)
	{
		return;
	}

	const double ElapsedSeconds = InCurrentTime > 0.0
		? InCurrentTime - PendingHeadlessAcceptanceBridge.StartedSeconds
		: FPlatformTime::Seconds() - PendingHeadlessAcceptanceBridge.StartedSeconds;
	if (ElapsedSeconds < static_cast<double>(PendingHeadlessAcceptanceBridge.Request.TimeoutSec))
	{
		return;
	}

	const FString FailureReason = FString::Printf(
		TEXT("timeout_after_%d_sec"),
		PendingHeadlessAcceptanceBridge.Request.TimeoutSec);
	if (bIsWaitingForResponse)
	{
		CancelRequest();
	}
	WriteHeadlessAcceptanceReceipt(TEXT("timeout"), false, nullptr, FailureReason, FailureReason);
	const bool bRequestExit = PendingHeadlessAcceptanceBridge.Request.bRequestEditorExitOnComplete;
	PendingHeadlessAcceptanceBridge = FPendingHeadlessAcceptanceBridge{};
	if (bRequestExit)
	{
		FPlatformMisc::RequestExit(false);
	}
}

void SOsvayderEditorWidget::CompletePendingHeadlessAcceptanceBridge(
	const FString& Response,
	const bool bAssistantSuccess,
	const FOsvayderUEActivePlanCloseoutDecision* CloseoutDecision,
	const FString& FailureReason)
{
	if (!PendingHeadlessAcceptanceBridge.bActive)
	{
		return;
	}

	WriteHeadlessAcceptanceReceipt(
		TEXT("completed"),
		bAssistantSuccess,
		CloseoutDecision,
		FailureReason,
		Response.Left(1000));

	const bool bRequestExit = PendingHeadlessAcceptanceBridge.Request.bRequestEditorExitOnComplete;
	PendingHeadlessAcceptanceBridge = FPendingHeadlessAcceptanceBridge{};
	if (bRequestExit)
	{
		FPlatformMisc::RequestExit(false);
	}
}

void SOsvayderEditorWidget::WriteHeadlessAcceptanceReceipt(
	const FString& Status,
	const bool bAssistantSuccess,
	const FOsvayderUEActivePlanCloseoutDecision* CloseoutDecision,
	const FString& FailureReason,
	const FString& ResponsePreview)
{
	if (!PendingHeadlessAcceptanceBridge.bActive && Status != TEXT("pending"))
	{
		return;
	}

	FOsvayderUEHeadlessAcceptanceReceiptContext Context;
	Context.Request = PendingHeadlessAcceptanceBridge.Request;
	Context.ReceiptPath = PendingHeadlessAcceptanceBridge.ReceiptPath;
	Context.PromptHash = PendingHeadlessAcceptanceBridge.PromptHash;
	Context.StartedAtUtc = PendingHeadlessAcceptanceBridge.StartedAtUtc;
	Context.CompletedAtUtc = Status.Equals(TEXT("pending"), ESearchCase::CaseSensitive) ? FString() : MakeUtcNowText();
	Context.DispatchPath = GetHeadlessAcceptanceDispatchPath(PendingHeadlessAcceptanceBridge.Request);
	Context.Status = Status;
	Context.FailureReason = FailureReason;
	Context.bAssistantSuccess = bAssistantSuccess;
	Context.bHasCloseoutDecision = CloseoutDecision != nullptr;
	Context.ActiveRunIdFallback =
		FOsvayderUEAgentTraceLog::Get().GetActiveRunIdForBackend(PendingHeadlessAcceptanceBridge.Backend);
	Context.LatestCloseoutPath =
		FPaths::Combine(FOsvayderUERelayAgentManager::GetRelayRootDir(), TEXT("closeout_decision.json"));
	Context.VisibleSessionPath = FOsvayderCodeSubsystem::Get().GetSessionFilePath();
	Context.TracePath = FOsvayderUEAgentTraceLog::Get().GetTraceLogPath();
	Context.LogPaths = CollectCurrentPrefixLogPaths(
		PendingHeadlessAcceptanceBridge.Request.Prefix,
		PendingHeadlessAcceptanceBridge.StartedAtUtcDateTime);
	if (CloseoutDecision != nullptr)
	{
		Context.CloseoutDecision = *CloseoutDecision;
		Context.ArchivePath = CloseoutDecision->SourceArchivePath;
	}

	TSharedPtr<FJsonObject> Receipt = BuildHeadlessAcceptanceReceiptJson(Context);
	Receipt->SetStringField(TEXT("response_preview"), ResponsePreview);

	FString SaveError;
	if (!SaveHeadlessAcceptanceReceiptObject(Receipt, PendingHeadlessAcceptanceBridge.ReceiptPath, SaveError))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to persist headless acceptance receipt: %s"), *SaveError);
		return;
	}

	if (Status.Equals(TEXT("running"), ESearchCase::CaseSensitive)
		|| Status.Equals(TEXT("restart_survival_recovering"), ESearchCase::CaseSensitive))
	{
		SaveHeadlessAcceptancePendingReceiptObject(Receipt);
	}
	else
	{
		DeleteHeadlessAcceptancePendingReceipt();
	}
}

void SOsvayderEditorWidget::OnClaudeResponse(
	const FString& Response,
	bool bSuccess,
	const uint64 RequestGeneration,
	const EOsvayderUEProviderBackend RequestBackend)
{
	if (!IsActiveRequestCallback(RequestGeneration, RequestBackend, TEXT("response")))
	{
		return;
	}

	bIsWaitingForResponse = false;
	const FPendingClosedEditorBuildBlockerIntervention BlockerIntervention = PendingClosedEditorBuildBlockerIntervention;
	FOsvayderUEActivePlanCloseoutDecision HeadlessCloseoutDecision;
	bool bHeadlessHasCloseoutDecision = false;
	bool bHeadlessShouldComplete = false;
	FString HeadlessFailureReason;

	if (bSuccess)
	{
		ClearPendingClosedEditorBuildBlockerIntervention();
		ClearTransportRetryState();
		LastRuntimeConnectionState = EBackendRuntimeConnectionState::Connected;
		LastRuntimeConnectionDetail = FString::Printf(
			TEXT("%s completed the last request successfully."),
			*GetConfiguredBackendDisplayName());

		// If the final user-facing response differs from the streamed text,
		// replace the active bubble so the committed visible chat matches the
		// bounded product contract rather than intermediate commentary/tool chatter.
		if (StreamingTextBlock.IsValid()
			&& !Response.IsEmpty()
			&& !Response.Equals(StreamingResponse, ESearchCase::CaseSensitive))
		{
			ReplaceStreamingResponseWithFinalText(Response);
		}
		// If we have a streaming bubble but no progress was received (e.g., image mode
		// suppresses progress callbacks), populate it with the final response so
		// FinalizeStreamingResponse updates the existing bubble instead of leaving "Thinking..."
		else if (StreamingResponse.IsEmpty() && StreamingTextBlock.IsValid())
		{
			StreamingResponse = Response;
			CurrentSegmentText = Response;
			if (StreamingTextBlock.IsValid())
			{
				StreamingTextBlock->SetText(FText::FromString(Response));
			}
		}

		FinalizeStreamingResponse();

		LastResponse = StreamingResponse.IsEmpty() ? Response : StreamingResponse;

		// Only add a new bubble if we had no streaming bubble at all.
		// Otherwise mirror the finalized assistant text for bounded proof/readback helpers.
		if (StreamingResponse.IsEmpty())
		{
			AddMessage(Response, false);
		}
		else if (!LastResponse.IsEmpty())
		{
			TrackVisibleChatMessage(LastResponse);
		}

		FOsvayderUERestartSurvivalState RestartState;
		if (FOsvayderUERestartSurvivalManager::DescribeCurrentState(RestartState)
			&& RestartState.Backend == RequestBackend
			&& RestartState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
			&& RestartState.bProviderThreadResumePending)
		{
			FString ResumeValidationError;
			FOsvayderUERestartSurvivalManager::MarkReattachValidated(ResumeValidationError);

			if (RestartState.bPostReattachCompletionDispatched)
			{
				FOsvayderUERestartSurvivalState UpdatedState;
				FString LoadError;
				if (FOsvayderUERestartSurvivalManager::LoadState(UpdatedState, LoadError))
				{
					UpdatedState.bPostReattachCompletionPending = false;
					UpdatedState.bPostReattachCompletionDispatched = false;
					FString SaveError;
					FOsvayderUERestartSurvivalManager::SaveState(UpdatedState, SaveError);
				}
			}
		}

		const FString ResponseBeforeRestartDecision = LastResponse.IsEmpty() ? Response : LastResponse;
		TryStartPendingRestartSurvivalAfterResponse(RequestBackend);
		if (!FOsvayderUERestartSurvivalManager::HasPendingResume(RequestBackend))
		{
			const FOsvayderUEActivePlanCloseoutDecision CloseoutDecision =
				FinalizeActivePlan(ResponseBeforeRestartDecision, true, true);
			HeadlessCloseoutDecision = CloseoutDecision;
			bHeadlessHasCloseoutDecision = true;
			bHeadlessShouldComplete = true;
			const FString CloseoutWarning =
				BuildActivePlanCloseoutWarningText(CloseoutDecision, ResponseBeforeRestartDecision);
			if (!CloseoutWarning.IsEmpty())
			{
				AddMessage(CloseoutWarning, false);
			}
			else
			{
				// 631 Agent self-retrospective policy: check for `status: full` claim
				// against plugin-observed verification-tool invocations. Warns if
				// mismatch without modifying the agent's text.
				CheckSelfVerificationMismatchAndWarn(ResponseBeforeRestartDecision, RequestBackend);
			}
		}
	}
	else
	{
		if (BlockerIntervention.bActive)
		{
			bHeadlessShouldComplete = true;
			HeadlessFailureReason = BlockerIntervention.UserFacingMessage;
			ClearTransportRetryState();
			LastRuntimeConnectionState = BlockerIntervention.bRestartSurvivalArmed
				? EBackendRuntimeConnectionState::Connecting
				: EBackendRuntimeConnectionState::Failed;
			LastRuntimeConnectionDetail = BlockerIntervention.UserFacingMessage;
			FinalizeStreamingResponse();
			AddMessage(BlockerIntervention.UserFacingMessage, false);
			// 624 fix: when the arm succeeded, the detached owner has already been
			// spawned synchronously inside TryHandleClosedEditorBuildBlocker
			// BEFORE the transport reset. There is no longer a race to lose here,
			// so we deliberately skip the post-callback
			// TryStartPendingRestartSurvivalAfterResponse path that used to be
			// the late fall-back spawn site (it was the path that structurally
			// could not fire reliably after the transport had been reset). The
			// prepared-restore-request consumption + state-delete that sit
			// behind HasPreparedRestoreRequest() remain in place elsewhere in
			// the widget so subsequent reattach cycles can still re-arm
			// normally.
			ClearPendingClosedEditorBuildBlockerIntervention();
			StreamingResponse.Empty();
			CurrentRequestPromptText.Empty();
			CurrentRequestImagePaths.Reset();
			if (BlockerIntervention.bRestartSurvivalArmed && PendingHeadlessAcceptanceBridge.bActive)
			{
				WriteHeadlessAcceptanceReceipt(
					TEXT("restart_survival_recovering"),
					false,
					nullptr,
					TEXT("restart_survival_recovery_pending"),
					Response.Left(1000));
			}
			else
			{
				CompletePendingHeadlessAcceptanceBridge(
					Response,
					false,
					bHeadlessHasCloseoutDecision ? &HeadlessCloseoutDecision : nullptr,
					HeadlessFailureReason);
			}
			return;
		}

		ClearPendingClosedEditorBuildBlockerIntervention();
		FOsvayderUERestartSurvivalState RestartState;
		if (FOsvayderUERestartSurvivalManager::DescribeCurrentState(RestartState)
			&& RestartState.Backend == RequestBackend
			&& RestartState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
			&& RestartState.bProviderThreadResumePending
			&& RestartState.bPostReattachCompletionDispatched)
		{
			RestartState.bPostReattachCompletionDispatched = false;
			FString SaveError;
			FOsvayderUERestartSurvivalManager::SaveState(RestartState, SaveError);
		}

		if (RequestBackend == EOsvayderUEProviderBackend::CodexCli
			&& FCodexCliRunner::IsPersistentTransportFailureMessage(Response))
		{
			FAgentTransportFailureSummary TransportSummary;
			const bool bHasTransportSummary =
				FOsvayderUEAgentTraceLog::Get().TryGetLatestTransportFailureSummary(RequestBackend, TransportSummary);
			const FString FailureFamily = bHasTransportSummary && !TransportSummary.FailureFamily.IsEmpty()
				? TransportSummary.FailureFamily
				: FCodexCliRunner::ClassifyPersistentTransportFailureMessage(Response);
			FString RetryBlockReason = bHasTransportSummary
				? TransportSummary.RetryBlockReason
				: FString();
			bool bRetrySafe = bHasTransportSummary ? TransportSummary.bRetrySafe : false;
			if (CurrentRequestPromptText.IsEmpty())
			{
				bRetrySafe = false;
				RetryBlockReason = TEXT("exact_prompt_unavailable");
			}

			LastTransportFailureState.bActive = !CurrentRequestPromptText.IsEmpty();
			LastTransportFailureState.bRetrySafe = bRetrySafe;
			LastTransportFailureState.Prompt = CurrentRequestPromptText;
			LastTransportFailureState.SourceRunId = bHasTransportSummary ? TransportSummary.RunId : FString();
			LastTransportFailureState.FailureFamily = FailureFamily;
			LastTransportFailureState.FailureMessage = Response;
			LastTransportFailureState.RetryBlockReason = RetryBlockReason;

			LastRuntimeConnectionState = EBackendRuntimeConnectionState::TransportReset;
			LastRuntimeConnectionDetail = FString::Printf(
				TEXT("Previous request failed due to %s. The next request will start a fresh backend session. %s\nDiagnostic: %s"),
				*DescribeTransportFailureFamilyForUser(FailureFamily),
				bRetrySafe
					? TEXT("Retry Last can replay the exact last prompt because no tool calls or side effects were observed.")
					: *FString::Printf(
						TEXT("Retry Last is blocked because %s."),
						*DescribeTransportRetryBlockReason(RetryBlockReason)),
				*Response);
			FinalizeStreamingResponse();
			AddMessage(
				BuildTransportRetryNotice(
					GetConfiguredBackendDisplayName(),
					bRetrySafe,
					RetryBlockReason),
				false);
			bHeadlessShouldComplete = true;
			HeadlessFailureReason = LastRuntimeConnectionDetail;
		}
		else
		{
			ClearTransportRetryState();
			LastRuntimeConnectionState = EBackendRuntimeConnectionState::Failed;
			LastRuntimeConnectionDetail = Response;
			FinalizeStreamingResponse();
			AddMessage(FString::Printf(TEXT("Error: %s"), *Response), false);
			HeadlessCloseoutDecision = FinalizeActivePlan(Response, false, true);
			bHeadlessHasCloseoutDecision = true;
			bHeadlessShouldComplete = true;
			HeadlessFailureReason = Response;
		}
	}

	if (bHeadlessShouldComplete)
	{
		if (PendingHeadlessAcceptanceBridge.bActive)
		{
			CompletePendingHeadlessAcceptanceBridge(
				Response,
				bSuccess,
				bHeadlessHasCloseoutDecision ? &HeadlessCloseoutDecision : nullptr,
				HeadlessFailureReason);
		}
		else if (bHeadlessHasCloseoutDecision)
		{
			bool bRecoveredRequestExit = false;
			FString RecoveredReceiptError;
			if (TryCompleteRecoveredHeadlessAcceptanceReceipt(
				Response,
				bSuccess,
				HeadlessCloseoutDecision,
				bRecoveredRequestExit,
				RecoveredReceiptError))
			{
				UE_LOG(LogOsvayderUE, Display, TEXT("Recovered headless acceptance receipt after restart-survival reattach."));
				if (bRecoveredRequestExit)
				{
					FPlatformMisc::RequestExit(false);
				}
			}
			else if (!RecoveredReceiptError.IsEmpty())
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to recover headless acceptance receipt after restart-survival: %s"), *RecoveredReceiptError);
			}
		}
	}

	// Clear streaming state
	StreamingResponse.Empty();
	CurrentRequestPromptText.Empty();
	CurrentRequestImagePaths.Reset();
}

void SOsvayderEditorWidget::CancelRequest()
{
	const EOsvayderUEProviderBackend ActiveBackend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
	FOsvayderCodeSubsystem::Get().CancelCurrentRequest();
	FOsvayderUERestartSurvivalState RestartState;
	if (FOsvayderUERestartSurvivalManager::DescribeCurrentState(RestartState)
		&& RestartState.Backend == ActiveBackend
		&& FOsvayderUERestartSurvivalManager::IsDetachedOwnerPhaseActive(RestartState.Phase))
	{
		FString CancelError;
		FOsvayderUERelayAgentManager::WriteCancelRequest(TEXT("widget_cancelled_request"), CancelError);
	}
	FOsvayderUEAgentTraceLog::Get().MarkCancellation(
		ActiveBackend,
		TEXT("Cancellation requested from the shared editor widget."));
	InvalidateActiveRequestCallbacks();
	bIsWaitingForResponse = false;
	CurrentRequestPromptText.Empty();
	CurrentRequestImagePaths.Reset();
	ClearPendingClosedEditorBuildBlockerIntervention();
	LastRuntimeConnectionState = EBackendRuntimeConnectionState::Unknown;
	LastRuntimeConnectionDetail = TEXT("Last request was cancelled before completion.");
	AddMessage(TEXT("Request cancelled."), false);
}

void SOsvayderEditorWidget::HandleDictationStatusChanged(const FOsvayderUEVoiceDictationStatus& Status)
{
	LastDictationStatusDetail = Status.Detail;
	UE_LOG(
		LogOsvayderUE,
		Log,
		TEXT("VOICE_DICTATION_WIDGET_STATUS state=%s detail=\"%s\""),
		*GetDictationStateLabel(Status.State),
		*Status.Detail);

	if ((Status.State == EOsvayderUEVoiceDictationState::Failed
		|| Status.State == EOsvayderUEVoiceDictationState::Unavailable)
		&& InputArea.IsValid())
	{
		InputArea->FocusTextEntry();
	}
}

void SOsvayderEditorWidget::HandleDictationTranscriptReady(const FString& Transcript)
{
	if (Transcript.IsEmpty())
	{
		return;
	}

	FString NewText = InputArea.IsValid() ? InputArea->GetText() : CurrentInputText;
	if (!NewText.IsEmpty() && !NewText.EndsWith(TEXT("\n")))
	{
		NewText += TEXT("\n");
	}

	NewText += Transcript;
	CurrentInputText = NewText;

	if (InputArea.IsValid())
	{
		InputArea->SetText(NewText);
		InputArea->FocusTextEntry();
	}
}

FText SOsvayderEditorWidget::GetDictationStatusText() const
{
	return FText::FromString(LastDictationStatusDetail);
}

void SOsvayderEditorWidget::CopyToClipboard()
{
	if (!LastResponse.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*LastResponse);
		UE_LOG(LogOsvayderUE, Log, TEXT("Copied response to clipboard"));
	}
}

void SOsvayderEditorWidget::RestoreSession()
{
	RestoreCurrentBackendSession(false);
}

void SOsvayderEditorWidget::ClearTransportRetryState()
{
	LastTransportFailureState = FLastTransportFailureState{};
	PendingTransportRetryReplayContext = FPendingTransportRetryReplayContext{};
}

void SOsvayderEditorWidget::NewSession()
{
	const FString DisplayName = GetConfiguredBackendDisplayName();
	FString ContinuityArchivePath;
	FString ContinuityArchiveError;
	ArchiveActivePlanContinuitySnapshotForNewSession(ContinuityArchivePath, ContinuityArchiveError);
	FString RestartSurvivalDeleteError;
	FOsvayderUERestartSurvivalManager::DeleteState(RestartSurvivalDeleteError);
	FString ActivePlanDeleteError;
	FOsvayderUERelayAgentManager::DeleteActivePlan(ActivePlanDeleteError);
	FString RelayDeleteError;
	FOsvayderUERelayAgentManager::DeleteHandoffContext(RelayDeleteError);
	FOsvayderUERelayAgentManager::DeleteProgressLog(RelayDeleteError);
	FOsvayderUERelayAgentManager::DeleteRelayResult(RelayDeleteError);
	FOsvayderUERelayAgentManager::DeleteCancelRequest(RelayDeleteError);
	TSharedPtr<FJsonObject> TracePayload = MakeShared<FJsonObject>();
	TracePayload->SetStringField(TEXT("display_name"), DisplayName);
	TracePayload->SetStringField(
		TEXT("detail"),
		FString::Printf(
			TEXT("New %s session started. Visible chat, saved project-local %s session history, and backend conversation state were reset."),
			*DisplayName,
			*DisplayName));
	if (!ContinuityArchivePath.IsEmpty())
	{
		TracePayload->SetStringField(TEXT("continuity_active_plan_backup_path"), ContinuityArchivePath);
	}
	if (!ContinuityArchiveError.IsEmpty())
	{
		TracePayload->SetStringField(TEXT("continuity_active_plan_backup_error"), ContinuityArchiveError);
	}
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("new_session"),
		FOsvayderCodeSubsystem::Get().GetConfiguredBackend(),
		TracePayload);

	InvalidateActiveRequestCallbacks();
	ClearTransportRetryState();
	ResetVisibleChatState();
	FOsvayderCodeSubsystem::Get().ClearHistory();
	AddMessage(
		FString::Printf(
			TEXT("New %s session started. Visible chat, saved project-local %s session history, and backend conversation state were reset."),
			*DisplayName,
			*DisplayName),
		false);
	AddInitialStatusMessage();
}

bool SOsvayderEditorWidget::StartRestartSurvivalForCurrentTask(const FClaudeRestartSurvivalStartOptions& Options, FString& OutError)
{
	OutError.Reset();

	// 624 fix: closed-editor build-blocker escalations must spawn the detached
	// owner from inside the live turn (before CancelCurrentRequest resets the
	// persistent transport), so the normal "are we mid-request?" guard is
	// bypassed via Options.bBypassWaitingForResponseGuard in that narrow path.
	if (bIsWaitingForResponse && !Options.bBypassWaitingForResponseGuard)
	{
		OutError = TEXT("Restart survival cannot start while a backend response is still in flight.");
		return false;
	}

	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	if (Status.Backend != EOsvayderUEProviderBackend::CodexCli)
	{
		OutError = TEXT("Restart survival is currently supported only on the ordinary Codex runtime path.");
		return false;
	}

	if (!CanSendPromptForStatus(Status))
	{
		OutError = Status.Detail.IsEmpty()
			? TEXT("Configured backend is not ready to start restart survival.")
			: Status.Detail;
		return false;
	}

	if (!FCodexCliRunner::ShouldUsePersistentConversationTransport())
	{
		OutError = TEXT("Restart survival requires the persistent Codex app-server transport.");
		return false;
	}

	FOsvayderUERestartSurvivalState ExistingState;
	if (FOsvayderUERestartSurvivalManager::DescribeCurrentState(ExistingState))
	{
		if (!FOsvayderUERestartSurvivalManager::CanReplaceExistingStateForFreshStart(ExistingState))
		{
			OutError = FString::Printf(
				TEXT("Restart survival is already active for this project in phase %s."),
				OsvayderUERestartSurvivalPhaseToString(ExistingState.Phase));
			return false;
		}

		FString DeleteError;
		if (!FOsvayderUERestartSurvivalManager::DeleteState(DeleteError))
		{
			OutError = DeleteError;
			return false;
		}
	}

	FCodexCliRunner* CodexRunner = static_cast<FCodexCliRunner*>(FOsvayderCodeSubsystem::Get().GetActiveBackend());
	if (CodexRunner == nullptr)
	{
		OutError = TEXT("Configured backend is not an active Codex CLI runner.");
		return false;
	}

	FString ProviderThreadStatePath;
	FString ProviderSessionId;
	const bool bHasPreparedRestoreRequestCandidate =
		!Options.RestoreIntent.bEnabled
		&& !Options.FileWriteIntent.bEnabled
		&& FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest();

	FString EffectiveTaskIdOverride = Options.TaskIdOverride;
	FString EffectiveSessionIdOverride = Options.SessionIdOverride;
	FString EffectiveReattachTokenOverride = Options.ReattachTokenOverride;
	FString EffectiveReattachNoticeOverride = Options.ReattachNoticeOverride;
	FString EffectiveAdditionalRelaunchArguments = Options.AdditionalRelaunchArguments;
	FOsvayderUERestartSurvivalRestoreIntent EffectiveRestoreIntent = Options.RestoreIntent;
	FOsvayderUERestartSurvivalFileWriteIntent EffectiveFileWriteIntent = Options.FileWriteIntent;
	FOsvayderUERestartSurvivalProofState EffectiveProof = Options.Proof;
	FString PreparedRestoreRequestId;
	FString PreparedRestoreRequestCreatedAtUtc;
	FString EffectivePostReattachCompletionText;
	FOsvayderUERestartSurvivalOriginTaskContext EffectiveOriginTask;
	bool bUseRelayAgentForStart = false;
	bool bConsumedPreparedRestoreRequest = false;
	FOsvayderUERestartSurvivalPreparedRestoreRequest PreparedRequest;
	bool bHasPreparedRestoreRequest = false;

	if (bHasPreparedRestoreRequestCandidate)
	{
		FString RequestError;
		if (!FOsvayderUERestartSurvivalManager::LoadPreparedRestoreRequest(PreparedRequest, RequestError))
		{
			OutError = RequestError;
			return false;
		}
		bHasPreparedRestoreRequest = true;
	}

	if (!CodexRunner->ExportActiveThreadStateForRestartSurvival(ProviderThreadStatePath, ProviderSessionId))
	{
		if (bHasPreparedRestoreRequest && !PreparedRequest.LinkedProviderSessionId.IsEmpty())
		{
			ProviderThreadStatePath.Reset();
			ProviderSessionId = PreparedRequest.LinkedProviderSessionId;
		}
		else
		{
			OutError = TEXT("Restart survival requires an active Codex task. Send one successful prompt first, then try again.");
			return false;
		}
	}

	if (bHasPreparedRestoreRequest)
	{
		FString RequestError;
		if (!FOsvayderUERestartSurvivalManager::ValidatePreparedRestoreRequestForStart(
			PreparedRequest,
			Status.Backend,
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
			ProviderSessionId,
			RequestError))
		{
			OutError = RequestError;
			return false;
		}

		if (!Options.TaskIdOverride.IsEmpty() && Options.TaskIdOverride != PreparedRequest.TaskId)
		{
			OutError = TEXT("Prepared restart-survival restore request task_id does not match the explicit task override.");
			return false;
		}

		if (!Options.SessionIdOverride.IsEmpty() && Options.SessionIdOverride != PreparedRequest.SessionId)
		{
			OutError = TEXT("Prepared restart-survival restore request session_id does not match the explicit session override.");
			return false;
		}

		EffectiveTaskIdOverride = PreparedRequest.TaskId;
		EffectiveSessionIdOverride = PreparedRequest.SessionId;
		const bool bPreparedRestoreEnabled = PreparedRequest.bRestoreEnabled
			|| !PreparedRequest.AutosaveSourcePath.IsEmpty()
			|| !PreparedRequest.TargetPath.IsEmpty()
			|| !PreparedRequest.BackupPath.IsEmpty();
		EffectiveRestoreIntent.bEnabled = bPreparedRestoreEnabled;
		if (bPreparedRestoreEnabled)
		{
			EffectiveRestoreIntent.AutosaveSourcePath = PreparedRequest.AutosaveSourcePath;
			EffectiveRestoreIntent.TargetPath = PreparedRequest.TargetPath;
			EffectiveRestoreIntent.BackupPath = PreparedRequest.BackupPath;
			EffectiveRestoreIntent.Detail = PreparedRequest.Detail;
		}
		const bool bPreparedFileWriteEnabled = IsPreparedRequestFileWriteEnabled(PreparedRequest);
		EffectiveFileWriteIntent.bEnabled = bPreparedFileWriteEnabled;
		if (bPreparedFileWriteEnabled)
		{
			EffectiveFileWriteIntent.SourcePath = PreparedRequest.FileWriteSourcePath;
			EffectiveFileWriteIntent.TargetPath = PreparedRequest.FileWriteTargetPath;
			EffectiveFileWriteIntent.BackupPath = PreparedRequest.FileWriteBackupPath;
			EffectiveFileWriteIntent.Detail = PreparedRequest.FileWriteDetail.IsEmpty()
				? PreparedRequest.Detail
				: PreparedRequest.FileWriteDetail;
		}
		PreparedRestoreRequestId = PreparedRequest.RequestId;
		PreparedRestoreRequestCreatedAtUtc = PreparedRequest.CreatedAtUtc;
		EffectiveOriginTask = PreparedRequest.OriginTask;
		bUseRelayAgentForStart = PreparedRequest.bUseRelayAgent;
		EffectivePostReattachCompletionText =
			FOsvayderUERestartSurvivalManager::ResolvePreparedRequestPostReattachCompletionText(PreparedRequest);
		if (bUseRelayAgentForStart)
		{
			EffectivePostReattachCompletionText.Reset();
		}

		FOsvayderUERestartSurvivalPreparedStartOverride PreparedStartOverride;
		if (FOsvayderUERestartSurvivalManager::TryConsumePreparedRequestStartOverride(PreparedRequest, PreparedStartOverride))
		{
			if (EffectiveReattachTokenOverride.IsEmpty())
			{
				EffectiveReattachTokenOverride = PreparedStartOverride.ReattachTokenOverride;
			}

			if (EffectiveReattachNoticeOverride.IsEmpty())
			{
				EffectiveReattachNoticeOverride = PreparedStartOverride.ReattachNoticeOverride;
			}

			if (EffectiveAdditionalRelaunchArguments.IsEmpty())
			{
				EffectiveAdditionalRelaunchArguments = PreparedStartOverride.AdditionalRelaunchArguments;
			}

			if (!EffectiveProof.bEnabled && PreparedStartOverride.Proof.bEnabled)
			{
				EffectiveProof = PreparedStartOverride.Proof;
			}
		}

		bConsumedPreparedRestoreRequest = true;
	}

	const FString TaskId = EffectiveTaskIdOverride.IsEmpty()
		? MakeRestartSurvivalStableId(TEXT("task_"))
		: EffectiveTaskIdOverride;
	const FString SessionId = EffectiveSessionIdOverride.IsEmpty()
		? FString::Printf(TEXT("restart_survival_%s"), *TaskId)
		: EffectiveSessionIdOverride;
	const FString ReattachToken = EffectiveReattachTokenOverride.IsEmpty()
		? MakeRestartSurvivalStableId(TEXT("reattach_"))
		: EffectiveReattachTokenOverride;
	const FString ReattachNotice = EffectiveReattachNoticeOverride.IsEmpty()
		? BuildRestartSurvivalDefaultReattachNotice(TaskId)
		: EffectiveReattachNoticeOverride;

	const FAgentProviderExecutionControlManifest Manifest =
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifest();
	FOsvayderUERestartSurvivalSupportBundle SupportBundle;
	FString SupportBundleError;
	if (!FOsvayderUERestartSurvivalManager::TryResolveSupportBundle(SupportBundle, SupportBundleError))
	{
		OutError = SupportBundleError;
		return false;
	}

	if (bUseRelayAgentForStart)
	{
		FString RelayCleanupError;
		if (!ResetRelayArtifactsForFreshRun(RelayCleanupError))
		{
			OutError = RelayCleanupError;
			return false;
		}

		const FOsvayderUERelayHandoffContext RelayContext = BuildClosedEditorRelayHandoffContext(
			PreparedRequest,
			ReattachToken,
			ReattachNotice);
		FString RelaySaveError;
		if (!FOsvayderUERelayAgentManager::SaveHandoffContext(RelayContext, RelaySaveError))
		{
			OutError = RelaySaveError;
			return false;
		}
	}

	FOsvayderUERestartSurvivalState RestartState;
	RestartState.SessionId = SessionId;
	RestartState.TaskId = TaskId;
	RestartState.ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	RestartState.UProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	RestartState.Backend = Status.Backend;
	RestartState.BackendDisplayName = GetConfiguredBackendDisplayName();
	RestartState.ExecutionControlProfileId = Manifest.ControlProfileId;
	RestartState.ExecutionTransportLabel = Manifest.ExecutionTransportLabel;
	RestartState.ProviderSessionId = ProviderSessionId;
	RestartState.ProviderThreadStatePath = ProviderThreadStatePath;
	RestartState.bProviderThreadResumePending = true;
	RestartState.Phase = EOsvayderUERestartSurvivalPhase::Detaching;
	RestartState.PhaseDetail = TEXT("Detaching from UnrealEditor.exe into a local out-of-process restart-survival companion.");
	RestartState.ReattachToken = ReattachToken;
	RestartState.ReattachNotice = ReattachNotice;
	RestartState.EditorProcessId = FPlatformProcess::GetCurrentProcessId();
	RestartState.EditorExecutablePath = GetRestartSurvivalEditorExecutablePath();
	RestartState.EngineRoot = GetRestartSurvivalEngineRoot();
	RestartState.BuildBatchPath = FPaths::Combine(RestartState.EngineRoot, TEXT("Engine"), TEXT("Build"), TEXT("BatchFiles"), TEXT("Build.bat"));
	RestartState.BuildTarget = GetRestartSurvivalProjectEditorTargetName();
	RestartState.RelaunchArguments = BuildRestartSurvivalRelaunchArguments(ReattachToken, EffectiveAdditionalRelaunchArguments);
	RestartState.SupportBundleResolution = SupportBundle.ResolutionLabel;
	RestartState.SupportBundleRoot = SupportBundle.BundleRoot;
	RestartState.SupervisorScriptPath = SupportBundle.SupervisorScriptPath;
	RestartState.MonitorScriptPath = SupportBundle.MonitorScriptPath;
	RestartState.PreflightLauncherPath = SupportBundle.PreflightScriptPath;
	RestartState.DetachedObjective = bUseRelayAgentForStart
		? TEXT("closed_editor_complex_build_relay_v1")
		: TEXT("closed_editor_task_owner_continuity_v2");
	RestartState.DetachedObjectiveDetail = bUseRelayAgentForStart
		? TEXT("Relay-agent owns bounded complex closed-editor build/fix work with a separate Codex session while Unreal is closed.")
		: TEXT("Persist the same logical task identity through bounded local-only detached file/build/restore/relaunch/reattach steps.");
	RestartState.DetachedStepIndex = 0;
	RestartState.DetachedStepBudget = 3;
	RestartState.DetachedCurrentStep = TEXT("detaching");
	RestartState.DetachedPendingStep.Reset();
	RestartState.DetachedLastStepOutcome = TEXT("pending");
	RestartState.DetachedLastBlockerFamily.Reset();
	RestartState.DetachedLastBlockerSignature.Reset();
	RestartState.DetachedTerminalOutcome = TEXT("none");
	RestartState.bDetachedFileWriteCompleted = false;
	RestartState.bDetachedRestoreCompleted = false;
	RestartState.bDetachedBuildCompleted = false;
	RestartState.DetachedOwnerProcessId = 0;
	RestartState.bDetachedOwnerActive = false;
	RestartState.bDetachedOwnerManualReopenDetected = false;
	RestartState.bDetachedOwnerCrashObserved = false;
	RestartState.PreparedRestoreRequestId = PreparedRestoreRequestId;
	RestartState.PreparedRestoreRequestCreatedAtUtc = PreparedRestoreRequestCreatedAtUtc;
	RestartState.PostReattachCompletionText = EffectivePostReattachCompletionText;
	RestartState.bPostReattachCompletionPending = !EffectivePostReattachCompletionText.IsEmpty();
	RestartState.bPostReattachCompletionDispatched = false;
	RestartState.OriginTask = EffectiveOriginTask;
	RestartState.RestoreIntent = EffectiveRestoreIntent;
	RestartState.FileWriteIntent = EffectiveFileWriteIntent;
	RestartState.Proof = EffectiveProof;

	FString SaveError;
	if (!FOsvayderUERestartSurvivalManager::SaveState(RestartState, SaveError))
	{
		OutError = SaveError;
		return false;
	}

	const FString ScriptPath = RestartState.SupervisorScriptPath;
	const FString StatePath = FOsvayderUERestartSurvivalManager::GetStatePath();
	const FString PowerShellExe = GetRestartSurvivalPowerShellExecutablePath();
	const FString Params = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -StatePath \"%s\""),
		*ScriptPath,
		*StatePath);
	const FProcHandle SupervisorHandle = FPlatformProcess::CreateProc(
		*PowerShellExe,
		*Params,
		false,
		true,
		true,
		nullptr,
		0,
		nullptr,
		nullptr);
	if (!SupervisorHandle.IsValid())
	{
		const FString LaunchError = FString::Printf(
			TEXT("Failed to launch the detached PowerShell restart-survival companion via %s."),
			*PowerShellExe);
		FString IgnoreError;
		FOsvayderUERestartSurvivalManager::MarkFailed(LaunchError, IgnoreError);
		OutError = LaunchError;
		return false;
	}

	if (bConsumedPreparedRestoreRequest)
	{
		FString DeleteRequestError;
		FOsvayderUERestartSurvivalManager::DeletePreparedRestoreRequest(DeleteRequestError);
	}

	if (Options.bRequestEditorExit)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float)
			{
				FPlatformMisc::RequestExit(false);
				return false;
			}),
			0.2f);
	}

	return true;
}

void SOsvayderEditorWidget::StartRestartSurvival()
{
	FClaudeRestartSurvivalStartOptions Options;
	FString Error;
	if (!StartRestartSurvivalForCurrentTask(Options, Error))
	{
		LastRuntimeConnectionState = EBackendRuntimeConnectionState::Failed;
		LastRuntimeConnectionDetail = Error;
		AddMessage(Error, false);
	}
}

bool SOsvayderEditorWidget::RestoreCurrentBackendSession(const bool bAutomatic, const FString& LeadingNotice)
{
	FOsvayderCodeSubsystem& Subsystem = FOsvayderCodeSubsystem::Get();
	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	const EOsvayderUEProviderBackend Backend = Status.Backend;
	const FString DisplayName = GetBackendDisplayNameFromStatus(Status);
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();

	TSharedPtr<FJsonObject> RestoreAttemptPayload = MakeShared<FJsonObject>();
	RestoreAttemptPayload->SetBoolField(TEXT("automatic"), bAutomatic);
	RestoreAttemptPayload->SetStringField(TEXT("leading_notice"), LeadingNotice);
	RestoreAttemptPayload->SetStringField(TEXT("display_name"), DisplayName);
	FOsvayderUEAgentTraceLog::Get().AppendEvent(TEXT("restore_session"), Backend, RestoreAttemptPayload);

	auto CombineNotice = [](const FString& Prefix, const FString& Message)
	{
		if (Prefix.IsEmpty())
		{
			return Message;
		}

		if (Message.IsEmpty())
		{
			return Prefix;
		}

		return Prefix + TEXT("\n\n") + Message;
	};

	const TArray<TPair<FString, FString>>& ExistingHistory = Subsystem.GetHistory();
	if (ExistingHistory.Num() > 0)
	{
		RenderConversationHistory(
			ExistingHistory,
			CombineNotice(
				LeadingNotice,
				bAutomatic
					? FString::Printf(TEXT("Current project-local %s session history restored."), *DisplayName)
					: FString::Printf(TEXT("Current project-local %s session history restored."), *DisplayName)),
				bAutomatic
					? FString()
					: FString::Printf(TEXT("Restored %d exchanges for %s. Continue the session below."), ExistingHistory.Num(), *DisplayName));

		TSharedPtr<FJsonObject> RestoredPayload = MakeShared<FJsonObject>();
		RestoredPayload->SetBoolField(TEXT("automatic"), bAutomatic);
		RestoredPayload->SetStringField(TEXT("outcome"), TEXT("loaded_from_memory"));
		RestoredPayload->SetNumberField(TEXT("message_count"), ExistingHistory.Num());
		RestoredPayload->SetStringField(TEXT("display_name"), DisplayName);
		FOsvayderUEAgentTraceLog::Get().AppendEvent(TEXT("session_restored"), Backend, RestoredPayload);
		return true;
	}

	if (bAutomatic && (!Settings || !Settings->ShouldAutoRestoreSessionOnOpen()))
	{
		TSharedPtr<FJsonObject> RestoreOutcomePayload = MakeShared<FJsonObject>();
		RestoreOutcomePayload->SetBoolField(TEXT("automatic"), true);
		RestoreOutcomePayload->SetStringField(TEXT("display_name"), DisplayName);
		RestoreOutcomePayload->SetStringField(TEXT("outcome"), TEXT("auto_restore_disabled"));
		FOsvayderUEAgentTraceLog::Get().AppendEvent(TEXT("session_restored"), Backend, RestoreOutcomePayload);
		return false;
	}

	const FAgentSessionRestoreResult RestoreResult = Subsystem.LoadSessionWithResult();
	if (RestoreResult.WasLoaded())
	{
		const TArray<TPair<FString, FString>>& LoadedHistory = Subsystem.GetHistory();
		if (LoadedHistory.Num() == 0)
		{
			if (!bAutomatic)
			{
				AddMessage(
					CombineNotice(
						LeadingNotice,
						FString::Printf(TEXT("Saved %s session file loaded but contained no exchanges."), *DisplayName)),
					false);
			}
			return false;
		}

		const FString RestoreSource = RestoreResult.RequestedSession.SessionFilePath.IsEmpty()
			? DisplayName
			: RestoreResult.RequestedSession.SessionFilePath;
		RenderConversationHistory(
			LoadedHistory,
			CombineNotice(
				LeadingNotice,
				bAutomatic
					? FString::Printf(TEXT("Saved project-local %s session history restored automatically from disk."), *DisplayName)
					: FString::Printf(TEXT("Saved project-local %s session history restored from disk."), *DisplayName)),
			FString::Printf(
				TEXT("Restored %d exchanges from %s."),
				LoadedHistory.Num(),
				*RestoreSource));

		TSharedPtr<FJsonObject> RestoredPayload = MakeShared<FJsonObject>();
		RestoredPayload->SetBoolField(TEXT("automatic"), bAutomatic);
		RestoredPayload->SetStringField(TEXT("outcome"), TEXT("loaded_from_disk"));
		RestoredPayload->SetNumberField(TEXT("message_count"), LoadedHistory.Num());
		RestoredPayload->SetStringField(TEXT("restore_source"), RestoreSource);
		RestoredPayload->SetObjectField(TEXT("requested_session"), BuildSessionMetadataTraceObject(RestoreResult.RequestedSession));
		FOsvayderUEAgentTraceLog::Get().AppendEvent(TEXT("session_restored"), Backend, RestoredPayload);
		return true;
	}

	if (bAutomatic)
	{
		if (RestoreResult.Outcome == EAgentSessionRestoreOutcome::LegacySharedSessionBlocked && LeadingNotice.IsEmpty())
		{
			AddMessage(
				CombineNotice(
					LeadingNotice,
					FString::Printf(
						TEXT("Legacy shared session detected at %s, but it was not auto-restored because provider ownership is unsafe."),
						*RestoreResult.RequestedSession.SessionFilePath)),
				false);
		}
		return false;
	}

	FString FailureMessage;
	switch (RestoreResult.Outcome)
	{
	case EAgentSessionRestoreOutcome::LegacySharedSessionBlocked:
		FailureMessage = FString::Printf(
			TEXT("Legacy shared session detected at %s. It was not restored because shared `session.json` does not safely identify which provider owns it."),
			*RestoreResult.RequestedSession.SessionFilePath);
		break;

	case EAgentSessionRestoreOutcome::Failed:
		FailureMessage = RestoreResult.FailureReason.IsEmpty()
			? FString::Printf(TEXT("Failed to restore the saved %s session. The file may be corrupted or inaccessible."), *DisplayName)
			: RestoreResult.FailureReason;
		break;

	case EAgentSessionRestoreOutcome::NoSession:
	default:
		if (RestoreResult.SavedSessions.OtherProviderSession.bHasSession)
		{
			const FString OtherDisplayName = GetSessionMetadataDisplayName(RestoreResult.SavedSessions.OtherProviderSession);
			FailureMessage = FString::Printf(
				TEXT("No saved %s session was found. A separate %s session exists at %s and can be restored after switching providers."),
				*DisplayName,
				*OtherDisplayName,
				*RestoreResult.SavedSessions.OtherProviderSession.SessionFilePath);
		}
		else
		{
			FailureMessage = FString::Printf(TEXT("No saved %s session was found."), *DisplayName);
		}

		if (RestoreResult.SavedSessions.LegacySharedSession.bHasSession)
		{
			FailureMessage += FString::Printf(
				TEXT(" Legacy shared session remains at %s but is blocked from provider restore."),
				*RestoreResult.SavedSessions.LegacySharedSession.SessionFilePath);
		}
		break;
	}

	AddMessage(CombineNotice(LeadingNotice, FailureMessage), false);

	TSharedPtr<FJsonObject> RestoreOutcomePayload = MakeShared<FJsonObject>();
	RestoreOutcomePayload->SetBoolField(TEXT("automatic"), bAutomatic);
	RestoreOutcomePayload->SetStringField(TEXT("display_name"), DisplayName);
	RestoreOutcomePayload->SetStringField(TEXT("failure_message"), FailureMessage);
	RestoreOutcomePayload->SetObjectField(TEXT("requested_session"), BuildSessionMetadataTraceObject(RestoreResult.RequestedSession));
	RestoreOutcomePayload->SetObjectField(TEXT("current_provider_session"), BuildSessionMetadataTraceObject(RestoreResult.SavedSessions.CurrentProviderSession));
	RestoreOutcomePayload->SetObjectField(TEXT("other_provider_session"), BuildSessionMetadataTraceObject(RestoreResult.SavedSessions.OtherProviderSession));
	RestoreOutcomePayload->SetObjectField(TEXT("legacy_shared_session"), BuildSessionMetadataTraceObject(RestoreResult.SavedSessions.LegacySharedSession));
	switch (RestoreResult.Outcome)
	{
	case EAgentSessionRestoreOutcome::Loaded:
		RestoreOutcomePayload->SetStringField(TEXT("outcome"), TEXT("loaded"));
		break;
	case EAgentSessionRestoreOutcome::LegacySharedSessionBlocked:
		RestoreOutcomePayload->SetStringField(TEXT("outcome"), TEXT("legacy_shared_session_blocked"));
		break;
	case EAgentSessionRestoreOutcome::Failed:
		RestoreOutcomePayload->SetStringField(TEXT("outcome"), TEXT("failed"));
		break;
	case EAgentSessionRestoreOutcome::NoSession:
	default:
		RestoreOutcomePayload->SetStringField(TEXT("outcome"), TEXT("no_session"));
		break;
	}
	FOsvayderUEAgentTraceLog::Get().AppendEvent(TEXT("session_restored"), Backend, RestoreOutcomePayload);
	return false;
}

bool SOsvayderEditorWidget::TryRestoreSessionOnOpen()
{
	FString LeadingNotice;
	FOsvayderUERestartSurvivalState RestartState;
	FString ReattachToken;
	bool bRestartSurvivalReattached = false;
	const bool bHasReattachToken = TryGetRestartSurvivalTokenFromCommandLine(ReattachToken);
	if (FOsvayderUERestartSurvivalManager::DescribeCurrentState(RestartState)
		&& RestartState.Backend == GetConfiguredBackendStatus().Backend
		&& bHasReattachToken)
	{
		FString ReattachError;
		bRestartSurvivalReattached = FOsvayderUERestartSurvivalManager::TryMarkReattached(
			GetConfiguredBackendStatus().Backend,
			ReattachToken,
			LeadingNotice,
			ReattachError);
	}
	else if (FOsvayderUERestartSurvivalManager::DescribeCurrentState(RestartState)
		&& RestartState.Backend == GetConfiguredBackendStatus().Backend
		&& !bHasReattachToken
		&& RestartState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingReattach
		&& RestartState.bDetachedOwnerManualReopenDetected)
	{
		FString ReattachError;
		bRestartSurvivalReattached = FOsvayderUERestartSurvivalManager::TryMarkReattachedFromManualReopen(
			GetConfiguredBackendStatus().Backend,
			LeadingNotice,
			ReattachError);
	}

	if (!bHasReattachToken
		&& FOsvayderUERestartSurvivalManager::DescribeCurrentState(RestartState)
		&& RestartState.Backend == GetConfiguredBackendStatus().Backend
		&& FOsvayderUERestartSurvivalManager::IsDetachedOwnerPhaseActive(RestartState.Phase)
		&& RestartState.EditorProcessId != FPlatformProcess::GetCurrentProcessId())
	{
		if (!RestartState.bDetachedOwnerManualReopenDetected)
		{
			RestartState.bDetachedOwnerManualReopenDetected = true;
			RestartState.PhaseDetail = TEXT("Manual editor reopen detected while the same task is still continuing outside the editor.");
			FString SaveError;
			FOsvayderUERestartSurvivalManager::SaveState(RestartState, SaveError);
		}

		LeadingNotice = FString::Printf(
			TEXT("Continuing outside the editor is still active for task %s. This reopened editor will wait for the same task to finish and reattach instead of starting competing ownership."),
			*RestartState.TaskId);
		LastDetachedOwnerWaitNoticeTaskId = RestartState.TaskId;
	}

	const bool bRestoredSession = RestoreCurrentBackendSession(true, LeadingNotice);
	if (bRestoredSession)
	{
		TryDispatchPendingRestartSurvivalCompletion();
		return true;
	}

	if (!LeadingNotice.IsEmpty())
	{
		AddMessage(LeadingNotice, false);
	}

	if (bRestartSurvivalReattached)
	{
		TryDispatchPendingRestartSurvivalCompletion();
		return true;
	}

	FOsvayderUERestartSurvivalState PendingRestartState;
	FString PendingLoadError;
	if (FOsvayderUERestartSurvivalManager::LoadState(PendingRestartState, PendingLoadError)
		&& PendingRestartState.Backend == GetConfiguredBackendStatus().Backend
		&& PendingRestartState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
		&& PendingRestartState.bProviderThreadResumePending
		&& PendingRestartState.bPostReattachCompletionPending
		&& !PendingRestartState.bPostReattachCompletionDispatched
		&& !PendingRestartState.PostReattachCompletionText.IsEmpty())
	{
		TryDispatchPendingRestartSurvivalCompletion();
		return true;
	}

	return false;
}

void SOsvayderEditorWidget::TryDispatchPendingRestartSurvivalCompletion()
{
	if (bIsWaitingForResponse)
	{
		return;
	}

	FOsvayderUERestartSurvivalState RestartState;
	FString LoadError;
	if (!FOsvayderUERestartSurvivalManager::LoadState(RestartState, LoadError))
	{
		return;
	}

	if (RestartState.Backend != GetConfiguredBackendStatus().Backend
		|| RestartState.Phase != EOsvayderUERestartSurvivalPhase::Reattached
		|| !RestartState.bProviderThreadResumePending
		|| RestartState.PostReattachCompletionText.IsEmpty()
		|| RestartState.bPostReattachCompletionDispatched)
	{
		return;
	}

	DebugClearInputText();
	MarkActivePlanForPostReattachVerification(RestartState);
	bResumeExistingActivePlanOnNextSend = true;
	bUsePostReattachResumePolicyOnNextSend = true;
	FString LastCompletedToolCallId = RestartState.TaskId;
	FOsvayderUEActivePlan ActivePlan;
	FString ActivePlanError;
	if (FOsvayderUERelayAgentManager::LoadActivePlan(ActivePlan, ActivePlanError)
		&& !ActivePlan.LastCompletedToolCallId.IsEmpty())
	{
		LastCompletedToolCallId = ActivePlan.LastCompletedToolCallId;
	}
	AddMessage(
		FString::Printf(
			TEXT("Reopening editor and reattaching: continue the active plan from last_completed_tool_call_id=%s."),
			*LastCompletedToolCallId),
		false);
	DebugSetInputText(RestartState.PostReattachCompletionText);
	if (!DebugSendCurrentInput())
	{
		bResumeExistingActivePlanOnNextSend = false;
		bUsePostReattachResumePolicyOnNextSend = false;
		return;
	}

	const FString ResumedRunId = FOsvayderUEAgentTraceLog::Get().GetActiveRunIdForBackend(GetConfiguredBackendStatus().Backend);
	if (!ResumedRunId.IsEmpty())
	{
		TSharedPtr<FJsonObject> ContinuationPayload = MakeShared<FJsonObject>();
		ContinuationPayload->SetStringField(TEXT("originating_run_id"), RestartState.OriginTask.OriginatingRunId);
		ContinuationPayload->SetStringField(TEXT("restart_survival_task_id"), RestartState.TaskId);
		ContinuationPayload->SetStringField(TEXT("restart_survival_session_id"), RestartState.SessionId);
		ContinuationPayload->SetStringField(TEXT("prepared_restore_request_id"), RestartState.PreparedRestoreRequestId);
		ContinuationPayload->SetStringField(TEXT("originating_task_mode"), RestartState.OriginTask.OriginatingTaskMode);
		ContinuationPayload->SetStringField(TEXT("originating_requested_tool_family"), RestartState.OriginTask.OriginatingRequestedToolFamily);
		ContinuationPayload->SetStringField(TEXT("originating_primary_mutation_tool_family"), RestartState.OriginTask.OriginatingPrimaryMutationToolFamily);
		FOsvayderUEAgentTraceLog::Get().AppendEvent(
			TEXT("restart_survival_post_reattach_continuation_dispatched"),
			GetConfiguredBackendStatus().Backend,
			ContinuationPayload,
			ResumedRunId);
	}

	RestartState.bPostReattachCompletionDispatched = true;
	FString SaveError;
	FOsvayderUERestartSurvivalManager::SaveState(RestartState, SaveError);
}

void SOsvayderEditorWidget::PollRestartSurvivalDetachedOwnerState()
{
	// 606 Part B (2026-04-18): refresh the Agent 2 background banner display
	// state before any other early-return branch. Banner visibility is
	// independent of bIsWaitingForResponse: the supervisor may be alive even
	// while the widget is mid-turn, and the user still needs the awareness
	// signal. Runs on the same 1 s polling tick that drives this function —
	// no new timer.
	{
		FOsvayderUERestartSurvivalState BannerRestartState;
		FString BannerLoadError;
		if (FOsvayderUERestartSurvivalManager::LoadState(BannerRestartState, BannerLoadError))
		{
			const bool bOwnerProcessRunning =
				BannerRestartState.DetachedOwnerProcessId > 0
				&& IsRestartSurvivalOwnerProcessRunning(BannerRestartState.DetachedOwnerProcessId);
			UpdateAgent2BannerStateFromRestartState(&BannerRestartState, bOwnerProcessRunning);
		}
		else
		{
			UpdateAgent2BannerStateFromRestartState(nullptr, false);
		}
	}

	if (bIsWaitingForResponse)
	{
		return;
	}

	FOsvayderUERestartSurvivalState RestartState;
	FString LoadError;
	if (!FOsvayderUERestartSurvivalManager::LoadState(RestartState, LoadError)
		|| RestartState.Backend != GetConfiguredBackendStatus().Backend)
	{
		LastDetachedOwnerWaitNoticeTaskId.Reset();
		return;
	}

	FString ReattachToken;
	const bool bHasReattachToken = TryGetRestartSurvivalTokenFromCommandLine(ReattachToken);
	const bool bManualReopenContext =
		!bHasReattachToken
		&& RestartState.EditorProcessId != 0
		&& RestartState.EditorProcessId != FPlatformProcess::GetCurrentProcessId();

	if (bManualReopenContext
		&& FOsvayderUERestartSurvivalManager::IsDetachedOwnerPhaseActive(RestartState.Phase))
	{
		if (!RestartState.bDetachedOwnerManualReopenDetected)
		{
			RestartState.bDetachedOwnerManualReopenDetected = true;
			RestartState.PhaseDetail = TEXT("Manual editor reopen detected while the same task is still continuing outside the editor.");
			FString SaveError;
			FOsvayderUERestartSurvivalManager::SaveState(RestartState, SaveError);
		}

		if (!RestartState.TaskId.IsEmpty() && LastDetachedOwnerWaitNoticeTaskId != RestartState.TaskId)
		{
			AddMessage(
				FString::Printf(
					TEXT("Continuing outside the editor is still active for task %s. Waiting for the same run to finish and reattach instead of forking competing ownership."),
					*RestartState.TaskId),
				false);
			LastDetachedOwnerWaitNoticeTaskId = RestartState.TaskId;
		}

		if (RestartState.bDetachedOwnerActive
			&& RestartState.DetachedOwnerProcessId > 0
			&& !IsRestartSurvivalOwnerProcessRunning(RestartState.DetachedOwnerProcessId))
		{
			RestartState.Phase = EOsvayderUERestartSurvivalPhase::FailedTerminal;
			RestartState.PhaseDetail = TEXT("The outside-editor continuation crashed or exited before reaching a terminal outcome.");
			RestartState.FailureReason = RestartState.PhaseDetail;
			RestartState.DetachedTerminalOutcome = TEXT("failed");
			RestartState.bDetachedOwnerActive = false;
			RestartState.DetachedOwnerProcessId = 0;
			RestartState.bDetachedOwnerCrashObserved = true;
			FString SaveError;
			FOsvayderUERestartSurvivalManager::SaveState(RestartState, SaveError);
			AddMessage(FString::Printf(TEXT("Restart-survival infrastructure failure: %s"), *RestartState.FailureReason), false);
		}
		return;
	}

	if (bManualReopenContext
		&& RestartState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingReattach
		&& RestartState.bProviderThreadResumePending
		&& RestartState.bDetachedOwnerManualReopenDetected)
	{
		FString Notice;
		FString ReattachError;
		if (FOsvayderUERestartSurvivalManager::TryMarkReattachedFromManualReopen(
			GetConfiguredBackendStatus().Backend,
			Notice,
			ReattachError))
		{
			if (!Notice.IsEmpty())
			{
				AddMessage(Notice, false);
			}
			TryDispatchPendingRestartSurvivalCompletion();
			return;
		}
	}

	if (RestartState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
		&& RestartState.bPostReattachCompletionPending
		&& !RestartState.bPostReattachCompletionDispatched)
	{
		TryDispatchPendingRestartSurvivalCompletion();
	}
}

bool SOsvayderEditorWidget::ShouldShowAgent2BackgroundBanner(
	const FOsvayderUERestartSurvivalState& RestartState,
	const bool bDetachedOwnerProcessRunning)
{
	// Terminal phases — never show. Reattached = task is already handed back to
	// Agent 1, FailedTerminal = supervisor stopped with failure (widget surfaces
	// via other channels), AttachedInEditor = no active detach cycle.
	if (RestartState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
		|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::FailedTerminal
		|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::AttachedInEditor)
	{
		return false;
	}

	// Non-terminal phases with owner pid populated — trust the caller's process
	// liveness check. If the process died, hide the banner; the next polling
	// tick's crash-detection path (at :2492-2509 in the same function) will
	// also flip the state file to FailedTerminal so this agrees on the very
	// next tick.
	if (RestartState.DetachedOwnerProcessId > 0 && !bDetachedOwnerProcessRunning)
	{
		return false;
	}

	// Early Detaching window: DetachedOwnerProcessId may not be populated yet
	// (the plugin writes it after CreateProc returns), but the state file is
	// already on disk from SaveState. Default to visible so the user sees the
	// banner immediately at arm-time; the polling tick refines this within 1 s.
	return true;
}

FString SOsvayderEditorWidget::BuildAgent2BannerCancelRequestPayload()
{
	// Same schema as the monitor-window Cancel button (schema_version=1,
	// requested_at_utc, reason) — see OsvayderUE-RestartSurvivalMonitor.ps1
	// :668-677. Only the reason string differs for audit.
	return FString::Printf(
		TEXT("{\n  \"schema_version\": 1,\n  \"requested_at_utc\": \"%s\",\n  \"reason\": \"user_requested_cancel_from_widget_banner\"\n}\n"),
		*FDateTime::UtcNow().ToIso8601());
}

FString SOsvayderEditorWidget::DescribeTransportRetryBlockReason(const FString& RetryBlockReason)
{
	if (RetryBlockReason.Equals(TEXT("exact_prompt_unavailable"), ESearchCase::CaseSensitive))
	{
		return TEXT("the exact last prompt is no longer available for bounded replay");
	}
	if (RetryBlockReason.Equals(TEXT("tool_activity_observed"), ESearchCase::CaseSensitive))
	{
		return TEXT("tool activity was already observed, so side effects may have started");
	}
	if (RetryBlockReason.Equals(TEXT("restart_survival_handoff_observed"), ESearchCase::CaseSensitive))
	{
		return TEXT("restart-survival handoff was already observed");
	}
	if (RetryBlockReason.Equals(TEXT("verification_side_effect_observed"), ESearchCase::CaseSensitive))
	{
		return TEXT("verification side effects were already observed");
	}
	if (RetryBlockReason.Equals(TEXT("mutation_side_effect_observed"), ESearchCase::CaseSensitive))
	{
		return TEXT("file mutation side effects were already observed");
	}

	return TEXT("side effects may already have started");
}

FString SOsvayderEditorWidget::DescribeTaskLaneStatus(const FOsvayderUETaskLaneState& LaneState)
{
	return DescribeTaskLaneStatusInternal(LaneState);
}

FString SOsvayderEditorWidget::BuildTransportRetryStatusLabel(const bool bRetrySafe)
{
	return bRetrySafe
		? TEXT("[working in editor / transport-reset] Previous request lost the backend session; Retry Last starts a fresh session")
		: TEXT("[working in editor / transport-reset] Previous request lost the backend session after tool activity");
}

FString SOsvayderEditorWidget::BuildTransportRetryNotice(
	const FString& BackendDisplayName,
	const bool bRetrySafe,
	const FString& RetryBlockReason)
{
	if (bRetrySafe)
	{
		return FString::Printf(
			TEXT("Working in editor: the previous %s request did not complete because the backend session was reset. Retry Last can replay the exact last prompt on a fresh backend session."),
			*BackendDisplayName);
	}

	return FString::Printf(
		TEXT("Working in editor: the previous %s request lost the backend session after tool activity. Retry Last is blocked because %s; review the partial work and continue with a new turn manually."),
		*BackendDisplayName,
		*DescribeTransportRetryBlockReason(RetryBlockReason));
}

FString SOsvayderEditorWidget::BuildTaskRecoveryAutoResumePrompt()
{
	return TEXT("[SYSTEM-GENERATED TASK RECOVERY AUTO-RESUME]\nContinue the interrupted task from the approved [TASK RECOVERY] context. Preserve the same active plan and feature-workflow identity if present, resume from the exact interrupted point, and do not restart the task from scratch.");
}

FString SOsvayderEditorWidget::BuildTaskRecoveryAutoResumeVisibleMarker()
{
	return TEXT("[System-generated task recovery auto-resume]");
}

FOpenEditorBuildLockCloseSafetyDecision SOsvayderEditorWidget::EvaluateOpenEditorBuildLockCloseSafety(
	const FOpenEditorBuildLockCloseSafetyInputs& Inputs)
{
	FOpenEditorBuildLockCloseSafetyDecision Decision;

	TArray<FString> BlockReasonCodes;
	TArray<FString> UserReasons;

	if (Inputs.DirtyPackageCount > 0)
	{
		BlockReasonCodes.Add(TEXT("unsaved_packages"));
		UserReasons.Add(Inputs.DirtyPackageSummary.IsEmpty()
			? FString::Printf(
				TEXT("Unreal has %d unsaved package%s"),
				Inputs.DirtyPackageCount,
				Inputs.DirtyPackageCount == 1 ? TEXT("") : TEXT("s"))
			: FString::Printf(
				TEXT("Unreal has %d unsaved package%s (%s)"),
				Inputs.DirtyPackageCount,
				Inputs.DirtyPackageCount == 1 ? TEXT("") : TEXT("s"),
				*Inputs.DirtyPackageSummary));
	}

	if (Inputs.bPlaySessionActive)
	{
		BlockReasonCodes.Add(TEXT("play_session_active"));
		UserReasons.Add(TEXT("a PIE or Simulate session is active"));
	}

	if (Inputs.bModalWindowActive)
	{
		BlockReasonCodes.Add(TEXT("modal_dialog_active"));
		UserReasons.Add(Inputs.ModalWindowTitle.IsEmpty()
			? FString(TEXT("a modal dialog is open"))
			: FString::Printf(TEXT("modal dialog \"%s\" is open"), *Inputs.ModalWindowTitle));
	}

	if (Inputs.bRestartSurvivalStateActive)
	{
		BlockReasonCodes.Add(TEXT("restart_survival_state_active"));
		UserReasons.Add(Inputs.RestartSurvivalPhase.IsEmpty()
			? FString(TEXT("restart-survival is already active"))
			: FString::Printf(TEXT("restart-survival is already active in phase %s"), *Inputs.RestartSurvivalPhase));
	}

	if (BlockReasonCodes.Num() == 0)
	{
		Decision.bCanAutoClose = true;
		return Decision;
	}

	Decision.BlockReasonCode = BlockReasonCodes.Num() == 1
		? BlockReasonCodes[0]
		: TEXT("multiple_unsafe_editor_states");

	if (Decision.BlockReasonCode == TEXT("unsaved_packages"))
	{
		Decision.UserFacingMessage = Inputs.DirtyPackageSummary.IsEmpty()
			? FString::Printf(
				TEXT("Open-editor build lock detected. Automatic closed-editor escalation is unsafe because Unreal has %d unsaved package%s. Save or discard those changes, then retry."),
				Inputs.DirtyPackageCount,
				Inputs.DirtyPackageCount == 1 ? TEXT("") : TEXT("s"))
			: FString::Printf(
				TEXT("Open-editor build lock detected. Automatic closed-editor escalation is unsafe because Unreal has %d unsaved package%s: %s. Save or discard those changes, then retry."),
				Inputs.DirtyPackageCount,
				Inputs.DirtyPackageCount == 1 ? TEXT("") : TEXT("s"),
				*Inputs.DirtyPackageSummary);
	}
	else if (Decision.BlockReasonCode == TEXT("play_session_active"))
	{
		Decision.UserFacingMessage = TEXT("Open-editor build lock detected. Automatic closed-editor escalation is unsafe while a PIE or Simulate session is active. Stop the session, then retry.");
	}
	else if (Decision.BlockReasonCode == TEXT("modal_dialog_active"))
	{
		Decision.UserFacingMessage = Inputs.ModalWindowTitle.IsEmpty()
			? FString(TEXT("Open-editor build lock detected. Automatic closed-editor escalation is unsafe while a modal dialog is open. Close or resolve that dialog, then retry."))
			: FString::Printf(
				TEXT("Open-editor build lock detected. Automatic closed-editor escalation is unsafe while modal dialog \"%s\" is open. Close or resolve that dialog, then retry."),
				*Inputs.ModalWindowTitle);
	}
	else if (Decision.BlockReasonCode == TEXT("restart_survival_state_active"))
	{
		Decision.UserFacingMessage = Inputs.RestartSurvivalPhase.IsEmpty()
			? FString(TEXT("Open-editor build lock detected. Automatic closed-editor escalation is unsafe because restart-survival is already active for this project. Let that lane finish or fail before retrying."))
			: FString::Printf(
				TEXT("Open-editor build lock detected. Automatic closed-editor escalation is unsafe because restart-survival is already active in phase %s. Let that lane finish or fail before retrying."),
				*Inputs.RestartSurvivalPhase);
	}
	else
	{
		Decision.UserFacingMessage = FString::Printf(
			TEXT("Open-editor build lock detected. Automatic closed-editor escalation is unsafe because %s. Resolve those editor states and retry."),
			*FString::Join(UserReasons, TEXT("; ")));
	}

	return Decision;
}

FTaskRecoveryAutoDispatchDecision SOsvayderEditorWidget::EvaluateTaskRecoveryAutoDispatch(
	const bool bIsWaitingForResponse,
	const bool bBackendCanSendPrompt,
	const FString& BackendBlockedDetail,
	const bool bUnsafeTransportRetry,
	const FString& TransportRetryBlockReason)
{
	FTaskRecoveryAutoDispatchDecision Decision;

	if (bIsWaitingForResponse)
	{
		Decision.BlockReasonCode = TEXT("request_already_running");
		Decision.UserFacingMessage = TEXT("Task recovery Continue is armed, but auto-resume cannot start yet because another request is already running. Finish or cancel the current request, then send the next turn manually.");
		return Decision;
	}

	if (bUnsafeTransportRetry)
	{
		Decision.BlockReasonCode = TEXT("transport_retry_blocked");
		Decision.UserFacingMessage = FString::Printf(
			TEXT("Task recovery Continue is armed, but auto-resume is blocked because the previous backend transport reset is not retry-safe: %s. Review the prior turn and send an explicit next message when ready."),
			*DescribeTransportRetryBlockReason(TransportRetryBlockReason));
		return Decision;
	}

	if (!bBackendCanSendPrompt)
	{
		Decision.BlockReasonCode = TEXT("backend_unavailable");
		Decision.UserFacingMessage = BackendBlockedDetail.IsEmpty()
			? TEXT("Task recovery Continue is armed, but auto-resume cannot start because the backend/session is unavailable. Send the next turn manually after the backend is ready.")
			: FString::Printf(
				TEXT("Task recovery Continue is armed, but auto-resume cannot start because the backend/session is unavailable: %s"),
				*BackendBlockedDetail);
		return Decision;
	}

	Decision.bCanAutoDispatch = true;
	return Decision;
}

void SOsvayderEditorWidget::UpdateAgent2BannerStateFromRestartState(
	const FOsvayderUERestartSurvivalState* RestartState,
	const bool bDetachedOwnerProcessRunning)
{
	if (RestartState == nullptr
		|| !ShouldShowAgent2BackgroundBanner(*RestartState, bDetachedOwnerProcessRunning))
	{
		Agent2BannerState = FAgent2BackgroundBannerState{};
		return;
	}

	Agent2BannerState.bVisible = true;
	Agent2BannerState.PhaseLabel = OsvayderUERestartSurvivalPhaseToString(RestartState->Phase);
	Agent2BannerState.LaneStatusLabel = DescribeTaskLaneStatusInternal(RestartState->LaneState);

	// Short task id suffix — last 8 characters, mirrors the convention used by
	// journal entries and bridge messages ("task_autonomous_019d9bfb_6BB7EC04").
	const FString& TaskId = RestartState->TaskId;
	Agent2BannerState.TaskIdShort = TaskId.Len() <= 8 ? TaskId : TaskId.Right(8);

	Agent2BannerState.StateFilePath = FOsvayderUERestartSurvivalManager::GetStatePath();
	Agent2BannerState.MonitorScriptPath = RestartState->MonitorScriptPath.IsEmpty()
		? FOsvayderUERestartSurvivalManager::GetMonitorScriptPath()
		: RestartState->MonitorScriptPath;

	// The monitor computes CancelPath via Get-RelayPath(StateFilePath, "relay_cancel_request.json")
	// at OsvayderUE-RestartSurvivalMonitor.ps1:402. We mirror that here so the
	// banner's Cancel button writes to the same disk artifact the supervisor polls.
	Agent2BannerState.CancelRequestPath = FPaths::Combine(
		FPaths::GetPath(Agent2BannerState.StateFilePath),
		TEXT("relay_cancel_request.json"));
}

TSharedRef<SWidget> SOsvayderEditorWidget::BuildAgent2BackgroundBanner()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.0f, 4.0f))
		.Visibility(this, &SOsvayderEditorWidget::GetAgent2BackgroundBannerVisibility)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SOsvayderEditorWidget::GetAgent2BackgroundBannerText)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("Agent2Banner_ShowMonitor", "Show Monitor"))
				.OnClicked(this, &SOsvayderEditorWidget::HandleAgent2BannerShowMonitor)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("Agent2Banner_Cancel", "Cancel"))
				.OnClicked(this, &SOsvayderEditorWidget::HandleAgent2BannerCancel)
			]
		];
}

EVisibility SOsvayderEditorWidget::GetAgent2BackgroundBannerVisibility() const
{
	return Agent2BannerState.bVisible ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SOsvayderEditorWidget::GetAgent2BackgroundBannerText() const
{
	return FText::FromString(FString::Printf(
		TEXT("%s — phase: %s, task: %s"),
		Agent2BannerState.LaneStatusLabel.IsEmpty() ? TEXT("Continuing outside the editor") : *Agent2BannerState.LaneStatusLabel,
		Agent2BannerState.PhaseLabel.IsEmpty() ? TEXT("unknown") : *Agent2BannerState.PhaseLabel,
		Agent2BannerState.TaskIdShort.IsEmpty() ? TEXT("unknown") : *Agent2BannerState.TaskIdShort));
}

FReply SOsvayderEditorWidget::HandleAgent2BannerShowMonitor()
{
	if (!Agent2BannerState.bVisible
		|| Agent2BannerState.MonitorScriptPath.IsEmpty()
		|| Agent2BannerState.StateFilePath.IsEmpty())
	{
		AddMessage(TEXT("Show Monitor skipped: banner state is empty or paths are missing."), false);
		return FReply::Handled();
	}

	// Mirror the supervisor's Start-DetachedStatusMonitor launch shape at
	// OsvayderUE-RestartSurvival.ps1:328-340 — same -Sta -NoProfile -Bypass -File
	// -StatePath argument set so the monitor window comes up exactly as it does
	// when launched by the supervisor.
	const FString PowerShellExe = GetRestartSurvivalPowerShellExecutablePath();
	const FString Params = FString::Printf(
		TEXT("-Sta -NoProfile -ExecutionPolicy Bypass -File \"%s\" -StatePath \"%s\""),
		*Agent2BannerState.MonitorScriptPath,
		*Agent2BannerState.StateFilePath);

	FProcHandle MonitorHandle = FPlatformProcess::CreateProc(
		*PowerShellExe,
		*Params,
		/*bLaunchDetached=*/true,
		/*bLaunchHidden=*/false,
		/*bLaunchReallyHidden=*/false,
		nullptr,
		0,
		nullptr,
		nullptr);
	if (!MonitorHandle.IsValid())
	{
		AddMessage(
			FString::Printf(TEXT("Show Monitor failed: CreateProc returned invalid handle for %s."), *PowerShellExe),
			false);
	}
	else
	{
		FPlatformProcess::CloseProc(MonitorHandle);
	}
	return FReply::Handled();
}

FReply SOsvayderEditorWidget::HandleAgent2BannerCancel()
{
	if (!Agent2BannerState.bVisible || Agent2BannerState.CancelRequestPath.IsEmpty())
	{
		AddMessage(TEXT("Cancel skipped: banner state is empty or cancel path is missing."), false);
		return FReply::Handled();
	}

	const FString Payload = BuildAgent2BannerCancelRequestPayload();
	if (!FFileHelper::SaveStringToFile(
		Payload,
		*Agent2BannerState.CancelRequestPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		AddMessage(
			FString::Printf(TEXT("Cancel failed: could not write %s."), *Agent2BannerState.CancelRequestPath),
			false);
	}
	else
	{
		AddMessage(
			TEXT("Cancel request written. Agent 2 should stop at its next safe boundary."),
			false);
	}
	return FReply::Handled();
}

void SOsvayderEditorWidget::TryStartPendingRestartSurvivalAfterResponse(const EOsvayderUEProviderBackend RequestBackend)
{
	if (bIsWaitingForResponse || RequestBackend != EOsvayderUEProviderBackend::CodexCli)
	{
		return;
	}

	FCodexCliRunner* CodexRunner = static_cast<FCodexCliRunner*>(FOsvayderCodeSubsystem::Get().GetActiveBackend());
	if (CodexRunner == nullptr || !FCodexCliRunner::ShouldUsePersistentConversationTransport())
	{
		return;
	}

	const FString ProviderSessionId = CodexRunner->GetActivePersistentThreadId();
	FString EffectiveProviderSessionId = ProviderSessionId;
	if (EffectiveProviderSessionId.IsEmpty())
	{
		FString IgnoredThreadStatePath;
		CodexRunner->ExportActiveThreadStateForRestartSurvival(IgnoredThreadStatePath, EffectiveProviderSessionId);
	}

	FOsvayderUERestartSurvivalPreparedRestoreRequest PreparedRequest;
	FString AutoStartError;
	if (!FOsvayderUERestartSurvivalManager::TryConsumePreparedRequestAutoStart(
		RequestBackend,
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
		EffectiveProviderSessionId,
		PreparedRequest,
		AutoStartError))
	{
		if (!AutoStartError.IsEmpty())
		{
			AddMessage(FString::Printf(TEXT("Restart survival handoff rejected: %s"), *AutoStartError), false);
		}
		return;
	}

	FClaudeRestartSurvivalStartOptions StartOptions;
	StartOptions.TaskIdOverride = PreparedRequest.TaskId;
	StartOptions.SessionIdOverride = PreparedRequest.SessionId;
	const FString TransitionNotice =
		FOsvayderUERestartSurvivalManager::BuildPreparedRequestClosedEditorTransitionNotice(PreparedRequest);
	if (!LastResponse.Equals(TransitionNotice, ESearchCase::CaseSensitive))
	{
		AddMessage(TransitionNotice, false);
	}

	FString StartError;
	if (!StartRestartSurvivalForCurrentTask(StartOptions, StartError))
	{
		AddMessage(FString::Printf(TEXT("Restart survival handoff failed: %s"), *StartError), false);
	}
}

void SOsvayderEditorWidget::ClearPendingClosedEditorBuildBlockerIntervention()
{
	PendingClosedEditorBuildBlockerIntervention = FPendingClosedEditorBuildBlockerIntervention();
}

FClaudeRestartSurvivalStartOptions SOsvayderEditorWidget::BuildArmOptionsForClosedEditorBuildBlocker()
{
	// 624 v3: request editor exit via FPlatformMisc::RequestExit on the 0.2 s
	// ticker inside StartRestartSurvivalForCurrentTask. The PS supervisor
	// (OsvayderUE-RestartSurvival.ps1) polls editor_process_id via
	// Get-Process and waits for the editor to exit — it does not kill the
	// editor itself. The 0.2 s ticker delay separates this request from the
	// synchronous CancelCurrentRequest that ExecuteSpawnBeforeTransportResetSequence
	// runs right after this helper returns, so there is no teardown race.
	//
	// bBypassWaitingForResponseGuard=true because this arm path runs mid-turn
	// (inside TryHandleClosedEditorBuildBlocker, before the Codex transport
	// reset that ends the turn). The ordinary bIsWaitingForResponse early-return
	// in StartRestartSurvivalForCurrentTask would otherwise reject us.
	//
	// Extracted into a pure static helper so the regression test
	// OsvayderUE.RestartSurvival.ArmEmissionPreservesEditorExitIntent can
	// observe bRequestEditorExit directly without a live widget instance. If
	// someone flips either flag to false, that test must fail.
	FClaudeRestartSurvivalStartOptions Options;
	Options.bBypassWaitingForResponseGuard = true;
	Options.bRequestEditorExit = true;
	return Options;
}

bool SOsvayderEditorWidget::StartRestartSurvivalForClosedEditorBuildBlocker(FString& OutError)
{
	// 624 fix: invoked from TryHandleClosedEditorBuildBlocker BEFORE the Codex
	// transport is reset. The prepared restore request was already persisted by
	// SavePreparedRestoreRequest earlier in the escalation body, so
	// StartRestartSurvivalForCurrentTask can consume it via
	// HasPreparedRestoreRequest()/TryConsumePreparedRequestAutoStart without
	// depending on the post-callback async path that used to drive this spawn.
	//
	// Options construction is factored into BuildArmOptionsForClosedEditorBuildBlocker
	// (624 v3) so the editor-exit-intent contract is directly testable.
	const FClaudeRestartSurvivalStartOptions Options = BuildArmOptionsForClosedEditorBuildBlocker();
	return StartRestartSurvivalForCurrentTask(Options, OutError);
}

void SOsvayderEditorWidget::TryHandleClosedEditorBuildBlocker(
	const FOsvayderStreamEvent& Event,
	const EOsvayderUEProviderBackend RequestBackend)
{
	if (PendingClosedEditorBuildBlockerIntervention.bActive)
	{
		return;
	}

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	if (!FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		Event.ToolName,
		Event.ToolInput, // 626 P2 Layer A: surface tool_input so detector can reject inspection commands.
		Event.ToolResultContent,
		Event.RawJson,
		Blocker))
	{
		return;
	}

	const FString ActiveRunId = FOsvayderUEAgentTraceLog::Get().GetActiveRunIdForBackend(RequestBackend);
	FAgentCanonExecution ActiveCanonExecution;
	FOsvayderUEAgentTraceLog::Get().TryGetActiveCanonExecutionForBackend(RequestBackend, ActiveCanonExecution);
	FOsvayderUERestartSurvivalOriginTaskContext EffectiveOriginTask;
	FAgentCanonExecution EffectiveCanonExecution;
	BuildEffectiveClosedEditorOriginTaskContext(
		RequestBackend,
		CurrentRequestPromptText,
		CurrentRequestImagePaths,
		ActiveRunId,
		ActiveCanonExecution,
		EffectiveOriginTask,
		EffectiveCanonExecution);
	TSharedPtr<FJsonObject> DetectionPayload = MakeShared<FJsonObject>();
	DetectionPayload->SetStringField(TEXT("tool_name"), Event.ToolName);
	DetectionPayload->SetStringField(TEXT("tool_call_id"), Event.ToolCallId);
	DetectionPayload->SetStringField(TEXT("classification"), Blocker.ClassificationLabel);
	DetectionPayload->SetStringField(TEXT("blocker_family"), Blocker.FamilyLabel);
	DetectionPayload->SetStringField(TEXT("escalation_reason"), Blocker.EscalationReason);
	DetectionPayload->SetStringField(TEXT("matched_evidence"), Blocker.MatchedEvidence);
	DetectionPayload->SetStringField(TEXT("current_run_id"), ActiveRunId);
	DetectionPayload->SetStringField(TEXT("originating_run_id"), EffectiveOriginTask.OriginatingRunId);
	DetectionPayload->SetStringField(TEXT("originating_task_mode"), EffectiveCanonExecution.TaskMode);
	DetectionPayload->SetStringField(TEXT("originating_requested_tool_family"), EffectiveCanonExecution.RequestedToolFamily);
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("closed_editor_build_blocker_detected"),
		RequestBackend,
		DetectionPayload,
		ActiveRunId);

	FPendingClosedEditorBuildBlockerIntervention PendingIntervention;
	PendingIntervention.bActive = true;
	PendingIntervention.BlockerFamily = Blocker.FamilyLabel;
	PendingIntervention.EscalationReason = Blocker.EscalationReason;
	const bool bUseRelayAgent = ShouldRouteClosedEditorBuildBlockerToRelay(EffectiveCanonExecution);
	const FOpenEditorBuildLockCloseSafetyInputs CloseSafetyInputs =
		BuildOpenEditorBuildLockCloseSafetyInputsFromEditorState();
	const FOpenEditorBuildLockCloseSafetyDecision CloseSafetyDecision =
		EvaluateOpenEditorBuildLockCloseSafety(CloseSafetyInputs);
	const bool bAutoCloseSafe = CloseSafetyDecision.bCanAutoClose;

	bool bEscalationAttempted = false;
	bool bRestartSurvivalArmed = false;
	FString EscalationOutcomeReason;

	if (!bAutoCloseSafe)
	{
		EscalationOutcomeReason = CloseSafetyDecision.BlockReasonCode;
		PendingIntervention.UserFacingMessage = CloseSafetyDecision.UserFacingMessage;
	}
	else if (RequestBackend == EOsvayderUEProviderBackend::CodexCli
		&& FCodexCliRunner::ShouldUsePersistentConversationTransport())
	{
		FCodexCliRunner* CodexRunner = static_cast<FCodexCliRunner*>(FOsvayderCodeSubsystem::Get().GetActiveBackend());
		const FString ProviderSessionId = CodexRunner != nullptr
			? CodexRunner->GetActivePersistentThreadId()
			: FString();
		if (CodexRunner != nullptr && !ProviderSessionId.IsEmpty())
		{
			bEscalationAttempted = true;
			FOsvayderUERestartSurvivalPreparedRestoreRequest PreparedRequest;
			FString TransitionNotice;
			FString EscalationError;
			if (FOsvayderUERestartSurvivalManager::TryPrepareClosedEditorBuildBlockerAutoStart(
				RequestBackend,
				FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
				ProviderSessionId,
				Blocker,
				EffectiveOriginTask,
				PreparedRequest,
				TransitionNotice,
				EscalationError))
			{
				bool bRelayPreparedRequestSaved = true;
				if (bUseRelayAgent)
				{
					PreparedRequest.bUseRelayAgent = true;
					FString SaveRelayPreparedRequestError;
					if (!FOsvayderUERestartSurvivalManager::SavePreparedRestoreRequest(
						PreparedRequest,
						SaveRelayPreparedRequestError))
					{
						EscalationError = SaveRelayPreparedRequestError;
						EscalationOutcomeReason = EscalationError;
						PendingIntervention.UserFacingMessage =
							BuildClosedEditorBuildBlockerFailFastMessage(Blocker, EscalationError);
						bRelayPreparedRequestSaved = false;
					}
				}

				if (bRelayPreparedRequestSaved)
				{
					bRestartSurvivalArmed = true;
					EscalationOutcomeReason = Blocker.EscalationReason;
					PendingIntervention.bRestartSurvivalArmed = true;
					PendingIntervention.UserFacingMessage = TransitionNotice;
				}
			}
			else
			{
				EscalationOutcomeReason = EscalationError;
				PendingIntervention.UserFacingMessage =
					BuildClosedEditorBuildBlockerFailFastMessage(Blocker, EscalationError);
			}
		}
		else
		{
			EscalationOutcomeReason = TEXT("Restart survival could not reuse the current provider session because the surviving Codex thread id was unavailable.");
			PendingIntervention.UserFacingMessage =
				BuildClosedEditorBuildBlockerFailFastMessage(Blocker, EscalationOutcomeReason);
		}
	}
	else
	{
		EscalationOutcomeReason = TEXT("Restart survival auto-escalation is available only on the ordinary Codex persistent runtime.");
		PendingIntervention.UserFacingMessage =
			BuildClosedEditorBuildBlockerFailFastMessage(Blocker, EscalationOutcomeReason);
	}

	TSharedPtr<FJsonObject> EscalationPayload = MakeShared<FJsonObject>();
	EscalationPayload->SetStringField(TEXT("tool_name"), Event.ToolName);
	EscalationPayload->SetStringField(TEXT("tool_call_id"), Event.ToolCallId);
	EscalationPayload->SetStringField(TEXT("classification"), Blocker.ClassificationLabel);
	EscalationPayload->SetStringField(TEXT("blocker_family"), Blocker.FamilyLabel);
	EscalationPayload->SetStringField(TEXT("current_run_id"), ActiveRunId);
	EscalationPayload->SetStringField(TEXT("originating_run_id"), EffectiveOriginTask.OriginatingRunId);
	EscalationPayload->SetStringField(TEXT("originating_task_mode"), EffectiveCanonExecution.TaskMode);
	EscalationPayload->SetStringField(TEXT("originating_requested_tool_family"), EffectiveCanonExecution.RequestedToolFamily);
	EscalationPayload->SetBoolField(TEXT("auto_close_safe"), bAutoCloseSafe);
	EscalationPayload->SetStringField(TEXT("auto_close_block_reason"), CloseSafetyDecision.BlockReasonCode);
	EscalationPayload->SetNumberField(TEXT("dirty_package_count"), CloseSafetyInputs.DirtyPackageCount);
	EscalationPayload->SetBoolField(TEXT("play_session_active"), CloseSafetyInputs.bPlaySessionActive);
	EscalationPayload->SetBoolField(TEXT("modal_window_active"), CloseSafetyInputs.bModalWindowActive);
	EscalationPayload->SetBoolField(TEXT("restart_survival_state_active"), CloseSafetyInputs.bRestartSurvivalStateActive);
	if (!CloseSafetyInputs.DirtyPackageSummary.IsEmpty())
	{
		EscalationPayload->SetStringField(TEXT("dirty_package_summary"), CloseSafetyInputs.DirtyPackageSummary);
	}
	if (!CloseSafetyInputs.ModalWindowTitle.IsEmpty())
	{
		EscalationPayload->SetStringField(TEXT("modal_window_title"), CloseSafetyInputs.ModalWindowTitle);
	}
	if (!CloseSafetyInputs.RestartSurvivalPhase.IsEmpty())
	{
		EscalationPayload->SetStringField(TEXT("restart_survival_phase"), CloseSafetyInputs.RestartSurvivalPhase);
	}
	EscalationPayload->SetBoolField(TEXT("relay_agent_requested"), bUseRelayAgent);
	EscalationPayload->SetBoolField(TEXT("escalation_attempted"), bEscalationAttempted);
	EscalationPayload->SetBoolField(TEXT("restart_survival_armed"), bRestartSurvivalArmed);
	EscalationPayload->SetStringField(TEXT("escalation_reason"), EscalationOutcomeReason);
	FString EscalationContinuationTaskId;
	if (bRestartSurvivalArmed)
	{
		FOsvayderUERestartSurvivalPreparedRestoreRequest PreparedRequest;
		FString PreparedRequestError;
		if (FOsvayderUERestartSurvivalManager::LoadPreparedRestoreRequest(PreparedRequest, PreparedRequestError))
		{
			EscalationContinuationTaskId = PreparedRequest.TaskId;
			EscalationPayload->SetStringField(TEXT("restart_survival_task_id"), PreparedRequest.TaskId);
			EscalationPayload->SetStringField(TEXT("restart_survival_session_id"), PreparedRequest.SessionId);
			EscalationPayload->SetStringField(TEXT("prepared_restore_request_id"), PreparedRequest.RequestId);
		}
	}
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("closed_editor_build_blocker_escalation"),
		RequestBackend,
		EscalationPayload,
		ActiveRunId);

	if (!EscalationOutcomeReason.IsEmpty())
	{
		PendingIntervention.EscalationReason = EscalationOutcomeReason;
	}
	if (bRestartSurvivalArmed)
	{
		MarkActivePlanAwaitingClosedEditorRelay(
			Blocker,
			EscalationContinuationTaskId,
			EscalationOutcomeReason,
			Event.ToolResultContent);
	}
	else if (bEscalationAttempted)
	{
		MarkActivePlanBlockedOnClosedEditorRelay(
			Blocker,
			EscalationOutcomeReason,
			Event.ToolResultContent);
	}
	PendingClosedEditorBuildBlockerIntervention = PendingIntervention;

	// 624 fix (spawn-before-reset): when the arm succeeded, spawn the detached
	// restart-survival owner (which writes restart_survival_state.json and
	// CreateProc's the PowerShell supervisor) BEFORE
	// FOsvayderCodeSubsystem::Get().CancelCurrentRequest() tears the Codex
	// persistent transport down. The ordering helper in
	// FOsvayderUERestartSurvivalManager guarantees the cancel is skipped if
	// the spawn-write fails, so no "arm without execute" state can leak.
	// If the arm was not set, there is nothing to spawn and we fall through to
	// the plain transport reset that existed before the 624 change.
	if (bRestartSurvivalArmed)
	{
		FString SpawnError;
		const bool bSequenceExecuted = FOsvayderUERestartSurvivalManager::ExecuteSpawnBeforeTransportResetSequence(
			[this, &SpawnError]() -> bool
			{
				return StartRestartSurvivalForClosedEditorBuildBlocker(SpawnError);
			},
			[]()
			{
				FOsvayderCodeSubsystem::Get().CancelCurrentRequest();
			});
		if (!bSequenceExecuted)
		{
			// Spawn failed before any transport reset fired. Surface the detail,
			// clear the armed flag on the pending intervention so the failure
			// branch of OnClaudeResponse reports the blocker outcome truthfully,
			// then fall through to the ordinary transport reset that the pre-624
			// code would have run unconditionally.
			PendingClosedEditorBuildBlockerIntervention.bRestartSurvivalArmed = false;
			if (!SpawnError.IsEmpty())
			{
				PendingClosedEditorBuildBlockerIntervention.UserFacingMessage =
					BuildClosedEditorBuildBlockerFailFastMessage(Blocker, SpawnError);
			}
			FOsvayderCodeSubsystem::Get().CancelCurrentRequest();
		}
	}
	else
	{
		FOsvayderCodeSubsystem::Get().CancelCurrentRequest();
	}
}

bool SOsvayderEditorWidget::DoesResponseClaimStatusFull(const FString& Response)
{
	if (Response.IsEmpty())
	{
		return false;
	}

	auto ContainsExplicitKeyedFullClaim = [](const FString& Lower, const FString& Key) -> bool
	{
		int32 KeyIdx = Lower.Find(Key, ESearchCase::CaseSensitive);
		while (KeyIdx != INDEX_NONE)
		{
			const bool bLeadingBoundary = KeyIdx == 0
				|| (!FChar::IsAlnum(Lower[KeyIdx - 1]) && Lower[KeyIdx - 1] != TEXT('_'));
			int32 Cursor = KeyIdx + Key.Len();
			const bool bTrailingBoundary = Cursor >= Lower.Len()
				|| (!FChar::IsAlnum(Lower[Cursor]) && Lower[Cursor] != TEXT('_'));

			if (bLeadingBoundary && bTrailingBoundary)
			{
				while (Cursor < Lower.Len() && (Lower[Cursor] == TEXT(' ') || Lower[Cursor] == TEXT('\t')))
				{
					++Cursor;
				}
				if (Cursor < Lower.Len() && (Lower[Cursor] == TEXT(':') || Lower[Cursor] == TEXT('=')))
				{
					++Cursor;
					while (Cursor < Lower.Len() && (Lower[Cursor] == TEXT(' ') || Lower[Cursor] == TEXT('\t')))
					{
						++Cursor;
					}
					const bool bWrappedInBackticks = Cursor < Lower.Len() && Lower[Cursor] == TEXT('`');
					if (bWrappedInBackticks)
					{
						++Cursor;
						while (Cursor < Lower.Len() && (Lower[Cursor] == TEXT(' ') || Lower[Cursor] == TEXT('\t')))
						{
							++Cursor;
						}
					}
					if (Cursor + 4 <= Lower.Len() && Lower.Mid(Cursor, 4) == TEXT("full"))
					{
						int32 ValueEnd = Cursor + 4;
						if (bWrappedInBackticks && ValueEnd < Lower.Len() && Lower[ValueEnd] == TEXT('`'))
						{
							++ValueEnd;
						}

						if (ValueEnd == Lower.Len())
						{
							return true;
						}

						const TCHAR NextChar = Lower[ValueEnd];
						if (!FChar::IsAlnum(NextChar) && NextChar != TEXT('_'))
						{
							return true;
						}
					}
				}
			}

			KeyIdx = Lower.Find(Key, ESearchCase::CaseSensitive, ESearchDir::FromStart, KeyIdx + 1);
		}

		return false;
	};

	auto ContainsMarkdownResultFullClaim = [](const FString& Lower) -> bool
	{
		static const FRegexPattern Pattern(TEXT("\\*\\*\\s*result\\s*\\*\\*[\\s`:_-]*full\\b"));
		FRegexMatcher Matcher(Pattern, Lower);
		return Matcher.FindNext();
	};

	const FString Lower = Response.ToLower();
	return ContainsExplicitKeyedFullClaim(Lower, TEXT("status"))
		|| ContainsExplicitKeyedFullClaim(Lower, TEXT("result"))
		|| ContainsMarkdownResultFullClaim(Lower);
}

namespace
{
	struct FVisualQaManifestValidationResult
	{
		bool bCandidateFound = false;
		bool bValid = false;
		FString ManifestPath;
		FString Verdict;
		FString PlanId;
		FString RunId;
		FString FailureCode;
		FString FailureDetail;
		TArray<FString> ActualScreenshotPaths;
	};

	bool IsVisualQaManifestRequiredForPlan(const FOsvayderUEActivePlan& Plan)
	{
		return Plan.bVisualProofRequired || Plan.bVisualQaManifestRequired;
	}

	FString TrimVisualQaManifestPathToken(FString Candidate)
	{
		Candidate.TrimStartAndEndInline();
		static const FString TrimChars = TEXT(" \t\r\n`\"'<>[]{}(),;");
		while (!Candidate.IsEmpty() && TrimChars.Contains(FString(1, &Candidate[0])))
		{
			Candidate.RightChopInline(1);
			Candidate.TrimStartInline();
		}
		while (!Candidate.IsEmpty() && TrimChars.Contains(FString(1, &Candidate[Candidate.Len() - 1])))
		{
			Candidate.LeftChopInline(1);
			Candidate.TrimEndInline();
		}

		const FString LowerCandidate = Candidate.ToLower();
		const int32 JsonIndex = LowerCandidate.Find(TEXT(".json"), ESearchCase::CaseSensitive);
		if (JsonIndex != INDEX_NONE)
		{
			Candidate = Candidate.Left(JsonIndex + 5);
		}
		return Candidate;
	}

	bool LooksLikeVisualQaManifestPath(const FString& Candidate)
	{
		const FString Lower = Candidate.ToLower();
		return Lower.Contains(TEXT(".json"))
			&& (Lower.Contains(TEXT("visual_qa"))
				|| Lower.Contains(TEXT("visual-qa"))
				|| Lower.Contains(TEXT("visualqamanifest"))
				|| Lower.Contains(TEXT("visual_qa_manifest")));
	}

	FString ResolveVisualQaManifestPath(const FString& Candidate)
	{
		FString Resolved = TrimVisualQaManifestPathToken(Candidate);
		FPaths::NormalizeFilename(Resolved);
		if (Resolved.IsEmpty())
		{
			return FString();
		}
		if (FPaths::FileExists(Resolved))
		{
			return Resolved;
		}
		if (FPaths::IsRelative(Resolved))
		{
			FString ProjectRelativePath = FPaths::Combine(FPaths::ProjectDir(), Resolved);
			FPaths::NormalizeFilename(ProjectRelativePath);
			if (FPaths::FileExists(ProjectRelativePath))
			{
				return ProjectRelativePath;
			}
		}
		return Resolved;
	}

	void AddVisualQaManifestCandidate(TArray<FString>& Candidates, const FString& Candidate)
	{
		const FString Resolved = ResolveVisualQaManifestPath(Candidate);
		if (!Resolved.IsEmpty())
		{
			Candidates.AddUnique(Resolved);
		}
	}

	void AddVisualQaManifestCandidatesAfterMarker(
		TArray<FString>& Candidates,
		const FString& Response,
		const FString& LowerResponse,
		const FString& Marker)
	{
		int32 MarkerIndex = LowerResponse.Find(Marker, ESearchCase::CaseSensitive);
		while (MarkerIndex != INDEX_NONE)
		{
			int32 CandidateStart = MarkerIndex + Marker.Len();
			while (CandidateStart < Response.Len()
				&& (FChar::IsWhitespace(Response[CandidateStart])
					|| Response[CandidateStart] == TEXT(':')
					|| Response[CandidateStart] == TEXT('=')
					|| Response[CandidateStart] == TEXT('`')
					|| Response[CandidateStart] == TEXT('"')
					|| Response[CandidateStart] == TEXT('\'')))
			{
				++CandidateStart;
			}

			const int32 JsonIndex = LowerResponse.Find(TEXT(".json"), ESearchCase::CaseSensitive, ESearchDir::FromStart, CandidateStart);
			if (JsonIndex != INDEX_NONE)
			{
				AddVisualQaManifestCandidate(Candidates, Response.Mid(CandidateStart, JsonIndex + 5 - CandidateStart));
			}
			MarkerIndex = LowerResponse.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, MarkerIndex + Marker.Len());
		}
	}

	TArray<FString> FindVisualQaManifestCandidates(const FOsvayderUEActivePlan& Plan, const FString& Response)
	{
		TArray<FString> Candidates;
		AddVisualQaManifestCandidate(Candidates, Plan.VisualQaManifestPath);
		if (LooksLikeVisualQaManifestPath(Plan.VisualProofArtifactPath))
		{
			AddVisualQaManifestCandidate(Candidates, Plan.VisualProofArtifactPath);
		}

		const FString LowerResponse = Response.ToLower();
		AddVisualQaManifestCandidatesAfterMarker(Candidates, Response, LowerResponse, TEXT("visual_qa_manifest"));
		AddVisualQaManifestCandidatesAfterMarker(Candidates, Response, LowerResponse, TEXT("visual qa manifest"));
		AddVisualQaManifestCandidatesAfterMarker(Candidates, Response, LowerResponse, TEXT("visual-qa manifest"));

		TArray<FString> Tokens;
		Response.ParseIntoArrayWS(Tokens);
		for (const FString& Token : Tokens)
		{
			if (LooksLikeVisualQaManifestPath(Token))
			{
				AddVisualQaManifestCandidate(Candidates, Token);
			}
		}
		return Candidates;
	}

	bool HasNonEmptyStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		FString Value;
		return Object.IsValid()
			&& Object->TryGetStringField(FieldName, Value)
			&& !Value.TrimStartAndEnd().IsEmpty();
	}

	bool HasArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(FieldName, ArrayValue) && ArrayValue != nullptr;
	}

	FVisualQaManifestValidationResult ValidateVisualQaManifestFile(const FString& ManifestPath)
	{
		FVisualQaManifestValidationResult Result;
		Result.bCandidateFound = !ManifestPath.IsEmpty();
		Result.ManifestPath = ManifestPath;
		if (ManifestPath.IsEmpty())
		{
			Result.FailureCode = TEXT("visual_qa_manifest_required_missing");
			Result.FailureDetail = TEXT("Visual-reference closeout requires a visual_qa_manifest.json path.");
			return Result;
		}
		if (!FPaths::FileExists(ManifestPath))
		{
			Result.FailureCode = TEXT("visual_qa_manifest_file_missing");
			Result.FailureDetail = FString::Printf(TEXT("Visual QA manifest file does not exist: %s"), *ManifestPath);
			return Result;
		}

		FString ManifestText;
		if (!FFileHelper::LoadFileToString(ManifestText, *ManifestPath))
		{
			Result.FailureCode = TEXT("visual_qa_manifest_unreadable");
			Result.FailureDetail = FString::Printf(TEXT("Visual QA manifest could not be read: %s"), *ManifestPath);
			return Result;
		}

		TSharedPtr<FJsonObject> ManifestObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestText);
		if (!FJsonSerializer::Deserialize(Reader, ManifestObject) || !ManifestObject.IsValid())
		{
			Result.FailureCode = TEXT("visual_qa_manifest_invalid_json");
			Result.FailureDetail = FString::Printf(TEXT("Visual QA manifest is not valid JSON: %s"), *ManifestPath);
			return Result;
		}

		FString ArtifactType;
		if (!ManifestObject->TryGetStringField(TEXT("artifact_type"), ArtifactType)
			|| !ArtifactType.Equals(TEXT("visual_qa_manifest"), ESearchCase::CaseSensitive))
		{
			Result.FailureCode = TEXT("visual_qa_manifest_invalid_type");
			Result.FailureDetail = TEXT("Visual QA manifest artifact_type must be visual_qa_manifest.");
			return Result;
		}

		if (!ManifestObject->HasField(TEXT("schema_version"))
			|| !HasNonEmptyStringField(ManifestObject, TEXT("run_id"))
			|| !HasNonEmptyStringField(ManifestObject, TEXT("plan_id"))
			|| !HasArrayField(ManifestObject, TEXT("reference_artifact_paths"))
			|| !HasArrayField(ManifestObject, TEXT("checklist"))
			|| !HasNonEmptyStringField(ManifestObject, TEXT("reviewer"))
			|| !HasNonEmptyStringField(ManifestObject, TEXT("summary"))
			|| !HasNonEmptyStringField(ManifestObject, TEXT("created_at_utc")))
		{
			Result.FailureCode = TEXT("visual_qa_manifest_missing_required_field");
			Result.FailureDetail = TEXT("Visual QA manifest is missing one or more required v1 fields.");
			return Result;
		}

		if (!HasNonEmptyStringField(ManifestObject, TEXT("target_surface"))
			&& !HasNonEmptyStringField(ManifestObject, TEXT("target_widget_or_level")))
		{
			Result.FailureCode = TEXT("visual_qa_manifest_missing_target");
			Result.FailureDetail = TEXT("Visual QA manifest requires target_surface or target_widget_or_level.");
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>* ActualScreenshotValues = nullptr;
		if (!ManifestObject->TryGetArrayField(TEXT("actual_screenshot_paths"), ActualScreenshotValues)
			|| ActualScreenshotValues == nullptr)
		{
			Result.FailureCode = TEXT("visual_qa_manifest_actual_screenshot_missing");
			Result.FailureDetail = TEXT("Visual QA manifest requires actual_screenshot_paths.");
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *ActualScreenshotValues)
		{
			FString ScreenshotPath;
			if (Value.IsValid() && Value->TryGetString(ScreenshotPath) && !ScreenshotPath.TrimStartAndEnd().IsEmpty())
			{
				Result.ActualScreenshotPaths.Add(ScreenshotPath);
			}
		}
		if (Result.ActualScreenshotPaths.Num() == 0)
		{
			Result.FailureCode = TEXT("visual_qa_manifest_actual_screenshot_missing");
			Result.FailureDetail = TEXT("Visual QA manifest must list at least one actual screenshot path.");
			return Result;
		}

		if (!ManifestObject->TryGetStringField(TEXT("verdict"), Result.Verdict) || Result.Verdict.TrimStartAndEnd().IsEmpty())
		{
			Result.FailureCode = TEXT("visual_qa_manifest_verdict_missing");
			Result.FailureDetail = TEXT("Visual QA manifest requires a verdict.");
			return Result;
		}
		ManifestObject->TryGetStringField(TEXT("plan_id"), Result.PlanId);
		ManifestObject->TryGetStringField(TEXT("run_id"), Result.RunId);
		Result.Verdict = Result.Verdict.TrimStartAndEnd();
		if (!Result.Verdict.Equals(TEXT("passed"), ESearchCase::IgnoreCase))
		{
			Result.FailureCode = Result.Verdict.Equals(TEXT("failed"), ESearchCase::IgnoreCase)
				? TEXT("visual_qa_manifest_failed")
				: TEXT("visual_qa_manifest_not_passed");
			Result.FailureDetail = FString::Printf(TEXT("Visual QA manifest verdict is %s, not passed."), *Result.Verdict);
			return Result;
		}

		Result.bValid = true;
		Result.FailureCode.Empty();
		Result.FailureDetail.Empty();
		return Result;
	}

	FVisualQaManifestValidationResult ValidateVisualQaManifestForPlan(
		const FOsvayderUEActivePlan& Plan,
		const FString& Response)
	{
		const TArray<FString> Candidates = FindVisualQaManifestCandidates(Plan, Response);
		FVisualQaManifestValidationResult FirstFailure;
		for (const FString& Candidate : Candidates)
		{
			const FVisualQaManifestValidationResult CandidateResult = ValidateVisualQaManifestFile(Candidate);
			if (CandidateResult.bValid)
			{
				if (!Plan.PlanId.IsEmpty()
					&& !CandidateResult.PlanId.IsEmpty()
					&& !CandidateResult.PlanId.Equals(Plan.PlanId, ESearchCase::CaseSensitive))
				{
					FVisualQaManifestValidationResult StaleResult = CandidateResult;
					StaleResult.bValid = false;
					StaleResult.FailureCode = TEXT("visual_qa_manifest_stale_plan_mismatch");
					StaleResult.FailureDetail = FString::Printf(
						TEXT("Visual QA manifest plan_id mismatch: expected %s but found %s in %s."),
						*Plan.PlanId,
						*CandidateResult.PlanId,
						*CandidateResult.ManifestPath);
					if (!FirstFailure.bCandidateFound)
					{
						FirstFailure = StaleResult;
					}
					continue;
				}
				return CandidateResult;
			}
			if (!FirstFailure.bCandidateFound)
			{
				FirstFailure = CandidateResult;
			}
		}

		if (FirstFailure.bCandidateFound)
		{
			return FirstFailure;
		}
		FirstFailure.FailureCode = TEXT("visual_qa_manifest_required_missing");
		FirstFailure.FailureDetail = TEXT("Visual-reference closeout requires a visual_qa_manifest.json with verdict=passed and actual_screenshot_paths; a bare screenshot path is not sufficient.");
		return FirstFailure;
	}

	void FailDecisionForVisualQaManifest(
		FOsvayderUEActivePlanCloseoutDecision& Decision,
		const FVisualQaManifestValidationResult& Validation,
		const bool bExplicitVisualBlocker)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		if (bExplicitVisualBlocker)
		{
			Decision.GateReasonCode = TEXT("visual_proof_blocked");
			Decision.BlockerFamily = TEXT("visual_proof_blocker_reported");
			Decision.BlockerDetail = TEXT("The final response or active plan reported a visual-proof blocker instead of a passed visual QA manifest.");
			return;
		}

		Decision.GateReasonCode = Validation.FailureCode.IsEmpty()
			? TEXT("visual_qa_manifest_required_missing")
			: Validation.FailureCode;
		if (Decision.GateReasonCode == TEXT("visual_qa_manifest_required_missing")
			|| Decision.GateReasonCode == TEXT("visual_qa_manifest_file_missing"))
		{
			Decision.BlockerFamily = TEXT("visual_qa_manifest_missing");
		}
		else
		{
			Decision.BlockerFamily = TEXT("visual_qa_manifest_invalid");
		}
		Decision.BlockerDetail = Validation.FailureDetail.IsEmpty()
			? TEXT("Visual-reference closeout requires a passed visual_qa_manifest.json with at least one actual_screenshot_paths entry.")
			: Validation.FailureDetail;
	}

	void ApplyVisualQaManifestEvidenceFromResponse(FOsvayderUEActivePlan& Plan, const FString& Response)
	{
		if (!IsVisualQaManifestRequiredForPlan(Plan))
		{
			return;
		}

		Plan.bVisualProofRequired = true;
		Plan.bVisualQaManifestRequired = true;
		const FVisualQaManifestValidationResult Validation = ValidateVisualQaManifestForPlan(Plan, Response);
		if (Validation.bValid)
		{
			Plan.VisualQaManifestPath = Validation.ManifestPath;
			Plan.VisualQaManifestVerdict = TEXT("passed");
			Plan.VisualProofStatus = TEXT("visual_qa_passed");
			Plan.VisualProofArtifactPath = Validation.ManifestPath;
			Plan.VisualProofBlocker.Empty();
			return;
		}

		if (Validation.bCandidateFound)
		{
			Plan.VisualQaManifestPath = Validation.ManifestPath;
			Plan.VisualQaManifestVerdict = Validation.Verdict;
			Plan.VisualProofStatus = TEXT("visual_qa_manifest_invalid");
			Plan.VisualProofBlocker = Validation.FailureDetail;
			return;
		}

		if (SOsvayderEditorWidget::DoesFinalResponseDeclareFailureOrBlocker(Response))
		{
			Plan.VisualProofStatus = TEXT("blocked");
			Plan.VisualProofBlocker = NormalizeSingleLineText(Response.Left(400));
		}
		else if (SOsvayderEditorWidget::DoesFinalResponseProvideVisualProof(Response))
		{
			Plan.VisualProofStatus = TEXT("visual_qa_manifest_missing");
			Plan.VisualProofArtifactPath = NormalizeSingleLineText(Response.Left(400));
		}
		else
		{
			Plan.VisualProofStatus = TEXT("missing");
		}
	}
}

bool SOsvayderEditorWidget::DoesPromptRequireVisualProof(
	const FString& Prompt,
	const TArray<FString>& ImagePaths)
{
	if (ImagePaths.Num() > 0)
	{
		return true;
	}

	const FString Lower = Prompt.ToLower();
	return Lower.Contains(TEXT("как на картин"))
		|| Lower.Contains(TEXT("как на изображ"))
		|| Lower.Contains(TEXT("attached image"))
		|| Lower.Contains(TEXT("attached screenshot"))
		|| Lower.Contains(TEXT("reference image"))
		|| Lower.Contains(TEXT("visual reference"))
		|| Lower.Contains(TEXT("визуальн"))
		|| Lower.Contains(TEXT("скриншот"))
		|| Lower.Contains(TEXT("screenshot"));
}

bool SOsvayderEditorWidget::DoesFinalResponseProvideVisualProof(const FString& Response)
{
	const FString Lower = Response.ToLower();
	return Lower.Contains(TEXT(".png"))
		|| Lower.Contains(TEXT(".jpg"))
		|| Lower.Contains(TEXT(".jpeg"))
		|| Lower.Contains(TEXT(".bmp"))
		|| Lower.Contains(TEXT(".webp"))
		|| Lower.Contains(TEXT(".gif"))
		|| Lower.Contains(TEXT("screenshot:"))
		|| Lower.Contains(TEXT("screenshot path"))
		|| Lower.Contains(TEXT("artifact path"))
		|| Lower.Contains(TEXT("артефакт:"))
		|| Lower.Contains(TEXT("скриншот:"));
}

bool SOsvayderEditorWidget::DoesFinalResponseDeclareFailureOrBlocker(const FString& Response)
{
	const FString Lower = Response.ToLower();
	return Lower.Contains(TEXT("result status: fail"))
		|| Lower.Contains(TEXT("result: fail"))
		|| Lower.Contains(TEXT("status: fail"))
		|| Lower.Contains(TEXT("status: failed"))
		|| Lower.Contains(TEXT("not achieved"))
		|| Lower.Contains(TEXT("not_achieved"))
		|| Lower.Contains(TEXT("blocked by"))
		|| Lower.Contains(TEXT("blocker:"))
		|| Lower.Contains(TEXT("compile failed"))
		|| Lower.Contains(TEXT("compilation failed"))
		|| Lower.Contains(TEXT("build failed"))
		|| Lower.Contains(TEXT("runtime proof failed"))
		|| Lower.Contains(TEXT("runtime verification failed"))
		|| Lower.Contains(TEXT("pie failed"))
		|| Lower.Contains(TEXT("visual proof failed"))
		|| Lower.Contains(TEXT("automation failed"))
		|| Lower.Contains(TEXT("test failed"))
		|| Lower.Contains(TEXT("tests failed"))
		|| Lower.Contains(TEXT("failed because"))
		|| Lower.Contains(TEXT("cannot complete"))
		|| Lower.Contains(TEXT("can't complete"))
		|| Lower.Contains(TEXT("not completed"))
		|| Lower.Contains(TEXT("not implemented"))
		|| Lower.Contains(TEXT("implementation incomplete"))
		|| Lower.Contains(TEXT("did not complete"))
		|| Lower.Contains(TEXT("didn't complete"))
		|| Lower.Contains(TEXT("incomplete implementation"))
		|| Lower.Contains(TEXT("сборка не прошла"))
		|| Lower.Contains(TEXT("компиляция не удалась"))
		|| Lower.Contains(TEXT("механика не реализована"))
		|| Lower.Contains(TEXT("механика не выполнена"))
		|| Lower.Contains(TEXT("задача не выполнена"))
		|| Lower.Contains(TEXT("не удалось"))
		|| Lower.Contains(TEXT("заблок"));
}

namespace
{
	bool ContainsAnyCloseoutToken(const FString& Lower, const TArray<FString>& Tokens)
	{
		for (const FString& Token : Tokens)
		{
			if (Lower.Contains(Token))
			{
				return true;
			}
		}
		return false;
	}

	bool DoesFinalResponseDeclareHardProofFailure(const FString& Lower)
	{
		return ContainsAnyCloseoutToken(
			Lower,
			{
				TEXT("compile failed"),
				TEXT("compilation failed"),
				TEXT("build failed"),
				TEXT("ubt failed"),
				TEXT("runtime proof failed"),
				TEXT("runtime verification failed"),
				TEXT("pie failed"),
				TEXT("visual proof failed"),
				TEXT("automation failed"),
				TEXT("test failed"),
				TEXT("tests failed"),
				TEXT("failed because"),
				TEXT("not completed"),
				TEXT("not implemented"),
				TEXT("implementation incomplete"),
				TEXT("did not complete"),
				TEXT("didn't complete"),
				TEXT("incomplete implementation"),
				TEXT("сборка не прошла"),
				TEXT("компиляция не удалась"),
				TEXT("механика не реализована"),
				TEXT("механика не выполнена"),
				TEXT("задача не выполнена"),
				TEXT("не удалось")
			});
	}

	bool DoesFinalResponseDeclarePositiveSuccessOrPartial(const FString& Lower)
	{
		return ContainsAnyCloseoutToken(
			Lower,
			{
				TEXT("status: partial"),
				TEXT("status = partial"),
				TEXT("result: partial"),
				TEXT("result = partial"),
				TEXT("achieved_partially"),
				TEXT("implementation succeeded"),
				TEXT("build succeeded"),
				TEXT("build passed"),
				TEXT("compile succeeded"),
				TEXT("compiled successfully"),
				TEXT("статус: частично"),
				TEXT("статус = частично"),
				TEXT("результат: частично"),
				TEXT("результат = частично"),
				TEXT("реализация выполнена"),
				TEXT("сборка прошла"),
				TEXT("сборка успешна"),
				TEXT("компиляция прошла"),
				TEXT("компиляция успешна")
			});
	}

	bool DoesFinalResponseDeclareManualRuntimeVisualProofGap(const FString& Response)
	{
		const FString Lower = Response.ToLower();
		if (Lower.IsEmpty() || DoesFinalResponseDeclareHardProofFailure(Lower))
		{
			return false;
		}

		if (!DoesFinalResponseDeclarePositiveSuccessOrPartial(Lower))
		{
			return false;
		}

		if (ContainsAnyCloseoutToken(
			Lower,
			{
				TEXT("manual verification required"),
				TEXT("manual verification is required"),
				TEXT("manual proof required"),
				TEXT("manual proof is required"),
				TEXT("manual runtime required"),
				TEXT("manual pie required"),
				TEXT("manual confirmation required"),
				TEXT("manual confirmation is required"),
				TEXT("requires manual"),
				TEXT("needs manual"),
				TEXT("blocked_on_manual"),
				TEXT("ручной проверки"),
				TEXT("нужна ручная"),
				TEXT("требуется ручная"),
				TEXT("проверить вручную"),
				TEXT("оставлена для ручн"),
				TEXT("оставлено для ручн"),
				TEXT("оставлен для ручн"),
				TEXT("left for manual")
			}))
		{
			return true;
		}

		const bool bMentionsRuntimeVisualSurface =
			Lower.Contains(TEXT("runtime"))
			|| Lower.Contains(TEXT("pie"))
			|| Lower.Contains(TEXT("visual"))
			|| Lower.Contains(TEXT("gameplay"))
			|| Lower.Contains(TEXT("viewport"))
			|| Lower.Contains(TEXT("визуаль"))
			|| Lower.Contains(TEXT("игров"))
			|| Lower.Contains(TEXT("вьюпорт"));
		const bool bMentionsMissingVerification =
			Lower.Contains(TEXT("not verified"))
			|| Lower.Contains(TEXT("not manually verified"))
			|| Lower.Contains(TEXT("not performed"))
			|| Lower.Contains(TEXT("not run"))
			|| Lower.Contains(TEXT("not executed"))
			|| Lower.Contains(TEXT("not tested"))
			|| Lower.Contains(TEXT("wasn't verified"))
			|| Lower.Contains(TEXT("was not verified"))
			|| Lower.Contains(TEXT("не выполнялась"))
			|| Lower.Contains(TEXT("не выполнялся"))
			|| Lower.Contains(TEXT("не выполнена"))
			|| Lower.Contains(TEXT("не проведена"))
			|| Lower.Contains(TEXT("не проводилась"))
			|| Lower.Contains(TEXT("не проверял"))
			|| Lower.Contains(TEXT("не протест"));
		return bMentionsRuntimeVisualSurface && bMentionsMissingVerification;
	}

	void ApplyManualRuntimeVisualProofGapDecision(
		FOsvayderUEActivePlanCloseoutDecision& Decision,
		const bool bHasCurrentClosedEditorProof)
	{
		Decision.ResultStatus = TEXT("achieved_partially");
		Decision.GateReasonCode = TEXT("gameplay_runtime_visual_proof_manual_required");
		Decision.BlockerFamily = TEXT("manual_verification_required");
		Decision.BlockerDetail = bHasCurrentClosedEditorProof
			? TEXT("Current closed-editor/build proof succeeded, but the final assistant text truthfully left PIE/runtime/visual gameplay verification for manual confirmation.")
			: TEXT("The final assistant text truthfully declared a partial result because PIE/runtime/visual gameplay verification was left for manual confirmation.");
	}
}

FOsvayderUEActivePlanCloseoutDecision SOsvayderEditorWidget::ApplyActivePlanCloseoutSafetyGates(
	const FOsvayderUEActivePlan& Plan,
	const FString& Response,
	const bool bResponseSuccess,
	const bool bHasCurrentClosedEditorProof,
	FOsvayderUEActivePlanCloseoutDecision Decision)
{
	if (!bResponseSuccess)
	{
		return Decision;
	}
	Decision.bCurrentClosedEditorProofObserved = bHasCurrentClosedEditorProof;

	const bool bFallbackPlan =
		Plan.ReviewerPlanReference.Equals(TEXT("post_reattach_state_json_fallback_plan_v1"), ESearchCase::CaseSensitive)
		|| Plan.PlanId.StartsWith(TEXT("plan_fallback_"), ESearchCase::CaseSensitive);
	if (bFallbackPlan && !bHasCurrentClosedEditorProof)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("fallback_closeout_proof_missing");
		Decision.BlockerFamily = TEXT("restart_survival_typed_result_missing");
		Decision.BlockerDetail = TEXT("Fallback/recovery plans cannot close as achieved without a current typed closed-editor result or closeout proof.");
		return Decision;
	}

	if (IsVisualQaManifestRequiredForPlan(Plan))
	{
		const FVisualQaManifestValidationResult Validation = ValidateVisualQaManifestForPlan(Plan, Response);
		if (!Validation.bValid)
		{
			const bool bExplicitVisualBlocker = DoesFinalResponseDeclareFailureOrBlocker(Response)
				|| Plan.VisualProofStatus.Equals(TEXT("blocked"), ESearchCase::IgnoreCase)
				|| !Plan.VisualProofBlocker.TrimStartAndEnd().IsEmpty();
			FailDecisionForVisualQaManifest(Decision, Validation, bExplicitVisualBlocker);
			return Decision;
		}
	}

	if (DoesFinalResponseDeclareFailureOrBlocker(Response))
	{
		if (Response.Contains(TEXT("manual_asset_dependency_blocker"), ESearchCase::IgnoreCase))
		{
			Decision.PlanStatus = TEXT("failed");
			Decision.ResultStatus = TEXT("not_achieved");
			Decision.GateReasonCode = TEXT("manual_asset_dependency_blocker");
			Decision.BlockerFamily = TEXT("manual_asset_dependency_blocker");
			Decision.BlockerDetail = TEXT("The final assistant text reported an external/local asset dependency blocker instead of a generic failure.");
			return Decision;
		}
		if (Response.Contains(TEXT("blocked_on_tool_surface"), ESearchCase::IgnoreCase))
		{
			Decision.PlanStatus = TEXT("failed");
			Decision.ResultStatus = TEXT("not_achieved");
			Decision.GateReasonCode = TEXT("blocked_on_tool_surface");
			Decision.BlockerFamily = TEXT("blocked_on_tool_surface");
			Decision.BlockerDetail = TEXT("The final assistant text reported missing runtime/capture tool surface instead of a generic failure.");
			return Decision;
		}
		if (Response.Contains(TEXT("mechanic_input_conflict_unresolved"), ESearchCase::IgnoreCase))
		{
			Decision.PlanStatus = TEXT("failed");
			Decision.ResultStatus = TEXT("not_achieved");
			Decision.GateReasonCode = TEXT("mechanic_input_conflict_unresolved");
			Decision.BlockerFamily = TEXT("mechanic_input_conflict_unresolved");
			Decision.BlockerDetail = TEXT("The final assistant text reported an unresolved mechanic/input ownership conflict instead of a generic failure.");
			return Decision;
		}
		if (Decision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive)
			&& DoesFinalResponseDeclareManualRuntimeVisualProofGap(Response))
		{
			ApplyManualRuntimeVisualProofGapDecision(Decision, bHasCurrentClosedEditorProof);
			return Decision;
		}
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("final_response_declares_failure");
		Decision.BlockerFamily = TEXT("assistant_reported_failure");
		Decision.BlockerDetail = TEXT("Provider transport succeeded, but the final assistant text explicitly reported failure or a blocker.");
	}
	else if (Decision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive)
		&& DoesFinalResponseDeclareManualRuntimeVisualProofGap(Response))
	{
		ApplyManualRuntimeVisualProofGapDecision(Decision, bHasCurrentClosedEditorProof);
	}

	return Decision;
}

namespace
{
	void AddCloseoutFactId(FOsvayderUECloseoutFactSnapshot& Facts, const FString& FactId)
	{
		Facts.AddFactId(FactId);
	}

	void MergeCloseoutCount(int32& Target, const int32 Candidate)
	{
		if (Candidate == INDEX_NONE)
		{
			return;
		}
		if (Target == INDEX_NONE || Candidate > Target)
		{
			Target = Candidate;
		}
	}

	int32 CountCloseoutNeedleOccurrences(const FString& Haystack, const FString& Needle)
	{
		if (Haystack.IsEmpty() || Needle.IsEmpty())
		{
			return 0;
		}

		int32 Count = 0;
		int32 SearchIndex = 0;
		while (SearchIndex < Haystack.Len())
		{
			const int32 MatchIndex = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchIndex);
			if (MatchIndex == INDEX_NONE)
			{
				break;
			}
			++Count;
			SearchIndex = MatchIndex + Needle.Len();
		}
		return Count;
	}

	TArray<TSharedPtr<FJsonValue>> MakeCloseoutStringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		Array.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}

	TSharedPtr<FJsonObject> MakeCloseoutStringMapJson(const TMap<FString, FString>& Values)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Values)
		{
			Object->SetStringField(Pair.Key, Pair.Value);
		}
		return Object;
	}

	TArray<FString> ReadCloseoutStringArray(const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		TArray<FString> Result;
		Result.Reserve(Values.Num());
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString Text;
			if (Value->TryGetString(Text))
			{
				Result.Add(Text);
			}
		}
		return Result;
	}

	void AddAnimationWorkflowFactId(FOsvayderUECloseoutFactSnapshot& Facts, const FString& FactId)
	{
		if (!FactId.TrimStartAndEnd().IsEmpty())
		{
			Facts.AnimationWorkflowFactIds.AddUnique(FactId);
			Facts.AddFactId(FactId);
		}
	}

	bool ContainsExplicitLocalFilesystemPath(const FString& Text)
	{
		for (int32 Index = 0; Index + 2 < Text.Len(); ++Index)
		{
			if (FChar::IsAlpha(Text[Index])
				&& Text[Index + 1] == TCHAR(':')
				&& (Text[Index + 2] == TCHAR('\\') || Text[Index + 2] == TCHAR('/')))
			{
				return true;
			}
		}
		return Text.Contains(TEXT("\\\\"), ESearchCase::CaseSensitive);
	}

	bool ContainsAnyCloseoutSubstring(const FString& Text, std::initializer_list<const TCHAR*> Needles)
	{
		for (const TCHAR* Needle : Needles)
		{
			if (Needle != nullptr && Text.Contains(Needle, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool HasAnimationOrMechanicIntent(const FString& Text)
	{
		return ContainsAnyCloseoutSubstring(
			Text,
			{
				TEXT("animation"),
				TEXT("animations"),
				TEXT("animbp"),
				TEXT("montage"),
				TEXT("retarget"),
				TEXT("parkour"),
				TEXT("wallrun"),
				TEXT("wall run"),
				TEXT("wall climb"),
				TEXT("ledge"),
				TEXT("mantle"),
				TEXT("climb"),
				TEXT("механик"),
				TEXT("анимац"),
				TEXT("паркур"),
				TEXT("уступ"),
				TEXT("забег"),
				TEXT("забраться")
			});
	}

	bool PromptRequiresLocalAnimationPackIntake(const FString& Prompt)
	{
		return ContainsExplicitLocalFilesystemPath(Prompt) && HasAnimationOrMechanicIntent(Prompt);
	}

	bool TextIndicatesToolFailureOrBlocker(const FString& Text)
	{
		FString CompactText = Text;
		CompactText.ReplaceInline(TEXT(" "), TEXT(""));
		CompactText.ReplaceInline(TEXT("\t"), TEXT(""));
		CompactText.ReplaceInline(TEXT("\r"), TEXT(""));
		CompactText.ReplaceInline(TEXT("\n"), TEXT(""));
		return ContainsAnyCloseoutSubstring(
			CompactText,
			{
				TEXT("\"success\":false"),
				TEXT("\"status\":\"failed\""),
				TEXT("\"status\":\"error\""),
				TEXT("\"status\":\"blocked\""),
				TEXT("\"ok\":false"),
				TEXT("retarget_api_unavailable"),
				TEXT("retargeter_asset_missing"),
				TEXT("tool_surface_unavailable"),
				TEXT("manual_asset_dependency_blocker")
			});
	}

	bool TextIndicatesSuccessfulToolResult(const FString& Text)
	{
		if (TextIndicatesToolFailureOrBlocker(Text))
		{
			return false;
		}
		FString CompactText = Text;
		CompactText.ReplaceInline(TEXT(" "), TEXT(""));
		CompactText.ReplaceInline(TEXT("\t"), TEXT(""));
		CompactText.ReplaceInline(TEXT("\r"), TEXT(""));
		CompactText.ReplaceInline(TEXT("\n"), TEXT(""));
		return ContainsAnyCloseoutSubstring(
			CompactText,
			{
				TEXT("\"success\":true"),
				TEXT("\"ok\":true"),
				TEXT("\"status\":\"success\""),
				TEXT("\"status\":\"succeeded\""),
				TEXT("\"status\":\"completed\""),
				TEXT("ready_to_claim_retarget_success\":true"),
				TEXT("preflight_ready_for_manual_verification\":true")
			});
	}

	bool IsAnimationPreflightEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT("animation_preflight"), ESearchCase::IgnoreCase)
			|| EvidenceText.Contains(TEXT("Animation preflight for"), ESearchCase::IgnoreCase);
	}

	bool IsAnimationPreflightSkeletonMismatchEvidence(const FString& EvidenceText)
	{
		return IsAnimationPreflightEvidence(EvidenceText)
			&& ContainsAnyCloseoutSubstring(
				EvidenceText,
				{
					TEXT("skeleton_mismatch"),
					TEXT("retarget_required_not_implemented"),
					TEXT("proof=manual_blocker")
				});
	}

	bool IsCompatibleAnimationPreflightEvidence(const FString& EvidenceText)
	{
		FString CompactText = EvidenceText;
		CompactText.ReplaceInline(TEXT(" "), TEXT(""));
		CompactText.ReplaceInline(TEXT("\t"), TEXT(""));
		CompactText.ReplaceInline(TEXT("\r"), TEXT(""));
		CompactText.ReplaceInline(TEXT("\n"), TEXT(""));
		return IsAnimationPreflightEvidence(EvidenceText)
			&& !IsAnimationPreflightSkeletonMismatchEvidence(EvidenceText)
			&& ContainsAnyCloseoutSubstring(
				CompactText,
				{
					TEXT("preflight_ready_for_manual_verification\":true"),
					TEXT("\"blocker_codes\":[]"),
					TEXT("\"compatible_roles\""),
					TEXT("\"proof_classification\":\"gameplay_code_proof\""),
					TEXT("proof=gameplay_code_proof"),
					TEXT("0missingroles,0unsatisfiedroles")
				});
	}

	bool IsLocalAnimationPackIntakeSuccessEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT("local_animation_pack_intake"), ESearchCase::IgnoreCase)
			&& TextIndicatesSuccessfulToolResult(EvidenceText);
	}

	bool IsAnimationRetargetFixupSuccessEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT("animation_retarget_fixup"), ESearchCase::IgnoreCase)
			&& TextIndicatesSuccessfulToolResult(EvidenceText)
			&& ContainsAnyCloseoutSubstring(
				EvidenceText,
				{
					TEXT("ready_to_claim_retarget_success"),
					TEXT("retargeted"),
					TEXT("generated_asset"),
					TEXT("generated_assets"),
					TEXT("destination_plan")
				});
	}

	FString NormalizeAnimationLineageRole(const FString& Role)
	{
		FString Normalized = Role.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("-"), TEXT("_"));
		Normalized.ReplaceInline(TEXT(" "), TEXT("_"));
		return Normalized;
	}

	FString NormalizeAnimationLineagePath(const FString& Path)
	{
		FString Normalized = Path.TrimStartAndEnd();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Normalized;
	}

	bool TryParseCloseoutJsonObjectFromText(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
	{
		FString Candidate = Text.TrimStartAndEnd();
		const int32 FirstBrace = Candidate.Find(TEXT("{"));
		int32 LastBrace = INDEX_NONE;
		if (!Candidate.FindLastChar(TEXT('}'), LastBrace) || FirstBrace == INDEX_NONE || LastBrace <= FirstBrace)
		{
			return false;
		}
		Candidate = Candidate.Mid(FirstBrace, LastBrace - FirstBrace + 1);
		Candidate.ReplaceInline(TEXT(" | "), TEXT("\n"));
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Candidate);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	void CollectCloseoutJsonObjectsFromText(
		const FString& Text,
		TArray<TSharedPtr<FJsonObject>>& OutObjects)
	{
		bool bInString = false;
		bool bEscaped = false;
		int32 Depth = 0;
		int32 ObjectStart = INDEX_NONE;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Ch = Text[Index];
			if (bInString)
			{
				if (bEscaped)
				{
					bEscaped = false;
				}
				else if (Ch == TEXT('\\'))
				{
					bEscaped = true;
				}
				else if (Ch == TEXT('"'))
				{
					bInString = false;
				}
				continue;
			}
			if (Ch == TEXT('"'))
			{
				bInString = true;
				continue;
			}
			if (Ch == TEXT('{'))
			{
				if (Depth == 0)
				{
					ObjectStart = Index;
				}
				++Depth;
			}
			else if (Ch == TEXT('}') && Depth > 0)
			{
				--Depth;
				if (Depth == 0 && ObjectStart != INDEX_NONE)
				{
					TSharedPtr<FJsonObject> Object;
					FString Candidate = Text.Mid(ObjectStart, Index - ObjectStart + 1);
					Candidate.ReplaceInline(TEXT(" | "), TEXT("\n"));
					const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Candidate);
					if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
					{
						OutObjects.Add(Object);
					}
					ObjectStart = INDEX_NONE;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> FindNestedCloseoutResultObject(const TSharedPtr<FJsonObject>& Object)
	{
		static const TCHAR* CandidateFields[] = {
			TEXT("data"),
			TEXT("result"),
			TEXT("tool_result"),
			TEXT("payload")
		};
		for (const TCHAR* FieldName : CandidateFields)
		{
			const TSharedPtr<FJsonObject>* NestedObject = nullptr;
			if (Object->TryGetObjectField(FieldName, NestedObject) && NestedObject && (*NestedObject).IsValid())
			{
				FString ResultType;
				FString Operation;
				if ((*NestedObject)->TryGetStringField(TEXT("result_type"), ResultType)
					|| (*NestedObject)->TryGetStringField(TEXT("operation"), Operation))
				{
					return *NestedObject;
				}
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> ParseAnimationWorkflowEvidenceObject(const FString& EvidenceText)
	{
		TArray<TSharedPtr<FJsonObject>> Objects;
		CollectCloseoutJsonObjectsFromText(EvidenceText, Objects);
		if (Objects.Num() == 0)
		{
			TSharedPtr<FJsonObject> RootObject;
			if (TryParseCloseoutJsonObjectFromText(EvidenceText, RootObject) && RootObject.IsValid())
			{
				Objects.Add(RootObject);
			}
		}
		TSharedPtr<FJsonObject> OperationCandidate;
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			if (!Object.IsValid())
			{
				continue;
			}
			FString ToolResult;
			if (Object->TryGetStringField(TEXT("tool_result"), ToolResult))
			{
				if (const TSharedPtr<FJsonObject> ToolResultObject = ParseAnimationWorkflowEvidenceObject(ToolResult))
				{
					return ToolResultObject;
				}
			}
			if (const TSharedPtr<FJsonObject> NestedObject = FindNestedCloseoutResultObject(Object))
			{
				FString ResultType;
				if (NestedObject->TryGetStringField(TEXT("result_type"), ResultType))
				{
					return NestedObject;
				}
				if (!OperationCandidate.IsValid())
				{
					OperationCandidate = NestedObject;
				}
			}
			FString ResultType;
			if (Object->TryGetStringField(TEXT("result_type"), ResultType))
			{
				return Object;
			}
			FString Operation;
			if (!OperationCandidate.IsValid() && Object->TryGetStringField(TEXT("operation"), Operation))
			{
				OperationCandidate = Object;
			}
		}
		return OperationCandidate;
	}

	bool TryGetCloseoutStringAny(
		const TSharedPtr<FJsonObject>& Object,
		const TArray<FString>& FieldNames,
		FString& OutValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const FString& FieldName : FieldNames)
		{
			if (Object->TryGetStringField(FieldName, OutValue))
			{
				OutValue = OutValue.TrimStartAndEnd();
				if (!OutValue.IsEmpty())
				{
					return true;
				}
			}
		}
		return false;
	}

	void AddNormalizedUnique(TArray<FString>& Values, const FString& Value)
	{
		const FString Normalized = NormalizeAnimationLineageRole(Value);
		if (!Normalized.IsEmpty())
		{
			Values.AddUnique(Normalized);
		}
	}

	bool TextIndicatesLineageMismatchStatus(const FString& Text)
	{
		return ContainsAnyCloseoutSubstring(
			Text,
			{
				TEXT("skeleton_mismatch"),
				TEXT("retarget_required_not_implemented"),
				TEXT("unsatisfied"),
				TEXT("missing")
			});
	}

	bool TextIndicatesLineageCompatibleStatus(const FString& Text)
	{
		return ContainsAnyCloseoutSubstring(
			Text,
			{
				TEXT("compatible"),
				TEXT("satisfied"),
				TEXT("ready")
			});
	}

	void ResetAnimationLineageAfterLatestMismatch(FOsvayderUECloseoutFactSnapshot& Facts)
	{
		Facts.bAnimationLineageRoleEvidencePresent = false;
		Facts.bAnimationLineagePathEvidencePresent = false;
		Facts.bAnimationLineagePathCheckApplied = false;
		Facts.RequiredAnimationRoles.Reset();
		Facts.CompatiblePostRetargetAnimationRoles.Reset();
		Facts.MissingAnimationLineageRoles.Reset();
		Facts.AnimationLineagePathMismatchRoles.Reset();
		Facts.GeneratedRetargetDestinationPaths.Reset();
		Facts.RequiredAnimationRoleSourcePaths.Reset();
		Facts.RetargetGeneratedDestinationsBySource.Reset();
		Facts.RetargetGeneratedDestinationsByRole.Reset();
		Facts.CompatibleAnimationRolePaths.Reset();
	}

	void RefreshAnimationLineageStatus(FOsvayderUECloseoutFactSnapshot& Facts)
	{
		Facts.MissingAnimationLineageRoles.Reset();
		Facts.AnimationLineagePathMismatchRoles.Reset();
		Facts.bAnimationLineageRoleEvidencePresent = Facts.RequiredAnimationRoles.Num() > 0;

		for (const FString& RequiredRole : Facts.RequiredAnimationRoles)
		{
			if (!Facts.CompatiblePostRetargetAnimationRoles.Contains(RequiredRole))
			{
				Facts.MissingAnimationLineageRoles.AddUnique(RequiredRole);
			}
		}

		Facts.bAnimationLineagePathCheckApplied = false;
		for (const FString& RequiredRole : Facts.RequiredAnimationRoles)
		{
			const FString* SourcePath = Facts.RequiredAnimationRoleSourcePaths.Find(RequiredRole);
			const FString* CompatiblePath = Facts.CompatibleAnimationRolePaths.Find(RequiredRole);
			if (!SourcePath || !CompatiblePath)
			{
				continue;
			}
			const FString* DestinationPath = Facts.RetargetGeneratedDestinationsBySource.Find(*SourcePath);
			if (!DestinationPath)
			{
				DestinationPath = Facts.RetargetGeneratedDestinationsByRole.Find(RequiredRole);
			}
			if (!DestinationPath)
			{
				continue;
			}
			Facts.bAnimationLineagePathCheckApplied = true;
			if (!NormalizeAnimationLineagePath(*CompatiblePath).Equals(
				NormalizeAnimationLineagePath(*DestinationPath),
				ESearchCase::CaseSensitive))
			{
				Facts.AnimationLineagePathMismatchRoles.AddUnique(RequiredRole);
			}
		}
	}

	void CopyAnimationLineageState(
		FOsvayderUECloseoutFactSnapshot& Target,
		const FOsvayderUECloseoutFactSnapshot& Source)
	{
		Target.bAnimationLineageRoleEvidencePresent = Source.bAnimationLineageRoleEvidencePresent;
		Target.bAnimationLineagePathEvidencePresent = Source.bAnimationLineagePathEvidencePresent;
		Target.bAnimationLineagePathCheckApplied = Source.bAnimationLineagePathCheckApplied;
		Target.RequiredAnimationRoles = Source.RequiredAnimationRoles;
		Target.CompatiblePostRetargetAnimationRoles = Source.CompatiblePostRetargetAnimationRoles;
		Target.MissingAnimationLineageRoles = Source.MissingAnimationLineageRoles;
		Target.AnimationLineagePathMismatchRoles = Source.AnimationLineagePathMismatchRoles;
		Target.GeneratedRetargetDestinationPaths = Source.GeneratedRetargetDestinationPaths;
		Target.RequiredAnimationRoleSourcePaths = Source.RequiredAnimationRoleSourcePaths;
		Target.RetargetGeneratedDestinationsBySource = Source.RetargetGeneratedDestinationsBySource;
		Target.RetargetGeneratedDestinationsByRole = Source.RetargetGeneratedDestinationsByRole;
		Target.CompatibleAnimationRolePaths = Source.CompatibleAnimationRolePaths;
	}

	void MergeAnimationLineageState(
		FOsvayderUECloseoutFactSnapshot& Target,
		const FOsvayderUECloseoutFactSnapshot& Source)
	{
		Target.bAnimationLineageRoleEvidencePresent =
			Target.bAnimationLineageRoleEvidencePresent || Source.bAnimationLineageRoleEvidencePresent;
		Target.bAnimationLineagePathEvidencePresent =
			Target.bAnimationLineagePathEvidencePresent || Source.bAnimationLineagePathEvidencePresent;
		for (const FString& Role : Source.RequiredAnimationRoles)
		{
			Target.RequiredAnimationRoles.AddUnique(Role);
		}
		for (const FString& Role : Source.CompatiblePostRetargetAnimationRoles)
		{
			Target.CompatiblePostRetargetAnimationRoles.AddUnique(Role);
		}
		for (const FString& Path : Source.GeneratedRetargetDestinationPaths)
		{
			Target.GeneratedRetargetDestinationPaths.AddUnique(Path);
		}
		for (const TPair<FString, FString>& Pair : Source.RequiredAnimationRoleSourcePaths)
		{
			Target.RequiredAnimationRoleSourcePaths.FindOrAdd(Pair.Key) = Pair.Value;
		}
		for (const TPair<FString, FString>& Pair : Source.RetargetGeneratedDestinationsBySource)
		{
			Target.RetargetGeneratedDestinationsBySource.FindOrAdd(Pair.Key) = Pair.Value;
		}
		for (const TPair<FString, FString>& Pair : Source.RetargetGeneratedDestinationsByRole)
		{
			Target.RetargetGeneratedDestinationsByRole.FindOrAdd(Pair.Key) = Pair.Value;
		}
		for (const TPair<FString, FString>& Pair : Source.CompatibleAnimationRolePaths)
		{
			Target.CompatibleAnimationRolePaths.FindOrAdd(Pair.Key) = Pair.Value;
		}
		RefreshAnimationLineageStatus(Target);
	}

	void ReadAnimationLineageRoleArray(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName,
		TArray<FString>& OutRoles)
	{
		const TArray<TSharedPtr<FJsonValue>>* RoleValues = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, RoleValues) || !RoleValues)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& RoleValue : *RoleValues)
		{
			if (!RoleValue.IsValid())
			{
				continue;
			}
			if (RoleValue->Type == EJson::String)
			{
				AddNormalizedUnique(OutRoles, RoleValue->AsString());
			}
			else if (RoleValue->Type == EJson::Object)
			{
				FString Role;
				const TSharedPtr<FJsonObject> RoleObject = RoleValue->AsObject();
				if (TryGetCloseoutStringAny(RoleObject, { TEXT("role"), TEXT("role_id"), TEXT("id"), TEXT("name") }, Role))
				{
					AddNormalizedUnique(OutRoles, Role);
				}
			}
		}
	}

	FString GetFirstAnimationMatchPath(const TSharedPtr<FJsonObject>& RoleObject, const bool bRequireMismatch)
	{
		FString DirectPath;
		if (TryGetCloseoutStringAny(
			RoleObject,
			{ TEXT("path"), TEXT("asset_path"), TEXT("animation_path"), TEXT("object_path"), TEXT("source_animation_path") },
			DirectPath))
		{
			return NormalizeAnimationLineagePath(DirectPath);
		}

		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		if (!RoleObject.IsValid() || !RoleObject->TryGetArrayField(TEXT("matches"), Matches) || !Matches)
		{
			return FString();
		}

		FString FirstPath;
		for (const TSharedPtr<FJsonValue>& MatchValue : *Matches)
		{
			if (!MatchValue.IsValid() || MatchValue->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> MatchObject = MatchValue->AsObject();
			FString MatchPath;
			if (!TryGetCloseoutStringAny(
				MatchObject,
				{ TEXT("path"), TEXT("asset_path"), TEXT("animation_path"), TEXT("object_path"), TEXT("source_animation_path") },
				MatchPath))
			{
				continue;
			}
			MatchPath = NormalizeAnimationLineagePath(MatchPath);
			if (FirstPath.IsEmpty())
			{
				FirstPath = MatchPath;
			}
			if (!bRequireMismatch)
			{
				return MatchPath;
			}
			const FString MatchJson = SerializeCloseoutJsonObjectCompact(MatchObject);
			if (TextIndicatesLineageMismatchStatus(MatchJson))
			{
				return MatchPath;
			}
		}
		return FirstPath;
	}

	void ExtractMismatchedAnimationLineage(
		const TSharedPtr<FJsonObject>& Object,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		if (!Object.IsValid())
		{
			return;
		}

		ReadAnimationLineageRoleArray(Object, TEXT("unsatisfied_roles"), Facts.RequiredAnimationRoles);
		const TArray<TSharedPtr<FJsonValue>>* UnsatisfiedRoles = nullptr;
		if (Object->TryGetArrayField(TEXT("unsatisfied_roles"), UnsatisfiedRoles) && UnsatisfiedRoles)
		{
			for (const TSharedPtr<FJsonValue>& RoleValue : *UnsatisfiedRoles)
			{
				if (!RoleValue.IsValid() || RoleValue->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> RoleObject = RoleValue->AsObject();
				FString Role;
				if (!TryGetCloseoutStringAny(RoleObject, { TEXT("role"), TEXT("role_id"), TEXT("id"), TEXT("name") }, Role))
				{
					continue;
				}
				const FString RoleKey = NormalizeAnimationLineageRole(Role);
				const FString SourcePath = GetFirstAnimationMatchPath(RoleObject, true);
				if (!RoleKey.IsEmpty() && !SourcePath.IsEmpty())
				{
					Facts.RequiredAnimationRoleSourcePaths.FindOrAdd(RoleKey) = SourcePath;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* RoleInventory = nullptr;
		if (Object->TryGetArrayField(TEXT("role_inventory"), RoleInventory) && RoleInventory)
		{
			for (const TSharedPtr<FJsonValue>& RoleValue : *RoleInventory)
			{
				if (!RoleValue.IsValid() || RoleValue->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> RoleObject = RoleValue->AsObject();
				const FString RoleJson = SerializeCloseoutJsonObjectCompact(RoleObject);
				if (!TextIndicatesLineageMismatchStatus(RoleJson))
				{
					continue;
				}
				FString Role;
				if (!TryGetCloseoutStringAny(RoleObject, { TEXT("role"), TEXT("role_id"), TEXT("id"), TEXT("name") }, Role))
				{
					continue;
				}
				const FString RoleKey = NormalizeAnimationLineageRole(Role);
				if (RoleKey.IsEmpty())
				{
					continue;
				}
				Facts.RequiredAnimationRoles.AddUnique(RoleKey);
				const FString SourcePath = GetFirstAnimationMatchPath(RoleObject, true);
				if (!SourcePath.IsEmpty())
				{
					Facts.RequiredAnimationRoleSourcePaths.FindOrAdd(RoleKey) = SourcePath;
				}
			}
		}

		Facts.bAnimationLineageRoleEvidencePresent = Facts.RequiredAnimationRoles.Num() > 0;
		Facts.bAnimationLineagePathEvidencePresent =
			Facts.bAnimationLineagePathEvidencePresent || Facts.RequiredAnimationRoleSourcePaths.Num() > 0;
		if (Facts.bAnimationLineageRoleEvidencePresent)
		{
			AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".animation_lineage_required_roles_observed"));
		}
		if (Facts.RequiredAnimationRoleSourcePaths.Num() > 0)
		{
			AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".animation_lineage_source_paths_observed"));
		}
		RefreshAnimationLineageStatus(Facts);
	}

	void ExtractCompatibleAnimationLineage(
		const TSharedPtr<FJsonObject>& Object,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		if (!Object.IsValid())
		{
			return;
		}

		ReadAnimationLineageRoleArray(Object, TEXT("compatible_roles"), Facts.CompatiblePostRetargetAnimationRoles);

		const TArray<TSharedPtr<FJsonValue>>* RoleInventory = nullptr;
		if (Object->TryGetArrayField(TEXT("role_inventory"), RoleInventory) && RoleInventory)
		{
			for (const TSharedPtr<FJsonValue>& RoleValue : *RoleInventory)
			{
				if (!RoleValue.IsValid() || RoleValue->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> RoleObject = RoleValue->AsObject();
				const FString RoleJson = SerializeCloseoutJsonObjectCompact(RoleObject);
				if (!TextIndicatesLineageCompatibleStatus(RoleJson) || TextIndicatesLineageMismatchStatus(RoleJson))
				{
					continue;
				}
				FString Role;
				if (!TryGetCloseoutStringAny(RoleObject, { TEXT("role"), TEXT("role_id"), TEXT("id"), TEXT("name") }, Role))
				{
					continue;
				}
				const FString RoleKey = NormalizeAnimationLineageRole(Role);
				if (RoleKey.IsEmpty())
				{
					continue;
				}
				Facts.CompatiblePostRetargetAnimationRoles.AddUnique(RoleKey);
				const FString CompatiblePath = GetFirstAnimationMatchPath(RoleObject, false);
				if (!CompatiblePath.IsEmpty())
				{
					Facts.CompatibleAnimationRolePaths.FindOrAdd(RoleKey) = CompatiblePath;
				}
			}
		}

		Facts.bAnimationLineagePathEvidencePresent =
			Facts.bAnimationLineagePathEvidencePresent || Facts.CompatibleAnimationRolePaths.Num() > 0;
		if (Facts.CompatiblePostRetargetAnimationRoles.Num() > 0)
		{
			AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".animation_lineage_compatible_roles_observed"));
		}
		if (Facts.CompatibleAnimationRolePaths.Num() > 0)
		{
			AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".animation_lineage_compatible_paths_observed"));
		}
		RefreshAnimationLineageStatus(Facts);
	}

	void ExtractRetargetLineage(
		const TSharedPtr<FJsonObject>& Object,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		const TArray<TSharedPtr<FJsonValue>>* GeneratedAssets = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("generated_assets"), GeneratedAssets) || !GeneratedAssets)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& AssetValue : *GeneratedAssets)
		{
			if (!AssetValue.IsValid())
			{
				continue;
			}
			if (AssetValue->Type == EJson::String)
			{
				const FString DestinationPath = NormalizeAnimationLineagePath(AssetValue->AsString());
				if (!DestinationPath.IsEmpty())
				{
					Facts.GeneratedRetargetDestinationPaths.AddUnique(DestinationPath);
				}
				continue;
			}
			if (AssetValue->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> AssetObject = AssetValue->AsObject();
			FString Role;
			FString SourcePath;
			FString DestinationPath;
			TryGetCloseoutStringAny(AssetObject, { TEXT("role"), TEXT("role_id"), TEXT("id"), TEXT("name") }, Role);
			TryGetCloseoutStringAny(
				AssetObject,
				{ TEXT("source_animation_path"), TEXT("source_path"), TEXT("source_object_path"), TEXT("input_animation_path") },
				SourcePath);
			TryGetCloseoutStringAny(
				AssetObject,
				{ TEXT("destination_object_path"), TEXT("destination_path"), TEXT("generated_asset"), TEXT("generated_path"), TEXT("path") },
				DestinationPath);
			SourcePath = NormalizeAnimationLineagePath(SourcePath);
			DestinationPath = NormalizeAnimationLineagePath(DestinationPath);
			if (!DestinationPath.IsEmpty())
			{
				Facts.GeneratedRetargetDestinationPaths.AddUnique(DestinationPath);
			}
			if (!SourcePath.IsEmpty() && !DestinationPath.IsEmpty())
			{
				Facts.RetargetGeneratedDestinationsBySource.FindOrAdd(SourcePath) = DestinationPath;
			}
			const FString RoleKey = NormalizeAnimationLineageRole(Role);
			if (!RoleKey.IsEmpty() && !DestinationPath.IsEmpty())
			{
				Facts.RetargetGeneratedDestinationsByRole.FindOrAdd(RoleKey) = DestinationPath;
			}
		}

		Facts.bAnimationLineagePathEvidencePresent =
			Facts.bAnimationLineagePathEvidencePresent
			|| Facts.GeneratedRetargetDestinationPaths.Num() > 0
			|| Facts.RetargetGeneratedDestinationsBySource.Num() > 0
			|| Facts.RetargetGeneratedDestinationsByRole.Num() > 0;
		if (Facts.GeneratedRetargetDestinationPaths.Num() > 0)
		{
			AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".animation_lineage_retarget_destinations_observed"));
		}
		RefreshAnimationLineageStatus(Facts);
	}

	bool HasAnimationWorkflowStep(
		const TArray<FString>& Steps,
		const FString& StepName)
	{
		for (const FString& Step : Steps)
		{
			if (Step.Equals(StepName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	void MarkLocalAnimationPackIntakeRequired(
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactId)
	{
		Facts.bLocalAnimationPackIntakeRequired = true;
		AddAnimationWorkflowFactId(Facts, FactId);
	}

	void MergeCanonAnimationWorkflowFacts(
		const FAgentCanonExecution& CanonExecution,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		if (HasAnimationWorkflowStep(CanonExecution.MandatoryAnimationWorkflowSteps, TEXT("local_animation_pack_intake")))
		{
			MarkLocalAnimationPackIntakeRequired(
				Facts,
				FactPrefix + TEXT(".local_animation_pack_intake_required"));
		}
		if (HasAnimationWorkflowStep(CanonExecution.MandatoryAnimationWorkflowSteps, TEXT("animation_preflight")))
		{
			Facts.bLocalAnimationPackIntakeRequired = true;
			AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".animation_preflight_required"));
		}
	}

	void ApplyAnimationWorkflowEvidence(
		const FString& EvidenceText,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		const TSharedPtr<FJsonObject> EvidenceObject = ParseAnimationWorkflowEvidenceObject(EvidenceText);
		if (IsLocalAnimationPackIntakeSuccessEvidence(EvidenceText))
		{
			Facts.bLocalAnimationPackIntakeSucceeded = true;
			AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".local_animation_pack_intake_succeeded"));
		}
		if (IsAnimationPreflightEvidence(EvidenceText))
		{
			Facts.bAnimationPreflightObserved = true;
			AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".animation_preflight_observed"));
			if (IsAnimationPreflightSkeletonMismatchEvidence(EvidenceText))
			{
				Facts.bAnimationPreflightSkeletonMismatchObserved = true;
				Facts.bAnimationRetargetFixupSucceeded = false;
				Facts.bPostRetargetCompatibleAnimationPreflightObserved = false;
				ResetAnimationLineageAfterLatestMismatch(Facts);
				ExtractMismatchedAnimationLineage(EvidenceObject, Facts, FactPrefix);
				AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".animation_preflight_skeleton_mismatch"));
			}
			else if (Facts.bAnimationRetargetFixupSucceeded && IsCompatibleAnimationPreflightEvidence(EvidenceText))
			{
				Facts.bPostRetargetCompatibleAnimationPreflightObserved = true;
				ExtractCompatibleAnimationLineage(EvidenceObject, Facts, FactPrefix);
				AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".post_retarget_compatible_animation_preflight"));
			}
		}
		if (IsAnimationRetargetFixupSuccessEvidence(EvidenceText))
		{
			Facts.bAnimationRetargetFixupSucceeded = true;
			ExtractRetargetLineage(EvidenceObject, Facts, FactPrefix);
			AddAnimationWorkflowFactId(Facts, FactPrefix + TEXT(".animation_retarget_fixup_succeeded"));
		}
		RefreshAnimationLineageStatus(Facts);
	}

	bool IsCloseoutInteractionAccessRecipe(const FAgentFeatureWorkflowState& Workflow)
	{
		return Workflow.RecipeId.Equals(OsvayderUERecipeRegistry::InteractionAccessRecipeId(), ESearchCase::CaseSensitive);
	}

	bool IsCloseoutFactSatisfiedByRecipeKey(
		const FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& EvidenceKey)
	{
		if (EvidenceKey == TEXT("known_proof_map_available"))
		{
			return Facts.bKnownProofMapAvailable;
		}
		if (EvidenceKey == TEXT("proof_input_mapping_available"))
		{
			return Facts.bProofInputMappingAvailable;
		}
		if (EvidenceKey == TEXT("placed_runtime_actors_available"))
		{
			return Facts.bPlacedRuntimeActorsAvailable;
		}
		if (EvidenceKey == TEXT("attempt_resolver_source_observed"))
		{
			return Facts.bAttemptResolverSourceObserved;
		}
		if (EvidenceKey == TEXT("event_subsystem_source_observed"))
		{
			return Facts.bEventSubsystemSourceObserved;
		}
		if (EvidenceKey == TEXT("runtime_smoke_success_observed"))
		{
			return Facts.bRuntimeSmokeSuccessObserved;
		}
		if (EvidenceKey == TEXT("prison_access_event_observed"))
		{
			return Facts.bPrisonAccessEventObserved;
		}
		if (EvidenceKey == TEXT("bounded_prison_access_automation_proof"))
		{
			return Facts.AutomationDiscoveryCount >= 7
				&& Facts.AutomationExecutedCount >= 7
				&& Facts.AutomationPassedCount >= 7
				&& Facts.AutomationFailedCount == 0;
		}
		if (EvidenceKey == TEXT("runtime_proof_state_passed"))
		{
			return Facts.bRuntimeProofPassed || Facts.HasBoundedInteractionAccessRuntimeProof();
		}
		return false;
	}

	bool IsWorkflowEvidenceSatisfiedByRecipeKey(
		const FAgentFeatureWorkflowState& Workflow,
		const FString& EvidenceKey)
	{
		if (EvidenceKey == TEXT("known_proof_map_available"))
		{
			return Workflow.bKnownProofMapAvailable;
		}
		if (EvidenceKey == TEXT("proof_input_mapping_available"))
		{
			return Workflow.InteractionAccessReuseObservation.HasSufficientReuseEvidence();
		}
		if (EvidenceKey == TEXT("placed_runtime_actors_available"))
		{
			return Workflow.bPlacedRuntimeActorsAvailable;
		}
		if (EvidenceKey == TEXT("attempt_resolver_source_observed"))
		{
			return Workflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved;
		}
		if (EvidenceKey == TEXT("event_subsystem_source_observed"))
		{
			return Workflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved;
		}
		if (EvidenceKey == TEXT("runtime_smoke_success_observed"))
		{
			return Workflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved;
		}
		if (EvidenceKey == TEXT("prison_access_event_observed"))
		{
			return Workflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved;
		}
		if (EvidenceKey == TEXT("bounded_prison_access_automation_proof"))
		{
			return Workflow.AutomationDiscoveryCount >= 7
				&& Workflow.AutomationExecutedCount >= 7
				&& Workflow.AutomationPassedCount >= 7
				&& Workflow.AutomationFailedCount == 0;
		}
		if (EvidenceKey == TEXT("runtime_proof_state_passed"))
		{
			return Workflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase);
		}
		return false;
	}

	TArray<FString> FindMissingCloseoutFactRecipeObligations(
		const FOsvayderUERecipeEvidenceContract& Contract,
		const FOsvayderUECloseoutFactSnapshot& Facts)
	{
		TArray<FString> Missing;
		for (const FString& EvidenceKey : Contract.RequiredPositiveEvidenceFacts)
		{
			if (!IsCloseoutFactSatisfiedByRecipeKey(Facts, EvidenceKey))
			{
				Missing.Add(EvidenceKey);
			}
		}
		return Missing;
	}

	TArray<FString> FindMissingWorkflowRecipeObligations(
		const FOsvayderUERecipeEvidenceContract& Contract,
		const FAgentFeatureWorkflowState& Workflow)
	{
		TArray<FString> Missing;
		for (const FString& EvidenceKey : Contract.RequiredPositiveEvidenceFacts)
		{
			if (!IsWorkflowEvidenceSatisfiedByRecipeKey(Workflow, EvidenceKey))
			{
				Missing.Add(EvidenceKey);
			}
		}
		return Missing;
	}

	FString JoinRecipeObligations(const TArray<FString>& Obligations)
	{
		return FString::Join(Obligations, TEXT(","));
	}

	FString BuildMissingRecipeObligationGateReason(const TArray<FString>& MissingObligations)
	{
		if (MissingObligations.Contains(TEXT("proof_input_mapping_available")))
		{
			return TEXT("feature_workflow_proof_input_mapping_missing");
		}
		if (MissingObligations.Contains(TEXT("attempt_resolver_source_observed"))
			|| MissingObligations.Contains(TEXT("event_subsystem_source_observed"))
			|| MissingObligations.Contains(TEXT("runtime_smoke_success_observed"))
			|| MissingObligations.Contains(TEXT("prison_access_event_observed")))
		{
			return TEXT("feature_workflow_attempt_resolver_observation_missing");
		}
		return TEXT("feature_workflow_recipe_obligation_missing");
	}

	FString BuildMissingRecipeObligationBlockerFamily(const TArray<FString>& MissingObligations)
	{
		if (MissingObligations.Contains(TEXT("proof_input_mapping_available")))
		{
			return TEXT("proof_input_mapping_missing");
		}
		if (BuildMissingRecipeObligationGateReason(MissingObligations)
			== TEXT("feature_workflow_attempt_resolver_observation_missing"))
		{
			return TEXT("attempt_resolver_observation_missing");
		}
		return TEXT("recipe_obligation_missing");
	}

	bool IsProofPrerequisiteAccountingBlocker(const FAgentFeatureWorkflowState& Workflow)
	{
		const FString Combined =
			(Workflow.StopLossReason + TEXT(" ") + Workflow.BlockerFamily + TEXT(" ") + Workflow.BlockerDetail).ToLower();
		return Combined.Contains(TEXT("proof_prerequisites_missing"))
			|| Combined.Contains(TEXT("proof_prerequisite"));
	}

	bool IsAdHocRuntimeProofAccountingBlocker(const FAgentFeatureWorkflowState& Workflow)
	{
		const FString Combined =
			(Workflow.StopLossReason + TEXT(" ") + Workflow.BlockerFamily + TEXT(" ") + Workflow.BlockerDetail).ToLower();
		return Combined.Contains(TEXT("ad_hoc_runtime_proof_attempts"));
	}

	bool IsAttemptResolverAndLoggingStopLoss(const FAgentFeatureWorkflowState& Workflow)
	{
		const FString Combined =
			(Workflow.StopLossReason + TEXT(" ") + Workflow.BlockerFamily + TEXT(" ") + Workflow.BlockerDetail).ToLower();
		return Combined.Contains(TEXT("phase_failed_twice:attempt_resolver_and_logging"));
	}

	bool IsWorkflowOrCloseoutFactSatisfiedByRecipeKey(
		const FAgentFeatureWorkflowState& Workflow,
		const FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& EvidenceKey)
	{
		return IsWorkflowEvidenceSatisfiedByRecipeKey(Workflow, EvidenceKey)
			|| IsCloseoutFactSatisfiedByRecipeKey(Facts, EvidenceKey);
	}

	TArray<FString> FindMissingWorkflowOrCloseoutFactRecipeObligations(
		const FOsvayderUERecipeEvidenceContract& Contract,
		const FAgentFeatureWorkflowState& Workflow,
		const FOsvayderUECloseoutFactSnapshot& Facts)
	{
		TArray<FString> Missing;
		for (const FString& EvidenceKey : Contract.RequiredPositiveEvidenceFacts)
		{
			if (!IsWorkflowOrCloseoutFactSatisfiedByRecipeKey(Workflow, Facts, EvidenceKey))
			{
				Missing.Add(EvidenceKey);
			}
		}
		return Missing;
	}

	bool HasBoundedInteractionAccessRuntimeProofFromWorkflowOrFacts(
		const FAgentFeatureWorkflowState& Workflow,
		const FOsvayderUECloseoutFactSnapshot& Facts)
	{
		FOsvayderUERecipeEvidenceContract Contract;
		if (!OsvayderUERecipeRegistry::TryGetRecipeEvidenceContract(Workflow.RecipeId, Contract)
			|| !Contract.RecipeId.Equals(OsvayderUERecipeRegistry::InteractionAccessRecipeId(), ESearchCase::CaseSensitive))
		{
			return false;
		}

		return FindMissingWorkflowOrCloseoutFactRecipeObligations(Contract, Workflow, Facts).Num() == 0;
	}

	bool IsPostProofReadOnlyCloseoutDriftBlocker(
		const FAgentFeatureWorkflowState& Workflow,
		const FOsvayderUECloseoutFactSnapshot& Facts)
	{
		return Workflow.bStopLossTriggered
			&& Workflow.StopLossReason.Equals(TEXT("command_execution_without_phase_advance_gt_5"), ESearchCase::IgnoreCase)
			&& Workflow.BlockerFamily.IsEmpty()
			&& Workflow.BlockerDetail.IsEmpty()
			&& Workflow.CurrentPhase.Equals(TEXT("memory_update"), ESearchCase::IgnoreCase)
			&& Workflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase)
			&& Workflow.ProofPrerequisiteState.Equals(TEXT("satisfied"), ESearchCase::IgnoreCase)
			&& (Workflow.AutomationFailedCount == INDEX_NONE || Workflow.AutomationFailedCount == 0)
			&& HasBoundedInteractionAccessRuntimeProofFromWorkflowOrFacts(Workflow, Facts)
			&& !Facts.bCommandMutationDetected
			&& !Facts.bManagedStateManualWriteDetected;
	}

	bool IsManagedOsvayderUEStatePathMentioned(const FString& Text)
	{
		FString Normalized = Text.ToLower();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Normalized.Contains(TEXT("saved/osvayderue/active_plan.json"))
			|| Normalized.Contains(TEXT("saved/osvayderue/planarchives"))
			|| Normalized.Contains(TEXT("saved/osvayderue/closeoutdecisions"))
			|| Normalized.Contains(TEXT("saved/osvayderue/closeout_decision.json"))
			|| Normalized.Contains(TEXT("saved/osvayderue/visible_session"))
			|| Normalized.Contains(TEXT("saved/osvayderue/agent_trace"));
	}

	bool IsCommandExecutionToolName(const FString& ToolName)
	{
		return ToolName.Equals(TEXT("command_execution"), ESearchCase::IgnoreCase)
			|| ToolName.EndsWith(TEXT("/command_execution"), ESearchCase::IgnoreCase)
			|| ToolName.Contains(TEXT("command_execution"), ESearchCase::IgnoreCase);
	}

	void ExtractManagedStateManualWriteFact(
		const TSharedPtr<FJsonObject>& Payload,
		const FString& ToolName,
		const FString& EvidenceText,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix,
		const FString& RunId = FString())
	{
		if (!Payload.IsValid())
		{
			return;
		}

		bool bClassifiedMutatingTool = false;
		bool bPrimaryMutationAssigned = false;
		FString ClassifiedToolFamily;
		Payload->TryGetBoolField(TEXT("classified_mutating_tool"), bClassifiedMutatingTool);
		Payload->TryGetBoolField(TEXT("primary_mutation_assigned_by_this_tool"), bPrimaryMutationAssigned);
		Payload->TryGetStringField(TEXT("classified_tool_family"), ClassifiedToolFamily);

		FString ToolInput;
		FString ToolResult;
		FString RawProviderEvent;
		Payload->TryGetStringField(TEXT("tool_input"), ToolInput);
		Payload->TryGetStringField(TEXT("tool_result"), ToolResult);
		Payload->TryGetStringField(TEXT("raw_provider_event"), RawProviderEvent);

		double ExitCodeNumber = static_cast<double>(INDEX_NONE);
		const bool bHasExitCode = Payload->TryGetNumberField(TEXT("exit_code"), ExitCodeNumber);

		OsvayderUE::CommandClassification::FOsvayderUEExecutionTruthInputs TruthInputs;
		TruthInputs.RunId = RunId;
		TruthInputs.ToolName = ToolName;
		TruthInputs.CommandInput = ToolInput;
		TruthInputs.ToolFamily = ClassifiedToolFamily;
		TruthInputs.ToolResult = ToolResult;
		TruthInputs.RawJson = RawProviderEvent;
		TruthInputs.ExitCode = bHasExitCode ? static_cast<int32>(ExitCodeNumber) : INDEX_NONE;
		TruthInputs.bClassifiedMutatingTool = bClassifiedMutatingTool;
		TruthInputs.bPrimaryMutationAssigned = bPrimaryMutationAssigned;
		const OsvayderUE::CommandClassification::FOsvayderUEExecutionTruthDecision TruthDecision =
			OsvayderUE::CommandClassification::ClassifyExecutionTruth(TruthInputs);

		if (OsvayderUE::CommandClassification::IsCommandExecutionLikeToolName(ToolName))
		{
			Facts.ExecutionTruthDecisionSummaries.AddUnique(
				FactPrefix + TEXT(".execution_truth: ") + TruthDecision.ToSummaryString());
		}

		const bool bTruthReadOnlyCommand =
			OsvayderUE::CommandClassification::IsCommandExecutionLikeToolName(ToolName)
			&& TruthDecision.Category == OsvayderUE::CommandClassification::EOsvayderUEExecutionTruthCategory::ReadOnlyInspection;
		const bool bCommandMutation =
			!bTruthReadOnlyCommand
			&& (
			TruthDecision.Category == OsvayderUE::CommandClassification::EOsvayderUEExecutionTruthCategory::ManagedStateWrite
			|| TruthDecision.Category == OsvayderUE::CommandClassification::EOsvayderUEExecutionTruthCategory::ApprovedProjectMutation
			|| bClassifiedMutatingTool
			|| bPrimaryMutationAssigned
			|| ClassifiedToolFamily.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase));
		if (!bCommandMutation)
		{
			return;
		}

		const FString MutationFactId = FactPrefix + TEXT(".command_mutation");
		Facts.bCommandMutationDetected = true;
		Facts.CommandMutationFactIds.AddUnique(MutationFactId);
		AddCloseoutFactId(Facts, MutationFactId);

		if (TruthDecision.Category != OsvayderUE::CommandClassification::EOsvayderUEExecutionTruthCategory::ManagedStateWrite
			&& !IsManagedOsvayderUEStatePathMentioned(EvidenceText))
		{
			return;
		}

		const FString FactId = FactPrefix + TEXT(".managed_state_manual_write");
		Facts.bManagedStateManualWriteDetected = true;
		Facts.ManagedStateManualWriteFactIds.AddUnique(FactId);
		AddCloseoutFactId(Facts, FactId);
	}

	FString BuildManagedStateManualWriteDetail(const TArray<FString>& FactIds)
	{
		if (FactIds.Num() == 0)
		{
			return TEXT("agent command_execution mutated Saved/OsvayderUE managed state");
		}

		TArray<FString> Preview;
		const int32 PreviewCount = FMath::Min(FactIds.Num(), 3);
		for (int32 Index = 0; Index < PreviewCount; ++Index)
		{
			Preview.Add(FactIds[Index]);
		}

		FString Detail = FString::Printf(
			TEXT("agent command_execution mutated Saved/OsvayderUE managed state: %s"),
			*FString::Join(Preview, TEXT(", ")));
		if (FactIds.Num() > PreviewCount)
		{
			Detail += FString::Printf(TEXT(" (+%d more)"), FactIds.Num() - PreviewCount);
		}
		return Detail;
	}

	void MarkCloseoutPhaseCompleted(FAgentFeatureWorkflowState& Workflow, const FString& PhaseId)
	{
		Workflow.CompletedPhaseIds.AddUnique(PhaseId);
		if (FAgentFeatureWorkflowPhaseState* Phase = Workflow.FindMutablePhase(PhaseId))
		{
			Phase->Status = TEXT("completed");
			Phase->LastFailureReason.Reset();
		}
	}

	void ClearContradictedProofAccountingStopLoss(FAgentFeatureWorkflowState& Workflow)
	{
		const bool bAccountingBlocker =
			IsProofPrerequisiteAccountingBlocker(Workflow)
			|| IsAdHocRuntimeProofAccountingBlocker(Workflow);
		Workflow.bStopLossTriggered = false;
		Workflow.StopLossReason.Reset();
		if (bAccountingBlocker)
		{
			Workflow.BlockerFamily.Reset();
			Workflow.BlockerDetail.Reset();
		}
		if (Workflow.TerminalStatus.Equals(TEXT("stop_loss"), ESearchCase::IgnoreCase))
		{
			Workflow.TerminalStatus.Reset();
		}
	}

	void ClearLatchedCleanCloseoutStopLoss(FAgentFeatureWorkflowState& Workflow)
	{
		Workflow.bStopLossTriggered = false;
		Workflow.StopLossReason.Reset();
		Workflow.BlockerFamily.Reset();
		Workflow.BlockerDetail.Reset();
		Workflow.CommandExecutionCallsWithoutPhaseAdvance = 0;
		Workflow.FailedPhaseIds.Remove(TEXT("memory_update"));
		if (Workflow.TerminalStatus.Equals(TEXT("stop_loss"), ESearchCase::IgnoreCase))
		{
			Workflow.TerminalStatus.Reset();
		}
		MarkCloseoutPhaseCompleted(Workflow, TEXT("memory_update"));
	}

	void ClearContradictedAttemptResolverAndLoggingStopLoss(FAgentFeatureWorkflowState& Workflow)
	{
		Workflow.bStopLossTriggered = false;
		Workflow.StopLossReason.Reset();
		Workflow.BlockerFamily.Reset();
		Workflow.BlockerDetail.Reset();
		Workflow.CommandExecutionCallsWithoutPhaseAdvance = 0;
		Workflow.FailedPhaseIds.Remove(TEXT("attempt_resolver_and_logging"));
		if (Workflow.TerminalStatus.Equals(TEXT("stop_loss"), ESearchCase::IgnoreCase))
		{
			Workflow.TerminalStatus.Reset();
		}
		MarkCloseoutPhaseCompleted(Workflow, TEXT("attempt_resolver_and_logging"));
	}

	FString SerializeCloseoutJsonObjectCompact(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return FString();
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
		{
			return FString();
		}
		Writer->Close();
		return JsonText;
	}

	bool ContainsProofInputMappingEvidence(const FString& Lower)
	{
		const bool bMentionsProofInputContext =
			Lower.Contains(TEXT("imc_prisonaccessproof"))
			|| Lower.Contains(TEXT("/game/prisonaccess/input/imc_prisonaccessproof"))
			|| Lower.Contains(TEXT("persistent_input_asset_observed"));
		const bool bMentionsInteractAction =
			Lower.Contains(TEXT("ia_interact"))
			|| Lower.Contains(TEXT("interaction_action_asset_observed"))
			|| Lower.Contains(TEXT("inputpathtruth"));
		const bool bMentionsReadOnlyInputInspection =
			Lower.Contains(TEXT("enhanced_input"))
			|| Lower.Contains(TEXT("read_only_enhanced_input_query_observed"))
			|| Lower.Contains(TEXT("input mapping"));
		return bMentionsProofInputContext && bMentionsInteractAction && bMentionsReadOnlyInputInspection;
	}

	bool ContainsCloseoutAutomationSuccessLineForTest(const FString& Lower, const TCHAR* TestName)
	{
		TArray<FString> Lines;
		Lower.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			if (Line.Contains(TestName) && Line.Contains(TEXT("result={success}")))
			{
				return true;
			}
		}
		return false;
	}

	bool TryParseCloseoutExitCode(const FString& RawExitCode, int32& OutExitCode)
	{
		const FString Trimmed = RawExitCode.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return false;
		}

		OutExitCode = FCString::Atoi(*Trimmed);
		return Trimmed.Equals(FString::FromInt(OutExitCode), ESearchCase::CaseSensitive);
	}

	bool ContainsCloseoutExitCodeMatching(const FString& Lower, const int32 ExpectedExitCode)
	{
		const FRegexPattern ExitCodePattern(TEXT("exit\\s*code\\s*:\\s*(-?\\d+)"));
		FRegexMatcher ExitCodeMatcher(ExitCodePattern, Lower);
		while (ExitCodeMatcher.FindNext())
		{
			int32 ExitCode = 0;
			if (TryParseCloseoutExitCode(ExitCodeMatcher.GetCaptureGroup(1), ExitCode)
				&& ExitCode == ExpectedExitCode)
			{
				return true;
			}
		}

		const FRegexPattern RequestExitPattern(TEXT("requestexitwithstatus\\s*\\([^,]*,\\s*(-?\\d+)"));
		FRegexMatcher RequestExitMatcher(RequestExitPattern, Lower);
		while (RequestExitMatcher.FindNext())
		{
			int32 ExitCode = 0;
			if (TryParseCloseoutExitCode(RequestExitMatcher.GetCaptureGroup(1), ExitCode)
				&& ExitCode == ExpectedExitCode)
			{
				return true;
			}
		}

		return false;
	}

	bool ContainsCloseoutNonZeroExitCode(const FString& Lower)
	{
		const FRegexPattern ExitCodePattern(TEXT("exit\\s*code\\s*:\\s*(-?\\d+)"));
		FRegexMatcher ExitCodeMatcher(ExitCodePattern, Lower);
		while (ExitCodeMatcher.FindNext())
		{
			int32 ExitCode = 0;
			if (TryParseCloseoutExitCode(ExitCodeMatcher.GetCaptureGroup(1), ExitCode)
				&& ExitCode != 0)
			{
				return true;
			}
		}

		const FRegexPattern RequestExitPattern(TEXT("requestexitwithstatus\\s*\\([^,]*,\\s*(-?\\d+)"));
		FRegexMatcher RequestExitMatcher(RequestExitPattern, Lower);
		while (RequestExitMatcher.FindNext())
		{
			int32 ExitCode = 0;
			if (TryParseCloseoutExitCode(RequestExitMatcher.GetCaptureGroup(1), ExitCode)
				&& ExitCode != 0)
			{
				return true;
			}
		}

		return false;
	}

	void ExtractCloseoutAutomationFactsFromText(
		const FString& EvidenceText,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		const FString Lower = EvidenceText.ToLower();
		if (!Lower.Contains(TEXT("alternative.prisonaccess")))
		{
			return;
		}

		const bool bDiscoveryLine =
			Lower.Contains(TEXT("found 7 automation tests based on 'alternative.prisonaccess'"))
			|| Lower.Contains(TEXT("found 7 automation tests based on \"alternative.prisonaccess\""))
			|| Lower.Contains(TEXT("found 7 automation tests based on alternative.prisonaccess"));
		const int32 SuccessCount = CountCloseoutNeedleOccurrences(Lower, TEXT("result={success}"));
		const bool bExitZero = ContainsCloseoutExitCodeMatching(Lower, 0)
			|| Lower.Contains(TEXT("exit code: 0"))
			|| Lower.Contains(TEXT("requestexitwithstatus(true, 0"))
			|| Lower.Contains(TEXT("requestexitwithstatus(..., 0"));
		const bool bKnownFailure = Lower.Contains(TEXT("result={failed}"))
			|| Lower.Contains(TEXT("result={error}"))
			|| ContainsCloseoutNonZeroExitCode(Lower)
			|| Lower.Contains(TEXT("exit code: 1"))
			|| Lower.Contains(TEXT("exit code: 3"))
			|| Lower.Contains(TEXT("automation test failed"));
		const bool bInputPathTruthSuccess =
			ContainsCloseoutAutomationSuccessLineForTest(Lower, TEXT("inputpathtruth"));
		const bool bProofFixturePresenceSuccess =
			ContainsCloseoutAutomationSuccessLineForTest(Lower, TEXT("prooffixturepresence"));
		const bool bProofFixtureSmokeSuccess =
			ContainsCloseoutAutomationSuccessLineForTest(Lower, TEXT("prooffixturesmoke"));
		const int32 PrisonAccessEventCount =
			CountCloseoutNeedleOccurrences(Lower, TEXT("prisonaccessevent order="));

		if (bDiscoveryLine)
		{
			MergeCloseoutCount(Facts.AutomationDiscoveryCount, 7);
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".automation_discovery_count"));
		}
		if (SuccessCount >= 7)
		{
			MergeCloseoutCount(Facts.AutomationExecutedCount, SuccessCount);
			MergeCloseoutCount(Facts.AutomationPassedCount, SuccessCount);
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".automation_success_count"));
		}
		if (bDiscoveryLine && SuccessCount >= 7 && bExitZero && !bKnownFailure)
		{
			MergeCloseoutCount(Facts.AutomationFailedCount, 0);
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".automation_zero_failures"));
		}
		if (bProofFixtureSmokeSuccess)
		{
			Facts.bRuntimeSmokeSuccessObserved = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".runtime_smoke_success"));
		}
		if (PrisonAccessEventCount >= 2)
		{
			Facts.bPrisonAccessEventObserved = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".prison_access_event"));
		}
		if (bInputPathTruthSuccess)
		{
			Facts.bProofInputMappingAvailable = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".proof_input_mapping"));
		}
		if (bProofFixturePresenceSuccess)
		{
			Facts.bKnownProofMapAvailable = true;
			Facts.bPlacedRuntimeActorsAvailable = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".known_proof_map"));
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".placed_runtime_actors"));
		}
	}

	void ExtractReferencedAutomationLogFactsFromText(
		const FString& EvidenceText,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		FString Normalized = EvidenceText;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

		const FRegexPattern LogPathPattern(
			TEXT("([A-Za-z]:/[^\\s\\\"']*Saved/Logs/[^\\s\\\"']*automation[^\\s\\\"']*\\.log|Saved/Logs/[^\\s\\\"']*automation[^\\s\\\"']*\\.log)"));
		FRegexMatcher Matcher(LogPathPattern, Normalized);
		TArray<FString> CandidatePaths;
		while (Matcher.FindNext())
		{
			CandidatePaths.AddUnique(Matcher.GetCaptureGroup(1));
		}

		for (const FString& CandidatePath : CandidatePaths)
		{
			FString ResolvedPath = CandidatePath;
			if (FPaths::IsRelative(ResolvedPath))
			{
				ResolvedPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), ResolvedPath));
			}
			FPaths::NormalizeFilename(ResolvedPath);
			const int64 FileSize = IFileManager::Get().FileSize(*ResolvedPath);
			if (FileSize <= 0 || FileSize > 8 * 1024 * 1024)
			{
				continue;
			}

			FString LogText;
			if (FFileHelper::LoadFileToString(LogText, *ResolvedPath))
			{
				ExtractCloseoutAutomationFactsFromText(
					LogText,
					Facts,
					FactPrefix + TEXT(".referenced_automation_log"));
			}
		}
	}

	void AddExternalRuntimeProofPrefixCandidate(TArray<FString>& OutPrefixes, FString Candidate)
	{
		Candidate = Candidate.TrimStartAndEnd();
		while (!Candidate.IsEmpty()
			&& (Candidate[0] == TEXT('`') || Candidate[0] == TEXT('"') || Candidate[0] == TEXT('\'')))
		{
			Candidate = Candidate.RightChop(1).TrimStartAndEnd();
		}
		while (!Candidate.IsEmpty())
		{
			const TCHAR Last = Candidate[Candidate.Len() - 1];
			if (Last != TEXT('`')
				&& Last != TEXT('"')
				&& Last != TEXT('\'')
				&& Last != TEXT('.')
				&& Last != TEXT(',')
				&& Last != TEXT(';')
				&& Last != TEXT(':'))
			{
				break;
			}
			Candidate = Candidate.Left(Candidate.Len() - 1).TrimStartAndEnd();
		}

		bool bHasDistinctiveSeparatorOrDigit = false;
		for (const TCHAR Ch : Candidate)
		{
			const bool bValid = FChar::IsAlnum(Ch)
				|| Ch == TEXT('_')
				|| Ch == TEXT('-')
				|| Ch == TEXT('.');
			if (!bValid)
			{
				return;
			}
			bHasDistinctiveSeparatorOrDigit =
				bHasDistinctiveSeparatorOrDigit
				|| FChar::IsDigit(Ch)
				|| Ch == TEXT('_')
				|| Ch == TEXT('-')
				|| Ch == TEXT('.');
		}

		if (Candidate.Len() >= 5 && bHasDistinctiveSeparatorOrDigit)
		{
			OutPrefixes.AddUnique(Candidate);
		}
	}

	TArray<FString> ExtractPlanDeclaredExternalRuntimeProofPrefixes(const FOsvayderUEActivePlan& Plan)
	{
		TArray<FString> Prefixes;
		const FString Text = Plan.OriginalUserTask;
		const FString Lower = Text.ToLower();
		int32 SearchStart = 0;
		while (SearchStart < Lower.Len())
		{
			const int32 PrefixIndex =
				Lower.Find(TEXT("prefix"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
			if (PrefixIndex == INDEX_NONE)
			{
				break;
			}

			int32 Cursor = PrefixIndex + 6;
			while (Cursor < Text.Len())
			{
				const TCHAR Ch = Text[Cursor];
				if (!FChar::IsWhitespace(Ch)
					&& Ch != TEXT(':')
					&& Ch != TEXT('=')
					&& Ch != TEXT('`')
					&& Ch != TEXT('"')
					&& Ch != TEXT('\''))
				{
					break;
				}
				++Cursor;
			}

			const int32 CandidateStart = Cursor;
			while (Cursor < Text.Len())
			{
				const TCHAR Ch = Text[Cursor];
				if (!FChar::IsAlnum(Ch)
					&& Ch != TEXT('_')
					&& Ch != TEXT('-')
					&& Ch != TEXT('.'))
				{
					break;
				}
				++Cursor;
			}

			AddExternalRuntimeProofPrefixCandidate(
				Prefixes,
				Text.Mid(CandidateStart, Cursor - CandidateStart));
			SearchStart = FMath::Max(Cursor, PrefixIndex + 6);
		}

		FString Normalized = Text;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		const FRegexPattern AutomationLogPattern(
			TEXT("([A-Za-z0-9][A-Za-z0-9_\\-.]*automation[A-Za-z0-9_\\-.]*\\.log)"));
		FRegexMatcher AutomationLogMatcher(AutomationLogPattern, Normalized);
		while (AutomationLogMatcher.FindNext())
		{
			const FString CleanFileName = FPaths::GetCleanFilename(AutomationLogMatcher.GetCaptureGroup(1));
			const FString BaseName = FPaths::GetBaseFilename(CleanFileName);
			const FString LowerBaseName = BaseName.ToLower();
			const int32 AutomationIndex = LowerBaseName.Find(TEXT("automation"), ESearchCase::CaseSensitive);
			if (AutomationIndex == INDEX_NONE)
			{
				continue;
			}

			FString Prefix = BaseName.Left(AutomationIndex);
			while (!Prefix.IsEmpty())
			{
				const TCHAR Last = Prefix[Prefix.Len() - 1];
				if (Last != TEXT('_') && Last != TEXT('-') && Last != TEXT('.'))
				{
					break;
				}
				Prefix.LeftChopInline(1);
			}
			AddExternalRuntimeProofPrefixCandidate(Prefixes, Prefix);
		}
		return Prefixes;
	}

	bool DoesExternalRuntimeProofLogMatchCurrentPrefix(
		const FString& FileName,
		const TArray<FString>& Prefixes)
	{
		const FString LowerFileName = FileName.ToLower();
		if (!LowerFileName.EndsWith(TEXT(".log"))
			|| !LowerFileName.Contains(TEXT("automation")))
		{
			return false;
		}

		for (const FString& Prefix : Prefixes)
		{
			const FString LowerPrefix = Prefix.ToLower();
			if (LowerPrefix.IsEmpty() || !LowerFileName.StartsWith(LowerPrefix))
			{
				continue;
			}

			if (LowerFileName.Len() == LowerPrefix.Len())
			{
				return true;
			}

			const TCHAR Next = LowerFileName[LowerPrefix.Len()];
			if (Next == TEXT('_') || Next == TEXT('-') || Next == TEXT('.'))
			{
				return true;
			}
		}
		return false;
	}

	bool IsExternalRuntimeProofRecoveryCandidatePlan(const FOsvayderUEActivePlan& Plan)
	{
		const FAgentFeatureWorkflowState& Workflow = Plan.FeatureWorkflow;
		return IsCloseoutInteractionAccessRecipe(Workflow)
			&& Workflow.bRuntimeProofRequired
			&& Workflow.bStopLossTriggered
			&& (IsProofPrerequisiteAccountingBlocker(Workflow)
				|| IsAdHocRuntimeProofAccountingBlocker(Workflow)
				|| IsAttemptResolverAndLoggingStopLoss(Workflow));
	}

	bool IsExternalRuntimeProofLogCurrentForPlan(
		const FString& CandidateLogPath,
		const FOsvayderUEActivePlan& Plan,
		FDateTime& OutModifiedAt)
	{
		if (Plan.CreatedAtUtc.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		FDateTime PlanCreatedAt;
		if (!FDateTime::ParseIso8601(*Plan.CreatedAtUtc, PlanCreatedAt))
		{
			return false;
		}

		OutModifiedAt = IFileManager::Get().GetTimeStamp(*CandidateLogPath);
		if (OutModifiedAt == FDateTime::MinValue())
		{
			return false;
		}

		return OutModifiedAt >= PlanCreatedAt;
	}

	void NoteExternalRuntimeProofLogTimestamp(
		FOsvayderUERunCloseoutContext& Context,
		const FDateTime& ModifiedAt)
	{
		const FString Timestamp = ModifiedAt.ToIso8601();
		if (Context.StartedAtUtc.IsEmpty() || IsIsoUtcAfter(Context.StartedAtUtc, Timestamp))
		{
			Context.StartedAtUtc = Timestamp;
		}
		if (Context.CompletedAtUtc.IsEmpty() || IsIsoUtcAfter(Timestamp, Context.CompletedAtUtc))
		{
			Context.CompletedAtUtc = Timestamp;
		}
	}

	bool TryLoadProjectLocalSourceText(
		const FString& ProjectRoot,
		const TArray<FString>& RelativePaths,
		FString& OutSourceText)
	{
		FString Root = FPaths::ConvertRelativePathToFull(ProjectRoot);
		FPaths::NormalizeDirectoryName(Root);
		for (const FString& RelativePath : RelativePaths)
		{
			FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root, RelativePath));
			FPaths::NormalizeFilename(FullPath);
			if (!FPaths::FileExists(FullPath))
			{
				continue;
			}

			FString Text;
			if (FFileHelper::LoadFileToString(Text, *FullPath))
			{
				OutSourceText += TEXT("\n") + FullPath + TEXT("\n") + Text;
			}
		}
		return !OutSourceText.TrimStartAndEnd().IsEmpty();
	}

	void MergeProjectLocalInteractionAccessSourceFacts(
		const FString& ProjectRoot,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		FString ResolverText;
		if (TryLoadProjectLocalSourceText(
				ProjectRoot,
				{
					TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.cpp"),
					TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessAttemptResolver.h")
				},
				ResolverText))
		{
			const FString LowerResolver = ResolverText.ToLower();
			if (LowerResolver.Contains(TEXT("falternativeprisonaccessattemptresolver"))
				&& (LowerResolver.Contains(TEXT("resolvedoorattempt"))
					|| LowerResolver.Contains(TEXT("resolvetechnicalboxattempt"))
					|| LowerResolver.Contains(TEXT("falternativeprisonaccessattemptoutcome"))))
			{
				Facts.bAttemptResolverSourceObserved = true;
				AddCloseoutFactId(Facts, FactPrefix + TEXT(".attempt_resolver_source"));
			}
		}

		FString EventSubsystemText;
		if (TryLoadProjectLocalSourceText(
				ProjectRoot,
				{
					TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.cpp"),
					TEXT("Source/Alternative/PrisonAccess/AlternativePrisonAccessEventSubsystem.h")
				},
				EventSubsystemText))
		{
			const FString LowerEventSubsystem = EventSubsystemText.ToLower();
			if (LowerEventSubsystem.Contains(TEXT("ualternativeprisonaccesseventsubsystem"))
				&& (LowerEventSubsystem.Contains(TEXT("falternativeprisonaccesseventrecord"))
					|| LowerEventSubsystem.Contains(TEXT("recordevent"))
					|| LowerEventSubsystem.Contains(TEXT("prisonaccessevent order="))))
			{
				Facts.bEventSubsystemSourceObserved = true;
				AddCloseoutFactId(Facts, FactPrefix + TEXT(".event_subsystem_source"));
			}
		}
	}

	void MergeCurrentPrefixExternalRuntimeProofLogFacts(
		const FOsvayderUEActivePlan& Plan,
		FOsvayderUERunCloseoutContext& Context)
	{
		if (!IsExternalRuntimeProofRecoveryCandidatePlan(Plan)
			|| Context.Facts.HasBoundedInteractionAccessRuntimeProof())
		{
			return;
		}

		const TArray<FString> Prefixes = ExtractPlanDeclaredExternalRuntimeProofPrefixes(Plan);
		if (Prefixes.Num() == 0)
		{
			return;
		}

		FString ProjectRoot = Context.ProjectRoot.TrimStartAndEnd();
		if (ProjectRoot.IsEmpty())
		{
			ProjectRoot = GetCloseoutProjectRoot();
		}
		ProjectRoot = FPaths::ConvertRelativePathToFull(ProjectRoot);
		FPaths::NormalizeDirectoryName(ProjectRoot);

		const FString LogDir = FPaths::Combine(ProjectRoot, TEXT("Saved"), TEXT("Logs"));
		if (!IFileManager::Get().DirectoryExists(*LogDir))
		{
			return;
		}

		TArray<FString> LogFileNames;
		IFileManager::Get().FindFiles(
			LogFileNames,
			*FPaths::Combine(LogDir, TEXT("*.log")),
			true,
			false);

		for (const FString& LogFileName : LogFileNames)
		{
			const FString CleanFileName = FPaths::GetCleanFilename(LogFileName);
			if (!DoesExternalRuntimeProofLogMatchCurrentPrefix(CleanFileName, Prefixes))
			{
				continue;
			}

			FString CandidateLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(LogDir, CleanFileName));
			FPaths::NormalizeFilename(CandidateLogPath);
			const int64 FileSize = IFileManager::Get().FileSize(*CandidateLogPath);
			if (FileSize <= 0 || FileSize > 8 * 1024 * 1024)
			{
				continue;
			}

			FDateTime ModifiedAt;
			if (!IsExternalRuntimeProofLogCurrentForPlan(CandidateLogPath, Plan, ModifiedAt))
			{
				continue;
			}

			FString LogText;
			if (!FFileHelper::LoadFileToString(LogText, *CandidateLogPath))
			{
				continue;
			}

			FOsvayderUECloseoutFactSnapshot CandidateFacts;
			const FString FactPrefix =
				TEXT("external_runtime_proof_log.")
				+ MakeCloseoutSafeFileTag(CleanFileName);
			ExtractCloseoutAutomationFactsFromText(LogText, CandidateFacts, FactPrefix);
			if (!CandidateFacts.HasBoundedPrisonAccessAutomationProof()
				|| !CandidateFacts.bProofInputMappingAvailable
				|| !CandidateFacts.bKnownProofMapAvailable
				|| !CandidateFacts.bPlacedRuntimeActorsAvailable
				|| !CandidateFacts.bRuntimeSmokeSuccessObserved
				|| !CandidateFacts.bPrisonAccessEventObserved)
			{
				continue;
			}

			MergeProjectLocalInteractionAccessSourceFacts(
				ProjectRoot,
				CandidateFacts,
				FactPrefix + TEXT(".project_source"));
			CandidateFacts.AddFactId(FactPrefix + TEXT(".accepted_current_prefix_external_log"));
			Context.Facts.MergeFrom(CandidateFacts);
			NoteExternalRuntimeProofLogTimestamp(Context, ModifiedAt);
		}
	}

	void ExtractCloseoutSourceAndRuntimeFactsFromText(
		const FString& ToolName,
		const FString& EvidenceText,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		const FString LowerToolName = ToolName.ToLower();
		const FString Lower = EvidenceText.ToLower();
		FString NormalizedLower = Lower;
		NormalizedLower.ReplaceInline(TEXT("\\"), TEXT("/"));
		const bool bAttemptResolverPathObserved =
			NormalizedLower.Contains(TEXT("source/alternative/prisonaccess/alternativeprisonaccessattemptresolver.cpp"))
			|| NormalizedLower.Contains(TEXT("source/alternative/prisonaccess/alternativeprisonaccessattemptresolver.h"))
			|| NormalizedLower.Contains(TEXT("source/alternative/prisonaccess/alternativeprisonaccessattemptresolver.*"));
		const bool bEventSubsystemPathObserved =
			NormalizedLower.Contains(TEXT("source/alternative/prisonaccess/alternativeprisonaccesseventsubsystem.cpp"))
			|| NormalizedLower.Contains(TEXT("source/alternative/prisonaccess/alternativeprisonaccesseventsubsystem.h"))
			|| NormalizedLower.Contains(TEXT("source/alternative/prisonaccess/alternativeprisonaccesseventsubsystem.*"));
		const bool bSourceInspectionSummary =
			Lower.Contains(TEXT("source inspection confirmed"))
			|| Lower.Contains(TEXT("source paths are present"))
			|| NormalizedLower.Contains(TEXT("source/alternative/prisonaccess resolver and event subsystem"));
		if (bAttemptResolverPathObserved
			&& (bSourceInspectionSummary
				|| Lower.Contains(TEXT("falternativeprisonaccessattemptresolver"))
				|| Lower.Contains(TEXT("resolvedoorattempt"))
				|| Lower.Contains(TEXT("resolvetechnicalboxattempt"))
				|| Lower.Contains(TEXT("falternativeprisonaccessattemptoutcome"))))
		{
			Facts.bAttemptResolverSourceObserved = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".attempt_resolver_source"));
		}
		if (bEventSubsystemPathObserved
			&& (bSourceInspectionSummary
				|| Lower.Contains(TEXT("ualternativeprisonaccesseventsubsystem"))
				|| Lower.Contains(TEXT("falternativeprisonaccesseventrecord"))
				|| Lower.Contains(TEXT("recordevent"))
				|| Lower.Contains(TEXT("prisonaccessevent order="))))
		{
			Facts.bEventSubsystemSourceObserved = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".event_subsystem_source"));
		}
		if (ContainsProofInputMappingEvidence(Lower))
		{
			Facts.bProofInputMappingAvailable = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".proof_input_mapping"));
		}

		const bool bUnrealRuntimeTool =
			LowerToolName.Contains(TEXT("open_level"))
			|| LowerToolName.Contains(TEXT("get_level_actors"))
			|| LowerToolName.Contains(TEXT("capture_viewport"))
			|| LowerToolName.Contains(TEXT("map_runtime_proof"));
		if (bUnrealRuntimeTool
			&& Lower.Contains(TEXT("lvl_prisonaccessproof")))
		{
			Facts.bKnownProofMapAvailable = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".known_proof_map"));
		}
		if (bUnrealRuntimeTool
			&& (Lower.Contains(TEXT("bp_prisonaccessproof"))
				|| Lower.Contains(TEXT("prisonaccessproofactor"))
				|| Lower.Contains(TEXT("prison access proof actor"))
				|| Lower.Contains(TEXT("aprisonaccess"))))
		{
			Facts.bPlacedRuntimeActorsAvailable = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".placed_runtime_actors"));
		}

		ExtractCloseoutAutomationFactsFromText(EvidenceText, Facts, FactPrefix);
		ExtractReferencedAutomationLogFactsFromText(EvidenceText, Facts, FactPrefix);
	}

	void MergeCloseoutWorkflowFacts(
		const FAgentFeatureWorkflowState& Workflow,
		FOsvayderUECloseoutFactSnapshot& Facts,
		const FString& FactPrefix)
	{
		if (Workflow.bKnownProofMapAvailable)
		{
			Facts.bKnownProofMapAvailable = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".known_proof_map_available"));
		}
		if (Workflow.InteractionAccessReuseObservation.HasSufficientReuseEvidence())
		{
			Facts.bProofInputMappingAvailable = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".proof_input_mapping_available"));
		}
		if (Workflow.bPlacedRuntimeActorsAvailable)
		{
			Facts.bPlacedRuntimeActorsAvailable = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".placed_runtime_actors_available"));
		}
		if (Workflow.bReducedProofModeAllowed)
		{
			Facts.bReducedProofModeAllowed = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".reduced_proof_mode_allowed"));
		}
		if (Workflow.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved)
		{
			Facts.bAttemptResolverSourceObserved = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".attempt_resolver_source_observed"));
		}
		if (Workflow.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved)
		{
			Facts.bEventSubsystemSourceObserved = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".event_subsystem_source_observed"));
		}
		if (Workflow.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved)
		{
			Facts.bRuntimeSmokeSuccessObserved = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".runtime_smoke_success_observed"));
		}
		if (Workflow.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved)
		{
			Facts.bPrisonAccessEventObserved = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".prison_access_event_observed"));
		}
		if (Workflow.AutomationDiscoveryCount != INDEX_NONE)
		{
			MergeCloseoutCount(Facts.AutomationDiscoveryCount, Workflow.AutomationDiscoveryCount);
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".automation_discovery_count"));
		}
		if (Workflow.AutomationExecutedCount != INDEX_NONE)
		{
			MergeCloseoutCount(Facts.AutomationExecutedCount, Workflow.AutomationExecutedCount);
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".automation_executed_count"));
		}
		if (Workflow.AutomationPassedCount != INDEX_NONE)
		{
			MergeCloseoutCount(Facts.AutomationPassedCount, Workflow.AutomationPassedCount);
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".automation_passed_count"));
		}
		if (Workflow.AutomationFailedCount != INDEX_NONE)
		{
			MergeCloseoutCount(Facts.AutomationFailedCount, Workflow.AutomationFailedCount);
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".automation_failed_count"));
		}
		if (Workflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase))
		{
			Facts.bRuntimeProofPassed = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".runtime_proof_state_passed"));
		}
		if (Workflow.CompileProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase)
			|| Workflow.CompileProofState.Equals(TEXT("not_required"), ESearchCase::IgnoreCase))
		{
			Facts.bCompileProofPassed = true;
			AddCloseoutFactId(Facts, FactPrefix + TEXT(".compile_proof_state"));
		}
	}

	bool SaveCloseoutJsonObjectToFile(
		const TSharedPtr<FJsonObject>& JsonObject,
		const FString& FilePath,
		FString& OutError)
	{
		if (!JsonObject.IsValid())
		{
			OutError = TEXT("closeout_decision json object is invalid");
			return false;
		}

		const FString Directory = FPaths::GetPath(FilePath);
		if (!Directory.IsEmpty() && !IFileManager::Get().MakeDirectory(*Directory, true))
		{
			OutError = FString::Printf(TEXT("could not create closeout_decision directory: %s"), *Directory);
			return false;
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer))
		{
			OutError = TEXT("could not serialize closeout_decision json");
			return false;
		}
		Writer->Close();

		if (!FFileHelper::SaveStringToFile(
				JsonText,
				*FilePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("could not write closeout_decision artifact: %s"), *FilePath);
			return false;
		}

		return true;
	}

	bool SaveHeadlessAcceptanceReceiptObject(
		const TSharedPtr<FJsonObject>& JsonObject,
		const FString& FilePath,
		FString& OutError)
	{
		OutError.Reset();
		if (!JsonObject.IsValid())
		{
			OutError = TEXT("headless acceptance receipt json object is invalid");
			return false;
		}

		const FString Directory = FPaths::GetPath(FilePath);
		if (!IFileManager::Get().MakeDirectory(*Directory, true))
		{
			OutError = FString::Printf(TEXT("could not create headless acceptance receipt directory: %s"), *Directory);
			return false;
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer))
		{
			OutError = TEXT("could not serialize headless acceptance receipt json");
			return false;
		}

		if (!FFileHelper::SaveStringToFile(
			JsonText,
			*FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("could not write headless acceptance receipt artifact: %s"), *FilePath);
			return false;
		}

		return true;
	}

	FString GetHeadlessAcceptancePendingReceiptPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FOsvayderUERelayAgentManager::GetRelayRootDir(),
			TEXT("headless_acceptance_pending_receipt.json")));
	}

	bool SaveHeadlessAcceptancePendingReceiptObject(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FString SaveError;
		const bool bSaved = SaveHeadlessAcceptanceReceiptObject(
			JsonObject,
			GetHeadlessAcceptancePendingReceiptPath(),
			SaveError);
		if (!bSaved)
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to persist pending headless acceptance receipt: %s"), *SaveError);
		}
		return bSaved;
	}

	void DeleteHeadlessAcceptancePendingReceipt()
	{
		IFileManager::Get().Delete(*GetHeadlessAcceptancePendingReceiptPath(), false, true, true);
	}

	bool TryLoadPendingHeadlessAcceptanceReceiptContext(FOsvayderUEHeadlessAcceptanceReceiptContext& OutContext)
	{
		OutContext = FOsvayderUEHeadlessAcceptanceReceiptContext{};

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *GetHeadlessAcceptancePendingReceiptPath()))
		{
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}

		Root->TryGetStringField(TEXT("prompt_file"), OutContext.Request.PromptFile);
		Root->TryGetStringField(TEXT("prefix"), OutContext.Request.Prefix);
		Root->TryGetStringField(TEXT("output_dir"), OutContext.Request.OutputDir);
		Root->TryGetStringField(TEXT("dispatch_path"), OutContext.DispatchPath);
		Root->TryGetStringField(TEXT("receipt_path"), OutContext.ReceiptPath);
		Root->TryGetStringField(TEXT("prompt_hash"), OutContext.PromptHash);
		Root->TryGetStringField(TEXT("started_at_utc"), OutContext.StartedAtUtc);
		Root->TryGetBoolField(TEXT("request_editor_exit_on_complete"), OutContext.Request.bRequestEditorExitOnComplete);
		Root->TryGetBoolField(TEXT("visible_manual_emulator"), OutContext.Request.bVisibleManualEmulator);
		Root->TryGetBoolField(TEXT("visible_unreal_required"), OutContext.Request.bRequireVisibleEditor);

		double TimeoutSec = 0.0;
		if (Root->TryGetNumberField(TEXT("timeout_sec"), TimeoutSec))
		{
			OutContext.Request.TimeoutSec = static_cast<int32>(TimeoutSec);
		}
		if (!OutContext.DispatchPath.IsEmpty())
		{
			OutContext.Request.TriggerPath = OutContext.DispatchPath;
		}
		OutContext.Request.bLocalDevOptIn = true;

		if (OutContext.ReceiptPath.IsEmpty() || OutContext.Request.Prefix.IsEmpty() || OutContext.StartedAtUtc.IsEmpty())
		{
			return false;
		}

		return true;
	}

	bool TryCompleteRecoveredHeadlessAcceptanceReceipt(
		const FString& ResponsePreview,
		const bool bAssistantSuccess,
		const FOsvayderUEActivePlanCloseoutDecision& CloseoutDecision,
		bool& bOutRequestEditorExit,
		FString& OutError)
	{
		bOutRequestEditorExit = false;
		OutError.Reset();

		FOsvayderUEHeadlessAcceptanceReceiptContext Context;
		if (!TryLoadPendingHeadlessAcceptanceReceiptContext(Context))
		{
			return false;
		}

		Context.CompletedAtUtc = MakeUtcNowText();
		Context.Status = TEXT("completed");
		Context.bAssistantSuccess = bAssistantSuccess;
		Context.bHasCloseoutDecision = true;
		Context.CloseoutDecision = CloseoutDecision;
		Context.ActiveRunIdFallback =
			FOsvayderUEAgentTraceLog::Get().GetActiveRunIdForBackend(FOsvayderCodeSubsystem::Get().GetConfiguredBackend());
		Context.LatestCloseoutPath =
			FPaths::Combine(FOsvayderUERelayAgentManager::GetRelayRootDir(), TEXT("closeout_decision.json"));
		Context.ArchivePath = CloseoutDecision.SourceArchivePath;
		Context.VisibleSessionPath = FOsvayderCodeSubsystem::Get().GetSessionFilePath();
		Context.TracePath = FOsvayderUEAgentTraceLog::Get().GetTraceLogPath();
		FDateTime StartedAtUtcDateTime = FDateTime::MinValue();
		FDateTime::ParseIso8601(*Context.StartedAtUtc, StartedAtUtcDateTime);
		Context.LogPaths = CollectCurrentPrefixLogPaths(Context.Request.Prefix, StartedAtUtcDateTime);

		TSharedPtr<FJsonObject> Receipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
		Receipt->SetStringField(TEXT("response_preview"), ResponsePreview.Left(1000));
		if (!SaveHeadlessAcceptanceReceiptObject(Receipt, Context.ReceiptPath, OutError))
		{
			return false;
		}

		DeleteHeadlessAcceptancePendingReceipt();
		bOutRequestEditorExit = Context.Request.bRequestEditorExitOnComplete;
		return true;
	}

	bool TryLoadHeadlessAcceptanceReceiptStartedAt(
		const FString& ReceiptPath,
		FString& OutStartedAtUtc,
		FDateTime& OutStartedAtDateTime)
	{
		OutStartedAtUtc.Reset();
		OutStartedAtDateTime = FDateTime::MinValue();

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *ReceiptPath))
		{
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}

		if (!Root->TryGetStringField(TEXT("started_at_utc"), OutStartedAtUtc) || OutStartedAtUtc.IsEmpty())
		{
			return false;
		}

		return FDateTime::ParseIso8601(*OutStartedAtUtc, OutStartedAtDateTime);
	}

	void CopyCloseoutStringArrayField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* FieldName,
		TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Object.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text))
			{
				OutValues.Add(Text);
			}
		}
	}

	bool TryLoadLatestHeadlessCloseoutDecision(
		FOsvayderUEActivePlanCloseoutDecision& OutDecision,
		FString& OutRawJson)
	{
		OutDecision = FOsvayderUEActivePlanCloseoutDecision{};
		OutRawJson.Reset();

		const FString CloseoutPath =
			FPaths::Combine(FOsvayderUERelayAgentManager::GetRelayRootDir(), TEXT("closeout_decision.json"));
		if (!FFileHelper::LoadFileToString(OutRawJson, *CloseoutPath))
		{
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutRawJson);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* DecisionObjectPtr = nullptr;
		if (!Root->TryGetObjectField(TEXT("decision"), DecisionObjectPtr)
			|| DecisionObjectPtr == nullptr
			|| !DecisionObjectPtr->IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>& DecisionObject = *DecisionObjectPtr;

		DecisionObject->TryGetStringField(TEXT("plan_status"), OutDecision.PlanStatus);
		DecisionObject->TryGetStringField(TEXT("result_status"), OutDecision.ResultStatus);
		DecisionObject->TryGetStringField(TEXT("gate_reason_code"), OutDecision.GateReasonCode);
		DecisionObject->TryGetStringField(TEXT("source_plan_id"), OutDecision.SourcePlanId);
		DecisionObject->TryGetStringField(TEXT("source_run_id"), OutDecision.SourceRunId);
		DecisionObject->TryGetStringField(TEXT("source_feature_workflow_id"), OutDecision.SourceFeatureWorkflowId);
		DecisionObject->TryGetStringField(TEXT("recipe_id"), OutDecision.SourceRecipeId);
		DecisionObject->TryGetStringField(TEXT("role_id"), OutDecision.SourceRoleId);
		double EvidenceSchemaVersion = 0.0;
		if (DecisionObject->TryGetNumberField(TEXT("evidence_schema_version"), EvidenceSchemaVersion))
		{
			OutDecision.EvidenceSchemaVersion = static_cast<int32>(EvidenceSchemaVersion);
		}
		DecisionObject->TryGetBoolField(TEXT("feature_workflow_required"), OutDecision.bFeatureWorkflowRequired);
		DecisionObject->TryGetBoolField(TEXT("recipe_contract_resolved"), OutDecision.bRecipeContractResolved);
		DecisionObject->TryGetBoolField(TEXT("role_contract_resolved"), OutDecision.bRoleContractResolved);
		DecisionObject->TryGetBoolField(TEXT("runtime_proof_required"), OutDecision.bRuntimeProofRequired);
		DecisionObject->TryGetBoolField(TEXT("runtime_proof_passed"), OutDecision.bRuntimeProofPassed);
		DecisionObject->TryGetBoolField(TEXT("runtime_proof_failed"), OutDecision.bRuntimeProofFailed);
		DecisionObject->TryGetBoolField(TEXT("stop_loss_triggered"), OutDecision.bStopLossTriggered);
		DecisionObject->TryGetBoolField(TEXT("proof_prerequisites_missing"), OutDecision.bProofPrerequisitesMissing);
		DecisionObject->TryGetBoolField(TEXT("automation_discovery_failed"), OutDecision.bAutomationDiscoveryFailed);
		DecisionObject->TryGetBoolField(TEXT("authoring_lane_denied"), OutDecision.bAuthoringLaneDenied);
		DecisionObject->TryGetBoolField(TEXT("compile_proof_required"), OutDecision.bCompileProofRequired);
		DecisionObject->TryGetBoolField(TEXT("fresh_compile_proof_observed"), OutDecision.bFreshCompileProofObserved);
		DecisionObject->TryGetBoolField(TEXT("fresh_compile_failure_observed"), OutDecision.bFreshCompileFailureObserved);
		DecisionObject->TryGetBoolField(
			TEXT("current_closed_editor_proof_observed"),
			OutDecision.bCurrentClosedEditorProofObserved);
		DecisionObject->TryGetBoolField(TEXT("post_compile_verification_passed"), OutDecision.bPostCompileVerificationPassed);
		DecisionObject->TryGetBoolField(TEXT("post_compile_verification_failed"), OutDecision.bPostCompileVerificationFailed);
		DecisionObject->TryGetBoolField(
			TEXT("managed_state_manual_write_detected"),
			OutDecision.bManagedStateManualWriteDetected);
		DecisionObject->TryGetStringField(TEXT("stop_loss_reason"), OutDecision.StopLossReason);
		DecisionObject->TryGetStringField(TEXT("blocker_family"), OutDecision.BlockerFamily);
		DecisionObject->TryGetStringField(TEXT("blocker_detail"), OutDecision.BlockerDetail);
		CopyCloseoutStringArrayField(DecisionObject, TEXT("consumed_fact_ids"), OutDecision.ConsumedFactIds);
		CopyCloseoutStringArrayField(
			DecisionObject,
			TEXT("missing_recipe_obligations"),
			OutDecision.MissingRecipeObligations);

		Root->TryGetStringField(TEXT("source_archive_path"), OutDecision.SourceArchivePath);
		Root->TryGetStringField(TEXT("source_project_root"), OutDecision.SourceProjectRoot);
		if (OutDecision.SourcePlanId.IsEmpty())
		{
			Root->TryGetStringField(TEXT("source_plan_id"), OutDecision.SourcePlanId);
		}
		if (OutDecision.SourceRunId.IsEmpty())
		{
			Root->TryGetStringField(TEXT("source_run_id"), OutDecision.SourceRunId);
		}
		if (OutDecision.SourceFeatureWorkflowId.IsEmpty())
		{
			Root->TryGetStringField(TEXT("source_feature_workflow_id"), OutDecision.SourceFeatureWorkflowId);
		}
		if (OutDecision.SourceRecipeId.IsEmpty())
		{
			Root->TryGetStringField(TEXT("recipe_id"), OutDecision.SourceRecipeId);
		}
		if (OutDecision.SourceRoleId.IsEmpty())
		{
			Root->TryGetStringField(TEXT("role_id"), OutDecision.SourceRoleId);
		}
		if (OutDecision.EvidenceSchemaVersion == 0)
		{
			EvidenceSchemaVersion = 0.0;
			if (Root->TryGetNumberField(TEXT("evidence_schema_version"), EvidenceSchemaVersion))
			{
				OutDecision.EvidenceSchemaVersion = static_cast<int32>(EvidenceSchemaVersion);
			}
		}

		return true;
	}

	TArray<FString> CollectCurrentPrefixLogPaths(const FString& Prefix, const FDateTime& StartedAtUtc)
	{
		TArray<FString> Results;
		if (Prefix.IsEmpty())
		{
			return Results;
		}

		const FString LogsDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"));
		const FString SafePrefix = MakeCloseoutSafeFileTag(Prefix);
		const FString Wildcard = FPaths::Combine(LogsDir, SafePrefix + TEXT("*"));
		TArray<FString> FileNames;
		IFileManager::Get().FindFiles(FileNames, *Wildcard, true, false);

		const FDateTime MinimumTimestamp = StartedAtUtc == FDateTime::MinValue()
			? FDateTime::MinValue()
			: StartedAtUtc - FTimespan::FromSeconds(1);
		for (const FString& FileName : FileNames)
		{
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(LogsDir, FileName));
			const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*Path);
			if (Timestamp >= MinimumTimestamp)
			{
				Results.Add(Path);
			}
		}

		Results.Sort();
		return Results;
	}

	FString FindActivePlanArchivePath(const TArray<FString>& ArchivedPaths)
	{
		for (const FString& ArchivedPath : ArchivedPaths)
		{
			if (ArchivedPath.EndsWith(TEXT(".active_plan.json"), ESearchCase::IgnoreCase))
			{
				return ArchivedPath;
			}
		}
		return FString();
	}

	FString MakeCloseoutSafeFileTag(const FString& Value)
	{
		FString Safe;
		Safe.Reserve(Value.Len());
		for (const TCHAR Ch : Value)
		{
			if (FChar::IsAlnum(Ch) || Ch == TEXT('-') || Ch == TEXT('_') || Ch == TEXT('.'))
			{
				Safe.AppendChar(Ch);
			}
			else
			{
				Safe.AppendChar(TEXT('_'));
			}
		}
		Safe.ReplaceInline(TEXT("__"), TEXT("_"));
		return Safe.TrimStartAndEnd();
	}

	struct FExplicitContinuityPromptRefs
	{
		FString PlanId;
		FString FeatureWorkflowId;
		FString RunId;

		bool HasAny() const
		{
			return !PlanId.IsEmpty() || !FeatureWorkflowId.IsEmpty() || !RunId.IsEmpty();
		}

		bool HasBindablePlanIdentity() const
		{
			return !PlanId.IsEmpty() || !FeatureWorkflowId.IsEmpty();
		}
	};

	bool IsReservedContinuitySchemaLabel(const FString& Token)
	{
		return Token.Equals(TEXT("run_id"), ESearchCase::IgnoreCase)
			|| Token.Equals(TEXT("plan_id"), ESearchCase::IgnoreCase)
			|| Token.Equals(TEXT("feature_id"), ESearchCase::IgnoreCase)
			|| Token.Equals(TEXT("feature_workflow_id"), ESearchCase::IgnoreCase);
	}

	bool TryExtractFirstContinuityToken(const FString& Text, const FString& PatternText, FString& OutToken)
	{
		const FRegexPattern Pattern(PatternText);
		FRegexMatcher Matcher(Pattern, Text);
		while (Matcher.FindNext())
		{
			const FString Token = Matcher.GetCaptureGroup(0);
			if (!Token.IsEmpty() && !IsReservedContinuitySchemaLabel(Token))
			{
				OutToken = Token;
				return true;
			}
		}

		OutToken.Reset();
		return false;
	}

	FExplicitContinuityPromptRefs ExtractExplicitContinuityPromptRefs(const FString& Prompt)
	{
		FExplicitContinuityPromptRefs Refs;
		TryExtractFirstContinuityToken(Prompt, TEXT("\\bplan_[A-Za-z0-9_]+\\b"), Refs.PlanId);
		TryExtractFirstContinuityToken(Prompt, TEXT("\\bfeature_[A-Za-z0-9_]+\\b"), Refs.FeatureWorkflowId);
		TryExtractFirstContinuityToken(Prompt, TEXT("\\brun_[A-Za-z0-9_]+\\b"), Refs.RunId);
		return Refs;
	}

	bool IsContinuityPlanTerminal(const FOsvayderUEActivePlan& Plan)
	{
		const auto IsTerminal = [](const FString& Value)
		{
			return Value.Equals(TEXT("done"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("failed"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("cancelled"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("abandoned_for_fresh_session"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("abandoned_by_user"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("achieved_fully"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("not_achieved"), ESearchCase::IgnoreCase);
		};
		return IsTerminal(Plan.Status) || IsTerminal(Plan.ResultStatus);
	}

	bool DoesPlanMatchExplicitContinuityRefs(
		const FOsvayderUEActivePlan& Plan,
		const FExplicitContinuityPromptRefs& Refs)
	{
		if (!Refs.PlanId.IsEmpty()
			&& !Plan.PlanId.Equals(Refs.PlanId, ESearchCase::CaseSensitive))
		{
			return false;
		}
		if (!Refs.FeatureWorkflowId.IsEmpty()
			&& !Plan.FeatureWorkflow.FeatureWorkflowId.Equals(Refs.FeatureWorkflowId, ESearchCase::CaseSensitive))
		{
			return false;
		}
		return Refs.HasBindablePlanIdentity() && !IsContinuityPlanTerminal(Plan);
	}

	FString DescribeExplicitContinuityRefs(const FExplicitContinuityPromptRefs& Refs)
	{
		TArray<FString> Parts;
		if (!Refs.RunId.IsEmpty())
		{
			Parts.Add(TEXT("run_id=") + Refs.RunId);
		}
		if (!Refs.PlanId.IsEmpty())
		{
			Parts.Add(TEXT("plan_id=") + Refs.PlanId);
		}
		if (!Refs.FeatureWorkflowId.IsEmpty())
		{
			Parts.Add(TEXT("feature_workflow_id=") + Refs.FeatureWorkflowId);
		}
		return FString::Join(Parts, TEXT("; "));
	}

	bool TryLoadContinuityPlanSnapshot(
		const FExplicitContinuityPromptRefs& Refs,
		FOsvayderUEActivePlan& OutPlan,
		FString& OutSourcePath)
	{
		FOsvayderUEActivePlan ActivePlan;
		FString LoadError;
		if (FOsvayderUERelayAgentManager::LoadActivePlan(ActivePlan, LoadError)
			&& DoesPlanMatchExplicitContinuityRefs(ActivePlan, Refs))
		{
			OutPlan = ActivePlan;
			OutSourcePath = FOsvayderUERelayAgentManager::GetActivePlanPath();
			return true;
		}

		const FString ArchiveDir = FOsvayderUERelayAgentManager::GetPlanArchiveDir();
		if (!IFileManager::Get().DirectoryExists(*ArchiveDir))
		{
			return false;
		}

		TArray<FString> ArchivePaths;
		IFileManager::Get().FindFilesRecursive(
			ArchivePaths,
			*ArchiveDir,
			TEXT("*.active_plan.json"),
			true,
			false,
			false);
		ArchivePaths.Sort([](const FString& A, const FString& B)
		{
			return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
		});

		for (const FString& ArchivePath : ArchivePaths)
		{
			FOsvayderUEActivePlan Candidate;
			FString CandidateError;
			if (!FOsvayderUERelayAgentManager::LoadPlanFromPath(ArchivePath, Candidate, CandidateError))
			{
				continue;
			}
			if (DoesPlanMatchExplicitContinuityRefs(Candidate, Refs))
			{
				OutPlan = Candidate;
				OutSourcePath = ArchivePath;
				return true;
			}
		}

		return false;
	}

	bool ArchiveActivePlanContinuitySnapshotForNewSession(FString& OutArchivePath, FString& OutError)
	{
		OutArchivePath.Reset();
		OutError.Reset();

		FOsvayderUEActivePlan ActivePlan;
		FString LoadError;
		if (!FOsvayderUERelayAgentManager::LoadActivePlan(ActivePlan, LoadError))
		{
			return false;
		}
		if (ActivePlan.PlanId.IsEmpty())
		{
			OutError = TEXT("active_plan_continuity_backup_missing_plan_id");
			return false;
		}

		const FString ArchiveDir = FOsvayderUERelayAgentManager::GetPlanArchiveDir();
		IFileManager::Get().MakeDirectory(*ArchiveDir, true);
		const FString ArchivePrefix = MakeCloseoutSafeFileTag(FString::Printf(
			TEXT("%s-new_session_continuity_backup-%s"),
			*FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")),
			*ActivePlan.PlanId));
		OutArchivePath = FPaths::Combine(ArchiveDir, ArchivePrefix + TEXT(".active_plan.json"));
		return FOsvayderUERelayAgentManager::SavePlanToPath(OutArchivePath, ActivePlan, OutError);
	}

	void AppendContinuationPromptToPlanIfRelevant(FOsvayderUEActivePlan& Plan, const FString& Prompt)
	{
		const FString TrimmedPrompt = Prompt.TrimStartAndEnd();
		if (TrimmedPrompt.IsEmpty())
		{
			return;
		}

		const FString LowerPrompt = TrimmedPrompt.ToLower();
		const bool bCarriesRuntimeProofBinding =
			LowerPrompt.Contains(TEXT("prefix"))
			|| LowerPrompt.Contains(TEXT("saved/logs"))
			|| LowerPrompt.Contains(TEXT("automation"))
			|| LowerPrompt.Contains(TEXT(".log"))
			|| ExtractExplicitContinuityPromptRefs(TrimmedPrompt).HasAny();
		if (!bCarriesRuntimeProofBinding
			|| Plan.OriginalUserTask.Contains(*TrimmedPrompt, ESearchCase::CaseSensitive))
		{
			return;
		}

		Plan.OriginalUserTask += TEXT("\n\nContinuation prompt:\n");
		Plan.OriginalUserTask += TrimmedPrompt.Left(6000);
	}

	bool TryRestoreExplicitContinuityPlanForPrompt(
		const FString& Prompt,
		FOsvayderUEActivePlan& OutPlan,
		FString& OutError)
	{
		OutError.Reset();
		const FExplicitContinuityPromptRefs Refs = ExtractExplicitContinuityPromptRefs(Prompt);
		if (!Refs.HasAny())
		{
			return false;
		}

		if (!Refs.HasBindablePlanIdentity())
		{
			OutError = FString::Printf(
				TEXT("continuity_mismatch_unbindable_reference: prompt cited %s but did not include a plan_id or feature_workflow_id. Refusing to create a replacement feature workflow."),
				*DescribeExplicitContinuityRefs(Refs));
			return false;
		}

		FString SourcePath;
		if (!TryLoadContinuityPlanSnapshot(Refs, OutPlan, SourcePath))
		{
			OutError = FString::Printf(
				TEXT("continuity_mismatch_no_matching_plan_snapshot: prompt cited %s but no active or archived non-terminal active_plan snapshot matched. Refusing to create a replacement feature workflow."),
				*DescribeExplicitContinuityRefs(Refs));
			return false;
		}

		OutPlan.Status = TEXT("active");
		OutPlan.ResultStatus = TEXT("incomplete");
		OutPlan.UpdatedAtUtc = MakeUtcNowText();
		OutPlan.TechnicalDetail = FString::Printf(
			TEXT("Restored explicit continuity plan from %s; replacement feature scope creation is disabled for this prompt."),
			*SourcePath);
		OutPlan.CurrentTechnicalDetail = OutPlan.TechnicalDetail;
		OutPlan.LaneState.ContinuityPlanId = OutPlan.PlanId;
		OutPlan.LaneState.ContinuityWorkflowId = OutPlan.FeatureWorkflow.FeatureWorkflowId;
		OutPlan.LaneState.ContinuationIntent =
			TEXT("Resume explicitly cited continuity plan; do not create a replacement feature workflow.");
		return true;
	}

	FString BuildRunIsolatedCloseoutArchiveRunTag(const FOsvayderUERunCloseoutContext& Context)
	{
		FString Timestamp = !Context.CompletedAtUtc.IsEmpty() ? Context.CompletedAtUtc : Context.StartedAtUtc;
		FString CompactTimestamp;
		for (const TCHAR Ch : Timestamp)
		{
			if (FChar::IsDigit(Ch))
			{
				CompactTimestamp.AppendChar(Ch);
			}
		}
		if (CompactTimestamp.Len() > 14)
		{
			CompactTimestamp = CompactTimestamp.Left(14);
		}
		if (CompactTimestamp.IsEmpty())
		{
			CompactTimestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
		}

		const FString RunTag = Context.RunId.IsEmpty() ? TEXT("run_unknown") : Context.RunId;
		return MakeCloseoutSafeFileTag(FString::Printf(TEXT("%s-%s"), *CompactTimestamp, *RunTag));
	}

	FString GetCloseoutProjectRoot()
	{
		FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectRoot);
		return ProjectRoot;
	}

	FString NormalizeCloseoutIdentityPath(const FString& InPath)
	{
		FString Normalized = InPath.TrimStartAndEnd();
		if (Normalized.IsEmpty())
		{
			return Normalized;
		}
		Normalized = FPaths::ConvertRelativePathToFull(Normalized);
		FPaths::NormalizeDirectoryName(Normalized);
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		Normalized.RemoveFromEnd(TEXT("/"), ESearchCase::CaseSensitive);
		return Normalized.ToLower();
	}

	bool TryGetTracePayloadObject(
		const TSharedPtr<FJsonObject>& Record,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		const TSharedPtr<FJsonObject>* PayloadPtr = nullptr;
		if (Record.IsValid()
			&& Record->TryGetObjectField(TEXT("payload"), PayloadPtr)
			&& PayloadPtr
			&& PayloadPtr->IsValid())
		{
			OutPayload = *PayloadPtr;
			return true;
		}
		return false;
	}

	FString GetTraceRecordEventId(const TSharedPtr<FJsonObject>& Record)
	{
		FString EventId;
		if (Record.IsValid())
		{
			Record->TryGetStringField(TEXT("event_id"), EventId);
		}
		return EventId.IsEmpty() ? FString(TEXT("trace_record")) : EventId;
	}

	bool TryGetStringFieldFromRecordOrPayload(
		const TSharedPtr<FJsonObject>& Record,
		const FString& FieldName,
		FString& OutValue)
	{
		if (Record.IsValid() && Record->TryGetStringField(FieldName, OutValue) && !OutValue.IsEmpty())
		{
			return true;
		}

		TSharedPtr<FJsonObject> Payload;
		if (TryGetTracePayloadObject(Record, Payload)
			&& Payload->TryGetStringField(FieldName, OutValue)
			&& !OutValue.IsEmpty())
		{
			return true;
		}
		return false;
	}

	FString GetTraceRecordPlanId(const TSharedPtr<FJsonObject>& Record)
	{
		FString PlanId;
		if (TryGetStringFieldFromRecordOrPayload(Record, TEXT("plan_id"), PlanId)
			|| TryGetStringFieldFromRecordOrPayload(Record, TEXT("source_plan_id"), PlanId)
			|| TryGetStringFieldFromRecordOrPayload(Record, TEXT("active_plan_id"), PlanId))
		{
			return PlanId.TrimStartAndEnd();
		}
		return FString();
	}

	FString GetTraceRecordFeatureWorkflowId(const TSharedPtr<FJsonObject>& Record)
	{
		FString WorkflowId;
		if (TryGetStringFieldFromRecordOrPayload(Record, TEXT("feature_workflow_id"), WorkflowId))
		{
			return WorkflowId.TrimStartAndEnd();
		}

		TSharedPtr<FJsonObject> Payload;
		if (!TryGetTracePayloadObject(Record, Payload))
		{
			return FString();
		}

		const TSharedPtr<FJsonObject>* WorkflowPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("feature_workflow"), WorkflowPtr)
			&& WorkflowPtr
			&& WorkflowPtr->IsValid()
			&& (*WorkflowPtr)->TryGetStringField(TEXT("feature_workflow_id"), WorkflowId)
			&& !WorkflowId.IsEmpty())
		{
			return WorkflowId.TrimStartAndEnd();
		}

		const TSharedPtr<FJsonObject>* CanonPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("canon_execution"), CanonPtr)
			&& CanonPtr
			&& CanonPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>* CanonWorkflowPtr = nullptr;
			if ((*CanonPtr)->TryGetObjectField(TEXT("feature_workflow"), CanonWorkflowPtr)
				&& CanonWorkflowPtr
				&& CanonWorkflowPtr->IsValid()
				&& (*CanonWorkflowPtr)->TryGetStringField(TEXT("feature_workflow_id"), WorkflowId)
				&& !WorkflowId.IsEmpty())
			{
				return WorkflowId.TrimStartAndEnd();
			}
		}

		return FString();
	}

	FString GetTraceRecordProjectRoot(const TSharedPtr<FJsonObject>& Record)
	{
		FString ProjectRoot;
		if (TryGetStringFieldFromRecordOrPayload(Record, TEXT("project_root"), ProjectRoot)
			|| TryGetStringFieldFromRecordOrPayload(Record, TEXT("working_directory"), ProjectRoot))
		{
			return ProjectRoot.TrimStartAndEnd();
		}
		return FString();
	}

	bool DoesTraceRecordMatchRunCloseoutContext(
		const TSharedPtr<FJsonObject>& Record,
		const FOsvayderUERunCloseoutContext& Context)
	{
		if (!Record.IsValid())
		{
			return false;
		}

		FString RecordRunId;
		Record->TryGetStringField(TEXT("run_id"), RecordRunId);
		RecordRunId = RecordRunId.TrimStartAndEnd();
		if (!Context.RunId.IsEmpty())
		{
			if (!RecordRunId.Equals(Context.RunId, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}

		const FString RecordPlanId = GetTraceRecordPlanId(Record);
		if (!Context.PlanId.IsEmpty()
			&& !RecordPlanId.IsEmpty()
			&& !RecordPlanId.Equals(Context.PlanId, ESearchCase::CaseSensitive))
		{
			return false;
		}

		const FString RecordWorkflowId = GetTraceRecordFeatureWorkflowId(Record);
		if (!Context.FeatureWorkflowId.IsEmpty()
			&& !RecordWorkflowId.IsEmpty()
			&& !RecordWorkflowId.Equals(Context.FeatureWorkflowId, ESearchCase::CaseSensitive))
		{
			return false;
		}

		const FString RecordProjectRoot = GetTraceRecordProjectRoot(Record);
		if (!Context.ProjectRoot.IsEmpty()
			&& !RecordProjectRoot.IsEmpty()
			&& NormalizeCloseoutIdentityPath(RecordProjectRoot) != NormalizeCloseoutIdentityPath(Context.ProjectRoot))
		{
			return false;
		}

		if (Context.RunId.IsEmpty())
		{
			const bool bExplicitPlanMatch = !Context.PlanId.IsEmpty()
				&& RecordPlanId.Equals(Context.PlanId, ESearchCase::CaseSensitive);
			const bool bExplicitWorkflowMatch = !Context.FeatureWorkflowId.IsEmpty()
				&& RecordWorkflowId.Equals(Context.FeatureWorkflowId, ESearchCase::CaseSensitive);
			return bExplicitPlanMatch || bExplicitWorkflowMatch;
		}

		return true;
	}

	void NoteCloseoutTraceTimestamp(
		FOsvayderUERunCloseoutContext& Context,
		const TSharedPtr<FJsonObject>& Record)
	{
		FString Timestamp;
		if (!Record.IsValid() || !Record->TryGetStringField(TEXT("timestamp"), Timestamp) || Timestamp.IsEmpty())
		{
			return;
		}
		if (Context.StartedAtUtc.IsEmpty() || IsIsoUtcAfter(Context.StartedAtUtc, Timestamp))
		{
			Context.StartedAtUtc = Timestamp;
		}
		if (Context.CompletedAtUtc.IsEmpty() || IsIsoUtcAfter(Timestamp, Context.CompletedAtUtc))
		{
			Context.CompletedAtUtc = Timestamp;
		}
	}
}

void FOsvayderUECloseoutFactSnapshot::AddFactId(const FString& FactId)
{
	if (!FactId.TrimStartAndEnd().IsEmpty())
	{
		ConsumedFactIds.AddUnique(FactId);
	}
}

void FOsvayderUECloseoutFactSnapshot::MergeFrom(const FOsvayderUECloseoutFactSnapshot& Other)
{
	bKnownProofMapAvailable = bKnownProofMapAvailable || Other.bKnownProofMapAvailable;
	bProofInputMappingAvailable = bProofInputMappingAvailable || Other.bProofInputMappingAvailable;
	bPlacedRuntimeActorsAvailable = bPlacedRuntimeActorsAvailable || Other.bPlacedRuntimeActorsAvailable;
	bReducedProofModeAllowed = bReducedProofModeAllowed || Other.bReducedProofModeAllowed;
	bAttemptResolverSourceObserved = bAttemptResolverSourceObserved || Other.bAttemptResolverSourceObserved;
	bEventSubsystemSourceObserved = bEventSubsystemSourceObserved || Other.bEventSubsystemSourceObserved;
	bRuntimeSmokeSuccessObserved = bRuntimeSmokeSuccessObserved || Other.bRuntimeSmokeSuccessObserved;
	bPrisonAccessEventObserved = bPrisonAccessEventObserved || Other.bPrisonAccessEventObserved;
	MergeCloseoutCount(AutomationDiscoveryCount, Other.AutomationDiscoveryCount);
	MergeCloseoutCount(AutomationExecutedCount, Other.AutomationExecutedCount);
	MergeCloseoutCount(AutomationPassedCount, Other.AutomationPassedCount);
	MergeCloseoutCount(AutomationFailedCount, Other.AutomationFailedCount);
	bRuntimeProofPassed = bRuntimeProofPassed || Other.bRuntimeProofPassed;
	bCompileProofPassed = bCompileProofPassed || Other.bCompileProofPassed;
	bCommandMutationDetected = bCommandMutationDetected || Other.bCommandMutationDetected;
	bManagedStateManualWriteDetected = bManagedStateManualWriteDetected || Other.bManagedStateManualWriteDetected;
	bLocalAnimationPackIntakeRequired = bLocalAnimationPackIntakeRequired || Other.bLocalAnimationPackIntakeRequired;
	bLocalAnimationPackIntakeSucceeded = bLocalAnimationPackIntakeSucceeded || Other.bLocalAnimationPackIntakeSucceeded;
	bAnimationPreflightObserved = bAnimationPreflightObserved || Other.bAnimationPreflightObserved;
	if (Other.AnimationWorkflowFactIds.Num() > 0 && Other.bAnimationPreflightSkeletonMismatchObserved)
	{
		bAnimationPreflightSkeletonMismatchObserved = true;
		bAnimationRetargetFixupSucceeded = Other.bAnimationRetargetFixupSucceeded;
		bPostRetargetCompatibleAnimationPreflightObserved = Other.bPostRetargetCompatibleAnimationPreflightObserved;
		CopyAnimationLineageState(*this, Other);
	}
	else
	{
		bAnimationPreflightSkeletonMismatchObserved =
			bAnimationPreflightSkeletonMismatchObserved || Other.bAnimationPreflightSkeletonMismatchObserved;
		bAnimationRetargetFixupSucceeded = bAnimationRetargetFixupSucceeded || Other.bAnimationRetargetFixupSucceeded;
		bPostRetargetCompatibleAnimationPreflightObserved =
			bPostRetargetCompatibleAnimationPreflightObserved || Other.bPostRetargetCompatibleAnimationPreflightObserved;
		MergeAnimationLineageState(*this, Other);
	}
	for (const FString& FactId : Other.ConsumedFactIds)
	{
		AddFactId(FactId);
	}
	for (const FString& FactId : Other.CommandMutationFactIds)
	{
		CommandMutationFactIds.AddUnique(FactId);
	}
	for (const FString& FactId : Other.ManagedStateManualWriteFactIds)
	{
		ManagedStateManualWriteFactIds.AddUnique(FactId);
	}
	for (const FString& FactId : Other.AnimationWorkflowFactIds)
	{
		AnimationWorkflowFactIds.AddUnique(FactId);
	}
	for (const FString& Summary : Other.ExecutionTruthDecisionSummaries)
	{
		ExecutionTruthDecisionSummaries.AddUnique(Summary);
	}
}

bool FOsvayderUECloseoutFactSnapshot::HasRuntimePrerequisiteFacts() const
{
	return bKnownProofMapAvailable
		|| bProofInputMappingAvailable
		|| bPlacedRuntimeActorsAvailable
		|| bReducedProofModeAllowed
		|| AutomationDiscoveryCount > 0;
}

bool FOsvayderUECloseoutFactSnapshot::HasCompleteInteractionAccessObservation() const
{
	return bAttemptResolverSourceObserved
		&& bEventSubsystemSourceObserved
		&& bRuntimeSmokeSuccessObserved
		&& bPrisonAccessEventObserved;
}

bool FOsvayderUECloseoutFactSnapshot::HasBoundedPrisonAccessAutomationProof() const
{
	return AutomationDiscoveryCount >= 7
		&& AutomationExecutedCount >= 7
		&& AutomationPassedCount >= 7
		&& AutomationFailedCount == 0;
}

bool FOsvayderUECloseoutFactSnapshot::HasBoundedInteractionAccessRuntimeProof() const
{
	const FOsvayderUERecipeEvidenceContract Contract =
		OsvayderUERecipeRegistry::GetInteractionAccessRecipeEvidenceContract();
	return FindMissingCloseoutFactRecipeObligations(Contract, *this).Num() == 0;
}

TSharedPtr<FJsonObject> FOsvayderUECloseoutFactSnapshot::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("known_proof_map_available"), bKnownProofMapAvailable);
	Object->SetBoolField(TEXT("proof_input_mapping_available"), bProofInputMappingAvailable);
	Object->SetBoolField(TEXT("placed_runtime_actors_available"), bPlacedRuntimeActorsAvailable);
	Object->SetBoolField(TEXT("reduced_proof_mode_allowed"), bReducedProofModeAllowed);
	Object->SetBoolField(TEXT("attempt_resolver_source_observed"), bAttemptResolverSourceObserved);
	Object->SetBoolField(TEXT("event_subsystem_source_observed"), bEventSubsystemSourceObserved);
	Object->SetBoolField(TEXT("runtime_smoke_success_observed"), bRuntimeSmokeSuccessObserved);
	Object->SetBoolField(TEXT("prison_access_event_observed"), bPrisonAccessEventObserved);
	Object->SetNumberField(TEXT("automation_discovery_count"), AutomationDiscoveryCount);
	Object->SetNumberField(TEXT("automation_executed_count"), AutomationExecutedCount);
	Object->SetNumberField(TEXT("automation_passed_count"), AutomationPassedCount);
	Object->SetNumberField(TEXT("automation_failed_count"), AutomationFailedCount);
	Object->SetBoolField(TEXT("runtime_proof_passed"), bRuntimeProofPassed);
	Object->SetBoolField(TEXT("compile_proof_passed"), bCompileProofPassed);
	Object->SetBoolField(TEXT("command_mutation_detected"), bCommandMutationDetected);
	Object->SetArrayField(
		TEXT("command_mutation_fact_ids"),
		MakeCloseoutStringArrayJson(CommandMutationFactIds));
	Object->SetBoolField(TEXT("managed_state_manual_write_detected"), bManagedStateManualWriteDetected);
	Object->SetArrayField(
		TEXT("managed_state_manual_write_fact_ids"),
		MakeCloseoutStringArrayJson(ManagedStateManualWriteFactIds));
	Object->SetBoolField(TEXT("local_animation_pack_intake_required"), bLocalAnimationPackIntakeRequired);
	Object->SetBoolField(TEXT("local_animation_pack_intake_succeeded"), bLocalAnimationPackIntakeSucceeded);
	Object->SetBoolField(TEXT("animation_preflight_observed"), bAnimationPreflightObserved);
	Object->SetBoolField(TEXT("animation_preflight_skeleton_mismatch_observed"), bAnimationPreflightSkeletonMismatchObserved);
	Object->SetBoolField(TEXT("animation_retarget_fixup_succeeded"), bAnimationRetargetFixupSucceeded);
	Object->SetBoolField(
		TEXT("post_retarget_compatible_animation_preflight_observed"),
		bPostRetargetCompatibleAnimationPreflightObserved);
	Object->SetBoolField(TEXT("animation_lineage_role_evidence_present"), bAnimationLineageRoleEvidencePresent);
	Object->SetBoolField(TEXT("animation_lineage_path_evidence_present"), bAnimationLineagePathEvidencePresent);
	Object->SetBoolField(TEXT("animation_lineage_path_check_applied"), bAnimationLineagePathCheckApplied);
	Object->SetArrayField(TEXT("animation_required_roles"), MakeCloseoutStringArrayJson(RequiredAnimationRoles));
	Object->SetArrayField(
		TEXT("animation_compatible_post_retarget_roles"),
		MakeCloseoutStringArrayJson(CompatiblePostRetargetAnimationRoles));
	Object->SetArrayField(TEXT("animation_missing_lineage_roles"), MakeCloseoutStringArrayJson(MissingAnimationLineageRoles));
	Object->SetArrayField(
		TEXT("animation_lineage_path_mismatch_roles"),
		MakeCloseoutStringArrayJson(AnimationLineagePathMismatchRoles));
	Object->SetArrayField(
		TEXT("animation_generated_retarget_destinations"),
		MakeCloseoutStringArrayJson(GeneratedRetargetDestinationPaths));
	Object->SetObjectField(
		TEXT("animation_required_role_source_paths"),
		MakeCloseoutStringMapJson(RequiredAnimationRoleSourcePaths));
	Object->SetObjectField(
		TEXT("animation_retarget_generated_destinations_by_source"),
		MakeCloseoutStringMapJson(RetargetGeneratedDestinationsBySource));
	Object->SetObjectField(
		TEXT("animation_retarget_generated_destinations_by_role"),
		MakeCloseoutStringMapJson(RetargetGeneratedDestinationsByRole));
	Object->SetObjectField(
		TEXT("animation_compatible_role_paths"),
		MakeCloseoutStringMapJson(CompatibleAnimationRolePaths));
	Object->SetArrayField(
		TEXT("animation_workflow_fact_ids"),
		MakeCloseoutStringArrayJson(AnimationWorkflowFactIds));
	Object->SetArrayField(
		TEXT("execution_truth_decisions"),
		MakeCloseoutStringArrayJson(ExecutionTruthDecisionSummaries));
	Object->SetBoolField(TEXT("runtime_prerequisite_facts"), HasRuntimePrerequisiteFacts());
	Object->SetBoolField(TEXT("complete_interaction_access_observation"), HasCompleteInteractionAccessObservation());
	Object->SetBoolField(TEXT("bounded_prison_access_automation_proof"), HasBoundedPrisonAccessAutomationProof());
	Object->SetBoolField(TEXT("bounded_interaction_access_runtime_proof"), HasBoundedInteractionAccessRuntimeProof());
	Object->SetArrayField(TEXT("consumed_fact_ids"), MakeCloseoutStringArrayJson(ConsumedFactIds));
	return Object;
}

#if WITH_DEV_AUTOMATION_TESTS
bool SOsvayderEditorWidget::PromptHasExplicitContinuityRefsForTests(const FString& Prompt)
{
	return ExtractExplicitContinuityPromptRefs(Prompt).HasAny();
}

FString SOsvayderEditorWidget::DescribeExplicitContinuityRefsForTests(const FString& Prompt)
{
	return DescribeExplicitContinuityRefs(ExtractExplicitContinuityPromptRefs(Prompt));
}
#endif

TSharedPtr<FJsonObject> FOsvayderUERunCloseoutContext::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("run_id"), RunId);
	Object->SetStringField(TEXT("plan_id"), PlanId);
	Object->SetStringField(TEXT("feature_workflow_id"), FeatureWorkflowId);
	Object->SetStringField(TEXT("recipe_id"), RecipeId);
	Object->SetStringField(TEXT("role_id"), RoleId);
	Object->SetNumberField(TEXT("evidence_schema_version"), EvidenceSchemaVersion);
	Object->SetStringField(TEXT("project_root"), ProjectRoot);
	Object->SetStringField(TEXT("started_at_utc"), StartedAtUtc);
	Object->SetStringField(TEXT("completed_at_utc"), CompletedAtUtc);
	Object->SetArrayField(TEXT("included_trace_event_ids"), MakeCloseoutStringArrayJson(IncludedTraceEventIds));
	Object->SetArrayField(TEXT("excluded_trace_event_ids"), MakeCloseoutStringArrayJson(ExcludedTraceEventIds));
	Object->SetObjectField(TEXT("facts"), Facts.ToJsonObject());
	return Object;
}

FOsvayderUECloseoutFactSnapshot SOsvayderEditorWidget::ExtractCloseoutFactsFromPlan(
	const FOsvayderUEActivePlan& Plan)
{
	FOsvayderUECloseoutFactSnapshot Facts;
	MergeCloseoutWorkflowFacts(Plan.FeatureWorkflow, Facts, TEXT("active_plan.feature_workflow"));
	if (PromptRequiresLocalAnimationPackIntake(Plan.OriginalUserTask))
	{
		MarkLocalAnimationPackIntakeRequired(
			Facts,
			TEXT("active_plan.original_user_task.local_animation_pack_intake_required"));
	}
	for (int32 ToolCallIndex = 0; ToolCallIndex < Plan.ToolCalls.Num(); ++ToolCallIndex)
	{
		const FOsvayderUEPlanToolCallEntry& ToolCall = Plan.ToolCalls[ToolCallIndex];
		const FString EvidenceText =
			ToolCall.ToolName + TEXT("\n")
			+ ToolCall.Status + TEXT("\n")
			+ ToolCall.ResultStatus + TEXT("\n")
			+ ToolCall.Summary + TEXT("\n")
			+ ToolCall.TechnicalDetail;
		ApplyAnimationWorkflowEvidence(
			EvidenceText,
			Facts,
			FString::Printf(TEXT("active_plan.tool_calls.%d"), ToolCallIndex));
	}
	if (Plan.CompileProof.LastCompileProofOutcome.Equals(TEXT("success"), ESearchCase::IgnoreCase)
		|| Plan.CompileProof.LastCompileProofOutcome.Equals(TEXT("not_required"), ESearchCase::IgnoreCase))
	{
		Facts.bCompileProofPassed = true;
		AddCloseoutFactId(Facts, TEXT("active_plan.compile_proof"));
	}
	return Facts;
}

FOsvayderUECloseoutFactSnapshot SOsvayderEditorWidget::ExtractAgentTraceFactsForCloseout(
	const TArray<TSharedPtr<FJsonObject>>& TraceRecords)
{
	FOsvayderUECloseoutFactSnapshot Facts;
	for (const TSharedPtr<FJsonObject>& Record : TraceRecords)
	{
		if (!Record.IsValid())
		{
			continue;
		}

		FString EventId;
		Record->TryGetStringField(TEXT("event_id"), EventId);
		if (EventId.IsEmpty())
		{
			EventId = TEXT("trace_record");
		}
		FString EventType;
		Record->TryGetStringField(TEXT("event_type"), EventType);
		FString RecordRunId;
		Record->TryGetStringField(TEXT("run_id"), RecordRunId);
		const FString FactPrefix = FString::Printf(TEXT("agent_trace.%s"), *EventId);

		const TSharedPtr<FJsonObject>* PayloadPtr = nullptr;
		if (!Record->TryGetObjectField(TEXT("payload"), PayloadPtr) || !PayloadPtr || !PayloadPtr->IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& Payload = *PayloadPtr;

		FString PromptText;
		if (Payload->TryGetStringField(TEXT("prompt_text"), PromptText)
			&& PromptRequiresLocalAnimationPackIntake(PromptText))
		{
			MarkLocalAnimationPackIntakeRequired(Facts, FactPrefix + TEXT(".local_animation_pack_intake_required"));
		}

		const TSharedPtr<FJsonObject>* FeatureWorkflowPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("feature_workflow"), FeatureWorkflowPtr)
			&& FeatureWorkflowPtr
			&& FeatureWorkflowPtr->IsValid())
		{
			MergeCloseoutWorkflowFacts(
				FAgentFeatureWorkflowState::FromJsonObject(*FeatureWorkflowPtr),
				Facts,
				FactPrefix + TEXT(".feature_workflow"));
		}
		const TArray<TSharedPtr<FJsonValue>>* MandatoryAnimationSteps = nullptr;
		if (Payload->TryGetArrayField(TEXT("mandatory_animation_workflow_steps"), MandatoryAnimationSteps)
			&& MandatoryAnimationSteps
			&& MandatoryAnimationSteps->Num() > 0)
		{
			FAgentCanonExecution CanonExecution;
			CanonExecution.MandatoryAnimationWorkflowSteps = ReadCloseoutStringArray(*MandatoryAnimationSteps);
			const TArray<TSharedPtr<FJsonValue>>* ConditionalAnimationSteps = nullptr;
			if (Payload->TryGetArrayField(TEXT("conditional_mandatory_animation_workflow_steps"), ConditionalAnimationSteps)
				&& ConditionalAnimationSteps)
			{
				CanonExecution.ConditionalMandatoryAnimationWorkflowSteps = ReadCloseoutStringArray(*ConditionalAnimationSteps);
			}
			const TArray<TSharedPtr<FJsonValue>>* CloseoutGateReasons = nullptr;
			if (Payload->TryGetArrayField(TEXT("closeout_gate_reason_codes"), CloseoutGateReasons)
				&& CloseoutGateReasons)
			{
				CanonExecution.CloseoutGateReasonCodes = ReadCloseoutStringArray(*CloseoutGateReasons);
			}
			MergeCanonAnimationWorkflowFacts(CanonExecution, Facts, FactPrefix + TEXT(".canon_execution"));
		}

		FString ToolName;
		FString ToolInput;
		FString ToolResult;
		FString RawProviderEvent;
		if (!Payload->TryGetStringField(TEXT("tool_name"), ToolName) || ToolName.IsEmpty())
		{
			if (!Payload->TryGetStringField(TEXT("tool"), ToolName) || ToolName.IsEmpty())
			{
				Payload->TryGetStringField(TEXT("name"), ToolName);
			}
		}
		Payload->TryGetStringField(TEXT("tool_input"), ToolInput);
		Payload->TryGetStringField(TEXT("tool_result"), ToolResult);
		Payload->TryGetStringField(TEXT("raw_provider_event"), RawProviderEvent);
		const bool bToolEvidence =
			!ToolName.IsEmpty()
			|| EventType.StartsWith(TEXT("tool_"), ESearchCase::IgnoreCase);
		const FString PayloadJson = bToolEvidence ? SerializeCloseoutJsonObjectCompact(Payload) : FString();
		const FString EvidenceText =
			ToolName + TEXT("\n") + ToolInput + TEXT("\n") + ToolResult + TEXT("\n") + RawProviderEvent + TEXT("\n") + PayloadJson;
		ExtractManagedStateManualWriteFact(Payload, ToolName, EvidenceText, Facts, FactPrefix, RecordRunId);
		ExtractCloseoutSourceAndRuntimeFactsFromText(ToolName, EvidenceText, Facts, FactPrefix);
		ApplyAnimationWorkflowEvidence(EvidenceText, Facts, FactPrefix);
	}
	return Facts;
}

FOsvayderUECloseoutFactSnapshot SOsvayderEditorWidget::ExtractCurrentAgentTraceFactsForCloseout()
{
	FAgentTraceQueryOptions Options;
	Options.Count = 2000;
	Options.bLatestOnly = true;
	Options.bIncludeRawJson = true;
	Options.PreviewChars = 12000;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records =
		FOsvayderUEAgentTraceLog::Get().QueryEvents(Options, ResolvedRunId, TotalLoaded);
	return ExtractAgentTraceFactsForCloseout(Records);
}

FOsvayderUERunCloseoutContext SOsvayderEditorWidget::BuildRunCloseoutContext(
	const FOsvayderUEActivePlan& Plan,
	const TArray<TSharedPtr<FJsonObject>>& TraceRecords,
	const FString& RunId,
	const FString& ProjectRoot)
{
	FOsvayderUERunCloseoutContext Context;
	Context.RunId = RunId.TrimStartAndEnd();
	Context.PlanId = Plan.PlanId.TrimStartAndEnd();
	Context.FeatureWorkflowId = Plan.FeatureWorkflow.FeatureWorkflowId.TrimStartAndEnd();
	Context.RecipeId = Plan.FeatureWorkflow.RecipeId.TrimStartAndEnd();
	Context.RoleId = Plan.FeatureWorkflow.RoleId.TrimStartAndEnd();
	if (Context.RoleId.IsEmpty() && Plan.FeatureWorkflow.HasAnySignal())
	{
		Context.RoleId = OsvayderUERoleRegistry::WorkerRoleId();
	}
	FOsvayderUERecipeEvidenceContract ContextContract;
	if (OsvayderUERecipeRegistry::TryGetRecipeEvidenceContract(Context.RecipeId, ContextContract))
	{
		Context.EvidenceSchemaVersion = ContextContract.EvidenceSchemaVersion;
	}
	Context.ProjectRoot = ProjectRoot.TrimStartAndEnd().IsEmpty() ? GetCloseoutProjectRoot() : ProjectRoot.TrimStartAndEnd();
	Context.Facts = ExtractCloseoutFactsFromPlan(Plan);

	TArray<TSharedPtr<FJsonObject>> IncludedRecords;
	for (const TSharedPtr<FJsonObject>& Record : TraceRecords)
	{
		if (DoesTraceRecordMatchRunCloseoutContext(Record, Context))
		{
			Context.IncludedTraceEventIds.AddUnique(GetTraceRecordEventId(Record));
			NoteCloseoutTraceTimestamp(Context, Record);
			IncludedRecords.Add(Record);
		}
		else
		{
			Context.ExcludedTraceEventIds.AddUnique(GetTraceRecordEventId(Record));
		}
	}

	Context.Facts.MergeFrom(ExtractAgentTraceFactsForCloseout(IncludedRecords));
	MergeCurrentPrefixExternalRuntimeProofLogFacts(Plan, Context);
	return Context;
}

FOsvayderUERunCloseoutContext SOsvayderEditorWidget::BuildCurrentRunCloseoutContext(
	const FOsvayderUEActivePlan& Plan,
	const FString& PreferredRunId)
{
	FAgentTraceQueryOptions Options;
	Options.Count = 2000;
	Options.RunId = PreferredRunId.TrimStartAndEnd();
	Options.bLatestOnly = Options.RunId.IsEmpty();
	Options.bIncludeRawJson = true;
	Options.PreviewChars = 12000;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records =
		FOsvayderUEAgentTraceLog::Get().QueryEvents(Options, ResolvedRunId, TotalLoaded);
	(void)TotalLoaded;
	return BuildRunCloseoutContext(Plan, Records, ResolvedRunId, GetCloseoutProjectRoot());
}

FAgentFeatureWorkflowState SOsvayderEditorWidget::ReduceFeatureWorkflowForCloseout(
	const FAgentFeatureWorkflowState& Workflow,
	const FOsvayderUECloseoutFactSnapshot& Facts)
{
	FAgentFeatureWorkflowState Reduced = Workflow;
	if (!Reduced.HasAnySignal())
	{
		return Reduced;
	}

	Reduced.bKnownProofMapAvailable = Reduced.bKnownProofMapAvailable || Facts.bKnownProofMapAvailable;
	if (Facts.bProofInputMappingAvailable)
	{
		Reduced.InteractionAccessReuseObservation.bPersistentInputAssetObserved = true;
		Reduced.InteractionAccessReuseObservation.bInteractionActionAssetObserved = true;
		Reduced.InteractionAccessReuseObservation.bReadOnlyEnhancedInputQueryObserved = true;
	}
	Reduced.bPlacedRuntimeActorsAvailable = Reduced.bPlacedRuntimeActorsAvailable || Facts.bPlacedRuntimeActorsAvailable;
	Reduced.bReducedProofModeAllowed = Reduced.bReducedProofModeAllowed || Facts.bReducedProofModeAllowed;
	Reduced.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved =
		Reduced.InteractionAccessAttemptResolverObservation.bAttemptResolverSourceObserved
		|| Facts.bAttemptResolverSourceObserved;
	Reduced.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved =
		Reduced.InteractionAccessAttemptResolverObservation.bEventSubsystemSourceObserved
		|| Facts.bEventSubsystemSourceObserved;
	Reduced.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved =
		Reduced.InteractionAccessAttemptResolverObservation.bRuntimeSmokeSuccessObserved
		|| Facts.bRuntimeSmokeSuccessObserved;
	Reduced.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved =
		Reduced.InteractionAccessAttemptResolverObservation.bPrisonAccessEventObserved
		|| Facts.bPrisonAccessEventObserved;
	MergeCloseoutCount(Reduced.AutomationDiscoveryCount, Facts.AutomationDiscoveryCount);
	MergeCloseoutCount(Reduced.AutomationExecutedCount, Facts.AutomationExecutedCount);
	MergeCloseoutCount(Reduced.AutomationPassedCount, Facts.AutomationPassedCount);
	MergeCloseoutCount(Reduced.AutomationFailedCount, Facts.AutomationFailedCount);

	if (Facts.HasRuntimePrerequisiteFacts() && IsProofPrerequisiteAccountingBlocker(Reduced))
	{
		ClearContradictedProofAccountingStopLoss(Reduced);
		if (Reduced.RuntimeProofState.Equals(TEXT("blocked"), ESearchCase::IgnoreCase))
		{
			Reduced.RuntimeProofState = TEXT("pending");
		}
	}

	if (Reduced.HasRuntimeProofPrerequisites())
	{
		Reduced.ProofPrerequisiteState = TEXT("satisfied");
	}

	if (IsCloseoutInteractionAccessRecipe(Reduced)
		&& HasBoundedInteractionAccessRuntimeProofFromWorkflowOrFacts(Reduced, Facts))
	{
		Reduced.RuntimeProofState = TEXT("passed");
		Reduced.AdHocProofAttemptCount = 0;
		MarkCloseoutPhaseCompleted(Reduced, TEXT("input_asset_authoring"));
		MarkCloseoutPhaseCompleted(Reduced, TEXT("automation_discovery_gate"));
		MarkCloseoutPhaseCompleted(Reduced, TEXT("runtime_proof"));
		Reduced.CurrentPhase = TEXT("memory_update");
		if (Reduced.bStopLossTriggered
			&& (IsProofPrerequisiteAccountingBlocker(Reduced) || IsAdHocRuntimeProofAccountingBlocker(Reduced)))
		{
			ClearContradictedProofAccountingStopLoss(Reduced);
		}
		if (Reduced.bStopLossTriggered && IsAttemptResolverAndLoggingStopLoss(Reduced))
		{
			ClearContradictedAttemptResolverAndLoggingStopLoss(Reduced);
		}
		if (IsPostProofReadOnlyCloseoutDriftBlocker(Reduced, Facts))
		{
			ClearLatchedCleanCloseoutStopLoss(Reduced);
		}
	}

	return Reduced;
}

FOsvayderUEActivePlanCloseoutDecision SOsvayderEditorWidget::EvaluateActivePlanCloseout(
	const FOsvayderUEActivePlan& Plan,
	const bool bResponseSuccess)
{
	return EvaluateActivePlanCloseoutWithFacts(Plan, bResponseSuccess, ExtractCloseoutFactsFromPlan(Plan));
}

FOsvayderUEActivePlanCloseoutDecision SOsvayderEditorWidget::EvaluateActivePlanCloseoutFromCurrentArtifacts(
	const FOsvayderUEActivePlan& Plan,
	const bool bResponseSuccess)
{
	return EvaluateActivePlanCloseoutWithContext(Plan, bResponseSuccess, BuildCurrentRunCloseoutContext(Plan));
}

FOsvayderUEActivePlanCloseoutDecision SOsvayderEditorWidget::EvaluateActivePlanCloseoutWithFacts(
	const FOsvayderUEActivePlan& Plan,
	const bool bResponseSuccess,
	const FOsvayderUECloseoutFactSnapshot& Facts)
{
	FOsvayderUERunCloseoutContext Context;
	Context.PlanId = Plan.PlanId;
	Context.FeatureWorkflowId = Plan.FeatureWorkflow.FeatureWorkflowId;
	Context.RecipeId = Plan.FeatureWorkflow.RecipeId;
	Context.RoleId = Plan.FeatureWorkflow.RoleId.TrimStartAndEnd();
	if (Context.RoleId.IsEmpty() && Plan.FeatureWorkflow.HasAnySignal())
	{
		Context.RoleId = OsvayderUERoleRegistry::WorkerRoleId();
	}
	FOsvayderUERecipeEvidenceContract ContextContract;
	if (OsvayderUERecipeRegistry::TryGetRecipeEvidenceContract(Context.RecipeId, ContextContract))
	{
		Context.EvidenceSchemaVersion = ContextContract.EvidenceSchemaVersion;
	}
	Context.ProjectRoot = GetCloseoutProjectRoot();
	Context.Facts = Facts;
	return EvaluateActivePlanCloseoutWithContext(Plan, bResponseSuccess, Context);
}

FOsvayderUEActivePlanCloseoutDecision SOsvayderEditorWidget::EvaluateActivePlanCloseoutWithContext(
	const FOsvayderUEActivePlan& Plan,
	const bool bResponseSuccess,
	const FOsvayderUERunCloseoutContext& Context)
{
	FOsvayderUEActivePlanCloseoutDecision Decision;
	const FOsvayderUECloseoutFactSnapshot& Facts = Context.Facts;
	Decision.PlanStatus = bResponseSuccess ? TEXT("done") : TEXT("failed");
	Decision.ResultStatus = bResponseSuccess ? TEXT("achieved_fully") : TEXT("not_achieved");
	Decision.SourcePlanId = Plan.PlanId;
	Decision.SourceRunId = Context.RunId;
	Decision.SourceFeatureWorkflowId = Context.FeatureWorkflowId;
	Decision.SourceProjectRoot = Context.ProjectRoot;
	Decision.SourceRecipeId = Plan.FeatureWorkflow.RecipeId;
	Decision.SourceRoleId = !Plan.FeatureWorkflow.RoleId.TrimStartAndEnd().IsEmpty()
		? Plan.FeatureWorkflow.RoleId.TrimStartAndEnd()
		: Context.RoleId.TrimStartAndEnd();
	if (Decision.SourceRoleId.IsEmpty() && Plan.FeatureWorkflow.HasAnySignal())
	{
		Decision.SourceRoleId = OsvayderUERoleRegistry::WorkerRoleId();
	}
	Decision.ConsumedFactIds = Facts.ConsumedFactIds;

	FOsvayderUEActivePlan EffectivePlan = Plan;
	EffectivePlan.FeatureWorkflow = ReduceFeatureWorkflowForCloseout(Plan.FeatureWorkflow, Facts);
	if (EffectivePlan.FeatureWorkflow.HasAnySignal() && EffectivePlan.FeatureWorkflow.RoleId.TrimStartAndEnd().IsEmpty())
	{
		EffectivePlan.FeatureWorkflow.RoleId = Decision.SourceRoleId;
	}
	Decision.bFeatureWorkflowRequired = EffectivePlan.FeatureWorkflow.HasAnySignal();

	FOsvayderUERecipeEvidenceContract RecipeContract;
	const bool bHasRecipeId = !EffectivePlan.FeatureWorkflow.RecipeId.TrimStartAndEnd().IsEmpty();
	const bool bRecipeContractResolved = bHasRecipeId
		&& OsvayderUERecipeRegistry::TryGetRecipeEvidenceContract(
			EffectivePlan.FeatureWorkflow.RecipeId,
			RecipeContract);
	if (bRecipeContractResolved)
	{
		Decision.SourceRecipeId = RecipeContract.RecipeId;
		Decision.EvidenceSchemaVersion = RecipeContract.EvidenceSchemaVersion;
		Decision.bRecipeContractResolved = true;
	}

	FOsvayderUERoleContract RoleContract;
	const FString EffectiveRoleId = EffectivePlan.FeatureWorkflow.RoleId.TrimStartAndEnd();
	const bool bHasRoleId = !EffectiveRoleId.IsEmpty();
	const bool bRoleContractResolved = bHasRoleId
		&& OsvayderUERoleRegistry::TryGetRoleContract(EffectiveRoleId, RoleContract);
	if (bRoleContractResolved)
	{
		Decision.SourceRoleId = RoleContract.RoleId;
		Decision.bRoleContractResolved = true;
	}

	if (EffectivePlan.FeatureWorkflow.HasAnySignal())
	{
		const FAgentFeatureWorkflowState& Workflow = EffectivePlan.FeatureWorkflow;
		Decision.bRuntimeProofRequired = Workflow.bRuntimeProofRequired;
		Decision.bStopLossTriggered = Workflow.bStopLossTriggered;
		Decision.StopLossReason = Workflow.StopLossReason;
		Decision.BlockerFamily = Workflow.BlockerFamily;
		Decision.BlockerDetail = Workflow.BlockerDetail;
		Decision.bCompileProofRequired = EffectivePlan.CompileProof.bCompiledModuleMutationObserved || Workflow.bCompileProofRequired;
	}

	if (!bResponseSuccess)
	{
		return Decision;
	}

	if (Decision.bFeatureWorkflowRequired && bHasRoleId && !bRoleContractResolved)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("feature_workflow_unknown_role_contract");
		Decision.BlockerFamily = TEXT("role_contract_unknown");
		Decision.BlockerDetail = FString::Printf(
			TEXT("No registered role contract for role_id=%s"),
			*EffectiveRoleId);
		return Decision;
	}

	if (bRoleContractResolved
		&& RoleContract.RoleId.Equals(OsvayderUERoleRegistry::ArchitectRoleId(), ESearchCase::CaseSensitive)
		&& (EffectivePlan.FeatureWorkflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase)
			|| Facts.bRuntimeProofPassed
			|| HasBoundedInteractionAccessRuntimeProofFromWorkflowOrFacts(EffectivePlan.FeatureWorkflow, Facts)))
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("role_contract_architect_runtime_proof_claim_denied");
		Decision.BlockerFamily = TEXT("role_contract_violation");
		Decision.BlockerDetail = TEXT("role_id=architect cannot claim implementation, runtime proof, or final acceptance");
		return Decision;
	}

	if (Facts.bManagedStateManualWriteDetected)
	{
		const FAgentFeatureWorkflowState& Workflow = EffectivePlan.FeatureWorkflow;
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("managed_state_manual_write_detected");
		Decision.BlockerFamily = TEXT("managed_state_integrity");
		Decision.BlockerDetail = BuildManagedStateManualWriteDetail(Facts.ManagedStateManualWriteFactIds);
		Decision.bManagedStateManualWriteDetected = true;
		Decision.bRuntimeProofPassed =
			Workflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase)
			|| Facts.bRuntimeProofPassed
			|| HasBoundedInteractionAccessRuntimeProofFromWorkflowOrFacts(Workflow, Facts);
		return Decision;
	}

	if (Facts.bLocalAnimationPackIntakeRequired && !Facts.bLocalAnimationPackIntakeSucceeded)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("local_animation_pack_intake_required_missing");
		Decision.BlockerFamily = TEXT("animation_workflow_gate");
		Decision.BlockerDetail = TEXT("The prompt provided an explicit local animation pack path, but current-run trace has no successful local_animation_pack_intake evidence.");
		return Decision;
	}
	if (Facts.bLocalAnimationPackIntakeRequired && !Facts.bAnimationPreflightObserved)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("animation_preflight_required_missing");
		Decision.BlockerFamily = TEXT("animation_workflow_gate");
		Decision.BlockerDetail = TEXT("Local animation pack intake completed, but current-run trace has no animation_preflight evidence before closeout.");
		return Decision;
	}
	if (Facts.bAnimationPreflightSkeletonMismatchObserved && !Facts.bAnimationRetargetFixupSucceeded)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("animation_retarget_required_missing");
		Decision.BlockerFamily = TEXT("animation_workflow_gate");
		Decision.BlockerDetail = TEXT("animation_preflight reported skeleton_mismatch or retarget_required_not_implemented, but no later successful animation_retarget_fixup was observed in the current run.");
		return Decision;
	}
	if (Facts.bAnimationPreflightSkeletonMismatchObserved
		&& Facts.bAnimationRetargetFixupSucceeded
		&& !Facts.bPostRetargetCompatibleAnimationPreflightObserved)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("animation_compatibility_gate_failed");
		Decision.BlockerFamily = TEXT("animation_workflow_gate");
		Decision.BlockerDetail = TEXT("animation_retarget_fixup succeeded after a skeleton mismatch, but no compatible post-retarget animation_preflight evidence was observed.");
		return Decision;
	}
	if (Facts.bAnimationPreflightSkeletonMismatchObserved
		&& Facts.bAnimationRetargetFixupSucceeded
		&& Facts.bPostRetargetCompatibleAnimationPreflightObserved
		&& Facts.bAnimationLineageRoleEvidencePresent
		&& Facts.MissingAnimationLineageRoles.Num() > 0)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("animation_required_roles_not_compatible");
		Decision.BlockerFamily = TEXT("animation_workflow_gate");
		Decision.BlockerDetail = FString::Printf(
			TEXT("Post-retarget compatible animation_preflight did not cover required role(s) from the latest mismatched preflight: %s"),
			*FString::Join(Facts.MissingAnimationLineageRoles, TEXT(", ")));
		return Decision;
	}
	if (Facts.bAnimationPreflightSkeletonMismatchObserved
		&& Facts.bAnimationRetargetFixupSucceeded
		&& Facts.bPostRetargetCompatibleAnimationPreflightObserved
		&& Facts.bAnimationLineagePathCheckApplied
		&& Facts.AnimationLineagePathMismatchRoles.Num() > 0)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("animation_role_lineage_gate_failed");
		Decision.BlockerFamily = TEXT("animation_workflow_gate");
		Decision.BlockerDetail = FString::Printf(
			TEXT("Post-retarget compatible animation_preflight used paths that do not match generated retarget destinations for role(s): %s"),
			*FString::Join(Facts.AnimationLineagePathMismatchRoles, TEXT(", ")));
		return Decision;
	}

	if (IsVisualQaManifestRequiredForPlan(EffectivePlan))
	{
		const FVisualQaManifestValidationResult Validation = ValidateVisualQaManifestForPlan(EffectivePlan, FString());
		if (!Validation.bValid)
		{
			const bool bExplicitVisualBlocker =
				EffectivePlan.VisualProofStatus.Equals(TEXT("blocked"), ESearchCase::IgnoreCase)
				|| !EffectivePlan.VisualProofBlocker.TrimStartAndEnd().IsEmpty();
			FailDecisionForVisualQaManifest(Decision, Validation, bExplicitVisualBlocker);
			return Decision;
		}
	}

	if (Decision.bFeatureWorkflowRequired && bHasRecipeId && !bRecipeContractResolved)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("feature_workflow_unknown_recipe_contract");
		Decision.BlockerFamily = TEXT("recipe_contract_unknown");
		Decision.BlockerDetail = FString::Printf(
			TEXT("No registered evidence contract for recipe_id=%s"),
			*EffectivePlan.FeatureWorkflow.RecipeId);
		return Decision;
	}

	if (EffectivePlan.FeatureWorkflow.HasAnySignal())
	{
		const FAgentFeatureWorkflowState& Workflow = EffectivePlan.FeatureWorkflow;
		if (Workflow.bStopLossTriggered)
		{
			Decision.PlanStatus = TEXT("failed");
			Decision.ResultStatus = TEXT("not_achieved");
			Decision.GateReasonCode = TEXT("feature_workflow_stop_loss_triggered");
			return Decision;
		}
	}

	const FOsvayderUECompileProofState& CompileProof = EffectivePlan.CompileProof;
	if (!Decision.bFeatureWorkflowRequired)
	{
		Decision.bCompileProofRequired = CompileProof.bCompiledModuleMutationObserved;
	}
	if (!Decision.bCompileProofRequired)
	{
		if (!Decision.bFeatureWorkflowRequired)
		{
			return Decision;
		}
	}

	const bool bHasCompileOutcome =
		CompileProof.LastCompileProofOutcome.Equals(TEXT("success"), ESearchCase::IgnoreCase)
		|| CompileProof.LastCompileProofOutcome.Equals(TEXT("failed"), ESearchCase::IgnoreCase);
	const bool bCompileProofFresh = !CompileProof.LastCompileProofAtUtc.IsEmpty()
		&& (CompileProof.LastMutationAtUtc.IsEmpty()
			|| IsIsoUtcAfter(CompileProof.LastCompileProofAtUtc, CompileProof.LastMutationAtUtc));
	if (!bCompileProofFresh || !bHasCompileOutcome)
	{
		if (Decision.bCompileProofRequired)
		{
			Decision.PlanStatus = TEXT("failed");
			Decision.ResultStatus = TEXT("not_achieved");
			Decision.GateReasonCode = TEXT("missing_compile_truth_after_cpp_mutation");
			return Decision;
		}
	}

	Decision.bFreshCompileProofObserved = bCompileProofFresh && bHasCompileOutcome;
	if (Decision.bFreshCompileProofObserved
		&& CompileProof.LastCompileProofOutcome.Equals(TEXT("failed"), ESearchCase::IgnoreCase))
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("compile_failed_after_cpp_mutation");
		Decision.bFreshCompileFailureObserved = true;
		return Decision;
	}

	const bool bVerificationFresh = !CompileProof.LastPostCompileVerificationAtUtc.IsEmpty()
		&& IsIsoUtcAfter(CompileProof.LastPostCompileVerificationAtUtc, CompileProof.LastCompileProofAtUtc);
	if (!Decision.bFeatureWorkflowRequired && !bVerificationFresh)
	{
		Decision.PlanStatus = TEXT("done");
		Decision.ResultStatus = TEXT("achieved_partially");
		Decision.GateReasonCode = TEXT("compile_succeeded_without_post_compile_verification");
		return Decision;
	}

	if (CompileProof.LastPostCompileVerificationOutcome.Equals(TEXT("fail"), ESearchCase::IgnoreCase))
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("post_compile_verification_failed_after_cpp_mutation");
		Decision.bPostCompileVerificationFailed = true;
		if (!Decision.bFeatureWorkflowRequired)
		{
			return Decision;
		}
	}

	if (CompileProof.LastPostCompileVerificationOutcome.Equals(TEXT("pass"), ESearchCase::IgnoreCase))
	{
		Decision.bPostCompileVerificationPassed = true;
		if (!Decision.bFeatureWorkflowRequired)
		{
			return Decision;
		}
	}

	if (!Decision.bFeatureWorkflowRequired)
	{
		Decision.PlanStatus = TEXT("done");
		Decision.ResultStatus = TEXT("achieved_partially");
		Decision.GateReasonCode = TEXT("compile_succeeded_without_post_compile_verification");
		return Decision;
	}

	const FAgentFeatureWorkflowState& Workflow = EffectivePlan.FeatureWorkflow;
	if (Workflow.AuthoringLaneState.Equals(TEXT("denied"), ESearchCase::IgnoreCase)
		&& !Workflow.bReducedProofModeAllowed)
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("feature_workflow_authoring_lane_denied");
		Decision.bAuthoringLaneDenied = true;
		return Decision;
	}
	if ((Workflow.BlockerFamily == TEXT("automation_discovery_failed"))
		|| (Workflow.AutomationDiscoveryCommand.Len() > 0 && Workflow.AutomationDiscoveryCount == 0))
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("feature_workflow_automation_discovery_failed");
		Decision.bAutomationDiscoveryFailed = true;
		return Decision;
	}
	const bool bInteractionAccessRecipe =
		bRecipeContractResolved
		&& RecipeContract.RecipeId.Equals(OsvayderUERecipeRegistry::InteractionAccessRecipeId(), ESearchCase::CaseSensitive);
	if (bInteractionAccessRecipe
		&& (Workflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase)
			|| Workflow.BlockerFamily.Equals(TEXT("attempt_resolver_observation_missing"), ESearchCase::IgnoreCase))
		&& !Workflow.InteractionAccessAttemptResolverObservation.HasCompleteEvidence())
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("feature_workflow_attempt_resolver_observation_missing");
		Decision.BlockerFamily = TEXT("attempt_resolver_observation_missing");
		Decision.BlockerDetail = TEXT("interaction_access_attempt_resolver_observation_empty");
		return Decision;
	}
	if (bInteractionAccessRecipe
		&& Workflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase)
		&& !Workflow.InteractionAccessReuseObservation.HasSufficientReuseEvidence())
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("feature_workflow_proof_input_mapping_missing");
		Decision.BlockerFamily = TEXT("proof_input_mapping_missing");
		Decision.BlockerDetail = TEXT("IMC_PrisonAccessProof/IA_Interact mapping evidence is missing from the current run");
		return Decision;
	}
	if (bRecipeContractResolved && Workflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase))
	{
		const TArray<FString> MissingObligations =
			FindMissingWorkflowRecipeObligations(RecipeContract, Workflow);
		if (MissingObligations.Num() > 0)
		{
			Decision.PlanStatus = TEXT("failed");
			Decision.ResultStatus = TEXT("not_achieved");
			Decision.MissingRecipeObligations = MissingObligations;
			Decision.GateReasonCode = BuildMissingRecipeObligationGateReason(MissingObligations);
			Decision.BlockerFamily = BuildMissingRecipeObligationBlockerFamily(MissingObligations);
			Decision.BlockerDetail = FString::Printf(
				TEXT("recipe_id=%s evidence_schema_version=%d missing_recipe_obligations=%s"),
				*RecipeContract.RecipeId,
				RecipeContract.EvidenceSchemaVersion,
				*JoinRecipeObligations(MissingObligations));
			return Decision;
		}
	}
	if (Workflow.RuntimeProofState.Equals(TEXT("blocked"), ESearchCase::IgnoreCase)
		|| (Workflow.ProofPrerequisiteState.Equals(TEXT("missing"), ESearchCase::IgnoreCase)
			&& !Workflow.HasRuntimeProofPrerequisites()))
	{
		if (Workflow.BlockerFamily.Equals(TEXT("manual_asset_dependency_blocker"), ESearchCase::IgnoreCase)
			|| Workflow.BlockerDetail.Contains(TEXT("manual_asset_dependency_blocker"), ESearchCase::IgnoreCase))
		{
			Decision.PlanStatus = TEXT("failed");
			Decision.ResultStatus = TEXT("not_achieved");
			Decision.GateReasonCode = TEXT("manual_asset_dependency_blocker");
			Decision.BlockerFamily = Workflow.BlockerFamily.IsEmpty()
				? FString(TEXT("manual_asset_dependency_blocker"))
				: Workflow.BlockerFamily;
			Decision.BlockerDetail = Workflow.BlockerDetail.IsEmpty()
				? FString(TEXT("Requested external asset dependency is not available locally."))
				: Workflow.BlockerDetail;
			return Decision;
		}
		if (Workflow.BlockerFamily.Equals(TEXT("blocked_on_tool_surface"), ESearchCase::IgnoreCase)
			|| Workflow.BlockerFamily.Equals(TEXT("tool_surface_unavailable"), ESearchCase::IgnoreCase)
			|| Workflow.BlockerDetail.Contains(TEXT("blocked_on_tool_surface"), ESearchCase::IgnoreCase)
			|| Workflow.BlockerDetail.Contains(TEXT("tool_surface_unavailable"), ESearchCase::IgnoreCase))
		{
			Decision.PlanStatus = TEXT("failed");
			Decision.ResultStatus = TEXT("not_achieved");
			Decision.GateReasonCode = TEXT("blocked_on_tool_surface");
			Decision.BlockerFamily = Workflow.BlockerFamily.IsEmpty()
				? FString(TEXT("blocked_on_tool_surface"))
				: Workflow.BlockerFamily;
			Decision.BlockerDetail = Workflow.BlockerDetail.IsEmpty()
				? FString(TEXT("Required runtime/capture tool surface is unavailable."))
				: Workflow.BlockerDetail;
			return Decision;
		}
		if (Workflow.BlockerFamily.Equals(TEXT("mechanic_input_conflict_unresolved"), ESearchCase::IgnoreCase)
			|| Workflow.BlockerDetail.Contains(TEXT("mechanic_input_conflict_unresolved"), ESearchCase::IgnoreCase)
			|| Workflow.BlockerDetail.Contains(TEXT("input conflict"), ESearchCase::IgnoreCase))
		{
			Decision.PlanStatus = TEXT("failed");
			Decision.ResultStatus = TEXT("not_achieved");
			Decision.GateReasonCode = TEXT("mechanic_input_conflict_unresolved");
			Decision.BlockerFamily = Workflow.BlockerFamily.IsEmpty()
				? FString(TEXT("mechanic_input_conflict_unresolved"))
				: Workflow.BlockerFamily;
			Decision.BlockerDetail = Workflow.BlockerDetail.IsEmpty()
				? FString(TEXT("Requested mechanic controls conflict with existing input ownership."))
				: Workflow.BlockerDetail;
			return Decision;
		}
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("feature_workflow_proof_prerequisites_missing");
		Decision.bProofPrerequisitesMissing = true;
		return Decision;
	}
	if (Workflow.RuntimeProofState.Equals(TEXT("denied"), ESearchCase::IgnoreCase))
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("feature_workflow_runtime_proof_denied_pending_compile");
		Decision.bRuntimeProofFailed = true;
		return Decision;
	}
	if (Workflow.RuntimeProofState.Equals(TEXT("failed"), ESearchCase::IgnoreCase))
	{
		Decision.PlanStatus = TEXT("failed");
		Decision.ResultStatus = TEXT("not_achieved");
		Decision.GateReasonCode = TEXT("feature_workflow_runtime_proof_failed");
		Decision.bRuntimeProofFailed = true;
		return Decision;
	}
	if (Workflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase))
	{
		Decision.bRuntimeProofPassed = true;
		return Decision;
	}

	Decision.PlanStatus = TEXT("done");
	Decision.ResultStatus = TEXT("achieved_partially");
	Decision.GateReasonCode = TEXT("feature_workflow_runtime_proof_missing");
	return Decision;
}

TSharedPtr<FJsonObject> SOsvayderEditorWidget::BuildCloseoutDecisionJson(
	const FOsvayderUEActivePlan& Plan,
	const FOsvayderUEActivePlanCloseoutDecision& Decision,
	const FOsvayderUECloseoutFactSnapshot& Facts)
{
	FOsvayderUERunCloseoutContext Context;
	Context.PlanId = Plan.PlanId;
	Context.FeatureWorkflowId = Plan.FeatureWorkflow.FeatureWorkflowId;
	Context.RecipeId = Plan.FeatureWorkflow.RecipeId;
	Context.RoleId = Plan.FeatureWorkflow.RoleId.TrimStartAndEnd();
	if (Context.RoleId.IsEmpty() && Plan.FeatureWorkflow.HasAnySignal())
	{
		Context.RoleId = OsvayderUERoleRegistry::WorkerRoleId();
	}
	FOsvayderUERecipeEvidenceContract ContextContract;
	if (OsvayderUERecipeRegistry::TryGetRecipeEvidenceContract(Context.RecipeId, ContextContract))
	{
		Context.EvidenceSchemaVersion = ContextContract.EvidenceSchemaVersion;
	}
	Context.ProjectRoot = GetCloseoutProjectRoot();
	Context.Facts = Facts;
	return BuildCloseoutDecisionJson(Plan, Decision, Context);
}

TSharedPtr<FJsonObject> SOsvayderEditorWidget::BuildCloseoutDecisionJson(
	const FOsvayderUEActivePlan& Plan,
	const FOsvayderUEActivePlanCloseoutDecision& Decision,
	const FOsvayderUERunCloseoutContext& Context)
{
	const FOsvayderUECloseoutFactSnapshot& Facts = Context.Facts;
	const FString SourcePlanId = Decision.SourcePlanId.IsEmpty() ? Plan.PlanId : Decision.SourcePlanId;
	const FString SourceRunId = Decision.SourceRunId.IsEmpty() ? Context.RunId : Decision.SourceRunId;
	const FString SourceFeatureWorkflowId = Decision.SourceFeatureWorkflowId.IsEmpty()
		? Context.FeatureWorkflowId
		: Decision.SourceFeatureWorkflowId;
	const FString SourceProjectRoot = Decision.SourceProjectRoot.IsEmpty()
		? Context.ProjectRoot
		: Decision.SourceProjectRoot;
	FString SourceRoleId = !Decision.SourceRoleId.IsEmpty()
		? Decision.SourceRoleId
		: (!Context.RoleId.IsEmpty() ? Context.RoleId : Plan.FeatureWorkflow.RoleId);
	if (SourceRoleId.TrimStartAndEnd().IsEmpty() && Plan.FeatureWorkflow.HasAnySignal())
	{
		SourceRoleId = OsvayderUERoleRegistry::WorkerRoleId();
	}
	FOsvayderUERecipeEvidenceContract RecipeContract;
	const FString SourceRecipeId = !Decision.SourceRecipeId.IsEmpty()
		? Decision.SourceRecipeId
		: (!Context.RecipeId.IsEmpty() ? Context.RecipeId : Plan.FeatureWorkflow.RecipeId);
	const bool bRecipeContractResolved =
		OsvayderUERecipeRegistry::TryGetRecipeEvidenceContract(SourceRecipeId, RecipeContract);
	FOsvayderUERoleContract RoleContract;
	const bool bRoleContractResolved =
		OsvayderUERoleRegistry::TryGetRoleContract(SourceRoleId, RoleContract);
	const int32 EvidenceSchemaVersion = Decision.EvidenceSchemaVersion > 0
		? Decision.EvidenceSchemaVersion
		: (Context.EvidenceSchemaVersion > 0
			? Context.EvidenceSchemaVersion
			: (bRecipeContractResolved ? RecipeContract.EvidenceSchemaVersion : 0));
	const FOsvayderUEUserFacingStatus UserFacingStatus =
		OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision, Context);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(TEXT("artifact_type"), TEXT("closeout_decision"));
	Root->SetStringField(TEXT("source_plan_id"), SourcePlanId);
	Root->SetStringField(TEXT("source_run_id"), SourceRunId);
	Root->SetStringField(TEXT("source_feature_workflow_id"), SourceFeatureWorkflowId);
	Root->SetStringField(TEXT("recipe_id"), SourceRecipeId);
	Root->SetStringField(TEXT("role_id"), SourceRoleId);
	Root->SetNumberField(TEXT("evidence_schema_version"), EvidenceSchemaVersion);
	Root->SetStringField(TEXT("source_project_root"), SourceProjectRoot);
	if (!Decision.SourceArchivePath.IsEmpty())
	{
		Root->SetStringField(TEXT("source_archive_path"), Decision.SourceArchivePath);
	}
	Root->SetStringField(TEXT("user_facing_status_id"), UserFacingStatus.StatusId);
	Root->SetStringField(TEXT("user_facing_headline"), UserFacingStatus.Headline);
	Root->SetObjectField(TEXT("user_facing_status"), UserFacingStatus.ToJsonObject());
	Root->SetStringField(TEXT("created_at_utc"), FDateTime::UtcNow().ToIso8601());

	TSharedPtr<FJsonObject> DecisionObject = MakeShared<FJsonObject>();
	DecisionObject->SetStringField(TEXT("plan_status"), Decision.PlanStatus);
	DecisionObject->SetStringField(TEXT("result_status"), Decision.ResultStatus);
	DecisionObject->SetStringField(TEXT("gate_reason_code"), Decision.GateReasonCode);
	DecisionObject->SetStringField(TEXT("source_run_id"), SourceRunId);
	DecisionObject->SetStringField(TEXT("source_plan_id"), SourcePlanId);
	DecisionObject->SetStringField(TEXT("source_feature_workflow_id"), SourceFeatureWorkflowId);
	DecisionObject->SetStringField(TEXT("recipe_id"), SourceRecipeId);
	DecisionObject->SetStringField(TEXT("role_id"), SourceRoleId);
	DecisionObject->SetNumberField(TEXT("evidence_schema_version"), EvidenceSchemaVersion);
	DecisionObject->SetStringField(TEXT("user_facing_status_id"), UserFacingStatus.StatusId);
	DecisionObject->SetStringField(TEXT("user_facing_headline"), UserFacingStatus.Headline);
	DecisionObject->SetStringField(TEXT("source_project_root"), SourceProjectRoot);
	DecisionObject->SetBoolField(TEXT("feature_workflow_required"), Decision.bFeatureWorkflowRequired);
	DecisionObject->SetBoolField(TEXT("recipe_contract_resolved"), Decision.bRecipeContractResolved || bRecipeContractResolved);
	DecisionObject->SetBoolField(TEXT("role_contract_resolved"), Decision.bRoleContractResolved || bRoleContractResolved);
	DecisionObject->SetBoolField(TEXT("runtime_proof_required"), Decision.bRuntimeProofRequired);
	DecisionObject->SetBoolField(TEXT("runtime_proof_passed"), Decision.bRuntimeProofPassed);
	DecisionObject->SetBoolField(TEXT("runtime_proof_failed"), Decision.bRuntimeProofFailed);
	DecisionObject->SetBoolField(TEXT("stop_loss_triggered"), Decision.bStopLossTriggered);
	DecisionObject->SetBoolField(TEXT("proof_prerequisites_missing"), Decision.bProofPrerequisitesMissing);
	DecisionObject->SetBoolField(TEXT("automation_discovery_failed"), Decision.bAutomationDiscoveryFailed);
	DecisionObject->SetBoolField(TEXT("authoring_lane_denied"), Decision.bAuthoringLaneDenied);
	DecisionObject->SetBoolField(TEXT("compile_proof_required"), Decision.bCompileProofRequired);
	DecisionObject->SetBoolField(TEXT("fresh_compile_proof_observed"), Decision.bFreshCompileProofObserved);
	DecisionObject->SetBoolField(TEXT("fresh_compile_failure_observed"), Decision.bFreshCompileFailureObserved);
	DecisionObject->SetBoolField(TEXT("current_closed_editor_proof_observed"), Decision.bCurrentClosedEditorProofObserved);
	DecisionObject->SetBoolField(TEXT("post_compile_verification_passed"), Decision.bPostCompileVerificationPassed);
	DecisionObject->SetBoolField(TEXT("post_compile_verification_failed"), Decision.bPostCompileVerificationFailed);
	DecisionObject->SetBoolField(TEXT("managed_state_manual_write_detected"), Decision.bManagedStateManualWriteDetected);
	DecisionObject->SetBoolField(TEXT("visual_proof_required"), Plan.bVisualProofRequired);
	DecisionObject->SetBoolField(TEXT("visual_qa_manifest_required"), Plan.bVisualQaManifestRequired);
	DecisionObject->SetStringField(TEXT("visual_proof_status"), Plan.VisualProofStatus);
	DecisionObject->SetStringField(TEXT("visual_qa_manifest_path"), Plan.VisualQaManifestPath);
	DecisionObject->SetStringField(TEXT("visual_qa_manifest_verdict"), Plan.VisualQaManifestVerdict);
	DecisionObject->SetStringField(TEXT("stop_loss_reason"), Decision.StopLossReason);
	DecisionObject->SetStringField(TEXT("blocker_family"), Decision.BlockerFamily);
	DecisionObject->SetStringField(TEXT("blocker_detail"), Decision.BlockerDetail);
	DecisionObject->SetArrayField(TEXT("consumed_fact_ids"), MakeCloseoutStringArrayJson(Decision.ConsumedFactIds));
	DecisionObject->SetArrayField(TEXT("missing_recipe_obligations"), MakeCloseoutStringArrayJson(Decision.MissingRecipeObligations));
	Root->SetObjectField(TEXT("decision"), DecisionObject);
	if (bRecipeContractResolved)
	{
		Root->SetObjectField(TEXT("recipe_contract"), RecipeContract.ToJsonObject());
	}
	if (bRoleContractResolved)
	{
		Root->SetObjectField(TEXT("role_contract"), RoleContract.ToJsonObject());
	}
	Root->SetObjectField(TEXT("facts_consumed"), Facts.ToJsonObject());
	Root->SetObjectField(TEXT("run_closeout_context"), Context.ToJsonObject());

	if (!Decision.GateReasonCode.IsEmpty())
	{
		TSharedPtr<FJsonObject> BlockerObject = MakeShared<FJsonObject>();
		BlockerObject->SetStringField(TEXT("gate_reason_code"), Decision.GateReasonCode);
		BlockerObject->SetStringField(TEXT("blocker_family"), Decision.BlockerFamily);
		BlockerObject->SetStringField(TEXT("blocker_detail"), Decision.BlockerDetail);
		BlockerObject->SetStringField(TEXT("stop_loss_reason"), Decision.StopLossReason);
		Root->SetObjectField(TEXT("blocker"), BlockerObject);
	}

	return Root;
}

FString SOsvayderEditorWidget::BuildActivePlanCloseoutWarningText(
	const FOsvayderUEActivePlanCloseoutDecision& Decision,
	const FString& FinalResponse)
{
	FString WarningBody;
	if (Decision.GateReasonCode == TEXT("missing_compile_truth_after_cpp_mutation"))
	{
		WarningBody = TEXT("Active-plan closeout blocked (638 policy):\n"
			"- C++ mutation was observed in a compiled module.\n"
			"- No fresh compile proof exists after the latest mutation.\n"
			"- Text alone cannot close this task; rerun compile/build and verify the result.");
	}
	else if (Decision.GateReasonCode == TEXT("compile_failed_after_cpp_mutation"))
	{
		WarningBody = TEXT("Active-plan closeout blocked (638 policy):\n"
			"- Fresh compile proof exists, but the latest compile failed after the C++ mutation.\n"
			"- Treat the task as NOT achieved until the compile errors are fixed and a new proof is recorded.");
	}
	else if (Decision.GateReasonCode == TEXT("post_compile_verification_failed_after_cpp_mutation"))
	{
		WarningBody = TEXT("Active-plan closeout blocked (638 policy):\n"
			"- Compile succeeded, but the latest post-compile verification failed.\n"
			"- The task remains NOT achieved until that verification passes.");
	}
	else if (Decision.GateReasonCode == TEXT("managed_state_manual_write_detected"))
	{
		WarningBody = TEXT("Active-plan closeout blocked (managed-state integrity):\n"
			"- A mutating agent shell command touched OsvayderUE managed state in this run.\n"
			"- Runtime proof may be valid, but only plugin-owned finalization can write accepted active_plan/archive/closeout artifacts.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_unknown_recipe_contract"))
	{
		WarningBody = TEXT("Feature workflow closeout blocked by unknown recipe contract:\n"
			"- The active recipe id has no registered evidence contract.\n"
			"- The task remains NOT achieved until a product-owned recipe/evidence contract is registered.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_recipe_obligation_missing"))
	{
		WarningBody = TEXT("Feature workflow recipe obligations are missing:\n"
			"- The recipe contract requires current-run evidence that was not consumed by closeout.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("missing_active_plan_closeout_proof"))
	{
		WarningBody = TEXT("Active-plan closeout blocked:\n"
			"- active_plan.json or equivalent current closeout proof is missing.\n"
			"- Provider transport success cannot be archived as task success without plugin-owned proof.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("fallback_closeout_proof_missing"))
	{
		WarningBody = TEXT("Restart-survival fallback closeout blocked:\n"
			"- The plan was recovered/synthesized from fallback state.\n"
			"- A current typed closed-editor result or closeout proof is required before `achieved_fully` can be accepted.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode.StartsWith(TEXT("visual_qa_manifest"), ESearchCase::CaseSensitive)
		|| Decision.GateReasonCode == TEXT("visual_proof_blocked"))
	{
		WarningBody = TEXT("Visual-reference closeout blocked:\n"
			"- A visual/reference task requires a visual_qa_manifest.json with verdict=passed.\n"
			"- The manifest must list at least one actual_screenshot_paths entry; a bare screenshot path is not enough.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("final_response_declares_failure"))
	{
		WarningBody = TEXT("Active-plan closeout blocked:\n"
			"- The provider call completed, but the final assistant text explicitly reported failure or a blocker.\n"
			"- Transport success is not task success.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("visual_proof_required_missing"))
	{
		WarningBody = TEXT("Visual proof gate blocked closeout:\n"
			"- The original prompt referenced an image/visual target.\n"
			"- Final success requires a passed visual_qa_manifest.json with actual_screenshot_paths, or an explicit visual-proof blocker.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("visual_proof_blocked"))
	{
		WarningBody = TEXT("Visual proof gate blocked closeout:\n"
			"- The final response reported a visual-proof blocker instead of a passed visual QA manifest.\n"
			"- The task remains NOT achieved until visual evidence exists or the blocker is resolved.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("compile_succeeded_without_post_compile_verification")
		&& DoesResponseClaimStatusFull(FinalResponse))
	{
		WarningBody = TEXT("Active-plan closeout downgraded (638 policy):\n"
			"- Fresh compile proof exists after the latest C++ mutation.\n"
			"- No successful post-compile verification was observed after that compile.\n"
			"- The task is kept PARTIAL/UNVERIFIED instead of `achieved_fully`.");
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_stop_loss_triggered"))
	{
		WarningBody = TEXT("Feature workflow stopped by stop-loss:\n"
			"- The same bounded workflow kept failing without phase progress.\n"
			"- Replan or reviewer intervention is required before claiming completion.");
		if (!Decision.StopLossReason.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- stop_loss_reason: %s"), *Decision.StopLossReason);
		}
		if (!Decision.BlockerFamily.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- blocker_family: %s"), *Decision.BlockerFamily);
		}
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- blocker_detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_runtime_proof_denied_pending_compile"))
	{
		WarningBody = TEXT("Feature workflow runtime proof was denied:\n"
			"- Runtime proof was attempted before the required compile gate passed.\n"
			"- Record compile success first, then retry runtime proof.");
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_runtime_proof_failed"))
	{
		WarningBody = TEXT("Feature workflow runtime proof failed:\n"
			"- The required runtime proof lane executed and reported failure.\n"
			"- The task remains NOT achieved until runtime proof passes.");
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_proof_input_mapping_missing"))
	{
		WarningBody = TEXT("Feature workflow proof input mapping is missing:\n"
			"- Runtime proof for interaction access must cite the current-run proof IMC/input action mapping.\n"
			"- The task remains NOT achieved until IMC_PrisonAccessProof / IA_Interact evidence is consumed by closeout.");
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_authoring_lane_denied"))
	{
		WarningBody = TEXT("Feature workflow input-authoring lane was denied:\n"
			"- Persistent input authoring was blocked by policy.\n"
			"- The controller must not silently degrade into transient runtime-only input and still claim progress.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- deny_rule: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_automation_discovery_failed"))
	{
		WarningBody = TEXT("Feature workflow automation discovery failed:\n"
			"- The automation proof command discovered or executed zero tests.\n"
			"- Zero-test automation is proof unavailable, not success.");
	}
	else if (Decision.GateReasonCode == TEXT("manual_asset_dependency_blocker"))
	{
		WarningBody = TEXT("Feature workflow blocked by an external asset dependency:\n"
			"- Fab/Marketplace/Epic Launcher acquisition cannot be used as automated acceptance evidence.\n"
			"- Continue with local code-only/placeholders if safe, or report the exact missing local asset dependency.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- blocker_detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("blocked_on_tool_surface"))
	{
		WarningBody = TEXT("Feature workflow blocked by missing post-reattach tool surface:\n"
			"- Runtime/capture proof requires the reopened editor to expose the needed tools.\n"
			"- If map/runtime/capture tools are unavailable, close as blocked_on_tool_surface with exact missing tool names.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- blocker_detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("mechanic_input_conflict_unresolved"))
	{
		WarningBody = TEXT("Feature workflow blocked by unresolved mechanic/input conflict:\n"
			"- Requested controls must be audited against existing action owners before implementation.\n"
			"- Resolve the input ownership conflict or report the exact conflicting mechanic instead of silently implementing over it.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- blocker_detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_proof_prerequisites_missing"))
	{
		WarningBody = TEXT("Feature workflow proof prerequisites are missing:\n"
			"- Runtime proof cannot start without a proof map, placed runtime actors/fixture, discoverable automation tests, or explicit reduced-proof mode.");
		if (!Decision.BlockerDetail.IsEmpty())
		{
			WarningBody += FString::Printf(TEXT("\n- detail: %s"), *Decision.BlockerDetail);
		}
	}
	else if (Decision.GateReasonCode == TEXT("feature_workflow_runtime_proof_missing")
		&& DoesResponseClaimStatusFull(FinalResponse))
	{
		WarningBody = TEXT("Feature workflow closeout downgraded:\n"
			"- Compile requirements are satisfied, but no approved runtime proof passed.\n"
			"- The task is kept PARTIAL instead of `achieved_fully`.");
	}

	if (WarningBody.IsEmpty())
	{
		return FString();
	}

	FOsvayderUEActivePlan EmptyPlan;
	const FOsvayderUEUserFacingStatus UserFacingStatus =
		OsvayderUEUserFacingStatus::BuildStatus(EmptyPlan, Decision);
	FString Warning = FString::Printf(
		TEXT("[WARN] %s\n- status_id: %s\n- detail: %s"),
		*UserFacingStatus.Headline,
		*UserFacingStatus.StatusId,
		*UserFacingStatus.Detail);
	if (!Decision.GateReasonCode.IsEmpty())
	{
		Warning += FString::Printf(TEXT("\n- gate_reason_code: %s"), *Decision.GateReasonCode);
	}
	Warning += TEXT("\n");
	Warning += WarningBody;
	return Warning;
}

FString SOsvayderEditorWidget::RewriteResponseForTruthfulActivePlanCloseout(
	const FOsvayderUEActivePlan& Plan,
	const FOsvayderUEActivePlanCloseoutDecision& Decision,
	const FString& FinalResponse)
{
	if (FinalResponse.IsEmpty()
		|| !DoesResponseClaimStatusFull(FinalResponse)
		|| Decision.ResultStatus.Equals(TEXT("achieved_fully"), ESearchCase::CaseSensitive))
	{
		return FinalResponse;
	}

	auto MapResultStatusToCloseoutLabel = [](const FString& ResultStatus) -> FString
	{
		if (ResultStatus.Equals(TEXT("achieved_partially"), ESearchCase::CaseSensitive))
		{
			return TEXT("partial");
		}
		if (ResultStatus.Equals(TEXT("not_achieved"), ESearchCase::CaseSensitive))
		{
			return TEXT("fail");
		}
		return TEXT("full");
	};

	auto DescribeFeatureWorkflowTerminalStatus = [&Decision]() -> FString
	{
		if (!Decision.bFeatureWorkflowRequired)
		{
			return FString();
		}
		if (Decision.bStopLossTriggered)
		{
			return TEXT("stop_loss");
		}
		if (Decision.PlanStatus.Equals(TEXT("done"), ESearchCase::CaseSensitive))
		{
			return Decision.ResultStatus.Equals(TEXT("achieved_fully"), ESearchCase::CaseSensitive)
				? TEXT("completed_full")
				: TEXT("completed_partial");
		}
		return TEXT("failed");
	};

	auto DescribeCompileProofState = [&Plan, &Decision]() -> FString
	{
		if (!Decision.bCompileProofRequired)
		{
			return TEXT("not_required");
		}
		if (Plan.CompileProof.LastCompileProofOutcome.Equals(TEXT("success"), ESearchCase::IgnoreCase)
			|| Decision.bFreshCompileProofObserved)
		{
			return TEXT("passed");
		}
		if (Plan.CompileProof.LastCompileProofOutcome.Equals(TEXT("failed"), ESearchCase::IgnoreCase)
			|| Decision.bFreshCompileFailureObserved)
		{
			return TEXT("failed");
		}
		return TEXT("missing");
	};

	auto DescribePostCompileVerificationState = [&Plan, &Decision]() -> FString
	{
		if (!Decision.bCompileProofRequired)
		{
			return TEXT("not_required");
		}
		if (Plan.CompileProof.LastPostCompileVerificationOutcome.Equals(TEXT("pass"), ESearchCase::IgnoreCase)
			|| Decision.bPostCompileVerificationPassed)
		{
			return TEXT("passed");
		}
		if (Plan.CompileProof.LastPostCompileVerificationOutcome.Equals(TEXT("fail"), ESearchCase::IgnoreCase)
			|| Decision.bPostCompileVerificationFailed)
		{
			return TEXT("failed");
		}
		return TEXT("pending");
	};

	auto DescribeRuntimeProofState = [&Plan, &Decision]() -> FString
	{
		if (!Decision.bFeatureWorkflowRequired || !Decision.bRuntimeProofRequired)
		{
			return TEXT("not_required");
		}
		if (Decision.bRuntimeProofPassed
			|| Plan.FeatureWorkflow.RuntimeProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase))
		{
			return TEXT("passed");
		}
		if (Decision.bRuntimeProofFailed
			|| Plan.FeatureWorkflow.RuntimeProofState.Equals(TEXT("failed"), ESearchCase::IgnoreCase)
			|| Plan.FeatureWorkflow.RuntimeProofState.Equals(TEXT("denied"), ESearchCase::IgnoreCase))
		{
			return TEXT("failed");
		}
		if (Decision.bProofPrerequisitesMissing
			|| Plan.FeatureWorkflow.RuntimeProofState.Equals(TEXT("blocked"), ESearchCase::IgnoreCase))
		{
			return TEXT("blocked");
		}
		return Plan.FeatureWorkflow.RuntimeProofState.IsEmpty()
			? FString(TEXT("pending"))
			: Plan.FeatureWorkflow.RuntimeProofState;
	};

	FString WhyText = BuildActivePlanCloseoutWarningText(Decision, FinalResponse);
	WhyText.RemoveFromStart(TEXT("[WARN] "));
	if (WhyText.IsEmpty())
	{
		WhyText = TEXT("Plugin-side terminal truth does not allow this task to close as `full`.");
	}

	FString Rewritten;
	Rewritten += TEXT("**Result**\n\n");
	Rewritten += FString::Printf(TEXT("`%s`\n\n"), *MapResultStatusToCloseoutLabel(Decision.ResultStatus));
	Rewritten += TEXT("**Truthful Closeout**\n\n");
	const FOsvayderUEUserFacingStatus UserFacingStatus =
		OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision);
	Rewritten += FString::Printf(TEXT("- user_status: %s\n"), *UserFacingStatus.Headline);
	Rewritten += FString::Printf(TEXT("- user_status_id: %s\n"), *UserFacingStatus.StatusId);
	Rewritten += FString::Printf(TEXT("- user_detail: %s\n"), *UserFacingStatus.Detail);
	Rewritten += TEXT("- Plugin-side closeout overrode the agent's `full` claim.\n");
	if (!Decision.GateReasonCode.IsEmpty())
	{
		Rewritten += FString::Printf(TEXT("- gate_reason: %s\n"), *Decision.GateReasonCode);
	}
	const FString FeatureWorkflowTerminalStatus = DescribeFeatureWorkflowTerminalStatus();
	if (!FeatureWorkflowTerminalStatus.IsEmpty())
	{
		Rewritten += FString::Printf(
			TEXT("- feature_workflow_terminal_status: %s\n"),
			*FeatureWorkflowTerminalStatus);
	}
	if (!Decision.BlockerFamily.IsEmpty())
	{
		Rewritten += FString::Printf(TEXT("- blocker_family: %s\n"), *Decision.BlockerFamily);
	}
	if (!Decision.BlockerDetail.IsEmpty())
	{
		Rewritten += FString::Printf(TEXT("- blocker_detail: %s\n"), *Decision.BlockerDetail);
	}
	if (!Plan.FeatureWorkflow.RuntimeProofState.IsEmpty())
	{
		Rewritten += FString::Printf(
			TEXT("- runtime_proof_state: %s\n"),
			*Plan.FeatureWorkflow.RuntimeProofState);
	}
	if (!Plan.FeatureWorkflow.ProofPrerequisiteState.IsEmpty())
	{
		Rewritten += FString::Printf(
			TEXT("- proof_prerequisite_state: %s\n"),
			*Plan.FeatureWorkflow.ProofPrerequisiteState);
	}

	Rewritten += TEXT("\n**Why This Is Not Full**\n\n");
	Rewritten += WhyText;
	Rewritten += TEXT("\n\n**Verification Checklist**\n\n");
	Rewritten += FString::Printf(TEXT("- compile proof: %s\n"), *DescribeCompileProofState());
	Rewritten += FString::Printf(
		TEXT("- post-compile verification: %s\n"),
		*DescribePostCompileVerificationState());
	if (Decision.bFeatureWorkflowRequired)
	{
		Rewritten += FString::Printf(TEXT("- runtime proof: %s"), *DescribeRuntimeProofState());
	}

	return Rewritten.TrimStartAndEnd();
}

void SOsvayderEditorWidget::CheckSelfVerificationMismatchAndWarn(
	const FString& FinalResponse,
	const EOsvayderUEProviderBackend RequestBackend)
{
	if (!DoesResponseClaimStatusFull(FinalResponse))
	{
		return;
	}
	FAgentCanonExecution ActiveCanon;
	if (!FOsvayderUEAgentTraceLog::Get().TryGetActiveCanonExecutionForBackend(RequestBackend, ActiveCanon))
	{
		// No active canon state observable — do not fabricate a mismatch.
		return;
	}
	if (ActiveCanon.bSelfVerificationAttempted
		&& ActiveCanon.SelfVerificationResult.Equals(TEXT("pass"), ESearchCase::IgnoreCase))
	{
		// Verification observed AND passed — the `full` claim is corroborated.
		return;
	}
	// Mismatch: `full` claim present, verification absent or failed.
	FString WarningText;
	WarningText += TEXT("[WARN] Agent self-verification mismatch (631 policy):\n");
	WarningText += TEXT("- Final response claims `full` completion.\n");
	if (!ActiveCanon.bSelfVerificationAttempted)
	{
		WarningText += TEXT("- But plugin-side trace inspection observed NO empirical verification tool invocation ");
		WarningText += TEXT("(livecoding_compile / execute_script / map_runtime_proof / multiplayer_audit / validate_blueprint / dependency_health / oss_session_proof / metadata_truth / cpp_reflection).\n");
	}
	else
	{
		WarningText += FString::Printf(TEXT("- Plugin-side verification-tool invocation observed, but outcome was `%s` (not `pass`).\n"),
			*ActiveCanon.SelfVerificationResult);
	}
	WarningText += TEXT("- Treat the completion claim as UNVERIFIED until you run a manual PIE/smoke test or automation check.\n");
	WarningText += TEXT("- See Docs/OsvayderUE/observed_failures.md + 631 policy in system prompt.");
	UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *WarningText);
	AddMessage(WarningText, /*bIsUser=*/false);
}

void SOsvayderEditorWidget::TryHandleCompileIntentPolicy(const FOsvayderStreamEvent& Event)
{
	// Only evaluate on command_execution tool-result events. Other tools
	// (livecoding_compile, restart_survival, cpp_reflection, etc.) are
	// already the redirect *targets* of the policy gate, so running the
	// gate against them would be self-referential noise.
	const bool bIsCommandExecution =
		Event.ToolName.Equals(TEXT("command_execution"), ESearchCase::IgnoreCase)
		|| Event.ToolName.EndsWith(TEXT("/command_execution"), ESearchCase::IgnoreCase)
		|| Event.ToolName.Contains(TEXT("command_execution"), ESearchCase::IgnoreCase);
	if (!bIsCommandExecution)
	{
		return;
	}

	// Build the 5 policy inputs from currently observable state.
	FCompileIntentPolicyInputs Inputs;
	Inputs.ToolName = Event.ToolName;
	Inputs.ToolCommandText = Event.ToolInput;
	// ChangeClassification: not yet plumbed at widget scope (the cpp_reflection
	// classifier output isn't cached per-turn anywhere observable here).
	// Pass empty -> gate will classify as Ambiguous when command is build-like,
	// which is the correct pre-classification advisory behavior per v3 spec.
	Inputs.ChangeClassification = FString();
	// Editor state: at widget-layer, the editor is by definition open -- the
	// widget itself is a Slate UI hosted inside UnrealEditor. If the host
	// process is running a commandlet / unattended automation, the widget
	// typically won't be constructed at all.
	Inputs.bEditorOpen = true;
	// Lane profile: resolve from the subsystem's configured default runtime
	// for this request. We don't currently have a per-turn profile override
	// at widget scope, so use ConfiguredDefaultRuntime unless a per-turn
	// override becomes available in a future stage.
	Inputs.LaneProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	// User intent: the prompt text of the current turn. Feeds the override
	// keyword scan ("rebuild everything", "don't use live coding", etc.).
	Inputs.UserIntentText = CurrentRequestPromptText;

	const FCompileIntentPolicyDecision Decision =
		FCompileIntentPolicyGate::EvaluateCompileIntent(Inputs);

	// Allow + inspection = Verbose (silent under ordinary logging).
	if (Decision.CommandClass == ECompileIntentCommandClass::Allow)
	{
		UE_LOG(LogOsvayderUE, Verbose,
			TEXT("[P3 policy] allow command_execution: compile_intent=%s detector_context=%s"),
			*Decision.CompileIntent,
			*Decision.DetectorContext);
		return;
	}

	// Non-Allow: surface at Log level so the transcript carries the advisory.
	UE_LOG(LogOsvayderUE, Log,
		TEXT("[P3 policy] command_execution routing=%s reason_code=%s redirect_to=%s compile_intent=%s detector_context=%s"),
		CompileIntentCommandClassToString(Decision.CommandClass),
		*Decision.DenyReasonCode,
		*Decision.RedirectTargetTool,
		*Decision.CompileIntent,
		*Decision.DetectorContext);

	// 626 P6-prep wiring (Option A): emit `policy_routing_decision` event into
	// agent_trace.jsonl so the transcript consumer / forensics / analysis
	// script can surface the decision post-hoc.
	{
		TSharedPtr<FJsonObject> RoutingJson = FCompileIntentPolicyGate::BuildPolicyRoutingJson(Decision);
		const EOsvayderUEProviderBackend Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
		FOsvayderUEAgentTraceLog::Get().AppendEvent(
			TEXT("policy_routing_decision"),
			Backend,
			RoutingJson,
			FOsvayderUEAgentTraceLog::Get().GetActiveRunIdForBackend(Backend));
	}

	// 626 P6-prep wiring (Option B): cache advisory text on the orchestrator
	// so the NEXT send injects it as a [POLICY ROUTING ADVISORY] context
	// block in the system prompt. This is the path that actually makes the
	// policy decision observable to the agent (vs log-only emission).
	{
		const FString AdvisoryText = FCompileIntentPolicyGate::BuildAgentAdvisoryText(Decision);
		if (!AdvisoryText.IsEmpty())
		{
			FOsvayderCodeSubsystem::Get().SetPendingPolicyRoutingAdvisory(AdvisoryText);
		}
	}
}

void SOsvayderEditorWidget::ResetVisibleChatState()
{
	if (ChatMessagesBox.IsValid())
	{
		ChatMessagesBox->ClearChildren();
	}
	VisibleChatMessages.Reset();

	bIsWaitingForResponse = false;
	LastResponse.Empty();
	LastResultStats.Empty();
	LastRuntimeConnectionState = EBackendRuntimeConnectionState::Unknown;
	LastRuntimeConnectionDetail.Empty();
	CurrentRequestPromptText.Empty();
	CurrentRequestImagePaths.Reset();
	ClearTransportRetryState();
	ClearPendingClosedEditorBuildBlockerIntervention();
	ResetStreamingState();
}

void SOsvayderEditorWidget::HandleConfiguredBackendChanged(const EOsvayderUEProviderBackend PreviousBackend)
{
	TSharedPtr<FJsonObject> SwitchPayload = MakeShared<FJsonObject>();
	SwitchPayload->SetStringField(TEXT("previous_backend"), OsvayderUEProviderBackendToString(PreviousBackend));
	SwitchPayload->SetStringField(TEXT("current_backend"), OsvayderUEProviderBackendToString(FOsvayderCodeSubsystem::Get().GetConfiguredBackend()));
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("backend_switch"),
		FOsvayderCodeSubsystem::Get().GetConfiguredBackend(),
		SwitchPayload,
		FOsvayderUEAgentTraceLog::Get().GetActiveRunIdForBackend(PreviousBackend));

	InvalidateActiveRequestCallbacks();
	ResetVisibleChatState();

	const FString PreviousDisplayName = PreviousBackend == EOsvayderUEProviderBackend::CodexCli
		? TEXT("Codex CLI")
		: TEXT("Claude CLI");
	const FString CurrentDisplayName = GetConfiguredBackendDisplayName();
	const FString SwitchNotice = FString::Printf(
		TEXT("Active assistant backend switched from %s to %s. Session history is now isolated per provider."),
		*PreviousDisplayName,
		*CurrentDisplayName);

	if (!RestoreCurrentBackendSession(true, SwitchNotice))
	{
		AddMessage(SwitchNotice, false);
		AddInitialStatusMessage();
	}
}

void SOsvayderEditorWidget::RenderConversationHistory(
	const TArray<TPair<FString, FString>>& History,
	const FString& IntroMessage,
	const FString& OutroMessage)
{
	if (ChatMessagesBox.IsValid())
	{
		ChatMessagesBox->ClearChildren();
	}
	VisibleChatMessages.Reset();

	if (!IntroMessage.IsEmpty())
	{
		AddMessage(IntroMessage, false);
	}

	for (const TPair<FString, FString>& Exchange : History)
	{
		AddMessage(Exchange.Key, true);
		AddMessage(Exchange.Value, false);
	}

	if (!OutroMessage.IsEmpty())
	{
		AddMessage(OutroMessage, false);
	}
}

void SOsvayderEditorWidget::AddInitialStatusMessage()
{
	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	const FString Snapshot = BuildBackendDebugSnapshot();
	LastBackendSnapshotSignature = BuildBackendSnapshotSignature();
	const FString DisplayName = GetBackendDisplayNameFromStatus(Status);

	FString Message;
	if (!Status.bAvailable || !CanSendPromptForStatus(Status))
	{
		Message = TEXT("Configured backend needs attention before the first prompt.\n\n");
		Message += Snapshot;
	}
	else
	{
		Message = FString::Printf(TEXT("Assistant ready on %s.\n\n"), *DisplayName);
		Message += TEXT("Shared chat UX is active, and session history is stored separately for each provider.\n\n");
		Message += Snapshot;
		Message += TEXT("\n\nType your question below and press Enter or click Send.");
	}

	AddMessage(Message, false);
	TrySurfacePersistedActivePlanNotice();

	TSharedPtr<FJsonObject> TracePayload = MakeShared<FJsonObject>();
	TracePayload->SetStringField(TEXT("snapshot_source"), TEXT("widget_initial_status"));
	TracePayload->SetStringField(TEXT("snapshot_signature"), LastBackendSnapshotSignature);
	TracePayload->SetStringField(TEXT("snapshot_text"), Snapshot);
	FOsvayderUEAgentTraceLog::Get().AppendEvent(TEXT("backend_snapshot"), Status.Backend, TracePayload);
}

void SOsvayderEditorWidget::RefreshBackendSnapshotIfChanged(bool bForce)
{
	if (!ChatMessagesBox.IsValid())
	{
		return;
	}

	const FString Signature = BuildBackendSnapshotSignature();
	if (!bForce && Signature == LastBackendSnapshotSignature)
	{
		return;
	}

	LastBackendSnapshotSignature = Signature;
	const FString Snapshot = BuildBackendDebugSnapshot();
	if (Snapshot.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> TracePayload = MakeShared<FJsonObject>();
	TracePayload->SetStringField(TEXT("snapshot_source"), bForce ? TEXT("widget_forced_refresh") : TEXT("widget_polled_update"));
	TracePayload->SetStringField(TEXT("snapshot_signature"), LastBackendSnapshotSignature);
	TracePayload->SetStringField(TEXT("snapshot_text"), Snapshot);
	TracePayload->SetBoolField(TEXT("visible_chat_message_emitted"), false);
	FOsvayderUEAgentTraceLog::Get().AppendEvent(TEXT("backend_snapshot"), GetConfiguredBackendStatus().Backend, TracePayload);
}

FString SOsvayderEditorWidget::BuildBackendDebugSnapshot() const
{
	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	const FString DisplayName = GetBackendDisplayNameFromStatus(Status);
	const FAgentSavedSessionIndex SavedSessions = FOsvayderCodeSubsystem::Get().DescribeSavedSessions();

	auto ToSingleLine = [](FString Value)
	{
		Value.ReplaceInline(TEXT("\r"), TEXT(" "));
		Value.ReplaceInline(TEXT("\n"), TEXT(" | "));
		while (Value.Contains(TEXT("  ")))
		{
			Value.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		Value.TrimStartAndEndInline();
		return Value;
	};

	FString Snapshot = TEXT("Backend debug:\n");
	Snapshot += FString::Printf(TEXT("backend = %s\n"), *DisplayName);

	if (Settings)
	{
		const bool bIsCodexBackend = Status.Backend == EOsvayderUEProviderBackend::CodexCli;
		const FString ModelLabel = Settings->GetConfiguredModelForBackend(Status.Backend);
		if (!ModelLabel.IsEmpty())
		{
			Snapshot += FString::Printf(TEXT("model = %s\n"), *ModelLabel);
		}

		if (bIsCodexBackend)
		{
			Snapshot += FString::Printf(TEXT("work_mode = %s\n"), *Settings->GetConfiguredCodexWorkModeName());
		}

		const FString SpeedMode = Settings->GetConfiguredSpeedModeLabelForBackend(Status.Backend);
		if (bIsCodexBackend && !SpeedMode.IsEmpty())
		{
			Snapshot += FString::Printf(TEXT("supports_speed_mode = %s\n"), Status.Capabilities.bSupportsSpeedModeControl ? TEXT("true") : TEXT("false"));
			Snapshot += FString::Printf(TEXT("speed_mode_requested = %s\n"), *SpeedMode);
			Snapshot += FString::Printf(TEXT("speed_mode_effective = %s\n"), *FCodexCliRunner::GetEffectiveSpeedModeName());
			Snapshot += FString::Printf(TEXT("speed_mode_support = %s\n"), *FCodexCliRunner::GetSpeedModeSupportLabel());
		}

		if (Status.Capabilities.bSupportsReasoningEffortControl)
		{
			const FString Reasoning = Settings->GetConfiguredCodexReasoningEffortName();
			Snapshot += TEXT("supports_reasoning_effort = true\n");
			Snapshot += FString::Printf(TEXT("reasoning_effort_requested = %s\n"), *Reasoning);
			Snapshot += FString::Printf(TEXT("reasoning_effort_effective = %s\n"), *Reasoning);
			Snapshot += FString::Printf(TEXT("reasoning_effort_support = %s\n"), *Settings->GetCodexReasoningSupportLabel());
		}

		if (Status.Capabilities.bSupportsVerbosityControl)
		{
			const FString Verbosity = Settings->GetConfiguredCodexVerbosityName();
			Snapshot += TEXT("supports_verbosity = true\n");
			Snapshot += FString::Printf(TEXT("verbosity_requested = %s\n"), *Verbosity);
			Snapshot += FString::Printf(TEXT("verbosity_effective = %s\n"), *Verbosity);
			Snapshot += FString::Printf(TEXT("verbosity_support = %s\n"), *Settings->GetCodexVerbositySupportLabel());
		}

		const FString ProfileLabel = Settings->GetConfiguredProfileLabelForBackend(Status.Backend);
		if (Status.Capabilities.bSupportsProfileSelection && !ProfileLabel.IsEmpty())
		{
			Snapshot += FString::Printf(TEXT("profile = %s\n"), *ProfileLabel);
		}

		Snapshot += FString::Printf(TEXT("auth = %s\n"), *Settings->GetConfiguredAuthModeLabelForBackend(Status.Backend));
		if (Status.Capabilities.bSupportsExplicitAuthModeSelection)
		{
			Snapshot += FString::Printf(TEXT("auth_path = %s\n"), *FCodexCliRunner::GetEffectiveAuthEntryPath());
			Snapshot += FString::Printf(TEXT("ownership = %s\n"), *FCodexCliRunner::GetEffectiveAuthOwnershipModel());
		}

		if (bIsCodexBackend)
		{
			const FCodexCliRunner::FCodexAuthDiagnostics AuthDiagnostics = FCodexCliRunner::GetAuthDiagnostics();
			Snapshot += FString::Printf(TEXT("codex_auth_diagnostic_state = %s\n"), *AuthDiagnostics.AuthState);
			Snapshot += FString::Printf(TEXT("codex_auth_configured_home = %s\n"), AuthDiagnostics.ConfiguredCodexHomePath.IsEmpty() ? TEXT("unresolved") : *AuthDiagnostics.ConfiguredCodexHomePath);
			Snapshot += FString::Printf(TEXT("codex_auth_effective_home = %s\n"), AuthDiagnostics.EffectiveCodexHomePath.IsEmpty() ? TEXT("unresolved") : *AuthDiagnostics.EffectiveCodexHomePath);
			Snapshot += FString::Printf(TEXT("codex_auth_home_resolution_source = %s\n"), *AuthDiagnostics.CodexHomeResolutionSource);
			Snapshot += FString::Printf(TEXT("codex_auth_credential_artifact = %s\n"), AuthDiagnostics.CredentialArtifactPath.IsEmpty() ? TEXT("none") : *AuthDiagnostics.CredentialArtifactPath);
			Snapshot += FString::Printf(TEXT("codex_auth_diagnostic_detail = %s\n"), *ToSingleLine(AuthDiagnostics.AuthDetailText));
			if (!LastCodexAuthProbeStatus.IsEmpty())
			{
				Snapshot += FString::Printf(TEXT("codex_auth_last_probe = %s\n"), *ToSingleLine(LastCodexAuthProbeStatus));
			}
		}

		if (Status.Capabilities.bSupportsProviderPersistentThreads)
		{
			Snapshot += FString::Printf(
				TEXT("persistent_thread_transport = %s\n"),
				FCodexCliRunner::ShouldUsePersistentConversationTransport() ? TEXT("enabled") : TEXT("disabled"));
		}

		Snapshot += FString::Printf(TEXT("dictation_runtime = %s\n"), *Settings->GetDictationRuntimeLabel());
		Snapshot += FString::Printf(TEXT("dictation_language_requested = %s\n"), *Settings->GetConfiguredDictationLanguageModeName());
		Snapshot += FString::Printf(TEXT("dictation_language_effective = %s\n"), *Settings->GetEffectiveDictationLanguageTag());
		Snapshot += FString::Printf(TEXT("dictation_language_display = %s\n"), *Settings->GetEffectiveDictationLanguageDisplayName());
		Snapshot += FString::Printf(TEXT("dictation_model = %s\n"), *Settings->GetEffectiveDictationModelName());
		Snapshot += FString::Printf(TEXT("dictation_supports_russian_offline = %s\n"), Settings->SupportsRussianOfflineDictation() ? TEXT("true") : TEXT("false"));
		Snapshot += FString::Printf(TEXT("dictation_support_claim = %s\n"), *Settings->GetDictationSupportClaimName());
		Snapshot += FString::Printf(TEXT("dictation_support_detail = %s\n"), *Settings->GetDictationSupportDetail());
	}

	Snapshot += FString::Printf(TEXT("provider_controls = %s\n"), *BuildProviderCapabilitySummary(Status.Capabilities));
	Snapshot += FString::Printf(TEXT("readiness = %s\n"), AgentBackendReadinessToString(Status.Readiness));
	Snapshot += FString::Printf(TEXT("auth_state = %s\n"), AgentBackendAuthStateToString(Status.AuthState));

	const FString Detail = ToSingleLine(Status.Detail);
	if (!Detail.IsEmpty())
	{
		Snapshot += FString::Printf(TEXT("detail = %s\n"), *Detail);
	}

	const FString AuthDetail = ToSingleLine(Status.AuthDetail);
	if (!AuthDetail.IsEmpty() && !AuthDetail.Equals(Detail, ESearchCase::CaseSensitive))
	{
		Snapshot += FString::Printf(TEXT("auth_detail = %s\n"), *AuthDetail);
	}

	if (SavedSessions.CurrentProviderSession.bHasSession)
	{
		Snapshot += TEXT("session_restore = current_provider_saved\n");
		Snapshot += FString::Printf(TEXT("session_restore_store_kind = %s\n"), *SavedSessions.CurrentProviderSession.StoreKind);
	}
	else if (SavedSessions.OtherProviderSession.bHasSession)
	{
		Snapshot += FString::Printf(
			TEXT("session_restore = separate_%s_session_available\n"),
			*ToSingleLine(GetSessionMetadataDisplayName(SavedSessions.OtherProviderSession)));
		Snapshot += FString::Printf(TEXT("session_restore_store_kind = %s\n"), *SavedSessions.OtherProviderSession.StoreKind);
	}
	else
	{
		Snapshot += TEXT("session_restore = none_saved_for_current_provider\n");
	}

	const FAgentSavedSessionIndex ExpertSessions =
		FOsvayderCodeSubsystem::Get().DescribeSavedSessionsForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);
	if (ExpertSessions.CurrentProviderSession.bHasSession)
	{
		Snapshot += FString::Printf(TEXT("explicit_expert_session_store_kind = %s\n"), *ExpertSessions.CurrentProviderSession.StoreKind);
	}

	if (SavedSessions.LegacySharedSession.bHasSession)
	{
		Snapshot += TEXT("legacy_session = blocked_shared_session_json\n");
	}

	const FString RestartSummary = FOsvayderUERestartSurvivalManager::BuildWidgetDebugSummary();
	if (!RestartSummary.IsEmpty())
	{
		Snapshot += RestartSummary + TEXT("\n");
	}

	if (Status.Capabilities.bSupportsBrowserVerifyLogin)
	{
		FString BrowserVerifyReason;
		const bool bCanLaunchBrowserVerify = FCodexCliRunner::CanLaunchBrowserVerify(BrowserVerifyReason);
		Snapshot += FString::Printf(
			TEXT("browser_verify = %s\n"),
			bCanLaunchBrowserVerify ? TEXT("available") : *ToSingleLine(BrowserVerifyReason));
	}

	const FString McpSummary = GenerateMCPCompactStatusSummary();
	if (!McpSummary.IsEmpty())
	{
		Snapshot += FString::Printf(TEXT("mcp = %s\n"), *McpSummary);
	}

	const FString BuildSummary = ToSingleLine(GetPluginBuildSyncSummary());
	if (!BuildSummary.IsEmpty())
	{
		Snapshot += FString::Printf(TEXT("build = %s\n"), *BuildSummary);
	}

	Snapshot.TrimEndInline();
	return Snapshot;
}

FString SOsvayderEditorWidget::BuildBackendSnapshotSignature() const
{
	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();

	auto ToSingleLine = [](FString Value)
	{
		Value.ReplaceInline(TEXT("\r"), TEXT(" "));
		Value.ReplaceInline(TEXT("\n"), TEXT(" | "));
		while (Value.Contains(TEXT("  ")))
		{
			Value.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		Value.TrimStartAndEndInline();
		return Value;
	};

	FString Signature = FString::Printf(
		TEXT("backend=%s|readiness=%s|auth_state=%s|available=%d|ready=%d"),
		*GetBackendDisplayNameFromStatus(Status),
		AgentBackendReadinessToString(Status.Readiness),
		AgentBackendAuthStateToString(Status.AuthState),
		Status.bAvailable ? 1 : 0,
		Status.bReady ? 1 : 0);

	if (Settings)
	{
		Signature += FString::Printf(TEXT("|model=%s"), *Settings->GetConfiguredModelForBackend(Status.Backend));
		Signature += FString::Printf(TEXT("|profile=%s"), *Settings->GetConfiguredProfileLabelForBackend(Status.Backend));
		Signature += FString::Printf(TEXT("|speed_requested=%s"), *Settings->GetConfiguredSpeedModeLabelForBackend(Status.Backend));
		if (Status.Backend == EOsvayderUEProviderBackend::CodexCli)
		{
			Signature += FString::Printf(TEXT("|work_mode=%s"), *Settings->GetConfiguredCodexWorkModeName());
			Signature += FString::Printf(TEXT("|speed_effective=%s"), *FCodexCliRunner::GetEffectiveSpeedModeName());
			Signature += FString::Printf(TEXT("|speed_support=%s"), *FCodexCliRunner::GetSpeedModeSupportLabel());
			const FCodexCliRunner::FCodexAuthDiagnostics AuthDiagnostics = FCodexCliRunner::GetAuthDiagnostics();
			Signature += FString::Printf(TEXT("|codex_auth_diag_state=%s"), *AuthDiagnostics.AuthState);
			Signature += FString::Printf(TEXT("|codex_auth_effective_home=%s"), *AuthDiagnostics.EffectiveCodexHomePath);
			Signature += FString::Printf(TEXT("|codex_auth_artifact=%s"), *AuthDiagnostics.CredentialArtifactPath);
			Signature += FString::Printf(TEXT("|codex_auth_probe=%s"), *LastCodexAuthProbeStatus);
		}
		Signature += FString::Printf(TEXT("|auth=%s"), *Settings->GetConfiguredAuthModeLabelForBackend(Status.Backend));
		Signature += FString::Printf(TEXT("|dictation_requested=%s"), *Settings->GetConfiguredDictationLanguageModeName());
		Signature += FString::Printf(TEXT("|dictation_effective=%s"), *Settings->GetEffectiveDictationLanguageTag());
		Signature += FString::Printf(TEXT("|dictation_model=%s"), *Settings->GetEffectiveDictationModelName());
		Signature += FString::Printf(TEXT("|dictation_claim=%s"), *Settings->GetDictationSupportClaimName());
	}

	const FAgentSavedSessionIndex SavedSessions = FOsvayderCodeSubsystem::Get().DescribeSavedSessions();
	const FAgentSavedSessionIndex ExpertSessions =
		FOsvayderCodeSubsystem::Get().DescribeSavedSessionsForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);
	Signature += FString::Printf(TEXT("|current_session_exists=%d"), SavedSessions.CurrentProviderSession.bHasSession ? 1 : 0);
	Signature += FString::Printf(TEXT("|current_session_store=%s"), *SavedSessions.CurrentProviderSession.StoreKind);
	Signature += FString::Printf(TEXT("|current_session_path=%s"), *SavedSessions.CurrentProviderSession.SessionFilePath);
	Signature += FString::Printf(TEXT("|other_session_exists=%d"), SavedSessions.OtherProviderSession.bHasSession ? 1 : 0);
	Signature += FString::Printf(TEXT("|other_session_store=%s"), *SavedSessions.OtherProviderSession.StoreKind);
	Signature += FString::Printf(TEXT("|legacy_session_exists=%d"), SavedSessions.LegacySharedSession.bHasSession ? 1 : 0);
	Signature += FString::Printf(TEXT("|expert_session_exists=%d"), ExpertSessions.CurrentProviderSession.bHasSession ? 1 : 0);
	Signature += FString::Printf(TEXT("|expert_session_store=%s"), *ExpertSessions.CurrentProviderSession.StoreKind);
	Signature += FString::Printf(TEXT("|expert_session_path=%s"), *ExpertSessions.CurrentProviderSession.SessionFilePath);

	FOsvayderUERestartSurvivalState RestartState;
	if (FOsvayderUERestartSurvivalManager::DescribeCurrentState(RestartState))
	{
		Signature += FString::Printf(TEXT("|restart_phase=%s"), OsvayderUERestartSurvivalPhaseToString(RestartState.Phase));
		Signature += FString::Printf(TEXT("|restart_task=%s"), *RestartState.TaskId);
		Signature += FString::Printf(TEXT("|restart_resume_pending=%d"), RestartState.bProviderThreadResumePending ? 1 : 0);
		Signature += FString::Printf(TEXT("|restart_session=%s"), *RestartState.ProviderSessionId);
	}
	else
	{
		Signature += TEXT("|restart_phase=none");
	}

	Signature += FString::Printf(
		TEXT("|caps=%d%d%d%d%d%d%d%d%d%d%d%d"),
		Status.Capabilities.bSupportsStreamingEvents ? 1 : 0,
		Status.Capabilities.bSupportsImages ? 1 : 0,
		Status.Capabilities.bSupportsCancellation ? 1 : 0,
		Status.Capabilities.bSupportsToolAllowList ? 1 : 0,
		Status.Capabilities.bUsesStructuredOutput ? 1 : 0,
		Status.Capabilities.bSupportsBrowserVerifyLogin ? 1 : 0,
		Status.Capabilities.bSupportsProviderPersistentThreads ? 1 : 0,
		Status.Capabilities.bSupportsReasoningEffortControl ? 1 : 0,
		Status.Capabilities.bSupportsVerbosityControl ? 1 : 0,
		Status.Capabilities.bSupportsSpeedModeControl ? 1 : 0,
		Status.Capabilities.bSupportsProfileSelection ? 1 : 0,
		Status.Capabilities.bSupportsExplicitAuthModeSelection ? 1 : 0);

	Signature += FString::Printf(TEXT("|detail=%s"), *ToSingleLine(Status.Detail));
	Signature += FString::Printf(TEXT("|auth_detail=%s"), *ToSingleLine(Status.AuthDetail));

	if (Status.Capabilities.bSupportsExplicitAuthModeSelection)
	{
		Signature += FString::Printf(TEXT("|auth_path=%s"), *FCodexCliRunner::GetEffectiveAuthEntryPath());
		Signature += FString::Printf(TEXT("|ownership=%s"), *FCodexCliRunner::GetEffectiveAuthOwnershipModel());
	}

	if (Status.Capabilities.bSupportsProviderPersistentThreads)
	{
		Signature += FString::Printf(
			TEXT("|persistent_thread_transport=%s"),
			FCodexCliRunner::ShouldUsePersistentConversationTransport() ? TEXT("enabled") : TEXT("disabled"));
	}

	if (Status.Capabilities.bSupportsBrowserVerifyLogin)
	{
		FString BrowserVerifyReason;
		const bool bCanLaunchBrowserVerify = FCodexCliRunner::CanLaunchBrowserVerify(BrowserVerifyReason);
		Signature += FString::Printf(
			TEXT("|browser_verify=%s"),
			bCanLaunchBrowserVerify ? TEXT("available") : *ToSingleLine(BrowserVerifyReason));
	}

	Signature += FString::Printf(TEXT("|mcp=%s"), *ToSingleLine(GenerateMCPCompactStatusSummary()));
	Signature += FString::Printf(TEXT("|build=%s"), *ToSingleLine(GetPluginBuildSyncSummary()));
	return Signature;
}

bool SOsvayderEditorWidget::IsClaudeAvailable() const
{
	return GetConfiguredBackendStatus().bAvailable;
}

void SOsvayderEditorWidget::LaunchBrowserVerifyLogin()
{
	if (!ShouldShowBrowserVerifyButton())
	{
		AddMessage(TEXT("The active provider does not support Browser Verify login."), false);
		return;
	}

	FString StatusMessage;
	if (FCodexCliRunner::LaunchBrowserVerifyLogin(StatusMessage))
	{
		AddMessage(StatusMessage, false);
	}
	else
	{
		AddMessage(StatusMessage.IsEmpty()
			? TEXT("Browser Verify launch failed.")
			: StatusMessage,
			false);
	}
}

void SOsvayderEditorWidget::OpenCodexAuthFolder()
{
	FString StatusMessage;
	if (!FCodexCliRunner::OpenEffectiveCodexAuthFolder(StatusMessage))
	{
		AddMessage(StatusMessage.IsEmpty() ? TEXT("Could not open Codex auth folder.") : StatusMessage, false);
		return;
	}

	AddMessage(StatusMessage, false);
}

void SOsvayderEditorWidget::LaunchCodexRelogin()
{
	FString StatusMessage;
	if (!FCodexCliRunner::LaunchCodexRelogin(StatusMessage))
	{
		AddMessage(StatusMessage.IsEmpty() ? TEXT("Codex relogin could not start.") : StatusMessage, false);
		return;
	}

	AddMessage(StatusMessage, false);
	RefreshBackendSnapshotIfChanged(true);
}

void SOsvayderEditorWidget::BackupAndClearCodexAuth()
{
	const FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
	const FString Prompt = FString::Printf(
		TEXT("Back up and clear known Codex auth artifacts from effective CODEX_HOME?\n\nCODEX_HOME: %s\nArtifact: %s\n\nOnly auth.json and credentials.json are touched. Backups are written under Project/Saved/OsvayderUE/CodexAuthDiagnostics first."),
		Diagnostics.EffectiveCodexHomePath.IsEmpty() ? TEXT("unresolved") : *Diagnostics.EffectiveCodexHomePath,
		Diagnostics.CredentialArtifactPath.IsEmpty() ? TEXT("none detected") : *Diagnostics.CredentialArtifactPath);

	if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Prompt)) != EAppReturnType::Yes)
	{
		AddMessage(TEXT("Codex auth clear canceled. No auth artifacts were changed."), false);
		return;
	}

	FString StatusMessage;
	if (!FCodexCliRunner::BackupAndClearStaleAuthArtifacts(StatusMessage))
	{
		AddMessage(StatusMessage.IsEmpty() ? TEXT("Codex auth backup/clear did not complete.") : StatusMessage, false);
		return;
	}

	AddMessage(StatusMessage, false);
	RefreshBackendSnapshotIfChanged(true);
}

void SOsvayderEditorWidget::ProbeCodexBackendAuth()
{
	if (!CanProbeCodexBackendAuth())
	{
		AddMessage(TEXT("Codex auth probe is already running or Codex is not the active provider."), false);
		return;
	}

	bCodexAuthProbeRunning = true;
	LastCodexAuthProbeStatus = TEXT("probe running");
	AddMessage(TEXT("Codex auth probe started. This does not send your prompt; it starts app-server and checks backend auth/transport readiness."), false);

	TWeakPtr<SOsvayderEditorWidget> SelfWeak = SharedThis(this).ToWeakPtr();
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [SelfWeak]()
	{
		FString StatusMessage;
		const bool bProbeOk = FCodexCliRunner::ProbeBackendAuth(StatusMessage);

		AsyncTask(ENamedThreads::GameThread, [SelfWeak, StatusMessage, bProbeOk]()
		{
			TSharedPtr<SOsvayderEditorWidget> Pinned = SelfWeak.Pin();
			if (!Pinned.IsValid())
			{
				return;
			}

			Pinned->bCodexAuthProbeRunning = false;
			Pinned->LastCodexAuthProbeStatus = StatusMessage;
			Pinned->AddMessage(
				StatusMessage.IsEmpty()
					? (bProbeOk ? TEXT("Codex auth probe completed.") : TEXT("Codex auth probe failed."))
					: StatusMessage,
				false);
			Pinned->RefreshBackendSnapshotIfChanged(true);
		});
	});
}

bool SOsvayderEditorWidget::CanSendPrompt() const
{
	return CanSendPromptForStatus(GetConfiguredBackendStatus());
}

bool SOsvayderEditorWidget::CanStartRestartSurvival() const
{
	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	return !bIsWaitingForResponse
		&& Status.Backend == EOsvayderUEProviderBackend::CodexCli
		&& CanSendPromptForStatus(Status)
		&& FCodexCliRunner::ShouldUsePersistentConversationTransport();
}

bool SOsvayderEditorWidget::ShouldShowBrowserVerifyButton() const
{
	return GetConfiguredBackendCapabilities().bSupportsBrowserVerifyLogin;
}

EVisibility SOsvayderEditorWidget::GetCodexAuthDiagnosticsVisibility() const
{
	return GetConfiguredBackendStatus().Backend == EOsvayderUEProviderBackend::CodexCli
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText SOsvayderEditorWidget::GetCodexAuthDiagnosticsText() const
{
	FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
	if (!LastCodexAuthProbeStatus.IsEmpty())
	{
		Diagnostics.ProbeDetailText = LastCodexAuthProbeStatus;
	}

	FString Text = FCodexCliRunner::BuildAuthDiagnosticsCompactText(Diagnostics);
	if (!LastCodexAuthProbeStatus.IsEmpty())
	{
		Text += FString::Printf(TEXT(" | last_probe=%s"), *LastCodexAuthProbeStatus);
	}
	return FText::FromString(Text);
}

FText SOsvayderEditorWidget::GetCodexAuthDiagnosticsToolTip() const
{
	FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
	if (!LastCodexAuthProbeStatus.IsEmpty())
	{
		Diagnostics.ProbeDetailText = LastCodexAuthProbeStatus;
	}
	return FText::FromString(FCodexCliRunner::BuildAuthDiagnosticsToolTip(Diagnostics));
}

bool SOsvayderEditorWidget::CanProbeCodexBackendAuth() const
{
	return !bCodexAuthProbeRunning
		&& GetConfiguredBackendStatus().Backend == EOsvayderUEProviderBackend::CodexCli;
}

FText SOsvayderEditorWidget::GetRestartSurvivalToolTip() const
{
	if (bIsWaitingForResponse)
	{
		return FText::FromString(TEXT("Wait for the current response to finish before starting restart survival."));
	}

	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	if (Status.Backend != EOsvayderUEProviderBackend::CodexCli)
	{
		return FText::FromString(TEXT("Restart survival is currently supported only on the ordinary Codex runtime path."));
	}

	if (!FCodexCliRunner::ShouldUsePersistentConversationTransport())
	{
		return FText::FromString(TEXT("Restart survival requires the persistent Codex app-server transport."));
	}

	if (!CanSendPromptForStatus(Status))
	{
		return FText::FromString(
			Status.Detail.IsEmpty()
				? TEXT("Configured backend is not ready to start restart survival.")
				: Status.Detail);
	}

	return FText::FromString(TEXT("Start a detached restart-survival handoff for the current Codex task. Unreal will close, continue bounded file/build/recovery work out of process, then relaunch and reattach."));
}

bool SOsvayderEditorWidget::CanLaunchBrowserVerifyButton() const
{
	if (!ShouldShowBrowserVerifyButton())
	{
		return false;
	}

	FString Reason;
	return FCodexCliRunner::CanLaunchBrowserVerify(Reason);
}

FText SOsvayderEditorWidget::GetBrowserVerifyToolTip() const
{
	if (!ShouldShowBrowserVerifyButton())
	{
		return FText::FromString(TEXT("Browser Verify is not supported by the active provider."));
	}

	FString Reason;
	FCodexCliRunner::CanLaunchBrowserVerify(Reason);
	return FText::FromString(Reason);
}

FText SOsvayderEditorWidget::GetToolbarTitleText() const
{
	return FText::FromString(TEXT("Osvayder UE"));
}

FText SOsvayderEditorWidget::GetProviderSummaryText() const
{
	return FText::FromString(BuildProviderSummaryText(GetConfiguredBackendStatus()));
}

FText SOsvayderEditorWidget::GetProviderSummaryToolTip() const
{
	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	const FString DisplayName = GetBackendDisplayNameFromStatus(Status);
	const FString CapabilitySummary = BuildProviderCapabilitySummary(Status.Capabilities);
	const FString StatusDetail = NormalizeStatusDetail(Status.Detail);
	const FString AuthDetail = NormalizeStatusDetail(Status.AuthDetail);
	const FString McpSummary = NormalizeStatusDetail(GenerateMCPCompactStatusSummary());

	FString ToolTip = FString::Printf(
		TEXT("%s status: %s."),
		*DisplayName,
		*BuildProviderSummaryText(Status));

	if (!CapabilitySummary.IsEmpty())
	{
		ToolTip += FString::Printf(TEXT("\nControls: %s."), *CapabilitySummary);
	}

	if (!StatusDetail.IsEmpty())
	{
		ToolTip += FString::Printf(TEXT("\nDetail: %s"), *StatusDetail);
	}

	if (!AuthDetail.IsEmpty() && !AuthDetail.Equals(StatusDetail, ESearchCase::CaseSensitive))
	{
		ToolTip += FString::Printf(TEXT("\nAuth: %s"), *AuthDetail);
	}

	if (!McpSummary.IsEmpty())
	{
		ToolTip += FString::Printf(TEXT("\nMCP: %s"), *McpSummary);
	}

	return FText::FromString(ToolTip);
}

FText SOsvayderEditorWidget::GetRestoreSessionToolTip() const
{
	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	const FString DisplayName = GetBackendDisplayNameFromStatus(Status);
	const FAgentSavedSessionIndex SavedSessions = FOsvayderCodeSubsystem::Get().DescribeSavedSessions();
	const FAgentSavedSessionIndex ExpertSessions =
		FOsvayderCodeSubsystem::Get().DescribeSavedSessionsForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);

	if (SavedSessions.CurrentProviderSession.bHasSession)
	{
		return FText::FromString(FString::Printf(
			TEXT("Restore the last saved project-local visible %s session from %s."),
			*DisplayName,
			*SavedSessions.CurrentProviderSession.SessionFilePath));
	}

	if (SavedSessions.OtherProviderSession.bHasSession)
	{
		return FText::FromString(FString::Printf(
			TEXT("No saved project-local visible %s session exists. A separate project-local visible %s session is available after switching providers."),
			*DisplayName,
			*GetSessionMetadataDisplayName(SavedSessions.OtherProviderSession)));
	}

	if (ExpertSessions.CurrentProviderSession.bHasSession)
	{
		return FText::FromString(FString::Printf(
			TEXT("The configured workspace-write default %s runtime restores its own project-local visible history. The separate normal provider session remains expert-only at %s."),
			*DisplayName,
			*ExpertSessions.CurrentProviderSession.SessionFilePath));
	}

	if (SavedSessions.LegacySharedSession.bHasSession)
	{
		return FText::FromString(FString::Printf(
			TEXT("Legacy shared session detected at %s, but it is blocked because provider ownership is unsafe."),
			*SavedSessions.LegacySharedSession.SessionFilePath));
	}

	return FText::FromString(FString::Printf(TEXT("No saved %s session is available."), *DisplayName));
}

FText SOsvayderEditorWidget::GetStatusText() const
{
	if (bIsWaitingForResponse)
	{
		double ElapsedSec = FPlatformTime::Seconds() - StreamingStartTime;
		FString StatusStr = FString::Printf(TEXT("[working in editor] %s is working... %.1fs"), *GetConfiguredBackendDisplayName(), ElapsedSec);

		if (StreamingToolCallCount > 0)
		{
			StatusStr += FString::Printf(TEXT(" | %d tool%s"),
				StreamingToolCallCount, StreamingToolCallCount != 1 ? TEXT("s") : TEXT(""));
		}

		return FText::FromString(StatusStr);
	}

	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	const EBackendRuntimeConnectionState RuntimeState = GetRuntimeConnectionState();

	if (RuntimeState == EBackendRuntimeConnectionState::Failed)
	{
		return FText::FromString(TEXT("[failed] Last backend request failed"));
	}

	if (RuntimeState == EBackendRuntimeConnectionState::TransportReset)
	{
		return FText::FromString(BuildTransportRetryStatusLabel(LastTransportFailureState.bRetrySafe));
	}

	if (RuntimeState == EBackendRuntimeConnectionState::Connected)
	{
		if (!LastResultStats.IsEmpty())
		{
			return FText::FromString(FString::Printf(TEXT("[connected] %s"), *LastResultStats));
		}

		return FText::FromString(FString::Printf(TEXT("[connected] %s responded successfully"), *GetConfiguredBackendDisplayName()));
	}

	if (!LastResultStats.IsEmpty())
	{
		return FText::FromString(FString::Printf(TEXT("[done] %s"), *LastResultStats));
	}

	return GetIdleBackendStatusText(Status);
}

FSlateColor SOsvayderEditorWidget::GetStatusColor() const
{
	return GetConnectionIndicatorColor();
}

SOsvayderEditorWidget::EBackendRuntimeConnectionState SOsvayderEditorWidget::GetRuntimeConnectionState() const
{
	if (bIsWaitingForResponse)
	{
		return EBackendRuntimeConnectionState::Connecting;
	}

	if (LastRuntimeConnectionState == EBackendRuntimeConnectionState::Connected ||
		LastRuntimeConnectionState == EBackendRuntimeConnectionState::Failed ||
		LastRuntimeConnectionState == EBackendRuntimeConnectionState::TransportReset)
	{
		return LastRuntimeConnectionState;
	}

	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	switch (Status.Readiness)
	{
	case EAgentBackendReadiness::Ready:
		return EBackendRuntimeConnectionState::Connected;

	case EAgentBackendReadiness::AvailableAuthUnknown:
		return EBackendRuntimeConnectionState::Unknown;

	case EAgentBackendReadiness::AvailableNotAuthenticated:
	case EAgentBackendReadiness::NotAvailable:
	default:
		return EBackendRuntimeConnectionState::Failed;
	}
}

FSlateColor SOsvayderEditorWidget::GetConnectionIndicatorColor() const
{
	switch (GetRuntimeConnectionState())
	{
	case EBackendRuntimeConnectionState::Connected:
		return FSlateColor(FLinearColor(0.2f, 0.9f, 0.35f));

	case EBackendRuntimeConnectionState::Connecting:
		return FSlateColor(FLinearColor(1.0f, 0.7f, 0.15f));

	case EBackendRuntimeConnectionState::Failed:
		return FSlateColor(FLinearColor(1.0f, 0.25f, 0.25f));

	case EBackendRuntimeConnectionState::TransportReset:
		return FSlateColor(FLinearColor(0.95f, 0.55f, 0.18f));

	case EBackendRuntimeConnectionState::Unknown:
	default:
		return FSlateColor(FLinearColor(1.0f, 0.7f, 0.15f));
	}
}

FText SOsvayderEditorWidget::GetConnectionIndicatorToolTip() const
{
	const EBackendRuntimeConnectionState RuntimeState = GetRuntimeConnectionState();
	FString Prefix;

	switch (RuntimeState)
	{
	case EBackendRuntimeConnectionState::Connected:
		Prefix = TEXT("Connected");
		break;

	case EBackendRuntimeConnectionState::Connecting:
		Prefix = TEXT("Connecting");
		break;

	case EBackendRuntimeConnectionState::Failed:
		Prefix = TEXT("Disconnected / failed");
		break;

	case EBackendRuntimeConnectionState::TransportReset:
		Prefix = TEXT("Backend transport reset");
		break;

	case EBackendRuntimeConnectionState::Unknown:
	default:
		Prefix = TEXT("Connection not yet proven");
		break;
	}

	if (!LastRuntimeConnectionDetail.IsEmpty())
	{
		return FText::FromString(FString::Printf(TEXT("%s\n%s"), *Prefix, *LastRuntimeConnectionDetail));
	}

	return FText::FromString(Prefix);
}

bool SOsvayderEditorWidget::CanRetryLastTransportRequest() const
{
	return LastTransportFailureState.bActive
		&& LastTransportFailureState.bRetrySafe
		&& !LastTransportFailureState.Prompt.IsEmpty()
		&& !bIsWaitingForResponse;
}

EVisibility SOsvayderEditorWidget::GetRetryLastTransportVisibility() const
{
	return GetRuntimeConnectionState() == EBackendRuntimeConnectionState::TransportReset
		&& LastTransportFailureState.bActive
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText SOsvayderEditorWidget::GetRetryLastTransportToolTip() const
{
	if (CanRetryLastTransportRequest())
	{
		return FText::FromString(TEXT("Replay the exact last prompt on a fresh backend session while preserving the current active plan/workflow context."));
	}

	return FText::FromString(FString::Printf(
		TEXT("Retry Last is blocked because %s."),
		*DescribeTransportRetryBlockReason(LastTransportFailureState.RetryBlockReason)));
}

FReply SOsvayderEditorWidget::HandleRetryLastTransportRequest()
{
	if (!CanRetryLastTransportRequest())
	{
		return FReply::Handled();
	}

	FOsvayderUEAgentTraceLog::Get().MarkTransportRetryAttempt(
		FOsvayderCodeSubsystem::Get().GetConfiguredBackend(),
		TEXT("widget_retry_last"));

	PendingTransportRetryReplayContext.bActive = true;
	PendingTransportRetryReplayContext.SourceRunId = LastTransportFailureState.SourceRunId;
	PendingTransportRetryReplayContext.FailureFamily = LastTransportFailureState.FailureFamily;
	bResumeExistingActivePlanOnNextSend = true;
	bUsePostReattachResumePolicyOnNextSend = false;
	AddMessage(TEXT("Retrying the exact last prompt on a fresh backend session after the transport reset."), false);
	CurrentInputText = LastTransportFailureState.Prompt;
	SendMessage();
	return FReply::Handled();
}

void SOsvayderEditorWidget::ResetStreamingState()
{
	StreamingResponse.Empty();
	CurrentSegmentText.Empty();
	StreamingTextBlock.Reset();
	StreamingContentBox.Reset();
	ToolCallStatusLabels.Empty();
	ToolCallResultTexts.Empty();
	ToolCallExpandables.Empty();
	ToolCallNames.Empty();
	AllTextSegments.Empty();
	TextSegmentBlocks.Empty();
	TextSegmentContainers.Empty();
	ToolGroupExpandArea.Reset();
	ToolGroupInnerBox.Reset();
	ToolGroupSummaryText.Reset();
	ToolGroupCount = 0;
	ToolGroupDoneCount = 0;
	ToolGroupCallIds.Empty();
	StreamingToolCallCount = 0;
	LastResultStats.Empty();
}

void SOsvayderEditorWidget::StartStreamingResponse()
{
	ResetStreamingState();
	StreamingStartTime = FPlatformTime::Seconds();

	if (ChatMessagesBox.IsValid())
	{
		// Add separator before streaming response
		if (ChatMessagesBox->NumSlots() > 0)
		{
			ChatMessagesBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(8.0f, 2.0f))
			[
			SNew(SSeparator)
			.ColorAndOpacity(FLinearColor(0.090f, 0.105f, 0.130f, 0.8f))
			];
		}

		// Create the first text segment container
		TSharedPtr<SVerticalBox> FirstSegmentContainer;

		// Build the inner content box with role label + first text segment
		SAssignNew(StreamingContentBox, SVerticalBox)

		// Role label
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 0, 0, 6)
		[
					SNew(STextBlock)
					.Text(FText::FromString(GetConfiguredBackendDisplayName()))
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(OsvayderSlateStyle::AccentVioletText())
		]

		// First text segment (wrapped in container for code block replacement)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(FirstSegmentContainer, SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(StreamingTextBlock, SMultiLineEditableText)
				.Text(FText::FromString(TEXT("Thinking...")))
				.TextStyle(&GetSelectableNormalStyle())
				.IsReadOnly(true)
				.AutoWrapText(true)
			]
		];

		TextSegmentBlocks.Add(StreamingTextBlock);
		TextSegmentContainers.Add(FirstSegmentContainer);

		// Wrap content box in accent bar + border (matching SChatMessage style)
		ChatMessagesBox->AddSlot()
		.AutoHeight()
		.Padding(FMargin(4.0f, 6.0f, 4.0f, 6.0f))
		[
			SNew(SHorizontalBox)

			// Left accent bar (orange for Claude)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(OsvayderSlateStyle::Color::Violet())
				.Padding(FMargin(2.0f, 0.0f))
				[
					SNullWidget::NullWidget
				]
			]

			// Message body containing the dynamic content box
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SBorder)
				.BorderImage(OsvayderSlateStyle::AssistantMessageBrush())
				.Padding(FMargin(12.0f, 9.0f, 12.0f, 10.0f))
				[
					StreamingContentBox.ToSharedRef()
				]
			]
		];

		// Scroll to bottom
		if (ChatScrollBox.IsValid())
		{
			ChatScrollBox->ScrollToEnd();
		}

	}
}

void SOsvayderEditorWidget::OnClaudeProgress(
	const FString& PartialOutput,
	const uint64 RequestGeneration,
	const EOsvayderUEProviderBackend RequestBackend)
{
	if (!IsActiveRequestCallback(RequestGeneration, RequestBackend, TEXT("progress")))
	{
		return;
	}

	// Append to total and current segment
	StreamingResponse += PartialOutput;
	CurrentSegmentText += PartialOutput;

	// Update the current text segment block
	if (StreamingTextBlock.IsValid())
	{
		StreamingTextBlock->SetText(FText::FromString(CurrentSegmentText));
	}

	// Auto-scroll to bottom as content streams in
	if (ChatScrollBox.IsValid())
	{
		ChatScrollBox->ScrollToEnd();
	}
}

void SOsvayderEditorWidget::OnClaudeStreamEvent(
	const FOsvayderStreamEvent& Event,
	const uint64 RequestGeneration,
	const EOsvayderUEProviderBackend RequestBackend)
{
	if (!IsActiveRequestCallback(RequestGeneration, RequestBackend, TEXT("stream_event")))
	{
		return;
	}

	switch (Event.Type)
	{
	case EOsvayderStreamEventType::SessionInit:
		UE_LOG(LogOsvayderUE, Log, TEXT("[StreamEvent] SessionInit: session_id=%s"), *Event.SessionId);
		break;

	case EOsvayderStreamEventType::TextContent:
		UE_LOG(LogOsvayderUE, Log, TEXT("[StreamEvent] TextContent: %d chars"), Event.Text.Len());
		break;

	case EOsvayderStreamEventType::ToolUse:
		UE_LOG(LogOsvayderUE, Log, TEXT("[StreamEvent] ToolUse: %s (id=%s)"), *Event.ToolName, *Event.ToolCallId);
		HandleToolUseEvent(Event);
		break;

	case EOsvayderStreamEventType::ToolResult:
		UE_LOG(LogOsvayderUE, Log, TEXT("[StreamEvent] ToolResult: tool_id=%s, %d chars"),
			*Event.ToolCallId, Event.ToolResultContent.Len());
		HandleToolResultEvent(Event, RequestBackend);
		break;

	case EOsvayderStreamEventType::Result:
		UE_LOG(LogOsvayderUE, Log, TEXT("[StreamEvent] Result: error=%d, duration=%dms, turns=%d, cost=$%.4f"),
			Event.bIsError, Event.DurationMs, Event.NumTurns, Event.TotalCostUsd);
		HandleResultEvent(Event);
		break;

	default:
		UE_LOG(LogOsvayderUE, Log, TEXT("[StreamEvent] Unknown type: %d"), static_cast<int32>(Event.Type));
		break;
	}
}

void SOsvayderEditorWidget::InvalidateActiveRequestCallbacks()
{
	ActiveRequestGeneration = ++NextRequestGeneration;
	ActiveRequestBackend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
	// 626 P1 crash-safety: reset the one-shot completion guard for the next
	// request generation. FinalizeStreamingResponse() consults this flag to
	// no-op on double-fire; resetting here ensures the next request can
	// legitimately finalize once.
	bHasCompletedResponseDelivery = false;
}

bool SOsvayderEditorWidget::IsActiveRequestCallback(
	const uint64 RequestGeneration,
	const EOsvayderUEProviderBackend RequestBackend,
	const TCHAR* CallbackKind) const
{
	if (RequestGeneration != ActiveRequestGeneration)
	{
		UE_LOG(
			LogOsvayderUE,
			Log,
			TEXT("Ignoring stale %s callback due to request generation mismatch. callback_generation=%llu active_generation=%llu request_backend=%s current_backend=%s"),
			CallbackKind,
			static_cast<unsigned long long>(RequestGeneration),
			static_cast<unsigned long long>(ActiveRequestGeneration),
			OsvayderUEProviderBackendToString(RequestBackend),
			OsvayderUEProviderBackendToString(FOsvayderCodeSubsystem::Get().GetConfiguredBackend()));
		return false;
	}

	if (RequestBackend != FOsvayderCodeSubsystem::Get().GetConfiguredBackend())
	{
		UE_LOG(
			LogOsvayderUE,
			Log,
			TEXT("Ignoring stale %s callback because the active provider changed. request_backend=%s current_backend=%s"),
			CallbackKind,
			OsvayderUEProviderBackendToString(RequestBackend),
			OsvayderUEProviderBackendToString(FOsvayderCodeSubsystem::Get().GetConfiguredBackend()));
		return false;
	}

	return true;
}

void SOsvayderEditorWidget::FinalizeStreamingResponse()
{
	// 626 P1 crash-safety: one-shot guard. If a streaming completion path +
	// final response path both race into Finalize, the shared_ptr controllers
	// referenced by TextSegmentBlocks / TextSegmentContainers would be
	// double-released (crash stack: ReleaseSharedReferenceNoInline ->
	// ParseAndRenderCodeBlocks:4721). The guard turns the second call into
	// a Warning log + no-op. Reset happens in InvalidateActiveRequestCallbacks
	// at the start of the next request generation.
	if (bHasCompletedResponseDelivery)
	{
		UE_LOG(LogOsvayderUE, Warning,
			TEXT("FinalizeStreamingResponse: ignored double-finalize for the same request generation (one-shot guard). See 626 P1."));
		return;
	}
	bHasCompletedResponseDelivery = true;

	// Save the final text segment
	AllTextSegments.Add(CurrentSegmentText);

	// Rebuild StreamingResponse from all segments for copy support
	FString Rebuilt;
	for (const FString& Segment : AllTextSegments)
	{
		Rebuilt += Segment;
	}
	if (!Rebuilt.IsEmpty())
	{
		StreamingResponse = Rebuilt;
	}

	// For simple single-segment responses (no tool events), ensure text block is up to date
	if (StreamingTextBlock.IsValid() && !StreamingResponse.IsEmpty() && TextSegmentBlocks.Num() <= 1)
	{
		StreamingTextBlock->SetText(FText::FromString(StreamingResponse));
	}

	LastResponse = StreamingResponse;

	// Post-process text segments to render code blocks
	ParseAndRenderCodeBlocks();

	// Clear all streaming state (except StreamingResponse which is used by OnClaudeResponse).
	// 626 P1 defense-in-depth: clearing the shared_ptr arrays here releases
	// the last references we hold on the text-block / container controllers
	// in a well-defined order. If Finalize were called again despite the
	// one-shot guard, the subsequent ParseAndRenderCodeBlocks call would
	// iterate empty arrays (no-op).
	StreamingTextBlock.Reset();
	StreamingContentBox.Reset();
	CurrentSegmentText.Empty();
	ToolCallStatusLabels.Empty();
	ToolCallResultTexts.Empty();
	ToolCallExpandables.Empty();
	ToolCallNames.Empty();
	AllTextSegments.Empty();
	TextSegmentBlocks.Empty();
	TextSegmentContainers.Empty();
	ToolGroupExpandArea.Reset();
	ToolGroupInnerBox.Reset();
	ToolGroupSummaryText.Reset();
	ToolGroupCount = 0;
	ToolGroupDoneCount = 0;
	ToolGroupCallIds.Empty();
}

void SOsvayderEditorWidget::HandleToolUseEvent(const FOsvayderStreamEvent& Event)
{
	if (!StreamingContentBox.IsValid())
	{
		return;
	}

	UpdateActivePlanForToolBoundary(Event, true);

	// Track tool call count for status bar
	StreamingToolCallCount++;

	// Store tool name for later lookup
	ToolCallNames.Add(Event.ToolCallId, Event.ToolName);

	FString DisplayName = GetDisplayToolName(Event.ToolName);

	// Check if this is a consecutive tool (no text since last tool = same group)
	bool bIsConsecutive = CurrentSegmentText.IsEmpty() && ToolGroupInnerBox.IsValid();

	if (!bIsConsecutive)
	{
		// Freeze the current text segment
		AllTextSegments.Add(CurrentSegmentText);
		CurrentSegmentText.Empty();

		// Collapse empty text segment
		if (AllTextSegments.Last().IsEmpty() && TextSegmentContainers.Num() > 0)
		{
			TextSegmentContainers.Last()->SetVisibility(EVisibility::Collapsed);
		}

		// Start a new tool group
		ToolGroupCount = 0;
		ToolGroupDoneCount = 0;
		ToolGroupCallIds.Empty();

		StreamingContentBox->AddSlot()
		.AutoHeight()
		.Padding(0, 3, 0, 3)
		[
			SNew(SBorder)
			.BorderImage(OsvayderSlateStyle::ToolGroupBrush())
			.Padding(FMargin(5.0f, 3.0f))
			[
				SAssignNew(ToolGroupExpandArea, SExpandableArea)
				.InitiallyCollapsed(false)
				.HeaderPadding(FMargin(5.0f, 2.0f))
				.HeaderContent()
				[
					SAssignNew(ToolGroupSummaryText, STextBlock)
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(GetToolRunningStatusColor())
				]
				.BodyContent()
				[
					SAssignNew(ToolGroupInnerBox, SVerticalBox)
				]
			]
		];

		// Create a new text segment for text after this tool group
		TSharedPtr<SVerticalBox> NewSegmentContainer;

		StreamingContentBox->AddSlot()
		.AutoHeight()
		[
			SAssignNew(NewSegmentContainer, SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(StreamingTextBlock, SMultiLineEditableText)
				.Text(FText::GetEmpty())
				.TextStyle(&GetSelectableNormalStyle())
				.IsReadOnly(true)
				.AutoWrapText(true)
			]
		];

		TextSegmentBlocks.Add(StreamingTextBlock);
		TextSegmentContainers.Add(NewSegmentContainer);
	}
	else
	{
		// Consecutive tool - transition from single to grouped display
		if (ToolGroupCount == 1)
		{
			// Collapse the group (was expanded for single tool)
			if (ToolGroupExpandArea.IsValid())
			{
				ToolGroupExpandArea->SetExpanded(false);
			}

			// Show the first tool's inner status label (was hidden for single-tool display)
			if (ToolGroupCallIds.Num() > 0)
			{
				TSharedPtr<STextBlock>* FirstStatusPtr = ToolCallStatusLabels.Find(ToolGroupCallIds[0]);
				if (FirstStatusPtr && FirstStatusPtr->IsValid())
				{
					(*FirstStatusPtr)->SetVisibility(EVisibility::Visible);
				}
			}
		}
	}

	// Add tool entry to the current group
	ToolGroupCount++;
	ToolGroupCallIds.Add(Event.ToolCallId);

	TSharedPtr<STextBlock> StatusLabel;
	TSharedPtr<SMultiLineEditableText> ResultText;
	TSharedPtr<SExpandableArea> ExpandArea;
	TSharedPtr<SOsvayderToolCallRow> ToolRow;

	ToolGroupInnerBox->AddSlot()
	.AutoHeight()
	.Padding(FMargin(2.0f, 2.0f))
	[
		SAssignNew(ToolRow, SOsvayderToolCallRow)
		.ToolName(FText::FromString(DisplayName))
		.StatusText(FText::FromString(TEXT("Running")))
		.StatusColor(GetToolRunningStatusColor())
		.ResultText(FText::GetEmpty())
		.bResultVisible(false)
	];

	StatusLabel = ToolRow->GetStatusLabel();
	ResultText = ToolRow->GetResultTextBlock();
	ExpandArea = ToolRow->GetExpandableArea();

	ToolCallStatusLabels.Add(Event.ToolCallId, StatusLabel);
	ToolCallResultTexts.Add(Event.ToolCallId, ResultText);
	ToolCallExpandables.Add(Event.ToolCallId, ExpandArea);

	// Update group summary header
	UpdateToolGroupSummary();

	if (ChatScrollBox.IsValid())
	{
		ChatScrollBox->ScrollToEnd();
	}
}

void SOsvayderEditorWidget::HandleToolResultEvent(
	const FOsvayderStreamEvent& Event,
	const EOsvayderUEProviderBackend RequestBackend)
{
	// Look up tool name
	const FString* ToolNamePtr = ToolCallNames.Find(Event.ToolCallId);
	FString ToolName = ToolNamePtr ? GetDisplayToolName(*ToolNamePtr) : TEXT("Tool");

	// Update status label to show completion
	TSharedPtr<STextBlock>* StatusLabelPtr = ToolCallStatusLabels.Find(Event.ToolCallId);
	if (StatusLabelPtr && StatusLabelPtr->IsValid())
	{
		const bool bBlocked = !Event.bIsError && ToolResultLooksBlocked(Event.ToolResultContent);
		const FSlateColor StatusColor = Event.bIsError
			? GetToolFailedStatusColor()
			: (bBlocked ? GetToolBlockedStatusColor() : GetToolCompletedStatusColor());
		const FString StatusWord = Event.bIsError ? TEXT("Failed") : (bBlocked ? TEXT("Blocked") : TEXT("Completed"));
		(*StatusLabelPtr)->SetText(FText::FromString(FString::Printf(TEXT("%s: %s"), *StatusWord, *ToolName)));
		(*StatusLabelPtr)->SetColorAndOpacity(StatusColor);
	}

	// Set result text (truncated for display)
	TSharedPtr<SMultiLineEditableText>* ResultTextPtr = ToolCallResultTexts.Find(Event.ToolCallId);
	if (ResultTextPtr && ResultTextPtr->IsValid())
	{
		FString ResultContent = Event.ToolResultContent;
		if (ResultContent.Len() > 2000)
		{
			ResultContent = ResultContent.Left(2000) + TEXT("\n... (truncated)");
		}
		(*ResultTextPtr)->SetText(FText::FromString(ResultContent));
	}

	// Make expandable area visible
	TSharedPtr<SExpandableArea>* ExpandPtr = ToolCallExpandables.Find(Event.ToolCallId);
	if (ExpandPtr && ExpandPtr->IsValid())
	{
		(*ExpandPtr)->SetVisibility(EVisibility::Visible);
	}

	// Update group summary
	ToolGroupDoneCount++;
	UpdateToolGroupSummary();
	UpdateActivePlanForToolBoundary(Event, false);

	TryHandleClosedEditorBuildBlocker(Event, RequestBackend);

	// 626 P3: compile-intent policy annotation. Runs AFTER the detector so the
	// detector decision (which arms restart_survival when a real UBT/LC blocker
	// is observed) is not disturbed. Policy gate only emits telemetry + log
	// advisory for the agent's transcript; it does not block Codex dispatch
	// because command_execution is Codex-owned.
	TryHandleCompileIntentPolicy(Event);

	if (ChatScrollBox.IsValid())
	{
		ChatScrollBox->ScrollToEnd();
	}
}

void SOsvayderEditorWidget::HandleResultEvent(const FOsvayderStreamEvent& Event)
{
	if (!StreamingContentBox.IsValid())
	{
		return;
	}

	// Collapse empty trailing text block if no text followed the last tool
	if (CurrentSegmentText.IsEmpty() && TextSegmentContainers.Num() > 0)
	{
		TextSegmentContainers.Last()->SetVisibility(EVisibility::Collapsed);
	}

	// Format stats footer
	float DurationSec = Event.DurationMs / 1000.0f;
	FString StatsText = FString::Printf(TEXT("Done in %.1fs"), DurationSec);

	if (Event.NumTurns > 0)
	{
		StatsText += FString::Printf(TEXT(" | %d turn%s"),
			Event.NumTurns, Event.NumTurns != 1 ? TEXT("s") : TEXT(""));
	}

	if (Event.TotalCostUsd > 0.0f)
	{
		StatsText += FString::Printf(TEXT(" | $%.4f"), Event.TotalCostUsd);
	}

	// Store final stats for the status bar
	LastResultStats = StatsText;

	// Append stats footer to content box
	StreamingContentBox->AddSlot()
	.AutoHeight()
	.Padding(0, 8, 0, 0)
	[
		SNew(STextBlock)
		.Text(FText::FromString(StatsText))
		.TextStyle(FAppStyle::Get(), "SmallText")
		.ColorAndOpacity(OsvayderSlateStyle::MutedText())
	];

	if (ChatScrollBox.IsValid())
	{
		ChatScrollBox->ScrollToEnd();
	}
}

FString SOsvayderEditorWidget::GetDisplayToolName(const FString& FullToolName)
{
	FString Name = FullToolName;
	// Strip common MCP server prefix for cleaner display
	Name.RemoveFromStart(TEXT("mcp__osvayderue__unreal_"));
	return Name;
}

void SOsvayderEditorWidget::UpdateToolGroupSummary()
{
	if (!ToolGroupSummaryText.IsValid())
	{
		return;
	}

	if (ToolGroupCount == 1)
	{
		// Single tool - show its name in the header
		FString DisplayName = TEXT("Tool");
		if (ToolGroupCallIds.Num() > 0)
		{
			const FString* NamePtr = ToolCallNames.Find(ToolGroupCallIds[0]);
			if (NamePtr)
			{
				DisplayName = GetDisplayToolName(*NamePtr);
			}
		}

		if (ToolGroupDoneCount >= 1)
		{
			ToolGroupSummaryText->SetText(FText::FromString(
				FString::Printf(TEXT("Completed: %s"), *DisplayName)));
			ToolGroupSummaryText->SetColorAndOpacity(
				GetToolCompletedStatusColor());
		}
		else
		{
			ToolGroupSummaryText->SetText(FText::FromString(
				FString::Printf(TEXT("Running: %s"), *DisplayName)));
			ToolGroupSummaryText->SetColorAndOpacity(GetToolRunningStatusColor());
		}
	}
	else
	{
		// Multiple tools - show count summary
		if (ToolGroupDoneCount >= ToolGroupCount)
		{
			ToolGroupSummaryText->SetText(FText::FromString(
				FString::Printf(TEXT("Completed: %d tools"), ToolGroupCount)));
			ToolGroupSummaryText->SetColorAndOpacity(
				GetToolCompletedStatusColor());
		}
		else
		{
			ToolGroupSummaryText->SetText(FText::FromString(
				FString::Printf(TEXT("Running: %d tools (%d/%d done)"),
					ToolGroupCount, ToolGroupDoneCount, ToolGroupCount)));
			ToolGroupSummaryText->SetColorAndOpacity(GetToolRunningStatusColor());
		}
	}

}

namespace
{

	FString MakeUtcNowText()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	FString NormalizeSingleLineText(FString Value)
	{
		Value.ReplaceInline(TEXT("\r"), TEXT(" "));
		Value.ReplaceInline(TEXT("\n"), TEXT(" | "));
		while (Value.Contains(TEXT("  ")))
		{
			Value.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		Value.TrimStartAndEndInline();
		return Value;
	}

	FString BuildPlanId()
	{
		return FString::Printf(
			TEXT("plan_%s_%s"),
			*FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	FOsvayderUEActivePlan BuildDefaultActivePlan(const FString& OriginalUserTask)
	{
		TArray<FAgentPromptContextBlock> ScratchContextBlocks;
		const FAgentCanonExecution SeedExecution = OsvayderUECanonRouting::BuildInitialCanonExecution(
			OriginalUserTask,
			EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
			ScratchContextBlocks);
		if (SeedExecution.FeatureWorkflow.HasAnySignal())
		{
			return BuildFeatureWorkflowPlan(OriginalUserTask, SeedExecution.FeatureWorkflow);
		}

		FOsvayderUEActivePlan Plan;
		Plan.SchemaVersion = 1;
		Plan.PlanId = BuildPlanId();
		Plan.ReviewerPlanReference = TEXT("editor_default_execution_plan_v1");
		Plan.OriginalUserTask = OriginalUserTask;
		Plan.CreatedAtUtc = MakeUtcNowText();
		Plan.UpdatedAtUtc = Plan.CreatedAtUtc;
		Plan.Status = TEXT("active");
		Plan.ResultStatus = TEXT("incomplete");
		Plan.Summary = TEXT("Preparing a bounded execution plan.");
        Plan.SummaryRu = TEXT("Подготовка активного плана выполнения.");
		Plan.TechnicalDetail = TEXT("Agent 1 persisted the active reviewer-plan snapshot before the first tool call.");
		Plan.CurrentMechanicId = TEXT("inspect_current_state");
		Plan.CurrentAction = TEXT("persist_active_plan");
        Plan.CurrentActionRu = TEXT("Сохранение плана в активное состояние.");
		Plan.CurrentTechnicalDetail = TEXT("Persisted active_plan.json before dispatching the first request.");
		Plan.ResumeHint = TEXT("Resume from the exact last tool-call boundary recorded in active_plan.json.");
		Plan.HandoffPolicy = TEXT("full_batch_default");
		Plan.Settings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
		Plan.Mechanics = {
            { 1, TEXT("inspect_current_state"), TEXT("Inspect current state"), TEXT("Проверить текущее состояние"), TEXT("pending") },
            { 1, TEXT("perform_bounded_work"), TEXT("Perform bounded work"), TEXT("Выполнить ограниченную работу"), TEXT("pending") },
            { 1, TEXT("verify_and_report"), TEXT("Verify and report"), TEXT("Проверить результат и отчитаться"), TEXT("pending") }
		};
		Plan.VerificationChecklist = {
			TEXT("Open the affected project or asset."),
			TEXT("Trigger the updated behavior once."),
			TEXT("Confirm the observed result matches the final summary.")
		};
		AssignPlanLaneState(
			Plan,
			TEXT("live_editor"),
			FString(),
			FString(),
			TEXT("none"),
			TEXT("steady"),
			TEXT("The same task is working in the editor."),
			FString(),
			FString(),
			TEXT("Finish the bounded task in the editor unless a closed-editor continuation is armed."));
		return Plan;
	}

	FOsvayderUEActivePlan BuildFeatureWorkflowPlan(const FString& OriginalUserTask, const FAgentFeatureWorkflowState& Workflow)
	{
		FOsvayderUEActivePlan Plan;
		Plan.SchemaVersion = 1;
		Plan.PlanId = BuildPlanId();
		Plan.ReviewerPlanReference = TEXT("editor_feature_workflow_plan_v1");
		Plan.OriginalUserTask = OriginalUserTask;
		Plan.CreatedAtUtc = MakeUtcNowText();
		Plan.UpdatedAtUtc = Plan.CreatedAtUtc;
		Plan.Status = TEXT("active");
		Plan.ResultStatus = TEXT("incomplete");
		Plan.FeatureWorkflow = Workflow;
		Plan.Summary = TEXT("Preparing a bounded feature workflow.");
		Plan.SummaryRu = TEXT("Подготовка структурированного feature workflow.");
		Plan.TechnicalDetail = FString::Printf(
			TEXT("Seeded feature_workflow_id=%s recipe_id=%s current_phase=%s before the first tool call."),
			*Workflow.FeatureWorkflowId,
			*Workflow.RecipeId,
			*Workflow.CurrentPhase);
		Plan.CurrentMechanicId = Workflow.CurrentPhase;
		Plan.CurrentAction = TEXT("persist_feature_workflow_plan");
		Plan.CurrentActionRu = TEXT("Сохранение feature workflow в активное состояние.");
		Plan.CurrentTechnicalDetail = Plan.TechnicalDetail;
		Plan.ResumeHint = FString::Printf(
			TEXT("Resume feature_workflow_id=%s at phase=%s from the exact last tool-call boundary."),
			*Workflow.FeatureWorkflowId,
			*Workflow.CurrentPhase);
		Plan.HandoffPolicy = TEXT("feature_slice_recipe_v1");
		Plan.Settings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
		for (const FAgentFeatureWorkflowPhaseState& Phase : Workflow.Phases)
		{
			FOsvayderUEPlanMechanicEntry& Mechanic = Plan.Mechanics.AddDefaulted_GetRef();
			Mechanic.SchemaVersion = 1;
			Mechanic.MechanicId = Phase.PhaseId;
			Mechanic.Label = Phase.Label.IsEmpty() ? Phase.PhaseId : Phase.Label;
			Mechanic.LabelRu = Phase.Label.IsEmpty() ? Phase.PhaseId : Phase.Label;
			Mechanic.Status = Phase.PhaseId.Equals(Workflow.CurrentPhase, ESearchCase::CaseSensitive)
				? TEXT("in_progress")
				: TEXT("pending");
		}
		Plan.VerificationChecklist = {
			TEXT("Phase through data_model -> runtime_owner -> input_controller -> ui_widget."),
			TEXT("Do not claim runtime proof before compile_gate passes when C++ mutation occurred."),
			TEXT("Record truthful runtime proof or leave a structured partial gap.")
		};
		AssignPlanLaneState(
			Plan,
			TEXT("live_editor"),
			FString(),
			FString(),
			TEXT("none"),
			TEXT("steady"),
			TEXT("The same feature workflow is working in the editor."),
			Workflow.BlockerFamily,
			FString(),
			TEXT("Continue in the editor until the workflow truthfully completes or a bounded closed-editor continuation is armed."));
		return Plan;
	}

	int32 FindMechanicIndex(const FOsvayderUEActivePlan& Plan, const FString& MechanicId)
	{
		for (int32 Index = 0; Index < Plan.Mechanics.Num(); ++Index)
		{
			if (Plan.Mechanics[Index].MechanicId.Equals(MechanicId, ESearchCase::CaseSensitive))
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	void SetMechanicStatus(
		FOsvayderUEActivePlan& Plan,
		const FString& MechanicId,
		const FString& Status,
		const FString& Summary,
		const FString& SummaryRu,
		const bool bMarkCompleted)
	{
		const int32 Index = FindMechanicIndex(Plan, MechanicId);
		if (Index == INDEX_NONE)
		{
			return;
		}

		FOsvayderUEPlanMechanicEntry& Mechanic = Plan.Mechanics[Index];
		if (Mechanic.StartedAtUtc.IsEmpty())
		{
			Mechanic.StartedAtUtc = MakeUtcNowText();
		}
		Mechanic.Status = Status;
		Mechanic.LastSummary = Summary;
		Mechanic.LastSummaryRu = SummaryRu;
		if (bMarkCompleted)
		{
			Mechanic.CompletedAtUtc = MakeUtcNowText();
			Plan.CompletedMechanicIds.AddUnique(Mechanic.MechanicId);
		}
	}

	void SetCurrentMechanic(
		FOsvayderUEActivePlan& Plan,
		const FString& MechanicId,
		const FString& Summary,
		const FString& SummaryRu,
		const FString& TechnicalDetail)
	{
		Plan.CurrentMechanicId = MechanicId;
		Plan.CurrentAction = Summary;
		Plan.CurrentActionRu = SummaryRu;
		Plan.CurrentTechnicalDetail = TechnicalDetail;
		Plan.Summary = Summary;
		Plan.SummaryRu = SummaryRu;
		Plan.TechnicalDetail = TechnicalDetail;
		Plan.UpdatedAtUtc = MakeUtcNowText();

		const int32 Index = FindMechanicIndex(Plan, MechanicId);
		if (Index != INDEX_NONE)
		{
			if (Plan.Mechanics[Index].StartedAtUtc.IsEmpty())
			{
				Plan.Mechanics[Index].StartedAtUtc = Plan.UpdatedAtUtc;
			}
			Plan.Mechanics[Index].Status = TEXT("in_progress");
			Plan.Mechanics[Index].LastSummary = Summary;
			Plan.Mechanics[Index].LastSummaryRu = SummaryRu;
		}
	}

	void MarkLeadingMechanicCompleted(FOsvayderUEActivePlan& Plan, const FString& MechanicId)
	{
		const int32 Index = FindMechanicIndex(Plan, MechanicId);
		if (Index == INDEX_NONE)
		{
			return;
		}

		FOsvayderUEPlanMechanicEntry& Mechanic = Plan.Mechanics[Index];
		Mechanic.Status = TEXT("completed");
		if (Mechanic.StartedAtUtc.IsEmpty())
		{
			Mechanic.StartedAtUtc = MakeUtcNowText();
		}
		if (Mechanic.CompletedAtUtc.IsEmpty())
		{
			Mechanic.CompletedAtUtc = MakeUtcNowText();
		}
		Plan.CompletedMechanicIds.AddUnique(Mechanic.MechanicId);
	}

	void EnsureToolCallEntryExists(
		FOsvayderUEActivePlan& Plan,
		const FString& ToolCallId,
		const FString& MechanicId,
		const FString& ToolName)
	{
		for (FOsvayderUEPlanToolCallEntry& Entry : Plan.ToolCalls)
		{
			if (Entry.ToolCallId.Equals(ToolCallId, ESearchCase::CaseSensitive))
			{
				if (Entry.MechanicId.IsEmpty())
				{
					Entry.MechanicId = MechanicId;
				}
				if (Entry.ToolName.IsEmpty())
				{
					Entry.ToolName = ToolName;
				}
				return;
			}
		}

		FOsvayderUEPlanToolCallEntry Entry;
		Entry.ToolCallId = ToolCallId;
		Entry.MechanicId = MechanicId;
		Entry.ToolName = ToolName;
		Plan.ToolCalls.Add(Entry);
	}

	bool ToolNameMatches(const FString& ToolName, const FString& CanonicalName)
	{
		const FString NormalizedToolName = ToolName.TrimStartAndEnd().ToLower();
		const FString NormalizedCanonicalName = CanonicalName.ToLower();
		return NormalizedToolName.Equals(NormalizedCanonicalName, ESearchCase::CaseSensitive)
			|| NormalizedToolName.EndsWith(TEXT("/") + NormalizedCanonicalName, ESearchCase::CaseSensitive)
			|| NormalizedToolName.EndsWith(TEXT("__") + NormalizedCanonicalName, ESearchCase::CaseSensitive)
			|| NormalizedToolName.EndsWith(TEXT(".") + NormalizedCanonicalName, ESearchCase::CaseSensitive);
	}

	FString NormalizeWorkflowEvidenceText(FString Text)
	{
		Text.ReplaceInline(TEXT("\\"), TEXT("/"));
		Text.ReplaceInline(TEXT("\r"), TEXT(" "));
		Text.ReplaceInline(TEXT("\n"), TEXT(" "));
		Text.TrimStartAndEndInline();
		return Text.ToLower();
	}

	bool ContainsAnyWorkflowEvidenceToken(const FString& Haystack, const TArray<FString>& Needles)
	{
		for (const FString& Needle : Needles)
		{
			if (Haystack.Contains(Needle, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}

		return false;
	}

	bool HasPersistentInputPathPairEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT("/game/"), ESearchCase::CaseSensitive)
			&& EvidenceText.Contains(TEXT("imc_"), ESearchCase::CaseSensitive)
			&& EvidenceText.Contains(TEXT("ia_"), ESearchCase::CaseSensitive);
	}

	bool HasPersistentInputContextEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT("/game/"), ESearchCase::CaseSensitive)
			&& EvidenceText.Contains(TEXT("imc_"), ESearchCase::CaseSensitive);
	}

	bool HasInteractionActionAssetEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT("/game/"), ESearchCase::CaseSensitive)
			&& EvidenceText.Contains(TEXT("ia_"), ESearchCase::CaseSensitive);
	}

	bool HasProjectLocalInputReferenceEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT(".cpp"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT(".h"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("persistent_project_input"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("getpreferredinteractionmappingcontext"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("getpreferredinteractaction"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("addmappingcontext"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("bindaction"), ESearchCase::CaseSensitive);
	}

	bool IsReadOnlyProjectInspectionTool(const FOsvayderStreamEvent& Event)
	{
		const FString ToolEnvelope = Event.ToolInput + TEXT("\n") + Event.RawJson;
		return ToolNameMatches(Event.ToolName, TEXT("read"))
			|| ToolNameMatches(Event.ToolName, TEXT("grep"))
			|| ToolNameMatches(Event.ToolName, TEXT("glob"))
			|| ((ToolNameMatches(Event.ToolName, TEXT("command_execution"))
					|| ToolNameMatches(Event.ToolName, TEXT("bash"))
					|| ToolNameMatches(Event.ToolName, TEXT("execute_terminal")))
				&& !OsvayderUECanonRouting::IsMutatingToolUse(Event.ToolName, ToolEnvelope));
	}

	bool HasProjectLocalSourcePathEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT("source/"), ESearchCase::CaseSensitive)
			&& (EvidenceText.Contains(TEXT(".cpp"), ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(TEXT(".h"), ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(TEXT(".*"), ESearchCase::CaseSensitive));
	}

	bool HasAttemptResolverSourcePathEvidence(const FString& EvidenceText)
	{
		return HasProjectLocalSourcePathEvidence(EvidenceText)
			&& (EvidenceText.Contains(
					TEXT("/source/alternative/prisonaccess/alternativeprisonaccessattemptresolver.cpp"),
					ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(
					TEXT("source/alternative/prisonaccess/alternativeprisonaccessattemptresolver.cpp"),
					ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(
					TEXT("/source/alternative/prisonaccess/alternativeprisonaccessattemptresolver.h"),
					ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(
					TEXT("source/alternative/prisonaccess/alternativeprisonaccessattemptresolver.h"),
					ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(
					TEXT("source/alternative/prisonaccess/alternativeprisonaccessattemptresolver.*"),
					ESearchCase::CaseSensitive));
	}

	bool HasEventSubsystemSourcePathEvidence(const FString& EvidenceText)
	{
		return HasProjectLocalSourcePathEvidence(EvidenceText)
			&& (EvidenceText.Contains(
					TEXT("/source/alternative/prisonaccess/alternativeprisonaccesseventsubsystem.cpp"),
					ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(
					TEXT("source/alternative/prisonaccess/alternativeprisonaccesseventsubsystem.cpp"),
					ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(
					TEXT("/source/alternative/prisonaccess/alternativeprisonaccesseventsubsystem.h"),
					ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(
					TEXT("source/alternative/prisonaccess/alternativeprisonaccesseventsubsystem.h"),
					ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(
					TEXT("source/alternative/prisonaccess/alternativeprisonaccesseventsubsystem.*"),
					ESearchCase::CaseSensitive));
	}

	bool HasAttemptResolverDirectSourceEvidence(const FString& EvidenceText)
	{
		return HasAttemptResolverSourcePathEvidence(EvidenceText)
			&& ContainsAnyWorkflowEvidenceToken(
				EvidenceText,
				{
					TEXT("falternativeprisonaccessattemptresolver"),
					TEXT("resolvedoorattempt"),
					TEXT("resolvetechnicalboxattempt"),
					TEXT("falternativeprisonaccessattemptoutcome")
				});
	}

	bool HasEventSubsystemDirectSourceEvidence(const FString& EvidenceText)
	{
		return HasEventSubsystemSourcePathEvidence(EvidenceText)
			&& ContainsAnyWorkflowEvidenceToken(
				EvidenceText,
				{
					TEXT("ualternativeprisonaccesseventsubsystem"),
					TEXT("falternativeprisonaccesseventrecord"),
					TEXT("recordevent"),
					TEXT("prisonaccessevent order=")
				});
	}

	bool HasInteractionAccessContractEvidence(const FOsvayderStreamEvent& Event)
	{
		if (Event.bIsError || Event.ToolResultContent.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		const bool bStructuredProjectTool =
			ToolNameMatches(Event.ToolName, TEXT("asset_search"))
			|| ToolNameMatches(Event.ToolName, TEXT("asset_dependencies"))
			|| ToolNameMatches(Event.ToolName, TEXT("asset_referencers"))
			|| ToolNameMatches(Event.ToolName, TEXT("enhanced_input"))
			|| ToolNameMatches(Event.ToolName, TEXT("open_level"))
			|| ToolNameMatches(Event.ToolName, TEXT("get_level_actors"))
			|| ToolNameMatches(Event.ToolName, TEXT("capture_viewport"))
			|| ToolNameMatches(Event.ToolName, TEXT("map_runtime_proof"));
		const bool bReadOnlyInspection = IsReadOnlyProjectInspectionTool(Event);
		if (!bStructuredProjectTool && !bReadOnlyInspection)
		{
			return false;
		}

		const FString CombinedEvidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);
		const bool bProofMapObserved =
			CombinedEvidence.Contains(TEXT("/game/prisonaccess/maps/lvl_prisonaccessproof"), ESearchCase::CaseSensitive)
			|| CombinedEvidence.Contains(TEXT("lvl_prisonaccessproof"), ESearchCase::CaseSensitive);
		const bool bPersistentInputPairObserved =
			HasPersistentInputPathPairEvidence(CombinedEvidence)
			|| (CombinedEvidence.Contains(TEXT("/game/prisonaccess/input/imc_prisonaccessproof"), ESearchCase::CaseSensitive)
				&& CombinedEvidence.Contains(TEXT("/game/variant_sidescrolling/input/actions/ia_interact"), ESearchCase::CaseSensitive));
		const bool bPlacedProofActorObserved = ContainsAnyWorkflowEvidenceToken(
			CombinedEvidence,
			{
				TEXT("proof_prisonaccess_door"),
				TEXT("proof_prisonaccess_controlbox"),
				TEXT("proof_prisonaccess_escapeitem")
			});
		const bool bDirectProjectSourceObserved =
			bReadOnlyInspection
			&& (HasAttemptResolverDirectSourceEvidence(CombinedEvidence)
				|| HasEventSubsystemDirectSourceEvidence(CombinedEvidence));

		return bProofMapObserved
			|| bPersistentInputPairObserved
			|| bPlacedProofActorObserved
			|| bDirectProjectSourceObserved;
	}

	bool DidInteractionAccessAttemptResolverObservationProgress(
		const FInteractionAccessAttemptResolverObservationState& Before,
		const FInteractionAccessAttemptResolverObservationState& After)
	{
		return Before.bAttemptResolverSourceObserved != After.bAttemptResolverSourceObserved
			|| Before.bEventSubsystemSourceObserved != After.bEventSubsystemSourceObserved
			|| Before.bRuntimeSmokeSuccessObserved != After.bRuntimeSmokeSuccessObserved
			|| Before.bPrisonAccessEventObserved != After.bPrisonAccessEventObserved;
	}

	bool DidInteractionAccessReuseObservationProgress(
		const FInteractionAccessReuseObservationState& Before,
		const FInteractionAccessReuseObservationState& After)
	{
		return Before.bPersistentInputAssetObserved != After.bPersistentInputAssetObserved
			|| Before.bInteractionActionAssetObserved != After.bInteractionActionAssetObserved
			|| Before.bReadOnlyEnhancedInputQueryObserved != After.bReadOnlyEnhancedInputQueryObserved;
	}

	bool HasRuntimeSmokeLogContextEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT("saved/logs"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("logautomationcontroller:"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("logautomationcommandline:"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("logalternative:"), ESearchCase::CaseSensitive);
	}

	bool HasPrisonAccessRuntimeSmokeSuccessEvidence(const FString& EvidenceText)
	{
		if (!EvidenceText.Contains(TEXT("result={success}"), ESearchCase::CaseSensitive))
		{
			return false;
		}

		return EvidenceText.Contains(TEXT("prooffixturesmoke"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("technicalroute"), ESearchCase::CaseSensitive)
			|| EvidenceText.Contains(TEXT("unskilledroute"), ESearchCase::CaseSensitive);
	}

	bool HasPrisonAccessEventLogEvidence(const FString& EvidenceText)
	{
		return EvidenceText.Contains(TEXT("prisonaccessevent order="), ESearchCase::CaseSensitive)
			&& (EvidenceText.Contains(TEXT("tag=attempt."), ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(TEXT("attempt.access."), ESearchCase::CaseSensitive)
				|| EvidenceText.Contains(TEXT("attempt.technical."), ESearchCase::CaseSensitive));
	}

	bool IsBoundedInteractionAccessAutomationWrapper(const FOsvayderStreamEvent& Event)
	{
		if (!(ToolNameMatches(Event.ToolName, TEXT("command_execution"))
			|| ToolNameMatches(Event.ToolName, TEXT("bash"))
			|| ToolNameMatches(Event.ToolName, TEXT("execute_terminal"))))
		{
			return false;
		}

		const FString CommandEnvelope =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson);
		const bool bRunsInteractionAccessAutomation =
			CommandEnvelope.Contains(TEXT("automation runtests alternative.prisonaccess"), ESearchCase::CaseSensitive)
			|| CommandEnvelope.Contains(
				TEXT("-execcmds=automation runtests alternative.prisonaccess;quit"),
				ESearchCase::CaseSensitive);
		if (!bRunsInteractionAccessAutomation)
		{
			return false;
		}

		const bool bHeadlessEditorWrapper =
			CommandEnvelope.Contains(TEXT("unrealeditor-cmd"), ESearchCase::CaseSensitive)
			|| CommandEnvelope.Contains(TEXT("-testexit=automation test queue empty"), ESearchCase::CaseSensitive)
			|| CommandEnvelope.Contains(TEXT("-fullstdoutlogoutput"), ESearchCase::CaseSensitive);
		const bool bLocalLogCaptureWrapper =
			CommandEnvelope.Contains(TEXT("-abslog"), ESearchCase::CaseSensitive)
			|| CommandEnvelope.Contains(TEXT("-log="), ESearchCase::CaseSensitive)
			|| CommandEnvelope.Contains(TEXT("-stdout"), ESearchCase::CaseSensitive)
			|| CommandEnvelope.Contains(TEXT("tee-object"), ESearchCase::CaseSensitive)
			|| CommandEnvelope.Contains(TEXT("saved/logs/packet"), ESearchCase::CaseSensitive);
		return bHeadlessEditorWrapper && bLocalLogCaptureWrapper;
	}

	bool HasInteractionAccessAutomationLogEvidence(const FOsvayderStreamEvent& Event)
	{
		if (Event.bIsError || Event.ToolResultContent.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		const FString Evidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);
		const bool bAutomationLogShape =
			Evidence.Contains(TEXT("logautomationcommandline"), ESearchCase::CaseSensitive)
			|| Evidence.Contains(TEXT("logautomationcontroller"), ESearchCase::CaseSensitive)
			|| Evidence.Contains(TEXT("test completed. result={success}"), ESearchCase::CaseSensitive);
		const bool bDiscoveryLine =
			Evidence.Contains(TEXT("found"), ESearchCase::CaseSensitive)
			&& Evidence.Contains(TEXT("automation tests"), ESearchCase::CaseSensitive);
		return bAutomationLogShape
			&& bDiscoveryLine
			&& Evidence.Contains(TEXT("alternative.prisonaccess"), ESearchCase::CaseSensitive)
			&& Evidence.Contains(TEXT("prooffixturesmoke"), ESearchCase::CaseSensitive)
			&& Evidence.Contains(TEXT("result={success}"), ESearchCase::CaseSensitive)
			&& Evidence.Contains(TEXT("**** test complete. exit code: 0 ****"), ESearchCase::CaseSensitive);
	}

	void AddAutomationLogCandidateFromRemainder(FString Remainder, TArray<FString>& OutCandidates)
	{
		Remainder.TrimStartAndEndInline();
		const int32 AbsLogKeyIndex = Remainder.Find(TEXT("-AbsLog"), ESearchCase::IgnoreCase);
		if (AbsLogKeyIndex != INDEX_NONE)
		{
			Remainder = Remainder.Mid(AbsLogKeyIndex + 7);
			Remainder.TrimStartAndEndInline();
		}
		const int32 AutomationLogKeyIndex = Remainder.Find(TEXT("AutomationLog"), ESearchCase::IgnoreCase);
		if (AutomationLogKeyIndex != INDEX_NONE)
		{
			Remainder = Remainder.Mid(AutomationLogKeyIndex + 13);
			Remainder.TrimStartAndEndInline();
		}
		while (Remainder.StartsWith(TEXT("=")) || Remainder.StartsWith(TEXT(":")))
		{
			Remainder = Remainder.RightChop(1);
			Remainder.TrimStartAndEndInline();
		}
		while (Remainder.StartsWith(TEXT("\""))
			|| Remainder.StartsWith(TEXT("'"))
			|| Remainder.StartsWith(TEXT("\\\"")))
		{
			Remainder = Remainder.RightChop(Remainder.StartsWith(TEXT("\\\"")) ? 2 : 1);
			Remainder.TrimStartAndEndInline();
		}

		const int32 LogExtensionIndex = Remainder.Find(TEXT(".log"), ESearchCase::IgnoreCase);
		if (LogExtensionIndex == INDEX_NONE)
		{
			return;
		}

		FString Candidate = Remainder.Left(LogExtensionIndex + 4);
		Candidate.TrimStartAndEndInline();
		while (Candidate.EndsWith(TEXT("\""))
			|| Candidate.EndsWith(TEXT("'"))
			|| Candidate.EndsWith(TEXT("\\\"")))
		{
			Candidate = Candidate.LeftChop(Candidate.EndsWith(TEXT("\\\"")) ? 2 : 1);
			Candidate.TrimStartAndEndInline();
		}
		Candidate.ReplaceInline(TEXT("\\\""), TEXT(""));
		Candidate.ReplaceInline(TEXT("\\\\"), TEXT("\\"));
		Candidate.ReplaceInline(TEXT("\\/"), TEXT("/"));
		if (!Candidate.IsEmpty() && !Candidate.Contains(TEXT("$")))
		{
			OutCandidates.AddUnique(Candidate);
		}
	}

	TArray<FString> ExtractReferencedAutomationLogCandidates(const FString& SearchText)
	{
		TArray<FString> Candidates;
		TArray<FString> Lines;
		SearchText.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			int32 KeyIndex = Line.Find(TEXT("-AbsLog"), ESearchCase::IgnoreCase);
			if (KeyIndex != INDEX_NONE)
			{
				AddAutomationLogCandidateFromRemainder(Line.Mid(KeyIndex + 7), Candidates);
			}

			KeyIndex = Line.Find(TEXT("AutomationLog"), ESearchCase::IgnoreCase);
			if (KeyIndex != INDEX_NONE)
			{
				AddAutomationLogCandidateFromRemainder(Line.Mid(KeyIndex + 13), Candidates);
			}
		}

		TArray<FString> Tokens;
		SearchText.ParseIntoArrayWS(Tokens);
		for (const FString& Token : Tokens)
		{
			if (Token.Contains(TEXT(".log"), ESearchCase::IgnoreCase)
				&& Token.Contains(TEXT("Saved"), ESearchCase::IgnoreCase)
				&& Token.Contains(TEXT("Logs"), ESearchCase::IgnoreCase))
			{
				AddAutomationLogCandidateFromRemainder(Token, Candidates);
			}
		}
		return Candidates;
	}

	bool TryLoadProjectLocalAutomationLog(const FString& CandidatePath, FString& OutLogText)
	{
		FString CleanPath = CandidatePath;
		CleanPath.ReplaceInline(TEXT("\\\""), TEXT(""));
		CleanPath.ReplaceInline(TEXT("\\\\"), TEXT("\\"));
		CleanPath.ReplaceInline(TEXT("\\/"), TEXT("/"));
		CleanPath.ReplaceInline(TEXT("\""), TEXT(""));
		CleanPath.ReplaceInline(TEXT("'"), TEXT(""));
		CleanPath.TrimStartAndEndInline();
		if (CleanPath.IsEmpty() || CleanPath.Contains(TEXT("$")))
		{
			return false;
		}

		FString FullPath = FPaths::IsRelative(CleanPath)
			? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), CleanPath)
			: FPaths::ConvertRelativePathToFull(CleanPath);
		FPaths::NormalizeFilename(FullPath);

		FString ProjectLogsDir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs")));
		FPaths::NormalizeFilename(ProjectLogsDir);
		FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeFilename(ProjectRoot);
		const bool bProjectSavedLog =
			FullPath.StartsWith(ProjectLogsDir, ESearchCase::IgnoreCase)
			|| (FullPath.StartsWith(ProjectRoot, ESearchCase::IgnoreCase)
				&& FullPath.Contains(TEXT("/Saved/Logs/"), ESearchCase::IgnoreCase));
		if (!bProjectSavedLog
			|| !FPaths::FileExists(FullPath))
		{
			return false;
		}

		return FFileHelper::LoadFileToString(OutLogText, *FullPath);
	}

	FString AppendReferencedAutomationLogEvidence(const FOsvayderStreamEvent& Event)
	{
		FString EvidenceText = Event.ToolResultContent;
		const FString SearchText =
			Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent;
		for (const FString& Candidate : ExtractReferencedAutomationLogCandidates(SearchText))
		{
			FString LoadedLog;
			if (TryLoadProjectLocalAutomationLog(Candidate, LoadedLog)
				&& !LoadedLog.TrimStartAndEnd().IsEmpty())
			{
				EvidenceText += TEXT("\n[ReferencedAutomationLog]\n") + LoadedLog;
			}
		}
		return EvidenceText;
	}

	bool HasInteractionAccessPlacedRuntimeActorObservation(
		const FAgentFeatureWorkflowState& Workflow,
		const FOsvayderStreamEvent& Event);

	bool HasInteractionAccessEvidenceSignal(
		const FAgentFeatureWorkflowState& Workflow,
		const FOsvayderStreamEvent& Event)
	{
		if (Event.bIsError || Event.ToolResultContent.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		const bool bAcceptedEvidenceCarrier =
			IsReadOnlyProjectInspectionTool(Event)
			|| ToolNameMatches(Event.ToolName, TEXT("asset_search"))
			|| ToolNameMatches(Event.ToolName, TEXT("asset_dependencies"))
			|| ToolNameMatches(Event.ToolName, TEXT("asset_referencers"))
			|| ToolNameMatches(Event.ToolName, TEXT("enhanced_input"))
			|| ToolNameMatches(Event.ToolName, TEXT("open_level"))
			|| ToolNameMatches(Event.ToolName, TEXT("get_level_actors"))
			|| ToolNameMatches(Event.ToolName, TEXT("capture_viewport"))
			|| ToolNameMatches(Event.ToolName, TEXT("map_runtime_proof"))
			|| IsBoundedInteractionAccessAutomationWrapper(Event);
		if (!bAcceptedEvidenceCarrier)
		{
			return false;
		}

		const FString Evidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);
		const bool bInputEvidence =
			HasPersistentInputContextEvidence(Evidence)
			|| HasInteractionActionAssetEvidence(Evidence)
			|| HasPersistentInputPathPairEvidence(Evidence);
		const bool bSourceEvidence =
			HasAttemptResolverSourcePathEvidence(Evidence)
			|| HasEventSubsystemSourcePathEvidence(Evidence)
			|| HasAttemptResolverDirectSourceEvidence(Evidence)
			|| HasEventSubsystemDirectSourceEvidence(Evidence);
		const bool bRuntimeEvidence =
			HasPrisonAccessRuntimeSmokeSuccessEvidence(Evidence)
			|| HasPrisonAccessEventLogEvidence(Evidence)
			|| HasInteractionAccessAutomationLogEvidence(Event);
		return bInputEvidence
			|| bSourceEvidence
			|| bRuntimeEvidence
			|| HasInteractionAccessContractEvidence(Event)
			|| HasInteractionAccessPlacedRuntimeActorObservation(Workflow, Event);
	}

	bool HasSucceededAutomationTruth(const FAgentFeatureWorkflowState& Workflow)
	{
		const bool bNoKnownFailures =
			Workflow.AutomationFailedCount == INDEX_NONE
			|| Workflow.AutomationFailedCount == 0;
		return Workflow.AutomationDiscoveryCount > 0
			&& Workflow.AutomationExecutedCount > 0
			&& bNoKnownFailures;
	}

	bool HasInteractionAccessBoundedRuntimeProofTruth(const FAgentFeatureWorkflowState& Workflow)
	{
		FOsvayderUERecipeEvidenceContract Contract;
		if (!OsvayderUERecipeRegistry::TryGetRecipeEvidenceContract(Workflow.RecipeId, Contract)
			|| !Contract.RecipeId.Equals(OsvayderUERecipeRegistry::InteractionAccessRecipeId(), ESearchCase::CaseSensitive))
		{
			return false;
		}

		return HasSucceededAutomationTruth(Workflow)
			&& FindMissingWorkflowRecipeObligations(Contract, Workflow).Num() == 0;
	}

	bool ObserveInteractionAccessPersistentInputReuseEvidence(
		FAgentFeatureWorkflowState& Workflow,
		const FOsvayderStreamEvent& Event)
	{
		FInteractionAccessReuseObservationState& Observation = Workflow.InteractionAccessReuseObservation;
		if (Event.bIsError || Event.ToolResultContent.TrimStartAndEnd().IsEmpty())
		{
			return Observation.HasSufficientReuseEvidence();
		}

		const FString NormalizedInput = NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson);
		const FString NormalizedResult = NormalizeWorkflowEvidenceText(Event.ToolResultContent);
		const FString CombinedEvidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);
		const bool bPersistentInputContextObserved = HasPersistentInputContextEvidence(CombinedEvidence);
		const bool bInteractionActionObserved = HasInteractionActionAssetEvidence(CombinedEvidence);

		const bool bReadOnlyEnhancedInputEvidence =
			ToolNameMatches(Event.ToolName, TEXT("enhanced_input"))
			&& !OsvayderUECanonRouting::IsMutatingToolUse(Event.ToolName, Event.ToolInput)
			&& (NormalizedInput.Contains(TEXT("query_"), ESearchCase::CaseSensitive)
				|| NormalizedInput.Contains(TEXT("get_"), ESearchCase::CaseSensitive)
				|| NormalizedResult.Contains(TEXT("mapping"), ESearchCase::CaseSensitive)
				|| NormalizedResult.Contains(TEXT("persistent_project_input"), ESearchCase::CaseSensitive));

		const bool bStructuredAssetEvidence =
			(ToolNameMatches(Event.ToolName, TEXT("asset_search"))
				|| ToolNameMatches(Event.ToolName, TEXT("asset_dependencies"))
				|| ToolNameMatches(Event.ToolName, TEXT("asset_referencers")));

		const bool bReadOnlyProjectInspectionEvidence =
			(ToolNameMatches(Event.ToolName, TEXT("read"))
				|| ToolNameMatches(Event.ToolName, TEXT("grep"))
				|| ToolNameMatches(Event.ToolName, TEXT("glob"))
				|| ((ToolNameMatches(Event.ToolName, TEXT("command_execution"))
						|| ToolNameMatches(Event.ToolName, TEXT("bash")))
					&& !OsvayderUECanonRouting::IsMutatingToolUse(Event.ToolName, Event.ToolInput)))
			&& (HasProjectLocalInputReferenceEvidence(CombinedEvidence)
				|| bPersistentInputContextObserved
				|| bInteractionActionObserved
				|| HasPersistentInputPathPairEvidence(CombinedEvidence));

		if (bReadOnlyEnhancedInputEvidence || bStructuredAssetEvidence || bReadOnlyProjectInspectionEvidence)
		{
			Observation.bPersistentInputAssetObserved |= bPersistentInputContextObserved;
			Observation.bInteractionActionAssetObserved |= bInteractionActionObserved;
		}
		if (bReadOnlyEnhancedInputEvidence && bPersistentInputContextObserved)
		{
			Observation.bReadOnlyEnhancedInputQueryObserved = true;
		}

		return Observation.HasSufficientReuseEvidence();
	}

	bool ObserveInteractionAccessAttemptResolverEvidence(
		FAgentFeatureWorkflowState& Workflow,
		const FOsvayderStreamEvent& Event)
	{
		FInteractionAccessAttemptResolverObservationState& Observation =
			Workflow.InteractionAccessAttemptResolverObservation;
		if (Event.bIsError || Event.ToolResultContent.TrimStartAndEnd().IsEmpty())
		{
			return Observation.HasSufficientEvidence();
		}

		const bool bReadOnlyInspection = IsReadOnlyProjectInspectionTool(Event);
		const bool bBoundedAutomationWrapper = IsBoundedInteractionAccessAutomationWrapper(Event);
		if (!bReadOnlyInspection && !bBoundedAutomationWrapper)
		{
			return Observation.HasSufficientEvidence();
		}

		const FString SourceLocatorEvidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson);
		const FString CombinedEvidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);

		if (bReadOnlyInspection)
		{
			Observation.bAttemptResolverSourceObserved |=
				HasAttemptResolverSourcePathEvidence(SourceLocatorEvidence)
				|| HasAttemptResolverDirectSourceEvidence(CombinedEvidence);
			Observation.bEventSubsystemSourceObserved |=
				HasEventSubsystemSourcePathEvidence(SourceLocatorEvidence)
				|| HasEventSubsystemDirectSourceEvidence(CombinedEvidence);
		}

		if ((bReadOnlyInspection || bBoundedAutomationWrapper)
			&& HasRuntimeSmokeLogContextEvidence(CombinedEvidence))
		{
			Observation.bRuntimeSmokeSuccessObserved |=
				HasPrisonAccessRuntimeSmokeSuccessEvidence(CombinedEvidence);
			Observation.bPrisonAccessEventObserved |=
				HasPrisonAccessEventLogEvidence(CombinedEvidence);
		}

		return Observation.HasSufficientEvidence();
	}

	bool HasInteractionAccessPlacedRuntimeActorObservation(
		const FAgentFeatureWorkflowState& Workflow,
		const FOsvayderStreamEvent& Event)
	{
		if (Event.bIsError)
		{
			return false;
		}

		if (!(ToolNameMatches(Event.ToolName, TEXT("get_level_actors"))
			|| ToolNameMatches(Event.ToolName, TEXT("capture_viewport"))
			|| ToolNameMatches(Event.ToolName, TEXT("open_level"))
			|| ToolNameMatches(Event.ToolName, TEXT("map_runtime_proof"))))
		{
			return false;
		}

		const FString CombinedEvidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);
		const bool bProofMapContext =
			Workflow.bKnownProofMapAvailable
			|| CombinedEvidence.Contains(TEXT("lvl_prisonaccessproof"), ESearchCase::CaseSensitive)
			|| CombinedEvidence.Contains(TEXT("proof_prisonaccess"), ESearchCase::CaseSensitive);
		if (!bProofMapContext)
		{
			return false;
		}

		return ContainsAnyWorkflowEvidenceToken(
			CombinedEvidence,
			{
				TEXT("proof_prisonaccess_door"),
				TEXT("proof_prisonaccess_controlbox"),
				TEXT("proof_prisonaccess_escapeitem"),
				TEXT("alternativestoragedoor"),
				TEXT("alternativetechnicalbox"),
				TEXT("alternativeescapeitem"),
				TEXT("storage door"),
				TEXT("technical box"),
				TEXT("control box"),
				TEXT("escape item")
			});
	}

	bool IsCompileProofTool(const FString& ToolName)
	{
		return ToolNameMatches(ToolName, TEXT("livecoding_compile"))
			|| ToolNameMatches(ToolName, TEXT("restart_survival"));
	}

	bool IsPostCompileVerificationTool(const FString& ToolName)
	{
		return ToolNameMatches(ToolName, TEXT("execute_script"))
			|| ToolNameMatches(ToolName, TEXT("map_runtime_proof"))
			|| ToolNameMatches(ToolName, TEXT("multiplayer_audit"))
			|| ToolNameMatches(ToolName, TEXT("validate_blueprint"))
			|| ToolNameMatches(ToolName, TEXT("oss_session_proof"));
	}

	bool ToolInputMentionsCompiledModuleSource(const FString& ToolInput)
	{
		if (ToolInput.IsEmpty())
		{
			return false;
		}

		FString LowerInput = ToolInput.ToLower();
		LowerInput.ReplaceInline(TEXT("\\"), TEXT("/"));
		const auto ContainsExtensionToken = [&LowerInput](const FString& Token)
		{
			int32 SearchFrom = 0;
			while (SearchFrom < LowerInput.Len())
			{
				const int32 Index = LowerInput.Find(Token, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
				if (Index == INDEX_NONE)
				{
					return false;
				}

				const int32 NextIndex = Index + Token.Len();
				if (NextIndex >= LowerInput.Len())
				{
					return true;
				}

				const TCHAR NextChar = LowerInput[NextIndex];
				if (!FChar::IsAlnum(NextChar) && NextChar != TEXT('_'))
				{
					return true;
				}

				SearchFrom = Index + 1;
			}

			return false;
		};

		static const TCHAR* ExtensionTokens[] = {
			TEXT(".cpp"),
			TEXT(".cxx"),
			TEXT(".cc"),
			TEXT(".h"),
			TEXT(".hpp"),
			TEXT(".hh"),
			TEXT(".hxx"),
			TEXT(".inl"),
			TEXT(".ixx"),
			TEXT(".cppm"),
			TEXT(".build.cs"),
			TEXT(".target.cs")
		};

		for (const TCHAR* Token : ExtensionTokens)
		{
			if (ContainsExtensionToken(Token))
			{
				return true;
			}
		}

		return false;
	}

	bool TextMentionsCompiledModuleSourcePath(const FString& Text)
	{
		return ToolInputMentionsCompiledModuleSource(Text)
			&& (Text.Contains(TEXT("source/"), ESearchCase::IgnoreCase)
				|| Text.Contains(TEXT("/source/"), ESearchCase::IgnoreCase)
				|| Text.Contains(TEXT("plugins/"), ESearchCase::IgnoreCase));
	}

	FString ExtractShellMutationTargetSegment(const FString& LowerInput, const int32 SegmentStart)
	{
		int32 SegmentEnd = LowerInput.Len();
		const FString Delimiters[] = { TEXT(";"), TEXT("\n"), TEXT("\r"), TEXT("|") };
		for (const FString& Delimiter : Delimiters)
		{
			const int32 CandidateEnd =
				LowerInput.Find(Delimiter, ESearchCase::CaseSensitive, ESearchDir::FromStart, SegmentStart);
			if (CandidateEnd != INDEX_NONE)
			{
				SegmentEnd = FMath::Min(SegmentEnd, CandidateEnd);
			}
		}

		FString Segment = LowerInput.Mid(SegmentStart, SegmentEnd - SegmentStart);
		const FString ValueArgumentTokens[] = { TEXT(" -value "), TEXT(" -inputobject ") };
		for (const FString& ValueArgumentToken : ValueArgumentTokens)
		{
			const int32 ValueArgumentIndex = Segment.Find(ValueArgumentToken, ESearchCase::CaseSensitive);
			if (ValueArgumentIndex != INDEX_NONE)
			{
				Segment = Segment.Left(ValueArgumentIndex);
			}
		}
		return Segment;
	}

	bool ShellVerbTargetsCompiledModuleSource(const FString& LowerInput, const FString& Verb)
	{
		int32 SearchFrom = 0;
		while (SearchFrom < LowerInput.Len())
		{
			const int32 VerbIndex =
				LowerInput.Find(Verb, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (VerbIndex == INDEX_NONE)
			{
				return false;
			}

			const FString Segment = ExtractShellMutationTargetSegment(LowerInput, VerbIndex + Verb.Len());
			if (TextMentionsCompiledModuleSourcePath(Segment))
			{
				return true;
			}
			SearchFrom = VerbIndex + Verb.Len();
		}

		return false;
	}

	bool ToolInputTargetsCompiledModuleSourceMutation(const FString& ToolName, const FString& ToolInput)
	{
		if (ToolInput.IsEmpty() || !ToolInputMentionsCompiledModuleSource(ToolInput))
		{
			return false;
		}

		FString LowerInput = ToolInput.ToLower();
		LowerInput.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (ToolNameMatches(ToolName, TEXT("write"))
			|| ToolNameMatches(ToolName, TEXT("edit"))
			|| ToolNameMatches(ToolName, TEXT("apply_patch")))
		{
			return TextMentionsCompiledModuleSourcePath(LowerInput);
		}

		if (ToolNameMatches(ToolName, TEXT("command_execution"))
			|| ToolNameMatches(ToolName, TEXT("execute_terminal"))
			|| ToolNameMatches(ToolName, TEXT("bash")))
		{
			if ((LowerInput.Contains(TEXT("apply_patch"), ESearchCase::CaseSensitive)
					|| LowerInput.Contains(TEXT("git apply"), ESearchCase::CaseSensitive)
					|| LowerInput.Contains(TEXT("patch "), ESearchCase::CaseSensitive))
				&& TextMentionsCompiledModuleSourcePath(LowerInput))
			{
				return true;
			}

			const FString WriteVerbs[] = {
				TEXT("set-content"),
				TEXT("add-content"),
				TEXT("out-file"),
				TEXT("copy-item"),
				TEXT("move-item"),
				TEXT("remove-item"),
				TEXT("rename-item"),
				TEXT("new-item"),
				TEXT(" ni "),
				TEXT("del "),
				TEXT("rm ")
			};
			for (const FString& WriteVerb : WriteVerbs)
			{
				if (ShellVerbTargetsCompiledModuleSource(LowerInput, WriteVerb))
				{
					return true;
				}
			}

			const FString RedirectTokens[] = { TEXT(">"), TEXT(">>") };
			for (const FString& RedirectToken : RedirectTokens)
			{
				if (ShellVerbTargetsCompiledModuleSource(LowerInput, RedirectToken))
				{
					return true;
				}
			}
			return false;
		}

		return TextMentionsCompiledModuleSourcePath(LowerInput);
	}

	bool IsWorkspaceFileBuildExecution(const FAgentCanonExecution& Execution)
	{
		return Execution.TaskMode.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase)
			|| Execution.RequestedToolFamily.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase)
			|| Execution.PrimaryMutationToolFamily.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase)
			|| Execution.ActualToolFamily.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase);
	}

	bool IsWorkspaceFileBuildRestartOrigin(const FOsvayderUERestartSurvivalState& RestartState)
	{
		return RestartState.OriginTask.OriginatingTaskMode.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase)
			|| RestartState.OriginTask.OriginatingRequestedToolFamily.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase)
			|| RestartState.OriginTask.OriginatingPrimaryMutationToolFamily.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase);
	}

	int32 CompareIsoUtcTexts(const FString& Left, const FString& Right)
	{
		if (Left.IsEmpty())
		{
			return Right.IsEmpty() ? 0 : -1;
		}
		if (Right.IsEmpty())
		{
			return 1;
		}

		FDateTime LeftTime;
		FDateTime RightTime;
		if (FDateTime::ParseIso8601(*Left, LeftTime) && FDateTime::ParseIso8601(*Right, RightTime))
		{
			if (LeftTime < RightTime)
			{
				return -1;
			}
			if (LeftTime > RightTime)
			{
				return 1;
			}
			return 0;
		}

		return Left.Compare(Right, ESearchCase::CaseSensitive);
	}

	bool IsIsoUtcAfter(const FString& Left, const FString& Right)
	{
		return CompareIsoUtcTexts(Left, Right) > 0;
	}

	void RecordCompileProof(
		FOsvayderUECompileProofState& State,
		const FString& TimestampUtc,
		const FString& ToolCallId,
		const FString& ToolName,
		const FString& Outcome,
		const FString& Detail)
	{
		State.LastCompileProofAtUtc = TimestampUtc;
		State.LastCompileProofToolCallId = ToolCallId;
		State.LastCompileProofToolName = ToolName;
		State.LastCompileProofOutcome = Outcome;
		State.LastCompileProofDetail = Detail;
	}

	void RecordPostCompileVerification(
		FOsvayderUECompileProofState& State,
		const FString& TimestampUtc,
		const FString& ToolCallId,
		const FString& ToolName,
		const FString& Outcome)
	{
		State.LastPostCompileVerificationAtUtc = TimestampUtc;
		State.LastPostCompileVerificationToolCallId = ToolCallId;
		State.LastPostCompileVerificationToolName = ToolName;
		State.LastPostCompileVerificationOutcome = Outcome;
	}

	bool IsFeatureWorkflowPlan(const FOsvayderUEActivePlan& Plan)
	{
		return Plan.FeatureWorkflow.HasAnySignal();
	}

	FString GetCurrentFeatureWorkflowPhaseId(const FOsvayderUEActivePlan& Plan)
	{
		if (!Plan.FeatureWorkflow.CurrentPhase.IsEmpty())
		{
			return Plan.FeatureWorkflow.CurrentPhase;
		}

		return Plan.FeatureWorkflow.Phases.Num() > 0
			? Plan.FeatureWorkflow.Phases[0].PhaseId
			: Plan.CurrentMechanicId;
	}

	FString GetFeatureWorkflowPhaseLabel(const FAgentFeatureWorkflowState& Workflow, const FString& PhaseId)
	{
		if (const FAgentFeatureWorkflowPhaseState* Phase = Workflow.FindPhase(PhaseId))
		{
			return Phase->Label.IsEmpty() ? PhaseId : Phase->Label;
		}

		return PhaseId;
	}

	FString GetNextFeatureWorkflowPhaseId(const FAgentFeatureWorkflowState& Workflow, const FString& PhaseId)
	{
		const int32 Index = Workflow.FindPhaseIndex(PhaseId);
		return (Index != INDEX_NONE && Index + 1 < Workflow.Phases.Num())
			? Workflow.Phases[Index + 1].PhaseId
			: FString();
	}

	bool IsInteractionAccessFeatureRecipe(const FAgentFeatureWorkflowState& Workflow)
	{
		return Workflow.RecipeId.Equals(TEXT("feature.interaction_access_slice_v1"), ESearchCase::CaseSensitive);
	}

	bool IsFeatureImplementationPhase(const FAgentFeatureWorkflowState& Workflow, const FString& PhaseId)
	{
		if (IsInteractionAccessFeatureRecipe(Workflow))
		{
			return PhaseId == TEXT("project_context_preflight")
				|| PhaseId == TEXT("interaction_contract")
				|| PhaseId == TEXT("input_asset_authoring")
				|| PhaseId == TEXT("runtime_actor_state")
				|| PhaseId == TEXT("attempt_resolver_and_logging")
				|| PhaseId == TEXT("proof_context_setup");
		}

		return PhaseId == TEXT("data_model")
			|| PhaseId == TEXT("runtime_owner")
			|| PhaseId == TEXT("input_controller")
			|| PhaseId == TEXT("ui_widget");
	}

	bool IsCommandExecutionLikeFeatureTool(const FString& ToolName)
	{
		return ToolNameMatches(ToolName, TEXT("command_execution"))
			|| ToolNameMatches(ToolName, TEXT("bash"));
	}

	bool IsAutomationDiscoveryPhase(const FString& PhaseId)
	{
		return PhaseId == TEXT("automation_discovery_gate");
	}

	bool IsProofContextObservationTool(const FString& ToolName)
	{
		return ToolNameMatches(ToolName, TEXT("open_level"))
			|| ToolNameMatches(ToolName, TEXT("spawn_actor"))
			|| ToolNameMatches(ToolName, TEXT("move_actor"))
			|| ToolNameMatches(ToolName, TEXT("set_property"))
			|| ToolNameMatches(ToolName, TEXT("get_level_actors"))
			|| ToolNameMatches(ToolName, TEXT("capture_viewport"))
			|| ToolNameMatches(ToolName, TEXT("map_runtime_proof"));
	}

	bool IsInteractionAccessInputAuthoringFailureTool(
		const FOsvayderStreamEvent& Event,
		const bool bMutatingToolUse)
	{
		if (ToolNameMatches(Event.ToolName, TEXT("enhanced_input"))
			|| ToolNameMatches(Event.ToolName, TEXT("asset_search"))
			|| ToolNameMatches(Event.ToolName, TEXT("asset_dependencies"))
			|| ToolNameMatches(Event.ToolName, TEXT("asset_referencers")))
		{
			return true;
		}

		return bMutatingToolUse
			&& !IsCommandExecutionLikeFeatureTool(Event.ToolName);
	}

	bool IsInteractionAccessOptionalContractDiscoveryFailure(const FOsvayderStreamEvent& Event)
	{
		if (!Event.bIsError || !IsReadOnlyProjectInspectionTool(Event))
		{
			return false;
		}

		const FString Evidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);
		const bool bOptionalProtocolProbe =
			(Evidence.Contains(TEXT("agentbridge/protocol.md"), ESearchCase::CaseSensitive)
				|| Evidence.Contains(TEXT("agentbridge//protocol.md"), ESearchCase::CaseSensitive))
			&& ContainsAnyWorkflowEvidenceToken(
				Evidence,
				{
					TEXT("cannot find path"),
					TEXT("does not exist"),
					TEXT("pathnotfound"),
					TEXT("itemnotfoundexception"),
					TEXT("no such file"),
					TEXT("could not find")
				});
		if (bOptionalProtocolProbe)
		{
			return true;
		}

		const bool bOptionalContextSearch =
			Evidence.Contains(TEXT("rg -n"), ESearchCase::CaseSensitive)
			&& Evidence.Contains(TEXT("agentbridge"), ESearchCase::CaseSensitive)
			&& Evidence.Contains(TEXT("docs"), ESearchCase::CaseSensitive)
			&& Evidence.Contains(TEXT("saved"), ESearchCase::CaseSensitive);
		const bool bUsefulInteractionOutput =
			ContainsAnyWorkflowEvidenceToken(
				Evidence,
				{
					TEXT("alternative.prisonaccess"),
					TEXT("prooffixturesmoke"),
					TEXT("prisonaccessevent"),
					TEXT("result={success}"),
					TEXT("interaction_access_attempt_resolver_observation")
				});
		return bOptionalContextSearch && bUsefulInteractionOutput;
	}

	bool IsInteractionAccessProjectLocalReadOnlyContextProbe(
		const FOsvayderStreamEvent& Event,
		const bool bAllowError)
	{
		if ((!bAllowError && Event.bIsError) || !IsReadOnlyProjectInspectionTool(Event))
		{
			return false;
		}

		const FString Evidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);
		const bool bProjectLocalContext =
			ContainsAnyWorkflowEvidenceToken(
				Evidence,
				{
					TEXT("saved/osvayderue"),
					TEXT("docs/osvayderue"),
					TEXT("source/alternative/prisonaccess"),
					TEXT("saved/logs"),
					TEXT("/game/prisonaccess"),
					TEXT("/game/variant_sidescrolling")
				});
		const bool bInteractionAccessContext =
			ContainsAnyWorkflowEvidenceToken(
				Evidence,
				{
					TEXT("feature.interaction_access_slice_v1"),
					TEXT("prisonaccess"),
					TEXT("prison_access"),
					TEXT("alternative.prisonaccess"),
					TEXT("imc_prisonaccessproof"),
					TEXT("ia_interact"),
					TEXT("lvl_prisonaccessproof"),
					TEXT("proof_prisonaccess"),
					TEXT("prooffixturesmoke"),
					TEXT("prisonaccessevent"),
					TEXT("alternativeprisonaccess")
				});
		return bProjectLocalContext && bInteractionAccessContext;
	}

	bool IsInteractionAccessProofPrerequisitePreflightProbe(
		const FOsvayderStreamEvent& Event,
		const bool bAllowResultless)
	{
		if (Event.bIsError || !IsReadOnlyProjectInspectionTool(Event))
		{
			return false;
		}

		const FString Evidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);
		const bool bAllowedPreflightPath =
			ContainsAnyWorkflowEvidenceToken(
				Evidence,
				{
					TEXT("saved/osvayderue/active_plan.json"),
					TEXT("saved/osvayderue/closeout_decision.json"),
					TEXT("saved/osvayderue/planarchives"),
					TEXT("saved/osvayderue"),
					TEXT("docs/osvayderue"),
					TEXT("source/alternative/prisonaccess"),
					TEXT("saved/logs"),
					TEXT("/game/prisonaccess"),
					TEXT("/game/variant_sidescrolling"),
					TEXT("agentbridge/protocol.md")
				});
		if (!bAllowedPreflightPath)
		{
			return false;
		}

		const bool bProofContextNeedle =
			ContainsAnyWorkflowEvidenceToken(
				Evidence,
				{
					TEXT("feature.interaction_access_slice_v1"),
					TEXT("prisonaccess"),
					TEXT("prison_access"),
					TEXT("alternative.prisonaccess"),
					TEXT("imc_prisonaccessproof"),
					TEXT("ia_interact"),
					TEXT("lvl_prisonaccessproof"),
					TEXT("proof_prisonaccess"),
					TEXT("prooffixturesmoke"),
					TEXT("prisonaccessevent"),
					TEXT("alternativeprisonaccess")
				});
		return bAllowResultless || bProofContextNeedle;
	}

	bool IsInteractionAccessInputAuthoringReadOnlyContextProbe(const FOsvayderStreamEvent& Event)
	{
		return IsInteractionAccessProjectLocalReadOnlyContextProbe(Event, false);
	}

	bool IsInteractionAccessRuntimeActorReadOnlyContextProbe(const FOsvayderStreamEvent& Event)
	{
		return IsInteractionAccessProjectLocalReadOnlyContextProbe(Event, true);
	}

	bool ShouldRecordInteractionAccessImplementationFailure(
		const FString& PhaseId,
		const FOsvayderStreamEvent& Event,
		const bool bMutatingToolUse)
	{
		if (PhaseId == TEXT("input_asset_authoring"))
		{
			return IsInteractionAccessInputAuthoringFailureTool(Event, bMutatingToolUse);
		}
		if (PhaseId == TEXT("interaction_contract"))
		{
			return !IsInteractionAccessOptionalContractDiscoveryFailure(Event);
		}
		if (PhaseId == TEXT("runtime_actor_state")
			&& IsInteractionAccessRuntimeActorReadOnlyContextProbe(Event))
		{
			return false;
		}
		return true;
	}

	bool IsAdHocRuntimeProofAttemptTool(const FString& ToolName)
	{
		return IsCommandExecutionLikeFeatureTool(ToolName)
			|| ToolNameMatches(ToolName, TEXT("execute_script"));
	}

	bool TryExtractCommandExecutionCommand(const FString& RawJson, FString& OutCommand)
	{
		OutCommand.Reset();
		if (RawJson.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ItemObject = nullptr;
		if (RootObject->TryGetObjectField(TEXT("item"), ItemObject) && ItemObject && (*ItemObject).IsValid())
		{
			return (*ItemObject)->TryGetStringField(TEXT("command"), OutCommand);
		}

		return RootObject->TryGetStringField(TEXT("command"), OutCommand);
	}

	bool TryExtractRegexCount(const FString& Text, const FString& PatternText, int32& OutCount)
	{
		const FRegexPattern Pattern(PatternText);
		FRegexMatcher Matcher(Pattern, Text);
		if (!Matcher.FindNext())
		{
			return false;
		}

		OutCount = FCString::Atoi(*Matcher.GetCaptureGroup(1));
		return true;
	}

	int32 CountRegexMatches(const FString& Text, const FString& PatternText)
	{
		const FRegexPattern Pattern(PatternText);
		FRegexMatcher Matcher(Pattern, Text);
		int32 MatchCount = 0;
		while (Matcher.FindNext())
		{
			++MatchCount;
		}

		return MatchCount;
	}

	void CaptureAutomationDiscoveryState(FAgentFeatureWorkflowState& Workflow, const FString& Command, const FString& ResultText)
	{
		Workflow.AutomationDiscoveryCommand = Command;

		int32 ParsedCount = 0;
		if (TryExtractRegexCount(ResultText, TEXT("Found\\s+(\\d+)\\s+[Aa]utomation\\s+[Tt]ests"), ParsedCount))
		{
			Workflow.AutomationDiscoveryCount = ParsedCount;
		}
		else if (ResultText.Contains(TEXT("No automation tests matched"), ESearchCase::IgnoreCase))
		{
			Workflow.AutomationDiscoveryCount = 0;
		}

		if (TryExtractRegexCount(ResultText, TEXT("Ran\\s+(\\d+)\\s+tests"), ParsedCount))
		{
			Workflow.AutomationExecutedCount = ParsedCount;
		}
		else
		{
			const int32 CompletedResultCount =
				CountRegexMatches(ResultText, TEXT("Test\\s+Completed\\.\\s+Result=\\{[A-Za-z]+\\}"));
			if (CompletedResultCount > 0)
			{
				Workflow.AutomationExecutedCount = CompletedResultCount;
			}
			else if (ResultText.Contains(TEXT("No automation tests matched"), ESearchCase::IgnoreCase))
			{
				Workflow.AutomationExecutedCount = 0;
			}
		}

		const int32 SuccessfulCompletionCount =
			CountRegexMatches(ResultText, TEXT("Test\\s+Completed\\.\\s+Result=\\{Success\\}"));
		const int32 FailedCompletionCount =
			CountRegexMatches(ResultText, TEXT("Test\\s+Completed\\.\\s+Result=\\{(?:Fail|Failed)\\}"));

		if (TryExtractRegexCount(ResultText, TEXT("Passed\\s*[:=]\\s*(\\d+)"), ParsedCount)
			|| TryExtractRegexCount(ResultText, TEXT("Success(?:ful)?\\s*[:=]\\s*(\\d+)"), ParsedCount))
		{
			Workflow.AutomationPassedCount = ParsedCount;
		}
		else if (SuccessfulCompletionCount > 0)
		{
			Workflow.AutomationPassedCount = SuccessfulCompletionCount;
		}

		if (TryExtractRegexCount(ResultText, TEXT("Failed\\s*[:=]\\s*(\\d+)"), ParsedCount))
		{
			Workflow.AutomationFailedCount = ParsedCount;
		}
		else if (FailedCompletionCount > 0)
		{
			Workflow.AutomationFailedCount = FailedCompletionCount;
		}
		else if (SuccessfulCompletionCount > 0
			&& ResultText.Contains(TEXT("**** TEST COMPLETE. EXIT CODE: 0 ****"), ESearchCase::IgnoreCase))
		{
			Workflow.AutomationFailedCount = 0;
		}

		if (Workflow.AutomationExecutedCount == INDEX_NONE
			&& ResultText.Contains(TEXT("**** TEST COMPLETE. EXIT CODE: 0 ****"), ESearchCase::IgnoreCase))
		{
			const int32 SummaryCompletedCount =
				FMath::Max(Workflow.AutomationPassedCount, 0) + FMath::Max(Workflow.AutomationFailedCount, 0);
			if (SummaryCompletedCount > 0)
			{
				Workflow.AutomationExecutedCount = SummaryCompletedCount;
			}
		}

		if (Workflow.AutomationDiscoveryCount == 0)
		{
			Workflow.ProofPrerequisiteState = TEXT("missing");
			Workflow.BlockerFamily = TEXT("automation_discovery_failed");
			Workflow.BlockerDetail = TEXT("discoverable_automation_tests_eq_0");
		}
		else if (Workflow.AutomationExecutedCount == 0)
		{
			Workflow.ProofPrerequisiteState = TEXT("missing");
			Workflow.BlockerFamily = TEXT("automation_discovery_failed");
			Workflow.BlockerDetail = TEXT("executed_automation_tests_eq_0");
		}
		else if (Workflow.AutomationDiscoveryCount > 0)
		{
			Workflow.ProofPrerequisiteState = TEXT("satisfied");
		}
	}

	bool IsVerificationOnlyCompileGateReadOnlyEvidence(const FOsvayderStreamEvent& Event)
	{
		if (Event.bIsError)
		{
			return false;
		}

		const bool bReadOnlyInspection =
			IsReadOnlyProjectInspectionTool(Event)
			|| ToolNameMatches(Event.ToolName, TEXT("get_level_actors"))
			|| ToolNameMatches(Event.ToolName, TEXT("capture_viewport"));
		if (!bReadOnlyInspection)
		{
			return false;
		}

		const FString Evidence =
			NormalizeWorkflowEvidenceText(Event.ToolInput + TEXT("\n") + Event.RawJson + TEXT("\n") + Event.ToolResultContent);
		const bool bCompileGateContext =
			Evidence.Contains(TEXT("compile_gate"), ESearchCase::CaseSensitive)
			|| Evidence.Contains(TEXT("compile_proof"), ESearchCase::CaseSensitive)
			|| Evidence.Contains(TEXT("compiled_module_mutation"), ESearchCase::CaseSensitive)
			|| Evidence.Contains(TEXT("no c++ mutation"), ESearchCase::CaseSensitive)
			|| Evidence.Contains(TEXT("source status is read-only"), ESearchCase::CaseSensitive);
		const bool bNoCompileRequiredTruth =
			(Evidence.Contains(TEXT("compile_proof_required"), ESearchCase::CaseSensitive)
				&& Evidence.Contains(TEXT("false"), ESearchCase::CaseSensitive))
			|| (Evidence.Contains(TEXT("compiled_module_mutation_observed"), ESearchCase::CaseSensitive)
				&& Evidence.Contains(TEXT("false"), ESearchCase::CaseSensitive))
			|| Evidence.Contains(TEXT("no c++ mutation"), ESearchCase::CaseSensitive)
			|| Evidence.Contains(TEXT("compile proof is not required"), ESearchCase::CaseSensitive)
			|| Evidence.Contains(TEXT("source status is read-only"), ESearchCase::CaseSensitive);
		return bCompileGateContext && bNoCompileRequiredTruth;
	}

	void EmitFeatureWorkflowTraceEvent(
		const EOsvayderUEProviderBackend Backend,
		const FString& EventType,
		const FAgentFeatureWorkflowState& Workflow,
		const FString& PhaseId,
		const FString& Reason,
		const FString& ToolCallId)
	{
		const FString RunId = FOsvayderUEAgentTraceLog::Get().GetActiveRunIdForBackend(Backend);
		if (RunId.IsEmpty())
		{
			return;
		}

		TSharedPtr<FJsonObject> Payload = Workflow.ToJsonObject();
		if (!PhaseId.IsEmpty())
		{
			Payload->SetStringField(TEXT("phase_id"), PhaseId);
		}
		if (!Reason.IsEmpty())
		{
			Payload->SetStringField(TEXT("reason"), Reason);
		}
		if (!ToolCallId.IsEmpty())
		{
			Payload->SetStringField(TEXT("tool_call_id"), ToolCallId);
		}
		FOsvayderUEAgentTraceLog::Get().AppendEvent(EventType, Backend, Payload, RunId);
	}

	void UpdateFeatureWorkflowForToolBoundary(
		FOsvayderUEActivePlan& Plan,
		const FAgentCanonExecution& ActiveCanon,
		const FOsvayderStreamEvent& Event,
		const bool bStarting)
	{
		FAgentFeatureWorkflowState& Workflow = Plan.FeatureWorkflow;
		if (!Workflow.HasAnySignal())
		{
			return;
		}

		if (Workflow.CurrentPhase.IsEmpty())
		{
			Workflow.CurrentPhase = GetCurrentFeatureWorkflowPhaseId(Plan);
		}

		const FString BoundaryTimestampUtc = MakeUtcNowText();
		if (bStarting && !Event.ToolCallId.IsEmpty())
		{
			Workflow.ToolUseStartPhaseByCallId.Add(Event.ToolCallId, Workflow.CurrentPhase);
			Workflow.ToolUseInputByCallId.Add(Event.ToolCallId, Event.ToolInput);
			Workflow.ToolUseRawJsonByCallId.Add(Event.ToolCallId, Event.RawJson);
		}

		FOsvayderStreamEvent EvidenceEvent = Event;
		FString ToolUseStartPhase;
		const bool bHasToolUseStartPhase =
			!Event.ToolCallId.IsEmpty()
			&& Workflow.ToolUseStartPhaseByCallId.Contains(Event.ToolCallId);
		if (bHasToolUseStartPhase)
		{
			ToolUseStartPhase = Workflow.ToolUseStartPhaseByCallId.FindChecked(Event.ToolCallId);
		}
		if (!bStarting && !Event.ToolCallId.IsEmpty())
		{
			if (const FString* StartedInput = Workflow.ToolUseInputByCallId.Find(Event.ToolCallId))
			{
				EvidenceEvent.ToolInput = *StartedInput + TEXT("\n") + Event.ToolInput;
			}
			if (const FString* StartedRawJson = Workflow.ToolUseRawJsonByCallId.Find(Event.ToolCallId))
			{
				EvidenceEvent.RawJson = *StartedRawJson + TEXT("\n") + Event.RawJson;
			}
		}

		const bool bInteractionAccessRecipe = IsInteractionAccessFeatureRecipe(Workflow);
		const bool bMutatingToolUse = OsvayderUECanonRouting::IsMutatingToolUse(Event.ToolName, Event.ToolInput);
		const bool bCommandExecutionLike = IsCommandExecutionLikeFeatureTool(Event.ToolName);
		const bool bCompileTool = ToolNameMatches(Event.ToolName, TEXT("livecoding_compile"));
		const bool bRuntimeProofTool = IsPostCompileVerificationTool(Event.ToolName);
		const bool bCompiledModuleMutation =
			bMutatingToolUse && ToolInputTargetsCompiledModuleSourceMutation(Event.ToolName, Event.ToolInput);
		FString CommandExecutionCommand;
		FString CommandExtractionRawJson = Event.RawJson;
		if (!bStarting && !Event.ToolCallId.IsEmpty())
		{
			if (const FString* StartedRawJson = Workflow.ToolUseRawJsonByCallId.Find(Event.ToolCallId))
			{
				CommandExtractionRawJson = *StartedRawJson;
			}
		}
		bool bHasCommandExecutionCommand =
			TryExtractCommandExecutionCommand(CommandExtractionRawJson, CommandExecutionCommand)
			|| TryExtractCommandExecutionCommand(EvidenceEvent.RawJson, CommandExecutionCommand);
		if (!bHasCommandExecutionCommand && bCommandExecutionLike)
		{
			CommandExecutionCommand = EvidenceEvent.ToolInput.TrimStartAndEnd();
			bHasCommandExecutionCommand = !CommandExecutionCommand.IsEmpty();
		}
		const bool bAutomationDiscoveryCommand =
			bHasCommandExecutionCommand
			&& CommandExecutionCommand.Contains(TEXT("Automation RunTests"), ESearchCase::IgnoreCase);
		if (!bStarting
			&& bInteractionAccessRecipe
			&& bAutomationDiscoveryCommand)
		{
			EvidenceEvent.ToolResultContent = AppendReferencedAutomationLogEvidence(EvidenceEvent);
		}
		const bool bAutomationLogEvidence =
			bInteractionAccessRecipe
			&& HasInteractionAccessAutomationLogEvidence(EvidenceEvent);
		const bool bAutomationDiscoveryEvidence =
			bAutomationDiscoveryCommand || bAutomationLogEvidence;
		const FString AutomationDiscoveryEvidenceCommand = bAutomationDiscoveryCommand
			? CommandExecutionCommand
			: TEXT("observed Alternative.PrisonAccess automation log");
		const bool bCompileGateReadOnlyNoMutationEvidence =
			bInteractionAccessRecipe
			&& IsVerificationOnlyCompileGateReadOnlyEvidence(EvidenceEvent);
		bool bAdvancedBeforePhaseDispatch = false;

		auto SyncCurrentPhaseSurface = [&Plan, &Workflow]()
		{
			const FString PhaseId = GetCurrentFeatureWorkflowPhaseId(Plan);
			const FString Label = GetFeatureWorkflowPhaseLabel(Workflow, PhaseId);
			SetCurrentMechanic(
				Plan,
				PhaseId,
				FString::Printf(TEXT("Feature workflow phase: %s."), *Label),
				FString::Printf(TEXT("Текущая фаза feature workflow: %s."), *Label),
				FString::Printf(
					TEXT("feature_workflow_id=%s; recipe_id=%s; current_phase=%s"),
					*Workflow.FeatureWorkflowId,
					*Workflow.RecipeId,
					*PhaseId));
		};

		auto SetBlocker = [&Workflow](const FString& Family, const FString& Detail)
		{
			Workflow.BlockerFamily = Family;
			Workflow.BlockerDetail = Detail;
		};

		auto BuildProofPrerequisiteDetail = [&Workflow]()
		{
			return FString::Printf(
				TEXT("known_proof_map=%s; placed_runtime_actors=%s; automation_discovery_count=%d; reduced_proof_mode_allowed=%s"),
				Workflow.bKnownProofMapAvailable ? TEXT("true") : TEXT("false"),
				Workflow.bPlacedRuntimeActorsAvailable ? TEXT("true") : TEXT("false"),
				Workflow.AutomationDiscoveryCount,
				Workflow.bReducedProofModeAllowed ? TEXT("true") : TEXT("false"));
		};

		auto RefreshProofPrerequisites = [&Workflow]()
		{
			if (Workflow.HasRuntimeProofPrerequisites())
			{
				Workflow.ProofPrerequisiteState = TEXT("satisfied");
				if (Workflow.BlockerFamily == TEXT("proof_prerequisites_missing")
					|| Workflow.BlockerFamily == TEXT("automation_discovery_failed"))
				{
					Workflow.BlockerFamily.Reset();
					Workflow.BlockerDetail.Reset();
				}
			}
			else if (Workflow.ProofPrerequisiteState.IsEmpty())
			{
				Workflow.ProofPrerequisiteState = TEXT("pending");
			}
		};

		auto TriggerStopLoss = [&Plan, &Workflow, &Event, &SyncCurrentPhaseSurface](const FString& Reason)
		{
			FString EffectiveReason = Reason;
			if (!Workflow.BlockerFamily.IsEmpty()
				&& (Reason == TEXT("command_execution_without_phase_advance_gt_5")
					|| Reason == TEXT("ad_hoc_runtime_proof_attempts_gt_3")))
			{
				EffectiveReason = FString::Printf(TEXT("%s:%s"), *Reason, *Workflow.BlockerFamily);
			}
			Workflow.bStopLossTriggered = true;
			Workflow.StopLossReason = EffectiveReason;
			Workflow.TerminalStatus = TEXT("stop_loss");
			SyncCurrentPhaseSurface();
			EmitFeatureWorkflowTraceEvent(
				Event.Backend,
				TEXT("stop_loss_triggered"),
				Workflow,
				Workflow.CurrentPhase,
				EffectiveReason,
				Event.ToolCallId);
		};

		auto StartPhaseIfNeeded = [&Workflow, &Plan, &Event, &BoundaryTimestampUtc](const FString& PhaseId)
		{
			if (FAgentFeatureWorkflowPhaseState* Phase = Workflow.FindMutablePhase(PhaseId))
			{
				if (Phase->StartedAtUtc.IsEmpty())
				{
					Phase->StartedAtUtc = BoundaryTimestampUtc;
					EmitFeatureWorkflowTraceEvent(
						Event.Backend,
						TEXT("phase_started"),
						Workflow,
						PhaseId,
						FString(),
						Event.ToolCallId);
				}
				Phase->Status = TEXT("in_progress");
				Plan.CurrentMechanicId = PhaseId;
			}
		};

		auto CompletePhaseAndAdvance = [&Workflow, &Plan, &Event, &BoundaryTimestampUtc, &SyncCurrentPhaseSurface, &StartPhaseIfNeeded](const FString& PhaseId)
		{
			if (FAgentFeatureWorkflowPhaseState* Phase = Workflow.FindMutablePhase(PhaseId))
			{
				if (Phase->StartedAtUtc.IsEmpty())
				{
					Phase->StartedAtUtc = BoundaryTimestampUtc;
				}
				Phase->Status = TEXT("completed");
				Phase->CompletedAtUtc = BoundaryTimestampUtc;
				Phase->LastToolCallId = Event.ToolCallId;
				Phase->LastToolName = Event.ToolName;
			}

			Workflow.CompletedPhaseIds.AddUnique(PhaseId);
			Workflow.FailedPhaseIds.Remove(PhaseId);
			Workflow.CommandExecutionCallsWithoutPhaseAdvance = 0;
			SetMechanicStatus(
				Plan,
				PhaseId,
				TEXT("completed"),
				FString::Printf(TEXT("Feature workflow phase %s completed."), *PhaseId),
				FString::Printf(TEXT("Фаза feature workflow %s завершена."), *PhaseId),
				true);
			EmitFeatureWorkflowTraceEvent(
				Event.Backend,
				TEXT("phase_completed"),
				Workflow,
				PhaseId,
				FString(),
				Event.ToolCallId);

			const FString NextPhaseId = GetNextFeatureWorkflowPhaseId(Workflow, PhaseId);
			Workflow.CurrentPhase = NextPhaseId.IsEmpty() ? PhaseId : NextPhaseId;
			if (!NextPhaseId.IsEmpty())
			{
				StartPhaseIfNeeded(NextPhaseId);
			}
			SyncCurrentPhaseSurface();
		};

		auto FailPhase = [&Workflow, &Plan, &Event, &BoundaryTimestampUtc, &TriggerStopLoss](const FString& PhaseId, const FString& Reason)
		{
			if (FAgentFeatureWorkflowPhaseState* Phase = Workflow.FindMutablePhase(PhaseId))
			{
				if (Phase->StartedAtUtc.IsEmpty())
				{
					Phase->StartedAtUtc = BoundaryTimestampUtc;
				}
				Phase->Status = TEXT("failed");
				Phase->FailureCount += 1;
				Phase->LastFailureReason = Reason;
				Phase->LastToolCallId = Event.ToolCallId;
				Phase->LastToolName = Event.ToolName;
			}

			Workflow.FailedPhaseIds.AddUnique(PhaseId);
			SetMechanicStatus(
				Plan,
				PhaseId,
				TEXT("failed"),
				FString::Printf(TEXT("Feature workflow phase %s failed."), *PhaseId),
				FString::Printf(TEXT("Фаза feature workflow %s завершилась ошибкой."), *PhaseId),
				false);
			EmitFeatureWorkflowTraceEvent(
				Event.Backend,
				TEXT("phase_failed"),
				Workflow,
				PhaseId,
				Reason,
				Event.ToolCallId);

			const FAgentFeatureWorkflowPhaseState* Phase = Workflow.FindPhase(PhaseId);
			if (Phase != nullptr && Phase->FailureCount >= 2)
			{
				TriggerStopLoss(FString::Printf(TEXT("phase_failed_twice:%s"), *PhaseId));
			}
		};

		auto CanCompleteCachedRuntimeProofForEvent = [&ToolUseStartPhase, bHasToolUseStartPhase]() -> bool
		{
			return !bHasToolUseStartPhase
				|| ToolUseStartPhase.Equals(TEXT("runtime_proof"), ESearchCase::CaseSensitive);
		};

		auto CompleteCachedRuntimeProofIfReady =
			[&Workflow, &RefreshProofPrerequisites, &CompletePhaseAndAdvance, &CanCompleteCachedRuntimeProofForEvent]() -> bool
		{
			if (Workflow.CurrentPhase == TEXT("runtime_proof")
				&& !Workflow.CompletedPhaseIds.Contains(TEXT("runtime_proof"))
				&& CanCompleteCachedRuntimeProofForEvent())
			{
				RefreshProofPrerequisites();
				if (HasInteractionAccessBoundedRuntimeProofTruth(Workflow))
				{
					Workflow.RuntimeProofState = TEXT("passed");
					Workflow.AdHocProofAttemptCount = 0;
					CompletePhaseAndAdvance(TEXT("runtime_proof"));
					return true;
				}
			}
			return false;
		};

		auto CompleteCachedAutomationDiscoveryIfReady = [&Workflow, &CompletePhaseAndAdvance]() -> bool
		{
			if (Workflow.CurrentPhase == TEXT("automation_discovery_gate")
				&& !Workflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate"))
				&& HasSucceededAutomationTruth(Workflow))
			{
				CompletePhaseAndAdvance(TEXT("automation_discovery_gate"));
				return true;
			}
			return false;
		};

		auto CompleteVerificationOnlyCompileGateIfReady =
			[&Plan, &Workflow, &Event, &RefreshProofPrerequisites, &CompletePhaseAndAdvance, &CompleteCachedAutomationDiscoveryIfReady](
				const bool bHasPhaseEventEvidence,
				const bool bRequireAttemptResolverObservation) -> bool
		{
			if (Event.bIsError
				|| Workflow.CurrentPhase != TEXT("compile_gate")
				|| Workflow.CompletedPhaseIds.Contains(TEXT("compile_gate"))
				|| Workflow.bCompileProofRequired
				|| Plan.CompileProof.bCompiledModuleMutationObserved)
			{
				return false;
			}

			RefreshProofPrerequisites();
			if (!Workflow.HasRuntimeProofPrerequisites())
			{
				return false;
			}
			if (bRequireAttemptResolverObservation
				&& !Workflow.InteractionAccessAttemptResolverObservation.HasSufficientSourceEvidence())
			{
				return false;
			}

			if (!bHasPhaseEventEvidence)
			{
				return false;
			}

			Workflow.CompileProofState = TEXT("not_required");
			CompletePhaseAndAdvance(TEXT("compile_gate"));
			CompleteCachedAutomationDiscoveryIfReady();
			return true;
		};

		auto HydrateInteractionAccessCurrentPhaseFromEvidence =
			[&Workflow, &RefreshProofPrerequisites, &CompletePhaseAndAdvance, &CompleteVerificationOnlyCompileGateIfReady, &CompleteCachedAutomationDiscoveryIfReady, &CompleteCachedRuntimeProofIfReady]() -> bool
		{
			bool bAdvanced = false;
			const int32 GuardLimit = Workflow.Phases.Num() + 4;
			for (int32 GuardIndex = 0; GuardIndex < GuardLimit; ++GuardIndex)
			{
				const FString PhaseId = Workflow.CurrentPhase;
				if (PhaseId.IsEmpty())
				{
					break;
				}

				if (PhaseId == TEXT("input_asset_authoring")
					&& !Workflow.CompletedPhaseIds.Contains(TEXT("input_asset_authoring"))
					&& Workflow.InteractionAccessReuseObservation.HasSufficientReuseEvidence())
				{
					Workflow.AuthoringLaneState = TEXT("persistent_input_reuse_verified");
					Workflow.AuthoringDecision = TEXT("persistent_input_assets_reuse_verified_via_project_evidence");
					if (Workflow.BlockerFamily.Equals(TEXT("authoring_lane_denied"), ESearchCase::IgnoreCase))
					{
						Workflow.BlockerFamily.Reset();
						Workflow.BlockerDetail.Reset();
					}
					CompletePhaseAndAdvance(TEXT("input_asset_authoring"));
					bAdvanced = true;
					continue;
				}

				if (PhaseId == TEXT("runtime_actor_state")
					&& !Workflow.CompletedPhaseIds.Contains(TEXT("runtime_actor_state"))
					&& Workflow.bPlacedRuntimeActorsAvailable)
				{
					RefreshProofPrerequisites();
					CompletePhaseAndAdvance(TEXT("runtime_actor_state"));
					bAdvanced = true;
					continue;
				}

				if (PhaseId == TEXT("attempt_resolver_and_logging")
					&& !Workflow.CompletedPhaseIds.Contains(TEXT("attempt_resolver_and_logging"))
					&& Workflow.InteractionAccessAttemptResolverObservation.HasSufficientSourceEvidence())
				{
					CompletePhaseAndAdvance(TEXT("attempt_resolver_and_logging"));
					bAdvanced = true;
					continue;
				}

				if (PhaseId == TEXT("proof_context_setup")
					&& !Workflow.CompletedPhaseIds.Contains(TEXT("proof_context_setup")))
				{
					break;
				}

				if (PhaseId == TEXT("automation_discovery_gate")
					&& !Workflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")))
				{
					if (!CompleteCachedAutomationDiscoveryIfReady())
					{
						break;
					}
					bAdvanced = true;
					continue;
				}

				if (PhaseId == TEXT("runtime_proof")
					&& !Workflow.CompletedPhaseIds.Contains(TEXT("runtime_proof")))
				{
					if (!CompleteCachedRuntimeProofIfReady())
					{
						break;
					}
					bAdvanced = true;
					continue;
				}

				break;
			}
			return bAdvanced;
		};

		if (bInteractionAccessRecipe
			&& (Event.ToolInput.Contains(TEXT("reduced proof mode"), ESearchCase::IgnoreCase)
				|| Event.ToolResultContent.Contains(TEXT("reduced proof mode"), ESearchCase::IgnoreCase)
				|| Event.ToolInput.Contains(TEXT("runtime-only prototype"), ESearchCase::IgnoreCase)
				|| Event.ToolResultContent.Contains(TEXT("runtime-only prototype"), ESearchCase::IgnoreCase)))
		{
			Workflow.bReducedProofModeAllowed = true;
			if (Workflow.AuthoringDecision.IsEmpty())
			{
				Workflow.AuthoringDecision = TEXT("explicit_degraded_runtime_only_input_allowed");
			}
		}

		if (bInteractionAccessRecipe
			&& bStarting
			&& ToolNameMatches(Event.ToolName, TEXT("map_runtime_proof")))
		{
			Workflow.bKnownProofMapAvailable = true;
			Workflow.bPlacedRuntimeActorsAvailable = true;
			RefreshProofPrerequisites();
		}

		if (bStarting && bCompiledModuleMutation)
		{
			Plan.CompileProof.bCompiledModuleMutationObserved = true;
			Plan.CompileProof.MutationToolFamily = !ActiveCanon.PrimaryMutationToolFamily.IsEmpty()
				? ActiveCanon.PrimaryMutationToolFamily
				: OsvayderUECanonRouting::DetermineToolFamily(ActiveCanon, Event.ToolName, Event.ToolInput);
			if (Plan.CompileProof.MutationToolFamily.IsEmpty())
			{
				Plan.CompileProof.MutationToolFamily = TEXT("workspace_file_build");
			}
			Plan.CompileProof.LastMutationAtUtc = BoundaryTimestampUtc;
			Plan.CompileProof.LastMutationToolCallId = Event.ToolCallId;
			Plan.CompileProof.LastMutationToolName = Event.ToolName;
			Workflow.bCompileProofRequired = true;
			Workflow.CompileProofState = TEXT("pending");
		}

		if (Workflow.bStopLossTriggered)
		{
			SyncCurrentPhaseSurface();
			return;
		}

		const FInteractionAccessReuseObservationState ReuseObservationBeforeGlobal =
			Workflow.InteractionAccessReuseObservation;
		const FInteractionAccessAttemptResolverObservationState AttemptResolverObservationBeforeGlobal =
			Workflow.InteractionAccessAttemptResolverObservation;
		const bool bKnownProofMapBeforeGlobal = Workflow.bKnownProofMapAvailable;
		const bool bPlacedRuntimeActorsBeforeGlobal = Workflow.bPlacedRuntimeActorsAvailable;
		const int32 AutomationDiscoveryCountBeforeGlobal = Workflow.AutomationDiscoveryCount;
		const int32 AutomationExecutedCountBeforeGlobal = Workflow.AutomationExecutedCount;
		const int32 AutomationPassedCountBeforeGlobal = Workflow.AutomationPassedCount;
		const int32 AutomationFailedCountBeforeGlobal = Workflow.AutomationFailedCount;
		bool bInteractionAccessEvidenceProgressed = false;
		bool bInteractionAccessEvidenceConfirmed = false;

		if (!bStarting
			&& bInteractionAccessRecipe
			&& !Event.bIsError)
		{
			if (ToolNameMatches(EvidenceEvent.ToolName, TEXT("open_level"))
				|| ToolNameMatches(EvidenceEvent.ToolName, TEXT("map_runtime_proof")))
			{
				Workflow.bKnownProofMapAvailable = true;
			}
			if (ToolNameMatches(EvidenceEvent.ToolName, TEXT("spawn_actor"))
				|| ToolNameMatches(EvidenceEvent.ToolName, TEXT("move_actor"))
				|| ToolNameMatches(EvidenceEvent.ToolName, TEXT("set_property")))
			{
				Workflow.bPlacedRuntimeActorsAvailable = true;
			}
			else if (HasInteractionAccessPlacedRuntimeActorObservation(Workflow, EvidenceEvent))
			{
				Workflow.bPlacedRuntimeActorsAvailable = true;
			}
			if (bAutomationDiscoveryEvidence)
			{
				CaptureAutomationDiscoveryState(Workflow, AutomationDiscoveryEvidenceCommand, EvidenceEvent.ToolResultContent);
			}
			ObserveInteractionAccessPersistentInputReuseEvidence(Workflow, EvidenceEvent);
			ObserveInteractionAccessAttemptResolverEvidence(Workflow, EvidenceEvent);
			RefreshProofPrerequisites();
			bInteractionAccessEvidenceProgressed =
				DidInteractionAccessReuseObservationProgress(
					ReuseObservationBeforeGlobal,
					Workflow.InteractionAccessReuseObservation)
				|| DidInteractionAccessAttemptResolverObservationProgress(
					AttemptResolverObservationBeforeGlobal,
					Workflow.InteractionAccessAttemptResolverObservation)
				|| bKnownProofMapBeforeGlobal != Workflow.bKnownProofMapAvailable
				|| bPlacedRuntimeActorsBeforeGlobal != Workflow.bPlacedRuntimeActorsAvailable
				|| AutomationDiscoveryCountBeforeGlobal != Workflow.AutomationDiscoveryCount
				|| AutomationExecutedCountBeforeGlobal != Workflow.AutomationExecutedCount
				|| AutomationPassedCountBeforeGlobal != Workflow.AutomationPassedCount
				|| AutomationFailedCountBeforeGlobal != Workflow.AutomationFailedCount;
			bInteractionAccessEvidenceConfirmed =
				HasInteractionAccessEvidenceSignal(Workflow, EvidenceEvent);
		}

		bAdvancedBeforePhaseDispatch = !bStarting
			&& bInteractionAccessRecipe
			&& CompleteVerificationOnlyCompileGateIfReady(
				bAutomationDiscoveryEvidence || bRuntimeProofTool || bCompileGateReadOnlyNoMutationEvidence,
				bCompileGateReadOnlyNoMutationEvidence && !bAutomationDiscoveryEvidence && !bRuntimeProofTool);
		bAdvancedBeforePhaseDispatch =
			(!bStarting && bInteractionAccessRecipe && HydrateInteractionAccessCurrentPhaseFromEvidence())
			|| bAdvancedBeforePhaseDispatch;

		const bool bCompileGateClosedOrNotCurrent =
			Workflow.CurrentPhase != TEXT("compile_gate")
			|| Workflow.CompletedPhaseIds.Contains(TEXT("compile_gate"));

		if (!bStarting
			&& bInteractionAccessRecipe
			&& bAutomationDiscoveryEvidence
			&& bCompileGateClosedOrNotCurrent
			&& Workflow.CurrentPhase == TEXT("automation_discovery_gate")
			&& !Workflow.CompletedPhaseIds.Contains(TEXT("automation_discovery_gate")))
		{
			Workflow.CurrentPhase = TEXT("automation_discovery_gate");
		}

		const FString PhaseId = Workflow.CurrentPhase;
		const bool bToolResultStartedBeforeCurrentPhase =
			!bStarting
			&& bHasToolUseStartPhase
			&& !ToolUseStartPhase.Equals(PhaseId, ESearchCase::CaseSensitive);
		const FInteractionAccessAttemptResolverObservationState AttemptResolverObservationBefore =
			Workflow.InteractionAccessAttemptResolverObservation;
		const bool bPersistentInputReuseVerified =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("input_asset_authoring")
			&& ObserveInteractionAccessPersistentInputReuseEvidence(Workflow, EvidenceEvent);
		const bool bInteractionContractVerified =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("interaction_contract")
			&& HasInteractionAccessContractEvidence(EvidenceEvent);
		const bool bInteractionContractOptionalReadOnlyContextFailure =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("interaction_contract")
			&& IsInteractionAccessOptionalContractDiscoveryFailure(EvidenceEvent);
		const bool bInteractionContractProofPreflightStartProbe =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("interaction_contract")
			&& bStarting
			&& IsInteractionAccessProofPrerequisitePreflightProbe(EvidenceEvent, true);
		const bool bInteractionContractProofPreflightResultProbe =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("interaction_contract")
			&& !bStarting
			&& IsInteractionAccessProofPrerequisitePreflightProbe(EvidenceEvent, false);
		const bool bInputAuthoringReadOnlyContextProbe =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("input_asset_authoring")
			&& IsInteractionAccessInputAuthoringReadOnlyContextProbe(EvidenceEvent);
		const bool bRuntimeActorReadOnlyContextProbe =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("runtime_actor_state")
			&& IsInteractionAccessRuntimeActorReadOnlyContextProbe(EvidenceEvent);
		const bool bProofContextObservationVerified =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("proof_context_setup")
			&& !bToolResultStartedBeforeCurrentPhase
			&& HasInteractionAccessPlacedRuntimeActorObservation(Workflow, EvidenceEvent);
		const bool bRuntimeActorStateVerified =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("runtime_actor_state")
			&& Workflow.bPlacedRuntimeActorsAvailable;
		const bool bAttemptResolverAndLoggingVerified =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("attempt_resolver_and_logging")
			&& ObserveInteractionAccessAttemptResolverEvidence(Workflow, EvidenceEvent)
			&& Workflow.InteractionAccessAttemptResolverObservation.HasSufficientSourceEvidence();
		const bool bAttemptResolverObservationProgressed =
			bInteractionAccessRecipe
			&& PhaseId == TEXT("attempt_resolver_and_logging")
			&& DidInteractionAccessAttemptResolverObservationProgress(
				AttemptResolverObservationBefore,
				Workflow.InteractionAccessAttemptResolverObservation);
		const bool bAttemptResolverReadOnlyCommandProgress =
			bAttemptResolverObservationProgressed
			&& bCommandExecutionLike
			&& IsReadOnlyProjectInspectionTool(EvidenceEvent);
		StartPhaseIfNeeded(PhaseId);
		if (FAgentFeatureWorkflowPhaseState* Phase = Workflow.FindMutablePhase(PhaseId))
		{
			const bool bAttemptWorthy = bMutatingToolUse || bCommandExecutionLike || bCompileTool || bRuntimeProofTool;
			if (bStarting && bAttemptWorthy)
			{
				Phase->AttemptCount += 1;
				Phase->LastToolCallId = Event.ToolCallId;
				Phase->LastToolName = Event.ToolName;
			}
		}

		if (bStarting
			&& bRuntimeProofTool
			&& Workflow.bCompileProofRequired
			&& !Workflow.CompileProofState.Equals(TEXT("passed"), ESearchCase::IgnoreCase))
		{
			Workflow.RuntimeProofState = TEXT("denied");
			FailPhase(TEXT("runtime_proof"), TEXT("runtime_proof_denied_pending_compile"));
			SyncCurrentPhaseSurface();
			return;
		}

		if (bStarting
			&& bRuntimeProofTool
			&& bInteractionAccessRecipe)
		{
			RefreshProofPrerequisites();
			if (!Workflow.HasRuntimeProofPrerequisites())
			{
				Workflow.ProofPrerequisiteState = TEXT("missing");
				Workflow.RuntimeProofState = TEXT("blocked");
				if (Workflow.BlockerFamily.IsEmpty())
				{
					SetBlocker(TEXT("proof_prerequisites_missing"), BuildProofPrerequisiteDetail());
				}
				else
				{
					Workflow.BlockerDetail = BuildProofPrerequisiteDetail();
				}
				FailPhase(TEXT("runtime_proof"), Workflow.BlockerFamily.IsEmpty() ? TEXT("proof_prerequisites_missing") : Workflow.BlockerFamily);
				SyncCurrentPhaseSurface();
				return;
			}
			if (!Workflow.InteractionAccessAttemptResolverObservation.HasSufficientSourceEvidence())
			{
				Workflow.RuntimeProofState = TEXT("blocked");
				Workflow.CurrentPhase = TEXT("attempt_resolver_and_logging");
				if (Workflow.BlockerFamily.IsEmpty())
				{
					SetBlocker(
						TEXT("attempt_resolver_observation_missing"),
						TEXT("interaction_access_attempt_resolver_observation_empty"));
				}
				StartPhaseIfNeeded(TEXT("attempt_resolver_and_logging"));
				SyncCurrentPhaseSurface();
				return;
			}
		}

		if (bStarting)
		{
			SyncCurrentPhaseSurface();
			return;
		}

		bool bAdvancedPhase = bAdvancedBeforePhaseDispatch;
		bool bPhaseFailureRecorded = false;

		if (bInteractionAccessRecipe)
		{
			FAgentExecutionPolicyDenyContract DenyContract;
			if (TryParsePolicyDenyContractFromToolResultJson(Event.ToolResultContent, DenyContract))
			{
				const bool bPersistentInputAuthoringDenied =
					DenyContract.PolicyRuleId == TEXT("workspace_write_project.broad_authoring_mutation_surface_denied")
					&& (ToolNameMatches(Event.ToolName, TEXT("enhanced_input"))
						|| DenyContract.RequestedAction.Contains(TEXT("enhanced_input"), ESearchCase::IgnoreCase)
						|| Event.ToolResultContent.Contains(TEXT("'enhanced_input'"), ESearchCase::IgnoreCase)
						|| Event.ToolResultContent.Contains(TEXT("\"enhanced_input\""), ESearchCase::IgnoreCase));
				if (bPersistentInputAuthoringDenied)
				{
					Workflow.AuthoringLaneState = TEXT("denied");
					Workflow.AuthoringPolicyRuleId = DenyContract.PolicyRuleId;
					Workflow.AuthoringDecision = Workflow.bReducedProofModeAllowed
						? TEXT("explicit_degraded_runtime_only_input_allowed")
						: TEXT("stop_for_replan_no_silent_runtime_only_input_fallback");
					SetBlocker(TEXT("authoring_lane_denied"), DenyContract.PolicyRuleId);
					if (Workflow.CurrentPhase != TEXT("input_asset_authoring"))
					{
						Workflow.CurrentPhase = TEXT("input_asset_authoring");
						StartPhaseIfNeeded(TEXT("input_asset_authoring"));
					}
					FailPhase(TEXT("input_asset_authoring"), TEXT("authoring_lane_denied"));
					bPhaseFailureRecorded = true;
				}
			}
		}

		if (bCompileTool)
		{
			const bool bCompileSucceeded = !Event.bIsError;
			Workflow.bCompileProofRequired = true;
			Workflow.CompileProofState = bCompileSucceeded ? TEXT("passed") : TEXT("failed");
			RecordCompileProof(
				Plan.CompileProof,
				BoundaryTimestampUtc,
				Event.ToolCallId,
				Event.ToolName,
				bCompileSucceeded ? TEXT("success") : TEXT("failed"),
				NormalizeSingleLineText(Event.ToolResultContent.Left(400)));
			if (Workflow.CurrentPhase != TEXT("compile_gate"))
			{
				Workflow.CurrentPhase = TEXT("compile_gate");
				StartPhaseIfNeeded(TEXT("compile_gate"));
			}
			if (bCompileSucceeded)
			{
				CompletePhaseAndAdvance(TEXT("compile_gate"));
				bAdvancedPhase = true;
			}
			else
			{
				FailPhase(TEXT("compile_gate"), TEXT("compile_gate_failed"));
				bPhaseFailureRecorded = true;
			}
		}
		else if (!bPhaseFailureRecorded
			&& bAutomationDiscoveryEvidence
			&& bCompileGateClosedOrNotCurrent
			&& Workflow.CurrentPhase == TEXT("automation_discovery_gate"))
		{
			if (Workflow.CurrentPhase != TEXT("automation_discovery_gate"))
			{
				Workflow.CurrentPhase = TEXT("automation_discovery_gate");
				StartPhaseIfNeeded(TEXT("automation_discovery_gate"));
			}

			CaptureAutomationDiscoveryState(Workflow, AutomationDiscoveryEvidenceCommand, EvidenceEvent.ToolResultContent);
			RefreshProofPrerequisites();
			if (!Event.bIsError && HasSucceededAutomationTruth(Workflow))
			{
				CompletePhaseAndAdvance(TEXT("automation_discovery_gate"));
				bAdvancedPhase = true;
			}
			else
			{
				Workflow.RuntimeProofState = TEXT("blocked");
				FailPhase(TEXT("automation_discovery_gate"), TEXT("automation_discovery_failed"));
				bPhaseFailureRecorded = true;
			}
		}
		else if (bRuntimeProofTool)
		{
			if (bInteractionAccessRecipe
				&& !Workflow.InteractionAccessAttemptResolverObservation.HasSufficientSourceEvidence())
			{
				Workflow.RuntimeProofState = TEXT("blocked");
				Workflow.CurrentPhase = TEXT("attempt_resolver_and_logging");
				if (Workflow.BlockerFamily.IsEmpty())
				{
					SetBlocker(
						TEXT("attempt_resolver_observation_missing"),
						TEXT("interaction_access_attempt_resolver_observation_empty"));
				}
				StartPhaseIfNeeded(TEXT("attempt_resolver_and_logging"));
				SyncCurrentPhaseSurface();
				return;
			}

			const bool bRuntimePassed = !Event.bIsError;
			Workflow.RuntimeProofState = bRuntimePassed ? TEXT("passed") : TEXT("failed");
			RecordPostCompileVerification(
				Plan.CompileProof,
				BoundaryTimestampUtc,
				Event.ToolCallId,
				Event.ToolName,
				bRuntimePassed ? TEXT("pass") : TEXT("fail"));
			if (Workflow.CurrentPhase != TEXT("runtime_proof"))
			{
				Workflow.CurrentPhase = TEXT("runtime_proof");
				StartPhaseIfNeeded(TEXT("runtime_proof"));
			}
			if (bRuntimePassed)
			{
				CompletePhaseAndAdvance(TEXT("runtime_proof"));
				bAdvancedPhase = true;
			}
			else
			{
				FailPhase(TEXT("runtime_proof"), TEXT("runtime_proof_failed"));
				bPhaseFailureRecorded = true;
			}
		}
		else if (!bPhaseFailureRecorded
			&& !bAdvancedPhase
			&& !Event.bIsError
			&& IsFeatureImplementationPhase(Workflow, PhaseId)
			&& (bMutatingToolUse
				|| bInteractionContractVerified
				|| bPersistentInputReuseVerified
				|| bRuntimeActorStateVerified
				|| bAttemptResolverAndLoggingVerified
				|| bProofContextObservationVerified))
		{
			bool bCanCompleteImplementationPhase = true;
			if (bInteractionAccessRecipe)
			{
				if (PhaseId == TEXT("input_asset_authoring"))
				{
					const bool bPersistentInputMutation =
						bMutatingToolUse
						&& (ToolNameMatches(Event.ToolName, TEXT("enhanced_input"))
							|| ToolNameMatches(Event.ToolName, TEXT("asset")));
					if (bPersistentInputMutation)
					{
						Workflow.AuthoringLaneState = TEXT("persistent_authoring_recorded");
						Workflow.AuthoringDecision = TEXT("persistent_input_assets_authored_or_reused");
					}
					else if (bPersistentInputReuseVerified)
					{
						Workflow.AuthoringLaneState = TEXT("persistent_input_reuse_verified");
						Workflow.AuthoringDecision = TEXT("persistent_input_assets_reuse_verified_via_project_evidence");
						if (Workflow.BlockerFamily.Equals(TEXT("authoring_lane_denied"), ESearchCase::IgnoreCase))
						{
							Workflow.BlockerFamily.Reset();
							Workflow.BlockerDetail.Reset();
						}
					}
					bCanCompleteImplementationPhase =
						bPersistentInputMutation
						|| bPersistentInputReuseVerified
						|| Workflow.bReducedProofModeAllowed;
					if (!bCanCompleteImplementationPhase
						&& Workflow.AuthoringLaneState.Equals(TEXT("denied"), ESearchCase::IgnoreCase))
					{
						SetBlocker(TEXT("authoring_lane_denied"), Workflow.AuthoringPolicyRuleId);
					}
				}
				else if (PhaseId == TEXT("interaction_contract"))
				{
					bCanCompleteImplementationPhase = bInteractionContractVerified;
				}
				else if (PhaseId == TEXT("runtime_actor_state"))
				{
					RefreshProofPrerequisites();
					bCanCompleteImplementationPhase = Workflow.bPlacedRuntimeActorsAvailable;
				}
				else if (PhaseId == TEXT("attempt_resolver_and_logging"))
				{
					bCanCompleteImplementationPhase = bAttemptResolverAndLoggingVerified;
				}
				else if (PhaseId == TEXT("proof_context_setup"))
				{
					RefreshProofPrerequisites();
					bCanCompleteImplementationPhase = Workflow.HasRuntimeProofPrerequisites();
					if (!bCanCompleteImplementationPhase)
					{
						Workflow.ProofPrerequisiteState = TEXT("missing");
						if (Workflow.BlockerFamily.IsEmpty())
						{
							SetBlocker(TEXT("proof_prerequisites_missing"), BuildProofPrerequisiteDetail());
						}
						else
						{
							Workflow.BlockerDetail = BuildProofPrerequisiteDetail();
						}
					}
				}
			}

			if (bCanCompleteImplementationPhase)
			{
				CompletePhaseAndAdvance(PhaseId);
				bAdvancedPhase = true;
			}
		}
		else if (!bPhaseFailureRecorded
			&& !bAdvancedPhase
			&& Event.bIsError
			&& IsFeatureImplementationPhase(Workflow, PhaseId)
			&& (bMutatingToolUse || bCommandExecutionLike)
			&& (!bInteractionAccessRecipe
				|| ShouldRecordInteractionAccessImplementationFailure(PhaseId, Event, bMutatingToolUse)))
		{
			FailPhase(PhaseId, TEXT("implementation_tool_failed"));
			bPhaseFailureRecorded = true;
		}

		if (!bPhaseFailureRecorded
			&& bInteractionAccessRecipe
			&& HydrateInteractionAccessCurrentPhaseFromEvidence())
		{
			bAdvancedPhase = true;
		}

		if (!bAdvancedPhase && bCommandExecutionLike)
		{
			if (bAttemptResolverReadOnlyCommandProgress || bInteractionAccessEvidenceProgressed)
			{
				Workflow.CommandExecutionCallsWithoutPhaseAdvance = 0;
			}
			else if (bInteractionAccessEvidenceConfirmed)
			{
				// Confirming already-cached project evidence is not phase progress,
				// but it should not consume the command drift budget.
			}
			else if (bToolResultStartedBeforeCurrentPhase
				&& bInteractionAccessRecipe
				&& IsReadOnlyProjectInspectionTool(EvidenceEvent))
			{
				// A result belongs to the phase where the tool call started; delayed read-only
				// output must not spend the newer phase's stop-loss budget.
			}
			else if (bInteractionContractOptionalReadOnlyContextFailure)
			{
				// Optional protocol/context discovery from the wrong project root is not phase progress,
				// but it also must not consume the interaction_contract stop-loss budget.
			}
			else if (bInteractionContractProofPreflightStartProbe)
			{
				// Wait for the result of bounded proof-context preflight reads before charging drift.
			}
			else if (bInteractionContractProofPreflightResultProbe)
			{
				// Bounded proof-context preflight checks are required before UE proof tools can run.
			}
			else if (bInputAuthoringReadOnlyContextProbe)
			{
				// Live verification often performs bounded project-local context/source/log probes
				// before the phase-local enhanced_input proof. Keep drift protection for irrelevant commands.
			}
			else if (bRuntimeActorReadOnlyContextProbe)
			{
				// Bounded project-local source/docs/log probes around runtime_actor_state are context checks;
				// the phase still requires Unreal actor observation before it can complete.
			}
			else if (bCompileGateReadOnlyNoMutationEvidence
				&& bInteractionAccessRecipe
				&& Workflow.CurrentPhase == TEXT("compile_gate")
				&& !Workflow.bCompileProofRequired)
			{
				// Verification-only compile_gate status/source checks prove no compile is required;
				// if other prerequisites are still missing, they should wait without spending drift budget.
			}
			else
			{
				Workflow.CommandExecutionCallsWithoutPhaseAdvance += 1;
				if (Workflow.CommandExecutionCallsWithoutPhaseAdvance > 5)
				{
					if (bInteractionAccessRecipe
						&& Workflow.BlockerFamily.IsEmpty()
						&& !Workflow.HasRuntimeProofPrerequisites())
					{
						Workflow.ProofPrerequisiteState = TEXT("missing");
						SetBlocker(TEXT("proof_prerequisites_missing"), BuildProofPrerequisiteDetail());
					}
					TriggerStopLoss(TEXT("command_execution_without_phase_advance_gt_5"));
				}
			}
		}

		if (Workflow.CurrentPhase == TEXT("runtime_proof") && IsAdHocRuntimeProofAttemptTool(Event.ToolName))
		{
			Workflow.AdHocProofAttemptCount += 1;
			if (Workflow.AdHocProofAttemptCount > 3)
			{
				if (bInteractionAccessRecipe
					&& Workflow.BlockerFamily.IsEmpty()
					&& !Workflow.HasRuntimeProofPrerequisites())
				{
					Workflow.ProofPrerequisiteState = TEXT("missing");
					SetBlocker(TEXT("proof_prerequisites_missing"), BuildProofPrerequisiteDetail());
				}
				TriggerStopLoss(TEXT("ad_hoc_runtime_proof_attempts_gt_3"));
			}
		}

		SyncCurrentPhaseSurface();
	}

	FString DescribeSettingsParity(const FOsvayderUERelaySettingsSnapshot& Expected, const FOsvayderUERelaySettingsSnapshot& Current)
	{
		if (Expected.Model.Equals(Current.Model, ESearchCase::CaseSensitive)
			&& Expected.Profile.Equals(Current.Profile, ESearchCase::CaseSensitive)
			&& Expected.WorkMode.Equals(Current.WorkMode, ESearchCase::CaseSensitive)
			&& Expected.ReasoningEffort.Equals(Current.ReasoningEffort, ESearchCase::CaseSensitive)
			&& Expected.Verbosity.Equals(Current.Verbosity, ESearchCase::CaseSensitive))
		{
			return TEXT("matched");
		}

		return TEXT("mismatch");
	}
}

void SOsvayderEditorWidget::ParseCodeFences(const FString& Input, TArray<TPair<FString, bool>>& OutSections)
{
	OutSections.Empty();

	int32 SearchFrom = 0;
	bool bInCodeBlock = false;
	int32 LastSplitPos = 0;

	while (SearchFrom < Input.Len())
	{
		int32 FencePos = Input.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
		if (FencePos == INDEX_NONE)
		{
			break;
		}

		if (!bInCodeBlock)
		{
			// Opening fence - text before it is plain text
			FString PlainText = Input.Mid(LastSplitPos, FencePos - LastSplitPos);
			if (!PlainText.IsEmpty())
			{
				OutSections.Add(TPair<FString, bool>(PlainText, false));
			}

			// Skip past the opening fence line (including language tag)
			int32 LineEnd = Input.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FencePos + 3);
			if (LineEnd == INDEX_NONE)
			{
				LineEnd = Input.Len();
			}

			LastSplitPos = LineEnd + 1;
			SearchFrom = LastSplitPos;
			bInCodeBlock = true;
		}
		else
		{
			// Closing fence - text before it is code
			FString CodeText = Input.Mid(LastSplitPos, FencePos - LastSplitPos);
			CodeText.TrimEndInline();
			if (!CodeText.IsEmpty())
			{
				OutSections.Add(TPair<FString, bool>(CodeText, true));
			}

			// Skip past the closing fence
			int32 LineEnd = Input.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FencePos + 3);
			if (LineEnd == INDEX_NONE)
			{
				LastSplitPos = FencePos + 3;
			}
			else
			{
				LastSplitPos = LineEnd + 1;
			}

			SearchFrom = LastSplitPos;
			bInCodeBlock = false;
		}
	}

	// Remaining text after last fence
	if (LastSplitPos < Input.Len())
	{
		FString Remaining = Input.Mid(LastSplitPos);
		if (!Remaining.IsEmpty())
		{
			OutSections.Add(TPair<FString, bool>(Remaining, bInCodeBlock));
		}
	}
}

void SOsvayderEditorWidget::ApplyFeatureWorkflowToolBoundaryForTest(
	FOsvayderUEActivePlan& Plan,
	const FAgentCanonExecution& ActiveCanon,
	const FOsvayderStreamEvent& Event,
	bool bStarting)
{
	UpdateFeatureWorkflowForToolBoundary(Plan, ActiveCanon, Event, bStarting);
}

void SOsvayderEditorWidget::ParseAndRenderCodeBlocks()
{
	// 626 P1 crash-safety: take local snapshots of the shared_ptr arrays
	// (+ text segments) at function entry. This increments the refcount on
	// each controller for the duration of the iteration, so even if some
	// other path racily mutates or clears the member arrays mid-loop, our
	// snapshots keep the underlying SWidget objects alive and the iteration
	// walks an immutable copy. Prevents the
	// `ReleaseSharedReferenceNoInline -> ParseAndRenderCodeBlocks:4721`
	// double-release crash documented in observed_failures.md rows #26/#28
	// and reproduced 3+ times in the 626 P1 dispatch.
	const TArray<TSharedPtr<SMultiLineEditableText>> BlockSnapshot = TextSegmentBlocks;
	const TArray<TSharedPtr<SVerticalBox>> ContainerSnapshot = TextSegmentContainers;
	const TArray<FString> SegmentTextSnapshot = AllTextSegments;

	for (int32 i = 0; i < BlockSnapshot.Num() && i < ContainerSnapshot.Num(); ++i)
	{
		TSharedPtr<SMultiLineEditableText> Block = BlockSnapshot[i];
		TSharedPtr<SVerticalBox> Container = ContainerSnapshot[i];

		if (!Block.IsValid() || !Container.IsValid())
		{
			continue;
		}

		// Get the segment text
		FString SegmentText;
		if (i < SegmentTextSnapshot.Num())
		{
			SegmentText = SegmentTextSnapshot[i];
		}
		else
		{
			// Defensive: guard against a malformed error body producing a
			// null or otherwise ungettable text on the underlying block.
			// In practice GetText() on a live SMultiLineEditableText does not
			// throw, but parser input hardening is a 626 P1 requirement per
			// spec Fix #4 risks #3 analogue.
			SegmentText = Block->GetText().ToString();
		}

		if (!SegmentText.Contains(TEXT("```")))
		{
			continue;
		}

		// Parse into code and plain text sections
		TArray<TPair<FString, bool>> Sections;
		ParseCodeFences(SegmentText, Sections);

		if (Sections.Num() <= 1)
		{
			continue;
		}

		// Replace container contents with parsed sections
		Container->ClearChildren();

		for (const TPair<FString, bool>& Section : Sections)
		{
			if (Section.Value)
			{
				// Code block: dark background + monospace font
				Container->AddSlot()
				.AutoHeight()
				.Padding(0, 4, 0, 4)
				[
					SNew(SBorder)
					.BorderImage(OsvayderSlateStyle::CodeBlockBrush())
					.Padding(FMargin(10.0f, 8.0f))
					[
						SNew(SMultiLineEditableText)
						.Text(FText::FromString(Section.Key))
						.TextStyle(&GetSelectableCodeStyle())
						.IsReadOnly(true)
						.AutoWrapText(true)
					]
				];
			}
			else
			{
				// Plain text
				Container->AddSlot()
				.AutoHeight()
				[
					SNew(SMultiLineEditableText)
					.Text(FText::FromString(Section.Key))
					.TextStyle(&GetSelectableNormalStyle())
					.IsReadOnly(true)
					.AutoWrapText(true)
				];
			}
		}
	}
}

void SOsvayderEditorWidget::AppendToLastResponse(const FString& Text)
{
	// Delegate to OnClaudeProgress for streaming updates
	OnClaudeProgress(Text, ActiveRequestGeneration, ActiveRequestBackend);
}

void SOsvayderEditorWidget::RefreshProjectContext()
{
	AddMessage(TEXT("Refreshing project context..."), false);

	FProjectContextManager::Get().RefreshContext();

	FString Summary = FProjectContextManager::Get().GetContextSummary();
	AddMessage(FString::Printf(TEXT("Project context updated: %s"), *Summary), false);

	TSharedPtr<FJsonObject> TracePayload = MakeShared<FJsonObject>();
	TracePayload->SetStringField(TEXT("summary"), Summary);
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("context_refreshed"),
		FOsvayderCodeSubsystem::Get().GetConfiguredBackend(),
		TracePayload);
}

FString SOsvayderEditorWidget::GenerateMCPCompactStatusSummary() const
{
	if (!FOsvayderUEModule::IsAvailable())
	{
		return TEXT("module not loaded");
	}

	TSharedPtr<FOsvayderUEMCPServer> MCPServer = FOsvayderUEModule::Get().GetMCPServer();
	if (!MCPServer.IsValid() || !MCPServer->IsRunning())
	{
		return TEXT("server not running");
	}

	TSharedPtr<FMCPToolRegistry> ToolRegistry = MCPServer->GetToolRegistry();
	if (!ToolRegistry.IsValid())
	{
		return TEXT("tool registry not initialized");
	}

	TArray<FMCPToolInfo> RegisteredTools = ToolRegistry->GetAllTools();
	const TArray<FString>& ExpectedTools = OsvayderUEConstants::MCPServer::ExpectedTools;
	if (ExpectedTools.Num() == 0)
	{
		return FString::Printf(TEXT("running, %d tools registered"), RegisteredTools.Num());
	}

	TSet<FString> RegisteredToolNames;
	for (const FMCPToolInfo& Tool : RegisteredTools)
	{
		RegisteredToolNames.Add(Tool.Name);
	}

	int32 AvailableCount = 0;
	for (const FString& ToolName : ExpectedTools)
	{
		if (RegisteredToolNames.Contains(ToolName))
		{
			++AvailableCount;
		}
	}

	const int32 MissingCount = FMath::Max(0, ExpectedTools.Num() - AvailableCount);
	if (MissingCount == 0)
	{
		return FString::Printf(TEXT("running, %d/%d expected tools available"), AvailableCount, ExpectedTools.Num());
	}

	return FString::Printf(TEXT("running, %d/%d expected tools available (%d missing)"), AvailableCount, ExpectedTools.Num(), MissingCount);
}

FString SOsvayderEditorWidget::GenerateMCPStatusMessage() const
{
	FString StatusMessage = TEXT("в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ\n");
	StatusMessage += TEXT("MCP Tool Status:\n");

	// Check module availability first to avoid race conditions during startup
	if (!FOsvayderUEModule::IsAvailable())
	{
		StatusMessage += TEXT("вќЊ MCP Server: MODULE NOT LOADED\n");
		StatusMessage += TEXT("в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ");
		return StatusMessage;
	}

	// Try to get MCP server
	TSharedPtr<FOsvayderUEMCPServer> MCPServer = FOsvayderUEModule::Get().GetMCPServer();

	if (!MCPServer.IsValid() || !MCPServer->IsRunning())
	{
		// MCP server not running
		StatusMessage += TEXT("вќЊ MCP Server: NOT RUNNING\n\n");
		StatusMessage += TEXT("вљ пёЏ MCP tools are unavailable.\n\n");
		StatusMessage += TEXT("Troubleshooting:\n");
		StatusMessage += TEXT("  - Check Output Log for MCP errors\n");
		StatusMessage += TEXT("  - Run: npm install in Resources/mcp-bridge\n");
		StatusMessage += FString::Printf(TEXT("  - Verify port %d is available\n"), OsvayderUEConstants::MCPServer::DefaultPort);
		StatusMessage += TEXT("в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ");
		return StatusMessage;
	}

	// MCP server running - check tools
	TSharedPtr<FMCPToolRegistry> ToolRegistry = MCPServer->GetToolRegistry();
	if (!ToolRegistry.IsValid())
	{
		StatusMessage += TEXT("вќЊ Tool Registry: NOT INITIALIZED\n");
		StatusMessage += TEXT("в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ");
		return StatusMessage;
	}

	// Get registered tools
	TArray<FMCPToolInfo> RegisteredTools = ToolRegistry->GetAllTools();

	// Build set of registered tool names for quick lookup
	TSet<FString> RegisteredToolNames;
	for (const FMCPToolInfo& Tool : RegisteredTools)
	{
		RegisteredToolNames.Add(Tool.Name);
	}

	// Get expected tools from constants
	const TArray<FString>& ExpectedTools = OsvayderUEConstants::MCPServer::ExpectedTools;

	// Check each expected tool - only track missing ones
	int32 AvailableCount = 0;
	TArray<FString> MissingTools;

	for (const FString& ToolName : ExpectedTools)
	{
		if (RegisteredToolNames.Contains(ToolName))
		{
			AvailableCount++;
		}
		else
		{
			MissingTools.Add(ToolName);
		}
	}

	// Summary - only show details if there are issues
	if (MissingTools.Num() == 0)
	{
		StatusMessage += FString::Printf(TEXT("  вњ“ All %d tools operational\n"), AvailableCount);
	}
	else
	{
		StatusMessage += FString::Printf(TEXT("  вњ“ %d/%d tools available\n"), AvailableCount, ExpectedTools.Num());
		StatusMessage += TEXT("\nвљ пёЏ Missing tools:\n");
		for (const FString& ToolName : MissingTools)
		{
			StatusMessage += FString::Printf(TEXT("  вњ— %s\n"), *ToolName);
		}
		StatusMessage += TEXT("\nCheck Output Log for details.\n");
	}

	StatusMessage += TEXT("в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ");

	return StatusMessage;
}

#undef LOCTEXT_NAMESPACE
