// Copyright Natali Caggiano. All Rights Reserved.

#include "ScriptExecutionManager.h"
#include "ScriptPermissionDialog.h"
#include "OsvayderUEModule.h"
#include "OsvayderUEScopePolicy.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEUtils.h"
#include "JsonUtils.h"
#include "MCP/MCPParamValidator.h"

#include "Internationalization/Regex.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

// Live Coding support
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

FScriptExecutionManager& FScriptExecutionManager::Get()
{
	static FScriptExecutionManager Instance;
	return Instance;
}

FScriptExecutionManager::FScriptExecutionManager()
	: MaxHistorySize(100)
	, ScriptCounter(0)
{
	LoadHistory();

	// Ensure script directories exist on startup (only if scope allows)
	FString ContentScriptDir = GetContentScriptDirectory();
	auto ScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(ContentScriptDir);
	if (ScopeCheck.bAllowed)
	{
		if (!IFileManager::Get().DirectoryExists(*ContentScriptDir))
		{
			IFileManager::Get().MakeDirectory(*ContentScriptDir, true);
			UE_LOG(LogOsvayderUE, Log, TEXT("Created script directory: %s"), *ContentScriptDir);
		}
	}
	else
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("Skipping Content script dir creation (PluginOnly scope): %s"), *ContentScriptDir);
	}
}

FScriptExecutionManager::~FScriptExecutionManager()
{
}

FScriptExecutionResult FScriptExecutionManager::ExecuteScript(
	EScriptType Type,
	const FString& ScriptContent,
	const FString& Description)
{
	// Parse description from header if not provided
	FString FinalDescription = Description;
	if (FinalDescription.IsEmpty())
	{
		FinalDescription = ScriptHeader::ParseDescription(ScriptContent);
	}

	// Show permission dialog
	if (!ShowPermissionDialog(ScriptContent, Type, FinalDescription))
	{
		return FScriptExecutionResult::Error(TEXT("Script execution denied by user"));
	}

	// Execute based on type
	FScriptExecutionResult Result;
	switch (Type)
	{
		case EScriptType::Cpp:
			Result = ExecuteCpp(ScriptContent, FinalDescription);
			break;
		case EScriptType::Python:
			Result = ExecutePython(ScriptContent, FinalDescription);
			break;
		case EScriptType::Console:
			Result = ExecuteConsole(ScriptContent, FinalDescription);
			break;
		case EScriptType::EditorUtility:
			Result = ExecuteEditorUtility(ScriptContent, FinalDescription);
			break;
		default:
			Result = FScriptExecutionResult::Error(TEXT("Unknown script type"));
	}

	return Result;
}

bool FScriptExecutionManager::ShowPermissionDialog(
	const FString& ScriptPreview,
	EScriptType Type,
	const FString& Description)
{
	// Delegate to the extracted permission dialog class
	return FScriptPermissionDialog::Show(ScriptPreview, Type, Description);
}

