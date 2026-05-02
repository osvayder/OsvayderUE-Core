// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IAgentBackend.h"
#include "Dom/JsonObject.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"

class UNREALCLAUDE_API FCodexCliRunner : public IAgentBackend, public FRunnable
{
public:
	struct FCodexAuthDiagnostics
	{
		FString ConfiguredCodexHomePath;
		FString EffectiveCodexHomePath;
		FString CodexHomeResolutionSource;
		FString CredentialArtifactPath;
		FString AuthState = TEXT("unknown_unprobed");
		FString AuthDetailText;
		FString AuthMode;
		FString EffectiveAuthEntryPath;
		FString AuthOwnershipModel;
		FString ProfileLabel;
		FString ModelName;
		FString RequestedSpeedMode;
		FString EffectiveSpeedMode;
		FString SpeedSupportLabel;
		FString WorkModeName;
		FString ReasoningEffortName;
		FString VerbosityName;
		FString ExecutablePath;
		bool bExecutableAvailable = false;
		bool bCredentialArtifactPresent = false;
		bool bPersistentAppServerEnabled = false;
		bool bProbePerformed = false;
		FString ProbeDetailText;
	};

	FCodexCliRunner();
	virtual ~FCodexCliRunner();

	virtual bool ExecuteAsync(
		const FAgentRequestConfig& Config,
		FOnAgentResponse OnComplete,
		FOnAgentProgress OnProgress = FOnAgentProgress()
	) override;

	virtual bool ExecuteSync(const FAgentRequestConfig& Config, FString& OutResponse) override;
	virtual void Cancel() override;
	virtual bool IsExecuting() const override { return bIsExecuting; }
	virtual bool IsAvailable() const override { return IsCodexAvailable(); }
	virtual EUnrealClaudeProviderBackend GetBackendType() const override { return EUnrealClaudeProviderBackend::CodexCli; }
	virtual FString GetBackendDisplayName() const override;
	virtual FAgentBackendCapabilities GetCapabilities() const override;
	virtual FAgentBackendStatus GetStatus() const override;

	static bool IsCodexAvailable();
	static FString GetCodexPath();
	static FString GetConfiguredAuthModeName();
	static FString GetConfiguredApiKeyEnvVarName();
	static FString GetConfiguredCodexHomePath();
	static FString GetConfiguredCodexHomeResolutionSource();
	static FString GetMachineStandardCodexHomePath();
	static bool IsConfiguredCodexHomeArtifactPresent();
	static TArray<FString> GetDetectedCodexCandidateHomes();
	static FString GetDetectedCodexArtifactHomePath();
	static bool HasExplicitCodexHomeOverride();
	static FString GetConfiguredSpeedModeName();
	static FString GetEffectiveSpeedModeName();
	static FString GetSpeedModeSupportLabel();
	static FString GetEffectiveAuthEntryPath();
	static FString GetEffectiveAuthOwnershipModel();
	static bool ShouldUsePersistentConversationTransport();
	static FString GetEffectiveCodexHomePathForLaunch();
	static FCodexAuthDiagnostics GetAuthDiagnostics();
	static FString BuildAuthDiagnosticsCompactText(const FCodexAuthDiagnostics& Diagnostics);
	static FString BuildAuthDiagnosticsToolTip(const FCodexAuthDiagnostics& Diagnostics);
	static FString ClassifyAuthFailureMessage(const FString& Message);
	static bool OpenEffectiveCodexAuthFolder(FString& OutStatusMessage);
	static bool LaunchCodexRelogin(FString& OutStatusMessage);
	static bool BackupAndClearStaleAuthArtifacts(FString& OutStatusMessage);
	static bool ProbeBackendAuth(FString& OutStatusMessage);
	static FString ClassifyPersistentTransportFailureMessage(const FString& Message);
	static bool IsPersistentTransportFailureMessage(const FString& Message);
	static bool CanLaunchBrowserVerify(FString& OutReason);
	static bool LaunchBrowserVerifyLogin(FString& OutStatusMessage);
#if WITH_DEV_AUTOMATION_TESTS
	static void SetTestKnownMachineStandardHomes(const TArray<FString>& InHomes);
	static void ClearTestKnownMachineStandardHomes();
	static void SetTestCodexLaunchOverride(const FString& InExecutablePath, const FString& InCodexJsPath = FString());
	static void ClearTestCodexLaunchOverride();
	static FString GetTestPersistentThreadSandboxMode(const FAgentRequestConfig& Config);
	static TSharedPtr<FJsonObject> MakeTestPersistentTurnSandboxPolicy(const FAgentRequestConfig& Config);
	static bool DoesTestConfigRequestUnrealMcpBridge(const FAgentRequestConfig& Config);
	static FString GetTestRequestedUnrealMcpToolFilterCsv(const FAgentRequestConfig& Config);
	static bool ShouldTestForceWindowsUnelevatedWorkspaceWriteSandbox(const FAgentRequestConfig& Config);
	static int32 GetTestPersistentRequestTimeoutMs(const FAgentRequestConfig& Config);
	void ClearTestInMemoryPersistentThreadId();
#endif
	bool ExportActiveThreadStateForRestartSurvival(FString& OutStatePath, FString& OutThreadId);
	FString GetActivePersistentThreadId() const;

	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;
	virtual void ResetConversation() override;

private:
	struct FExecutionResult
	{
		FString ResponseText;
		FString DiagnosticText;
		bool bSuccess = false;
	};

