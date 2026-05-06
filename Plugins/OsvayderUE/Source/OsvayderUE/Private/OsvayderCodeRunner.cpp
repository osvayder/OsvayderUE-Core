// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderCodeRunner.h"
#include "AgentPromptContract.h"
#include "OsvayderSessionManager.h"
#include "OsvayderUESettings.h"
#include "OsvayderUEScopePolicy.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEModule.h"
#include "OsvayderUEConstants.h"
#include "ProjectContext.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Base64.h"
#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"

FString FOsvayderCodeRunner::CachedCliJsPath;

namespace
{
	bool TryConvertAllowedUnrealMcpToolPatternToRawToolName(const FString& AllowedTool, FString& OutRawToolName)
	{
		static const FString UnrealMcpPrefix = TEXT("mcp__osvayderue__");
		if (!AllowedTool.StartsWith(UnrealMcpPrefix, ESearchCase::IgnoreCase))
		{
			return false;
		}

		const FString RawToolName = AllowedTool.RightChop(UnrealMcpPrefix.Len()).TrimStartAndEnd();
		if (RawToolName.IsEmpty() || RawToolName == TEXT("*"))
		{
			return false;
		}

		OutRawToolName = RawToolName;
		return true;
	}

	TArray<FString> GetRequestedScopedUnrealMcpToolNames(const FAgentRequestConfig& Config)
	{
		TArray<FString> ToolNames;
		for (const FString& AllowedTool : Config.AllowedTools)
		{
			FString RawToolName;
			if (TryConvertAllowedUnrealMcpToolPatternToRawToolName(AllowedTool, RawToolName))
			{
				ToolNames.AddUnique(RawToolName);
			}
		}

		ToolNames.Sort([](const FString& A, const FString& B)
		{
			return A.Compare(B, ESearchCase::IgnoreCase) < 0;
		});

		return ToolNames;
	}

	bool ConfigRequestsUnrealMcpBridge(const FAgentRequestConfig& Config)
	{
		return Config.bEnableUnrealMcpBridge || GetRequestedScopedUnrealMcpToolNames(Config).Num() > 0;
	}

	FString ResolveHomeDirectory()
	{
#if PLATFORM_WINDOWS
		const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		if (!UserProfile.IsEmpty())
		{
			return UserProfile;
		}
#endif

		const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
		if (!Home.IsEmpty())
		{
			return Home;
		}

		return FString();
	}

	FString GetClaudeCredentialsPath()
	{
		const FString HomeDir = ResolveHomeDirectory();
		if (HomeDir.IsEmpty())
		{
			return FString();
		}

		return FPaths::Combine(HomeDir, TEXT(".claude"), TEXT(".credentials.json"));
	}

	/** Singleton persistent-session manager owned by the OsvayderCodeRunner translation unit. */
	FOsvayderSessionManager& GetPersistentSessionManagerSingleton()
	{
		static FOsvayderSessionManager Manager;
		return Manager;
	}
}

FString FOsvayderCodeRunner::ComposePersistentSessionFlags(
	const bool bUsePersistentSession,
	const FString& SessionId,
	const bool bWasExisting)
{
	if (!bUsePersistentSession)
	{
		return FString();
	}

	const FString TrimmedId = SessionId.TrimStartAndEnd();
	if (TrimmedId.IsEmpty())
	{
		return FString();
	}

	// Spec 621 §2: first turn → --session-id <uuid>, subsequent turns → --resume <uuid>.
	const TCHAR* Flag = bWasExisting ? TEXT("--resume") : TEXT("--session-id");
	return FString::Printf(TEXT("%s %s "), Flag, *TrimmedId);
}

FString FOsvayderCodeRunner::GetClaudeLanguageDisplayName(const EOsvayderUEDictationLanguage Language)
{
	// Spec 621 §3: Auto → empty (no directive), EnglishUs → "English", RussianRu → "Russian".
	switch (Language)
	{
	case EOsvayderUEDictationLanguage::RussianRu:
		return TEXT("Russian");

	case EOsvayderUEDictationLanguage::EnglishUs:
		return TEXT("English");

	case EOsvayderUEDictationLanguage::Auto:
	default:
		return FString();
	}
}

FString FOsvayderCodeRunner::ComposeLanguageHintFlag(const FString& LanguageDisplayName)
{
	// Spec 621 reviewer 2026-04-17 14:10 fix [A1-2]: per-turn injection of the language directive via
	// --append-system-prompt, which Claude CLI re-applies on --resume turns (unlike the session's original
	// system prompt which is only read at create time). Empty display name → empty fragment.
	const FString TrimmedName = LanguageDisplayName.TrimStartAndEnd();
	if (TrimmedName.IsEmpty())
	{
		return FString();
	}

	// Verbatim spec §3 line; keep in sync with the MaterializeClaudeSystemPrompt(Contract, Language) overload.
	const FString LanguageLine = FString::Printf(
		TEXT("The user's preferred interaction language is %s. Respond in that language unless the user explicitly asks otherwise or the content is source code."),
		*TrimmedName);

	// Escape embedded double quotes so the CLI-quoted form stays well-formed.
	const FString EscapedLine = LanguageLine.Replace(TEXT("\""), TEXT("\\\""));
	return FString::Printf(TEXT("--append-system-prompt \"%s\" "), *EscapedLine);
}

FString FOsvayderCodeRunner::ComposeModelFlag(const FString& ModelName)
{
	// Spec 621 reviewer 2026-04-17 15:30 v4 / P3: forward the configured Claude model to the CLI. Empty model
	// → skip the flag entirely and let Claude CLI pick its default (better than crashing on bad input).
	const FString TrimmedName = ModelName.TrimStartAndEnd();
	if (TrimmedName.IsEmpty())
	{
		return FString();
	}
	return FString::Printf(TEXT("--model %s "), *TrimmedName);
}

FString FOsvayderCodeRunner::ComposeEffortFlag(const FString& EffortLevel)
{
	// Plan 623: forward the configured Claude effort to the CLI via `--effort <level>`. Empty level → skip
	// the flag entirely (Default setting yields empty via GetConfiguredClaudeEffortLevelName()); CLI then
	// applies its own default. Mirrors the ComposeModelFlag shape.
	const FString TrimmedLevel = EffortLevel.TrimStartAndEnd();
	if (TrimmedLevel.IsEmpty())
	{
		return FString();
	}
	return FString::Printf(TEXT("--effort %s "), *TrimmedLevel);
}

FString FOsvayderCodeRunner::ComposeStdinPayloadForClaudePrint(const FString& PromptText)
{
	// Spec 621 reviewer 2026-04-17 15:30 v4 / P1: stdin is the plain user prompt, no `[CONTEXT]` wrap, no
	// NDJSON envelope. Baseline Step 1a evidence: `claude -p "..."` on stdin reaches the model correctly.
	// Empty prompt → empty stdin (caller can skip the write).
	if (PromptText.IsEmpty())
	{
		return FString();
	}
	if (PromptText.EndsWith(TEXT("\n")))
	{
		return PromptText;
	}
	return PromptText + TEXT("\n");
}

bool FOsvayderCodeRunner::ShouldAutoResetOnExit(const int32 ExitCode, const FString& Output, const bool bUsedResume)
{
	// Spec 621 reviewer 2026-04-17 14:10 fix [A1-1]: decide whether to rotate the persistent-session artifact.
	//   * Non-zero exit is required (normal success never resets).
	//   * Classic resume-failure signals fire on any turn: "session not found" / "No conversation found" /
	//     the literal word "resume" (present in most CLI resume-specific error phrases).
	//   * Anthropic API 400 invalid_request_error is only a resume-failure signal when this particular
	//     invocation actually used --resume — otherwise an unrelated first-turn 400 (bad prompt shape, etc.)
	//     would spuriously rotate the artifact.
	if (ExitCode == 0)
	{
		return false;
	}

	const bool bClassicResumeSignal =
		Output.Contains(TEXT("session not found"), ESearchCase::IgnoreCase) ||
		Output.Contains(TEXT("No conversation found"), ESearchCase::IgnoreCase) ||
		Output.Contains(TEXT("resume"), ESearchCase::IgnoreCase);

	const bool bResumeInvalidRequest =
		bUsedResume &&
		Output.Contains(TEXT("invalid_request_error"), ESearchCase::IgnoreCase);

	return bClassicResumeSignal || bResumeInvalidRequest;
}

FOsvayderCodeRunner::FOsvayderCodeRunner()
	: Thread(nullptr)
	, bIsExecuting(false)
	, ReadPipe(nullptr)
	, WritePipe(nullptr)
	, StdInReadPipe(nullptr)
	, StdInWritePipe(nullptr)
{
}