FScriptExecutionResult FScriptExecutionManager::ExecuteCpp(
	const FString& ScriptContent,
	const FString& Description)
{
#if WITH_LIVE_CODING
	double PhaseStart = FPlatformTime::Seconds();
	double WriteStart, WriteEnd, CompileStart, CompileEnd;

	// Generate script name and write to file
	FString ScriptName = GenerateScriptName(EScriptType::Cpp, Description);

	WriteStart = FPlatformTime::Seconds();
	FString FilePath = WriteScriptFile(ScriptContent, EScriptType::Cpp, ScriptName);
	WriteEnd = FPlatformTime::Seconds();

	if (FilePath.IsEmpty())
	{
		// Check if this was a scope denial
		FString TargetPath = FPaths::Combine(GetCppScriptDirectory(), ScriptName + TEXT(".cpp"));
		auto ScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(TargetPath);
		if (!ScopeCheck.bAllowed)
		{
			// Log denial trace with timing
			FExecutionReceipt DenialTrace;
			DenialTrace.Tool = TEXT("execute_script");
			DenialTrace.TraceId = FExecutionReceipt::GenerateTraceId();
			DenialTrace.bSuccess = false;
			DenialTrace.Status = TEXT("denied");
			DenialTrace.Classification = TEXT("denied");
			DenialTrace.TargetType = TEXT("file");
			DenialTrace.Targets.Add(TargetPath);
			DenialTrace.DurationMs = (FPlatformTime::Seconds() - PhaseStart) * 1000.0;
			DenialTrace.ErrorOrDenialReason = ScopeCheck.DenialReason;
			DenialTrace.Summary = FString::Printf(TEXT("Scope denied: %s"), *ScopeCheck.DenialReason);
			DenialTrace.AddPhaseTiming(TEXT("write_file"), (WriteEnd - WriteStart) * 1000.0, (WriteStart - PhaseStart) * 1000.0);
			FOsvayderUEExecutionLog::Get().AddReceipt(DenialTrace);

			return FScriptExecutionResult::Error(FString::Printf(
				TEXT("Scope denied: %s\nAllowed roots: Plugins/OsvayderUE, Docs/OsvayderUE, AgentBridge"),
				*ScopeCheck.DenialReason));
		}
		return FScriptExecutionResult::Error(TEXT("Failed to write C++ script file"));
	}

	UE_LOG(LogOsvayderUE, Log, TEXT("C++ script written to: %s"), *FilePath);

	// Trigger Live Coding compilation
	FString ErrorLog;
	TSharedPtr<FJsonObject> CompileDiagnostics;
	FScriptExecutionResult Result;

	CompileStart = FPlatformTime::Seconds();
	bool bCompileSuccess = TriggerLiveCodingCompile(ErrorLog, CompileDiagnostics);
	CompileEnd = FPlatformTime::Seconds();

	// Log compile trace with phase timings
	{
		FExecutionReceipt CompileTrace;
		CompileTrace.Tool = TEXT("execute_script");
		CompileTrace.TraceId = FExecutionReceipt::GenerateTraceId();
		CompileTrace.bSuccess = bCompileSuccess;
		CompileTrace.bHitCompile = true;
		CompileTrace.Status = bCompileSuccess ? TEXT("success") : TEXT("failed");
		CompileTrace.TargetType = TEXT("file");
		CompileTrace.Targets.Add(FilePath);
		CompileTrace.Classification = TEXT("user_mutation");
		CompileTrace.DurationMs = (CompileEnd - PhaseStart) * 1000.0;
		CompileTrace.Summary = bCompileSuccess
			? FString::Printf(TEXT("C++ compile success: %s"), *FilePath)
			: FString::Printf(TEXT("C++ compile failed: %s"), *FilePath);
		CompileTrace.AddPhaseTiming(TEXT("write_file"), (WriteEnd - WriteStart) * 1000.0, (WriteStart - PhaseStart) * 1000.0);
		CompileTrace.AddPhaseTiming(TEXT("compile"), (CompileEnd - CompileStart) * 1000.0, (CompileStart - PhaseStart) * 1000.0);
		if (!bCompileSuccess) CompileTrace.ErrorOrDenialReason = ErrorLog;
		FOsvayderUEExecutionLog::Get().AddReceipt(CompileTrace);
	}

	if (bCompileSuccess)
	{
		// Compilation succeeded
		Result = FScriptExecutionResult::Success(
			TEXT("C++ script compiled successfully via Live Coding"),
			TEXT("Script file: ") + FilePath
		);
		Result.ScriptFilePath = FilePath;
		Result.Diagnostics = CompileDiagnostics; // may contain warnings

		// Add to history
		FScriptHistoryEntry Entry;
		Entry.ScriptType = EScriptType::Cpp;
		Entry.Filename = ScriptName + TEXT(".cpp");
		Entry.Description = Description;
		Entry.bSuccess = true;
		Entry.ResultMessage = Result.Message;
		Entry.FilePath = FilePath;
		AddToHistory(Entry);

		return Result;
	}

	// Compilation failed - return structured diagnostics for Claude to fix and retry
	UE_LOG(LogOsvayderUE, Warning, TEXT("C++ compilation failed, returning structured diagnostics for repair"));

	Result = FScriptExecutionResult::Error(
		TEXT("Compilation failed. Fix these errors and call execute_script again:\n\n") + ErrorLog,
		ErrorLog
	);
	Result.ScriptFilePath = FilePath;
	Result.Diagnostics = CompileDiagnostics; // structured errors with file/line/severity

	// Add to history as failure
	FScriptHistoryEntry Entry;
	Entry.ScriptType = EScriptType::Cpp;
	Entry.Filename = ScriptName + TEXT(".cpp");
	Entry.Description = Description;
	Entry.bSuccess = false;
	Entry.ResultMessage = TEXT("Compilation failed: ") + ErrorLog.Left(200);
	Entry.FilePath = FilePath;
	AddToHistory(Entry);

	return Result;
#else
	return FScriptExecutionResult::Error(TEXT("Live Coding not available - C++ scripts cannot be compiled"));
#endif
}

/**
 * Custom output device to capture Live Coding compilation output
 * Monitors for error patterns in compiler output
 */
class FLiveCodingOutputCapture : public FOutputDevice
{
public:
	TArray<FString> ErrorMessages;
	TArray<FString> WarningMessages;
	bool bHasErrors = false;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		FString Message(V);

		// Check for Live Coding specific messages
		if (Category == FName("LiveCoding") || Category == FName("LogLiveCoding"))
		{
			if (Verbosity == ELogVerbosity::Error || Verbosity == ELogVerbosity::Fatal)
			{
				ErrorMessages.Add(Message);
				bHasErrors = true;
			}
			else if (Verbosity == ELogVerbosity::Warning)
			{
				WarningMessages.Add(Message);
			}
		}

