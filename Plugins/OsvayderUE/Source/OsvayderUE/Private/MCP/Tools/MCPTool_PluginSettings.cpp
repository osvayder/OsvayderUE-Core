// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_PluginSettings.h"
#include "AgentExecutionControl.h"
#include "OsvayderSessionManager.h"
#include "OsvayderSubsystem.h"
#include "CodexCliRunner.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUECapabilityRiskManifest.h"
#include "OsvayderUEModule.h"
#include "OsvayderUERestartSurvival.h"
#include "OsvayderUESettings.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEReportArtifacts.h"

FMCPToolResult FMCPTool_PluginSettings::Execute(const TSharedRef<FJsonObject>& Params)
{
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	if (!Settings)
	{
		return FMCPToolResult::Error(TEXT("Settings not available"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// Scope
	TSharedPtr<FJsonObject> ScopeObj = MakeShared<FJsonObject>();
	ScopeObj->SetStringField(TEXT("mode"), Settings->ScopeMode == EOsvayderUEScopeMode::PluginOnly ? TEXT("PluginOnly") : TEXT("PluginAndProject"));
	Data->SetObjectField(TEXT("scope"), ScopeObj);

	// Verification
	TSharedPtr<FJsonObject> VerifObj = MakeShared<FJsonObject>();
	VerifObj->SetStringField(TEXT("mode"), Settings->VerificationMode == EOsvayderUEVerificationMode::PracticalEditorFirst ? TEXT("PracticalEditorFirst") : TEXT("Mixed"));
	VerifObj->SetBoolField(TEXT("prefer_short_checks"), Settings->bPreferShortPracticalChecks);
	VerifObj->SetBoolField(TEXT("osvayder_fallback"), Settings->bUseOsvayderAsFallback);
	Data->SetObjectField(TEXT("verification"), VerifObj);

	// Architecture
	TSharedPtr<FJsonObject> ArchObj = MakeShared<FJsonObject>();
	ArchObj->SetBoolField(TEXT("multiplayer_first"), Settings->bMultiplayerFirst);
	ArchObj->SetBoolField(TEXT("cpp_for_authority"), Settings->bPreferCppForAuthority);
	ArchObj->SetBoolField(TEXT("blueprint_for_orchestration"), Settings->bPreferBlueprintForOrchestration);
	Data->SetObjectField(TEXT("architecture"), ArchObj);

	// MCP
	TSharedPtr<FJsonObject> McpObj = MakeShared<FJsonObject>();
	McpObj->SetNumberField(TEXT("port"), Settings->MCPServerPort);
	Data->SetObjectField(TEXT("mcp_server"), McpObj);

	// OsvayderEye
	TSharedPtr<FJsonObject> EyeObj = MakeShared<FJsonObject>();
	EyeObj->SetBoolField(TEXT("enabled"), Settings->bEnableOsvayderEye);
	EyeObj->SetStringField(TEXT("url"), Settings->OsvayderEyeUrl);
	EyeObj->SetStringField(TEXT("server_path"), Settings->OsvayderEyeServerPath.FilePath);
	EyeObj->SetStringField(TEXT("python_path"), Settings->OsvayderEyePythonPath.FilePath);
	Data->SetObjectField(TEXT("osvayder_eye"), EyeObj);

	// Assistant UI settings
	TSharedPtr<FJsonObject> AssistObj = MakeShared<FJsonObject>();
	AssistObj->SetStringField(TEXT("model"), Settings->DefaultModel);
	AssistObj->SetNumberField(TEXT("max_tokens"), Settings->MaxResponseTokens);
	Data->SetObjectField(TEXT("claude_assistant"), AssistObj);

	// Dictation
	TSharedPtr<FJsonObject> DictationObj = MakeShared<FJsonObject>();
	DictationObj->SetStringField(TEXT("runtime"), Settings->GetDictationRuntimeLabel());
	DictationObj->SetStringField(TEXT("language_requested"), Settings->GetConfiguredDictationLanguageModeName());
	DictationObj->SetStringField(TEXT("language_effective"), Settings->GetEffectiveDictationLanguageTag());
	DictationObj->SetStringField(TEXT("language_display_name"), Settings->GetEffectiveDictationLanguageDisplayName());
	DictationObj->SetStringField(TEXT("model_name"), Settings->GetEffectiveDictationModelName());
	DictationObj->SetStringField(TEXT("model_url"), Settings->GetEffectiveDictationModelUrl());
	DictationObj->SetBoolField(TEXT("supports_russian_offline"), Settings->SupportsRussianOfflineDictation());
	DictationObj->SetStringField(TEXT("support_claim"), Settings->GetDictationSupportClaimName());
	DictationObj->SetStringField(TEXT("support_detail"), Settings->GetDictationSupportDetail());
	Data->SetObjectField(TEXT("dictation"), DictationObj);

	// Backend selection and runtime status
	const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
	const FAgentSavedSessionIndex SavedSessions = FOsvayderCodeSubsystem::Get().DescribeSavedSessions();
	TSharedPtr<FJsonObject> BackendObj = MakeShared<FJsonObject>();
	BackendObj->SetStringField(TEXT("preferred"), Settings->PreferredBackend == EOsvayderUEProviderBackend::CodexCli ? TEXT("CodexCli") : TEXT("ClaudeCli"));
	BackendObj->SetStringField(TEXT("current"), BackendStatus.DisplayName);
	BackendObj->SetStringField(TEXT("backend"), BackendStatus.Backend == EOsvayderUEProviderBackend::CodexCli ? TEXT("CodexCli") : TEXT("ClaudeCli"));
	BackendObj->SetBoolField(TEXT("available"), BackendStatus.bAvailable);
	BackendObj->SetBoolField(TEXT("ready"), BackendStatus.bReady);
	BackendObj->SetStringField(TEXT("detail"), BackendStatus.Detail);
	BackendObj->SetStringField(TEXT("executable_path"), BackendStatus.ExecutablePath);
	BackendObj->SetStringField(TEXT("auth_state"), AgentBackendAuthStateToString(BackendStatus.AuthState));
	BackendObj->SetStringField(TEXT("auth_detail"), BackendStatus.AuthDetail);
	BackendObj->SetStringField(TEXT("readiness"), AgentBackendReadinessToString(BackendStatus.Readiness));
	BackendObj->SetBoolField(TEXT("supports_structured_output"), BackendStatus.Capabilities.bUsesStructuredOutput);
	BackendObj->SetBoolField(TEXT("supports_browser_verify_login"), BackendStatus.Capabilities.bSupportsBrowserVerifyLogin);
	BackendObj->SetBoolField(TEXT("supports_provider_persistent_threads"), BackendStatus.Capabilities.bSupportsProviderPersistentThreads);
	BackendObj->SetBoolField(TEXT("supports_reasoning_effort_control"), BackendStatus.Capabilities.bSupportsReasoningEffortControl);
	BackendObj->SetBoolField(TEXT("supports_verbosity_control"), BackendStatus.Capabilities.bSupportsVerbosityControl);
	BackendObj->SetBoolField(TEXT("supports_speed_mode_control"), BackendStatus.Capabilities.bSupportsSpeedModeControl);
	BackendObj->SetBoolField(TEXT("supports_profile_selection"), BackendStatus.Capabilities.bSupportsProfileSelection);
	BackendObj->SetBoolField(TEXT("supports_explicit_auth_mode_selection"), BackendStatus.Capabilities.bSupportsExplicitAuthModeSelection);

	TSharedPtr<FJsonObject> CapabilitiesObj = MakeShared<FJsonObject>();
	CapabilitiesObj->SetBoolField(TEXT("streaming_events"), BackendStatus.Capabilities.bSupportsStreamingEvents);
	CapabilitiesObj->SetBoolField(TEXT("images"), BackendStatus.Capabilities.bSupportsImages);
	CapabilitiesObj->SetBoolField(TEXT("cancellation"), BackendStatus.Capabilities.bSupportsCancellation);
	CapabilitiesObj->SetBoolField(TEXT("tool_allow_list"), BackendStatus.Capabilities.bSupportsToolAllowList);
	CapabilitiesObj->SetBoolField(TEXT("structured_output"), BackendStatus.Capabilities.bUsesStructuredOutput);
	CapabilitiesObj->SetBoolField(TEXT("browser_verify_login"), BackendStatus.Capabilities.bSupportsBrowserVerifyLogin);
	CapabilitiesObj->SetBoolField(TEXT("provider_persistent_threads"), BackendStatus.Capabilities.bSupportsProviderPersistentThreads);
	CapabilitiesObj->SetBoolField(TEXT("reasoning_effort_control"), BackendStatus.Capabilities.bSupportsReasoningEffortControl);
	CapabilitiesObj->SetBoolField(TEXT("verbosity_control"), BackendStatus.Capabilities.bSupportsVerbosityControl);
	CapabilitiesObj->SetBoolField(TEXT("speed_mode_control"), BackendStatus.Capabilities.bSupportsSpeedModeControl);
	CapabilitiesObj->SetBoolField(TEXT("profile_selection"), BackendStatus.Capabilities.bSupportsProfileSelection);
	CapabilitiesObj->SetBoolField(TEXT("explicit_auth_mode_selection"), BackendStatus.Capabilities.bSupportsExplicitAuthModeSelection);
	BackendObj->SetObjectField(TEXT("capabilities"), CapabilitiesObj);

	BackendObj->SetStringField(TEXT("codex_model"), Settings->GetConfiguredCodexModel());
	BackendObj->SetStringField(TEXT("codex_profile"), Settings->GetEffectiveCodexProfileLabel());
	BackendObj->SetBoolField(TEXT("codex_profile_is_explicit"), Settings->HasExplicitCodexProfile());
	BackendObj->SetBoolField(TEXT("codex_supports_speed_mode"), true);
	BackendObj->SetStringField(TEXT("codex_speed_mode_requested"), Settings->GetConfiguredCodexSpeedModeName());
	BackendObj->SetStringField(TEXT("codex_speed_mode_effective"), FCodexCliRunner::GetEffectiveSpeedModeName());
	BackendObj->SetStringField(TEXT("codex_speed_mode_support"), FCodexCliRunner::GetSpeedModeSupportLabel());
	BackendObj->SetStringField(TEXT("codex_work_mode"), Settings->GetConfiguredCodexWorkModeName());
	BackendObj->SetBoolField(TEXT("codex_supports_reasoning_effort"), true);
	BackendObj->SetStringField(TEXT("codex_reasoning_effort_requested"), Settings->GetConfiguredCodexReasoningEffortName());
	BackendObj->SetStringField(TEXT("codex_reasoning_effort_effective"), Settings->GetConfiguredCodexReasoningEffortName());
	BackendObj->SetBoolField(TEXT("codex_reasoning_effort_is_explicit"), Settings->HasExplicitCodexReasoningEffortOverride());
	BackendObj->SetStringField(TEXT("codex_reasoning_effort_support"), Settings->GetCodexReasoningSupportLabel());
	BackendObj->SetBoolField(TEXT("codex_supports_verbosity"), true);
	BackendObj->SetStringField(TEXT("codex_verbosity_requested"), Settings->GetConfiguredCodexVerbosityName());
	BackendObj->SetStringField(TEXT("codex_verbosity_effective"), Settings->GetConfiguredCodexVerbosityName());
	BackendObj->SetBoolField(TEXT("codex_verbosity_is_explicit"), Settings->HasExplicitCodexVerbosityOverride());
	BackendObj->SetStringField(TEXT("codex_verbosity_support"), Settings->GetCodexVerbositySupportLabel());
	BackendObj->SetStringField(TEXT("codex_home_path"), FCodexCliRunner::GetConfiguredCodexHomePath());
	BackendObj->SetStringField(TEXT("codex_home_resolution_source"), FCodexCliRunner::GetConfiguredCodexHomeResolutionSource());
	BackendObj->SetStringField(TEXT("codex_machine_standard_home"), FCodexCliRunner::GetMachineStandardCodexHomePath());
	BackendObj->SetBoolField(TEXT("codex_artifact_present_in_configured_home"), FCodexCliRunner::IsConfiguredCodexHomeArtifactPresent());
	TArray<TSharedPtr<FJsonValue>> DetectedCandidateHomesJson;
	for (const FString& CandidateHome : FCodexCliRunner::GetDetectedCodexCandidateHomes())
	{
		DetectedCandidateHomesJson.Add(MakeShared<FJsonValueString>(CandidateHome));
	}
	BackendObj->SetArrayField(TEXT("codex_detected_candidate_homes"), DetectedCandidateHomesJson);
	BackendObj->SetStringField(TEXT("codex_detected_artifact_home"), FCodexCliRunner::GetDetectedCodexArtifactHomePath());
	BackendObj->SetBoolField(TEXT("codex_home_is_explicit"), FCodexCliRunner::HasExplicitCodexHomeOverride());
	BackendObj->SetStringField(TEXT("codex_auth_mode"), FCodexCliRunner::GetConfiguredAuthModeName());
	BackendObj->SetStringField(TEXT("codex_auth_env_var"), FCodexCliRunner::GetConfiguredApiKeyEnvVarName());
	BackendObj->SetStringField(TEXT("codex_auth_effective_path"), FCodexCliRunner::GetEffectiveAuthEntryPath());
	BackendObj->SetStringField(TEXT("codex_auth_ownership_model"), FCodexCliRunner::GetEffectiveAuthOwnershipModel());
	BackendObj->SetBoolField(TEXT("codex_exec_clear_proxy_env"), Settings->ShouldClearProxyEnvForCodexExec());
	BackendObj->SetBoolField(TEXT("codex_browser_verify_clear_proxy_env"), Settings->ShouldClearProxyEnvForBrowserVerify());
	BackendObj->SetBoolField(TEXT("codex_persistent_conversation_transport"), FCodexCliRunner::ShouldUsePersistentConversationTransport());
	BackendObj->SetStringField(TEXT("codex_execution_transport"), FCodexCliRunner::ShouldUsePersistentConversationTransport()
		? TEXT("persistent_app_server")
		: TEXT("exec_per_message"));

	// Spec 621 §5: Claude runtime status fields (parity with codex_* layout above).
	const bool bClaudePersistentSessionEnabled = Settings->ShouldUseClaudePersistentSession();
	BackendObj->SetStringField(TEXT("claude_persistent_session_transport"),
		bClaudePersistentSessionEnabled ? TEXT("session-id+resume") : TEXT("none"));
	BackendObj->SetStringField(TEXT("claude_execution_transport"), TEXT("print-stream-json"));
	{
		FOsvayderSessionManager TempSessionManager;
		BackendObj->SetStringField(TEXT("claude_persistent_session_id"),
			TempSessionManager.ReadPersistentSessionId(EOsvayderUEProviderBackend::ClaudeCli));
	}
	BackendObj->SetBoolField(TEXT("claude_forward_language"), Settings->ShouldForwardLanguageToClaudeSystemPrompt());
	// Plan 623: surface the configured Claude effort level. Empty string ⇔ Default setting ⇔ CLI default applies.
	BackendObj->SetStringField(TEXT("claude_effort_level"), Settings->GetConfiguredClaudeEffortLevelName());
	// 619 P6: surface the bAllowAutoEnableLiveCoding setting. Default true;
	// when false, livecoding_compile returns a structured error instead of
	// calling ILiveCodingModule::EnableForSession(true). Agents inspecting
	// this field can decide whether to suggest toggling the setting to the
	// user before retrying livecoding_compile.
	BackendObj->SetBoolField(TEXT("claude_allow_auto_enable_live_coding"), Settings->bAllowAutoEnableLiveCoding);
	BackendObj->SetBoolField(TEXT("assistant_auto_restore_session_on_open"), Settings->ShouldAutoRestoreSessionOnOpen());
	FString BrowserVerifyHint;
	BackendObj->SetBoolField(TEXT("codex_browser_verify_can_launch"), FCodexCliRunner::CanLaunchBrowserVerify(BrowserVerifyHint));
	BackendObj->SetStringField(TEXT("codex_browser_verify_hint"), BrowserVerifyHint);
	const FOsvayderUEBuildSyncStatus BuildSyncStatus = FOsvayderUEModule::GetBuildSyncStatus();
	BackendObj->SetBoolField(TEXT("plugin_build_binary_present"), BuildSyncStatus.bBinaryPresent);
	BackendObj->SetBoolField(TEXT("plugin_build_fresh"), BuildSyncStatus.bFresh);
	BackendObj->SetStringField(TEXT("plugin_build_detail"), BuildSyncStatus.Detail);
	BackendObj->SetStringField(TEXT("plugin_build_binary_path"), BuildSyncStatus.BinaryPath);
	BackendObj->SetStringField(TEXT("plugin_build_latest_source_path"), BuildSyncStatus.LatestSourcePath);
	BackendObj->SetStringField(TEXT("plugin_build_binary_timestamp"), BuildSyncStatus.BinaryTimestamp == FDateTime::MinValue() ? FString() : BuildSyncStatus.BinaryTimestamp.ToIso8601());
	BackendObj->SetStringField(TEXT("plugin_build_latest_source_timestamp"), BuildSyncStatus.LatestSourceTimestamp == FDateTime::MinValue() ? FString() : BuildSyncStatus.LatestSourceTimestamp.ToIso8601());
	BackendObj->SetStringField(TEXT("plugin_preflight_launcher"), FOsvayderUEModule::GetPreflightLauncherPath());

	const FAgentProviderExecutionControlManifest ExecutionControlManifest = FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifest();
	const FAgentProviderExecutionControlManifest BoundedExecutionControlManifest =
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile::BoundedPluginMutation);
	const FAgentProviderExecutionControlManifest ReadOnlyExecutionControlManifest =
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile::ReadOnlyDiagnostic);
	const FAgentProviderExecutionControlManifest ExplicitExpertExecutionControlManifest =
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);
	BackendObj->SetStringField(TEXT("execution_control_profile_id"), ExecutionControlManifest.ControlProfileId);
	BackendObj->SetStringField(TEXT("execution_transport"), ExecutionControlManifest.ExecutionTransportLabel);
	BackendObj->SetStringField(TEXT("session_persistence_mode"), AgentSessionPersistenceModeToString(ExecutionControlManifest.SessionPersistenceMode));
	BackendObj->SetStringField(TEXT("current_effective_provider_power"), AgentExecutionPowerClassToString(ExecutionControlManifest.CurrentEffectiveProviderPowerClass));
	BackendObj->SetStringField(TEXT("desired_future_default_provider_power"), AgentExecutionPowerClassToString(ExecutionControlManifest.DesiredFutureDefaultProviderPowerClass));
	BackendObj->SetStringField(TEXT("execution_control_plumbing_state"), AgentExecutionGovernanceStateToString(ExecutionControlManifest.ExecutionControlPlumbingState));
	BackendObj->SetBoolField(TEXT("normal_provider_session_history_touched"), ExecutionControlManifest.bTouchesNormalProviderSessionHistory);
	BackendObj->SetBoolField(TEXT("provider_session_file_updated_on_success"), ExecutionControlManifest.bWritesProviderSessionFileOnSuccess);
	BackendObj->SetObjectField(TEXT("provider_execution_control"), MakeAgentProviderExecutionControlJson(ExecutionControlManifest));
	TSharedPtr<FJsonObject> ExecutionProfilesObj = MakeShared<FJsonObject>();
	ExecutionProfilesObj->SetObjectField(TEXT("configured_default_runtime"), MakeAgentProviderExecutionControlJson(ExecutionControlManifest));
	ExecutionProfilesObj->SetObjectField(TEXT("bounded_plugin_mutation"), MakeAgentProviderExecutionControlJson(BoundedExecutionControlManifest));
	ExecutionProfilesObj->SetObjectField(TEXT("read_only_diagnostic"), MakeAgentProviderExecutionControlJson(ReadOnlyExecutionControlManifest));
	ExecutionProfilesObj->SetObjectField(TEXT("explicit_expert_opt_in"), MakeAgentProviderExecutionControlJson(ExplicitExpertExecutionControlManifest));
	BackendObj->SetObjectField(TEXT("execution_profiles"), ExecutionProfilesObj);

	auto BuildSessionMetadataObject = [](const FAgentSessionMetadata& Metadata)
	{
		TSharedPtr<FJsonObject> SessionMetadataObj = MakeShared<FJsonObject>();
		SessionMetadataObj->SetBoolField(TEXT("exists"), Metadata.bHasSession);
		SessionMetadataObj->SetBoolField(TEXT("readable"), Metadata.bIsReadable);
		SessionMetadataObj->SetBoolField(TEXT("legacy_shared_file"), Metadata.bIsLegacySharedFile);
		SessionMetadataObj->SetStringField(TEXT("backend"), OsvayderUEProviderBackendToString(Metadata.Backend));
		SessionMetadataObj->SetStringField(TEXT("backend_display_name"), Metadata.BackendDisplayName);
		SessionMetadataObj->SetStringField(TEXT("store_kind"), Metadata.StoreKind);
		SessionMetadataObj->SetStringField(TEXT("model"), Metadata.Model);
		SessionMetadataObj->SetStringField(TEXT("profile"), Metadata.Profile);
		SessionMetadataObj->SetStringField(TEXT("auth_mode"), Metadata.AuthMode);
		SessionMetadataObj->SetStringField(TEXT("path"), Metadata.SessionFilePath);
		SessionMetadataObj->SetStringField(TEXT("last_updated"), Metadata.LastUpdated);
		SessionMetadataObj->SetNumberField(TEXT("message_count"), Metadata.MessageCount);
		SessionMetadataObj->SetStringField(TEXT("detail"), Metadata.Detail);
		return SessionMetadataObj;
	};

	TSharedPtr<FJsonObject> SessionObj = MakeShared<FJsonObject>();
	SessionObj->SetObjectField(TEXT("current_provider"), BuildSessionMetadataObject(SavedSessions.CurrentProviderSession));
	SessionObj->SetObjectField(TEXT("other_provider"), BuildSessionMetadataObject(SavedSessions.OtherProviderSession));
	SessionObj->SetObjectField(TEXT("legacy_shared"), BuildSessionMetadataObject(SavedSessions.LegacySharedSession));
	SessionObj->SetObjectField(
		TEXT("explicit_expert_opt_in_provider"),
		BuildSessionMetadataObject(
			FOsvayderCodeSubsystem::Get().DescribeSavedSessionsForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn).CurrentProviderSession));
	SessionObj->SetBoolField(TEXT("has_any_saved_session"), SavedSessions.HasAnySavedSession());
	SessionObj->SetBoolField(TEXT("has_current_provider_session"), SavedSessions.HasCurrentProviderSession());
	BackendObj->SetObjectField(TEXT("session"), SessionObj);
	BackendObj->SetObjectField(TEXT("restart_survival"), FOsvayderUERestartSurvivalManager::BuildReadbackJson());
	Data->SetObjectField(TEXT("assistant_backend"), BackendObj);

	const FAgentCapabilityRiskManifest CapabilityRiskManifest =
		BuildAgentCapabilityRiskManifest(
			ExecutionControlManifest,
			ReadOnlyExecutionControlManifest,
			BoundedExecutionControlManifest,
			ExplicitExpertExecutionControlManifest);
	Data->SetObjectField(TEXT("capability_risk_manifest"), MakeAgentCapabilityRiskManifestJson(CapabilityRiskManifest));

	// Paths
	TSharedPtr<FJsonObject> PathsObj = MakeShared<FJsonObject>();
	PathsObj->SetStringField(TEXT("project_memory"), Settings->ProjectMemoryPath);
	PathsObj->SetStringField(TEXT("agent_bridge"), Settings->AgentBridgePath);
	PathsObj->SetStringField(TEXT("report_artifacts_dir"), FOsvayderUEReportArtifacts::GetReportsDirectory());
	PathsObj->SetStringField(TEXT("report_artifact_summary_glob"), FOsvayderUEReportArtifacts::GetReportSummaryGlob());
	Data->SetObjectField(TEXT("paths"), PathsObj);

	// Logging
	Data->SetBoolField(TEXT("verbose_logging"), Settings->bVerboseToolLogging);
	Data->SetStringField(TEXT("execution_log_path"), FOsvayderUEExecutionLog::Get().GetSessionLogPath());
	Data->SetStringField(TEXT("agent_trace_log_path"), FOsvayderUEAgentTraceLog::Get().GetTraceLogPath());

	return FMCPToolResult::Success(TEXT("Current Osvayder UE settings"), Data);
}