FAgentBackendCapabilities FOsvayderCodeRunner::GetCapabilities() const
{
	FAgentBackendCapabilities Capabilities;
	Capabilities.Backend = EOsvayderUEProviderBackend::ClaudeCli;
	Capabilities.DisplayName = TEXT("Claude CLI");
	Capabilities.bSupportsStreamingEvents = true;
	Capabilities.bSupportsImages = true;
	Capabilities.bSupportsCancellation = true;
	Capabilities.bSupportsToolAllowList = true;
	Capabilities.bUsesStructuredOutput = true;
	Capabilities.bSupportsBrowserVerifyLogin = false;
	Capabilities.bSupportsProviderPersistentThreads = false;
	Capabilities.bSupportsReasoningEffortControl = false;
	Capabilities.bSupportsVerbosityControl = false;
	Capabilities.bSupportsSpeedModeControl = false;
	Capabilities.bSupportsProfileSelection = false;
	Capabilities.bSupportsExplicitAuthModeSelection = false;
	return Capabilities;
}

FAgentBackendStatus FOsvayderCodeRunner::GetStatus() const
{
	FAgentBackendStatus Status;
	Status.Backend = EOsvayderUEProviderBackend::ClaudeCli;
	Status.DisplayName = GetBackendDisplayName();
	Status.Capabilities = GetCapabilities();
	Status.ExecutablePath = GetClaudePath();
	Status.bAvailable = !Status.ExecutablePath.IsEmpty();

	if (!Status.bAvailable)
	{
		Status.Readiness = EAgentBackendReadiness::NotAvailable;
		Status.AuthState = EAgentBackendAuthState::Unknown;
		Status.bReady = false;
		Status.Detail = TEXT("Claude CLI not found. Please install with: npm install -g @anthropic-ai/claude-code");
		Status.AuthDetail = TEXT("Authentication cannot be evaluated because executable is missing.");
		return Status;
	}

	const FString AnthropicApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_API_KEY"));
	const FString CredentialsPath = GetClaudeCredentialsPath();
	const bool bHasCredentialsFile = !CredentialsPath.IsEmpty() && IFileManager::Get().FileExists(*CredentialsPath);

	if (!AnthropicApiKey.IsEmpty())
	{
		Status.AuthState = EAgentBackendAuthState::Authenticated;
		Status.Readiness = EAgentBackendReadiness::Ready;
		Status.bReady = true;
		Status.AuthDetail = TEXT("ANTHROPIC_API_KEY is present in the environment.");
		Status.Detail = FString::Printf(TEXT("Claude CLI ready at %s (auth via ANTHROPIC_API_KEY)."), *Status.ExecutablePath);
	}
	else if (bHasCredentialsFile)
	{
		Status.AuthState = EAgentBackendAuthState::Unknown;
		Status.Readiness = EAgentBackendReadiness::AvailableAuthUnknown;
		Status.bReady = false;
		Status.AuthDetail = FString::Printf(
			TEXT("Claude credentials file detected at %s (file presence only, token validity not probed)."),
			*CredentialsPath);
		Status.Detail = FString::Printf(
			TEXT("Claude CLI detected at %s with credential artifact present, but auth readiness is unconfirmed without a live probe."),
			*Status.ExecutablePath);
	}
	else
	{
		Status.AuthState = EAgentBackendAuthState::NotAuthenticated;
		Status.Readiness = EAgentBackendReadiness::AvailableNotAuthenticated;
		Status.bReady = false;
		Status.AuthDetail = TEXT("No ANTHROPIC_API_KEY and no ~/.claude/.credentials.json file detected.");
		Status.Detail = FString::Printf(
			TEXT("Claude CLI detected at %s, but credentials were not detected. Run `claude login` or set ANTHROPIC_API_KEY."),
			*Status.ExecutablePath);
	}

	return Status;
}

FOsvayderCodeRunner::~FOsvayderCodeRunner()
{
	// Signal stop FIRST (thread-safe) before touching anything
	StopTaskCounter.Set(1);

	// Wait for thread to exit BEFORE touching handles
	if (Thread)
	{
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}

	// NOW safe to cleanup handles (thread has exited)
	CleanupHandles();
}

void FOsvayderCodeRunner::CleanupHandles()
{
	if (ReadPipe || WritePipe)
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = nullptr;
		WritePipe = nullptr;
	}
	if (StdInReadPipe || StdInWritePipe)
	{
		FPlatformProcess::ClosePipe(StdInReadPipe, StdInWritePipe);
		StdInReadPipe = nullptr;
		StdInWritePipe = nullptr;
	}
	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::CloseProc(ProcessHandle);
	}
}

bool FOsvayderCodeRunner::IsClaudeAvailable()
{
	FString ClaudePath = GetClaudePath();
	return !ClaudePath.IsEmpty();
}

FString FOsvayderCodeRunner::GetClaudePath()
{
	// Cache the path to avoid repeated lookups and log spam
	static FString CachedClaudePath;
	static bool bHasSearched = false;

	if (bHasSearched && !CachedClaudePath.IsEmpty())
	{
		// Only return cached path if it's valid
		return CachedClaudePath;
	}
	// Allow re-search if previous search failed (CachedClaudePath is empty)
	bHasSearched = true;

#if PLATFORM_WINDOWS
	// On Windows, we launch node.exe directly with cli.js as argument
	// to avoid issues with .cmd batch wrappers in UE's CreateProc.
	// First, find node.exe and cli.js paths.
	FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	if (!AppData.IsEmpty())
	{
		FString CliJsPath = FPaths::Combine(AppData, TEXT("npm"), TEXT("node_modules"), TEXT("@anthropic-ai"), TEXT("claude-code"), TEXT("cli.js"));
		if (IFileManager::Get().FileExists(*CliJsPath))
		{
			// Find node.exe - check common locations
			TArray<FString> NodePaths;
			NodePaths.Add(TEXT("D:\\Program Files\\nodejs\\node.exe"));
			NodePaths.Add(TEXT("C:\\Program Files\\nodejs\\node.exe"));

			// Also check PATH
			FString NodePathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
			TArray<FString> NodePathDirs;
			NodePathEnv.ParseIntoArray(NodePathDirs, TEXT(";"), true);
			for (const FString& Dir : NodePathDirs)
			{
				NodePaths.Add(FPaths::Combine(Dir, TEXT("node.exe")));
			}

			for (const FString& NodePath : NodePaths)
			{
				if (IFileManager::Get().FileExists(*NodePath))
				{
					CachedCliJsPath = CliJsPath;
					CachedClaudePath = NodePath;
					UE_LOG(LogOsvayderUE, Log, TEXT("Found Claude CLI (direct node mode): node=%s, cli=%s"), *NodePath, *CliJsPath);
					return CachedClaudePath;
				}
			}
		}
	}

	// Fallback: check common locations for claude CLI
	TArray<FString> PossiblePaths;

	// User profile .local/bin (Claude Code native installer location)
	FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	if (!UserProfile.IsEmpty())
	{
		PossiblePaths.Add(FPaths::Combine(UserProfile, TEXT(".local"), TEXT("bin"), TEXT("claude.exe")));
	}

	// npm global install location
	if (!AppData.IsEmpty())
	{
		PossiblePaths.Add(FPaths::Combine(AppData, TEXT("npm"), TEXT("claude.cmd")));
	}

	// Local AppData npm
	FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (!LocalAppData.IsEmpty())
	{
		PossiblePaths.Add(FPaths::Combine(LocalAppData, TEXT("npm"), TEXT("claude.cmd")));
	}

	// User profile npm
	if (!UserProfile.IsEmpty())
	{
		PossiblePaths.Add(FPaths::Combine(UserProfile, TEXT("AppData"), TEXT("Roaming"), TEXT("npm"), TEXT("claude.cmd")));
	}

	// Check PATH - try to find claude.cmd or claude.exe
	FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	TArray<FString> PathDirs;
	PathEnv.ParseIntoArray(PathDirs, TEXT(";"), true);

	for (const FString& Dir : PathDirs)
	{
		PossiblePaths.Add(FPaths::Combine(Dir, TEXT("claude.cmd")));
		PossiblePaths.Add(FPaths::Combine(Dir, TEXT("claude.exe")));
	}
#else
	TArray<FString> PossiblePaths;

	// Linux/Mac: Claude Code native installer location
	FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!Home.IsEmpty())
	{
		PossiblePaths.Add(FPaths::Combine(Home, TEXT(".local"), TEXT("bin"), TEXT("claude")));
	}

	// Common system paths
	PossiblePaths.Add(TEXT("/usr/local/bin/claude"));
	PossiblePaths.Add(TEXT("/usr/bin/claude"));

	// npm global install locations
	if (!Home.IsEmpty())
	{
		// npm default global prefix on Linux
		PossiblePaths.Add(FPaths::Combine(Home, TEXT(".npm-global"), TEXT("bin"), TEXT("claude")));
		// nvm-managed node
		PossiblePaths.Add(FPaths::Combine(Home, TEXT(".nvm"), TEXT("versions"), TEXT("node")));
	}

	// Check PATH
	FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	TArray<FString> PathDirs;
	PathEnv.ParseIntoArray(PathDirs, TEXT(":"), true);

	for (const FString& Dir : PathDirs)
	{
		PossiblePaths.Add(FPaths::Combine(Dir, TEXT("claude")));
	}
