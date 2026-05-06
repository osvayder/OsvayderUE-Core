// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IOsvayderRunner.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"

enum class EOsvayderUEDictationLanguage : uint8;

/**
 * Async runner for Claude Code CLI commands (cross-platform implementation)
 * Executes 'claude -p' in print mode and captures output
 * Implements IOsvayderRunner interface for abstraction
 */
class OSVAYDERUE_API FOsvayderCodeRunner : public IOsvayderRunner, public FRunnable
{
public:
	FOsvayderCodeRunner();
	virtual ~FOsvayderCodeRunner();

	// IOsvayderRunner interface
	virtual bool ExecuteAsync(
		const FOsvayderRequestConfig& Config,
		FOnOsvayderResponse OnComplete,
		FOnOsvayderProgress OnProgress = FOnOsvayderProgress()
	) override;

	virtual bool ExecuteSync(const FOsvayderRequestConfig& Config, FString& OutResponse) override;
	virtual void Cancel() override;
	virtual bool IsExecuting() const override { return bIsExecuting; }
	virtual bool IsAvailable() const override { return IsClaudeAvailable(); }
	virtual EOsvayderUEProviderBackend GetBackendType() const override { return EOsvayderUEProviderBackend::ClaudeCli; }
	virtual FString GetBackendDisplayName() const override { return TEXT("Claude CLI"); }
	virtual FAgentBackendCapabilities GetCapabilities() const override;
	virtual FAgentBackendStatus GetStatus() const override;

	/** Check if Claude CLI is available on this system (static for backward compatibility) */
	static bool IsClaudeAvailable();

	/** Get the Claude CLI path */
	static FString GetClaudePath();

	// FRunnable interface
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

	/** Build stream-json NDJSON payload with text + base64 image content blocks */
	FString BuildStreamJsonPayload(const FString& TextPrompt, const TArray<FString>& ImagePaths);

	/** Parse stream-json NDJSON output to extract the response text */
	FString ParseStreamJsonOutput(const FString& RawOutput);

	/**
	 * Spec 621 §2 testable helper. Composes the Claude persistent-session command-line fragment:
	 *   - empty string when bUsePersistentSession is false or SessionId is empty
	 *   - "--session-id <uuid> " on first turn (bWasExisting == false)
	 *   - "--resume <uuid> " on subsequent turns (bWasExisting == true)
	 */
	static FString ComposePersistentSessionFlags(bool bUsePersistentSession, const FString& SessionId, bool bWasExisting);

	/** Spec 621 §3 helper: map DefaultDictationLanguage to a Claude-facing display name. Auto maps to empty (no directive emitted). */
	static FString GetClaudeLanguageDisplayName(EOsvayderUEDictationLanguage Language);

	/**
	 * Spec 621 reviewer 2026-04-17 14:10 fix [A1-2]: Claude CLI applies the materialized system prompt only at
	 * session-create time; on --resume turns our folded language directive never reaches the model. This helper
	 * emits a per-turn `--append-system-prompt "<language line>"` fragment so the directive rides every invocation.
	 * Returns empty string when LanguageDisplayName is empty.
	 */
	static FString ComposeLanguageHintFlag(const FString& LanguageDisplayName);

	/**
	 * Spec 621 reviewer 2026-04-17 14:10 fix [A1-1]: decide whether the just-failed Claude CLI run should trigger
	 * a one-shot auto-reset of the persistent-session artifact. True when ExitCode != 0 AND either a classic
	 * resume-failure string was seen OR (bUsedResume AND Anthropic API 400 invalid_request_error was seen — which
	 * means --resume replayed a poisoned native-store message).
	 */
	static bool ShouldAutoResetOnExit(int32 ExitCode, const FString& Output, bool bUsedResume);

	/**
	 * Spec 621 reviewer 2026-04-17 15:30 v4 / P3: returns `"--model <Name> "` when ModelName is non-empty,
	 * empty string otherwise. The plugin used to store the configured model in the persistent-session artifact
	 * but never forward it to the CLI invocation, so every turn ran on Claude CLI's default model instead of
	 * the configured Opus. Exposed as a static helper so tests can cover both branches without spawning a
	 * real editor session.
	 */
	static FString ComposeModelFlag(const FString& ModelName);

	/**
	 * Plan 623: returns `"--effort <Level> "` when EffortLevel is non-empty (expected CLI values:
	 * `low`/`medium`/`high`/`xhigh`/`max`), empty string otherwise. Empty = do not emit the flag — Claude CLI
	 * uses its own default. Mirrors ComposeModelFlag shape.
	 */
	static FString ComposeEffortFlag(const FString& EffortLevel);

	/**
	 * Spec 621 reviewer 2026-04-17 15:30 v4 / P1: returns the stdin payload the Claude CLI child should receive
	 * on a text-only turn — literally the prompt text plus a trailing newline when one is missing. No
	 * `[CONTEXT]...[/CONTEXT]` wrap, no NDJSON envelope. Paired with the P2 removal of `--input-format stream-json`
	 * so the CLI interprets stdin as the user message under `-p` per baseline 1a evidence. Empty prompt yields
	 * the empty string so the caller can skip the stdin write entirely.
	 */
	static FString ComposeStdinPayloadForClaudePrint(const FString& PromptText);

private:
	FString BuildCommandLine(const FOsvayderRequestConfig& Config);
	void ExecuteProcess();
	void CleanupHandles();

	/** Parse a single NDJSON line and emit structured events */
	void ParseAndEmitNdjsonLine(const FString& JsonLine);

	/** Buffer for accumulating incomplete NDJSON lines across read chunks */
	FString NdjsonLineBuffer;

	/** Accumulated text from assistant messages for the final response */
	FString AccumulatedResponseText;

	/** Create pipes for process stdout/stderr capture */
	bool CreateProcessPipes();

	/** Launch the Claude process with given command */
	bool LaunchProcess(const FString& FullCommand, const FString& WorkingDir);

	/** Read output from process until completion or cancellation */
	FString ReadProcessOutput();

	/** Report error to callback on game thread */
	void ReportError(const FString& ErrorMessage);

	/** Report completion to callback on game thread */
	void ReportCompletion(const FString& Output, bool bSuccess);

	FOsvayderRequestConfig CurrentConfig;
	FOnOsvayderResponse OnCompleteDelegate;
	FOnOsvayderProgress OnProgressDelegate;

	FRunnableThread* Thread;
	FThreadSafeCounter StopTaskCounter;
	TAtomic<bool> bIsExecuting;

	// Process handle (FProcHandle stored as void* for atomic exchange compatibility)
	FProcHandle ProcessHandle;

	// Pipe handles (UE cross-platform pipe handles)
	void* ReadPipe;
	void* WritePipe;
	void* StdInReadPipe;
	void* StdInWritePipe;

	// Temp file paths for prompts (to avoid command line length limits)
	FString SystemPromptFilePath;
	FString PromptFilePath;

	// Spec 621 §2: guard so a single --resume failure within one ExecuteAsync only triggers one auto-reset retry.
	bool bAlreadyRetriedPersistentSession = false;

	// Spec 621 reviewer 2026-04-17 14:10 fix [A1-1]: tracks whether the current command-line emitted `--resume`.
	// Needed so ShouldAutoResetOnExit can scope the `invalid_request_error` signal to resume turns only,
	// avoiding spurious artifact rotation on ordinary first-turn 400s that are unrelated to session replay.
	bool bCurrentRunUsedResume = false;

	// Cached cli.js path for direct node invocation (Windows)
	static FString CachedCliJsPath;
};