		// Also check for compiler error patterns in any category
		if (Message.Contains(TEXT("error C")) ||      // MSVC error
		    Message.Contains(TEXT("error:")) ||        // Generic compiler error
		    Message.Contains(TEXT("fatal error")) ||   // Fatal errors
		    Message.Contains(TEXT("LNK2001")) ||       // Linker errors
		    Message.Contains(TEXT("LNK2019")))
		{
			if (!ErrorMessages.Contains(Message))
			{
				ErrorMessages.Add(Message);
				bHasErrors = true;
			}
		}
	}

	FString GetErrorSummary() const
	{
		if (ErrorMessages.Num() == 0)
		{
			return FString();
		}

		// Return first few errors (avoid overwhelming output)
		FString Summary;
		int32 MaxErrors = FMath::Min(5, ErrorMessages.Num());
		for (int32 i = 0; i < MaxErrors; i++)
		{
			Summary += ErrorMessages[i] + TEXT("\n");
		}
		if (ErrorMessages.Num() > MaxErrors)
		{
			Summary += FString::Printf(TEXT("... and %d more errors"), ErrorMessages.Num() - MaxErrors);
		}
		return Summary;
	}

	/** Get structured diagnostics with file/line info parsed from compiler output */
	TSharedPtr<FJsonObject> GetStructuredDiagnostics() const
	{
		TSharedPtr<FJsonObject> Diag = MakeShared<FJsonObject>();
		Diag->SetNumberField(TEXT("error_count"), ErrorMessages.Num());
		Diag->SetNumberField(TEXT("warning_count"), WarningMessages.Num());

		TArray<TSharedPtr<FJsonValue>> ErrorsArr;
		for (const FString& Msg : ErrorMessages)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("message"), Msg);

			// Try to parse file(line): pattern — MSVC format
			FRegexPattern Pattern(TEXT("(.+?)\\((\\d+)(?:,(\\d+))?\\)\\s*:\\s*(error|fatal error|warning)\\s+(.+)"));
			FRegexMatcher Matcher(Pattern, Msg);
			if (Matcher.FindNext())
			{
				ErrObj->SetStringField(TEXT("file"), Matcher.GetCaptureGroup(1));
				ErrObj->SetStringField(TEXT("line"), Matcher.GetCaptureGroup(2));
				if (!Matcher.GetCaptureGroup(3).IsEmpty())
					ErrObj->SetStringField(TEXT("column"), Matcher.GetCaptureGroup(3));
				ErrObj->SetStringField(TEXT("severity"), Matcher.GetCaptureGroup(4));
				ErrObj->SetStringField(TEXT("code_and_detail"), Matcher.GetCaptureGroup(5));
			}
			else
			{
				// Linker error or other format
				ErrObj->SetStringField(TEXT("severity"), Msg.Contains(TEXT("LNK")) ? TEXT("linker_error") : TEXT("error"));
			}

			ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrObj));
		}
		Diag->SetArrayField(TEXT("errors"), ErrorsArr);

		TArray<TSharedPtr<FJsonValue>> WarnsArr;
		for (const FString& Msg : WarningMessages)
		{
			TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
			WarnObj->SetStringField(TEXT("message"), Msg);
			WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
			WarnsArr.Add(MakeShared<FJsonValueObject>(WarnObj));
		}
		Diag->SetArrayField(TEXT("warnings"), WarnsArr);

		return Diag;
	}
};