#endif

	// Check each path
	for (const FString& Path : PossiblePaths)
	{
		if (IFileManager::Get().FileExists(*Path))
		{
			UE_LOG(LogOsvayderUE, Log, TEXT("Found Claude CLI at: %s"), *Path);
			CachedClaudePath = Path;
			return CachedClaudePath;
		}
	}

	// Try using 'where' (Windows) or 'which' (Linux/Mac) as fallback
	FString WhereOutput;
	FString WhereErrors;
	int32 ReturnCode;

#if PLATFORM_WINDOWS
	const TCHAR* WhichCmd = TEXT("where");
	const TCHAR* WhichArgs = TEXT("claude");
#else
	// Route through /bin/sh for PATH resolution (consistent with clipboard handling)
	const TCHAR* WhichCmd = TEXT("/bin/sh");
	const TCHAR* WhichArgs = TEXT("-c 'which claude 2>/dev/null'");
#endif

	if (FPlatformProcess::ExecProcess(WhichCmd, WhichArgs, &ReturnCode, &WhereOutput, &WhereErrors) && ReturnCode == 0)
	{
		WhereOutput.TrimStartAndEndInline();
		TArray<FString> Lines;
		WhereOutput.ParseIntoArrayLines(Lines);
		if (Lines.Num() > 0)
		{
			UE_LOG(LogOsvayderUE, Log, TEXT("Found Claude CLI via '%s': %s"), WhichCmd, *Lines[0]);
			CachedClaudePath = Lines[0];
			return CachedClaudePath;
		}
	}

	UE_LOG(LogOsvayderUE, Warning, TEXT("Claude CLI not found. Please install with: npm install -g @anthropic-ai/claude-code"));

	// CachedClaudePath remains empty if not found
	return CachedClaudePath;
}

bool FOsvayderCodeRunner::ExecuteAsync(
	const FOsvayderRequestConfig& Config,
	FOnOsvayderResponse OnComplete,
	FOnOsvayderProgress OnProgress)
{
	// Use atomic compare-exchange for thread-safe check-and-set
	bool Expected = false;
	if (!bIsExecuting.CompareExchange(Expected, true))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Claude is already executing a request"));
		return false;
	}

	if (!IsClaudeAvailable())
	{
		bIsExecuting = false;
		OnComplete.ExecuteIfBound(TEXT("Claude CLI not found. Please install with: npm install -g @anthropic-ai/claude-code"), false);
		return false;
	}

	// Clean up old thread if exists (from previous completed execution)
	if (Thread)
	{
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}

	CurrentConfig = Config;
	OnCompleteDelegate = OnComplete;
	OnProgressDelegate = OnProgress;

	// Start the execution thread
	Thread = FRunnableThread::Create(this, TEXT("OsvayderCodeRunner"), 0, TPri_Normal);

	if (!Thread)
	{
		bIsExecuting = false;
		return false;
	}
	return true;
}

bool FOsvayderCodeRunner::ExecuteSync(const FOsvayderRequestConfig& Config, FString& OutResponse)
{
	if (!IsClaudeAvailable())
	{
		OutResponse = TEXT("Claude CLI not found. Please install with: npm install -g @anthropic-ai/claude-code");
		return false;
	}

	FString ClaudePath = GetClaudePath();
	FString CommandLine = BuildCommandLine(Config);

	UE_LOG(LogOsvayderUE, Log, TEXT("Executing Claude: %s %s"), *ClaudePath, *CommandLine);

	FString StdOut;
	FString StdErr;
	int32 ReturnCode;

	// Set working directory
	FString WorkingDir = Config.WorkingDirectory;
	if (WorkingDir.IsEmpty())
	{
		WorkingDir = FPaths::ProjectDir();
	}

	bool bSuccess = FPlatformProcess::ExecProcess(
		*ClaudePath,
		*CommandLine,
		&ReturnCode,
		&StdOut,
		&StdErr,
		*WorkingDir
	);

	if (bSuccess && ReturnCode == 0)
	{
		OutResponse = StdOut;
		return true;
	}
	else
	{
		OutResponse = StdErr.IsEmpty() ? StdOut : StdErr;
		UE_LOG(LogOsvayderUE, Error, TEXT("Claude execution failed: %s"), *OutResponse);
		return false;
	}
}

// Get the plugin directory path
static FString GetPluginDirectory()
{
	// Try engine plugins directly (manual install location)
	FString EnginePluginPath = FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("OsvayderUE"));
	if (FPaths::DirectoryExists(EnginePluginPath))
	{
		return EnginePluginPath;
	}

	// Try engine Marketplace plugins (Epic marketplace location)
	FString MarketplacePluginPath = FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("Marketplace"), TEXT("OsvayderUE"));
	if (FPaths::DirectoryExists(MarketplacePluginPath))
	{
		return MarketplacePluginPath;
	}

	// Try project plugins
	FString ProjectPluginPath = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("OsvayderUE"));
	if (FPaths::DirectoryExists(ProjectPluginPath))
	{
		return ProjectPluginPath;
	}

	UE_LOG(LogOsvayderUE, Warning, TEXT("Could not find OsvayderUE plugin directory. Checked: %s, %s, %s"),
		*EnginePluginPath, *MarketplacePluginPath, *ProjectPluginPath);
	return FString();
}

