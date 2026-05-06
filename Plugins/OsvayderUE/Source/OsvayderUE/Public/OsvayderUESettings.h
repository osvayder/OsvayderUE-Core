// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"
#include "Engine/DeveloperSettings.h"
#include "OsvayderUESettings.generated.h"

/** Plugin write scope policy */
UENUM()
enum class EOsvayderUEScopeMode : uint8
{
	/** Plugin files only (Plugins/OsvayderUE, Docs/OsvayderUE, AgentBridge) */
	PluginOnly UMETA(DisplayName="Plugin Only"),

	/** Plugin + project gameplay code and assets */
	PluginAndProject UMETA(DisplayName="Plugin and Project"),
};

/** Verification approach */
UENUM()
enum class EOsvayderUEVerificationMode : uint8
{
	/** Short practical checks in the editor first */
	PracticalEditorFirst UMETA(DisplayName="Practical Editor First"),

	/** Mix of practical checks and automated tests */
	Mixed UMETA(DisplayName="Mixed"),
};

/** Codex authentication entry selection */
UENUM()
enum class EOsvayderUECodexAuthMode : uint8
{
	/** Preserve the accepted AP3 behavior: inherited OPENAI_API_KEY first, then CLI-managed Codex artifacts. */
	Auto UMETA(DisplayName="Auto"),

	/** Read a named editor env var and forward it to child Codex launches as OPENAI_API_KEY. */
	ApiKeyEnvVar UMETA(DisplayName="API"),

	/** Use the normal Codex home, but assume the user manages `codex login` externally in a terminal. */
	CliTerminal UMETA(DisplayName="CLI Terminal"),

	/** Use plugin-owned Browser Verify home and let the plugin request a ChatGPT browser login URL from Codex app-server. */
	BrowserVerify UMETA(DisplayName="Browser Verify"),
};

/** High-level Codex work mode preset shown in Unreal settings/UI */
UENUM()
enum class EOsvayderUECodexWorkMode : uint8
{
	Fast UMETA(DisplayName="Fast"),
	Balanced UMETA(DisplayName="Balanced"),
	Deep UMETA(DisplayName="Deep"),
	Max UMETA(DisplayName="Max"),
	Custom UMETA(DisplayName="Custom"),
};

/** Requested Codex reasoning effort override */
UENUM()
enum class EOsvayderUECodexReasoningEffort : uint8
{
	ModelDefault UMETA(DisplayName="Model Default"),
	Low UMETA(DisplayName="Low"),
	Medium UMETA(DisplayName="Medium"),
	High UMETA(DisplayName="High"),
	VeryHigh UMETA(DisplayName="Very High"),
};

/** Requested Codex response detail / verbosity override */
UENUM()
enum class EOsvayderUECodexVerbosity : uint8
{
	ModelDefault UMETA(DisplayName="Model Default"),
	Low UMETA(DisplayName="Low"),
	Medium UMETA(DisplayName="Medium"),
	High UMETA(DisplayName="High"),
};

/** Requested Codex speed mode */
UENUM()
enum class EOsvayderUECodexSpeedMode : uint8
{
	Standard UMETA(DisplayName="Standard"),
	Fast UMETA(DisplayName="Fast"),
};

/** Requested Claude CLI effort level for the legacy Claude backend. */
UENUM()
enum class EOsvayderUEClaudeEffortLevel : uint8
{
	Default UMETA(DisplayName="Default (CLI decides)"),
	Low UMETA(DisplayName="Low"),
	Medium UMETA(DisplayName="Medium"),
	High UMETA(DisplayName="High"),
	XHigh UMETA(DisplayName="XHigh"),
	Max UMETA(DisplayName="Max"),
};

/** Requested offline dictation language */
UENUM()
enum class EOsvayderUEDictationLanguage : uint8
{
	/** Use the project-configured fallback or infer English when no explicit language is selected. */
	Auto UMETA(DisplayName="Auto"),

	/** Use the bounded lightweight US English offline Vosk model. */
	EnglishUs UMETA(DisplayName="English (US)"),

	/** Use the bounded lightweight Russian offline Vosk model. */
	RussianRu UMETA(DisplayName="Russian"),
};

/**
 * Osvayder UE Plugin Settings
 * Configurable in Editor: Project Settings -> Plugins -> Osvayder UE
 */