bool FScriptExecutionManager::TriggerLiveCodingCompile(FString& OutErrorLog, TSharedPtr<FJsonObject>& OutDiagnostics)
{
#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>("LiveCoding");
	if (!LiveCoding)
	{
		OutErrorLog = TEXT("Live Coding module not loaded");
		return false;
	}

	if (!LiveCoding->IsEnabledForSession())
	{
		OutErrorLog = TEXT("Live Coding is not enabled. Press Ctrl+Alt+F11 to enable.");
		return false;
	}

	// Snapshot UBT log file before compile for fallback parsing
	FString UBTLogPath = FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("UnrealBuildTool"), TEXT("Log.txt"));
	int64 UBTLogSizeBefore = 0;
	if (IFileManager::Get().FileExists(*UBTLogPath))
	{
		UBTLogSizeBefore = IFileManager::Get().FileSize(*UBTLogPath);
	}

	// Set up output capture to monitor for compilation errors
	FLiveCodingOutputCapture OutputCapture;
	GLog->AddOutputDevice(&OutputCapture);

	// Trigger compilation — use WaitForCompletion to get explicit result
	ELiveCodingCompileResult CompileResult = ELiveCodingCompileResult::NotStarted;
	bool bCompileStarted = LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &CompileResult);

	// If Compile() returned false (e.g., LC already auto-triggered from file write),
	// wait for the active compile to finish and check UBT log
	if (!bCompileStarted || CompileResult == ELiveCodingCompileResult::CompileStillActive || CompileResult == ELiveCodingCompileResult::NotStarted)
	{
		// Wait for any active LC compile to finish
		float WaitTime = 0.0f;
		const float MaxWait = 120.0f;
		const float PollInterval = 0.5f;

		// First wait for compile to start (LC may need a moment)
		FPlatformProcess::Sleep(2.0f);
		WaitTime += 2.0f;

		while (LiveCoding->IsCompiling() && WaitTime < MaxWait)
		{
			FPlatformProcess::Sleep(PollInterval);
			WaitTime += PollInterval;
		}
	}

	// Brief wait for UBT log to be flushed
	FPlatformProcess::Sleep(1.0f);

	// Remove output capture
	GLog->RemoveOutputDevice(&OutputCapture);

	// Always build structured diagnostics from GLog capture
	OutDiagnostics = OutputCapture.GetStructuredDiagnostics();

	// Check compile result from LC enum
	if (CompileResult == ELiveCodingCompileResult::Cancelled)
	{
		OutErrorLog = TEXT("Live Coding compile was cancelled");
		return false;
	}

	// Success cases from explicit Compile() result
	if (CompileResult == ELiveCodingCompileResult::Success || CompileResult == ELiveCodingCompileResult::NoChanges)
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("Live Coding compilation completed successfully (result: %d)"), (int32)CompileResult);
		return true;
	}

	// GLog may have caught a generic "Live coding failed" message but not detailed errors.
	// Always check UBT log for richer diagnostics regardless of GLog state.
	bool bGLogHasErrors = OutputCapture.bHasErrors;
	FString GLogErrorSummary = bGLogHasErrors ? OutputCapture.GetErrorSummary() : FString();

	// ===== Parse UBT log for detailed compile errors =====
	if (IFileManager::Get().FileExists(*UBTLogPath))
	{
		int64 UBTLogSizeAfter = IFileManager::Get().FileSize(*UBTLogPath);
		if (UBTLogSizeAfter > UBTLogSizeBefore)
		{
			// Read full file as string (UBT writes in system locale encoding)
			FString FullContent;
			if (FFileHelper::LoadFileToString(FullContent, *UBTLogPath))
			{
				// Find the last compile section by searching for the last "Running UnrealBuildTool" or compile trace
				// Use a generous tail to capture all of the last compile output
				int32 TailChars = FMath::Min(FullContent.Len(), 8000);
				FString NewContent = FullContent.Right(TailChars);
				{

					// Check UBT outcome
					bool bUBTFailed = NewContent.Contains(TEXT("Result: Failed"));
					bool bUBTSucceeded = NewContent.Contains(TEXT("Result: Succeeded"));

					// If UBT reports success and Compile() didn't give us a result, treat as success
					if (bUBTSucceeded && !bUBTFailed)
					{
						UE_LOG(LogOsvayderUE, Log, TEXT("Live Coding compilation succeeded (detected via UBT log)"));
						return true;
					}

					// Parse MSVC error patterns from new UBT log content
					TArray<FString> FallbackErrors;
					TArray<FString> FallbackWarnings;
					TArray<FString> Lines;
					NewContent.ParseIntoArrayLines(Lines);

					for (const FString& Line : Lines)
					{
						// Match lines containing ": error " or ": fatal error " (MSVC compile errors)
						if (Line.Contains(TEXT(": error ")) || Line.Contains(TEXT(": fatal error ")))
						{
							FallbackErrors.Add(Line.TrimStartAndEnd());
						}
						else if (Line.Contains(TEXT(": warning ")) && !Line.Contains(TEXT("is not a preferred version")))
						{
							FallbackWarnings.Add(Line.TrimStartAndEnd());
						}
					}

					if (bUBTFailed || FallbackErrors.Num() > 0)
					{
						// Build diagnostics from fallback
						TSharedPtr<FJsonObject> FallbackDiag = MakeShared<FJsonObject>();
						FallbackDiag->SetNumberField(TEXT("error_count"), FallbackErrors.Num());
						FallbackDiag->SetNumberField(TEXT("warning_count"), FallbackWarnings.Num());
						FallbackDiag->SetStringField(TEXT("diagnostics_source"), TEXT("ubt_log_fallback"));

						TArray<TSharedPtr<FJsonValue>> ErrorsArr;
						for (const FString& ErrLine : FallbackErrors)
						{
							TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
							ErrObj->SetStringField(TEXT("message"), ErrLine);

							// Try to extract file(line,col) from MSVC format via string search
							int32 ParenOpen = ErrLine.Find(TEXT("("));
							int32 ParenClose = ErrLine.Find(TEXT(")"), ESearchCase::IgnoreCase, ESearchDir::FromStart, ParenOpen);
							if (ParenOpen != INDEX_NONE && ParenClose != INDEX_NONE && ParenOpen > 0)
							{
								ErrObj->SetStringField(TEXT("file"), ErrLine.Left(ParenOpen).TrimStartAndEnd());
								FString LineCol = ErrLine.Mid(ParenOpen + 1, ParenClose - ParenOpen - 1);
								FString LineNum, ColNum;
								if (LineCol.Split(TEXT(","), &LineNum, &ColNum))
								{
									ErrObj->SetStringField(TEXT("line"), LineNum.TrimStartAndEnd());
									ErrObj->SetStringField(TEXT("column"), ColNum.TrimStartAndEnd());
								}
								else
								{
									ErrObj->SetStringField(TEXT("line"), LineCol.TrimStartAndEnd());
								}

								// Extract error code after ": error "
								int32 ErrorIdx = ErrLine.Find(TEXT(": error "));
								if (ErrorIdx == INDEX_NONE) ErrorIdx = ErrLine.Find(TEXT(": fatal error "));
								if (ErrorIdx != INDEX_NONE)
								{
									ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
									FString Detail = ErrLine.Mid(ErrorIdx + 2).TrimStartAndEnd();
									ErrObj->SetStringField(TEXT("code_and_detail"), Detail);
								}
								else
								{
									ErrObj->SetStringField(TEXT("severity"), TEXT("error"));
								}
							}
							else
							{
								ErrObj->SetStringField(TEXT("severity"), ErrLine.Contains(TEXT("LNK")) ? TEXT("linker_error") : TEXT("error"));
							}
							ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrObj));
						}
						FallbackDiag->SetArrayField(TEXT("errors"), ErrorsArr);

						TArray<TSharedPtr<FJsonValue>> WarnsArr;
						for (const FString& WarnLine : FallbackWarnings)
						{
							TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
							WarnObj->SetStringField(TEXT("message"), WarnLine);
							WarnObj->SetStringField(TEXT("severity"), TEXT("warning"));
							WarnsArr.Add(MakeShared<FJsonValueObject>(WarnObj));
						}
						FallbackDiag->SetArrayField(TEXT("warnings"), WarnsArr);

						OutDiagnostics = FallbackDiag;

						// Build error summary
						FString FallbackSummary;
						int32 MaxShow = FMath::Min(5, FallbackErrors.Num());
						for (int32 i = 0; i < MaxShow; i++)
							FallbackSummary += FallbackErrors[i] + TEXT("\n");
						if (FallbackErrors.Num() > MaxShow)
							FallbackSummary += FString::Printf(TEXT("... and %d more errors"), FallbackErrors.Num() - MaxShow);

						OutErrorLog = FallbackSummary.IsEmpty()
							? TEXT("Live Coding compile failed (UBT reported failure, no parseable errors)")
							: FallbackSummary;

						// Emit receipt
						FExecutionReceipt CompileReceipt;
						CompileReceipt.Tool = TEXT("cpp_compile");
						CompileReceipt.bSuccess = false;
						CompileReceipt.TargetType = TEXT("script");
						CompileReceipt.Classification = TEXT("user_mutation");
						CompileReceipt.Status = TEXT("failed");
						CompileReceipt.ErrorOrDenialReason = OutErrorLog;
						CompileReceipt.Summary = FString::Printf(TEXT("C++ compile failed (UBT log fallback): %d errors, %d warnings"), FallbackErrors.Num(), FallbackWarnings.Num());
						FOsvayderUEExecutionLog::Get().AddReceipt(CompileReceipt);

						UE_LOG(LogOsvayderUE, Warning, TEXT("Live Coding compile failed (detected via UBT log fallback):\n%s"), *OutErrorLog);
						return false;
					}
				}
			}
		}
	}

	// If we get here, UBT log had no parseable errors. Fall back to GLog summary if available.
	if (bGLogHasErrors)
	{
		OutErrorLog = GLogErrorSummary;

		FExecutionReceipt CompileReceipt;
		CompileReceipt.Tool = TEXT("cpp_compile");
		CompileReceipt.bSuccess = false;
		CompileReceipt.TargetType = TEXT("script");
		CompileReceipt.Classification = TEXT("user_mutation");
		CompileReceipt.Status = TEXT("failed");
		CompileReceipt.ErrorOrDenialReason = OutErrorLog;
		CompileReceipt.Summary = FString::Printf(TEXT("C++ compile failed (GLog): %d errors"), OutputCapture.ErrorMessages.Num());
		FOsvayderUEExecutionLog::Get().AddReceipt(CompileReceipt);

		UE_LOG(LogOsvayderUE, Error, TEXT("Live Coding compilation failed (GLog):\n%s"), *OutErrorLog);
		return false;
	}

	UE_LOG(LogOsvayderUE, Warning, TEXT("Live Coding compile result: %d, no errors captured"), (int32)CompileResult);
	OutErrorLog = FString::Printf(TEXT("Live Coding compile failed (result: %d, no parseable errors from GLog or UBT log)"), (int32)CompileResult);
	return false;