FString FOsvayderCodeRunner::BuildCommandLine(const FOsvayderRequestConfig& Config)
{
	FString CommandLine;

	// Spec 621 §3: optionally fold a language hint into the materialized Claude system prompt.
	const UOsvayderUESettings* ClaudeSettings = UOsvayderUESettings::Get();
	FString LanguageDisplayName;
	if (ClaudeSettings && ClaudeSettings->ShouldForwardLanguageToClaudeSystemPrompt())
	{
		LanguageDisplayName = GetClaudeLanguageDisplayName(ClaudeSettings->GetConfiguredDictationLanguage());
	}

	const FString MaterializedSystemPrompt = Config.PromptContract.HasAnyContent()
		? FAgentPromptMaterializer::MaterializeClaudeSystemPrompt(Config.PromptContract, LanguageDisplayName)
		: Config.SystemPrompt;

	// Print mode (non-interactive)
	CommandLine += TEXT("-p ");

	// Verbose mode to show thinking (required by stream-json output format)
	CommandLine += TEXT("--verbose ");

	// Skip permissions if requested
	if (Config.bSkipPermissions)
	{
		CommandLine += TEXT("--dangerously-skip-permissions ");
	}

	// Spec 621 reviewer 2026-04-17 15:30 v4 / P2: emit only `--output-format stream-json` (controls stdout
	// NDJSON parsing, currently working). The matching `--input-format stream-json` flag is intentionally
	// dropped — the plugin's stdin envelope omitted `session_id` / `uuid` / `parent_tool_use_id` required
	// by Claude CLI's `SDKUserMessage` contract, so the CLI silently discarded every turn and the assistant
	// saw zero user messages (primary root cause per external advisor 2026-04-17). Under `-p` with no
	// `--input-format`, stdin plain text is interpreted as the user message body (baseline 1a evidence).
	CommandLine += TEXT("--output-format stream-json ");

	// Spec 621 §2: disk-backed persistent session id so ordinary multi-turn prompts keep conversation memory.
	bCurrentRunUsedResume = false;
	if (ClaudeSettings && ClaudeSettings->ShouldUseClaudePersistentSession())
	{
		const FString RequestModel = ClaudeSettings->GetConfiguredModelForBackend(EOsvayderUEProviderBackend::ClaudeCli);
		bool bWasExisting = false;
		const FString SessionId = GetPersistentSessionManagerSingleton()
			.GetOrCreatePersistentSessionId(EOsvayderUEProviderBackend::ClaudeCli, RequestModel, &bWasExisting);
		const FString SessionFragment = ComposePersistentSessionFlags(true, SessionId, bWasExisting);
		if (!SessionFragment.IsEmpty())
		{
			CommandLine += SessionFragment;
			// bWasExisting is the same gate ComposePersistentSessionFlags uses to pick --resume vs --session-id,
			// so track it here for ShouldAutoResetOnExit below.
			bCurrentRunUsedResume = bWasExisting;
			UE_LOG(LogOsvayderUE, Log,
				TEXT("Claude persistent session fragment: %s (model=%s, used_resume=%s)"),
				*SessionFragment.TrimStartAndEnd(),
				*RequestModel,
				bCurrentRunUsedResume ? TEXT("true") : TEXT("false"));
		}
	}

	// Spec 621 reviewer 2026-04-17 14:10 fix [A1-2]: Claude CLI only reads the session's original system prompt at
	// session-create time — our folded language directive never reaches the model on --resume turns. Emit
	// --append-system-prompt on every invocation so the language hint rides each turn, not only session creation.
	// Approach chosen: --append-system-prompt (native Claude CLI flag, documented safe on --resume). Rejected
	// alternative: folding the language line into the user prompt every turn (pollutes visible transcript).
	if (!LanguageDisplayName.IsEmpty())
	{
		CommandLine += ComposeLanguageHintFlag(LanguageDisplayName);
	}

	// Spec 621 reviewer 2026-04-17 15:30 v4 / P3: forward the configured model. Prior to this fix the plugin
	// stored the configured model in the persistent-session artifact but never passed it to the CLI, so every
	// turn ran on Claude CLI's default (Sonnet 4.6 per external advisor), regardless of settings.
	if (ClaudeSettings)
	{
		const FString ConfiguredModel = ClaudeSettings->GetConfiguredModelForBackend(EOsvayderUEProviderBackend::ClaudeCli);
		const FString ModelFragment = ComposeModelFlag(ConfiguredModel);
		if (!ModelFragment.IsEmpty())
		{
			CommandLine += ModelFragment;
			UE_LOG(LogOsvayderUE, Log, TEXT("Claude model fragment: %s"), *ModelFragment.TrimStartAndEnd());
		}

		// Plan 623: forward the configured Claude effort level via `--effort <level>`. Default setting yields
		// empty fragment (flag omitted; CLI default applies). Emitted right after the model flag so it sits in
		// the same "run tuning" cluster as --model, before the per-turn --append-system-prompt language hint.
		const FString EffortFragment = ComposeEffortFlag(ClaudeSettings->GetConfiguredClaudeEffortLevelName());
		if (!EffortFragment.IsEmpty())
		{
			CommandLine += EffortFragment;
			UE_LOG(LogOsvayderUE, Log, TEXT("Claude effort fragment: %s"), *EffortFragment.TrimStartAndEnd());
		}
	}

	// MCP config for editor tools
	if (ConfigRequestsUnrealMcpBridge(Config))
	{
		FString PluginDir = GetPluginDirectory();
		if (!PluginDir.IsEmpty())
		{
			FString MCPBridgePath = FPaths::Combine(PluginDir, TEXT("Resources"), TEXT("mcp-bridge"), TEXT("index.js"));
			FPaths::NormalizeFilename(MCPBridgePath);
			MCPBridgePath = FPaths::ConvertRelativePathToFull(MCPBridgePath);

			if (FPaths::FileExists(MCPBridgePath))
			{
			// Write MCP config to temp file (Claude CLI needs a file path)
			FString MCPConfigDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"));
			IFileManager::Get().MakeDirectory(*MCPConfigDir, true);

			FString MCPConfigPath = FPaths::Combine(MCPConfigDir, TEXT("mcp-config.json"));
			// Build env entries — always include Unreal MCP URL
			const UOsvayderUESettings* PortSettings = UOsvayderUESettings::Get();
			int32 McpPort = (PortSettings) ? PortSettings->MCPServerPort : OsvayderUEConstants::MCPServer::DefaultPort;
			FString EnvEntries = FString::Printf(
				TEXT("\"UNREAL_MCP_URL\": \"http://localhost:%d\""),
				McpPort
			);

			if (!Config.bEnableUnrealMcpBridge)
			{
				const TArray<FString> ScopedToolNames = GetRequestedScopedUnrealMcpToolNames(Config);
				if (ScopedToolNames.Num() > 0)
				{
					EnvEntries += FString::Printf(
						TEXT(",\n        \"UNREAL_MCP_ALLOWED_TOOLS\": \"%s\""),
						*FString::Join(ScopedToolNames, TEXT(",")));
				}
			}

			// Add OsvayderEye paths from settings if server.py exists
			const UOsvayderUESettings* EyeSettings = UOsvayderUESettings::Get();
			FString EyeServerPath = EyeSettings ? EyeSettings->OsvayderEyeServerPath.FilePath : FString();
			FString EyePythonPath = EyeSettings ? EyeSettings->OsvayderEyePythonPath.FilePath : FString();
			if (EyeSettings && EyeSettings->bEnableOsvayderEye && !EyeServerPath.IsEmpty() && IFileManager::Get().FileExists(*EyeServerPath))
			{
				EnvEntries += FString::Printf(
					TEXT(",\n        \"EYE_SERVER_PATH\": \"%s\""),
					*EyeServerPath.Replace(TEXT("\\"), TEXT("/")));
				EnvEntries += FString::Printf(
					TEXT(",\n        \"EYE_PYTHON_PATH\": \"%s\""),
					*EyePythonPath.Replace(TEXT("\\"), TEXT("/")));
				UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderEye enabled in MCP config: %s"), *EyeServerPath);
			}

			FString MCPConfigContent = FString::Printf(
				TEXT("{\n  \"mcpServers\": {\n    \"osvayderue\": {\n      \"command\": \"node\",\n      \"args\": [\"%s\"],\n      \"env\": {\n        %s\n      }\n    }\n  }\n}"),
				*MCPBridgePath.Replace(TEXT("\\"), TEXT("/")),
				*EnvEntries
			);

			// Scope check — mcp-config is internal runtime state
			auto McpScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(MCPConfigPath);
			if (McpScopeCheck.bAllowed)
			{
				if (FFileHelper::SaveStringToFile(MCPConfigContent, *MCPConfigPath))
				{
					FString AbsConfigPath = FPaths::ConvertRelativePathToFull(MCPConfigPath);
					FString EscapedConfigPath = AbsConfigPath.Replace(TEXT("\\"), TEXT("/"));
					CommandLine += FString::Printf(TEXT("--mcp-config \"%s\" "), *EscapedConfigPath);
					UE_LOG(LogOsvayderUE, Log, TEXT("MCP config written to: %s"), *MCPConfigPath);

					FExecutionReceipt Receipt;
					Receipt.Tool = TEXT("claude_runner");
					Receipt.bSuccess = true;
					Receipt.TargetType = TEXT("file");
					Receipt.Targets.Add(MCPConfigPath);
					Receipt.Created.Add(MCPConfigPath);
					Receipt.Classification = TEXT("internal_state");
					Receipt.ValidationSummary = TEXT("mcp-config.json");
					FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
				}
				else
				{
					UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to write MCP config to: %s"), *MCPConfigPath);
					FExecutionReceipt Receipt;
					Receipt.Tool = TEXT("claude_runner");
					Receipt.bSuccess = false;
					Receipt.TargetType = TEXT("file");
					Receipt.Targets.Add(MCPConfigPath);
					Receipt.Classification = TEXT("internal_state");
					Receipt.ValidationSummary = TEXT("File write failed");
					FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
				}
			}
			else
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Scope denied for MCP config write: %s"), *MCPConfigPath);
				FExecutionReceipt Receipt;
				Receipt.Tool = TEXT("claude_runner");
				Receipt.bSuccess = false;
				Receipt.TargetType = TEXT("file");
				Receipt.Targets.Add(MCPConfigPath);
				Receipt.Classification = TEXT("denied");
				Receipt.ValidationSummary = McpScopeCheck.DenialReason;
				FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
			}
		}
		else
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("MCP bridge not found at: %s"), *MCPBridgePath);
		}
	}
	}

	// Allowed tools - add MCP tools
	TArray<FString> AllTools = Config.AllowedTools;
	if (Config.bEnableUnrealMcpBridge)
	{
		AllTools.Add(TEXT("mcp__osvayderue__*")); // Allow all osvayderue MCP tools
	}
	if (AllTools.Num() > 0)
	{
		CommandLine += FString::Printf(TEXT("--allowedTools \"%s\" "), *FString::Join(AllTools, TEXT(",")));
	}

	// Write prompts to files to avoid command line length limits (Error 206)
	FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"));
	IFileManager::Get().MakeDirectory(*TempDir, true);

	// System prompt - scope check + write
	if (!MaterializedSystemPrompt.IsEmpty())
	{
		FString SystemPromptPath = FPaths::Combine(TempDir, TEXT("system-prompt.txt"));
		auto SysScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(SystemPromptPath);
		if (SysScopeCheck.bAllowed)
		{
			if (FFileHelper::SaveStringToFile(MaterializedSystemPrompt, *SystemPromptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				SystemPromptFilePath = SystemPromptPath;
				UE_LOG(LogOsvayderUE, Log, TEXT("System prompt written to: %s (%d chars)"), *SystemPromptPath, MaterializedSystemPrompt.Len());
				FExecutionReceipt R; R.Tool = TEXT("claude_runner"); R.bSuccess = true; R.TargetType = TEXT("file");
				R.Targets.Add(SystemPromptPath); R.Created.Add(SystemPromptPath);
				R.Classification = TEXT("internal_state"); R.ValidationSummary = TEXT("system-prompt.txt");
				FOsvayderUEExecutionLog::Get().AddReceipt(R);
			}
			else
			{
				FExecutionReceipt R; R.Tool = TEXT("claude_runner"); R.bSuccess = false; R.TargetType = TEXT("file");
				R.Targets.Add(SystemPromptPath); R.Classification = TEXT("internal_state"); R.ValidationSummary = TEXT("File write failed");
				FOsvayderUEExecutionLog::Get().AddReceipt(R);
			}
		}
		else
		{
			FExecutionReceipt R; R.Tool = TEXT("claude_runner"); R.bSuccess = false; R.TargetType = TEXT("file");
			R.Targets.Add(SystemPromptPath); R.Classification = TEXT("denied"); R.ValidationSummary = SysScopeCheck.DenialReason;
			FOsvayderUEExecutionLog::Get().AddReceipt(R);
		}
	}

	// User prompt - scope check + write
	FString PromptPath = FPaths::Combine(TempDir, TEXT("prompt.txt"));
	auto PromptScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(PromptPath);
	if (PromptScopeCheck.bAllowed)
	{
		if (FFileHelper::SaveStringToFile(Config.Prompt, *PromptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			PromptFilePath = PromptPath;
			UE_LOG(LogOsvayderUE, Log, TEXT("Prompt written to: %s (%d chars)"), *PromptPath, Config.Prompt.Len());
			FExecutionReceipt R; R.Tool = TEXT("claude_runner"); R.bSuccess = true; R.TargetType = TEXT("file");
			R.Targets.Add(PromptPath); R.Created.Add(PromptPath);
			R.Classification = TEXT("internal_state"); R.ValidationSummary = TEXT("prompt.txt");
			FOsvayderUEExecutionLog::Get().AddReceipt(R);
		}
		else
		{
			FExecutionReceipt R; R.Tool = TEXT("claude_runner"); R.bSuccess = false; R.TargetType = TEXT("file");
			R.Targets.Add(PromptPath); R.Classification = TEXT("internal_state"); R.ValidationSummary = TEXT("File write failed");
			FOsvayderUEExecutionLog::Get().AddReceipt(R);
		}
	}
	else
	{
		FExecutionReceipt R; R.Tool = TEXT("claude_runner"); R.bSuccess = false; R.TargetType = TEXT("file");
		R.Targets.Add(PromptPath); R.Classification = TEXT("denied"); R.ValidationSummary = PromptScopeCheck.DenialReason;
		FOsvayderUEExecutionLog::Get().AddReceipt(R);
	}

	// Don't add prompts to command line - we'll pipe them via stdin.
	// Spec 621 §2: multi-turn memory rides on --session-id (first turn) / --resume (subsequent turns) emitted above,
	// not on in-band prompt history.
	return CommandLine;
}

FString FOsvayderCodeRunner::BuildStreamJsonPayload(const FString& TextPrompt, const TArray<FString>& ImagePaths)
{
	using namespace OsvayderUEConstants::ClipboardImage;

	// Pre-compute expected directory once for all images
	FString ExpectedDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("screenshots")));

	// Build content blocks array
	TArray<TSharedPtr<FJsonValue>> ContentArray;

	// Text content block (system context + user message)
	if (!TextPrompt.IsEmpty())
	{
		TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
		TextBlock->SetStringField(TEXT("type"), TEXT("text"));
		TextBlock->SetStringField(TEXT("text"), TextPrompt);
		ContentArray.Add(MakeShared<FJsonValueObject>(TextBlock));
	}

	// Image content blocks (base64-encoded PNGs)
	int32 EncodedCount = 0;
	int64 TotalImageBytes = 0;
	const int32 MaxCount = FMath::Min(ImagePaths.Num(), MaxImagesPerMessage);

	for (int32 i = 0; i < MaxCount; ++i)
	{
		const FString& ImagePath = ImagePaths[i];
		if (ImagePath.IsEmpty())
		{
			continue;
		}

		FString FullImagePath = FPaths::ConvertRelativePathToFull(ImagePath);

		if (FullImagePath.Contains(TEXT("..")))
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("Rejecting image path with traversal: %s"), *FullImagePath);
			continue;
		}
		if (!FullImagePath.StartsWith(ExpectedDir))
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("Rejecting image path outside screenshots directory: %s"), *FullImagePath);
			continue;
		}
		if (!FPaths::FileExists(FullImagePath))
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("Attached image file no longer exists: %s"), *FullImagePath);
			continue;
		}

		// Check per-file size
		const int64 FileSize = IFileManager::Get().FileSize(*FullImagePath);
		if (FileSize > MaxImageFileSize)
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("Image file too large for base64 encoding: %s (%lld bytes, max %lld)"),
				*FullImagePath, FileSize, MaxImageFileSize);
			continue;
		}

		// Check total payload size
		if (TotalImageBytes + FileSize > MaxTotalImagePayloadSize)
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("Skipping image (total payload would exceed %lld bytes): %s"),
				MaxTotalImagePayloadSize, *FullImagePath);
			continue;
		}

		// Load and base64 encode the PNG
		TArray<uint8> ImageData;
		if (!FFileHelper::LoadFileToArray(ImageData, *FullImagePath))
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to load image file for base64 encoding: %s"), *FullImagePath);
			continue;
		}

		FString Base64ImageData = FBase64::Encode(ImageData);
		TotalImageBytes += FileSize;

		TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("type"), TEXT("base64"));
		Source->SetStringField(TEXT("media_type"), TEXT("image/png"));
		Source->SetStringField(TEXT("data"), Base64ImageData);

		TSharedPtr<FJsonObject> ImageBlock = MakeShared<FJsonObject>();
		ImageBlock->SetStringField(TEXT("type"), TEXT("image"));
		ImageBlock->SetObjectField(TEXT("source"), Source);
		ContentArray.Add(MakeShared<FJsonValueObject>(ImageBlock));

		EncodedCount++;
		UE_LOG(LogOsvayderUE, Log, TEXT("Base64 encoded image [%d]: %s (%d bytes -> %d chars)"),
			EncodedCount, *FullImagePath, ImageData.Num(), Base64ImageData.Len());
	}

	if (EncodedCount > 0)
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("Encoded %d image(s), total %lld bytes"), EncodedCount, TotalImageBytes);
	}

	// Build the inner message object: {"role":"user","content":[...]}
	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("role"), TEXT("user"));
	Message->SetArrayField(TEXT("content"), ContentArray);

	// Build the outer SDKUserMessage envelope: {"type":"user","message":{...}}
	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("type"), TEXT("user"));
	Envelope->SetObjectField(TEXT("message"), Message);

	// Serialize to condensed JSON (single line for NDJSON)
	FString JsonLine;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonLine);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);
	Writer->Close();

	// NDJSON requires newline termination
	JsonLine += TEXT("\n");

	UE_LOG(LogOsvayderUE, Log, TEXT("Built stream-json payload: %d chars (images: %d)"),
		JsonLine.Len(), EncodedCount);

	return JsonLine;
}