UCLASS(config=Game, defaultconfig, meta=(DisplayName="Osvayder UE"))
class OSVAYDERUE_API UOsvayderUESettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UOsvayderUESettings();

	// Scope / Safety

	/** Plugin write scope policy */
	UPROPERTY(config, EditAnywhere, Category="General", meta=(DisplayName="Write Scope", ToolTip="How far Osvayder UE is allowed to write: only plugin-owned files, or plugin plus project files."))
	EOsvayderUEScopeMode ScopeMode = EOsvayderUEScopeMode::PluginOnly;

	// Verification Policy

	/** How the plugin verifies its operations */
	UPROPERTY(config, EditAnywhere, Category="Verification", meta=(DisplayName="Verification Mode", ToolTip="Choose whether Osvayder UE should prefer short editor checks or a mixed verification style."))
	EOsvayderUEVerificationMode VerificationMode = EOsvayderUEVerificationMode::PracticalEditorFirst;

	/** Prefer short practical checks over heavy test suites */
	UPROPERTY(config, EditAnywhere, Category="Verification", meta=(DisplayName="Prefer Short Practical Checks", ToolTip="Use short in-editor checks first when they are enough, instead of always running heavier verification."))
	bool bPreferShortPracticalChecks = true;

	/** Use OsvayderEye as fallback for verification when semantic tools are unavailable */
	UPROPERTY(config, EditAnywhere, Category="Verification", meta=(DisplayName="Use OsvayderEye As Fallback", ToolTip="Allow screen-control verification only when semantic Unreal tools are not enough."))
	bool bUseOsvayderAsFallback = true;

	// Architecture Defaults

	/** Design multiplayer-first: decide authority before implementing mechanics */
	UPROPERTY(config, EditAnywhere, Category="Architecture", meta=(DisplayName="Multiplayer First", ToolTip="Prefer authority and replication decisions early instead of retrofitting them later."))
	bool bMultiplayerFirst = true;

	/** Prefer C++ for authority, replication, and core gameplay logic */
	UPROPERTY(config, EditAnywhere, Category="Architecture", meta=(DisplayName="Prefer C++ For Authority", ToolTip="Bias authoritative gameplay rules, replication, and core systems toward C++."))
	bool bPreferCppForAuthority = true;

	/** Prefer Blueprint for orchestration, wiring, and presentation */
	UPROPERTY(config, EditAnywhere, Category="Architecture", meta=(DisplayName="Prefer Blueprint For Orchestration", ToolTip="Bias orchestration, presentation, and designer-facing wiring toward Blueprint."))
	bool bPreferBlueprintForOrchestration = true;

	// Paths

	/** Path to project memory docs (relative to project root) */
	UPROPERTY(config, EditAnywhere, Category="Paths", meta=(DisplayName="Project Memory Path", ToolTip="Relative path to the project memory/docs folder. Defaults to the legacy Docs/OsvayderUE location for compatibility."))
	FString ProjectMemoryPath = TEXT("Docs/OsvayderUE");

	/** Path to agent bridge (relative to project root) */
	UPROPERTY(config, EditAnywhere, Category="Paths", meta=(DisplayName="Agent Bridge Path", ToolTip="Relative path to the AgentBridge handoff folder inside the project."))
	FString AgentBridgePath = TEXT("AgentBridge");

	// MCP Server

	/** Port for Osvayder UE MCP HTTP server */
	UPROPERTY(config, EditAnywhere, Category="MCP", meta=(DisplayName="MCP Server Port", ClampMin=1024, ClampMax=65535, ToolTip="HTTP port used by the built-in Osvayder UE MCP server. Editor restart required after changes."))
	int32 MCPServerPort = 3000;

	// OsvayderEye Integration

	/** Enable OsvayderEye screen control tools */
	UPROPERTY(config, EditAnywhere, Category="OsvayderEye", meta=(DisplayName="Enable OsvayderEye", ToolTip="Enable the optional screen-control integration used only when semantic Unreal tools are not enough."))
	bool bEnableOsvayderEye = true;

	/** URL for OsvayderEye HTTP sidecar */
	UPROPERTY(config, EditAnywhere, Category="OsvayderEye", meta=(DisplayName="OsvayderEye URL", EditCondition="bEnableOsvayderEye", ToolTip="HTTP URL for the OsvayderEye sidecar when the integration is enabled."))
	FString OsvayderEyeUrl = TEXT("http://localhost:3002");

	/** Path to OsvayderEye server.py (for CLI bridge mode) */
	UPROPERTY(config, EditAnywhere, Category="OsvayderEye", meta=(DisplayName="OsvayderEye Server Path", EditCondition="bEnableOsvayderEye", ToolTip="Path to the OsvayderEye server.py file for CLI bridge mode."))
	FFilePath OsvayderEyeServerPath;

	/** Path to Python executable (for CLI bridge mode) */
	UPROPERTY(config, EditAnywhere, Category="OsvayderEye", meta=(DisplayName="OsvayderEye Python Path", EditCondition="bEnableOsvayderEye", ToolTip="Python executable used to launch OsvayderEye in CLI bridge mode."))
	FFilePath OsvayderEyePythonPath;

	/** Preferred backend for editor prompts. Codex is the public-beta runtime default; Claude remains a legacy/experimental compatibility path. */
	UPROPERTY(config, EditAnywhere, Category="General", meta=(DisplayName="Assistant Backend", ToolTip="Choose which assistant Osvayder UE should use for normal editor prompts. Codex CLI is the public-beta default; Claude CLI remains a legacy/experimental compatibility path."))
	EOsvayderUEProviderBackend PreferredBackend = EOsvayderUEProviderBackend::CodexCli;

	// Legacy/experimental Claude backend settings

	/** Default Claude model when the legacy Claude backend is selected. */
	UPROPERTY(config, EditAnywhere, Category="Claude (Legacy)", meta=(DisplayName="Claude Model", ToolTip="Default Claude model used when the legacy Claude backend is selected."))
	FString DefaultModel = TEXT("claude-opus-4-6");

	/** Default model to use for Codex CLI */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Model", ToolTip="Default Codex model used for normal Codex runs."))
	FString DefaultCodexModel = TEXT("gpt-5.4");

	/** Optional named Codex CLI profile from ~/.codex/config.toml. Empty means the CLI default profile. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="CLI Profile", ToolTip="Optional named Codex CLI profile from the resolved Codex home. Leave empty to use the CLI default profile."))
	FString DefaultCodexProfile;

	/** Requested Codex speed mode. This stays separate from the work-mode preset that drives reasoning and verbosity. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Speed", ToolTip="Requested speed tier for Codex. This is separate from Work Mode."))
	EOsvayderUECodexSpeedMode DefaultCodexSpeedMode = EOsvayderUECodexSpeedMode::Standard;

	/** High-level preset for how aggressively Codex should reason and how detailed it should respond. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Work Mode", ToolTip="High-level preset for Codex reasoning depth and response detail. Choose Custom to unlock the manual controls below."))
	EOsvayderUECodexWorkMode DefaultCodexWorkMode = EOsvayderUECodexWorkMode::Balanced;

	/** Manual reasoning effort used only when Codex work mode is Custom. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Reasoning (Custom)", EditCondition="DefaultCodexWorkMode == EOsvayderUECodexWorkMode::Custom", ToolTip="Manual reasoning setting used only when Work Mode is Custom."))
	EOsvayderUECodexReasoningEffort DefaultCodexReasoningEffort = EOsvayderUECodexReasoningEffort::Medium;

	/** Manual response detail used only when Codex work mode is Custom. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Response Detail (Custom)", EditCondition="DefaultCodexWorkMode == EOsvayderUECodexWorkMode::Custom", ToolTip="Manual response-detail setting used only when Work Mode is Custom."))
	EOsvayderUECodexVerbosity DefaultCodexVerbosity = EOsvayderUECodexVerbosity::Medium;

	/** Managed shared Codex home override. Empty means the runtime resolution ladder chooses the shared home (OSVAYDERUE_CODEX_HOME, legacy OSVAYDERUE_CODEX_HOME, then a known machine-standard artifact home, then ~/.codex). This is intended for machine bring-up when terminal Codex login intentionally lives in a different home such as C:/Users/<user>/.codex-cli, not for normal day-to-day UI editing. */
	UPROPERTY(config, VisibleAnywhere, Category="Codex", meta=(DisplayName="Home Override (Bring-Up Only)", ToolTip="Advanced bring-up field. Empty means Osvayder UE uses the accepted Codex home resolution ladder. This field stays read-only in Project Settings."))
	FString DefaultCodexHomeOverride;

	/** Codex auth entry mode. Auto preserves the accepted AP3 path; API uses the explicit env-var bridge; CLI Terminal assumes external `codex login`; Browser Verify requests a browser login URL from Codex and opens it from plugin UI. The widget shows the exact effective CODEX_HOME and repair actions for the selected mode. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Authentication", ToolTip="Choose how Osvayder UE should authenticate Codex: Auto, API env var, external CLI Terminal login, or Browser Verify. The widget diagnostics panel shows the exact effective CODEX_HOME/auth artifact and provides relogin/probe/backup-clear actions."))
	EOsvayderUECodexAuthMode DefaultCodexAuthMode = EOsvayderUECodexAuthMode::Auto;

	/** Named editor environment variable used when Codex auth mode is ApiKeyEnvVar. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Editor Env Var For API Key", EditCondition="DefaultCodexAuthMode == EOsvayderUECodexAuthMode::ApiKeyEnvVar", ToolTip="Used only when Authentication is API. Osvayder UE reads this editor env var and forwards it to Codex as OPENAI_API_KEY."))
	FString DefaultCodexApiKeyEnvVar = TEXT("OSVAYDERUE_OPENAI_API_KEY");

	/** If true, Browser Verify clears inherited proxy env vars before launching Codex and forces localhost into NO_PROXY. Use this only on machines where proxy env vars break OAuth callback or token exchange. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Browser Verify: Clear Proxy Env", EditCondition="DefaultCodexAuthMode == EOsvayderUECodexAuthMode::BrowserVerify", ToolTip="Browser Verify only. Clears inherited proxy variables before Osvayder UE requests a browser login from Codex app-server."))
	bool bCodexBrowserVerifyClearProxyEnv = false;

	/** If true, all normal Codex child launches (`auto` / `api` / `cli_terminal` / prompt execution after Browser Verify) clear inherited proxy env vars before launch and force localhost into NO_PROXY. Use this only on machines where ALL_PROXY / HTTP_PROXY / HTTPS_PROXY break Codex websocket or HTTPS transport. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Normal Runs: Clear Proxy Env", ToolTip="Normal Codex runs only. Clears inherited proxy variables before ordinary Codex execution paths."))
	bool bCodexExecClearProxyEnv = false;

	/** If true, Codex prompt execution uses a persistent `codex app-server` thread instead of spawning `codex exec` for every message. This keeps conversation state in a real Codex thread and avoids per-message process startup overhead. The auth diagnostics panel reports whether this transport is enabled. */
	UPROPERTY(config, EditAnywhere, Category="Codex", meta=(DisplayName="Persistent App Server", ToolTip="Keep one live codex app-server for normal Codex prompts instead of starting a fresh process every time. The Codex auth diagnostics panel reports this transport separately from auth state."))
	bool bCodexUsePersistentAppServer = true;

	/** If true, the widget automatically restores the latest saved session when reopened. */
	UPROPERTY(config, EditAnywhere, Category="General", meta=(DisplayName="Restore Last Session On Open", ToolTip="Automatically restore the latest saved chat/session when the Osvayder UE panel is reopened."))
	bool bAutoRestoreSessionOnOpen = true;

	/** Spoken language used by the bounded offline dictation lane. */
	UPROPERTY(config, EditAnywhere, Category="Dictation", meta=(DisplayName="Language", ToolTip="Choose which spoken language offline dictation should transcribe."))
	EOsvayderUEDictationLanguage DefaultDictationLanguage = EOsvayderUEDictationLanguage::Auto;

	/** Maximum response tokens */
	UPROPERTY(config, EditAnywhere, Category="Claude (Legacy)", meta=(DisplayName="Claude Max Response Tokens", ClampMin=256, ClampMax=128000, ToolTip="Maximum response length requested from the legacy Claude backend."))
	int32 MaxResponseTokens = 16384;

	/** Requested Claude CLI reasoning effort level (plan 623). Default skips the flag entirely. Mirrors the Codex Speed/Work-Mode dropdown pattern. */
	UPROPERTY(config, EditAnywhere, Category="Claude (Legacy)", meta=(DisplayName="Effort Level", ToolTip="Reasoning depth forwarded to Claude CLI via --effort. 'Default' skips the flag. Mirrors the Codex Speed/Work-Mode pattern."))
	EOsvayderUEClaudeEffortLevel DefaultClaudeEffortLevel = EOsvayderUEClaudeEffortLevel::Default;

	/** If true, Claude CLI prompt execution uses a stable disk-backed session id (--session-id / --resume) so ordinary multi-turn prompts keep conversation memory across per-prompt child processes. */
	UPROPERTY(config, EditAnywhere, Category="Claude (Legacy)", meta=(DisplayName="Persistent Session", ToolTip="Keep one Claude CLI conversation alive across prompts via --session-id/--resume, instead of starting a fresh conversation every time. Required for multi-turn memory."))
	bool bClaudeUsePersistentSession = true;

	/** If true, Osvayder UE appends a one-line language directive to the legacy Claude system prompt. This is opt-in because Claude CLI usually honors the user's input language without help. */
	UPROPERTY(config, EditAnywhere, Category="Claude (Legacy)", meta=(DisplayName="Forward Dictation Language", ToolTip="Opt-in. Claude CLI already replies in the user's input language without help. Enable this only if you want to force a target language that differs from the input language."))
	bool bClaudeForwardLanguageToSystemPrompt = false;

	// Mutation Lifecycle

	/** When enabled, mutation tools automatically save and (for Blueprints) compile affected assets after each successful mutation. Default is on because Osvayder UE is designed for autonomous agent use. Per-call override: pass auto_save=false in the tool params for one specific call. Disable globally only if you want every mutation to leave the asset dirty in memory for manual save. */
	UPROPERTY(config, EditAnywhere, Category="Mutation Lifecycle", meta=(DisplayName="Autonomous Mutation Mode", ToolTip="When enabled, mutation tools automatically save and (for Blueprints) compile affected assets after each successful mutation. Default is on because Osvayder UE is designed for autonomous agent use. Per-call override: pass auto_save=false in the tool params for one specific call. Disable globally only if you want every mutation to leave the asset dirty in memory for manual save."))
	bool bAutonomousMutationMode = true;

	/** 619 P1: When enabled, the `livecoding_compile` MCP tool may call `ILiveCodingModule::EnableForSession(true)` on behalf of the agent when Live Coding is currently disabled for the editor session. When disabled, the tool returns a structured error (`refresh_status="live_coding_disabled"`, reason `user_opt_out_bAllowAutoEnableLiveCoding`) and asks the user to enable Live Coding manually in Editor Preferences. Default is on because the 619 spec intent is to maximize hot-patch availability for agent flows; flip off only if your team has an explicit project policy that Live Coding must be manually toggled. Settings UI lands in P6. */
	UPROPERTY(config, EditAnywhere, Category="Mutation Lifecycle", meta=(DisplayName="Allow Auto-Enable Live Coding", ToolTip="When enabled, the livecoding_compile tool may call ILiveCodingModule::EnableForSession(true) on behalf of the agent. Default true. Disable only if your project policy requires manual Live Coding enable."))
	bool bAllowAutoEnableLiveCoding = true;

	// Logging

	/** Enable verbose MCP tool logging */
	UPROPERTY(config, EditAnywhere, Category="Logging", meta=(DisplayName="Verbose Tool Logging", ToolTip="Enable extra MCP tool logging for diagnostics and bring-up."))
	bool bVerboseToolLogging = false;

	UPROPERTY(EditAnywhere, Category="Diagnostic")
	int32 DiagCounter626_P6_S2 = 0;

	// Accessors

	/** Get singleton settings */
	static const UOsvayderUESettings* Get()
	{
		return GetDefault<UOsvayderUESettings>();
	}

	/** Get mutable settings */
	static UOsvayderUESettings* GetMutable()
	{
		return GetMutableDefault<UOsvayderUESettings>();
	}

	FString GetConfiguredCodexModel() const
	{
		return DefaultCodexModel.TrimStartAndEnd();
	}

	FString GetExplicitCodexProfile() const
	{
		return DefaultCodexProfile.TrimStartAndEnd();
	}

	bool HasExplicitCodexProfile() const
	{
		return !GetExplicitCodexProfile().IsEmpty();
	}

	FString GetConfiguredCodexHomeOverride() const
	{
		return DefaultCodexHomeOverride.TrimStartAndEnd();
	}

	bool HasExplicitCodexHomeOverride() const
	{
		return !GetConfiguredCodexHomeOverride().IsEmpty();
	}

	EOsvayderUECodexAuthMode GetCodexAuthMode() const
	{
		return DefaultCodexAuthMode;
	}

	FString GetConfiguredCodexAuthModeName() const
	{
		switch (GetCodexAuthMode())
		{
		case EOsvayderUECodexAuthMode::ApiKeyEnvVar:
			return TEXT("api");

		case EOsvayderUECodexAuthMode::CliTerminal:
			return TEXT("cli_terminal");

		case EOsvayderUECodexAuthMode::BrowserVerify:
			return TEXT("browser_verify");

		case EOsvayderUECodexAuthMode::Auto:
		default:
			return TEXT("auto");
		}
	}

	static FString GetDefaultCodexApiKeyEnvVarName()
	{
		return TEXT("OSVAYDERUE_OPENAI_API_KEY");
	}

	FString GetConfiguredCodexApiKeyEnvVar() const
	{
		const FString EnvVarName = DefaultCodexApiKeyEnvVar.TrimStartAndEnd();
		return EnvVarName.IsEmpty() ? GetDefaultCodexApiKeyEnvVarName() : EnvVarName;
	}

	bool ShouldClearProxyEnvForBrowserVerify() const
	{
		return bCodexBrowserVerifyClearProxyEnv || bCodexExecClearProxyEnv;
	}

	bool ShouldClearProxyEnvForCodexExec() const
	{
		return bCodexExecClearProxyEnv;
	}

	bool ShouldUsePersistentCodexAppServer() const
	{
		return bCodexUsePersistentAppServer;
	}

	bool ShouldUseClaudePersistentSession() const
	{
		return bClaudeUsePersistentSession;
	}

	/** Plan 623 / Claude effort level: lowercase CLI string for --effort, or empty string for Default (flag omitted). */
	FString GetConfiguredClaudeEffortLevelName() const
	{
		switch (DefaultClaudeEffortLevel)
		{
		case EOsvayderUEClaudeEffortLevel::Low:    return TEXT("low");
		case EOsvayderUEClaudeEffortLevel::Medium: return TEXT("medium");
		case EOsvayderUEClaudeEffortLevel::High:   return TEXT("high");
		case EOsvayderUEClaudeEffortLevel::XHigh:  return TEXT("xhigh");
		case EOsvayderUEClaudeEffortLevel::Max:    return TEXT("max");
		case EOsvayderUEClaudeEffortLevel::Default:
		default:
			return FString();
		}
	}

	bool ShouldForwardLanguageToClaudeSystemPrompt() const
	{
		return bClaudeForwardLanguageToSystemPrompt;
	}

	bool ShouldAutoSaveMutations() const
	{
		return bAutonomousMutationMode;
	}

	bool ShouldAutoRestoreSessionOnOpen() const
	{
		return bAutoRestoreSessionOnOpen;
	}

	EOsvayderUEDictationLanguage GetConfiguredDictationLanguage() const
	{
		return DefaultDictationLanguage;
	}

	FString GetConfiguredDictationLanguageModeName() const;
	FString GetEffectiveDictationLanguageTag() const;
	FString GetEffectiveDictationLanguageDisplayName() const;
	FString GetEffectiveDictationModelName() const;
	FString GetEffectiveDictationModelUrl() const;
	FString GetDictationRuntimeLabel() const;
	bool SupportsRussianOfflineDictation() const;
	FString GetDictationSupportClaimName() const;
	FString GetDictationSupportDetail() const;

	FString GetEffectiveCodexProfileLabel() const
	{
		const FString Profile = GetExplicitCodexProfile();
		return Profile.IsEmpty() ? TEXT("default") : Profile;
	}

	FString GetConfiguredCodexWorkModeName() const
	{
		switch (DefaultCodexWorkMode)
		{
		case EOsvayderUECodexWorkMode::Fast:
			return TEXT("fast");

		case EOsvayderUECodexWorkMode::Deep:
			return TEXT("deep");

		case EOsvayderUECodexWorkMode::Max:
			return TEXT("max");

		case EOsvayderUECodexWorkMode::Custom:
			return TEXT("custom");

		case EOsvayderUECodexWorkMode::Balanced:
		default:
			return TEXT("balanced");
		}
	}

	FString GetConfiguredCodexSpeedModeName() const
	{
		switch (DefaultCodexSpeedMode)
		{
		case EOsvayderUECodexSpeedMode::Fast:
			return TEXT("fast");

		case EOsvayderUECodexSpeedMode::Standard:
		default:
			return TEXT("standard");
		}
	}

	static FString GetCodexReasoningEffortName(const EOsvayderUECodexReasoningEffort Effort)
	{
		switch (Effort)
		{
		case EOsvayderUECodexReasoningEffort::Low:
			return TEXT("low");

		case EOsvayderUECodexReasoningEffort::Medium:
			return TEXT("medium");

		case EOsvayderUECodexReasoningEffort::High:
			return TEXT("high");

		case EOsvayderUECodexReasoningEffort::VeryHigh:
			return TEXT("xhigh");

		case EOsvayderUECodexReasoningEffort::ModelDefault:
		default:
			return TEXT("model_default");
		}
	}

	static FString GetCodexVerbosityName(const EOsvayderUECodexVerbosity Verbosity)
	{
		switch (Verbosity)
		{
		case EOsvayderUECodexVerbosity::Low:
			return TEXT("low");

		case EOsvayderUECodexVerbosity::Medium:
			return TEXT("medium");

		case EOsvayderUECodexVerbosity::High:
			return TEXT("high");

		case EOsvayderUECodexVerbosity::ModelDefault:
		default:
			return TEXT("model_default");
		}
	}

	EOsvayderUECodexReasoningEffort GetEffectiveCodexReasoningEffort() const
	{
		switch (DefaultCodexWorkMode)
		{
		case EOsvayderUECodexWorkMode::Fast:
			return EOsvayderUECodexReasoningEffort::Low;

		case EOsvayderUECodexWorkMode::Deep:
			return EOsvayderUECodexReasoningEffort::High;

		case EOsvayderUECodexWorkMode::Max:
			return EOsvayderUECodexReasoningEffort::VeryHigh;

		case EOsvayderUECodexWorkMode::Custom:
			return DefaultCodexReasoningEffort;

		case EOsvayderUECodexWorkMode::Balanced:
		default:
			return EOsvayderUECodexReasoningEffort::Medium;
		}
	}

	EOsvayderUECodexVerbosity GetEffectiveCodexVerbosity() const
	{
		switch (DefaultCodexWorkMode)
		{
		case EOsvayderUECodexWorkMode::Fast:
			return EOsvayderUECodexVerbosity::Low;

		case EOsvayderUECodexWorkMode::Deep:
			return EOsvayderUECodexVerbosity::High;

		case EOsvayderUECodexWorkMode::Max:
			return EOsvayderUECodexVerbosity::High;

		case EOsvayderUECodexWorkMode::Custom:
			return DefaultCodexVerbosity;

		case EOsvayderUECodexWorkMode::Balanced:
		default:
			return EOsvayderUECodexVerbosity::Medium;
		}
	}

	FString GetConfiguredCodexReasoningEffortName() const
	{
		return GetCodexReasoningEffortName(GetEffectiveCodexReasoningEffort());
	}

	FString GetConfiguredCodexVerbosityName() const
	{
		return GetCodexVerbosityName(GetEffectiveCodexVerbosity());
	}

	bool HasExplicitCodexReasoningEffortOverride() const
	{
		return GetEffectiveCodexReasoningEffort() != EOsvayderUECodexReasoningEffort::ModelDefault;
	}

	bool HasExplicitCodexVerbosityOverride() const
	{
		return GetEffectiveCodexVerbosity() != EOsvayderUECodexVerbosity::ModelDefault;
	}

	FString GetCodexReasoningSupportLabel() const
	{
		return TEXT("codex_config_model_dependent");
	}

	FString GetCodexVerbositySupportLabel() const
	{
		return TEXT("codex_config_responses_api");
	}

	FString GetConfiguredModelForBackend(const EOsvayderUEProviderBackend Backend) const
	{
		return Backend == EOsvayderUEProviderBackend::CodexCli
			? GetConfiguredCodexModel()
			: DefaultModel.TrimStartAndEnd();
	}

	FString GetConfiguredProfileLabelForBackend(const EOsvayderUEProviderBackend Backend) const
	{
		return Backend == EOsvayderUEProviderBackend::CodexCli
			? GetEffectiveCodexProfileLabel()
			: FString();
	}

	FString GetConfiguredSpeedModeLabelForBackend(const EOsvayderUEProviderBackend Backend) const
	{
		return Backend == EOsvayderUEProviderBackend::CodexCli
			? GetConfiguredCodexSpeedModeName()
			: FString();
	}

	FString GetConfiguredAuthModeLabelForBackend(const EOsvayderUEProviderBackend Backend) const
	{
		return Backend == EOsvayderUEProviderBackend::CodexCli
			? GetConfiguredCodexAuthModeName()
			: TEXT("claude_cli");
	}

	/** Build a concise settings summary for prompt/context injection */
	FString BuildSettingsSummary() const
	{
		FString Summary;
		Summary += FString::Printf(TEXT("Scope: %s\n"), ScopeMode == EOsvayderUEScopeMode::PluginOnly ? TEXT("PluginOnly") : TEXT("PluginAndProject"));
		Summary += FString::Printf(TEXT("Verification: %s, ShortChecks=%s, OsvayderFallback=%s\n"),
			VerificationMode == EOsvayderUEVerificationMode::PracticalEditorFirst ? TEXT("PracticalEditorFirst") : TEXT("Mixed"),
			bPreferShortPracticalChecks ? TEXT("yes") : TEXT("no"),
			bUseOsvayderAsFallback ? TEXT("yes") : TEXT("no"));
		Summary += FString::Printf(TEXT("Architecture: MultiplayerFirst=%s, CppForAuthority=%s, BPForOrchestration=%s\n"),
			bMultiplayerFirst ? TEXT("yes") : TEXT("no"),
			bPreferCppForAuthority ? TEXT("yes") : TEXT("no"),
			bPreferBlueprintForOrchestration ? TEXT("yes") : TEXT("no"));
		Summary += FString::Printf(TEXT("MCP: port=%d, OsvayderEye=%s (%s)\n"),
			MCPServerPort,
			bEnableOsvayderEye ? TEXT("enabled") : TEXT("disabled"),
			*OsvayderEyeUrl);
		const TCHAR* BackendName = PreferredBackend == EOsvayderUEProviderBackend::CodexCli
			? TEXT("CodexCli")
			: TEXT("ClaudeCli");
		Summary += FString::Printf(TEXT("Backend: %s, ClaudeModel=%s, CodexModel=%s, CodexProfile=%s, CodexSpeed=%s, CodexWorkMode=%s, CodexReasoning=%s, CodexVerbosity=%s, CodexHome=%s, CodexAuthMode=%s, CodexAuthEnvVar=%s, CodexExecProxyClear=%s, CodexPersistentTransport=%s, AutoRestoreSession=%s, MaxTokens=%d\n"),
			BackendName,
			*DefaultModel,
			*GetConfiguredCodexModel(),
			*GetEffectiveCodexProfileLabel(),
			*GetConfiguredCodexSpeedModeName(),
			*GetConfiguredCodexWorkModeName(),
			*GetConfiguredCodexReasoningEffortName(),
			*GetConfiguredCodexVerbosityName(),
			HasExplicitCodexHomeOverride() ? *GetConfiguredCodexHomeOverride() : TEXT("resolution_ladder"),
			*GetConfiguredCodexAuthModeName(),
			*GetConfiguredCodexApiKeyEnvVar(),
			ShouldClearProxyEnvForCodexExec() ? TEXT("yes") : TEXT("no"),
			ShouldUsePersistentCodexAppServer() ? TEXT("yes") : TEXT("no"),
			ShouldAutoRestoreSessionOnOpen() ? TEXT("yes") : TEXT("no"),
			MaxResponseTokens);
		Summary += FString::Printf(
			TEXT("Dictation: requested=%s, effective_language=%s, model=%s, runtime=%s, supports_russian=%s, support_claim=%s, support_detail=%s\n"),
			*GetConfiguredDictationLanguageModeName(),
			*GetEffectiveDictationLanguageTag(),
			*GetEffectiveDictationModelName(),
			*GetDictationRuntimeLabel(),
			SupportsRussianOfflineDictation() ? TEXT("yes") : TEXT("no"),
			*GetDictationSupportClaimName(),
			*GetDictationSupportDetail());
		Summary += FString::Printf(TEXT("Paths: memory=%s, bridge=%s\n"), *ProjectMemoryPath, *AgentBridgePath);
		return Summary;
	}

	// UDeveloperSettings interface
	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("OsvayderUE"); }

#if WITH_EDITOR
	virtual FText GetSectionText() const override { return NSLOCTEXT("OsvayderUE", "SettingsSection", "Osvayder UE"); }
	virtual FText GetSectionDescription() const override { return NSLOCTEXT("OsvayderUE", "SettingsDesc", "Configure Osvayder UE for normal editor use. Advanced bring-up, verification, and diagnostics stay clearly labeled."); }
#endif
};