#else
	OutErrorLog = TEXT("Live Coding not available in this build");
	return false;
#endif
}

FScriptExecutionResult FScriptExecutionManager::ExecutePython(
	const FString& ScriptContent,
	const FString& Description)
{
	// Check if editor is available
	if (!GEditor)
	{
		return FScriptExecutionResult::Error(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return FScriptExecutionResult::Error(TEXT("No active world"));
	}

	// Write script to file
	FString ScriptName = GenerateScriptName(EScriptType::Python, Description);
	FString FilePath = WriteScriptFile(ScriptContent, EScriptType::Python, ScriptName);

	if (FilePath.IsEmpty())
	{
		FString TargetPath = FPaths::Combine(GetContentScriptDirectory(), ScriptName + TEXT(".py"));
		auto ScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(TargetPath);
		if (!ScopeCheck.bAllowed)
		{
			return FScriptExecutionResult::Error(FString::Printf(
				TEXT("Scope denied: %s\nAllowed roots: Plugins/OsvayderUE, Docs/OsvayderUE, AgentBridge"),
				*ScopeCheck.DenialReason));
		}
		return FScriptExecutionResult::Error(TEXT("Failed to write Python script file"));
	}

	// Count actors before execution for validation
	int32 ActorCountBefore = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		ActorCountBefore++;
	}

	// Execute via console command
	FString Command = FString::Printf(TEXT("py \"%s\""), *FilePath);

	// Capture both exec output and global log output (Python errors go to GLog, not exec output)
	FOsvayderUEOutputDevice ExecOutput;
	FOsvayderUEOutputDevice LogOutput;
	GLog->AddOutputDevice(&LogOutput);

	GEditor->Exec(World, *Command, ExecOutput);

	GLog->RemoveOutputDevice(&LogOutput);

	// Combine both output sources
	FString ExecText = ExecOutput.GetTrimmedOutput();
	FString LogText = LogOutput.GetTrimmedOutput();
	FString Output = ExecText;
	if (!LogText.IsEmpty())
	{
		if (!Output.IsEmpty())
		{
			Output += TEXT("\n");
		}
		Output += LogText;
	}

	// Count actors after execution
	int32 ActorCountAfter = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		ActorCountAfter++;
	}
	int32 ActorsCreated = ActorCountAfter - ActorCountBefore;

	UE_LOG(LogOsvayderUE, Log, TEXT("Python script output (%d chars): %s"),
		Output.Len(), Output.Len() > 500 ? *(Output.Left(500) + TEXT("...")) : *Output);
	UE_LOG(LogOsvayderUE, Log, TEXT("Python script actor delta: %d before, %d after (%+d)"),
		ActorCountBefore, ActorCountAfter, ActorsCreated);

	// Detect Python errors in output (check both exec and log output)
	bool bHasError = Output.Contains(TEXT("Traceback")) ||
	                 Output.Contains(TEXT("Error:")) ||
	                 Output.Contains(TEXT("SyntaxError")) ||
	                 Output.Contains(TEXT("NameError")) ||
	                 Output.Contains(TEXT("TypeError")) ||
	                 Output.Contains(TEXT("ValueError")) ||
	                 Output.Contains(TEXT("ImportError")) ||
	                 Output.Contains(TEXT("AttributeError")) ||
	                 Output.Contains(TEXT("RuntimeError")) ||
	                 Output.Contains(TEXT("Exception:")) ||
	                 Output.Contains(TEXT("ModuleNotFoundError")) ||
	                 Output.Contains(TEXT("FileNotFoundError")) ||
	                 Output.Contains(TEXT("IndentationError")) ||
	                 Output.Contains(TEXT("KeyError"));

	// Build result message with actor count info
	FString ResultMessage;
	if (bHasError)
	{
		ResultMessage = TEXT("Python script execution failed");
	}
	else
	{
		ResultMessage = FString::Printf(TEXT("Python script executed (actors created: %d)"), ActorsCreated);
	}

	// Append output to result so Claude can see what happened
	FString FullOutput = Output;
	if (ActorsCreated > 0)
	{
		FullOutput += FString::Printf(TEXT("\n[%d new actors added to level]"), ActorsCreated);
	}
	else if (!bHasError)
	{
		FullOutput += TEXT("\n[WARNING: Script reported success but no new actors were created in the level. The script may have failed silently.]");
	}

	// Add to history
	FScriptHistoryEntry Entry;
	Entry.ScriptType = EScriptType::Python;
	Entry.Filename = ScriptName + TEXT(".py");
	Entry.Description = Description;
	Entry.bSuccess = !bHasError;
	Entry.ResultMessage = ResultMessage.Left(200);
	Entry.FilePath = FilePath;
	AddToHistory(Entry);

	if (bHasError)
	{
		return FScriptExecutionResult::Error(ResultMessage, FullOutput);
	}

	return FScriptExecutionResult::Success(ResultMessage, FullOutput);
}