FString FOsvayderCodeRunner::ParseStreamJsonOutput(const FString& RawOutput)
{
	// Stream-json output is NDJSON: one JSON object per line
	// We look for the "result" message which contains the final response text
	// Format: {"type":"result","result":"the text response",...}
	// Fallback: accumulate text from "assistant" content blocks

	TArray<FString> Lines;
	RawOutput.ParseIntoArrayLines(Lines);

	// First pass: look for the "result" message
	for (const FString& Line : Lines)
	{
		if (Line.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			continue;
		}

		FString Type;
		if (!JsonObj->TryGetStringField(TEXT("type"), Type))
		{
			continue;
		}

		if (Type == TEXT("result"))
		{
			FString ResultText;
			if (JsonObj->TryGetStringField(TEXT("result"), ResultText))
			{
				UE_LOG(LogOsvayderUE, Log, TEXT("Parsed stream-json result: %d chars"), ResultText.Len());
				return ResultText;
			}
		}
	}

	// Fallback: accumulate text from assistant content blocks
	FString AccumulatedText;
	for (const FString& Line : Lines)
	{
		if (Line.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			continue;
		}

		FString Type;
		if (!JsonObj->TryGetStringField(TEXT("type"), Type))
		{
			continue;
		}

		if (Type == TEXT("assistant"))
		{
			const TSharedPtr<FJsonObject>* MessageObj;
			if (JsonObj->TryGetObjectField(TEXT("message"), MessageObj))
			{
				const TArray<TSharedPtr<FJsonValue>>* ContentArray;
				if ((*MessageObj)->TryGetArrayField(TEXT("content"), ContentArray))
				{
					for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
					{
						const TSharedPtr<FJsonObject>* ContentObj;
						if (ContentValue->TryGetObject(ContentObj))
						{
							FString ContentType;
							if ((*ContentObj)->TryGetStringField(TEXT("type"), ContentType) && ContentType == TEXT("text"))
							{
								FString Text;
								if ((*ContentObj)->TryGetStringField(TEXT("text"), Text))
								{
									AccumulatedText += Text;
								}
							}
						}
					}
				}
			}
		}
	}

	if (!AccumulatedText.IsEmpty())
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("Parsed stream-json from assistant blocks: %d chars"), AccumulatedText.Len());
		return AccumulatedText;
	}

	// Last resort: return a user-friendly error instead of raw NDJSON
	UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to parse stream-json output (%d chars). Raw output logged below:"), RawOutput.Len());
	UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *RawOutput.Left(2000));
	return TEXT("Error: Failed to parse Claude's response. Check the Output Log for details.");
}

