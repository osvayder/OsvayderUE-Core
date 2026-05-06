// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUEModule.h"
#include "OsvayderUECommands.h"
#include "OsvayderUEEnginePatchProbe.h" // 628-v3 startup patch-presence probe
#include "OsvayderUETaskRecoveryDetector.h" // 632 task recovery detection probe
#include "OsvayderEditorWidget.h"
#include "OsvayderCodeRunner.h"
#include "OsvayderSubsystem.h"
#include "CodexCliRunner.h"
#include "OsvayderUESettings.h"
#include "OsvayderUESettingsDetails.h"
#include "ScriptExecutionManager.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUECanonRouting.h"
#include "OsvayderUERestartSurvival.h"
#include "MCP/OsvayderUEMCPServer.h"
#include "MCP/Tools/MCPTool_PluginSettings.h"
#include "ProjectContext.h"

#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "LevelEditor.h"
#include "Interfaces/IPluginManager.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Application/SlateApplication.h"
#include "HttpServerModule.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Containers/Ticker.h"
#include "ISettingsModule.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PropertyEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"

DEFINE_LOG_CATEGORY(LogOsvayderUE);

#define LOCTEXT_NAMESPACE "FOsvayderUEModule"

static const FName ClaudeTabName("OsvayderAssistant");

namespace
{
	FString GetOsvayderUEPluginBaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("OsvayderUE"));
		if (Plugin.IsValid())
		{
			return Plugin->GetBaseDir();
		}

		return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("OsvayderUE"));
	}

	FString GetOsvayderUEBinaryPath()
	{
#if PLATFORM_WINDOWS
		return FPaths::Combine(GetOsvayderUEPluginBaseDir(), TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealEditor-OsvayderUE.dll"));
#elif PLATFORM_LINUX
		return FPaths::Combine(GetOsvayderUEPluginBaseDir(), TEXT("Binaries"), TEXT("Linux"), TEXT("libUnrealEditor-OsvayderUE.so"));
#elif PLATFORM_MAC
		return FPaths::Combine(GetOsvayderUEPluginBaseDir(), TEXT("Binaries"), TEXT("Mac"), TEXT("UnrealEditor-OsvayderUE.dylib"));
#else
		return FString();
#endif
	}

	FString GetOsvayderUESourceDir()
	{
		return FPaths::Combine(GetOsvayderUEPluginBaseDir(), TEXT("Source"));
	}

	FString GetWindowsPowerShellExecutablePath()
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

	bool IsTrackedBuildInput(const FString& FilePath)
	{
		const FString Extension = FPaths::GetExtension(FilePath, true).ToLower();
		return Extension == TEXT(".h")
			|| Extension == TEXT(".hpp")
			|| Extension == TEXT(".cpp")
			|| Extension == TEXT(".c")
			|| Extension == TEXT(".cs")
			|| Extension == TEXT(".uplugin");
	}

	void ConsiderBuildInputFile(const FString& FilePath, FString& LatestPath, FDateTime& LatestTimestamp)
	{
		if (!IsTrackedBuildInput(FilePath))
		{
			return;
		}

		const FDateTime FileTimestamp = IFileManager::Get().GetTimeStamp(*FilePath);
		if (FileTimestamp > LatestTimestamp)
		{
			LatestTimestamp = FileTimestamp;
			LatestPath = FilePath;
		}
	}

	FOsvayderUEBuildSyncStatus EvaluateBuildSyncStatus()
	{
		FOsvayderUEBuildSyncStatus Status;
		Status.BinaryPath = GetOsvayderUEBinaryPath();
		Status.bBinaryPresent = !Status.BinaryPath.IsEmpty() && IFileManager::Get().FileExists(*Status.BinaryPath);
		if (Status.bBinaryPresent)
		{
			Status.BinaryTimestamp = IFileManager::Get().GetTimeStamp(*Status.BinaryPath);
		}

		const FString SourceDir = GetOsvayderUESourceDir();
		TArray<FString> SourceFiles;
		IFileManager::Get().FindFilesRecursive(SourceFiles, *SourceDir, TEXT("*.*"), true, false);
		for (const FString& FilePath : SourceFiles)
		{
			ConsiderBuildInputFile(FilePath, Status.LatestSourcePath, Status.LatestSourceTimestamp);
		}

		const FString UPluginPath = FPaths::Combine(GetOsvayderUEPluginBaseDir(), TEXT("OsvayderUE.uplugin"));
		ConsiderBuildInputFile(UPluginPath, Status.LatestSourcePath, Status.LatestSourceTimestamp);

		if (Status.LatestSourcePath.IsEmpty() || Status.LatestSourceTimestamp == FDateTime::MinValue())
		{
			Status.Detail = TEXT("Osvayder UE build sync check could not determine a tracked source timestamp.");
			return Status;
		}

		if (!Status.bBinaryPresent)
		{
			Status.Detail = FString::Printf(
				TEXT("Osvayder UE binary is missing. Latest tracked source is %s (%s). Run the preflight launcher before practical verification."),
				*Status.LatestSourcePath,
				*Status.LatestSourceTimestamp.ToString());
			return Status;
		}

		Status.bFresh = Status.BinaryTimestamp >= Status.LatestSourceTimestamp;
		if (Status.bFresh)
		{
			Status.Detail = FString::Printf(
				TEXT("Osvayder UE build is current. Binary %s (%s) is up to date against tracked source %s (%s)."),
				*Status.BinaryPath,
				*Status.BinaryTimestamp.ToString(),
				*Status.LatestSourcePath,
				*Status.LatestSourceTimestamp.ToString());
		}
		else
		{
			Status.Detail = FString::Printf(
				TEXT("Osvayder UE binary is stale. Binary %s (%s) is older than tracked source %s (%s). Close the editor and run %s before practical verification."),
				*Status.BinaryPath,
				*Status.BinaryTimestamp.ToString(),
				*Status.LatestSourcePath,
				*Status.LatestSourceTimestamp.ToString(),
				*FOsvayderUEModule::GetPreflightLauncherPath());
		}

		return Status;
	}

	void ShowBuildSyncNotification(const FOsvayderUEBuildSyncStatus& Status)
	{
		if (Status.bFresh)
		{
			return;
		}

		FNotificationInfo Info(FText::FromString(Status.Detail));
		Info.ExpireDuration = 12.0f;
		Info.bUseLargeFont = false;
		Info.bUseSuccessFailIcons = true;

		const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
		if (Notification.IsValid())
		{
			Notification->SetCompletionState(SNotificationItem::CS_Fail);
		}
	}

	FString SanitizeBackendRunTag(const FString& InTag)
	{
		FString Sanitized = InTag.TrimStartAndEnd();
		if (Sanitized.IsEmpty())
		{
			Sanitized = TEXT("latest");
		}

		for (TCHAR& Ch : Sanitized)
		{
			if (!(FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-')))
			{
				Ch = TEXT('_');
			}
		}

		return Sanitized;
	}

	FString GetBackendRunOutputPath(const FString& RunTag)
	{
		const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("backend_runs"));
		IFileManager::Get().MakeDirectory(*OutputDir, true);
		return FPaths::Combine(OutputDir, RunTag + TEXT(".json"));
	}

	TSharedPtr<FJsonObject> MakeBackendStatusJson(const FAgentBackendStatus& Status)
	{
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		const FCodexCliRunner::FCodexAuthDiagnostics AuthDiagnostics = FCodexCliRunner::GetAuthDiagnostics();
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("backend"), Status.Backend == EOsvayderUEProviderBackend::CodexCli ? TEXT("CodexCli") : TEXT("ClaudeCli"));
		Json->SetStringField(TEXT("display_name"), Status.DisplayName);
		Json->SetBoolField(TEXT("available"), Status.bAvailable);
		Json->SetBoolField(TEXT("ready"), Status.bReady);
		Json->SetStringField(TEXT("readiness"), AgentBackendReadinessToString(Status.Readiness));
		Json->SetStringField(TEXT("auth_state"), AgentBackendAuthStateToString(Status.AuthState));
		Json->SetStringField(TEXT("detail"), Status.Detail);
		Json->SetStringField(TEXT("auth_detail"), Status.AuthDetail);
		Json->SetStringField(TEXT("executable_path"), Status.ExecutablePath);
		Json->SetBoolField(TEXT("can_execute_prompt"), AgentBackendCanExecutePrompt(Status));
		Json->SetStringField(TEXT("codex_model"), Settings ? Settings->GetConfiguredCodexModel() : FString());
		Json->SetStringField(TEXT("codex_profile"), Settings ? Settings->GetEffectiveCodexProfileLabel() : TEXT("default"));
		Json->SetBoolField(TEXT("codex_profile_is_explicit"), Settings ? Settings->HasExplicitCodexProfile() : false);
		Json->SetStringField(TEXT("codex_home_path"), FCodexCliRunner::GetConfiguredCodexHomePath());
		Json->SetBoolField(TEXT("codex_home_is_explicit"), FCodexCliRunner::HasExplicitCodexHomeOverride());
		Json->SetStringField(TEXT("codex_auth_mode"), FCodexCliRunner::GetConfiguredAuthModeName());
		Json->SetStringField(TEXT("codex_auth_env_var"), FCodexCliRunner::GetConfiguredApiKeyEnvVarName());
		Json->SetStringField(TEXT("codex_auth_effective_path"), FCodexCliRunner::GetEffectiveAuthEntryPath());
		Json->SetStringField(TEXT("codex_auth_ownership_model"), FCodexCliRunner::GetEffectiveAuthOwnershipModel());
		Json->SetBoolField(TEXT("codex_exec_clear_proxy_env"), Settings ? Settings->ShouldClearProxyEnvForCodexExec() : false);
		Json->SetBoolField(TEXT("codex_browser_verify_clear_proxy_env"), Settings ? Settings->ShouldClearProxyEnvForBrowserVerify() : false);
		Json->SetBoolField(TEXT("codex_persistent_conversation_transport"), FCodexCliRunner::ShouldUsePersistentConversationTransport());
		Json->SetStringField(TEXT("codex_execution_transport"), FCodexCliRunner::ShouldUsePersistentConversationTransport()
			? TEXT("persistent_app_server")
			: TEXT("exec_per_message"));
		FString BrowserVerifyHint;
		Json->SetBoolField(TEXT("codex_browser_verify_can_launch"), FCodexCliRunner::CanLaunchBrowserVerify(BrowserVerifyHint));
		Json->SetStringField(TEXT("codex_browser_verify_hint"), BrowserVerifyHint);
		Json->SetStringField(TEXT("codex_auth_diagnostic_state"), AuthDiagnostics.AuthState);
		Json->SetStringField(TEXT("codex_auth_configured_home"), AuthDiagnostics.ConfiguredCodexHomePath);
		Json->SetStringField(TEXT("codex_auth_effective_home"), AuthDiagnostics.EffectiveCodexHomePath);
		Json->SetStringField(TEXT("codex_auth_home_resolution_source"), AuthDiagnostics.CodexHomeResolutionSource);
		Json->SetStringField(TEXT("codex_auth_credential_artifact"), AuthDiagnostics.CredentialArtifactPath);
		Json->SetStringField(TEXT("codex_auth_diagnostic_detail"), AuthDiagnostics.AuthDetailText);
		return Json;
	}

	TSharedPtr<FJsonObject> MakeCodexAuthDiagnosticsJson(const FCodexCliRunner::FCodexAuthDiagnostics& Diagnostics)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("configured_codex_home"), Diagnostics.ConfiguredCodexHomePath);
		Json->SetStringField(TEXT("effective_codex_home"), Diagnostics.EffectiveCodexHomePath);
		Json->SetStringField(TEXT("home_resolution_source"), Diagnostics.CodexHomeResolutionSource);
		Json->SetStringField(TEXT("credential_artifact_path"), Diagnostics.CredentialArtifactPath);
		Json->SetStringField(TEXT("auth_state"), Diagnostics.AuthState);
		Json->SetStringField(TEXT("auth_detail"), Diagnostics.AuthDetailText);
		Json->SetStringField(TEXT("auth_mode"), Diagnostics.AuthMode);
		Json->SetStringField(TEXT("effective_auth_path"), Diagnostics.EffectiveAuthEntryPath);
		Json->SetStringField(TEXT("ownership"), Diagnostics.AuthOwnershipModel);
		Json->SetStringField(TEXT("profile"), Diagnostics.ProfileLabel);
		Json->SetStringField(TEXT("model"), Diagnostics.ModelName);
		Json->SetStringField(TEXT("speed_requested"), Diagnostics.RequestedSpeedMode);
		Json->SetStringField(TEXT("speed_effective"), Diagnostics.EffectiveSpeedMode);
		Json->SetStringField(TEXT("speed_support"), Diagnostics.SpeedSupportLabel);
		Json->SetStringField(TEXT("work_mode"), Diagnostics.WorkModeName);
		Json->SetStringField(TEXT("reasoning_effort"), Diagnostics.ReasoningEffortName);
		Json->SetStringField(TEXT("verbosity"), Diagnostics.VerbosityName);
		Json->SetStringField(TEXT("executable_path"), Diagnostics.ExecutablePath);
		Json->SetBoolField(TEXT("executable_available"), Diagnostics.bExecutableAvailable);
		Json->SetBoolField(TEXT("credential_artifact_present"), Diagnostics.bCredentialArtifactPresent);
		Json->SetBoolField(TEXT("persistent_app_server_enabled"), Diagnostics.bPersistentAppServerEnabled);
		Json->SetBoolField(TEXT("probe_performed"), Diagnostics.bProbePerformed);
		Json->SetStringField(TEXT("probe_detail"), Diagnostics.ProbeDetailText);
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> MakeJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}

		return JsonValues;
	}

	TSharedPtr<FJsonObject> BuildInitialCanonExecutionJson(
		const FString& Prompt,
		const EAgentExecutionRunProfile ExecutionProfile)
	{
		FAgentRequestConfig Config;
		Config.ExecutionProfile = ExecutionProfile;
		Config.CanonExecution =
			OsvayderUECanonRouting::BuildInitialCanonExecution(Prompt, ExecutionProfile, Config.PromptContract.ContextBlocks);
		OsvayderUECanonRouting::ApplyRuntimeToolExposure(Config);
		return OsvayderUECanonRouting::MakeCanonExecutionJson(Config.CanonExecution);
	}

	bool PromoteCanonEntryFromProvenRun(
		const FString& ProvenRunPath,
		const FString& PatternKey,
		const FString& ExpectedSubsystem,
		const FString& ShortTitle,
		const FString& ChosenPathSummary,
		const FString& WhyPreferred,
		const FString& BadPathToAvoid,
		FString& OutError)
	{
		FOsvayderUECanonPromotionRequest Request;
		Request.ProvenRunReceiptPath = ProvenRunPath;
		Request.PatternKey = PatternKey;
		Request.ExpectedSubsystem = ExpectedSubsystem;
		Request.ShortTitle = ShortTitle;
		Request.ChosenPathSummary = ChosenPathSummary;
		Request.WhyPreferred = WhyPreferred;
		Request.BadPathToAvoid = BadPathToAvoid;
		Request.LastConfirmedEngineContext = TEXT("UE5.7");
		return FOsvayderUECanonLedger::PromoteFromProvenRun(Request, OutError, nullptr);
	}

	bool SeedCanonLedgerV3Proof(FString& OutError, TArray<FString>& OutPatternKeys)
	{
		OutError.Reset();
		OutPatternKeys.Reset();

		const FString BackendRunsDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("backend_runs"));
		if (!PromoteCanonEntryFromProvenRun(
			FPaths::Combine(BackendRunsDir, TEXT("p16_canon_input_live_v5.json")),
			TEXT("input.sprint_binding"),
			TEXT("input"),
			TEXT("Sprint binding through Enhanced Input"),
			TEXT("Use native Enhanced Input assets and mappings before any fallback lane."),
			TEXT("This project already routes player bindings through Enhanced Input assets."),
			TEXT("Do not start by patching workspace files or UI-driving for input asset work."),
			OutError))
		{
			return false;
		}
		OutPatternKeys.Add(TEXT("input.sprint_binding"));

		if (!PromoteCanonEntryFromProvenRun(
			FPaths::Combine(BackendRunsDir, TEXT("p16_canon_animation_live_v3.json")),
			TEXT("animation.state_machine_creation"),
			TEXT("animation"),
			TEXT("Animation state machine creation through AnimBP tooling"),
			TEXT("Use native animation blueprint mutation before generic blueprint or shell fallback."),
			TEXT("The accepted animation proof fixture already validates state-machine creation on the native animation path."),
			TEXT("Do not start with workspace shell edits for AnimBP graph mutation."),
			OutError))
		{
			return false;
		}
		OutPatternKeys.Add(TEXT("animation.state_machine_creation"));
		return true;
	}

	TSharedPtr<FJsonObject> MakeSessionFileObservationJson(const FString& SessionPath)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		const bool bExists = !SessionPath.IsEmpty() && IFileManager::Get().FileExists(*SessionPath);
		const int64 FileSize = bExists ? IFileManager::Get().FileSize(*SessionPath) : -1;
		const FDateTime Timestamp = bExists ? IFileManager::Get().GetTimeStamp(*SessionPath) : FDateTime::MinValue();
		const FString TimestampIso = Timestamp == FDateTime::MinValue() ? FString() : Timestamp.ToIso8601();
		const FString StateSignature = bExists
			? FString::Printf(TEXT("%lld|%s"), FileSize, *TimestampIso)
			: TEXT("missing");

		Json->SetStringField(TEXT("path"), SessionPath);
		Json->SetBoolField(TEXT("exists"), bExists);
		Json->SetNumberField(TEXT("size_bytes"), FileSize >= 0 ? static_cast<double>(FileSize) : -1.0);
		Json->SetStringField(TEXT("timestamp_utc"), TimestampIso);
		Json->SetStringField(TEXT("state_signature"), StateSignature);
		Json->SetStringField(TEXT("comparison_basis"), TEXT("file_exists_plus_size_plus_timestamp"));
		return Json;
	}

	TSharedPtr<FJsonObject> GetObjectFieldOrEmpty(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return MakeShared<FJsonObject>();
		}

		const TSharedPtr<FJsonObject>* NestedObject = nullptr;
		if (!Object->TryGetObjectField(FieldName, NestedObject) || !NestedObject || !(*NestedObject).IsValid())
		{
			return MakeShared<FJsonObject>();
		}

		return *NestedObject;
	}

	TArray<FString> CollectTraceEventTypesForRun(const FString& RunId)
	{
		FAgentTraceQueryOptions QueryOptions;
		QueryOptions.RunId = RunId;
		QueryOptions.bLatestOnly = false;
		QueryOptions.Count = 24;

		FString ResolvedRunId;
		int32 TotalLoaded = 0;
		const TArray<TSharedPtr<FJsonObject>> Records =
			FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);

		TArray<FString> EventTypes;
		for (const TSharedPtr<FJsonObject>& Record : Records)
		{
			if (!Record.IsValid())
			{
				continue;
			}

			FString EventType;
			if (Record->TryGetStringField(TEXT("event_type"), EventType))
			{
				EventTypes.Add(EventType);
			}
		}

		return EventTypes;
	}

	TArray<TSharedPtr<FJsonObject>> QueryLatestTraceRecordsForBackend(const EOsvayderUEProviderBackend Backend, FString& OutResolvedRunId)
	{
		FAgentTraceQueryOptions QueryOptions;
		QueryOptions.bLatestOnly = true;
		QueryOptions.Count = 24;
		QueryOptions.Backend = Backend == EOsvayderUEProviderBackend::CodexCli
			? TEXT("CodexCli")
			: TEXT("ClaudeCli");

		int32 TotalLoaded = 0;
		return FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, OutResolvedRunId, TotalLoaded);
	}

	TArray<FString> CollectTraceEventTypes(const TArray<TSharedPtr<FJsonObject>>& Records)
	{
		TArray<FString> EventTypes;
		for (const TSharedPtr<FJsonObject>& Record : Records)
		{
			if (!Record.IsValid())
			{
				continue;
			}

			FString EventType;
			if (Record->TryGetStringField(TEXT("event_type"), EventType))
			{
				EventTypes.Add(EventType);
			}
		}

		return EventTypes;
	}

	TSharedPtr<FJsonObject> FindTracePayloadByEventType(const TArray<TSharedPtr<FJsonObject>>& Records, const FString& EventType)
	{
		for (const TSharedPtr<FJsonObject>& Record : Records)
		{
			if (!Record.IsValid())
			{
				continue;
			}

			FString RecordEventType;
			if (!Record->TryGetStringField(TEXT("event_type"), RecordEventType) ||
				!RecordEventType.Equals(EventType, ESearchCase::CaseSensitive))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* PayloadObject = nullptr;
			if (Record->TryGetObjectField(TEXT("payload"), PayloadObject) && PayloadObject && (*PayloadObject).IsValid())
			{
				return *PayloadObject;
			}
		}

		return nullptr;
	}

	FAgentRequestConfig BuildReadOnlyDirectFilePolicyProbeConfig(const EOsvayderUEProviderBackend Backend)
	{
		FAgentRequestConfig Config;
		Config.WorkingDirectory = FPaths::ProjectDir();
		Config.ExecutionProfile = EAgentExecutionRunProfile::ReadOnlyDiagnostic;
		Config.ExecutionControlProfileId = TEXT("read_only_diagnostic_v1");
		Config.ExecutionTransportLabel = Backend == EOsvayderUEProviderBackend::CodexCli
			? TEXT("exec_per_message")
			: TEXT("cli_process");
		Config.DesiredFutureDefaultProviderPowerClass = EAgentExecutionPowerClass::BoundedMutationCapable;
		Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
		Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
		Config.bSkipPermissions = false;
		Config.bExecutionProfileEnforcedNow = true;
		Config.bToolAllowListEnforced = Backend == EOsvayderUEProviderBackend::ClaudeCli;
		Config.bEnableUnrealMcpBridge = false;
		Config.bForceDisablePersistentConversationTransport = Backend == EOsvayderUEProviderBackend::CodexCli;
		Config.AllowedTools = {
			TEXT("Read"),
			TEXT("Grep"),
			TEXT("Glob"),
			TEXT("Write")
		};
		return Config;
	}

	void WriteBackendRunJson(
		const FString& RunTag,
		const FString& Prompt,
		const FString& State,
		const FAgentBackendStatus& Status,
		const FAgentProviderExecutionControlManifest* ExecutionControlManifest,
		bool bHasResult,
		bool bSuccess,
		const FString& Response,
		const FString& ErrorText,
		const FString& TraceRunId = FString(),
		const TArray<FString>* TraceEventTypes = nullptr,
		TSharedPtr<FJsonObject> PolicyDeniedPayload = nullptr,
		TSharedPtr<FJsonObject> SessionBoundaryProof = nullptr,
		TSharedPtr<FJsonObject> CanonExecutionPayload = nullptr)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("run_tag"), RunTag);
		Root->SetStringField(TEXT("state"), State);
		Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("prompt"), Prompt);
		Root->SetObjectField(TEXT("backend_status"), MakeBackendStatusJson(Status));
		if (ExecutionControlManifest)
		{
			Root->SetStringField(TEXT("execution_profile"), ExecutionControlManifest->ExecutionProfile);
			Root->SetStringField(TEXT("execution_control_profile_id"), ExecutionControlManifest->ControlProfileId);
			Root->SetStringField(TEXT("execution_transport"), ExecutionControlManifest->ExecutionTransportLabel);
			Root->SetStringField(TEXT("session_persistence_mode"), AgentSessionPersistenceModeToString(ExecutionControlManifest->SessionPersistenceMode));
			Root->SetStringField(TEXT("current_effective_provider_power"), AgentExecutionPowerClassToString(ExecutionControlManifest->CurrentEffectiveProviderPowerClass));
			Root->SetStringField(TEXT("desired_future_default_provider_power"), AgentExecutionPowerClassToString(ExecutionControlManifest->DesiredFutureDefaultProviderPowerClass));
			Root->SetStringField(TEXT("execution_control_plumbing_state"), AgentExecutionGovernanceStateToString(ExecutionControlManifest->ExecutionControlPlumbingState));
			Root->SetBoolField(TEXT("normal_provider_session_history_touched"), ExecutionControlManifest->bTouchesNormalProviderSessionHistory);
			Root->SetBoolField(TEXT("provider_session_file_updated_on_success"), ExecutionControlManifest->bWritesProviderSessionFileOnSuccess);
			Root->SetObjectField(TEXT("provider_execution_control"), MakeAgentProviderExecutionControlJson(*ExecutionControlManifest));
		}

		if (bHasResult)
		{
			Root->SetBoolField(TEXT("success"), bSuccess);
			Root->SetStringField(TEXT("response"), Response);
			Root->SetStringField(TEXT("error"), ErrorText);
		}

		if (!TraceRunId.IsEmpty() || (TraceEventTypes && TraceEventTypes->Num() > 0))
		{
			TSharedPtr<FJsonObject> TraceObject = MakeShared<FJsonObject>();
			TraceObject->SetStringField(TEXT("run_id"), TraceRunId);
			TraceObject->SetStringField(TEXT("resolution_strategy"), TEXT("latest_only_backend_filter_after_completion"));
			TraceObject->SetBoolField(TEXT("policy_denied_visible"), PolicyDeniedPayload.IsValid());
			if (TraceEventTypes)
			{
				TraceObject->SetArrayField(TEXT("event_types"), MakeJsonStringArray(*TraceEventTypes));
			}
			Root->SetObjectField(TEXT("trace"), TraceObject);
		}

		if (PolicyDeniedPayload.IsValid())
		{
			Root->SetObjectField(TEXT("policy_denied_contract"), PolicyDeniedPayload);
		}

		if (SessionBoundaryProof.IsValid())
		{
			Root->SetObjectField(TEXT("session_boundary_proof"), SessionBoundaryProof);
		}
		if (CanonExecutionPayload.IsValid())
		{
			Root->SetObjectField(TEXT("canon_execution"), CanonExecutionPayload);
			const TSharedPtr<FJsonObject>* FeatureWorkflowObject = nullptr;
			if (CanonExecutionPayload->TryGetObjectField(TEXT("feature_workflow"), FeatureWorkflowObject)
				&& FeatureWorkflowObject != nullptr
				&& (*FeatureWorkflowObject).IsValid())
			{
				Root->SetObjectField(TEXT("feature_workflow"), *FeatureWorkflowObject);
			}
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		FFileHelper::SaveStringToFile(JsonText, *GetBackendRunOutputPath(RunTag), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	void StartConfiguredBackendPromptRun(
		const FString& CommandName,
		const FString& RunTag,
		const FString& Prompt,
		const FOsvayderPromptOptions& Options)
	{
		const FAgentBackendStatus InitialStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
		const FAgentProviderExecutionControlManifest InitialManifest =
			FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(Options.ExecutionProfile);
		const FString ActiveProfileSessionPath = FOsvayderCodeSubsystem::Get().GetSessionFilePathForProfile(Options.ExecutionProfile);
		const FString NormalProviderSessionPath =
			FOsvayderCodeSubsystem::Get().GetSessionFilePathForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);
		const TSharedPtr<FJsonObject> ActiveProfileSessionBefore = MakeSessionFileObservationJson(ActiveProfileSessionPath);
		const TSharedPtr<FJsonObject> NormalProviderSessionBefore = MakeSessionFileObservationJson(NormalProviderSessionPath);
		const TSharedPtr<FJsonObject> InitialCanonExecutionPayload =
			BuildInitialCanonExecutionJson(Prompt, Options.ExecutionProfile);

		WriteBackendRunJson(
			RunTag,
			Prompt,
			TEXT("pending"),
			InitialStatus,
			&InitialManifest,
			false,
			false,
			FString(),
			FString(),
			FString(),
			nullptr,
			nullptr,
			nullptr,
			InitialCanonExecutionPayload);

		if (!AgentBackendCanExecutePrompt(InitialStatus))
		{
			WriteBackendRunJson(
				RunTag,
				Prompt,
				TEXT("rejected"),
				InitialStatus,
				&InitialManifest,
				true,
				false,
				FString(),
				InitialStatus.Detail,
				FString(),
				nullptr,
				nullptr,
				nullptr,
				InitialCanonExecutionPayload);
			UE_LOG(LogOsvayderUE, Warning, TEXT("%s rejected: %s"), *CommandName, *InitialStatus.Detail);
			return;
		}

		FOsvayderCodeSubsystem::Get().SendPrompt(
			Prompt,
			FOnOsvayderResponse::CreateLambda([
				CommandName,
				RunTag,
				Prompt,
				ExecutionProfile = Options.ExecutionProfile,
				InitialCanonExecutionPayload,
				ActiveProfileSessionBefore,
				NormalProviderSessionBefore,
				ActiveProfileSessionPath,
				NormalProviderSessionPath](const FString& Response, bool bSuccess)
			{
				const FAgentBackendStatus FinalStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
				const FAgentProviderExecutionControlManifest FinalManifest =
					FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(ExecutionProfile);
				FString TraceRunId;
				const TArray<TSharedPtr<FJsonObject>> TraceRecords = QueryLatestTraceRecordsForBackend(FinalStatus.Backend, TraceRunId);
				const TArray<FString> TraceEventTypes = CollectTraceEventTypes(TraceRecords);
				const TSharedPtr<FJsonObject> PolicyDeniedPayload = FindTracePayloadByEventType(TraceRecords, TEXT("policy_denied"));
				const TSharedPtr<FJsonObject> CanonExecutionPayload =
					OsvayderUECanonRouting::ExtractCanonExecutionFromTraceRecords(TraceRecords);
				const TSharedPtr<FJsonObject> ActiveProfileSessionAfter = MakeSessionFileObservationJson(ActiveProfileSessionPath);
				const TSharedPtr<FJsonObject> NormalProviderSessionAfter = MakeSessionFileObservationJson(NormalProviderSessionPath);
				FString ActiveBeforeSignature;
				FString ActiveAfterSignature;
				FString NormalBeforeSignature;
				FString NormalAfterSignature;
				ActiveProfileSessionBefore->TryGetStringField(TEXT("state_signature"), ActiveBeforeSignature);
				ActiveProfileSessionAfter->TryGetStringField(TEXT("state_signature"), ActiveAfterSignature);
				NormalProviderSessionBefore->TryGetStringField(TEXT("state_signature"), NormalBeforeSignature);
				NormalProviderSessionAfter->TryGetStringField(TEXT("state_signature"), NormalAfterSignature);

				TSharedPtr<FJsonObject> SessionBoundaryProof = MakeShared<FJsonObject>();
				SessionBoundaryProof->SetObjectField(TEXT("active_profile_session_before"), ActiveProfileSessionBefore);
				SessionBoundaryProof->SetObjectField(TEXT("active_profile_session_after"), ActiveProfileSessionAfter);
				SessionBoundaryProof->SetBoolField(TEXT("active_profile_session_state_unchanged"), ActiveBeforeSignature == ActiveAfterSignature);
				SessionBoundaryProof->SetObjectField(TEXT("normal_provider_session_before"), NormalProviderSessionBefore);
				SessionBoundaryProof->SetObjectField(TEXT("normal_provider_session_after"), NormalProviderSessionAfter);
				SessionBoundaryProof->SetBoolField(TEXT("normal_provider_session_state_unchanged"), NormalBeforeSignature == NormalAfterSignature);
				SessionBoundaryProof->SetStringField(TEXT("boundary_observation"), TEXT("active_profile_vs_explicit_expert_session_boundary_check"));

				const FString FinalState = bSuccess
					? TEXT("completed")
					: (PolicyDeniedPayload.IsValid() ? TEXT("policy_denied") : TEXT("failed"));
				WriteBackendRunJson(
					RunTag,
					Prompt,
					FinalState,
					FinalStatus,
					&FinalManifest,
					true,
					bSuccess,
					bSuccess ? Response : FString(),
					bSuccess ? FString() : Response,
					TraceRunId,
					&TraceEventTypes,
					PolicyDeniedPayload,
					SessionBoundaryProof,
					CanonExecutionPayload.IsValid() ? CanonExecutionPayload : InitialCanonExecutionPayload);

				if (bSuccess)
				{
					UE_LOG(LogOsvayderUE, Log, TEXT("%s %s -> completed"), *CommandName, *RunTag);
				}
				else if (PolicyDeniedPayload.IsValid())
				{
					UE_LOG(LogOsvayderUE, Warning, TEXT("%s %s -> policy_denied"), *CommandName, *RunTag);
				}
				else
				{
					UE_LOG(LogOsvayderUE, Error, TEXT("%s %s -> failed"), *CommandName, *RunTag);
				}
			}),
			Options);

		UE_LOG(LogOsvayderUE, Log, TEXT("%s started: %s"), *CommandName, *GetBackendRunOutputPath(RunTag));
	}

	void RunExecutionControlSlice1Proof(const FString& RunTag)
	{
		const FString Prompt = TEXT("Post-ULTRA HC1/HC2 slice 1 policy deny and session boundary proof");
		const FAgentBackendStatus Status = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
		const FString SessionPath =
			FOsvayderCodeSubsystem::Get().GetSessionFilePathForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);
		const TSharedPtr<FJsonObject> SessionBefore = MakeSessionFileObservationJson(SessionPath);

		FMCPTool_PluginSettings PluginSettingsTool;
		const FMCPToolResult PluginSettingsResult = PluginSettingsTool.Execute(MakeShared<FJsonObject>());
		const TSharedPtr<FJsonObject> SettingsData = PluginSettingsResult.Data.IsValid()
			? PluginSettingsResult.Data
			: MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject> AssistantBackendObject = GetObjectFieldOrEmpty(SettingsData, TEXT("assistant_backend"));
		const TSharedPtr<FJsonObject> ProviderExecutionControlObject = GetObjectFieldOrEmpty(AssistantBackendObject, TEXT("provider_execution_control"));
		const TSharedPtr<FJsonObject> ExecutionProfilesObject = GetObjectFieldOrEmpty(AssistantBackendObject, TEXT("execution_profiles"));
		const TSharedPtr<FJsonObject> ReadOnlyExecutionProfileObject = GetObjectFieldOrEmpty(ExecutionProfilesObject, TEXT("read_only_diagnostic"));

		const FAgentRequestConfig ProbeConfig = BuildReadOnlyDirectFilePolicyProbeConfig(Status.Backend);
		FAgentExecutionPolicyDenyContract DenyContract;
		const bool bDenyBuilt = TryBuildAgentExecutionPolicyDenyContract(Status, ProbeConfig, TEXT("direct_file_tools"), DenyContract);

		FString TraceRunId;
		TArray<FString> TraceEventTypes;
		if (bDenyBuilt)
		{
			TraceRunId = FOsvayderUEAgentTraceLog::Get().BeginRun(Status, ProbeConfig, Prompt, false, false);
			TSharedPtr<FJsonObject> DenyPayload = MakeAgentExecutionPolicyDenyContractJson(DenyContract);
			DenyPayload->SetStringField(TEXT("probe_id"), TEXT("read_only_direct_file_tools"));
			FOsvayderUEAgentTraceLog::Get().AppendEvent(TEXT("policy_denied"), Status.Backend, DenyPayload, TraceRunId);
			FOsvayderUEAgentTraceLog::Get().CompleteRun(TraceRunId, Status.Backend, DenyContract.DenyReason, false);
			TraceEventTypes = CollectTraceEventTypesForRun(TraceRunId);
		}

		const TSharedPtr<FJsonObject> SessionAfter = MakeSessionFileObservationJson(SessionPath);
		FString BeforeSignature;
		FString AfterSignature;
		SessionBefore->TryGetStringField(TEXT("state_signature"), BeforeSignature);
		SessionAfter->TryGetStringField(TEXT("state_signature"), AfterSignature);
		const bool bNormalSessionStateUnchanged = BeforeSignature == AfterSignature;

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("run_tag"), RunTag);
		Root->SetStringField(TEXT("state"), bDenyBuilt ? TEXT("policy_denied") : TEXT("probe_failed"));
		Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("command"), TEXT("OsvayderUE.RunExecutionControlSlice1Proof"));
		Root->SetStringField(TEXT("prompt"), Prompt);
		Root->SetObjectField(TEXT("backend_status"), MakeBackendStatusJson(Status));
		Root->SetBoolField(TEXT("plugin_settings_success"), PluginSettingsResult.bSuccess);
		Root->SetObjectField(TEXT("provider_execution_control_readback"), ProviderExecutionControlObject);
		Root->SetObjectField(TEXT("read_only_execution_profile_readback"), ReadOnlyExecutionProfileObject);
		Root->SetBoolField(TEXT("proof_a_taxonomy_present"), ProviderExecutionControlObject->HasField(TEXT("runtime_lane_taxonomy")));
		Root->SetBoolField(TEXT("proof_b_matrix_present"), ProviderExecutionControlObject->HasField(TEXT("provider_transport_matrix")));
		Root->SetBoolField(TEXT("proof_c_deny_contract_present"), bDenyBuilt);
		Root->SetBoolField(TEXT("proof_d_session_boundary_present"), ProviderExecutionControlObject->HasField(TEXT("session_boundary")));
		if (bDenyBuilt)
		{
			Root->SetObjectField(TEXT("deny_contract"), MakeAgentExecutionPolicyDenyContractJson(DenyContract));
		}

		TSharedPtr<FJsonObject> TraceObject = MakeShared<FJsonObject>();
		TraceObject->SetStringField(TEXT("run_id"), TraceRunId);
		TraceObject->SetBoolField(TEXT("policy_denied_visible"), TraceEventTypes.Contains(TEXT("policy_denied")));
		TraceObject->SetArrayField(TEXT("event_types"), MakeJsonStringArray(TraceEventTypes));
		Root->SetObjectField(TEXT("trace"), TraceObject);

		TSharedPtr<FJsonObject> SessionBoundaryObject = MakeShared<FJsonObject>();
		SessionBoundaryObject->SetObjectField(TEXT("normal_provider_session_before"), SessionBefore);
		SessionBoundaryObject->SetObjectField(TEXT("normal_provider_session_after"), SessionAfter);
		SessionBoundaryObject->SetBoolField(TEXT("normal_provider_session_state_unchanged"), bNormalSessionStateUnchanged);
		SessionBoundaryObject->SetStringField(TEXT("boundary_observation"), TEXT("read_only_policy_probe_did_not_write_to_normal_provider_session_store"));
		Root->SetObjectField(TEXT("session_boundary_proof"), SessionBoundaryObject);

		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		FFileHelper::SaveStringToFile(JsonText, *GetBackendRunOutputPath(RunTag), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		if (bDenyBuilt && PluginSettingsResult.bSuccess)
		{
			UE_LOG(LogOsvayderUE, Log, TEXT("POSTULTRA_HC12_SLICE1_OK -> %s"), *GetBackendRunOutputPath(RunTag));
		}
		else
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("Post-ULTRA HC1/HC2 slice 1 proof incomplete -> %s"), *GetBackendRunOutputPath(RunTag));
		}
	}

	void RunExecutionControlSlice2Proof(const FString& RunTag)
	{
		const FString Prompt = TEXT("Post-ULTRA HC1/HC2 slice 2 bounded runtime policy deny proof");

		FOsvayderPromptOptions Options;
		Options.bIncludeEngineContext = false;
		Options.bIncludeProjectContext = false;
		Options.ExecutionProfile = EAgentExecutionRunProfile::BoundedPluginMutation;

		StartConfiguredBackendPromptRun(TEXT("OsvayderUE.RunExecutionControlSlice2Proof"), RunTag, Prompt, Options);
	}

	FString JoinPromptArgs(const TArray<FString>& Args, int32 StartIndex)
	{
		FString Prompt;
		for (int32 Index = StartIndex; Index < Args.Num(); ++Index)
		{
			if (!Prompt.IsEmpty())
			{
				Prompt += TEXT(" ");
			}
			Prompt += Args[Index];
		}
		return Prompt;
	}

	bool IsFlagArg(const FString& Arg, const TCHAR* FlagName)
	{
		return Arg.Equals(FlagName, ESearchCase::IgnoreCase);
	}

	FString GetDefaultHeadlessAcceptanceOutputDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("HeadlessAcceptance"));
	}

	bool TryParseHeadlessAcceptanceArgs(
		const TArray<FString>& Args,
		FOsvayderUEHeadlessAcceptanceRequest& OutRequest,
		FString& OutError)
	{
		OutRequest = FOsvayderUEHeadlessAcceptanceRequest{};
		OutError.Reset();
		if (Args.Num() < 2)
		{
			OutError = TEXT("Usage: OsvayderUE.RunHeadlessNewSessionAcceptance <PromptFile> <Prefix> [TimeoutSec] [OutputDir] --allow-local-dev [--quit-on-complete]");
			return false;
		}

		OutRequest.PromptFile = SOsvayderEditorWidget::NormalizeHeadlessAcceptancePathArgument(Args[0]);
		OutRequest.Prefix = Args[1];
		OutRequest.TimeoutSec = 900;
		int32 ArgIndex = 2;
		if (Args.IsValidIndex(ArgIndex) && !Args[ArgIndex].StartsWith(TEXT("--")))
		{
			OutRequest.TimeoutSec = FCString::Atoi(*Args[ArgIndex]);
			++ArgIndex;
		}
		if (Args.IsValidIndex(ArgIndex) && !Args[ArgIndex].StartsWith(TEXT("--")))
		{
			OutRequest.OutputDir = SOsvayderEditorWidget::NormalizeHeadlessAcceptancePathArgument(Args[ArgIndex]);
			++ArgIndex;
		}

		OutRequest.bLocalDevOptIn =
			FParse::Param(FCommandLine::Get(), TEXT("OsvayderUEAllowHeadlessAcceptanceBridge"));
		for (; ArgIndex < Args.Num(); ++ArgIndex)
		{
			if (IsFlagArg(Args[ArgIndex], TEXT("--allow-local-dev")))
			{
				OutRequest.bLocalDevOptIn = true;
			}
			else if (IsFlagArg(Args[ArgIndex], TEXT("--quit-on-complete")))
			{
				OutRequest.bRequestEditorExitOnComplete = true;
			}
			else
			{
				OutError = FString::Printf(TEXT("unknown_headless_acceptance_arg: %s"), *Args[ArgIndex]);
				return false;
			}
		}

		if (OutRequest.OutputDir.IsEmpty())
		{
			OutRequest.OutputDir = GetDefaultHeadlessAcceptanceOutputDir();
		}
		SOsvayderEditorWidget::NormalizeHeadlessAcceptanceRequestPaths(OutRequest);
		return true;
	}

	bool TryParseHeadlessAcceptanceCommandLineRequest(
		FOsvayderUEHeadlessAcceptanceRequest& OutRequest,
		FString& OutError)
	{
		OutRequest = FOsvayderUEHeadlessAcceptanceRequest{};
		OutError.Reset();

		const TCHAR* CommandLine = FCommandLine::Get();
		const bool bVisibleManualRequest =
			FParse::Param(CommandLine, TEXT("OsvayderUERunVisibleManualNewSessionAcceptance"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUERunVisibleManualNewSessionAcceptance"));
		const bool bHeadlessRequest =
			FParse::Param(CommandLine, TEXT("OsvayderUERunHeadlessNewSessionAcceptance"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUERunHeadlessNewSessionAcceptance"));
		if (!bVisibleManualRequest && !bHeadlessRequest)
		{
			return false;
		}

		OutRequest.bVisibleManualEmulator = bVisibleManualRequest;
		OutRequest.bRequireVisibleEditor = bVisibleManualRequest;
		OutRequest.TriggerPath = bVisibleManualRequest
			? TEXT("command_line_visible_manual_acceptance")
			: TEXT("command_line_headless_acceptance");

		const bool bPromptFileFound = bVisibleManualRequest
			? FParse::Value(CommandLine, TEXT("OsvayderUEVisibleManualAcceptancePromptFile="), OutRequest.PromptFile)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEVisibleManualAcceptancePromptFile="), OutRequest.PromptFile)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptancePromptFile="), OutRequest.PromptFile)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptancePromptFile="), OutRequest.PromptFile)
			: FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptancePromptFile="), OutRequest.PromptFile)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptancePromptFile="), OutRequest.PromptFile);
		if (!bPromptFileFound)
		{
			OutError = TEXT("missing_command_line_prompt_file");
			return true;
		}
		const bool bPrefixFound = bVisibleManualRequest
			? FParse::Value(CommandLine, TEXT("OsvayderUEVisibleManualAcceptancePrefix="), OutRequest.Prefix)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEVisibleManualAcceptancePrefix="), OutRequest.Prefix)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptancePrefix="), OutRequest.Prefix)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptancePrefix="), OutRequest.Prefix)
			: FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptancePrefix="), OutRequest.Prefix)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptancePrefix="), OutRequest.Prefix);
		if (!bPrefixFound)
		{
			OutError = TEXT("missing_command_line_prefix");
			return true;
		}

		OutRequest.TimeoutSec = 900;
		const bool bTimeoutFound = bVisibleManualRequest
			? FParse::Value(CommandLine, TEXT("OsvayderUEVisibleManualAcceptanceTimeoutSec="), OutRequest.TimeoutSec)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEVisibleManualAcceptanceTimeoutSec="), OutRequest.TimeoutSec)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceTimeoutSec="), OutRequest.TimeoutSec)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceTimeoutSec="), OutRequest.TimeoutSec)
			: FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceTimeoutSec="), OutRequest.TimeoutSec)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceTimeoutSec="), OutRequest.TimeoutSec);
		if (!bTimeoutFound && bVisibleManualRequest)
		{
			FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceTimeoutSec="), OutRequest.TimeoutSec);
		}
		const bool bOutputDirFound = bVisibleManualRequest
			? FParse::Value(CommandLine, TEXT("OsvayderUEVisibleManualAcceptanceOutputDir="), OutRequest.OutputDir)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEVisibleManualAcceptanceOutputDir="), OutRequest.OutputDir)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceOutputDir="), OutRequest.OutputDir)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceOutputDir="), OutRequest.OutputDir)
			: FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceOutputDir="), OutRequest.OutputDir)
				|| FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceOutputDir="), OutRequest.OutputDir);
		if (!bOutputDirFound && bVisibleManualRequest)
		{
			FParse::Value(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceOutputDir="), OutRequest.OutputDir);
		}
		if (OutRequest.OutputDir.IsEmpty())
		{
			OutRequest.OutputDir = GetDefaultHeadlessAcceptanceOutputDir();
		}
		OutRequest.bLocalDevOptIn =
			FParse::Param(CommandLine, TEXT("OsvayderUEAllowHeadlessAcceptanceBridge"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUEAllowHeadlessAcceptanceBridge"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceAllowLocalDev"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUEVisibleManualAcceptanceAllowLocalDev"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceAllowLocalDev"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUEVisibleManualAcceptanceAllowLocalDev"));
		OutRequest.bRequestEditorExitOnComplete =
			FParse::Param(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceQuitOnComplete"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUEVisibleManualAcceptanceQuitOnComplete"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUEHeadlessAcceptanceQuitOnComplete"))
			|| FParse::Param(CommandLine, TEXT("OsvayderUEVisibleManualAcceptanceQuitOnComplete"));
		SOsvayderEditorWidget::NormalizeHeadlessAcceptanceRequestPaths(OutRequest);
		return true;
	}

	void WriteHeadlessAcceptanceFailureReceipt(
		FOsvayderUEHeadlessAcceptanceRequest Request,
		const FString& FailureReason,
		const FString& DispatchPath)
	{
		if (Request.OutputDir.IsEmpty())
		{
			Request.OutputDir = GetDefaultHeadlessAcceptanceOutputDir();
		}
		SOsvayderEditorWidget::NormalizeHeadlessAcceptanceRequestPaths(Request);

		FOsvayderUEHeadlessAcceptanceReceiptContext Context;
		Context.Request = Request;
		Context.ReceiptPath = SOsvayderEditorWidget::ResolveHeadlessAcceptanceReceiptPath(Request);
		Context.StartedAtUtc = FDateTime::UtcNow().ToIso8601();
		Context.CompletedAtUtc = Context.StartedAtUtc;
		Context.DispatchPath = DispatchPath;
		Context.Status = TEXT("rejected");
		Context.FailureReason = FailureReason;

		FString SaveError;
		if (!SOsvayderEditorWidget::SaveHeadlessAcceptanceReceiptArtifact(Context, SaveError))
		{
			UE_LOG(
				LogOsvayderUE,
				Warning,
				TEXT("OsvayderUE.RunHeadlessNewSessionAcceptance failed to write rejection receipt: %s"),
				*SaveError);
		}
	}

	TSharedPtr<SOsvayderEditorWidget> EnsureLiveOsvayderEditorWidget()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(ClaudeTabName);
		TSharedPtr<SOsvayderEditorWidget> Widget = SOsvayderEditorWidget::GetLiveWidget();
		if (!Widget.IsValid())
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.DebugDictation failed: Osvayder UE widget is not available."));
		}

		return Widget;
	}

	void DispatchHeadlessAcceptanceRequest(
		const FOsvayderUEHeadlessAcceptanceRequest& Request,
		const FString& DispatchPath)
	{
		FOsvayderUEHeadlessAcceptanceRequest DispatchRequest = Request;
		if (DispatchRequest.TriggerPath.IsEmpty())
		{
			DispatchRequest.TriggerPath = DispatchPath;
		}

		TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
		if (!Widget.IsValid())
		{
			WriteHeadlessAcceptanceFailureReceipt(DispatchRequest, TEXT("widget_unavailable"), DispatchPath);
			UE_LOG(
				LogOsvayderUE,
				Warning,
				TEXT("OsvayderUE.RunHeadlessNewSessionAcceptance failed: Osvayder UE widget is not available."));
			if (DispatchRequest.bRequestEditorExitOnComplete)
			{
				FPlatformMisc::RequestExit(false);
			}
			return;
		}

		FString StartError;
		if (!Widget->StartHeadlessNewSessionAcceptanceBridge(DispatchRequest, StartError))
		{
			UE_LOG(
				LogOsvayderUE,
				Warning,
				TEXT("OsvayderUE.RunHeadlessNewSessionAcceptance did not start: %s"),
				*StartError);
			if (DispatchRequest.bRequestEditorExitOnComplete)
			{
				FPlatformMisc::RequestExit(false);
			}
			return;
		}

		UE_LOG(
			LogOsvayderUE,
			Log,
			TEXT("OsvayderUE.RunHeadlessNewSessionAcceptance started: prefix=%s receipt=%s"),
			*DispatchRequest.Prefix,
			*SOsvayderEditorWidget::ResolveHeadlessAcceptanceReceiptPath(DispatchRequest));
	}

	void ScheduleCommandLineHeadlessAcceptanceRequest()
	{
		FOsvayderUEHeadlessAcceptanceRequest Request;
		FString ParseError;
		if (!TryParseHeadlessAcceptanceCommandLineRequest(Request, ParseError))
		{
			return;
		}

		if (!ParseError.IsEmpty())
		{
			const FString ParseDispatchPath = Request.bVisibleManualEmulator
				? TEXT("command_line_visible_manual_parse")
				: TEXT("command_line_parse");
			WriteHeadlessAcceptanceFailureReceipt(Request, ParseError, ParseDispatchPath);
			UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE command-line headless acceptance rejected: %s"), *ParseError);
			if (Request.bRequestEditorExitOnComplete)
			{
				FPlatformMisc::RequestExit(false);
			}
			return;
		}

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Request](float)
			{
				DispatchHeadlessAcceptanceRequest(
					Request,
					Request.TriggerPath.IsEmpty() ? TEXT("command_line_headless_acceptance") : Request.TriggerPath);
				return false;
			}),
			1.0f);
	}

	void ScheduleDictationAutoStop(const TWeakPtr<SOsvayderEditorWidget>& WeakWidget, const float DelaySeconds)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([WeakWidget](float)
			{
				const TSharedPtr<SOsvayderEditorWidget> Widget = WeakWidget.Pin();
				if (!Widget.IsValid())
				{
					return false;
				}

				if (!Widget->DebugStopDictation())
				{
					UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.DebugDictation auto-stop skipped: dictation was not recording."));
				}

				return false;
			}),
			DelaySeconds);
	}

	FString BackendToString(const EOsvayderUEProviderBackend Backend)
	{
		return Backend == EOsvayderUEProviderBackend::CodexCli ? TEXT("CodexCli") : TEXT("ClaudeCli");
	}

	FString RuntimeConnectionStateToString(const SOsvayderEditorWidget::EBackendRuntimeConnectionState State);

	FString GetRestartSurvivalSeedPrompt()
	{
		return TEXT("Reply with exactly: restart survival proof seed");
	}

	FString GetRestartSurvivalResumePrompt()
	{
		return TEXT("Reply with exactly: restart survival proof resumed");
	}

	FString GetRestartSurvivalRestoreIntentCompletionResponse()
	{
		return TEXT("restart survival restore intent completed");
	}

	FString GetRestartSurvivalRestoreIntentCompletionPrompt()
	{
		return FString::Printf(
			TEXT("Reply with exactly: %s"),
			*GetRestartSurvivalRestoreIntentCompletionResponse());
	}

	FString GetRestartSurvivalTaskContinuationSeedResponse()
	{
		return TEXT("restart survival task continuation seeded");
	}

	FString GetRestartSurvivalTaskContinuationCompletionResponse()
	{
		return TEXT("restart survival task continuation completed");
	}

	FString GetRestartSurvivalTaskContinuationCompletionPrompt()
	{
		return FString::Printf(
			TEXT("Reply with exactly: %s"),
			*GetRestartSurvivalTaskContinuationCompletionResponse());
	}

	FString BuildRestartSurvivalTaskContinuationSeedPrompt(
		const FString& TaskId,
		const FString& SessionId,
		const FString& RestoreSourcePath,
		const FString& RestoreTargetPath,
		const FString& RestoreBackupPath)
	{
		FString NormalizedRestoreSourcePath = RestoreSourcePath;
		FString NormalizedRestoreTargetPath = RestoreTargetPath;
		FString NormalizedRestoreBackupPath = RestoreBackupPath;
		FPaths::MakeStandardFilename(NormalizedRestoreSourcePath);
		FPaths::MakeStandardFilename(NormalizedRestoreTargetPath);
		FPaths::MakeStandardFilename(NormalizedRestoreBackupPath);

		return FString::Printf(
			TEXT("Use the structured MCP tool `restart_survival` exactly once with this exact JSON payload and no other tool calls:\n")
			TEXT("{\n")
			TEXT("  \"operation\": \"prepare_task_continuation_handoff\",\n")
			TEXT("  \"task_id\": \"%s\",\n")
			TEXT("  \"session_id\": \"%s\",\n")
			TEXT("  \"continuation_intent_prompt\": \"%s\",\n")
			TEXT("  \"detail\": \"task_driven_structured_handoff\",\n")
			TEXT("  \"auto_start_after_response\": true,\n")
			TEXT("  \"restore_enabled\": true,\n")
			TEXT("  \"restore_autosave_source_path\": \"%s\",\n")
			TEXT("  \"restore_target_path\": \"%s\",\n")
			TEXT("  \"restore_backup_path\": \"%s\",\n")
			TEXT("  \"restore_detail\": \"explicit_autosave_backed_restore\"\n")
			TEXT("}\n")
			TEXT("After the tool call succeeds, reply with exactly: %s"),
			*TaskId,
			*SessionId,
			*GetRestartSurvivalTaskContinuationCompletionPrompt(),
			*NormalizedRestoreSourcePath,
			*NormalizedRestoreTargetPath,
			*NormalizedRestoreBackupPath,
			*GetRestartSurvivalTaskContinuationSeedResponse());
	}

	FString BuildRestartSurvivalTaskContinuationNegativePrompt(
		const FString& TaskId,
		const FString& SessionId,
		const FString& WrongLinkedProviderSessionId)
	{
		return FString::Printf(
			TEXT("Use the structured MCP tool `restart_survival` exactly once with this exact JSON payload and no other tool calls:\n")
			TEXT("{\n")
			TEXT("  \"operation\": \"prepare_task_continuation_handoff\",\n")
			TEXT("  \"task_id\": \"%s\",\n")
			TEXT("  \"session_id\": \"%s\",\n")
			TEXT("  \"continuation_intent_prompt\": \"%s\",\n")
			TEXT("  \"detail\": \"task_driven_structured_handoff\",\n")
			TEXT("  \"linked_provider_session_id\": \"%s\",\n")
			TEXT("  \"auto_start_after_response\": true\n")
			TEXT("}\n")
			TEXT("After the tool call attempt finishes, reply with exactly: restart survival task continuation negative finished"),
			*TaskId,
			*SessionId,
			*GetRestartSurvivalTaskContinuationCompletionPrompt(),
			*WrongLinkedProviderSessionId);
	}

	FString GetRestartSurvivalAutonomousClosedEditorSeedResponse()
	{
		return TEXT("Closed-editor blocker detected. Unreal will close so bounded restore/build work can continue, then the same task will resume after relaunch.");
	}

	FString GetRestartSurvivalAutonomousClosedEditorCompletionResponse()
	{
		return TEXT("restart survival autonomous closed-editor escalation completed");
	}

	FString GetRestartSurvivalAutonomousClosedEditorNegativeResponse()
	{
		return TEXT("restart survival autonomous closed-editor escalation rejected");
	}

	FString GetRestartSurvivalAutonomousClosedEditorCompletionPrompt()
	{
		return FString::Printf(
			TEXT("Reply with exactly: %s"),
			*GetRestartSurvivalAutonomousClosedEditorCompletionResponse());
	}

	bool IsRestartSurvivalAutonomousClosedEditorPostReattachPromptSafe(const FString& Prompt)
	{
		return Prompt.Contains(TEXT("Do not call `restart_survival` again"), ESearchCase::CaseSensitive)
			&& Prompt.Contains(TEXT("do not close Unreal again"), ESearchCase::IgnoreCase)
			&& Prompt.Contains(TEXT("Stop-Process on UnrealEditor"), ESearchCase::CaseSensitive);
	}

	FString GetRestartSurvivalClosedEditorTransitionNoticePrefix()
	{
		return TEXT("Closed-editor blocker detected.");
	}

	FString BuildRestartSurvivalAutonomousClosedEditorSeedPrompt(
		const FString& RestoreSourcePath,
		const FString& RestoreTargetPath,
		const FString& RestoreBackupPath)
	{
		FString NormalizedRestoreSourcePath = RestoreSourcePath;
		FString NormalizedRestoreTargetPath = RestoreTargetPath;
		FString NormalizedRestoreBackupPath = RestoreBackupPath;
		FPaths::MakeStandardFilename(NormalizedRestoreSourcePath);
		FPaths::MakeStandardFilename(NormalizedRestoreTargetPath);
		FPaths::MakeStandardFilename(NormalizedRestoreBackupPath);

		return FString::Printf(
			TEXT("Continue one ordinary bounded project-local task.\n")
			TEXT("Replace the target file at \"%s\" with the exact autosave content from \"%s\".\n")
			TEXT("Before replacing it, preserve the previous target content as a backup at \"%s\".\n")
			TEXT("If this exact restore cannot continue truthfully while UnrealEditor stays open, use the accepted project-local closed-editor escalation path yourself.\n")
			TEXT("Do not ask me to press Restart Survival, do not emit JSON, do not invent task_id or session_id yourself, and keep the work inside the bounded family of exact restore/file/build/relaunch/reattach only.\n")
			TEXT("Give one short truthful notice before Unreal closes.\n")
			TEXT("After the restart-survival handoff is prepared, reply with that short truthful notice only.\n")
			TEXT("After relaunch and reattach, finish the bounded restore flow and reply with exactly: %s"),
			*NormalizedRestoreTargetPath,
			*NormalizedRestoreSourcePath,
			*NormalizedRestoreBackupPath,
			*GetRestartSurvivalAutonomousClosedEditorCompletionResponse());
	}

	FString BuildRestartSurvivalAutonomousClosedEditorNegativePrompt()
	{
		return FString::Printf(
			TEXT("Continue one ordinary task, but do not leave the truthful bounded restart-survival family.\n")
			TEXT("Close Unreal and, while Unreal is closed, inspect the currently loaded editor world and summarize live actors before reopening.\n")
			TEXT("That request is intentionally outside the accepted closed-editor family because it needs Unreal-dependent editor access while the editor is closed.\n")
			TEXT("If you cannot do this truthfully inside the bounded closed-editor lane, do not start restart survival and reply with exactly: %s"),
			*GetRestartSurvivalAutonomousClosedEditorNegativeResponse());
	}

	FString GetCurrentEditorExecutablePath()
	{
		return FPlatformProcess::ExecutablePath();
	}

	FString GetCurrentEngineRoot()
	{
		const FString EditorExecutablePath = GetCurrentEditorExecutablePath();
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(EditorExecutablePath), TEXT(".."), TEXT(".."), TEXT("..")));
	}

	FString GetProjectEditorTargetName()
	{
		return FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) + TEXT("Editor");
	}

	FString GetRestartSurvivalProofDir(const FString& RunTag)
	{
		const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("restart_survival"), RunTag);
		IFileManager::Get().MakeDirectory(*OutputDir, true);
		return OutputDir;
	}

	FString LoadTextFileIfExists(const FString& Path)
	{
		FString Text;
		if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path))
		{
			return FString();
		}

		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	FString FindProviderSessionIdInTraceRecords(const TArray<TSharedPtr<FJsonObject>>& Records)
	{
		for (const TSharedPtr<FJsonObject>& Record : Records)
		{
			if (!Record.IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* PayloadObject = nullptr;
			if (!Record->TryGetObjectField(TEXT("payload"), PayloadObject) || !PayloadObject || !(*PayloadObject).IsValid())
			{
				continue;
			}

			FString ProviderSessionId;
			if ((*PayloadObject)->TryGetStringField(TEXT("provider_session_id"), ProviderSessionId) && !ProviderSessionId.IsEmpty())
			{
				return ProviderSessionId;
			}
		}

		return FString();
	}

	struct FRestartSurvivalProofState : public TSharedFromThis<FRestartSurvivalProofState>
	{
		FString RunTag;
		FString StartPath;
		FString CommandDescription;
		TWeakPtr<SOsvayderEditorWidget> WeakWidget;
		EOsvayderUEProviderBackend Backend = EOsvayderUEProviderBackend::CodexCli;
		double StartTimeSeconds = 0.0;
		double TimeoutSeconds = 240.0;
		bool bSeedPromptStarted = false;
		bool bResumePromptStarted = false;
		bool bTimedOut = false;
		FString ErrorText;
		FString SeedPromptText;
		FString ExpectedSeedResponseText;
		FString ObservedSeedResponseText;
		FString ProviderSessionIdBefore;
		FString ProviderSessionIdAfter;
		FString ProviderThreadStatePath;
		FString DetachedFileTargetPath;
		FString DetachedFileExpectedText;
		FString DetachedFileObservedText;
		FString RestoreSourcePath;
		FString RestoreTargetPath;
		FString RestoreBackupPath;
		FString RestoreExpectedText;
		FString RestoreTargetObservedText;
		FString RestoreBackupObservedText;
		FString BuildLogPath;
		FString DetachedLogPath;
		FString ReattachNotice;
		FString TransitionNoticeText;
		FString PreparedRequestPath;
		FString PreparedRequestId;
		FString PreparedTaskId;
		FString PreparedSessionId;
		FString CompletionPromptText;
		FString ExpectedCompletionResponseText;
		FString ObservedCompletionResponseText;
		bool bReattachNoticeVisible = false;
		bool bTransitionNoticeVisible = false;
		bool bSeedHistoryVisible = false;
		bool bDetachedFileWriteObserved = false;
		bool bRestoreObserved = false;
		bool bRestoreBackupObserved = false;
		bool bBuildLogObserved = false;
		bool bPreparedRequestWritten = false;
		bool bPreparedRequestConsumed = false;
		bool bAutonomousTaskIdDerived = false;
		bool bAutonomousSessionIdDerived = false;
		bool bSeedResponseMatched = false;
		bool bCompletionResponseMatched = false;
		SOsvayderEditorWidget::EBackendRuntimeConnectionState RuntimeState = SOsvayderEditorWidget::EBackendRuntimeConnectionState::Unknown;
		FString RuntimeDetail;
	};

void WriteRestartSurvivalProofJson(
	const TSharedRef<FRestartSurvivalProofState>& State,
	const FString& FinalState)
{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("run_tag"), State->RunTag);
		Root->SetStringField(TEXT("state"), FinalState);
		Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(
			TEXT("command"),
			State->CommandDescription.IsEmpty()
				? TEXT("OsvayderUE.RunRestartSurvivalProof / OsvayderUE.FinalizeRestartSurvivalProof")
				: State->CommandDescription);
		Root->SetStringField(TEXT("start_path"), State->StartPath);
		Root->SetStringField(TEXT("backend"), BackendToString(State->Backend));
		Root->SetBoolField(TEXT("seed_prompt_started"), State->bSeedPromptStarted);
		Root->SetStringField(TEXT("seed_prompt_text"), State->SeedPromptText);
		Root->SetStringField(TEXT("expected_seed_response_text"), State->ExpectedSeedResponseText);
		Root->SetStringField(TEXT("observed_seed_response_text"), State->ObservedSeedResponseText);
		Root->SetBoolField(TEXT("seed_response_matched"), State->bSeedResponseMatched);
		Root->SetBoolField(TEXT("resume_prompt_started"), State->bResumePromptStarted);
		Root->SetBoolField(TEXT("timed_out"), State->bTimedOut);
		Root->SetStringField(TEXT("runtime_state"), RuntimeConnectionStateToString(State->RuntimeState));
		Root->SetStringField(TEXT("runtime_detail"), State->RuntimeDetail);
		Root->SetStringField(TEXT("error"), State->ErrorText);

		TSharedPtr<FJsonObject> SessionProof = MakeShared<FJsonObject>();
		SessionProof->SetStringField(TEXT("provider_thread_id_before"), State->ProviderSessionIdBefore);
		SessionProof->SetStringField(TEXT("provider_thread_id_after"), State->ProviderSessionIdAfter);
		SessionProof->SetBoolField(TEXT("same_provider_thread"), !State->ProviderSessionIdBefore.IsEmpty() && State->ProviderSessionIdBefore == State->ProviderSessionIdAfter);
		SessionProof->SetStringField(TEXT("provider_thread_state_path"), State->ProviderThreadStatePath);
		Root->SetObjectField(TEXT("session_proof"), SessionProof);

		TSharedPtr<FJsonObject> DetachedWork = MakeShared<FJsonObject>();
		DetachedWork->SetStringField(TEXT("detached_file_target_path"), State->DetachedFileTargetPath);
		DetachedWork->SetStringField(TEXT("detached_file_expected_text"), State->DetachedFileExpectedText);
		DetachedWork->SetStringField(TEXT("detached_file_observed_text"), State->DetachedFileObservedText);
		DetachedWork->SetBoolField(TEXT("detached_file_write_observed"), State->bDetachedFileWriteObserved);
		DetachedWork->SetStringField(TEXT("restore_source_path"), State->RestoreSourcePath);
		DetachedWork->SetStringField(TEXT("restore_target_path"), State->RestoreTargetPath);
		DetachedWork->SetStringField(TEXT("restore_backup_path"), State->RestoreBackupPath);
		DetachedWork->SetStringField(TEXT("restore_expected_text"), State->RestoreExpectedText);
		DetachedWork->SetStringField(TEXT("restore_target_observed_text"), State->RestoreTargetObservedText);
		DetachedWork->SetStringField(TEXT("restore_backup_observed_text"), State->RestoreBackupObservedText);
		DetachedWork->SetBoolField(TEXT("restore_observed"), State->bRestoreObserved);
		DetachedWork->SetBoolField(TEXT("restore_backup_observed"), State->bRestoreBackupObserved);
		DetachedWork->SetStringField(TEXT("build_log_path"), State->BuildLogPath);
		DetachedWork->SetBoolField(TEXT("build_log_observed"), State->bBuildLogObserved);
		DetachedWork->SetStringField(TEXT("detached_log_path"), State->DetachedLogPath);
		Root->SetObjectField(TEXT("detached_work_proof"), DetachedWork);

		TSharedPtr<FJsonObject> WidgetProof = MakeShared<FJsonObject>();
		WidgetProof->SetStringField(TEXT("reattach_notice"), State->ReattachNotice);
		WidgetProof->SetBoolField(TEXT("reattach_notice_visible"), State->bReattachNoticeVisible);
		WidgetProof->SetStringField(TEXT("transition_notice_text"), State->TransitionNoticeText);
		WidgetProof->SetBoolField(TEXT("transition_notice_visible"), State->bTransitionNoticeVisible);
		WidgetProof->SetBoolField(TEXT("seed_history_visible"), State->bSeedHistoryVisible);
		Root->SetObjectField(TEXT("widget_proof"), WidgetProof);

		TSharedPtr<FJsonObject> RestoreRequestProof = MakeShared<FJsonObject>();
		RestoreRequestProof->SetStringField(TEXT("request_path"), State->PreparedRequestPath);
		RestoreRequestProof->SetStringField(TEXT("request_id"), State->PreparedRequestId);
		RestoreRequestProof->SetStringField(TEXT("task_id"), State->PreparedTaskId);
		RestoreRequestProof->SetStringField(TEXT("session_id"), State->PreparedSessionId);
		RestoreRequestProof->SetBoolField(TEXT("prepared"), State->bPreparedRequestWritten);
		RestoreRequestProof->SetBoolField(TEXT("consumed"), State->bPreparedRequestConsumed);
		RestoreRequestProof->SetBoolField(TEXT("autonomous_task_id_derived"), State->bAutonomousTaskIdDerived);
		RestoreRequestProof->SetBoolField(TEXT("autonomous_session_id_derived"), State->bAutonomousSessionIdDerived);
		Root->SetObjectField(TEXT("restore_request_proof"), RestoreRequestProof);

		TSharedPtr<FJsonObject> CompletionProof = MakeShared<FJsonObject>();
		CompletionProof->SetStringField(TEXT("prompt_text"), State->CompletionPromptText);
		CompletionProof->SetStringField(TEXT("expected_response_text"), State->ExpectedCompletionResponseText);
		CompletionProof->SetStringField(TEXT("observed_response_text"), State->ObservedCompletionResponseText);
		CompletionProof->SetBoolField(TEXT("response_matched"), State->bCompletionResponseMatched);
		Root->SetObjectField(TEXT("completion_proof"), CompletionProof);

		Root->SetObjectField(TEXT("restart_survival"), FOsvayderUERestartSurvivalManager::BuildReadbackJson());

		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	FFileHelper::SaveStringToFile(JsonText, *GetBackendRunOutputPath(State->RunTag), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void CarryForwardRestartSurvivalProofSeedState(const TSharedRef<FRestartSurvivalProofState>& State)
{
	const FString ExistingJson = LoadTextFileIfExists(GetBackendRunOutputPath(State->RunTag));
	if (ExistingJson.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExistingJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return;
	}

	bool bBoolValue = false;
	FString StringValue;
	if (Root->TryGetBoolField(TEXT("seed_prompt_started"), bBoolValue))
	{
		State->bSeedPromptStarted = bBoolValue;
	}
	if (Root->TryGetStringField(TEXT("seed_prompt_text"), StringValue) && !StringValue.IsEmpty())
	{
		State->SeedPromptText = StringValue;
	}
	if (Root->TryGetStringField(TEXT("expected_seed_response_text"), StringValue) && !StringValue.IsEmpty())
	{
		State->ExpectedSeedResponseText = StringValue;
	}
	if (Root->TryGetStringField(TEXT("observed_seed_response_text"), StringValue) && !StringValue.IsEmpty())
	{
		State->ObservedSeedResponseText = StringValue;
	}
	if (Root->TryGetBoolField(TEXT("seed_response_matched"), bBoolValue))
	{
		State->bSeedResponseMatched = bBoolValue;
	}

	const TSharedPtr<FJsonObject>* WidgetProof = nullptr;
	if (Root->TryGetObjectField(TEXT("widget_proof"), WidgetProof)
		&& WidgetProof
		&& (*WidgetProof).IsValid())
	{
		if ((*WidgetProof)->TryGetStringField(TEXT("transition_notice_text"), StringValue) && !StringValue.IsEmpty())
		{
			State->TransitionNoticeText = StringValue;
		}
		if ((*WidgetProof)->TryGetBoolField(TEXT("transition_notice_visible"), bBoolValue))
		{
			State->bTransitionNoticeVisible = bBoolValue;
		}
	}

	const TSharedPtr<FJsonObject>* RestoreRequestProof = nullptr;
	if (Root->TryGetObjectField(TEXT("restore_request_proof"), RestoreRequestProof)
		&& RestoreRequestProof
		&& (*RestoreRequestProof).IsValid())
	{
		if ((*RestoreRequestProof)->TryGetBoolField(TEXT("prepared"), bBoolValue))
		{
			State->bPreparedRequestWritten = bBoolValue;
		}
		if ((*RestoreRequestProof)->TryGetStringField(TEXT("request_id"), StringValue) && !StringValue.IsEmpty())
		{
			State->PreparedRequestId = StringValue;
		}
		if ((*RestoreRequestProof)->TryGetStringField(TEXT("task_id"), StringValue) && !StringValue.IsEmpty())
		{
			State->PreparedTaskId = StringValue;
		}
		if ((*RestoreRequestProof)->TryGetStringField(TEXT("session_id"), StringValue) && !StringValue.IsEmpty())
		{
			State->PreparedSessionId = StringValue;
		}
		if ((*RestoreRequestProof)->TryGetBoolField(TEXT("autonomous_task_id_derived"), bBoolValue))
		{
			State->bAutonomousTaskIdDerived = bBoolValue;
		}
		if ((*RestoreRequestProof)->TryGetBoolField(TEXT("autonomous_session_id_derived"), bBoolValue))
		{
			State->bAutonomousSessionIdDerived = bBoolValue;
		}
	}
}

EOsvayderUEProviderBackend GetAlternateBackend(const EOsvayderUEProviderBackend Backend)
{
	return Backend == EOsvayderUEProviderBackend::CodexCli
			? EOsvayderUEProviderBackend::ClaudeCli
			: EOsvayderUEProviderBackend::CodexCli;
	}

	FString RuntimeConnectionStateToString(const SOsvayderEditorWidget::EBackendRuntimeConnectionState State)
	{
		switch (State)
		{
		case SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected:
			return TEXT("connected");
		case SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connecting:
			return TEXT("connecting");
		case SOsvayderEditorWidget::EBackendRuntimeConnectionState::Failed:
			return TEXT("failed");
		case SOsvayderEditorWidget::EBackendRuntimeConnectionState::Unknown:
		default:
			return TEXT("unknown");
		}
	}

	struct FBackendStatusChatNoiseProofState : public TSharedFromThis<FBackendStatusChatNoiseProofState>
	{
		FString RunTag;
		FString Prompt;
		TWeakPtr<SOsvayderEditorWidget> WeakWidget;
		EOsvayderUEProviderBackend OriginalBackend = EOsvayderUEProviderBackend::ClaudeCli;
		EOsvayderUEProviderBackend PromptBackend = EOsvayderUEProviderBackend::ClaudeCli;
		EOsvayderUEProviderBackend SwitchedBackend = EOsvayderUEProviderBackend::CodexCli;
		double StartTimeSeconds = 0.0;
		double TimeoutSeconds = 120.0;
		bool bPromptDispatchStarted = false;
		bool bTimedOut = false;
		int32 VisibleMessageCountBefore = 0;
		int32 VisibleMessageCountAfterResponse = 0;
		int32 VisibleMessageCountAfterForcedRefresh = 0;
		int32 VisibleMessageCountAfterSwitch = 0;
		bool bNoisePresentBefore = false;
		bool bNoisePresentAfterResponse = false;
		bool bNoisePresentAfterForcedRefresh = false;
		bool bSwitchNoticePresent = false;
		SOsvayderEditorWidget::EBackendRuntimeConnectionState RuntimeState = SOsvayderEditorWidget::EBackendRuntimeConnectionState::Unknown;
		FString RuntimeDetail;
	};

	void WriteBackendStatusChatNoiseProofJson(
		const TSharedRef<FBackendStatusChatNoiseProofState>& State,
		const FString& FinalState,
		const FString& ErrorText)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("run_tag"), State->RunTag);
		Root->SetStringField(TEXT("state"), FinalState);
		Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("command"), TEXT("OsvayderUE.RunBackendStatusChatNoiseProof"));
		Root->SetStringField(TEXT("prompt"), State->Prompt);
		Root->SetStringField(TEXT("original_backend"), BackendToString(State->OriginalBackend));
		Root->SetStringField(TEXT("prompt_backend"), BackendToString(State->PromptBackend));
		Root->SetStringField(TEXT("switched_backend"), BackendToString(State->SwitchedBackend));
		Root->SetBoolField(TEXT("prompt_dispatch_started"), State->bPromptDispatchStarted);
		Root->SetBoolField(TEXT("timed_out"), State->bTimedOut);
		Root->SetStringField(TEXT("runtime_state"), RuntimeConnectionStateToString(State->RuntimeState));
		Root->SetStringField(TEXT("runtime_detail"), State->RuntimeDetail);
		Root->SetStringField(TEXT("error"), ErrorText);

		TSharedPtr<FJsonObject> NormalPromptProof = MakeShared<FJsonObject>();
		NormalPromptProof->SetNumberField(TEXT("visible_message_count_before"), State->VisibleMessageCountBefore);
		NormalPromptProof->SetNumberField(TEXT("visible_message_count_after_response"), State->VisibleMessageCountAfterResponse);
		NormalPromptProof->SetNumberField(TEXT("visible_message_count_after_forced_refresh"), State->VisibleMessageCountAfterForcedRefresh);
		NormalPromptProof->SetBoolField(TEXT("noise_present_before"), State->bNoisePresentBefore);
		NormalPromptProof->SetBoolField(TEXT("noise_present_after_response"), State->bNoisePresentAfterResponse);
		NormalPromptProof->SetBoolField(TEXT("noise_present_after_forced_refresh"), State->bNoisePresentAfterForcedRefresh);
		NormalPromptProof->SetBoolField(
			TEXT("forced_refresh_kept_visible_count_stable"),
			State->VisibleMessageCountAfterForcedRefresh == State->VisibleMessageCountAfterResponse);
		Root->SetObjectField(TEXT("normal_prompt_proof"), NormalPromptProof);

		TSharedPtr<FJsonObject> BackendSwitchProof = MakeShared<FJsonObject>();
		BackendSwitchProof->SetNumberField(TEXT("visible_message_count_after_switch"), State->VisibleMessageCountAfterSwitch);
		BackendSwitchProof->SetBoolField(TEXT("switch_notice_present"), State->bSwitchNoticePresent);
		Root->SetObjectField(TEXT("backend_switch_proof"), BackendSwitchProof);

		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		FFileHelper::SaveStringToFile(JsonText, *GetBackendRunOutputPath(State->RunTag), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static FAutoConsoleCommand GOsvayderUEDebugDictationCommand(
		TEXT("OsvayderUE.DebugDictation"),
		TEXT("Open or drive the bounded dictation flow for proof automation. Usage: OsvayderUE.DebugDictation <open|start|stop|toggle|run_capture|run_fixture> [auto_stop_seconds]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.DebugDictation <open|start|stop|toggle|run_capture|run_fixture> [auto_stop_seconds]"));
				return;
			}

			const FString Action = Args[0].TrimStartAndEnd().ToLower();
			TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
				return;
			}

			if (Action == TEXT("open"))
			{
				UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE.DebugDictation -> panel_open"));
				return;
			}

			if (Action == TEXT("start"))
			{
				if (Widget->DebugStartDictation())
				{
					UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE.DebugDictation -> start"));
				}
				else
				{
					UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.DebugDictation -> start_failed"));
				}
				return;
			}

			if (Action == TEXT("stop"))
			{
				if (Widget->DebugStopDictation())
				{
					UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE.DebugDictation -> stop"));
				}
				else
				{
					UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.DebugDictation -> stop_failed"));
				}
				return;
			}

			if (Action == TEXT("toggle"))
			{
				if (Widget->DebugToggleDictation())
				{
					UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE.DebugDictation -> toggle"));
				}
				else
				{
					UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.DebugDictation -> toggle_failed"));
				}
				return;
			}

			if (Action == TEXT("run_fixture"))
			{
				const FString FixturePath = FPlatformMisc::GetEnvironmentVariable(OsvayderUEConstants::VoiceDictation::DebugFixtureEnvVar);
				if (FixturePath.IsEmpty())
				{
					UE_LOG(
						LogOsvayderUE,
						Warning,
						TEXT("OsvayderUE.DebugDictation run_fixture requires %s to point to a .wav file."),
						OsvayderUEConstants::VoiceDictation::DebugFixtureEnvVar);
					return;
				}

				const float AutoStopDelaySeconds = Args.Num() > 1
					? FMath::Max(0.1f, FCString::Atof(*Args[1]))
					: 3.0f;

				if (!Widget->DebugStartDictation())
				{
					UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.DebugDictation run_fixture -> start_failed"));
					return;
				}

				ScheduleDictationAutoStop(Widget, AutoStopDelaySeconds);
				UE_LOG(
					LogOsvayderUE,
					Log,
					TEXT("OsvayderUE.DebugDictation run_fixture -> started fixture=\"%s\" auto_stop_seconds=%.2f"),
					*FixturePath,
					AutoStopDelaySeconds);
				return;
			}

			if (Action == TEXT("run_capture"))
			{
				const float AutoStopDelaySeconds = Args.Num() > 1
					? FMath::Max(0.1f, FCString::Atof(*Args[1]))
					: 0.2f;

				if (!Widget->DebugStartDictation())
				{
					UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.DebugDictation run_capture -> start_failed"));
					return;
				}

				ScheduleDictationAutoStop(Widget, AutoStopDelaySeconds);
				UE_LOG(
					LogOsvayderUE,
					Log,
					TEXT("OsvayderUE.DebugDictation run_capture -> started auto_stop_seconds=%.2f"),
					AutoStopDelaySeconds);
				return;
			}

			UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.DebugDictation <open|start|stop|toggle|run_capture|run_fixture> [auto_stop_seconds]"));
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunBackendStatusChatNoiseProofCommand(
		TEXT("OsvayderUE.RunBackendStatusChatNoiseProof"),
		TEXT("Capture a live widget proof that ordinary prompts do not append `Backend status updated.` while backend-switch notices still appear. Usage: OsvayderUE.RunBackendStatusChatNoiseProof <run_tag> [prompt...]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunBackendStatusChatNoiseProof <run_tag> [prompt...]"));
				return;
			}

			UOsvayderUESettings* Settings = UOsvayderUESettings::GetMutable();
			if (!Settings)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunBackendStatusChatNoiseProof failed: settings unavailable"));
				return;
			}

			TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunBackendStatusChatNoiseProof failed: Osvayder UE widget is not available."));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const FString Prompt = Args.Num() > 1
				? JoinPromptArgs(Args, 1)
				: TEXT("Reply with exactly: backend status chat noise proof");

			const EOsvayderUEProviderBackend OriginalBackend = Settings->PreferredBackend;
			EOsvayderUEProviderBackend PromptBackend = OriginalBackend;
			FAgentBackendStatus PromptBackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			if (!AgentBackendCanExecutePrompt(PromptBackendStatus))
			{
				const EOsvayderUEProviderBackend AlternateBackend = GetAlternateBackend(OriginalBackend);
				Settings->PreferredBackend = AlternateBackend;
				Widget->DebugProcessBackendStateNow();
				PromptBackend = AlternateBackend;
				PromptBackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			}

			const TSharedRef<FBackendStatusChatNoiseProofState> ProofState = MakeShared<FBackendStatusChatNoiseProofState>();
			ProofState->RunTag = RunTag;
			ProofState->Prompt = Prompt;
			ProofState->WeakWidget = Widget;
			ProofState->OriginalBackend = OriginalBackend;
			ProofState->PromptBackend = PromptBackend;
			ProofState->SwitchedBackend = GetAlternateBackend(PromptBackend);
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();

			if (!AgentBackendCanExecutePrompt(PromptBackendStatus))
			{
				ProofState->RuntimeState = SOsvayderEditorWidget::EBackendRuntimeConnectionState::Failed;
				ProofState->RuntimeDetail = PromptBackendStatus.Detail.IsEmpty()
					? TEXT("No configured backend could execute the prompt for widget proof.")
					: PromptBackendStatus.Detail;
				WriteBackendStatusChatNoiseProofJson(ProofState, TEXT("rejected"), ProofState->RuntimeDetail);
				Settings->PreferredBackend = OriginalBackend;
				Widget->DebugProcessBackendStateNow();
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunBackendStatusChatNoiseProof %s -> rejected"), *RunTag);
				return;
			}

			ProofState->VisibleMessageCountBefore = Widget->DebugGetVisibleMessageCount();
			ProofState->bNoisePresentBefore = Widget->DebugVisibleChatContains(TEXT("Backend status updated."));
			Widget->DebugClearInputText();
			Widget->DebugSetInputText(Prompt);
			ProofState->bPromptDispatchStarted = Widget->DebugSendCurrentInput();

			if (!ProofState->bPromptDispatchStarted)
			{
				ProofState->RuntimeState = Widget->DebugGetLastRuntimeConnectionState();
				ProofState->RuntimeDetail = TEXT("Widget prompt dispatch did not start.");
				WriteBackendStatusChatNoiseProofJson(ProofState, TEXT("failed"), ProofState->RuntimeDetail);
				Settings->PreferredBackend = OriginalBackend;
				Widget->DebugProcessBackendStateNow();
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunBackendStatusChatNoiseProof %s -> start_failed"), *RunTag);
				return;
			}

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState, Settings](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
						ProofState->RuntimeState = SOsvayderEditorWidget::EBackendRuntimeConnectionState::Failed;
				ProofState->RuntimeDetail = TEXT("Osvayder UE widget became unavailable during proof.");
						WriteBackendStatusChatNoiseProofJson(ProofState, TEXT("failed"), ProofState->RuntimeDetail);
						UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunBackendStatusChatNoiseProof %s -> widget_unavailable"), *ProofState->RunTag);
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for the widget prompt to complete.");
						WriteBackendStatusChatNoiseProofJson(ProofState, TEXT("failed"), ProofState->RuntimeDetail);
						Settings->PreferredBackend = ProofState->OriginalBackend;
						LiveWidget->DebugProcessBackendStateNow();
						UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunBackendStatusChatNoiseProof %s -> timed_out"), *ProofState->RunTag);
						return false;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					ProofState->VisibleMessageCountAfterResponse = LiveWidget->DebugGetVisibleMessageCount();
					ProofState->bNoisePresentAfterResponse = LiveWidget->DebugVisibleChatContains(TEXT("Backend status updated."));

					LiveWidget->DebugForceBackendSnapshotRefresh(true);
					ProofState->VisibleMessageCountAfterForcedRefresh = LiveWidget->DebugGetVisibleMessageCount();
					ProofState->bNoisePresentAfterForcedRefresh = LiveWidget->DebugVisibleChatContains(TEXT("Backend status updated."));

					Settings->PreferredBackend = ProofState->SwitchedBackend;
					LiveWidget->DebugProcessBackendStateNow();
					ProofState->VisibleMessageCountAfterSwitch = LiveWidget->DebugGetVisibleMessageCount();
					ProofState->bSwitchNoticePresent = LiveWidget->DebugVisibleChatContains(TEXT("Active assistant backend switched from"));

					Settings->PreferredBackend = ProofState->OriginalBackend;
					LiveWidget->DebugProcessBackendStateNow();

					const bool bSuccess =
						ProofState->RuntimeState == SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected
						&& !ProofState->bNoisePresentAfterResponse
						&& !ProofState->bNoisePresentAfterForcedRefresh
						&& ProofState->VisibleMessageCountAfterForcedRefresh == ProofState->VisibleMessageCountAfterResponse
						&& ProofState->bSwitchNoticePresent;

					WriteBackendStatusChatNoiseProofJson(
						ProofState,
						bSuccess ? TEXT("completed") : TEXT("failed"),
						bSuccess ? FString() : ProofState->RuntimeDetail);

					if (bSuccess)
					{
						UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE.RunBackendStatusChatNoiseProof %s -> completed"), *ProofState->RunTag);
					}
					else
					{
						UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunBackendStatusChatNoiseProof %s -> failed"), *ProofState->RunTag);
					}

					return false;
				}),
				0.2f);

			UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE.RunBackendStatusChatNoiseProof started: %s"), *GetBackendRunOutputPath(RunTag));
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunRestartSurvivalProofCommand(
		TEXT("OsvayderUE.RunRestartSurvivalProof"),
		TEXT("Start a live detached restart-survival proof for ordinary configured Codex work. Usage: OsvayderUE.RunRestartSurvivalProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunRestartSurvivalProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("widget_restart_survival_action");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();

			const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			if (BackendStatus.Backend != EOsvayderUEProviderBackend::CodexCli)
			{
				ProofState->ErrorText = TEXT("Restart-survival V1 is currently wired only for Codex CLI.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunRestartSurvivalProof %s -> rejected_non_codex"), *RunTag);
				return;
			}

			if (!FCodexCliRunner::ShouldUsePersistentConversationTransport())
			{
				ProofState->ErrorText = TEXT("Restart-survival V1 requires persistent_app_server transport for Codex CLI.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunRestartSurvivalProof %s -> rejected_non_persistent"), *RunTag);
				return;
			}

			if (!AgentBackendCanExecutePrompt(BackendStatus))
			{
				ProofState->ErrorText = BackendStatus.Detail.IsEmpty()
					? TEXT("Configured backend is not ready for restart-survival proof.")
					: BackendStatus.Detail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunRestartSurvivalProof %s -> rejected_backend_unready"), *RunTag);
				return;
			}

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available for restart-survival proof.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunRestartSurvivalProof %s -> widget_unavailable"), *RunTag);
				return;
			}

			ProofState->WeakWidget = Widget;
			Widget->DebugClearInputText();
			Widget->DebugSetInputText(GetRestartSurvivalSeedPrompt());
			ProofState->bSeedPromptStarted = Widget->DebugSendCurrentInput();
			if (!ProofState->bSeedPromptStarted)
			{
				ProofState->RuntimeState = Widget->DebugGetLastRuntimeConnectionState();
				ProofState->RuntimeDetail = TEXT("Seed prompt dispatch did not start.");
				ProofState->ErrorText = ProofState->RuntimeDetail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunRestartSurvivalProof %s -> seed_start_failed"), *RunTag);
				return;
			}

			WriteRestartSurvivalProofJson(ProofState, TEXT("seed_pending"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable before detach.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for the seed prompt to complete.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					if (ProofState->RuntimeState != SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected)
					{
						ProofState->ErrorText = ProofState->RuntimeDetail.IsEmpty()
							? TEXT("Seed prompt did not complete successfully.")
							: ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					FCodexCliRunner* CodexRunner = static_cast<FCodexCliRunner*>(FOsvayderCodeSubsystem::Get().GetActiveBackend());
					if (CodexRunner == nullptr)
					{
						ProofState->ErrorText = TEXT("Configured backend is not an active Codex CLI runner.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (!CodexRunner->ExportActiveThreadStateForRestartSurvival(ProofState->ProviderThreadStatePath, ProofState->ProviderSessionIdBefore))
					{
						ProofState->ErrorText = TEXT("Failed to export the active Codex thread state for restart survival.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					const FString ProofDir = FPaths::ConvertRelativePathToFull(GetRestartSurvivalProofDir(ProofState->RunTag));
					ProofState->DetachedFileTargetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProofDir, TEXT("detached_file_result.txt")));
					ProofState->DetachedFileExpectedText = TEXT("detached file write completed during editor downtime");
					ProofState->RestoreSourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
						FPaths::ProjectSavedDir(),
						TEXT("Autosaves"),
						TEXT("OsvayderUE"),
						TEXT("RestartSurvival"),
						ProofState->RunTag,
						TEXT("proof_restore_source.txt")));
					ProofState->RestoreTargetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
						FPaths::ProjectDir(),
						TEXT("Content"),
						TEXT("RestartSurvivalProof"),
						ProofState->RunTag + TEXT("_proof_asset.txt")));
					ProofState->RestoreBackupPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProofDir, TEXT("proof_restore_target.backup.txt")));
					ProofState->RestoreExpectedText = TEXT("autosave-backed restore payload");
					ProofState->BuildLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), ProofState->RunTag + TEXT("_restart_survival_build.log")));
					ProofState->DetachedLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), ProofState->RunTag + TEXT("_restart_survival_detached.log")));
					ProofState->ReattachNotice = FString::Printf(
						TEXT("Restart-survival reattached to task task_%s. Detached file/build/recovery work completed while the editor was closed."),
						*ProofState->RunTag);

					IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProofState->RestoreSourcePath), true);
					IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProofState->RestoreTargetPath), true);
					FFileHelper::SaveStringToFile(ProofState->RestoreExpectedText, *ProofState->RestoreSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					FFileHelper::SaveStringToFile(TEXT("pre_restore target content"), *ProofState->RestoreTargetPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

					FClaudeRestartSurvivalStartOptions StartOptions;
					StartOptions.TaskIdOverride = FString::Printf(TEXT("task_%s"), *ProofState->RunTag);
					StartOptions.SessionIdOverride = FString::Printf(TEXT("restart_survival_%s"), *ProofState->RunTag);
					StartOptions.ReattachTokenOverride = FString::Printf(TEXT("reattach_%s"), *ProofState->RunTag);
					StartOptions.ReattachNoticeOverride = ProofState->ReattachNotice;
					StartOptions.AdditionalRelaunchArguments = FString::Printf(
						TEXT("-ExecCmds=\"OsvayderUE.FinalizeRestartSurvivalProof %s\""),
						*ProofState->RunTag);
					StartOptions.RestoreIntent.bEnabled = true;
					StartOptions.RestoreIntent.AutosaveSourcePath = ProofState->RestoreSourcePath;
					StartOptions.RestoreIntent.TargetPath = ProofState->RestoreTargetPath;
					StartOptions.RestoreIntent.BackupPath = ProofState->RestoreBackupPath;
					StartOptions.RestoreIntent.Detail = TEXT("explicit_autosave_backed_restore");
					StartOptions.Proof.bEnabled = true;
					StartOptions.Proof.RunTag = ProofState->RunTag;
					StartOptions.Proof.ProofOutputPath = GetBackendRunOutputPath(ProofState->RunTag);
					StartOptions.Proof.DetachedLogPath = ProofState->DetachedLogPath;
					StartOptions.Proof.BuildLogPath = ProofState->BuildLogPath;
					StartOptions.Proof.DetachedFileTargetPath = ProofState->DetachedFileTargetPath;
					StartOptions.Proof.DetachedFileExpectedText = ProofState->DetachedFileExpectedText;
					StartOptions.Proof.RestoreExpectedText = ProofState->RestoreExpectedText;
					StartOptions.Proof.FinalizeCommand = FString::Printf(TEXT("OsvayderUE.FinalizeRestartSurvivalProof %s"), *ProofState->RunTag);

					FString StartError;
					ProofState->StartPath = TEXT("widget_restart_survival_action");
					if (!LiveWidget->StartRestartSurvivalForCurrentTask(StartOptions, StartError))
					{
						ProofState->ErrorText = StartError;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					WriteRestartSurvivalProofJson(ProofState, TEXT("detaching"));
					UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE.RunRestartSurvivalProof %s -> detaching"), *ProofState->RunTag);
					return false;
				}),
				0.2f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUEFinalizeRestartSurvivalProofCommand(
		TEXT("OsvayderUE.FinalizeRestartSurvivalProof"),
		TEXT("Finalize the live restart-survival proof after relaunch. Usage: OsvayderUE.FinalizeRestartSurvivalProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.FinalizeRestartSurvivalProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("widget_restart_survival_action");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();

			FOsvayderUERestartSurvivalState RestartState;
			FString RestartError;
			if (!FOsvayderUERestartSurvivalManager::LoadState(RestartState, RestartError))
			{
				ProofState->ErrorText = RestartError;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.FinalizeRestartSurvivalProof %s -> missing_state"), *RunTag);
				return;
			}

			if (RestartState.Proof.RunTag != RunTag)
			{
				ProofState->ErrorText = FString::Printf(
					TEXT("Restart-survival state run_tag mismatch. Expected %s but found %s."),
					*RunTag,
					*RestartState.Proof.RunTag);
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.FinalizeRestartSurvivalProof %s -> run_tag_mismatch"), *RunTag);
				return;
			}

			ProofState->ProviderSessionIdBefore = RestartState.ProviderSessionId;
			ProofState->ProviderThreadStatePath = RestartState.ProviderThreadStatePath;
			ProofState->DetachedFileTargetPath = RestartState.Proof.DetachedFileTargetPath;
			ProofState->DetachedFileExpectedText = RestartState.Proof.DetachedFileExpectedText;
			ProofState->RestoreSourcePath = RestartState.RestoreIntent.AutosaveSourcePath;
			ProofState->RestoreTargetPath = RestartState.RestoreIntent.TargetPath;
			ProofState->RestoreBackupPath = RestartState.RestoreIntent.BackupPath;
			ProofState->RestoreExpectedText = RestartState.Proof.RestoreExpectedText;
			ProofState->BuildLogPath = RestartState.Proof.BuildLogPath;
			ProofState->DetachedLogPath = RestartState.Proof.DetachedLogPath;
			ProofState->ReattachNotice = RestartState.ReattachNotice;

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available after relaunch.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.FinalizeRestartSurvivalProof %s -> widget_unavailable"), *RunTag);
				return;
			}

			ProofState->WeakWidget = Widget;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable during proof finalization.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for restart-survival finalization.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					ProofState->DetachedFileObservedText = LoadTextFileIfExists(ProofState->DetachedFileTargetPath).TrimStartAndEnd();
					ProofState->RestoreTargetObservedText = LoadTextFileIfExists(ProofState->RestoreTargetPath).TrimStartAndEnd();
					ProofState->RestoreBackupObservedText = LoadTextFileIfExists(ProofState->RestoreBackupPath).TrimStartAndEnd();
					ProofState->bDetachedFileWriteObserved = ProofState->DetachedFileObservedText == ProofState->DetachedFileExpectedText;
					ProofState->bRestoreObserved = ProofState->RestoreTargetObservedText == ProofState->RestoreExpectedText;
					ProofState->bRestoreBackupObserved = !ProofState->RestoreBackupObservedText.IsEmpty();
					ProofState->bBuildLogObserved = !LoadTextFileIfExists(ProofState->BuildLogPath).IsEmpty();
					ProofState->bReattachNoticeVisible = !ProofState->ReattachNotice.IsEmpty() && LiveWidget->DebugVisibleChatContains(ProofState->ReattachNotice);
					ProofState->bSeedHistoryVisible = LiveWidget->DebugVisibleChatContains(GetRestartSurvivalSeedPrompt());

					if (!ProofState->bResumePromptStarted)
					{
						if (!ProofState->bDetachedFileWriteObserved ||
							!ProofState->bRestoreObserved ||
							!ProofState->bRestoreBackupObserved ||
							!ProofState->bBuildLogObserved ||
							!ProofState->bSeedHistoryVisible)
						{
							return true;
						}

						LiveWidget->DebugClearInputText();
						LiveWidget->DebugSetInputText(GetRestartSurvivalResumePrompt());
						ProofState->bResumePromptStarted = LiveWidget->DebugSendCurrentInput();
						if (!ProofState->bResumePromptStarted)
						{
							ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
							ProofState->RuntimeDetail = TEXT("Resume prompt dispatch did not start after relaunch.");
							ProofState->ErrorText = ProofState->RuntimeDetail;
							WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
							return false;
						}

						return true;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();

					FString TraceRunId;
					const TArray<TSharedPtr<FJsonObject>> TraceRecords = QueryLatestTraceRecordsForBackend(ProofState->Backend, TraceRunId);
					ProofState->ProviderSessionIdAfter = FindProviderSessionIdInTraceRecords(TraceRecords);

					const bool bSameProviderThread =
						!ProofState->ProviderSessionIdBefore.IsEmpty() &&
						ProofState->ProviderSessionIdBefore == ProofState->ProviderSessionIdAfter;
					const bool bSuccess =
						ProofState->RuntimeState == SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected
						&& ProofState->bDetachedFileWriteObserved
						&& ProofState->bRestoreObserved
						&& ProofState->bRestoreBackupObserved
						&& ProofState->bBuildLogObserved
						&& ProofState->bSeedHistoryVisible
						&& ProofState->bReattachNoticeVisible;

					FString ResumeError;
					if (bSuccess)
					{
						FOsvayderUERestartSurvivalManager::MarkReattachValidated(ResumeError);
						WriteRestartSurvivalProofJson(ProofState, TEXT("completed"));
						UE_LOG(
							LogOsvayderUE,
							Log,
							TEXT("OsvayderUE.FinalizeRestartSurvivalProof %s -> completed (same_provider_thread=%s)"),
							*ProofState->RunTag,
							bSameProviderThread ? TEXT("true") : TEXT("false"));
					}
					else
					{
						ProofState->ErrorText = ProofState->RuntimeDetail.IsEmpty()
							? TEXT("Restart-survival proof validation failed.")
							: ProofState->RuntimeDetail;
						if (!ResumeError.IsEmpty())
						{
							ProofState->ErrorText += TEXT(" ");
							ProofState->ErrorText += ResumeError;
						}
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.FinalizeRestartSurvivalProof %s -> failed"), *ProofState->RunTag);
					}

					FPlatformMisc::RequestExit(false);
					return false;
				}),
				0.25f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunRestartSurvivalRestoreIntentProofCommand(
		TEXT("OsvayderUE.RunRestartSurvivalRestoreIntentProof"),
		TEXT("Start a live ordinary restart-survival restore-intent proof. Usage: OsvayderUE.RunRestartSurvivalRestoreIntentProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunRestartSurvivalRestoreIntentProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("widget_restart_survival_action");
			ProofState->CommandDescription = TEXT("OsvayderUE.RunRestartSurvivalRestoreIntentProof / OsvayderUE.ObserveRestartSurvivalRestoreIntentProof");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();

			FString IgnoreError;
			FOsvayderUERestartSurvivalManager::DeleteState(IgnoreError);
			FOsvayderUERestartSurvivalManager::DeletePreparedRestoreRequest(IgnoreError);

			const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			if (BackendStatus.Backend != EOsvayderUEProviderBackend::CodexCli)
			{
				ProofState->ErrorText = TEXT("Restart-survival restore-intent proof is currently wired only for Codex CLI.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			if (!FCodexCliRunner::ShouldUsePersistentConversationTransport())
			{
				ProofState->ErrorText = TEXT("Restart-survival restore-intent proof requires persistent_app_server transport for Codex CLI.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			if (!AgentBackendCanExecutePrompt(BackendStatus))
			{
				ProofState->ErrorText = BackendStatus.Detail.IsEmpty()
					? TEXT("Configured backend is not ready for restart-survival restore-intent proof.")
					: BackendStatus.Detail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available for restart-survival restore-intent proof.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->WeakWidget = Widget;
			Widget->DebugClearInputText();
			Widget->DebugSetInputText(GetRestartSurvivalSeedPrompt());
			ProofState->bSeedPromptStarted = Widget->DebugSendCurrentInput();
			if (!ProofState->bSeedPromptStarted)
			{
				ProofState->RuntimeState = Widget->DebugGetLastRuntimeConnectionState();
				ProofState->RuntimeDetail = TEXT("Seed prompt dispatch did not start.");
				ProofState->ErrorText = ProofState->RuntimeDetail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			WriteRestartSurvivalProofJson(ProofState, TEXT("seed_pending"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable before detach.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for the restore-intent seed prompt to complete.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					if (ProofState->RuntimeState != SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected)
					{
						ProofState->ErrorText = ProofState->RuntimeDetail.IsEmpty()
							? TEXT("Seed prompt did not complete successfully.")
							: ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					FCodexCliRunner* CodexRunner = static_cast<FCodexCliRunner*>(FOsvayderCodeSubsystem::Get().GetActiveBackend());
					if (CodexRunner == nullptr)
					{
						ProofState->ErrorText = TEXT("Configured backend is not an active Codex CLI runner.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (!CodexRunner->ExportActiveThreadStateForRestartSurvival(ProofState->ProviderThreadStatePath, ProofState->ProviderSessionIdBefore))
					{
						ProofState->ErrorText = TEXT("Failed to export the active Codex thread state for restart-survival restore-intent proof.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					const FString ProofDir = FPaths::ConvertRelativePathToFull(GetRestartSurvivalProofDir(ProofState->RunTag));
					ProofState->DetachedFileTargetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProofDir, TEXT("detached_file_result.txt")));
					ProofState->DetachedFileExpectedText = TEXT("detached file write completed during editor downtime");
					ProofState->RestoreSourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
						FPaths::ProjectSavedDir(),
						TEXT("Autosaves"),
						TEXT("OsvayderUE"),
						TEXT("RestartSurvival"),
						ProofState->RunTag,
						TEXT("proof_restore_source.txt")));
					ProofState->RestoreTargetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
						FPaths::ProjectDir(),
						TEXT("Content"),
						TEXT("RestartSurvivalProof"),
						ProofState->RunTag + TEXT("_proof_asset.txt")));
					ProofState->RestoreBackupPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProofDir, TEXT("proof_restore_target.backup.txt")));
					ProofState->RestoreExpectedText = TEXT("autosave-backed restore payload");
					ProofState->BuildLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), ProofState->RunTag + TEXT("_restart_survival_build.log")));
					ProofState->DetachedLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), ProofState->RunTag + TEXT("_restart_survival_detached.log")));
					ProofState->PreparedRequestPath = FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath();
					ProofState->PreparedRequestId = FString::Printf(TEXT("request_%s"), *ProofState->RunTag);
					ProofState->CompletionPromptText = GetRestartSurvivalRestoreIntentCompletionPrompt();
					ProofState->ExpectedCompletionResponseText = GetRestartSurvivalRestoreIntentCompletionResponse();

					IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProofState->RestoreSourcePath), true);
					IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProofState->RestoreTargetPath), true);
					FFileHelper::SaveStringToFile(ProofState->RestoreExpectedText, *ProofState->RestoreSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					FFileHelper::SaveStringToFile(TEXT("pre_restore target content"), *ProofState->RestoreTargetPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

					FOsvayderUERestartSurvivalPreparedRestoreRequest Request;
					Request.RequestId = ProofState->PreparedRequestId;
					Request.TaskId = FString::Printf(TEXT("task_%s"), *ProofState->RunTag);
					Request.SessionId = FString::Printf(TEXT("restart_survival_%s"), *ProofState->RunTag);
					Request.Backend = ProofState->Backend;
					Request.LinkedProviderSessionId = ProofState->ProviderSessionIdBefore;
					Request.ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
					Request.AutosaveSourcePath = ProofState->RestoreSourcePath;
					Request.TargetPath = ProofState->RestoreTargetPath;
					Request.BackupPath = ProofState->RestoreBackupPath;
					Request.Detail = TEXT("explicit_autosave_backed_restore");
					Request.PostReattachCompletionText = ProofState->CompletionPromptText;

					FString RequestError;
					if (!FOsvayderUERestartSurvivalManager::SavePreparedRestoreRequest(Request, RequestError))
					{
						ProofState->ErrorText = RequestError;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}
					ProofState->bPreparedRequestWritten = true;

					FClaudeRestartSurvivalStartOptions StartOptions;
					StartOptions.AdditionalRelaunchArguments = FString::Printf(
						TEXT("-ExecCmds=\"OsvayderUE.ObserveRestartSurvivalRestoreIntentProof %s\""),
						*ProofState->RunTag);
					StartOptions.Proof.bEnabled = true;
					StartOptions.Proof.RunTag = ProofState->RunTag;
					StartOptions.Proof.ProofOutputPath = GetBackendRunOutputPath(ProofState->RunTag);
					StartOptions.Proof.DetachedLogPath = ProofState->DetachedLogPath;
					StartOptions.Proof.BuildLogPath = ProofState->BuildLogPath;
					StartOptions.Proof.DetachedFileTargetPath = ProofState->DetachedFileTargetPath;
					StartOptions.Proof.DetachedFileExpectedText = ProofState->DetachedFileExpectedText;
					StartOptions.Proof.RestoreExpectedText = ProofState->RestoreExpectedText;
					StartOptions.Proof.FinalizeCommand = FString::Printf(TEXT("OsvayderUE.ObserveRestartSurvivalRestoreIntentProof %s"), *ProofState->RunTag);

					FString StartError;
					if (!LiveWidget->StartRestartSurvivalForCurrentTask(StartOptions, StartError))
					{
						ProofState->ErrorText = StartError;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					WriteRestartSurvivalProofJson(ProofState, TEXT("detaching"));
					return false;
				}),
				0.2f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUEObserveRestartSurvivalRestoreIntentProofCommand(
		TEXT("OsvayderUE.ObserveRestartSurvivalRestoreIntentProof"),
		TEXT("Observe the live ordinary restart-survival restore-intent proof after relaunch. Usage: OsvayderUE.ObserveRestartSurvivalRestoreIntentProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.ObserveRestartSurvivalRestoreIntentProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("widget_restart_survival_action");
			ProofState->CommandDescription = TEXT("OsvayderUE.RunRestartSurvivalRestoreIntentProof / OsvayderUE.ObserveRestartSurvivalRestoreIntentProof");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();
			ProofState->PreparedRequestPath = FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath();
			ProofState->ExpectedCompletionResponseText = GetRestartSurvivalRestoreIntentCompletionResponse();

			FOsvayderUERestartSurvivalState RestartState;
			FString RestartError;
			if (!FOsvayderUERestartSurvivalManager::LoadState(RestartState, RestartError))
			{
				ProofState->ErrorText = RestartError;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			if (RestartState.Proof.RunTag != RunTag)
			{
				ProofState->ErrorText = FString::Printf(
					TEXT("Restart-survival state run_tag mismatch. Expected %s but found %s."),
					*RunTag,
					*RestartState.Proof.RunTag);
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->ProviderSessionIdBefore = RestartState.ProviderSessionId;
			ProofState->ProviderThreadStatePath = RestartState.ProviderThreadStatePath;
			ProofState->DetachedFileTargetPath = RestartState.Proof.DetachedFileTargetPath;
			ProofState->DetachedFileExpectedText = RestartState.Proof.DetachedFileExpectedText;
			ProofState->RestoreSourcePath = RestartState.RestoreIntent.AutosaveSourcePath;
			ProofState->RestoreTargetPath = RestartState.RestoreIntent.TargetPath;
			ProofState->RestoreBackupPath = RestartState.RestoreIntent.BackupPath;
			ProofState->RestoreExpectedText = RestartState.Proof.RestoreExpectedText;
			ProofState->BuildLogPath = RestartState.Proof.BuildLogPath;
			ProofState->DetachedLogPath = RestartState.Proof.DetachedLogPath;
			ProofState->ReattachNotice = RestartState.ReattachNotice;
			ProofState->PreparedRequestId = RestartState.PreparedRestoreRequestId;
			ProofState->CompletionPromptText = RestartState.PostReattachCompletionText;

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available after relaunch.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->WeakWidget = Widget;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState, RunTag](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable during restore-intent proof observation.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for restart-survival restore-intent observation.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					ProofState->DetachedFileObservedText = LoadTextFileIfExists(ProofState->DetachedFileTargetPath).TrimStartAndEnd();
					ProofState->RestoreTargetObservedText = LoadTextFileIfExists(ProofState->RestoreTargetPath).TrimStartAndEnd();
					ProofState->RestoreBackupObservedText = LoadTextFileIfExists(ProofState->RestoreBackupPath).TrimStartAndEnd();
					ProofState->bDetachedFileWriteObserved = ProofState->DetachedFileObservedText == ProofState->DetachedFileExpectedText;
					ProofState->bRestoreObserved = ProofState->RestoreTargetObservedText == ProofState->RestoreExpectedText;
					ProofState->bRestoreBackupObserved = !ProofState->RestoreBackupObservedText.IsEmpty();
					ProofState->bBuildLogObserved = !LoadTextFileIfExists(ProofState->BuildLogPath).IsEmpty();
					ProofState->bReattachNoticeVisible = !ProofState->ReattachNotice.IsEmpty() && LiveWidget->DebugVisibleChatContains(ProofState->ReattachNotice);
					ProofState->bSeedHistoryVisible = LiveWidget->DebugVisibleChatContains(GetRestartSurvivalSeedPrompt());
					ProofState->ObservedCompletionResponseText = LiveWidget->DebugGetLastResponseText();
					ProofState->bCompletionResponseMatched = ProofState->ObservedCompletionResponseText == ProofState->ExpectedCompletionResponseText;
					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					ProofState->bPreparedRequestConsumed = !FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest();

					FOsvayderUERestartSurvivalState CurrentState;
					FString CurrentStateError;
					if (!FOsvayderUERestartSurvivalManager::LoadState(CurrentState, CurrentStateError))
					{
						ProofState->ErrorText = CurrentStateError;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (CurrentState.Proof.RunTag != RunTag)
					{
						ProofState->ErrorText = TEXT("Observed restart-survival state no longer matches the proof run.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (CurrentState.bPostReattachCompletionDispatched)
					{
						ProofState->bResumePromptStarted = true;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					const bool bSuccess =
						ProofState->RuntimeState == SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected
						&& ProofState->bDetachedFileWriteObserved
						&& ProofState->bRestoreObserved
						&& ProofState->bRestoreBackupObserved
						&& ProofState->bBuildLogObserved
						&& ProofState->bPreparedRequestConsumed
						&& ProofState->bReattachNoticeVisible
						&& ProofState->bSeedHistoryVisible
						&& ProofState->bCompletionResponseMatched
						&& CurrentState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
						&& !CurrentState.bProviderThreadResumePending
						&& CurrentState.TaskId == FString::Printf(TEXT("task_%s"), *RunTag)
						&& CurrentState.SessionId == FString::Printf(TEXT("restart_survival_%s"), *RunTag);

					if (bSuccess)
					{
						FString TraceRunId;
						const TArray<TSharedPtr<FJsonObject>> TraceRecords = QueryLatestTraceRecordsForBackend(ProofState->Backend, TraceRunId);
						ProofState->ProviderSessionIdAfter = FindProviderSessionIdInTraceRecords(TraceRecords);
						WriteRestartSurvivalProofJson(ProofState, TEXT("completed"));
					}
					else if (CurrentState.Phase == EOsvayderUERestartSurvivalPhase::FailedTerminal)
					{
						ProofState->ErrorText = CurrentState.PhaseDetail.IsEmpty()
							? TEXT("Restart-survival restore-intent proof reached FailedTerminal.")
							: CurrentState.PhaseDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
					}
					else
					{
						return true;
					}

					FPlatformMisc::RequestExit(false);
					return false;
				}),
				0.25f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunRestartSurvivalRestoreIntentNegativeProofCommand(
		TEXT("OsvayderUE.RunRestartSurvivalRestoreIntentNegativeProof"),
		TEXT("Run a live negative proof for invalid prepared restart-survival restore requests. Usage: OsvayderUE.RunRestartSurvivalRestoreIntentNegativeProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunRestartSurvivalRestoreIntentNegativeProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("widget_restart_survival_action");
			ProofState->CommandDescription = TEXT("OsvayderUE.RunRestartSurvivalRestoreIntentNegativeProof");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();

			FString IgnoreError;
			FOsvayderUERestartSurvivalManager::DeleteState(IgnoreError);
			FOsvayderUERestartSurvivalManager::DeletePreparedRestoreRequest(IgnoreError);

			const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			if (BackendStatus.Backend != EOsvayderUEProviderBackend::CodexCli || !FCodexCliRunner::ShouldUsePersistentConversationTransport())
			{
				ProofState->ErrorText = TEXT("Negative restore-intent proof requires the ordinary persistent Codex runtime path.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			if (!AgentBackendCanExecutePrompt(BackendStatus))
			{
				ProofState->ErrorText = BackendStatus.Detail.IsEmpty()
					? TEXT("Configured backend is not ready for negative restore-intent proof.")
					: BackendStatus.Detail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available for negative restore-intent proof.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->WeakWidget = Widget;
			Widget->DebugClearInputText();
			Widget->DebugSetInputText(GetRestartSurvivalSeedPrompt());
			ProofState->bSeedPromptStarted = Widget->DebugSendCurrentInput();
			if (!ProofState->bSeedPromptStarted)
			{
				ProofState->RuntimeState = Widget->DebugGetLastRuntimeConnectionState();
				ProofState->RuntimeDetail = TEXT("Seed prompt dispatch did not start.");
				ProofState->ErrorText = ProofState->RuntimeDetail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			WriteRestartSurvivalProofJson(ProofState, TEXT("seed_pending"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable before negative proof validation.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for the negative restore-intent seed prompt to complete.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					if (ProofState->RuntimeState != SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected)
					{
						ProofState->ErrorText = ProofState->RuntimeDetail.IsEmpty()
							? TEXT("Seed prompt did not complete successfully.")
							: ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					FCodexCliRunner* CodexRunner = static_cast<FCodexCliRunner*>(FOsvayderCodeSubsystem::Get().GetActiveBackend());
					if (CodexRunner == nullptr)
					{
						ProofState->ErrorText = TEXT("Configured backend is not an active Codex CLI runner.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (!CodexRunner->ExportActiveThreadStateForRestartSurvival(ProofState->ProviderThreadStatePath, ProofState->ProviderSessionIdBefore))
					{
						ProofState->ErrorText = TEXT("Failed to export the active Codex thread state for negative restore-intent proof.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					ProofState->PreparedRequestPath = FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath();
					ProofState->PreparedRequestId = FString::Printf(TEXT("request_negative_%s"), *ProofState->RunTag);
					ProofState->RestoreTargetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
						FPaths::ProjectDir(),
						TEXT("Content"),
						TEXT("RestartSurvivalProof"),
						ProofState->RunTag + TEXT("_negative_asset.txt")));
					ProofState->RestoreBackupPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
						FPaths::ProjectSavedDir(),
						TEXT("OsvayderUE"),
						TEXT("restart_survival"),
						ProofState->RunTag,
						TEXT("negative_restore_target.backup.txt")));
					ProofState->RestoreSourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
						FPaths::ProjectSavedDir(),
						TEXT("Autosaves"),
						TEXT("OsvayderUE"),
						TEXT("RestartSurvival"),
						ProofState->RunTag,
						TEXT("missing_restore_source.txt")));

					FOsvayderUERestartSurvivalPreparedRestoreRequest Request;
					Request.RequestId = ProofState->PreparedRequestId;
					Request.TaskId = FString::Printf(TEXT("task_negative_%s"), *ProofState->RunTag);
					Request.SessionId = FString::Printf(TEXT("restart_survival_negative_%s"), *ProofState->RunTag);
					Request.Backend = ProofState->Backend;
					Request.LinkedProviderSessionId = ProofState->ProviderSessionIdBefore;
					Request.ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
					Request.AutosaveSourcePath = ProofState->RestoreSourcePath;
					Request.TargetPath = ProofState->RestoreTargetPath;
					Request.BackupPath = ProofState->RestoreBackupPath;
					Request.Detail = TEXT("explicit_autosave_backed_restore");
					Request.CreatedAtUtc = FDateTime::UtcNow().ToIso8601();

					FString RequestError;
					if (!FOsvayderUERestartSurvivalManager::SavePreparedRestoreRequest(Request, RequestError))
					{
						ProofState->ErrorText = RequestError;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}
					ProofState->bPreparedRequestWritten = true;

					FClaudeRestartSurvivalStartOptions StartOptions;
					FString StartError;
					const bool bStarted = LiveWidget->StartRestartSurvivalForCurrentTask(StartOptions, StartError);
					ProofState->bPreparedRequestConsumed = !FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest();

					if (bStarted)
					{
						ProofState->ErrorText = TEXT("Invalid prepared restore request incorrectly started restart survival.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					FOsvayderUERestartSurvivalState StateAfterReject;
					const bool bStatePresentAfterReject = FOsvayderUERestartSurvivalManager::DescribeCurrentState(StateAfterReject);
					if (bStatePresentAfterReject && StateAfterReject.Phase == EOsvayderUERestartSurvivalPhase::Detaching)
					{
						ProofState->ErrorText = TEXT("Invalid prepared restore request incorrectly advanced restart survival into Detaching.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					ProofState->ErrorText = StartError;
					WriteRestartSurvivalProofJson(ProofState, TEXT("rejected_invalid_request"));
					return false;
				}),
				0.2f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunRestartSurvivalTaskContinuationProofCommand(
		TEXT("OsvayderUE.RunRestartSurvivalTaskContinuationProof"),
		TEXT("Start a live ordinary task-driven restart-survival continuation proof. Usage: OsvayderUE.RunRestartSurvivalTaskContinuationProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunRestartSurvivalTaskContinuationProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("task_driven_structured_handoff");
			ProofState->CommandDescription = TEXT("OsvayderUE.RunRestartSurvivalTaskContinuationProof / OsvayderUE.ObserveRestartSurvivalTaskContinuationProof");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();

			FString IgnoreError;
			FOsvayderUERestartSurvivalManager::DeleteState(IgnoreError);
			FOsvayderUERestartSurvivalManager::DeletePreparedRestoreRequest(IgnoreError);
			FOsvayderUERestartSurvivalManager::ClearPreparedRequestStartOverride();

			const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			if (BackendStatus.Backend != EOsvayderUEProviderBackend::CodexCli)
			{
				ProofState->ErrorText = TEXT("Task-continuation proof is currently wired only for Codex CLI.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			if (!FCodexCliRunner::ShouldUsePersistentConversationTransport())
			{
				ProofState->ErrorText = TEXT("Task-continuation proof requires persistent_app_server transport for Codex CLI.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			if (!AgentBackendCanExecutePrompt(BackendStatus))
			{
				ProofState->ErrorText = BackendStatus.Detail.IsEmpty()
					? TEXT("Configured backend is not ready for task-continuation proof.")
					: BackendStatus.Detail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available for task-continuation proof.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			const FString TaskId = FString::Printf(TEXT("task_%s"), *RunTag);
			const FString SessionId = FString::Printf(TEXT("restart_survival_%s"), *RunTag);
			const FString ProofDir = FPaths::ConvertRelativePathToFull(GetRestartSurvivalProofDir(RunTag));
			ProofState->DetachedFileTargetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProofDir, TEXT("detached_file_result.txt")));
			ProofState->DetachedFileExpectedText = TEXT("detached file write completed during editor downtime");
			ProofState->RestoreSourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("Autosaves"),
				TEXT("OsvayderUE"),
				TEXT("RestartSurvival"),
				RunTag,
				TEXT("task_continuation_restore_source.txt")));
			ProofState->RestoreTargetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Content"),
				TEXT("RestartSurvivalProof"),
				RunTag + TEXT("_task_continuation_asset.txt")));
			ProofState->RestoreBackupPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProofDir, TEXT("task_continuation_restore_target.backup.txt")));
			ProofState->RestoreExpectedText = TEXT("autosave-backed restore payload");
			ProofState->BuildLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), RunTag + TEXT("_task_continuation_restart_survival_build.log")));
			ProofState->DetachedLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), RunTag + TEXT("_task_continuation_restart_survival_detached.log")));
			ProofState->PreparedRequestPath = FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath();
			ProofState->CompletionPromptText = GetRestartSurvivalTaskContinuationCompletionPrompt();
			ProofState->ExpectedCompletionResponseText = GetRestartSurvivalTaskContinuationCompletionResponse();
			ProofState->ExpectedSeedResponseText = GetRestartSurvivalTaskContinuationSeedResponse();
			ProofState->SeedPromptText = BuildRestartSurvivalTaskContinuationSeedPrompt(
				TaskId,
				SessionId,
				ProofState->RestoreSourcePath,
				ProofState->RestoreTargetPath,
				ProofState->RestoreBackupPath);

			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProofState->RestoreSourcePath), true);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProofState->RestoreTargetPath), true);
			FFileHelper::SaveStringToFile(ProofState->RestoreExpectedText, *ProofState->RestoreSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			FFileHelper::SaveStringToFile(TEXT("pre_restore target content"), *ProofState->RestoreTargetPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

			FOsvayderUERestartSurvivalPreparedStartOverride StartOverride;
			StartOverride.TaskId = TaskId;
			StartOverride.SessionId = SessionId;
			StartOverride.ReattachTokenOverride = FString::Printf(TEXT("reattach_%s"), *RunTag);
			StartOverride.ReattachNoticeOverride = FString::Printf(
				TEXT("Restart-survival reattached to task %s. Detached file/build/recovery work completed while the editor was closed."),
				*TaskId);
			StartOverride.AdditionalRelaunchArguments = FString::Printf(
				TEXT("-ExecCmds=\"OsvayderUE.ObserveRestartSurvivalTaskContinuationProof %s\""),
				*RunTag);
			StartOverride.Proof.bEnabled = true;
			StartOverride.Proof.RunTag = RunTag;
			StartOverride.Proof.ProofOutputPath = GetBackendRunOutputPath(RunTag);
			StartOverride.Proof.DetachedLogPath = ProofState->DetachedLogPath;
			StartOverride.Proof.BuildLogPath = ProofState->BuildLogPath;
			StartOverride.Proof.DetachedFileTargetPath = ProofState->DetachedFileTargetPath;
			StartOverride.Proof.DetachedFileExpectedText = ProofState->DetachedFileExpectedText;
			StartOverride.Proof.RestoreExpectedText = ProofState->RestoreExpectedText;
			StartOverride.Proof.FinalizeCommand = FString::Printf(TEXT("OsvayderUE.ObserveRestartSurvivalTaskContinuationProof %s"), *RunTag);
			FOsvayderUERestartSurvivalManager::ArmPreparedRequestStartOverride(StartOverride);

			ProofState->WeakWidget = Widget;
			Widget->DebugClearInputText();
			Widget->DebugSetInputText(ProofState->SeedPromptText);
			ProofState->bSeedPromptStarted = Widget->DebugSendCurrentInput();
			if (!ProofState->bSeedPromptStarted)
			{
				FOsvayderUERestartSurvivalManager::ClearPreparedRequestStartOverride();
				ProofState->RuntimeState = Widget->DebugGetLastRuntimeConnectionState();
				ProofState->RuntimeDetail = TEXT("Task-continuation seed prompt dispatch did not start.");
				ProofState->ErrorText = ProofState->RuntimeDetail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			WriteRestartSurvivalProofJson(ProofState, TEXT("seed_pending"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState, TaskId, SessionId](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable before task-driven detach.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						FOsvayderUERestartSurvivalManager::ClearPreparedRequestStartOverride();
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for the task-driven handoff prompt to complete.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					ProofState->ObservedSeedResponseText = LiveWidget->DebugGetLastResponseText();
					ProofState->bSeedResponseMatched = ProofState->ObservedSeedResponseText.Contains(
						ProofState->ExpectedSeedResponseText,
						ESearchCase::CaseSensitive);
					if (ProofState->RuntimeState != SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected)
					{
						ProofState->ErrorText = ProofState->RuntimeDetail.IsEmpty()
							? TEXT("Task-driven handoff prompt did not complete successfully.")
							: ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					FOsvayderUERestartSurvivalState RestartState;
					FString RestartError;
					if (!FOsvayderUERestartSurvivalManager::LoadState(RestartState, RestartError))
					{
						return true;
					}

					if (RestartState.Proof.RunTag != ProofState->RunTag)
					{
						ProofState->ErrorText = TEXT("Observed restart-survival state does not match the task-continuation proof run.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					ProofState->ProviderSessionIdBefore = RestartState.ProviderSessionId;
					ProofState->ProviderThreadStatePath = RestartState.ProviderThreadStatePath;
					ProofState->PreparedRequestId = RestartState.PreparedRestoreRequestId;
					ProofState->bPreparedRequestWritten = !RestartState.PreparedRestoreRequestId.IsEmpty();
					ProofState->bPreparedRequestConsumed = !FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest();

					const bool bDetachingOrBeyond =
						RestartState.Phase == EOsvayderUERestartSurvivalPhase::Detaching
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::DetachedRunning
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingRelaunch
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::Relaunching
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingReattach
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::Reattached;

					if (!bDetachingOrBeyond)
					{
						if (RestartState.Phase == EOsvayderUERestartSurvivalPhase::FailedTerminal)
						{
							ProofState->ErrorText = RestartState.PhaseDetail.IsEmpty()
								? TEXT("Task-driven handoff reached FailedTerminal.")
								: RestartState.PhaseDetail;
							WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
							return false;
						}

						return true;
					}

					const bool bSuccess =
						ProofState->bSeedResponseMatched
						&& RestartState.TaskId == TaskId
						&& RestartState.SessionId == SessionId
						&& RestartState.PostReattachCompletionText == ProofState->CompletionPromptText
						&& !RestartState.PreparedRestoreRequestId.IsEmpty()
						&& RestartState.RestoreIntent.bEnabled;

					if (!bSuccess)
					{
						ProofState->ErrorText = TEXT("Task-driven handoff did not preserve the expected continuation or restore state.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					WriteRestartSurvivalProofJson(ProofState, TEXT("detaching"));
					return false;
				}),
				0.2f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUEObserveRestartSurvivalTaskContinuationProofCommand(
		TEXT("OsvayderUE.ObserveRestartSurvivalTaskContinuationProof"),
		TEXT("Observe the live ordinary task-driven restart-survival continuation proof after relaunch. Usage: OsvayderUE.ObserveRestartSurvivalTaskContinuationProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.ObserveRestartSurvivalTaskContinuationProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("task_driven_structured_handoff");
			ProofState->CommandDescription = TEXT("OsvayderUE.RunRestartSurvivalTaskContinuationProof / OsvayderUE.ObserveRestartSurvivalTaskContinuationProof");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();
			ProofState->PreparedRequestPath = FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath();
			ProofState->ExpectedCompletionResponseText = GetRestartSurvivalTaskContinuationCompletionResponse();

			FOsvayderUERestartSurvivalState RestartState;
			FString RestartError;
			if (!FOsvayderUERestartSurvivalManager::LoadState(RestartState, RestartError))
			{
				ProofState->ErrorText = RestartError;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			if (RestartState.Proof.RunTag != RunTag)
			{
				ProofState->ErrorText = FString::Printf(
					TEXT("Restart-survival state run_tag mismatch. Expected %s but found %s."),
					*RunTag,
					*RestartState.Proof.RunTag);
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->ProviderSessionIdBefore = RestartState.ProviderSessionId;
			ProofState->ProviderThreadStatePath = RestartState.ProviderThreadStatePath;
			ProofState->DetachedFileTargetPath = RestartState.Proof.DetachedFileTargetPath;
			ProofState->DetachedFileExpectedText = RestartState.Proof.DetachedFileExpectedText;
			ProofState->RestoreSourcePath = RestartState.RestoreIntent.AutosaveSourcePath;
			ProofState->RestoreTargetPath = RestartState.RestoreIntent.TargetPath;
			ProofState->RestoreBackupPath = RestartState.RestoreIntent.BackupPath;
			ProofState->RestoreExpectedText = RestartState.Proof.RestoreExpectedText;
			ProofState->BuildLogPath = RestartState.Proof.BuildLogPath;
			ProofState->DetachedLogPath = RestartState.Proof.DetachedLogPath;
			ProofState->ReattachNotice = RestartState.ReattachNotice;
			ProofState->PreparedRequestId = RestartState.PreparedRestoreRequestId;
			ProofState->CompletionPromptText = RestartState.PostReattachCompletionText;
			ProofState->ExpectedSeedResponseText = GetRestartSurvivalTaskContinuationSeedResponse();
			ProofState->SeedPromptText = BuildRestartSurvivalTaskContinuationSeedPrompt(
				RestartState.TaskId,
				RestartState.SessionId,
				ProofState->RestoreSourcePath,
				ProofState->RestoreTargetPath,
				ProofState->RestoreBackupPath);
			CarryForwardRestartSurvivalProofSeedState(ProofState);

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available after relaunch.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->WeakWidget = Widget;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState, RunTag](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable during task-continuation proof observation.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for task-continuation proof observation.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					ProofState->DetachedFileObservedText = LoadTextFileIfExists(ProofState->DetachedFileTargetPath).TrimStartAndEnd();
					ProofState->RestoreTargetObservedText = LoadTextFileIfExists(ProofState->RestoreTargetPath).TrimStartAndEnd();
					ProofState->RestoreBackupObservedText = LoadTextFileIfExists(ProofState->RestoreBackupPath).TrimStartAndEnd();
					ProofState->bDetachedFileWriteObserved = ProofState->DetachedFileObservedText == ProofState->DetachedFileExpectedText;
					ProofState->bRestoreObserved = ProofState->RestoreTargetObservedText == ProofState->RestoreExpectedText;
					ProofState->bRestoreBackupObserved = !ProofState->RestoreBackupObservedText.IsEmpty();
					ProofState->bBuildLogObserved = !LoadTextFileIfExists(ProofState->BuildLogPath).IsEmpty();
					ProofState->bReattachNoticeVisible = !ProofState->ReattachNotice.IsEmpty() && LiveWidget->DebugVisibleChatContains(ProofState->ReattachNotice);
					ProofState->bSeedHistoryVisible = LiveWidget->DebugVisibleChatContains(ProofState->SeedPromptText);
					ProofState->ObservedCompletionResponseText = LiveWidget->DebugGetLastResponseText();
					ProofState->bCompletionResponseMatched = ProofState->ObservedCompletionResponseText.Contains(
						ProofState->ExpectedCompletionResponseText,
						ESearchCase::CaseSensitive);
					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					ProofState->bPreparedRequestConsumed = !FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest();

					FOsvayderUERestartSurvivalState CurrentState;
					FString CurrentStateError;
					if (!FOsvayderUERestartSurvivalManager::LoadState(CurrentState, CurrentStateError))
					{
						ProofState->ErrorText = CurrentStateError;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (CurrentState.Proof.RunTag != RunTag)
					{
						ProofState->ErrorText = TEXT("Observed restart-survival state no longer matches the task-continuation proof run.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (CurrentState.bPostReattachCompletionDispatched)
					{
						ProofState->bResumePromptStarted = true;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					const bool bSuccess =
						ProofState->RuntimeState == SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected
						&& ProofState->bDetachedFileWriteObserved
						&& ProofState->bRestoreObserved
						&& ProofState->bRestoreBackupObserved
						&& ProofState->bBuildLogObserved
						&& ProofState->bPreparedRequestConsumed
						&& ProofState->bReattachNoticeVisible
						&& ProofState->bSeedHistoryVisible
						&& ProofState->bCompletionResponseMatched
						&& CurrentState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
						&& !CurrentState.bProviderThreadResumePending
						&& CurrentState.TaskId == FString::Printf(TEXT("task_%s"), *RunTag)
						&& CurrentState.SessionId == FString::Printf(TEXT("restart_survival_%s"), *RunTag);

					if (bSuccess)
					{
						FString TraceRunId;
						const TArray<TSharedPtr<FJsonObject>> TraceRecords = QueryLatestTraceRecordsForBackend(ProofState->Backend, TraceRunId);
						ProofState->ProviderSessionIdAfter = FindProviderSessionIdInTraceRecords(TraceRecords);
						WriteRestartSurvivalProofJson(ProofState, TEXT("completed"));
					}
					else if (CurrentState.Phase == EOsvayderUERestartSurvivalPhase::FailedTerminal)
					{
						ProofState->ErrorText = CurrentState.PhaseDetail.IsEmpty()
							? TEXT("Task-continuation proof reached FailedTerminal.")
							: CurrentState.PhaseDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
					}
					else
					{
						return true;
					}

					FPlatformMisc::RequestExit(false);
					return false;
				}),
				0.25f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunRestartSurvivalTaskContinuationNegativeProofCommand(
		TEXT("OsvayderUE.RunRestartSurvivalTaskContinuationNegativeProof"),
		TEXT("Run a live negative proof for malformed or unlinked task-driven restart-survival handoffs. Usage: OsvayderUE.RunRestartSurvivalTaskContinuationNegativeProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunRestartSurvivalTaskContinuationNegativeProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("task_driven_structured_handoff");
			ProofState->CommandDescription = TEXT("OsvayderUE.RunRestartSurvivalTaskContinuationNegativeProof");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();
			ProofState->ExpectedSeedResponseText = TEXT("restart survival task continuation negative finished");

			FString IgnoreError;
			FOsvayderUERestartSurvivalManager::DeleteState(IgnoreError);
			FOsvayderUERestartSurvivalManager::DeletePreparedRestoreRequest(IgnoreError);
			FOsvayderUERestartSurvivalManager::ClearPreparedRequestStartOverride();

			const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			if (BackendStatus.Backend != EOsvayderUEProviderBackend::CodexCli || !FCodexCliRunner::ShouldUsePersistentConversationTransport())
			{
				ProofState->ErrorText = TEXT("Negative task-continuation proof requires the ordinary persistent Codex runtime path.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			if (!AgentBackendCanExecutePrompt(BackendStatus))
			{
				ProofState->ErrorText = BackendStatus.Detail.IsEmpty()
					? TEXT("Configured backend is not ready for negative task-continuation proof.")
					: BackendStatus.Detail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available for negative task-continuation proof.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->SeedPromptText = BuildRestartSurvivalTaskContinuationNegativePrompt(
				FString::Printf(TEXT("task_negative_%s"), *RunTag),
				FString::Printf(TEXT("restart_survival_negative_%s"), *RunTag),
				FString::Printf(TEXT("provider_wrong_%s"), *RunTag));

			ProofState->WeakWidget = Widget;
			Widget->DebugClearInputText();
			Widget->DebugSetInputText(ProofState->SeedPromptText);
			ProofState->bSeedPromptStarted = Widget->DebugSendCurrentInput();
			if (!ProofState->bSeedPromptStarted)
			{
				ProofState->RuntimeState = Widget->DebugGetLastRuntimeConnectionState();
				ProofState->RuntimeDetail = TEXT("Negative task-continuation prompt dispatch did not start.");
				ProofState->ErrorText = ProofState->RuntimeDetail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			WriteRestartSurvivalProofJson(ProofState, TEXT("seed_pending"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable before negative task-continuation validation.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for the negative task-continuation prompt to complete.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					ProofState->ObservedSeedResponseText = LiveWidget->DebugGetLastResponseText();
					ProofState->bSeedResponseMatched = ProofState->ObservedSeedResponseText.Contains(
						ProofState->ExpectedSeedResponseText,
						ESearchCase::CaseSensitive);
					ProofState->bPreparedRequestConsumed = !FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest();

					FOsvayderUERestartSurvivalState CurrentState;
					const bool bStatePresent = FOsvayderUERestartSurvivalManager::DescribeCurrentState(CurrentState);
					if (bStatePresent)
					{
						const bool bInvalidStateAdvance =
							CurrentState.Phase == EOsvayderUERestartSurvivalPhase::Detaching
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::DetachedRunning
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingRelaunch
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::Relaunching
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingReattach
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::Reattached;
						if (bInvalidStateAdvance)
						{
							ProofState->ErrorText = TEXT("Malformed task-driven handoff incorrectly advanced restart survival.");
							WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
							return false;
						}
					}

					if (ProofState->RuntimeState != SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected)
					{
						ProofState->ErrorText = ProofState->RuntimeDetail.IsEmpty()
							? TEXT("Negative task-continuation prompt did not complete successfully.")
							: ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (!ProofState->bPreparedRequestConsumed)
					{
						ProofState->ErrorText = TEXT("Malformed task-driven handoff incorrectly left a prepared request behind.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					WriteRestartSurvivalProofJson(ProofState, TEXT("rejected_invalid_request"));
					return false;
				}),
				0.2f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunRestartSurvivalAutonomousClosedEditorEscalationProofCommand(
		TEXT("OsvayderUE.RunRestartSurvivalAutonomousClosedEditorEscalationProof"),
		TEXT("Start a live ordinary natural-language restart-survival autonomous closed-editor escalation proof. Usage: OsvayderUE.RunRestartSurvivalAutonomousClosedEditorEscalationProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunRestartSurvivalAutonomousClosedEditorEscalationProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("autonomous_closed_editor_escalation");
			ProofState->CommandDescription = TEXT("OsvayderUE.RunRestartSurvivalAutonomousClosedEditorEscalationProof / OsvayderUE.ObserveRestartSurvivalAutonomousClosedEditorEscalationProof");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();
			ProofState->TransitionNoticeText = GetRestartSurvivalClosedEditorTransitionNoticePrefix();

			FString IgnoreError;
			FOsvayderUERestartSurvivalManager::DeleteState(IgnoreError);
			FOsvayderUERestartSurvivalManager::DeletePreparedRestoreRequest(IgnoreError);
			FOsvayderUERestartSurvivalManager::ClearPreparedRequestStartOverride();

			const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			if (BackendStatus.Backend != EOsvayderUEProviderBackend::CodexCli)
			{
				ProofState->ErrorText = TEXT("Autonomous closed-editor escalation proof is currently wired only for Codex CLI.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			if (!FCodexCliRunner::ShouldUsePersistentConversationTransport())
			{
				ProofState->ErrorText = TEXT("Autonomous closed-editor escalation proof requires persistent_app_server transport for Codex CLI.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			if (!AgentBackendCanExecutePrompt(BackendStatus))
			{
				ProofState->ErrorText = BackendStatus.Detail.IsEmpty()
					? TEXT("Configured backend is not ready for autonomous closed-editor escalation proof.")
					: BackendStatus.Detail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available for autonomous closed-editor escalation proof.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			const FString ProofDir = FPaths::ConvertRelativePathToFull(GetRestartSurvivalProofDir(RunTag));
			ProofState->DetachedFileTargetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProofDir, TEXT("autonomous_closed_editor_detached_file_result.txt")));
			ProofState->DetachedFileExpectedText = TEXT("autonomous closed-editor detached work completed");
			ProofState->RestoreSourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("Autosaves"),
				TEXT("OsvayderUE"),
				TEXT("RestartSurvival"),
				RunTag,
				TEXT("autonomous_closed_editor_restore_source.txt")));
			ProofState->RestoreTargetPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Content"),
				TEXT("RestartSurvivalProof"),
				RunTag + TEXT("_autonomous_closed_editor_asset.txt")));
			ProofState->RestoreBackupPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProofDir, TEXT("autonomous_closed_editor_restore_target.backup.txt")));
			ProofState->RestoreExpectedText = TEXT("autonomous closed-editor autosave-backed restore payload");
			ProofState->BuildLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), RunTag + TEXT("_autonomous_closed_editor_restart_survival_build.log")));
			ProofState->DetachedLogPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), RunTag + TEXT("_autonomous_closed_editor_restart_survival_detached.log")));
			ProofState->PreparedRequestPath = FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath();
			ProofState->CompletionPromptText = GetRestartSurvivalAutonomousClosedEditorCompletionPrompt();
			ProofState->ExpectedCompletionResponseText = GetRestartSurvivalAutonomousClosedEditorCompletionResponse();
			ProofState->ExpectedSeedResponseText = GetRestartSurvivalAutonomousClosedEditorSeedResponse();
			ProofState->SeedPromptText = BuildRestartSurvivalAutonomousClosedEditorSeedPrompt(
				ProofState->RestoreSourcePath,
				ProofState->RestoreTargetPath,
				ProofState->RestoreBackupPath);

			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProofState->RestoreSourcePath), true);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProofState->RestoreTargetPath), true);
			FFileHelper::SaveStringToFile(ProofState->RestoreExpectedText, *ProofState->RestoreSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			FFileHelper::SaveStringToFile(TEXT("pre_restore target content"), *ProofState->RestoreTargetPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

			FOsvayderUERestartSurvivalPreparedStartOverride StartOverride;
			StartOverride.ReattachTokenOverride = FString::Printf(TEXT("reattach_%s"), *RunTag);
			StartOverride.ReattachNoticeOverride = TEXT("Restart-survival reattached to the same autonomous closed-editor task. Bounded detached restore/build work completed while the editor was closed.");
			StartOverride.AdditionalRelaunchArguments = FString::Printf(
				TEXT("-ExecCmds=\"OsvayderUE.ObserveRestartSurvivalAutonomousClosedEditorEscalationProof %s\""),
				*RunTag);
			StartOverride.Proof.bEnabled = true;
			StartOverride.Proof.RunTag = RunTag;
			StartOverride.Proof.ProofOutputPath = GetBackendRunOutputPath(RunTag);
			StartOverride.Proof.DetachedLogPath = ProofState->DetachedLogPath;
			StartOverride.Proof.BuildLogPath = ProofState->BuildLogPath;
			StartOverride.Proof.DetachedFileTargetPath = ProofState->DetachedFileTargetPath;
			StartOverride.Proof.DetachedFileExpectedText = ProofState->DetachedFileExpectedText;
			StartOverride.Proof.RestoreExpectedText = ProofState->RestoreExpectedText;
			StartOverride.Proof.FinalizeCommand = FString::Printf(TEXT("OsvayderUE.ObserveRestartSurvivalAutonomousClosedEditorEscalationProof %s"), *RunTag);
			FOsvayderUERestartSurvivalManager::ArmPreparedRequestStartOverride(StartOverride);

			ProofState->WeakWidget = Widget;
			Widget->DebugClearInputText();
			Widget->DebugSetInputText(ProofState->SeedPromptText);
			ProofState->bSeedPromptStarted = Widget->DebugSendCurrentInput();
			if (!ProofState->bSeedPromptStarted)
			{
				FOsvayderUERestartSurvivalManager::ClearPreparedRequestStartOverride();
				ProofState->RuntimeState = Widget->DebugGetLastRuntimeConnectionState();
				ProofState->RuntimeDetail = TEXT("Autonomous closed-editor escalation seed prompt dispatch did not start.");
				ProofState->ErrorText = ProofState->RuntimeDetail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			WriteRestartSurvivalProofJson(ProofState, TEXT("seed_pending"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable before autonomous closed-editor detach.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						FOsvayderUERestartSurvivalManager::ClearPreparedRequestStartOverride();
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for the autonomous closed-editor handoff prompt to complete.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					ProofState->ObservedSeedResponseText = LiveWidget->DebugGetLastResponseText();
					ProofState->bSeedResponseMatched = ProofState->ObservedSeedResponseText == ProofState->ExpectedSeedResponseText;
					ProofState->bTransitionNoticeVisible = LiveWidget->DebugVisibleChatContains(ProofState->TransitionNoticeText);
					if (ProofState->RuntimeState != SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected)
					{
						ProofState->ErrorText = ProofState->RuntimeDetail.IsEmpty()
							? TEXT("Autonomous closed-editor handoff prompt did not complete successfully.")
							: ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					FOsvayderUERestartSurvivalState RestartState;
					FString RestartError;
					if (!FOsvayderUERestartSurvivalManager::LoadState(RestartState, RestartError))
					{
						return true;
					}

					if (RestartState.Proof.RunTag != ProofState->RunTag)
					{
						ProofState->ErrorText = TEXT("Observed restart-survival state does not match the autonomous closed-editor proof run.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					ProofState->ProviderSessionIdBefore = RestartState.ProviderSessionId;
					ProofState->ProviderThreadStatePath = RestartState.ProviderThreadStatePath;
					ProofState->PreparedRequestId = RestartState.PreparedRestoreRequestId;
					ProofState->PreparedTaskId = RestartState.TaskId;
					ProofState->PreparedSessionId = RestartState.SessionId;
					ProofState->bPreparedRequestWritten = !RestartState.PreparedRestoreRequestId.IsEmpty();
					ProofState->bPreparedRequestConsumed = !FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest();
					ProofState->bAutonomousTaskIdDerived = RestartState.TaskId.StartsWith(TEXT("task_autonomous_"));
					ProofState->bAutonomousSessionIdDerived = !RestartState.TaskId.IsEmpty()
						&& RestartState.SessionId == FString::Printf(TEXT("restart_survival_%s"), *RestartState.TaskId);

					const bool bDetachingOrBeyond =
						RestartState.Phase == EOsvayderUERestartSurvivalPhase::Detaching
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::DetachedRunning
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingRelaunch
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::Relaunching
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingReattach
						|| RestartState.Phase == EOsvayderUERestartSurvivalPhase::Reattached;

					if (!bDetachingOrBeyond)
					{
						if (RestartState.Phase == EOsvayderUERestartSurvivalPhase::FailedTerminal)
						{
							ProofState->ErrorText = RestartState.PhaseDetail.IsEmpty()
								? TEXT("Autonomous closed-editor handoff reached FailedTerminal.")
								: RestartState.PhaseDetail;
							WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
							return false;
						}

						return true;
					}

					ProofState->CompletionPromptText = RestartState.PostReattachCompletionText;
					const bool bSuccess =
						ProofState->bTransitionNoticeVisible
						&& ProofState->bPreparedRequestWritten
						&& ProofState->bAutonomousTaskIdDerived
						&& ProofState->bAutonomousSessionIdDerived
						&& IsRestartSurvivalAutonomousClosedEditorPostReattachPromptSafe(ProofState->CompletionPromptText)
						&& !RestartState.PreparedRestoreRequestId.IsEmpty()
						&& RestartState.RestoreIntent.bEnabled;

					if (!bSuccess)
					{
						ProofState->ErrorText = TEXT("Autonomous closed-editor handoff did not preserve the expected derived identity, notice, or safe post-reattach continuation state.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					WriteRestartSurvivalProofJson(ProofState, TEXT("detaching"));
					return false;
				}),
				0.2f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUEObserveRestartSurvivalAutonomousClosedEditorEscalationProofCommand(
		TEXT("OsvayderUE.ObserveRestartSurvivalAutonomousClosedEditorEscalationProof"),
		TEXT("Observe the live ordinary natural-language restart-survival autonomous closed-editor escalation proof after relaunch. Usage: OsvayderUE.ObserveRestartSurvivalAutonomousClosedEditorEscalationProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.ObserveRestartSurvivalAutonomousClosedEditorEscalationProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("autonomous_closed_editor_escalation");
			ProofState->CommandDescription = TEXT("OsvayderUE.RunRestartSurvivalAutonomousClosedEditorEscalationProof / OsvayderUE.ObserveRestartSurvivalAutonomousClosedEditorEscalationProof");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();
			ProofState->PreparedRequestPath = FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath();
			ProofState->ExpectedCompletionResponseText = GetRestartSurvivalAutonomousClosedEditorCompletionResponse();
			ProofState->TransitionNoticeText = GetRestartSurvivalClosedEditorTransitionNoticePrefix();

			FOsvayderUERestartSurvivalState RestartState;
			FString RestartError;
			if (!FOsvayderUERestartSurvivalManager::LoadState(RestartState, RestartError))
			{
				ProofState->ErrorText = RestartError;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			if (RestartState.Proof.RunTag != RunTag)
			{
				ProofState->ErrorText = FString::Printf(
					TEXT("Restart-survival state run_tag mismatch. Expected %s but found %s."),
					*RunTag,
					*RestartState.Proof.RunTag);
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->ProviderSessionIdBefore = RestartState.ProviderSessionId;
			ProofState->ProviderThreadStatePath = RestartState.ProviderThreadStatePath;
			ProofState->DetachedFileTargetPath = RestartState.Proof.DetachedFileTargetPath;
			ProofState->DetachedFileExpectedText = RestartState.Proof.DetachedFileExpectedText;
			ProofState->RestoreSourcePath = RestartState.RestoreIntent.AutosaveSourcePath;
			ProofState->RestoreTargetPath = RestartState.RestoreIntent.TargetPath;
			ProofState->RestoreBackupPath = RestartState.RestoreIntent.BackupPath;
			ProofState->RestoreExpectedText = RestartState.Proof.RestoreExpectedText;
			ProofState->BuildLogPath = RestartState.Proof.BuildLogPath;
			ProofState->DetachedLogPath = RestartState.Proof.DetachedLogPath;
			ProofState->ReattachNotice = RestartState.ReattachNotice;
			ProofState->PreparedRequestId = RestartState.PreparedRestoreRequestId;
			ProofState->PreparedTaskId = RestartState.TaskId;
			ProofState->PreparedSessionId = RestartState.SessionId;
			ProofState->bAutonomousTaskIdDerived = RestartState.TaskId.StartsWith(TEXT("task_autonomous_"));
			ProofState->bAutonomousSessionIdDerived = !RestartState.TaskId.IsEmpty()
				&& RestartState.SessionId == FString::Printf(TEXT("restart_survival_%s"), *RestartState.TaskId);
			ProofState->CompletionPromptText = RestartState.PostReattachCompletionText;
			ProofState->ExpectedSeedResponseText = GetRestartSurvivalAutonomousClosedEditorSeedResponse();
			ProofState->SeedPromptText = BuildRestartSurvivalAutonomousClosedEditorSeedPrompt(
				ProofState->RestoreSourcePath,
				ProofState->RestoreTargetPath,
				ProofState->RestoreBackupPath);
			CarryForwardRestartSurvivalProofSeedState(ProofState);

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available after relaunch.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->WeakWidget = Widget;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState, RunTag](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable during autonomous closed-editor proof observation.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for autonomous closed-editor proof observation.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					ProofState->DetachedFileObservedText = LoadTextFileIfExists(ProofState->DetachedFileTargetPath).TrimStartAndEnd();
					ProofState->RestoreTargetObservedText = LoadTextFileIfExists(ProofState->RestoreTargetPath).TrimStartAndEnd();
					ProofState->RestoreBackupObservedText = LoadTextFileIfExists(ProofState->RestoreBackupPath).TrimStartAndEnd();
					ProofState->bDetachedFileWriteObserved = ProofState->DetachedFileObservedText == ProofState->DetachedFileExpectedText;
					ProofState->bRestoreObserved = ProofState->RestoreTargetObservedText == ProofState->RestoreExpectedText;
					ProofState->bRestoreBackupObserved = !ProofState->RestoreBackupObservedText.IsEmpty();
					ProofState->bBuildLogObserved = !LoadTextFileIfExists(ProofState->BuildLogPath).IsEmpty();
					ProofState->bReattachNoticeVisible = !ProofState->ReattachNotice.IsEmpty() && LiveWidget->DebugVisibleChatContains(ProofState->ReattachNotice);
					ProofState->bSeedHistoryVisible = LiveWidget->DebugVisibleChatContains(ProofState->SeedPromptText);
					ProofState->ObservedCompletionResponseText = LiveWidget->DebugGetLastResponseText();
					ProofState->bCompletionResponseMatched = ProofState->ObservedCompletionResponseText.Contains(
						ProofState->ExpectedCompletionResponseText,
						ESearchCase::CaseSensitive);
					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					ProofState->bPreparedRequestConsumed = !FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest();

					FOsvayderUERestartSurvivalState CurrentState;
					FString CurrentStateError;
					if (!FOsvayderUERestartSurvivalManager::LoadState(CurrentState, CurrentStateError))
					{
						ProofState->ErrorText = CurrentStateError;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (CurrentState.Proof.RunTag != RunTag)
					{
						ProofState->ErrorText = TEXT("Observed restart-survival state no longer matches the autonomous closed-editor proof run.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (CurrentState.bPostReattachCompletionDispatched)
					{
						ProofState->bResumePromptStarted = true;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					const bool bSuccess =
						ProofState->RuntimeState == SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected
						&& ProofState->bTransitionNoticeVisible
						&& ProofState->bDetachedFileWriteObserved
						&& ProofState->bRestoreObserved
						&& ProofState->bRestoreBackupObserved
						&& ProofState->bBuildLogObserved
						&& ProofState->bPreparedRequestConsumed
						&& ProofState->bReattachNoticeVisible
						&& ProofState->bAutonomousTaskIdDerived
						&& ProofState->bAutonomousSessionIdDerived
						&& ProofState->bCompletionResponseMatched
						&& CurrentState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
						&& !CurrentState.bProviderThreadResumePending
						&& CurrentState.TaskId == ProofState->PreparedTaskId
						&& CurrentState.SessionId == ProofState->PreparedSessionId;

					if (bSuccess)
					{
						FString TraceRunId;
						const TArray<TSharedPtr<FJsonObject>> TraceRecords = QueryLatestTraceRecordsForBackend(ProofState->Backend, TraceRunId);
						ProofState->ProviderSessionIdAfter = FindProviderSessionIdInTraceRecords(TraceRecords);
						WriteRestartSurvivalProofJson(ProofState, TEXT("completed"));
					}
					else if (CurrentState.Phase == EOsvayderUERestartSurvivalPhase::FailedTerminal)
					{
						ProofState->ErrorText = CurrentState.PhaseDetail.IsEmpty()
							? TEXT("Autonomous closed-editor proof reached FailedTerminal.")
							: CurrentState.PhaseDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
					}
					else
					{
						return true;
					}

					FPlatformMisc::RequestExit(false);
					return false;
				}),
				0.25f);
		}));

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunRestartSurvivalAutonomousClosedEditorEscalationNegativeProofCommand(
		TEXT("OsvayderUE.RunRestartSurvivalAutonomousClosedEditorEscalationNegativeProof"),
		TEXT("Run a live negative proof for out-of-scope autonomous closed-editor escalation. Usage: OsvayderUE.RunRestartSurvivalAutonomousClosedEditorEscalationNegativeProof <run_tag>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunRestartSurvivalAutonomousClosedEditorEscalationNegativeProof <run_tag>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const TSharedRef<FRestartSurvivalProofState> ProofState = MakeShared<FRestartSurvivalProofState>();
			ProofState->RunTag = RunTag;
			ProofState->StartPath = TEXT("autonomous_closed_editor_escalation");
			ProofState->CommandDescription = TEXT("OsvayderUE.RunRestartSurvivalAutonomousClosedEditorEscalationNegativeProof");
			ProofState->Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
			ProofState->StartTimeSeconds = FPlatformTime::Seconds();
			ProofState->ExpectedSeedResponseText = GetRestartSurvivalAutonomousClosedEditorNegativeResponse();
			ProofState->TransitionNoticeText = GetRestartSurvivalClosedEditorTransitionNoticePrefix();

			FString IgnoreError;
			FOsvayderUERestartSurvivalManager::DeleteState(IgnoreError);
			FOsvayderUERestartSurvivalManager::DeletePreparedRestoreRequest(IgnoreError);
			FOsvayderUERestartSurvivalManager::ClearPreparedRequestStartOverride();

			const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			if (BackendStatus.Backend != EOsvayderUEProviderBackend::CodexCli || !FCodexCliRunner::ShouldUsePersistentConversationTransport())
			{
				ProofState->ErrorText = TEXT("Negative autonomous closed-editor proof requires the ordinary persistent Codex runtime path.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			if (!AgentBackendCanExecutePrompt(BackendStatus))
			{
				ProofState->ErrorText = BackendStatus.Detail.IsEmpty()
					? TEXT("Configured backend is not ready for negative autonomous closed-editor proof.")
					: BackendStatus.Detail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("rejected"));
				return;
			}

			const TSharedPtr<SOsvayderEditorWidget> Widget = EnsureLiveOsvayderEditorWidget();
			if (!Widget.IsValid())
			{
			ProofState->ErrorText = TEXT("Osvayder UE widget is not available for negative autonomous closed-editor proof.");
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			ProofState->SeedPromptText = BuildRestartSurvivalAutonomousClosedEditorNegativePrompt();

			ProofState->WeakWidget = Widget;
			Widget->DebugClearInputText();
			Widget->DebugSetInputText(ProofState->SeedPromptText);
			ProofState->bSeedPromptStarted = Widget->DebugSendCurrentInput();
			if (!ProofState->bSeedPromptStarted)
			{
				ProofState->RuntimeState = Widget->DebugGetLastRuntimeConnectionState();
				ProofState->RuntimeDetail = TEXT("Negative autonomous closed-editor prompt dispatch did not start.");
				ProofState->ErrorText = ProofState->RuntimeDetail;
				WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
				return;
			}

			WriteRestartSurvivalProofJson(ProofState, TEXT("seed_pending"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([ProofState](float)
				{
					const TSharedPtr<SOsvayderEditorWidget> LiveWidget = ProofState->WeakWidget.Pin();
					if (!LiveWidget.IsValid())
					{
				ProofState->ErrorText = TEXT("Osvayder UE widget became unavailable before negative autonomous closed-editor validation.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (FPlatformTime::Seconds() - ProofState->StartTimeSeconds > ProofState->TimeoutSeconds)
					{
						ProofState->bTimedOut = true;
						ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
						ProofState->RuntimeDetail = TEXT("Timed out while waiting for the negative autonomous closed-editor prompt to complete.");
						ProofState->ErrorText = ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (LiveWidget->DebugIsWaitingForResponse())
					{
						return true;
					}

					ProofState->RuntimeState = LiveWidget->DebugGetLastRuntimeConnectionState();
					ProofState->RuntimeDetail = LiveWidget->DebugGetLastRuntimeConnectionDetail();
					ProofState->ObservedSeedResponseText = LiveWidget->DebugGetLastResponseText();
					ProofState->bSeedResponseMatched = ProofState->ObservedSeedResponseText.Contains(
						ProofState->ExpectedSeedResponseText,
						ESearchCase::CaseSensitive);
					ProofState->bPreparedRequestConsumed = !FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest();
					ProofState->bTransitionNoticeVisible = LiveWidget->DebugVisibleChatContains(ProofState->TransitionNoticeText);

					FOsvayderUERestartSurvivalState CurrentState;
					const bool bStatePresent = FOsvayderUERestartSurvivalManager::DescribeCurrentState(CurrentState);
					if (bStatePresent)
					{
						const bool bInvalidStateAdvance =
							CurrentState.Phase == EOsvayderUERestartSurvivalPhase::Detaching
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::DetachedRunning
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingRelaunch
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::Relaunching
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::AwaitingReattach
							|| CurrentState.Phase == EOsvayderUERestartSurvivalPhase::Reattached;
						if (bInvalidStateAdvance)
						{
							ProofState->ErrorText = TEXT("Out-of-scope autonomous closed-editor request incorrectly advanced restart survival.");
							WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
							return false;
						}
					}

					if (ProofState->RuntimeState != SOsvayderEditorWidget::EBackendRuntimeConnectionState::Connected)
					{
						ProofState->ErrorText = ProofState->RuntimeDetail.IsEmpty()
							? TEXT("Negative autonomous closed-editor prompt did not complete successfully.")
							: ProofState->RuntimeDetail;
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					if (!ProofState->bPreparedRequestConsumed)
					{
						ProofState->ErrorText = TEXT("Out-of-scope autonomous closed-editor request incorrectly left a prepared request behind.");
						WriteRestartSurvivalProofJson(ProofState, TEXT("failed"));
						return false;
					}

					WriteRestartSurvivalProofJson(ProofState, TEXT("rejected_invalid_request"));
					return false;
				}),
				0.2f);
		}));

	static FAutoConsoleCommand GOsvayderUESetCodexProfileCommand(
		TEXT("OsvayderUE.SetCodexProfile"),
		TEXT("Override the in-memory Codex CLI profile for this editor session. Usage: OsvayderUE.SetCodexProfile <profile|default>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			UOsvayderUESettings* Settings = UOsvayderUESettings::GetMutable();
			if (!Settings)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.SetCodexProfile failed: settings unavailable"));
				return;
			}

			FString NewProfile = JoinPromptArgs(Args, 0).TrimStartAndEnd();
			if (NewProfile.Equals(TEXT("default"), ESearchCase::IgnoreCase) ||
				NewProfile.Equals(TEXT("none"), ESearchCase::IgnoreCase) ||
				NewProfile.Equals(TEXT("empty"), ESearchCase::IgnoreCase))
			{
				NewProfile.Empty();
			}

			Settings->DefaultCodexProfile = NewProfile;
			UE_LOG(
				LogOsvayderUE,
				Log,
				TEXT("OsvayderUE.SetCodexProfile -> %s (explicit=%s, model=%s, auth_mode=%s, auth_path=%s)"),
				*Settings->GetEffectiveCodexProfileLabel(),
				Settings->HasExplicitCodexProfile() ? TEXT("true") : TEXT("false"),
				*Settings->GetConfiguredCodexModel(),
				*FCodexCliRunner::GetConfiguredAuthModeName(),
				*FCodexCliRunner::GetEffectiveAuthEntryPath());
		}));

	static const FConsoleCommandWithArgsDelegate SetCodexAuthModeDelegate = FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			UOsvayderUESettings* Settings = UOsvayderUESettings::GetMutable();
			if (!Settings)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.SetCodexAuthMode failed: settings unavailable"));
				return;
			}

			const FString RequestedMode = JoinPromptArgs(Args, 0).TrimStartAndEnd();
			if (RequestedMode.Equals(TEXT("api"), ESearchCase::IgnoreCase) ||
				RequestedMode.Equals(TEXT("api_key_env_var"), ESearchCase::IgnoreCase) ||
				RequestedMode.Equals(TEXT("env"), ESearchCase::IgnoreCase))
			{
				Settings->DefaultCodexAuthMode = EOsvayderUECodexAuthMode::ApiKeyEnvVar;
			}
			else if (RequestedMode.Equals(TEXT("cli_terminal"), ESearchCase::IgnoreCase) ||
				RequestedMode.Equals(TEXT("cli"), ESearchCase::IgnoreCase))
			{
				Settings->DefaultCodexAuthMode = EOsvayderUECodexAuthMode::CliTerminal;
			}
			else if (RequestedMode.Equals(TEXT("browser_verify"), ESearchCase::IgnoreCase) ||
				RequestedMode.Equals(TEXT("browser"), ESearchCase::IgnoreCase) ||
				RequestedMode.Equals(TEXT("device"), ESearchCase::IgnoreCase))
			{
				Settings->DefaultCodexAuthMode = EOsvayderUECodexAuthMode::BrowserVerify;
			}
			else if (RequestedMode.Equals(TEXT("auto"), ESearchCase::IgnoreCase) ||
				RequestedMode.IsEmpty())
			{
				Settings->DefaultCodexAuthMode = EOsvayderUECodexAuthMode::Auto;
			}
			else
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.SetCodexAuthMode <auto|api|api_key_env_var|cli_terminal|browser_verify> (OsvayderUE.SetCodexAuthMode remains as a compatibility alias)"));
				return;
			}

			const FAgentBackendStatus Status = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			UE_LOG(
				LogOsvayderUE,
				Log,
				TEXT("OsvayderUE.SetCodexAuthMode -> %s (effective_path=%s, ownership=%s, ready=%s)"),
				*FCodexCliRunner::GetConfiguredAuthModeName(),
				*FCodexCliRunner::GetEffectiveAuthEntryPath(),
				*FCodexCliRunner::GetEffectiveAuthOwnershipModel(),
				Status.bReady ? TEXT("true") : TEXT("false"));
		});
	static FAutoConsoleCommand GOsvayderUESetCodexAuthModeCommand(
		TEXT("OsvayderUE.SetCodexAuthMode"),
		TEXT("Preferred command: OsvayderUE.SetCodexAuthMode <auto|api|api_key_env_var|cli_terminal|browser_verify>. OsvayderUE.SetCodexAuthMode remains as a compatibility alias."),
		SetCodexAuthModeDelegate);

	static const FConsoleCommandDelegate LaunchCodexBrowserVerifyDelegate = FConsoleCommandDelegate::CreateLambda([]()
		{
			FString StatusMessage;
			if (FCodexCliRunner::LaunchBrowserVerifyLogin(StatusMessage))
			{
				UE_LOG(LogOsvayderUE, Log, TEXT("%s"), *StatusMessage);
			}
			else
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *StatusMessage);
			}
		});
	static FAutoConsoleCommand GOsvayderUELaunchCodexBrowserVerifyCommand(
		TEXT("OsvayderUE.LaunchCodexBrowserVerify"),
		TEXT("Preferred command: OsvayderUE.LaunchCodexBrowserVerify. OsvayderUE.LaunchCodexBrowserVerify remains as a compatibility alias. Launches Codex Browser Verify by requesting a browser login URL from `codex app-server` and opening it from Unreal."),
		LaunchCodexBrowserVerifyDelegate);

	static const FConsoleCommandDelegate DumpCodexAuthDiagnosticsDelegate = FConsoleCommandDelegate::CreateLambda([]()
		{
			const FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
			const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("CodexAuthDiagnostics"));
			IFileManager::Get().MakeDirectory(*OutputDir, true);
			const FString OutputPath = FPaths::Combine(OutputDir, TEXT("codex_auth_diagnostics_latest.json"));

			TSharedPtr<FJsonObject> Root = MakeCodexAuthDiagnosticsJson(Diagnostics);
			FString JsonText;
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
			if (FJsonSerializer::Serialize(Root.ToSharedRef(), Writer) &&
				FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE.DumpCodexAuthDiagnostics -> %s | %s"),
					*OutputPath,
					*FCodexCliRunner::BuildAuthDiagnosticsCompactText(Diagnostics));
			}
			else
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.DumpCodexAuthDiagnostics failed to write %s"), *OutputPath);
			}
		});
	static FAutoConsoleCommand GOsvayderUEDumpCodexAuthDiagnosticsCommand(
		TEXT("OsvayderUE.DumpCodexAuthDiagnostics"),
		TEXT("Preferred command: OsvayderUE.DumpCodexAuthDiagnostics. OsvayderUE.DumpCodexAuthDiagnostics remains as a compatibility alias. Writes current Codex auth diagnostics to Saved/OsvayderUE/CodexAuthDiagnostics/codex_auth_diagnostics_latest.json."),
		DumpCodexAuthDiagnosticsDelegate);

	static const FConsoleCommandDelegate ProbeCodexAuthDelegate = FConsoleCommandDelegate::CreateLambda([]()
		{
			FString StatusMessage;
			if (FCodexCliRunner::ProbeBackendAuth(StatusMessage))
			{
				UE_LOG(LogOsvayderUE, Log, TEXT("%s"), *StatusMessage);
			}
			else
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *StatusMessage);
			}
		});
	static FAutoConsoleCommand GOsvayderUEProbeCodexAuthCommand(
		TEXT("OsvayderUE.ProbeCodexAuth"),
		TEXT("Preferred command: OsvayderUE.ProbeCodexAuth. OsvayderUE.ProbeCodexAuth remains as a compatibility alias. Probes Codex backend auth with app-server/thread-start and writes a receipt under Saved/OsvayderUE/CodexAuthDiagnostics."),
		ProbeCodexAuthDelegate);

	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunCodexAuthDiagnosticsLiveCheckCommand(
		TEXT("OsvayderUE.RunCodexAuthDiagnosticsLiveCheck"),
		TEXT("Run Codex auth diagnostics, probe backend auth, then send one read-only Codex prompt. Usage: OsvayderUE.RunCodexAuthDiagnosticsLiveCheck <tag> <prompt...>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunCodexAuthDiagnosticsLiveCheck <tag> <prompt...>"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const FString Prompt = JoinPromptArgs(Args, 1);
			FCodexCliRunner::FCodexAuthDiagnostics DiagnosticsBefore = FCodexCliRunner::GetAuthDiagnostics();

			FString ProbeStatusMessage;
			const bool bProbeOk = FCodexCliRunner::ProbeBackendAuth(ProbeStatusMessage);
			FCodexCliRunner::FCodexAuthDiagnostics DiagnosticsAfterProbe = FCodexCliRunner::GetAuthDiagnostics();
			DiagnosticsAfterProbe.bProbePerformed = true;
			DiagnosticsAfterProbe.ProbeDetailText = ProbeStatusMessage;

			FAgentRequestConfig Config;
			Config.Prompt = Prompt;
			Config.WorkingDirectory = FPaths::ProjectDir();
			Config.ExecutionProfile = EAgentExecutionRunProfile::ReadOnlyDiagnostic;
			Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
			Config.bSkipPermissions = false;
			Config.bEnableUnrealMcpBridge = false;
			Config.TimeoutSeconds = 120.0f;
			Config.AllowedTools = { TEXT("Read"), TEXT("Grep"), TEXT("Glob") };

			FCodexCliRunner Runner;
			FString PromptOutput;
			const bool bPromptOk = Runner.ExecuteSync(Config, PromptOutput);

			const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("CodexAuthDiagnostics"));
			IFileManager::Get().MakeDirectory(*OutputDir, true);
			const FString OutputPath = FPaths::Combine(OutputDir, RunTag + TEXT(".live_check.json"));

			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("run_tag"), RunTag);
			Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
			Root->SetStringField(TEXT("command"), TEXT("OsvayderUE.RunCodexAuthDiagnosticsLiveCheck"));
			Root->SetStringField(TEXT("prompt"), Prompt);
			Root->SetObjectField(TEXT("backend_status"), MakeBackendStatusJson(FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus()));
			Root->SetObjectField(TEXT("diagnostics_before"), MakeCodexAuthDiagnosticsJson(DiagnosticsBefore));
			Root->SetObjectField(TEXT("diagnostics_after_probe"), MakeCodexAuthDiagnosticsJson(DiagnosticsAfterProbe));
			Root->SetBoolField(TEXT("probe_success"), bProbeOk);
			Root->SetStringField(TEXT("probe_status_message"), ProbeStatusMessage);
			Root->SetBoolField(TEXT("prompt_success"), bPromptOk);
			Root->SetStringField(TEXT("prompt_output"), PromptOutput.Left(4000));
			Root->SetBoolField(TEXT("relogin_path_available_without_api_env_mode"),
				DiagnosticsBefore.bExecutableAvailable && !DiagnosticsBefore.AuthMode.Equals(TEXT("api"), ESearchCase::IgnoreCase));

			FString JsonText;
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
			if (FJsonSerializer::Serialize(Root.ToSharedRef(), Writer) &&
				FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE.RunCodexAuthDiagnosticsLiveCheck -> probe=%s prompt=%s path=%s"),
					bProbeOk ? TEXT("ok") : TEXT("failed"),
					bPromptOk ? TEXT("ok") : TEXT("failed"),
					*OutputPath);
			}
			else
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunCodexAuthDiagnosticsLiveCheck failed to write %s"), *OutputPath);
			}
		}));

	static const FConsoleCommandDelegate OpenProjectSettingsDelegate = FConsoleCommandDelegate::CreateLambda([]()
		{
			ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
			if (!SettingsModule)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.OpenProjectSettings failed: Settings module is unavailable"));
				return;
			}

			SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("OsvayderUE"));
	UE_LOG(LogOsvayderUE, Log, TEXT("Opened Project Settings -> Plugins -> Osvayder UE"));
		});
	static FAutoConsoleCommand GOsvayderUEOpenProjectSettingsCommand(
		TEXT("OsvayderUE.OpenProjectSettings"),
	TEXT("Preferred command: OsvayderUE.OpenProjectSettings. OsvayderUE.OpenProjectSettings remains as a compatibility alias. Opens Project Settings -> Plugins -> Osvayder UE."),
		OpenProjectSettingsDelegate);

	static const FConsoleCommandWithArgsDelegate SetCodexApiKeyEnvVarDelegate = FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			UOsvayderUESettings* Settings = UOsvayderUESettings::GetMutable();
			if (!Settings)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.SetCodexApiKeyEnvVar failed: settings unavailable"));
				return;
			}

			FString NewEnvVar = JoinPromptArgs(Args, 0).TrimStartAndEnd();
			if (NewEnvVar.Equals(TEXT("default"), ESearchCase::IgnoreCase) ||
				NewEnvVar.Equals(TEXT("reset"), ESearchCase::IgnoreCase) ||
				NewEnvVar.Equals(TEXT("empty"), ESearchCase::IgnoreCase))
			{
				NewEnvVar.Empty();
			}
			else if (NewEnvVar.IsEmpty())
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.SetCodexApiKeyEnvVar <env_var|default> (OsvayderUE.SetCodexApiKeyEnvVar remains as a compatibility alias)"));
				return;
			}

			Settings->DefaultCodexApiKeyEnvVar = NewEnvVar;
			const FAgentBackendStatus Status = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
			UE_LOG(
				LogOsvayderUE,
				Log,
				TEXT("OsvayderUE.SetCodexApiKeyEnvVar -> %s (auth_mode=%s, effective_path=%s, ready=%s)"),
				*Settings->GetConfiguredCodexApiKeyEnvVar(),
				*FCodexCliRunner::GetConfiguredAuthModeName(),
				*FCodexCliRunner::GetEffectiveAuthEntryPath(),
				Status.bReady ? TEXT("true") : TEXT("false"));
		});
	static FAutoConsoleCommand GOsvayderUESetCodexApiKeyEnvVarCommand(
		TEXT("OsvayderUE.SetCodexApiKeyEnvVar"),
		TEXT("Preferred command: OsvayderUE.SetCodexApiKeyEnvVar <env_var|default>. OsvayderUE.SetCodexApiKeyEnvVar remains as a compatibility alias."),
		SetCodexApiKeyEnvVarDelegate);

	static const FConsoleCommandWithWorldAndArgsDelegate RunConfiguredBackendPromptDelegate = FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunConfiguredBackendPrompt <run_tag> <prompt...> (OsvayderUE.RunConfiguredBackendPrompt remains as a compatibility alias)"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const FString Prompt = JoinPromptArgs(Args, 1);

			FOsvayderPromptOptions Options;
			Options.bIncludeEngineContext = true;
			Options.bIncludeProjectContext = true;
			Options.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;

			StartConfiguredBackendPromptRun(TEXT("OsvayderUE.RunConfiguredBackendPrompt"), RunTag, Prompt, Options);
		});
	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunConfiguredBackendPromptCommand(
		TEXT("OsvayderUE.RunConfiguredBackendPrompt"),
		TEXT("Preferred command: OsvayderUE.RunConfiguredBackendPrompt. OsvayderUE.RunConfiguredBackendPrompt remains as a compatibility alias. Runs a prompt through the configured OsvayderUE backend using the ordinary workspace-write default runtime and persists the result to Saved/OsvayderUE/backend_runs/<tag>.json."),
		RunConfiguredBackendPromptDelegate);

	static const FConsoleCommandWithWorldAndArgsDelegate RunHeadlessNewSessionAcceptanceDelegate = FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			FOsvayderUEHeadlessAcceptanceRequest Request;
			FString ParseError;
			if (!TryParseHeadlessAcceptanceArgs(Args, Request, ParseError))
			{
				WriteHeadlessAcceptanceFailureReceipt(Request, ParseError, TEXT("console_command_parse"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunHeadlessNewSessionAcceptance rejected: %s"), *ParseError);
				return;
			}

			Request.TriggerPath = TEXT("console_command_headless_acceptance");
			DispatchHeadlessAcceptanceRequest(Request, TEXT("console_command_headless_acceptance"));
		});
	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunHeadlessNewSessionAcceptanceCommand(
		TEXT("OsvayderUE.RunHeadlessNewSessionAcceptance"),
		TEXT("Preferred command: OsvayderUE.RunHeadlessNewSessionAcceptance. OsvayderUE.RunHeadlessNewSessionAcceptance remains as a compatibility alias. For path-safe automation, prefer -OsvayderUERunHeadlessNewSessionAcceptance -OsvayderUEHeadlessAcceptancePromptFile=<path> -OsvayderUEHeadlessAcceptancePrefix=<prefix>; the OsvayderUE flags still work."),
		RunHeadlessNewSessionAcceptanceDelegate);

	static const FConsoleCommandWithWorldAndArgsDelegate RunVisibleManualNewSessionAcceptanceDelegate = FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			FOsvayderUEHeadlessAcceptanceRequest Request;
			FString ParseError;
			if (!TryParseHeadlessAcceptanceArgs(Args, Request, ParseError))
			{
				Request.bVisibleManualEmulator = true;
				Request.bRequireVisibleEditor = true;
				Request.TriggerPath = TEXT("console_command_visible_manual_parse");
				WriteHeadlessAcceptanceFailureReceipt(Request, ParseError, TEXT("console_command_visible_manual_parse"));
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.RunVisibleManualNewSessionAcceptance rejected: %s"), *ParseError);
				return;
			}

			Request.bVisibleManualEmulator = true;
			Request.bRequireVisibleEditor = true;
			Request.TriggerPath = TEXT("console_command_visible_manual_acceptance");
			DispatchHeadlessAcceptanceRequest(Request, TEXT("console_command_visible_manual_acceptance"));
		});
	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunVisibleManualNewSessionAcceptanceCommand(
		TEXT("OsvayderUE.RunVisibleManualNewSessionAcceptance"),
		TEXT("Preferred command: OsvayderUE.RunVisibleManualNewSessionAcceptance. OsvayderUE.RunVisibleManualNewSessionAcceptance remains as a compatibility alias. For path-safe automation, prefer -OsvayderUERunVisibleManualNewSessionAcceptance -OsvayderUEVisibleManualAcceptancePromptFile=<path> -OsvayderUEVisibleManualAcceptancePrefix=<prefix>; the OsvayderUE flags still work."),
		RunVisibleManualNewSessionAcceptanceDelegate);

	static FAutoConsoleCommand GOsvayderUESeedCanonLedgerV3ProofCommand(
		TEXT("OsvayderUE.SeedCanonLedgerV3Proof"),
		TEXT("Explicitly promote the accepted P16 canon proof receipts into Saved/OsvayderUE/canon_ledger.json for V3 directed-execution proofing."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			TArray<FString> PatternKeys;
			FString Error;
			if (!SeedCanonLedgerV3Proof(Error, PatternKeys))
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("OsvayderUE.SeedCanonLedgerV3Proof failed: %s"), *Error);
				return;
			}

			UE_LOG(
				LogOsvayderUE,
				Log,
				TEXT("OsvayderUE.SeedCanonLedgerV3Proof -> ledger=%s promoted=%d patterns=%s"),
				*FOsvayderUECanonLedger::GetLedgerPath(),
				PatternKeys.Num(),
				*FString::Join(PatternKeys, TEXT(",")));
		}));

	static const FConsoleCommandWithWorldAndArgsDelegate RunConfiguredBackendExpertOptInPromptDelegate = FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunConfiguredBackendExpertOptInPrompt <run_tag> <prompt...> (OsvayderUE.RunConfiguredBackendExpertOptInPrompt remains as a compatibility alias)"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const FString Prompt = JoinPromptArgs(Args, 1);

			FOsvayderPromptOptions Options;
			Options.bIncludeEngineContext = true;
			Options.bIncludeProjectContext = true;
			Options.ExecutionProfile = EAgentExecutionRunProfile::ExplicitExpertOptIn;

			StartConfiguredBackendPromptRun(TEXT("OsvayderUE.RunConfiguredBackendExpertOptInPrompt"), RunTag, Prompt, Options);
		});
	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunConfiguredBackendExpertOptInPromptCommand(
		TEXT("OsvayderUE.RunConfiguredBackendExpertOptInPrompt"),
		TEXT("Preferred command: OsvayderUE.RunConfiguredBackendExpertOptInPrompt. OsvayderUE.RunConfiguredBackendExpertOptInPrompt remains as a compatibility alias. Runs a prompt through the configured OsvayderUE backend using the explicit expert opt-in execution profile and persists the result to Saved/OsvayderUE/backend_runs/<tag>.json."),
		RunConfiguredBackendExpertOptInPromptDelegate);

	static const FConsoleCommandWithWorldAndArgsDelegate RunConfiguredBackendReadOnlyPromptDelegate = FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunConfiguredBackendReadOnlyPrompt <run_tag> <prompt...> (OsvayderUE.RunConfiguredBackendReadOnlyPrompt remains as a compatibility alias)"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const FString Prompt = JoinPromptArgs(Args, 1);

			FOsvayderPromptOptions Options;
			Options.bIncludeEngineContext = false;
			Options.bIncludeProjectContext = false;
			Options.ExecutionProfile = EAgentExecutionRunProfile::ReadOnlyDiagnostic;

			StartConfiguredBackendPromptRun(TEXT("OsvayderUE.RunConfiguredBackendReadOnlyPrompt"), RunTag, Prompt, Options);
		});
	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunConfiguredBackendReadOnlyPromptCommand(
		TEXT("OsvayderUE.RunConfiguredBackendReadOnlyPrompt"),
		TEXT("Preferred command: OsvayderUE.RunConfiguredBackendReadOnlyPrompt. OsvayderUE.RunConfiguredBackendReadOnlyPrompt remains as a compatibility alias. Runs a prompt through the configured OsvayderUE backend using the explicit read-only diagnostic execution profile and persists the result to Saved/OsvayderUE/backend_runs/<tag>.json."),
		RunConfiguredBackendReadOnlyPromptDelegate);

	static const FConsoleCommandWithWorldAndArgsDelegate RunConfiguredBackendBoundedMutationPromptDelegate = FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunConfiguredBackendBoundedMutationPrompt <run_tag> <prompt...> (OsvayderUE.RunConfiguredBackendBoundedMutationPrompt remains as a compatibility alias)"));
				return;
			}

			const FString RunTag = SanitizeBackendRunTag(Args[0]);
			const FString Prompt = JoinPromptArgs(Args, 1);

			FOsvayderPromptOptions Options;
			Options.bIncludeEngineContext = false;
			Options.bIncludeProjectContext = false;
			Options.ExecutionProfile = EAgentExecutionRunProfile::BoundedPluginMutation;

			StartConfiguredBackendPromptRun(TEXT("OsvayderUE.RunConfiguredBackendBoundedMutationPrompt"), RunTag, Prompt, Options);
		});
	static FAutoConsoleCommandWithWorldAndArgs GOsvayderUERunConfiguredBackendBoundedMutationPromptCommand(
		TEXT("OsvayderUE.RunConfiguredBackendBoundedMutationPrompt"),
		TEXT("Preferred command: OsvayderUE.RunConfiguredBackendBoundedMutationPrompt. OsvayderUE.RunConfiguredBackendBoundedMutationPrompt remains as a compatibility alias. Runs a prompt through the configured OsvayderUE backend using the explicit bounded plugin mutation execution profile and persists the result to Saved/OsvayderUE/backend_runs/<tag>.json."),
		RunConfiguredBackendBoundedMutationPromptDelegate);

	static FAutoConsoleCommand GOsvayderUERunExecutionControlSlice1ProofCommand(
		TEXT("OsvayderUE.RunExecutionControlSlice1Proof"),
		TEXT("Capture a live HC1/HC2 slice-1 control proof receipt to Saved/OsvayderUE/backend_runs/<tag>.json. Usage: OsvayderUE.RunExecutionControlSlice1Proof <run_tag>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunExecutionControlSlice1Proof <run_tag>"));
				return;
			}

			RunExecutionControlSlice1Proof(SanitizeBackendRunTag(Args[0]));
		}));

	static FAutoConsoleCommand GOsvayderUERunExecutionControlSlice2ProofCommand(
		TEXT("OsvayderUE.RunExecutionControlSlice2Proof"),
		TEXT("Capture a live HC1/HC2 slice-2 bounded-runtime deny proof receipt to Saved/OsvayderUE/backend_runs/<tag>.json. Usage: OsvayderUE.RunExecutionControlSlice2Proof <run_tag>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunExecutionControlSlice2Proof <run_tag>"));
				return;
			}

			RunExecutionControlSlice2Proof(SanitizeBackendRunTag(Args[0]));
		}));

	static FAutoConsoleCommand GOsvayderUERunExecutionControlSlice3ProofCommand(
		TEXT("OsvayderUE.RunExecutionControlSlice3Proof"),
		TEXT("Capture a live HC1/HC2 slice-3 safer-default proof receipt to Saved/OsvayderUE/backend_runs/<tag>.json. Usage: OsvayderUE.RunExecutionControlSlice3Proof <run_tag>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Usage: OsvayderUE.RunExecutionControlSlice3Proof <run_tag>"));
				return;
			}

			const FString Prompt = TEXT("Post-ULTRA HC1/HC2 slice 3 safer-default runtime proof");
			FOsvayderPromptOptions Options;
			Options.bIncludeEngineContext = false;
			Options.bIncludeProjectContext = false;
			Options.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
			StartConfiguredBackendPromptRun(TEXT("OsvayderUE.RunExecutionControlSlice3Proof"), SanitizeBackendRunTag(Args[0]), Prompt, Options);
		}));
}