FScriptExecutionResult FScriptExecutionManager::ExecuteConsole(
	const FString& ScriptContent,
	const FString& Description)
{
	if (!GEditor)
	{
		return FScriptExecutionResult::Error(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return FScriptExecutionResult::Error(TEXT("No active world"));
	}

	// Parse commands (one per line)
	TArray<FString> Commands;
	ScriptContent.ParseIntoArrayLines(Commands, true);

	// Output capture using shared utility
	FOsvayderUEOutputDevice OutputDevice;
	FString AllOutput;
	int32 ExecutedCount = 0;

	for (const FString& RawCommand : Commands)
	{
		FString Command = RawCommand.TrimStartAndEnd();

		// Skip empty lines and comments
		if (Command.IsEmpty() || Command.StartsWith(TEXT("#")) || Command.StartsWith(TEXT("//")))
		{
			continue;
		}

		// Skip header metadata lines
		if (Command.Contains(TEXT("@OsvayderUE")) || Command.Contains(TEXT("@Name:")) ||
			Command.Contains(TEXT("@Description:")) || Command.Contains(TEXT("@Created:")))
		{
			continue;
		}

		// Validate command
		FString ValidationError;
		if (!FMCPParamValidator::ValidateConsoleCommand(Command, ValidationError))
		{
			AllOutput += FString::Printf(TEXT("Skipped blocked command: %s\n"), *Command);
			continue;
		}

		OutputDevice.Clear();
		GEditor->Exec(World, *Command, OutputDevice);
		AllOutput += FString::Printf(TEXT("> %s\n%s\n"), *Command, *OutputDevice.GetTrimmedOutput());
		ExecutedCount++;
	}

	// Add to history
	FScriptHistoryEntry Entry;
	Entry.ScriptType = EScriptType::Console;
	Entry.Filename = FString::Printf(TEXT("console_%d.txt"), ScriptCounter);
	Entry.Description = Description;
	Entry.bSuccess = ExecutedCount > 0;
	Entry.ResultMessage = FString::Printf(TEXT("Executed %d commands"), ExecutedCount);
	AddToHistory(Entry);
	ScriptCounter++;

	return FScriptExecutionResult::Success(
		FString::Printf(TEXT("Executed %d console commands"), ExecutedCount),
		AllOutput
	);
}

FScriptExecutionResult FScriptExecutionManager::ExecuteEditorUtility(
	const FString& ScriptContent,
	const FString& Description)
{
	// Editor Utility execution is more complex - would need to create Blueprint asset
	// For now, return not implemented
	return FScriptExecutionResult::Error(
		TEXT("Editor Utility script execution not yet implemented. Use Python or Console commands instead.")
	);
}

FString FScriptExecutionManager::WriteScriptFile(
	const FString& Content,
	EScriptType Type,
	const FString& ScriptName)
{
	FString Directory;
	FString Extension = GetScriptExtension(Type);

	if (Type == EScriptType::Cpp)
	{
		Directory = GetCppScriptDirectory();
	}
	else
	{
		Directory = GetContentScriptDirectory();
	}

	// Runtime scope enforcement
	FString FilePath = FPaths::Combine(Directory, ScriptName + Extension);
	auto ScopeResult = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(FilePath);
	if (!ScopeResult.bAllowed)
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Scope denied write to '%s': %s"), *FilePath, *ScopeResult.DenialReason);

		// Log denial receipt
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("execute_script");
		Receipt.bSuccess = false;
		Receipt.TargetType = TEXT("file");
		Receipt.Targets.Add(FilePath);
		Receipt.ValidationSummary = ScopeResult.DenialReason;
		Receipt.Classification = TEXT("denied");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);

		return FString(); // Empty = failure
	}

	// Ensure directory exists
	if (!IFileManager::Get().DirectoryExists(*Directory))
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}

	if (FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		// Log success receipt
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("execute_script");
		Receipt.bSuccess = true;
		Receipt.TargetType = TEXT("file");
		Receipt.Targets.Add(FilePath);
		Receipt.Created.Add(FilePath);
		Receipt.Classification = TEXT("user_mutation");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);

		return FilePath;
	}

	UE_LOG(LogOsvayderUE, Error, TEXT("Failed to write script to: %s"), *FilePath);
	return FString();
}