void FOsvayderCodeRunner::ParseAndEmitNdjsonLine(const FString& JsonLine)
{
	if (JsonLine.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonLine);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		UE_LOG(LogOsvayderUE, Verbose, TEXT("NDJSON: Non-JSON line (skipping): %.200s"), *JsonLine);
		return;
	}

	FString Type;
	if (!JsonObj->TryGetStringField(TEXT("type"), Type))
	{
		UE_LOG(LogOsvayderUE, Verbose, TEXT("NDJSON: Line missing 'type' field"));
		return;
	}

	UE_LOG(LogOsvayderUE, Log, TEXT("NDJSON Event: type=%s"), *Type);

	if (Type == TEXT("system"))
	{
		// Session init event
		FString Subtype;
		JsonObj->TryGetStringField(TEXT("subtype"), Subtype);
		FString SessionId;
		JsonObj->TryGetStringField(TEXT("session_id"), SessionId);

		// Log full details for api_retry events to diagnose connection issues
		FString ErrorStr, ErrorStatus;
		JsonObj->TryGetStringField(TEXT("error"), ErrorStr);
		JsonObj->TryGetStringField(TEXT("error_status"), ErrorStatus);
		double Attempt = 0;
		JsonObj->TryGetNumberField(TEXT("attempt"), Attempt);

		if (Subtype == TEXT("api_retry"))
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("NDJSON System: subtype=%s, attempt=%.0f, error=%s, error_status=%s, session_id=%s, raw=%s"),
				*Subtype, Attempt, *ErrorStr, *ErrorStatus, *SessionId, *JsonLine.Left(500));
		}
		else
		{
			UE_LOG(LogOsvayderUE, Log, TEXT("NDJSON System: subtype=%s, session_id=%s"), *Subtype, *SessionId);
		}

		if (CurrentConfig.OnStreamEvent.IsBound())
		{
			FOsvayderStreamEvent Event;
			Event.Type = EOsvayderStreamEventType::SessionInit;
			Event.SessionId = SessionId;
			Event.RawJson = JsonLine;
			FOnOsvayderStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
			AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
			{
				EventDelegate.ExecuteIfBound(Event);
			});
		}
	}
	else if (Type == TEXT("assistant"))
	{
		// Assistant message with content blocks
		const TSharedPtr<FJsonObject>* MessageObj;
		if (!JsonObj->TryGetObjectField(TEXT("message"), MessageObj))
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("NDJSON: assistant message missing 'message' field"));
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* ContentArray;
		if (!(*MessageObj)->TryGetArrayField(TEXT("content"), ContentArray))
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("NDJSON: assistant message.content missing"));
			return;
		}

		for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
		{
			const TSharedPtr<FJsonObject>* ContentObj;
			if (!ContentValue->TryGetObject(ContentObj))
			{
				continue;
			}

			FString ContentType;
			if (!(*ContentObj)->TryGetStringField(TEXT("type"), ContentType))
			{
				continue;
			}

			if (ContentType == TEXT("text"))
			{
				FString Text;
				if ((*ContentObj)->TryGetStringField(TEXT("text"), Text))
				{
					UE_LOG(LogOsvayderUE, Log, TEXT("NDJSON TextContent: %d chars"), Text.Len());
					AccumulatedResponseText += Text;

					// Fire old progress delegate for backward compat
					if (OnProgressDelegate.IsBound())
					{
						FOnOsvayderProgress ProgressCopy = OnProgressDelegate;
						AsyncTask(ENamedThreads::GameThread, [ProgressCopy, Text]()
						{
							ProgressCopy.ExecuteIfBound(Text);
						});
					}

					// Fire new structured event
					if (CurrentConfig.OnStreamEvent.IsBound())
					{
						FOsvayderStreamEvent Event;
						Event.Type = EOsvayderStreamEventType::TextContent;
						Event.Text = Text;
						FOnOsvayderStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
						AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
						{
							EventDelegate.ExecuteIfBound(Event);
						});
					}
				}
			}
			else if (ContentType == TEXT("tool_use"))
			{
				FString ToolName, ToolId;
				(*ContentObj)->TryGetStringField(TEXT("name"), ToolName);
				(*ContentObj)->TryGetStringField(TEXT("id"), ToolId);

				// Serialize input to string
				FString ToolInputStr;
				const TSharedPtr<FJsonObject>* InputObj;
				if ((*ContentObj)->TryGetObjectField(TEXT("input"), InputObj))
				{
					TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
						TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ToolInputStr);
					FJsonSerializer::Serialize((*InputObj).ToSharedRef(), Writer);
					Writer->Close();
				}

				UE_LOG(LogOsvayderUE, Log, TEXT("NDJSON ToolUse: name=%s, id=%s, input=%d chars"),
					*ToolName, *ToolId, ToolInputStr.Len());

				if (CurrentConfig.OnStreamEvent.IsBound())
				{
					FOsvayderStreamEvent Event;
					Event.Type = EOsvayderStreamEventType::ToolUse;
					Event.ToolName = ToolName;
					Event.ToolCallId = ToolId;
					Event.ToolInput = ToolInputStr;
					Event.RawJson = JsonLine;
					FOnOsvayderStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
					AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
					{
						EventDelegate.ExecuteIfBound(Event);
					});
				}
			}
			else
			{
				UE_LOG(LogOsvayderUE, Verbose, TEXT("NDJSON: unknown content block type: %s"), *ContentType);
			}
		}
	}
	else if (Type == TEXT("user"))
	{
		// Tool result message
		const TSharedPtr<FJsonObject>* MessageObj;
		if (!JsonObj->TryGetObjectField(TEXT("message"), MessageObj))
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* ContentArray;
		if (!(*MessageObj)->TryGetArrayField(TEXT("content"), ContentArray))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
		{
			const TSharedPtr<FJsonObject>* ContentObj;
			if (!ContentValue->TryGetObject(ContentObj))
			{
				continue;
			}

			FString ContentType;
			if (!(*ContentObj)->TryGetStringField(TEXT("type"), ContentType))
			{
				continue;
			}

			if (ContentType == TEXT("tool_result"))
			{
				FString ToolUseId, ResultContent;
				(*ContentObj)->TryGetStringField(TEXT("tool_use_id"), ToolUseId);

				// content can be a string OR an array of content blocks
				if (!(*ContentObj)->TryGetStringField(TEXT("content"), ResultContent))
				{
					// Extract text from content block array: [{"type":"text","text":"..."},...]
					const TArray<TSharedPtr<FJsonValue>>* ResultArray;
					if ((*ContentObj)->TryGetArrayField(TEXT("content"), ResultArray))
					{
						for (const TSharedPtr<FJsonValue>& Block : *ResultArray)
						{
							const TSharedPtr<FJsonObject>* BlockObj;
							if (Block->TryGetObject(BlockObj))
							{
								FString BlockType;
								(*BlockObj)->TryGetStringField(TEXT("type"), BlockType);
								if (BlockType == TEXT("text"))
								{
									FString BlockText;
									if ((*BlockObj)->TryGetStringField(TEXT("text"), BlockText))
									{
										if (!ResultContent.IsEmpty())
										{
											ResultContent += TEXT("\n");
										}
										ResultContent += BlockText;
									}
								}
							}
						}
					}
				}

				UE_LOG(LogOsvayderUE, Log, TEXT("NDJSON ToolResult: tool_use_id=%s, content=%d chars"),
					*ToolUseId, ResultContent.Len());

				if (CurrentConfig.OnStreamEvent.IsBound())
				{
					FOsvayderStreamEvent Event;
					Event.Type = EOsvayderStreamEventType::ToolResult;
					Event.ToolCallId = ToolUseId;
					Event.ToolResultContent = ResultContent;
					Event.RawJson = JsonLine;
					FOnOsvayderStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
					AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
					{
						EventDelegate.ExecuteIfBound(Event);
					});
				}
			}
		}
	}
	else if (Type == TEXT("result"))
	{
		// Final result message with stats
		FString ResultText, Subtype;
		JsonObj->TryGetStringField(TEXT("result"), ResultText);
		JsonObj->TryGetStringField(TEXT("subtype"), Subtype);
		bool bIsError = false;
		JsonObj->TryGetBoolField(TEXT("is_error"), bIsError);
		double DurationMs = 0.0;
		JsonObj->TryGetNumberField(TEXT("duration_ms"), DurationMs);
		double NumTurns = 0.0;
		JsonObj->TryGetNumberField(TEXT("num_turns"), NumTurns);
		double TotalCostUsd = 0.0;
		JsonObj->TryGetNumberField(TEXT("total_cost_usd"), TotalCostUsd);

		UE_LOG(LogOsvayderUE, Log, TEXT("NDJSON Result: subtype=%s, is_error=%d, duration=%.0fms, turns=%.0f, cost=$%.4f, result=%d chars"),
			*Subtype, bIsError, DurationMs, NumTurns, TotalCostUsd, ResultText.Len());

		if (CurrentConfig.OnStreamEvent.IsBound())
		{
			FOsvayderStreamEvent Event;
			Event.Type = EOsvayderStreamEventType::Result;
			Event.ResultText = ResultText;
			Event.bIsError = bIsError;
			Event.DurationMs = static_cast<int32>(DurationMs);
			Event.NumTurns = static_cast<int32>(NumTurns);
			Event.TotalCostUsd = static_cast<float>(TotalCostUsd);
			Event.RawJson = JsonLine;
			FOnOsvayderStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
			AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
			{
				EventDelegate.ExecuteIfBound(Event);
			});
		}
	}
	else
	{
		UE_LOG(LogOsvayderUE, Verbose, TEXT("NDJSON: unhandled message type: %s"), *Type);
	}
}