FOsvayderUEBuildSyncStatus FOsvayderUEModule::GetBuildSyncStatus()
{
	return EvaluateBuildSyncStatus();
}

bool FOsvayderUEModule::HasFreshPluginBuild()
{
	return GetBuildSyncStatus().bFresh;
}

FString FOsvayderUEModule::GetPreflightLauncherPath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Launch_OsvayderPlugin_WithPreflight.bat")));
}

void FOsvayderUEModule::RegisterSettingsUiCustomization()
{
	if (bSettingsUiCustomizationRegistered)
	{
		return;
	}

	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout(
		TEXT("OsvayderUESettings"),
		FOnGetDetailCustomizationInstance::CreateStatic(&FOsvayderUESettingsDetails::MakeInstance));
	PropertyModule.NotifyCustomizationModuleChanged();
	bSettingsUiCustomizationRegistered = true;
}

void FOsvayderUEModule::UnregisterSettingsUiCustomization()
{
	if (!bSettingsUiCustomizationRegistered)
	{
		return;
	}

	if (FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
	{
		PropertyModule->UnregisterCustomClassLayout(TEXT("OsvayderUESettings"));
		PropertyModule->NotifyCustomizationModuleChanged();
	}

	bSettingsUiCustomizationRegistered = false;
}

void FOsvayderUEModule::StartupModule()
{
	UE_LOG(LogOsvayderUE, Warning, TEXT("=== OsvayderUE BUILD 20260107-1450 THREAD_TESTS_DISABLED ==="));
	
	// Register commands
	FOsvayderUECommands::Register();
	RegisterSettingsUiCustomization();
	
	PluginCommands = MakeShared<FUICommandList>();
	
	// Map commands to actions
	PluginCommands->MapAction(
		FOsvayderUECommands::Get().OpenClaudePanel,
		FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(ClaudeTabName);
		}),
		FCanExecuteAction()
	);

	// Map QuickAsk command - shows a popup for quick questions
	PluginCommands->MapAction(
		FOsvayderUECommands::Get().QuickAsk,
		FExecuteAction::CreateLambda([]()
		{
			// Create a simple input dialog
			TSharedRef<SWindow> QuickAskWindow = SNew(SWindow)
				.Title(LOCTEXT("QuickAskTitle", "Quick Ask"))
				.ClientSize(FVector2D(500, 100))
				.SupportsMinimize(false)
				.SupportsMaximize(false);

			TSharedPtr<SEditableTextBox> InputBox;

			QuickAskWindow->SetContent(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.Padding(10)
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("QuickAskLabel", "Ask the active assistant provider a quick question:"))
				]
				+ SVerticalBox::Slot()
				.Padding(10, 0, 10, 10)
				.FillHeight(1.0f)
				[
					SAssignNew(InputBox, SEditableTextBox)
					.HintText(LOCTEXT("QuickAskHint", "Type your question here..."))
					.OnTextCommitted_Lambda([QuickAskWindow](const FText& Text, ETextCommit::Type CommitType)
					{
						if (CommitType == ETextCommit::OnEnter && !Text.IsEmpty())
						{
							// Close the window
							QuickAskWindow->RequestDestroyWindow();

							// Send prompt to Claude
							FString Prompt = Text.ToString();
							FOsvayderPromptOptions Options;
							Options.bIncludeEngineContext = true;
							Options.bIncludeProjectContext = true;
							FOsvayderCodeSubsystem::Get().SendPrompt(
								Prompt,
								FOnOsvayderResponse::CreateLambda([](const FString& Response, bool bSuccess)
								{
									// Show response in notification
									FNotificationInfo Info(FText::FromString(
										bSuccess
											? (Response.Len() > 300 ? Response.Left(300) + TEXT("...") : Response)
											: TEXT("Error: ") + Response));
									Info.ExpireDuration = bSuccess ? 15.0f : 5.0f;
									Info.bUseLargeFont = false;
									Info.bUseSuccessFailIcons = true;
									FSlateNotificationManager::Get().AddNotification(Info);
								}),
								Options
							);
						}
					})
				]
			);

			FSlateApplication::Get().AddWindow(QuickAskWindow);

			// Focus the input box
			if (InputBox.IsValid())
			{
				FSlateApplication::Get().SetKeyboardFocus(InputBox);
			}
		}),
		FCanExecuteAction::CreateLambda([]()
		{
			return AgentBackendCanExecutePrompt(FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus());
		})
	);

	// Register the tab spawner
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		ClaudeTabName,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs& Args) -> TSharedRef<SDockTab>
		{
			const TSharedRef<SOsvayderEditorWidget> OsvayderEditorWidget = SNew(SOsvayderEditorWidget);
			SOsvayderEditorWidget::RegisterLiveWidget(OsvayderEditorWidget);

			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				.Label(LOCTEXT("ClaudeTabTitle", "Osvayder UE"))
				[
					OsvayderEditorWidget
				];
		}))
		.SetDisplayName(LOCTEXT("ClaudeTabTitle", "Osvayder UE"))
		.SetTooltipText(LOCTEXT("ClaudeTabTooltip", "Open the Osvayder UE assistant for UE5.7 development help"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Help"));
	
	// Register menus after engine init
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FOsvayderUEModule::RegisterMenus));

	// Bind keyboard shortcuts to the Level Editor
	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	LevelEditorModule.GetGlobalLevelEditorActions()->Append(PluginCommands.ToSharedRef());

	// Check configured backend availability
	const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
	if (BackendStatus.bReady)
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("%s"), *BackendStatus.Detail);
	}
	else
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *BackendStatus.Detail);
	}

	const FOsvayderUEBuildSyncStatus BuildSyncStatus = GetBuildSyncStatus();
	if (BuildSyncStatus.bFresh)
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("%s"), *BuildSyncStatus.Detail);
	}
	else
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *BuildSyncStatus.Detail);
		ShowBuildSyncNotification(BuildSyncStatus);
	}

	// Start MCP Server
	StartMCPServer();

	// Initialize project context (async, will gather in background)
	FProjectContextManager::Get().RefreshContext();

	// Initialize script execution manager (creates script directories)
	FScriptExecutionManager::Get();

	ScheduleCommandLineHeadlessAcceptanceRequest();

	// 628-v3: run the engine-patch-presence probe at module startup.
	// Emits warning + agent_trace event if the LiveCodingLimit=1000 patch
	// was overwritten by an engine reinstall. Pure check; never modifies
	// engine source. See OsvayderUEEnginePatchProbe.cpp for details.
	FOsvayderUEEnginePatchProbe::RunStartupProbe();

	// 632: task recovery probe at module startup. Detects an interrupted
	// autonomous task from Saved/OsvayderUE/active_plan.json (status=active
	// + result_status=incomplete + >5min stale), persists interruption_*
	// metadata back to the plan, and emits a task_recovery_detected
	// agent_trace event. The widget dialog that surfaces the 3-option
	// recovery UX reads the persisted metadata on first Tick.
	FOsvayderUETaskRecoveryDetector::RunStartupProbe();
}