FString FScriptExecutionManager::GenerateScriptName(EScriptType Type, const FString& Description)
{
	// Sanitize description for filename using single-pass character filtering
	FString BaseName;
	BaseName.Reserve(30);

	// Invalid filename characters on Windows
	static const TCHAR* InvalidChars = TEXT(" /\\:*?\"<>|");

	int32 CharCount = 0;
	for (TCHAR C : Description)
	{
		if (CharCount >= 30)
		{
			break;
		}

		// Replace invalid characters with underscore
		bool bIsInvalid = false;
		for (const TCHAR* InvalidChar = InvalidChars; *InvalidChar; ++InvalidChar)
		{
			if (C == *InvalidChar)
			{
				bIsInvalid = true;
				break;
			}
		}

		BaseName.AppendChar(bIsInvalid ? TEXT('_') : C);
		CharCount++;
	}

	if (BaseName.IsEmpty())
	{
		BaseName = TEXT("Script");
	}

	ScriptCounter++;
	return FString::Printf(TEXT("%s_%03d"), *BaseName, ScriptCounter);
}

void FScriptExecutionManager::AddToHistory(const FScriptHistoryEntry& Entry)
{
	History.Add(Entry);

	// Trim if exceeds max
	while (History.Num() > MaxHistorySize)
	{
		History.RemoveAt(0);
	}

	// Auto-save
	SaveHistory();
}

TArray<FScriptHistoryEntry> FScriptExecutionManager::GetRecentScripts(int32 Count) const
{
	TArray<FScriptHistoryEntry> Recent;
	int32 StartIdx = FMath::Max(0, History.Num() - Count);

	for (int32 i = History.Num() - 1; i >= StartIdx; --i)
	{
		Recent.Add(History[i]);
	}

	return Recent;
}

FString FScriptExecutionManager::FormatHistoryForContext(int32 Count) const
{
	TArray<FScriptHistoryEntry> Recent = GetRecentScripts(Count);

	if (Recent.Num() == 0)
	{
		return TEXT("");
	}

	FString Context = TEXT("## Recent Script Executions:\n");

	for (int32 i = 0; i < Recent.Num(); ++i)
	{
		const FScriptHistoryEntry& Entry = Recent[i];
		Context += FString::Printf(
			TEXT("%d. [%s] %s - \"%s\" %s\n"),
			i + 1,
			*ScriptTypeToString(Entry.ScriptType).ToUpper(),
			*Entry.Filename,
			*Entry.Description.Left(50),
			Entry.bSuccess ? TEXT("✓") : TEXT("✗")
		);
	}

	return Context;
}