void FOsvayderCodeRunner::Cancel()
{
	// Signal stop first - ReadProcessOutput() checks this and will exit its loop
	StopTaskCounter.Set(1);

	// Terminate the process to unblock any pending pipe reads
	// Don't close pipes/handles here - ExecuteProcess() on the worker thread
	// will handle cleanup after ReadProcessOutput() returns
	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(ProcessHandle, true);
	}
}

bool FOsvayderCodeRunner::Init()
{
	// bIsExecuting is already set by ExecuteAsync (thread-safe)
	StopTaskCounter.Reset();
	NdjsonLineBuffer.Empty();
	AccumulatedResponseText.Empty();
	return true;
}

uint32 FOsvayderCodeRunner::Run()
{
	ExecuteProcess();
	return 0;
}

void FOsvayderCodeRunner::Stop()
{
	StopTaskCounter.Increment();
}

void FOsvayderCodeRunner::Exit()
{
	bIsExecuting = false;
}

bool FOsvayderCodeRunner::CreateProcessPipes()
{
	// Create stdout pipe (we read from ReadPipe, child writes to WritePipe)
	if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe, false))
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to create stdout pipe"));
		return false;
	}

	// Create stdin pipe (child reads from StdInReadPipe, we write to StdInWritePipe)
	if (!FPlatformProcess::CreatePipe(StdInReadPipe, StdInWritePipe, true))
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to create stdin pipe"));
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = nullptr;
		WritePipe = nullptr;
		return false;
	}

	return true;
}

bool FOsvayderCodeRunner::LaunchProcess(const FString& FullCommand, const FString& WorkingDir)
{
	FString ClaudePath = GetClaudePath();

	FString Params;
	FString ActualExe;

#if PLATFORM_WINDOWS
	// Read critical environment variables that Claude CLI needs for credential
	// discovery and operation. UE's CreateProc inherits the editor's environment,
	// which after a system restart may lack user-level variables.
	FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	FString TempPath = FPlatformMisc::GetEnvironmentVariable(TEXT("TEMP"));

	// Build inline env var setup for cmd.exe (no space before && to avoid trailing spaces in values)
	FString EnvSetup;
	if (!UserProfile.IsEmpty())
	{
		EnvSetup += FString::Printf(TEXT("set USERPROFILE=%s&& set HOME=%s&& "), *UserProfile, *UserProfile);
	}
	if (!AppData.IsEmpty())
	{
		EnvSetup += FString::Printf(TEXT("set APPDATA=%s&& "), *AppData);
	}
	if (!LocalAppData.IsEmpty())
	{
		EnvSetup += FString::Printf(TEXT("set LOCALAPPDATA=%s&& "), *LocalAppData);
	}
	if (!TempPath.IsEmpty())
	{
		EnvSetup += FString::Printf(TEXT("set TEMP=%s&& set TMP=%s&& "), *TempPath, *TempPath);
	}

	// Diagnostic logging
	UE_LOG(LogOsvayderUE, Log, TEXT("Process env: USERPROFILE=%s, APPDATA=%s, LOCALAPPDATA=%s"),
		*UserProfile, *AppData, *LocalAppData);

	FString CredsPath = FPaths::Combine(UserProfile, TEXT(".claude"), TEXT(".credentials.json"));
	bool bCredsExist = IFileManager::Get().FileExists(*CredsPath);
	UE_LOG(LogOsvayderUE, Log, TEXT("Credentials check: %s (exists=%d)"), *CredsPath, bCredsExist);
	if (!bCredsExist)
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Claude credentials not found! Run 'claude login' in terminal first."));
	}

	// Launch via cmd.exe /c with explicit env vars + stderr redirect
	ActualExe = TEXT("cmd.exe");
	if (!CachedCliJsPath.IsEmpty())
	{
		Params = FString::Printf(TEXT("/c \"%s\"%s\" \"%s\" %s 2>&1\""),
			*EnvSetup, *ClaudePath, *CachedCliJsPath, *FullCommand);
	}
	else
	{
		Params = FString::Printf(TEXT("/c \"%s\"%s\" %s 2>&1\""),
			*EnvSetup, *ClaudePath, *FullCommand);
	}
	UE_LOG(LogOsvayderUE, Log, TEXT("Launching via cmd.exe: %s %s"), *ActualExe, *Params);
#else
	ActualExe = ClaudePath;
	Params = FullCommand;
#endif

	ProcessHandle = FPlatformProcess::CreateProc(
		*ActualExe,
		*Params,
		false,    // bLaunchDetached
		false,    // bLaunchHidden
		true,     // bLaunchReallyHidden
		nullptr,  // OutProcessID
		0,        // PriorityModifier
		*WorkingDir,
		WritePipe,    // PipeWriteChild - child's stdout goes here
		StdInReadPipe // PipeReadChild - child reads stdin from here
	);

	if (!ProcessHandle.IsValid())
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to create Claude process"));
		UE_LOG(LogOsvayderUE, Error, TEXT("Claude Path: %s"), *ClaudePath);
		UE_LOG(LogOsvayderUE, Error, TEXT("Params: %s"), *Params);
		UE_LOG(LogOsvayderUE, Error, TEXT("Working directory: %s"), *WorkingDir);
		return false;
	}

	return true;
}

FString FOsvayderCodeRunner::ReadProcessOutput()
{
	FString FullOutput;

	// Reset NDJSON state for this request
	NdjsonLineBuffer.Empty();
	AccumulatedResponseText.Empty();

	while (!StopTaskCounter.GetValue())
	{
		// Read any available output from the pipe
		FString OutputChunk = FPlatformProcess::ReadPipe(ReadPipe);

		if (!OutputChunk.IsEmpty())
		{
			FullOutput += OutputChunk;

			// Parse NDJSON line-by-line: buffer chunks and split on newlines
			NdjsonLineBuffer += OutputChunk;

			int32 NewlineIdx;
			while (NdjsonLineBuffer.FindChar(TEXT('\n'), NewlineIdx))
			{
				FString CompleteLine = NdjsonLineBuffer.Left(NewlineIdx);
				CompleteLine.TrimEndInline();
				NdjsonLineBuffer.RightChopInline(NewlineIdx + 1);

				if (!CompleteLine.IsEmpty())
				{
					ParseAndEmitNdjsonLine(CompleteLine);
				}
			}
		}

		// Check if process has exited
		if (!FPlatformProcess::IsProcRunning(ProcessHandle))
		{
			// Process finished - read any remaining output
			FString RemainingOutput = FPlatformProcess::ReadPipe(ReadPipe);
			while (!RemainingOutput.IsEmpty())
			{
				FullOutput += RemainingOutput;
				NdjsonLineBuffer += RemainingOutput;
				RemainingOutput = FPlatformProcess::ReadPipe(ReadPipe);
			}

			// Parse all remaining buffered lines
			int32 FinalNewlineIdx;
			while (NdjsonLineBuffer.FindChar(TEXT('\n'), FinalNewlineIdx))
			{
				FString CompleteLine = NdjsonLineBuffer.Left(FinalNewlineIdx);
				CompleteLine.TrimEndInline();
				NdjsonLineBuffer.RightChopInline(FinalNewlineIdx + 1);

				if (!CompleteLine.IsEmpty())
				{
					ParseAndEmitNdjsonLine(CompleteLine);
				}
			}

			// Parse any final incomplete line (no trailing newline)
			NdjsonLineBuffer.TrimEndInline();
			if (!NdjsonLineBuffer.IsEmpty())
			{
				ParseAndEmitNdjsonLine(NdjsonLineBuffer);
				NdjsonLineBuffer.Empty();
			}

			break;
		}

		// Brief sleep to avoid busy-waiting
		FPlatformProcess::Sleep(0.01f);
	}

	return FullOutput;
}

void FOsvayderCodeRunner::ReportError(const FString& ErrorMessage)
{
	FOnOsvayderResponse CompleteCopy = OnCompleteDelegate;
	FString Message = ErrorMessage;
	AsyncTask(ENamedThreads::GameThread, [CompleteCopy, Message]()
	{
		CompleteCopy.ExecuteIfBound(Message, false);
	});
}

void FOsvayderCodeRunner::ReportCompletion(const FString& Output, bool bSuccess)
{
	FOnOsvayderResponse CompleteCopy = OnCompleteDelegate;
	FString FinalOutput = Output;
	AsyncTask(ENamedThreads::GameThread, [CompleteCopy, FinalOutput, bSuccess]()
	{
		CompleteCopy.ExecuteIfBound(FinalOutput, bSuccess);
	});
}