void FOsvayderUEModule::ShutdownModule()
{
	UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderUE module shutting down"));
	UnregisterSettingsUiCustomization();

	// Stop MCP Server
	StopMCPServer();

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	FOsvayderUECommands::Unregister();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ClaudeTabName);
}

FOsvayderUEModule& FOsvayderUEModule::Get()
{
	return FModuleManager::LoadModuleChecked<FOsvayderUEModule>("OsvayderUE");
}

bool FOsvayderUEModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded("OsvayderUE");
}

TSharedPtr<SOsvayderEditorWidget> FOsvayderUEModule::OpenClaudePanelAndGetWidget()
{
	FGlobalTabmanager::Get()->TryInvokeTab(ClaudeTabName);
	return SOsvayderEditorWidget::GetLiveWidget();
}

void FOsvayderUEModule::RegisterMenus()
{
	// Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
	FToolMenuOwnerScoped OwnerScoped(this);
	
	// Add to the main menu bar under Tools
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("OsvayderUE");

		Section.AddMenuEntryWithCommandList(
			FOsvayderUECommands::Get().OpenClaudePanel,
			PluginCommands,
			LOCTEXT("OpenClaudeMenuItem", "Osvayder UE"),
			LOCTEXT("OpenClaudeMenuItemTooltip", "Open the Osvayder UE assistant for UE5.7 help (Ctrl+Shift+C)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Help")
		);

		Section.AddMenuEntryWithCommandList(
			FOsvayderUECommands::Get().QuickAsk,
			PluginCommands,
			LOCTEXT("QuickAskMenuItem", "Quick Ask"),
			LOCTEXT("QuickAskMenuItemTooltip", "Quickly ask the active assistant provider a question (Ctrl+Alt+C)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Help")
		);
	}
	
	// Add to the toolbar
	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("OsvayderUE");
		
		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			FOsvayderUECommands::Get().OpenClaudePanel,
			LOCTEXT("ClaudeToolbarButton", "Osvayder UE"),
			LOCTEXT("ClaudeToolbarTooltip", "Open Osvayder UE (Ctrl+Shift+C)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Help")
		));
	}
}

void FOsvayderUEModule::UnregisterMenus()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FOsvayderUEModule::StartMCPServer()
{
	if (MCPServer.IsValid())
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("MCP Server already exists"));
		return;
	}

	MCPServer = MakeShared<FOsvayderUEMCPServer>();

	if (!MCPServer->Start(GetMCPServerPort()))
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to start MCP Server on port %d"), GetMCPServerPort());
		MCPServer.Reset();
	}
}

void FOsvayderUEModule::StopMCPServer()
{
	if (MCPServer.IsValid())
	{
		MCPServer->Stop();
		MCPServer.Reset();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOsvayderUEModule, OsvayderUE)