void FScriptExecutionManager::ClearHistory()
{
	History.Empty();
	SaveHistory();
}

bool FScriptExecutionManager::SaveHistory()
{
	FString HistoryPath = GetHistoryFilePath();
	FString SaveDir = FPaths::GetPath(HistoryPath);

	if (!IFileManager::Get().DirectoryExists(*SaveDir))
	{
		IFileManager::Get().MakeDirectory(*SaveDir, true);
	}

	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ScriptsArray;
	for (const FScriptHistoryEntry& Entry : History)
	{
		ScriptsArray.Add(MakeShared<FJsonValueObject>(Entry.ToJson()));
	}

	RootObject->SetArrayField(TEXT("scripts"), ScriptsArray);
	RootObject->SetStringField(TEXT("last_updated"), FDateTime::UtcNow().ToString(TEXT("%Y-%m-%dT%H:%M:%SZ")));

	FString JsonString = FJsonUtils::Stringify(RootObject, true);

	// Scope check — history is internal runtime state
	auto HistScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(HistoryPath);
	if (!HistScopeCheck.bAllowed)
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("script_history");
		Receipt.bSuccess = false;
		Receipt.TargetType = TEXT("file");
		Receipt.Targets.Add(HistoryPath);
		Receipt.Classification = TEXT("denied");
		Receipt.ValidationSummary = HistScopeCheck.DenialReason;
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
		return false;
	}

	if (FFileHelper::SaveStringToFile(JsonString, *HistoryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("script_history");
		Receipt.bSuccess = true;
		Receipt.TargetType = TEXT("file");
		Receipt.Targets.Add(HistoryPath);
		Receipt.Modified.Add(HistoryPath);
		Receipt.Classification = TEXT("internal_state");
		Receipt.ValidationSummary = TEXT("internal runtime state");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
		UE_LOG(LogOsvayderUE, Log, TEXT("Script history saved: %d entries"), History.Num());
		return true;
	}

	UE_LOG(LogOsvayderUE, Error, TEXT("Failed to save script history to: %s"), *HistoryPath);
	return false;
}

bool FScriptExecutionManager::LoadHistory()
{
	FString HistoryPath = GetHistoryFilePath();

	if (!IFileManager::Get().FileExists(*HistoryPath))
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("No script history file found"));
		return false;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *HistoryPath))
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to load script history from: %s"), *HistoryPath);
		return false;
	}

	TSharedPtr<FJsonObject> RootObject = FJsonUtils::Parse(JsonString);
	if (!RootObject.IsValid())
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to parse script history JSON"));
		return false;
	}

	History.Empty();

	TArray<TSharedPtr<FJsonValue>> ScriptsArray;
	if (FJsonUtils::GetArrayField(RootObject, TEXT("scripts"), ScriptsArray))
	{
		for (const TSharedPtr<FJsonValue>& ScriptValue : ScriptsArray)
		{
			const TSharedPtr<FJsonObject>* ScriptObject = nullptr;
			if (ScriptValue->TryGetObject(ScriptObject) && ScriptObject != nullptr && (*ScriptObject).IsValid())
			{
				History.Add(FScriptHistoryEntry::FromJson(*ScriptObject));
			}
		}
	}

	UE_LOG(LogOsvayderUE, Log, TEXT("Loaded script history: %d entries"), History.Num());
	return true;
}

FString FScriptExecutionManager::CleanupAll()
{
	int32 DeletedFiles = 0;
	TArray<FString> Errors;

	// Delete C++ scripts
	FString CppDir = GetCppScriptDirectory();
	if (IFileManager::Get().DirectoryExists(*CppDir))
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *CppDir, TEXT("*.*"), true, false);

		for (const FString& File : Files)
		{
			if (IFileManager::Get().Delete(*File))
			{
				DeletedFiles++;
			}
		}

		IFileManager::Get().DeleteDirectory(*CppDir, false, true);
	}

	// Delete content scripts
	FString ContentDir = GetContentScriptDirectory();
	if (IFileManager::Get().DirectoryExists(*ContentDir))
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *ContentDir, TEXT("*.*"), true, false);

		for (const FString& File : Files)
		{
			if (IFileManager::Get().Delete(*File))
			{
				DeletedFiles++;
			}
		}

		IFileManager::Get().DeleteDirectory(*ContentDir, false, true);
	}

	// Clear history
	int32 HistoryCount = History.Num();
	ClearHistory();

	return FString::Printf(TEXT("Cleanup complete: Deleted %d files, cleared %d history entries"), DeletedFiles, HistoryCount);
}

FString FScriptExecutionManager::GetHistoryFilePath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("script_history.json"));
}

FString FScriptExecutionManager::GetCppScriptDirectory() const
{
	// Source/[ProjectName]/Generated/OsvayderUE/
	FString ProjectName = FApp::GetProjectName();
	return FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"), ProjectName, TEXT("Generated"), TEXT("OsvayderUE"));
}

FString FScriptExecutionManager::GetContentScriptDirectory() const
{
	// Content/OsvayderUE/Scripts/
	return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("OsvayderUE"), TEXT("Scripts"));
}