void FOsvayderCodeRunner::ExecuteProcess()
{
	FString ClaudePath = GetClaudePath();

	// Verify the path exists
	if (ClaudePath.IsEmpty())
	{
		ReportError(TEXT("Claude CLI not found. Please install with: npm install -g @anthropic-ai/claude-code"));
		return;
	}

	if (!IFileManager::Get().FileExists(*ClaudePath))
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Claude path no longer exists: %s"), *ClaudePath);
		ReportError(FString::Printf(TEXT("Claude CLI path invalid: %s"), *ClaudePath));
		return;
	}

	FString CommandLine = BuildCommandLine(CurrentConfig);

	UE_LOG(LogOsvayderUE, Log, TEXT("Async executing Claude: %s %s"), *ClaudePath, *CommandLine);

	// Set working directory - convert to absolute path since FPaths::ProjectDir()
	// returns a relative path on macOS that external processes can't resolve
	FString WorkingDir = CurrentConfig.WorkingDirectory;
	if (WorkingDir.IsEmpty())
	{
		WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}

	// Create pipes for stdout capture
	if (!CreateProcessPipes())
	{
		ReportError(TEXT("Failed to create pipe for Claude process"));
		return;
	}

	UE_LOG(LogOsvayderUE, Log, TEXT("Full command: %s %s"), *ClaudePath, *CommandLine);
	UE_LOG(LogOsvayderUE, Log, TEXT("Working directory: %s"), *WorkingDir);

	if (!LaunchProcess(CommandLine, WorkingDir))
	{
		CleanupHandles();

		FString ErrorMsg = FString::Printf(
			TEXT("Failed to start Claude process.\n\n")
			TEXT("Claude Path: %s\n")
			TEXT("Working Dir: %s\n\n")
			TEXT("Command (truncated): %.200s..."),
			*ClaudePath,
			*WorkingDir,
			*CommandLine
		);
		ReportError(ErrorMsg);
		return;
	}

	// Spec 621 reviewer 2026-04-17 15:30 v4 / P1 + P4 + v5 empty-stdin fix: stdin is the plain user prompt —
	// no `[CONTEXT]` wrap, no NDJSON envelope. The prior wrap stuffed the full materialized system prompt
	// into user content AND used a stream-json envelope that Claude CLI silently rejected because our
	// envelope omitted the SDK-required `session_id`/`uuid`/`parent_tool_use_id` fields. Combined with the
	// P2 removal of `--input-format stream-json` flag, the CLI now interprets stdin as the user message
	// under `-p` per baseline 1a evidence. System prompt delivery for directives rides `--append-system-prompt`
	// on every invocation (see ComposeLanguageHintFlag). `system-prompt.txt` write in BuildCommandLine remains
	// for debug/audit visibility but is no longer consumed here.
	//
	// v5 empty-stdin fix (2026-04-17 16:00 reviewer): previous v4 implementation read the prompt from
	// `PromptFilePath` via `LoadFileToString`. If the file write in BuildCommandLine was scope-denied or if
	// the read race-failed, PromptContent became empty, stdin write sent 0 bytes, CLI exited with stderr
	// "Input must be provided either through stdin or as a prompt argument when using --print" and empty
	// stdout — which then produced the user-visible "Failed to parse Claude's response" after ParseStreamJsonOutput
	// found nothing parseable. Using `CurrentConfig.Prompt` directly as the primary source eliminates the
	// file-roundtrip failure surface entirely; the file read stays as a backup fallback.
	if (StdInWritePipe)
	{
		FString PromptContent = CurrentConfig.Prompt;
		if (PromptContent.IsEmpty() && !PromptFilePath.IsEmpty())
		{
			FFileHelper::LoadFileToString(PromptContent, *PromptFilePath);
		}
		if (CurrentConfig.AttachedImagePaths.Num() > 0)
		{
			UE_LOG(LogOsvayderUE, Warning,
				TEXT("Claude runner: %d image attachment(s) present but stream-json input path is deferred per 621 v4 P2; images NOT sent this turn."),
				CurrentConfig.AttachedImagePaths.Num());
		}

		FString StdinPayload = ComposeStdinPayloadForClaudePrint(PromptContent);

		// Write to stdin
		if (!StdinPayload.IsEmpty())
		{
			FTCHARToUTF8 Utf8Payload(*StdinPayload);
			int32 BytesWritten = 0;
			bool bWritten = FPlatformProcess::WritePipe(StdInWritePipe, (const uint8*)Utf8Payload.Get(), Utf8Payload.Length(), &BytesWritten);
			const UOsvayderUESettings* DiagSettings = UOsvayderUESettings::Get();
			FString DiagLanguageDisplayName;
			if (DiagSettings && DiagSettings->ShouldForwardLanguageToClaudeSystemPrompt())
			{
				DiagLanguageDisplayName = GetClaudeLanguageDisplayName(DiagSettings->GetConfiguredDictationLanguage());
			}
			const int32 MaterializedSystemPromptLen = CurrentConfig.PromptContract.HasAnyContent()
				? FAgentPromptMaterializer::MaterializeClaudeSystemPrompt(CurrentConfig.PromptContract, DiagLanguageDisplayName).Len()
				: CurrentConfig.SystemPrompt.Len();
			UE_LOG(LogOsvayderUE, Log, TEXT("Wrote to Claude stdin (stream-json, success=%d, %d/%d bytes, images: %d, system: %d chars, user: %d chars)"),
				bWritten, BytesWritten, Utf8Payload.Length(), CurrentConfig.AttachedImagePaths.Num(),
				MaterializedSystemPromptLen, CurrentConfig.Prompt.Len());
		}

		// Close stdin write pipe to signal EOF to Claude
		// We close the entire stdin pipe pair since child has the read end
		FPlatformProcess::ClosePipe(StdInReadPipe, StdInWritePipe);
		StdInReadPipe = nullptr;
		StdInWritePipe = nullptr;
	}

	// Clear temp file paths
	SystemPromptFilePath.Empty();
	PromptFilePath.Empty();

	// Read output until process completes (NDJSON events parsed during reading)
	FString FullOutput = ReadProcessOutput();

	// Use accumulated response text from parsed NDJSON events
	// Falls back to legacy ParseStreamJsonOutput if no events were parsed
	FString ResponseText = AccumulatedResponseText;
	if (ResponseText.IsEmpty() && !FullOutput.IsEmpty())
	{
		// Fallback: try legacy parsing in case NDJSON format wasn't as expected
		ResponseText = ParseStreamJsonOutput(FullOutput);
		UE_LOG(LogOsvayderUE, Log, TEXT("NDJSON parser produced no text, fell back to legacy parser (%d chars)"),
			ResponseText.Len());
	}

	// Get exit code
	int32 ExitCode = 0;
	FPlatformProcess::GetProcReturnCode(ProcessHandle, &ExitCode);

	// Cleanup handles
	if (ReadPipe || WritePipe)
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = nullptr;
		WritePipe = nullptr;
	}
	FPlatformProcess::CloseProc(ProcessHandle);

	// Spec 621 §2 + reviewer 2026-04-17 14:10 fix [A1-1]: if Claude CLI rejected our --resume <uuid> (stale
	// session store, CLI downgrade, or Anthropic 400 invalid_request_error from replaying a poisoned native-store
	// message), auto-reset the persistent artifact and retry this same turn once with a fresh --session-id.
	const UOsvayderUESettings* RetrySettings = UOsvayderUESettings::Get();
	const bool bPersistentSessionEnabled = RetrySettings && RetrySettings->ShouldUseClaudePersistentSession();
	const bool bUserCancelled = StopTaskCounter.GetValue() != 0;
	const bool bShouldReset =
		!bUserCancelled
		&& bPersistentSessionEnabled
		&& !bAlreadyRetriedPersistentSession
		&& ShouldAutoResetOnExit(ExitCode, FullOutput, bCurrentRunUsedResume);

	if (bShouldReset)
	{
		UE_LOG(LogOsvayderUE, Warning,
			TEXT("Claude --resume appears to have failed (exit=%d). Resetting persistent session artifact and retrying once with a fresh --session-id."),
			ExitCode);
		GetPersistentSessionManagerSingleton().ResetPersistentSession(EOsvayderUEProviderBackend::ClaudeCli);
		bAlreadyRetriedPersistentSession = true;

		// Clear temp prompt paths (BuildCommandLine will re-materialize them on retry).
		SystemPromptFilePath.Empty();
		PromptFilePath.Empty();

		ExecuteProcess();
		return;
	}

	// Reset retry guard for the next ExecuteAsync call.
	bAlreadyRetriedPersistentSession = false;

	// Report completion with parsed response text
	bool bSuccess = (ExitCode == 0) && !StopTaskCounter.GetValue();
	ReportCompletion(ResponseText, bSuccess);
}

// FOsvayderCodeSubsystem is now in OsvayderSubsystem.cpp