	struct FPersistentAppServerSession
	{
		FProcHandle ProcessHandle;
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		void* StdInReadPipe = nullptr;
		void* StdInWritePipe = nullptr;
		FString ReadBuffer;
		int32 NextRequestId = 1;
		bool bInitialized = false;
		FString ThreadId;
		FString ActiveTurnId;
		FString HomePath;
		FString ProfileLabel;
		FString ModelName;
		FString RequestedSpeedModeName;
		FString EffectiveSpeedModeName;
		FString WorkModeName;
		FString ReasoningEffortName;
		FString VerbosityName;
		FString ExecutionProfileName;
		FString UnrealMcpLaunchSignature;
		bool bPersistThreadState = false;
		bool bAwaitingBootstrapTurn = false;
	};

	bool CreateProcessPipes();
	bool LaunchProcess(const FString& FullCommand, const FString& WorkingDir);
	FString ReadProcessOutput();
	FExecutionResult ExecuteProcessBlocking();
	FExecutionResult ExecutePersistentTurnBlocking();
	void ExecuteProcessAsync();
	void CleanupHandles();
	void CleanupPersistentAppServer(bool bTerminateProcess);
	void ParseAndEmitJsonLine(const FString& JsonLine);
	void EmitTextEvent(const FString& Text);
	void EmitToolUseEvent(const FString& ToolId, const FString& ToolName, const FString& ToolInput, const FString& RawJson);
	void EmitToolResultEvent(const FString& ToolId, const FString& ToolName, const FString& ToolResult, int32 ExitCode, const FString& RawJson);
	void EmitResultEvent(int32 NumTurns, const FString& RawJson, int32 DurationMs = 0);
	void ReportError(const FString& ErrorMessage);
	void ReportCompletion(const FString& Output, bool bSuccess);
	FString BuildCommandLine(const FAgentRequestConfig& Config);
	FString BuildPromptPayload(const FAgentRequestConfig& Config) const;
	FString BuildPersistentTurnPayload(const FAgentRequestConfig& Config, bool bIncludeSystemPrompt, bool bIncludeBootstrapContext) const;
	bool EnsurePersistentAppServerReady(FString& OutDiagnostic);
	bool EnsurePersistentThreadReady(const FAgentRequestConfig& Config, FString& OutDiagnostic);
	bool StartOrResumePersistentThread(const FAgentRequestConfig& Config, bool bPreferResume, FString& OutDiagnostic);
	bool SendPersistentAppServerRequest(const FString& Method, const TSharedRef<FJsonObject>& Params, int32 RequestId, FString& OutDiagnostic);
	bool SendPersistentAppServerNotification(const FString& Method, const TSharedRef<FJsonObject>& Params, FString& OutDiagnostic);
	bool SendPersistentAppServerMessage(const TSharedRef<FJsonObject>& Message, FString& OutDiagnostic);
	bool ReadNextPersistentAppServerMessage(int32 TimeoutMs, TSharedPtr<FJsonObject>& OutMessage, FString& OutDiagnostic);
	bool WaitForPersistentAppServerResult(int32 RequestId, int32 TimeoutMs, TSharedPtr<FJsonObject>& OutResult, FString& OutDiagnostic);
	bool HandlePersistentAppServerNotification(const TSharedPtr<FJsonObject>& Message, bool& bOutTurnFinished, bool& bOutTurnSucceeded, FString& OutFinalTurnRawJson, FString& OutDiagnostic);
	int32 AllocatePersistentRequestId();
	static int32 GetPersistentRequestTimeoutMs(const FAgentRequestConfig& Config);
	FString GetPersistentThreadStatePath() const;
	void WritePersistentThreadStateSnapshot(const FString& StatePath) const;
	bool TryLoadPersistedThreadState(FString& OutThreadId) const;
	void PersistCurrentThreadState() const;
	void ClearPersistedThreadState() const;
	void ResetPersistentConversationState();
	void PoisonPersistentTransport(bool bTerminateProcess, bool bClearPersistedThreadState);
	void RecoverFromPersistentTransportFailure(FString& InOutDiagnostic, bool bTerminateProcess, bool bClearPersistedThreadState);

	FAgentRequestConfig CurrentConfig;
	FOnAgentResponse OnCompleteDelegate;
	FOnAgentProgress OnProgressDelegate;

	FRunnableThread* Thread;
	FThreadSafeCounter StopTaskCounter;
	TAtomic<bool> bIsExecuting;
	FProcHandle ProcessHandle;
	void* ReadPipe;
	void* WritePipe;
	void* StdInReadPipe;
	void* StdInWritePipe;

	FString JsonLineBuffer;
	FString AccumulatedResponseText;
	FString DiagnosticOutput;
	FPersistentAppServerSession PersistentSession;

	static FString CachedCodexJsPath;
};
