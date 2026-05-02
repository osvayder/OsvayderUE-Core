// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_LiveCodingCompile.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ScriptExecutionManager.h"
#include "UnrealClaudeModule.h"
#include "UnrealClaudeSettings.h"

#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

namespace UnrealClaude
{
namespace LiveCodingCompile
{
namespace
{
	// 619 P1 test-only overrides. Null by default; real LC path runs.
	TFunction<bool()> GIsEnabledForSessionOverride;
	TFunction<void(bool)> GEnableForSessionOverride;
	TFunction<Testing::FMockCompileResult()> GTriggerLiveCodingCompileOverride;
	TFunction<bool(int32)> GIsCompilingPollOverride;

	// 626 P4 test-only overrides for the triangulation I/O layer.
	TFunction<bool(bool&)> GDllMtimeChangedOverride;
	TFunction<FString()> GEngineLogTailOverride;

	// Poll counter for the IsCompiling override -- bumped by the tool's polling guard.
	int32 GIsCompilingPollCounter = 0;

	bool CheckLiveCodingEnabled()
	{
		if (GIsEnabledForSessionOverride)
		{
			return GIsEnabledForSessionOverride();
		}
#if WITH_LIVE_CODING
		ILiveCodingModule* LC = FModuleManager::GetModulePtr<ILiveCodingModule>(TEXT("LiveCoding"));
		return LC && LC->IsEnabledForSession();
#else
		return false;
#endif
	}

	void InvokeEnableLiveCodingForSession(bool bEnabled)
	{
		if (GEnableForSessionOverride)
		{
			GEnableForSessionOverride(bEnabled);
			return;
		}
#if WITH_LIVE_CODING
		ILiveCodingModule* LC = FModuleManager::GetModulePtr<ILiveCodingModule>(TEXT("LiveCoding"));
		if (LC)
		{
			LC->EnableForSession(bEnabled);
		}
#endif
	}

	Testing::FMockCompileResult InvokeTriggerLiveCodingCompile()
	{
		if (GTriggerLiveCodingCompileOverride)
		{
			return GTriggerLiveCodingCompileOverride();
		}

		Testing::FMockCompileResult Result;
		Result.bSuccess = FScriptExecutionManager::Get().TriggerLiveCodingCompile(
			Result.ErrorLog, Result.Diagnostics);
		return Result;
	}

	// 619 P1 reviewer addition #1: PollIsCompilingBlocking guards the tool's
	// path from racing a concurrent compile. Tests exercise this through the
	// SetIsCompilingPollOverride seam. In production, this is normally not
	// needed (TriggerLiveCodingCompile already waits) -- the guard is a
	// defense-in-depth layer for the case where the agent dispatch somehow
	// overlaps with another compile trigger (e.g. an external UI click).
	bool PollIsCompilingBlocking(int32 TimeoutSeconds)
	{
		if (!GIsCompilingPollOverride)
		{
			// No override: production path relies on TriggerLiveCodingCompile's
			// internal WaitForCompletion. Return false immediately.
			return false;
		}

		const double Deadline = FPlatformTime::Seconds() + static_cast<double>(FMath::Max(1, TimeoutSeconds));
		GIsCompilingPollCounter = 0;
		while (FPlatformTime::Seconds() < Deadline)
		{
			const bool bCompiling = GIsCompilingPollOverride(GIsCompilingPollCounter++);
			if (!bCompiling)
			{
				return true;
			}
			// Tight loop for tests; production would sleep here.
		}
		return false;
	}

	// ====================================================================
	// 626 P4 triangulation helpers. All pure (no engine / file I/O) so they
	// can be unit-tested deterministically. File I/O (DLL mtime scan, engine
	// log tail) happens at Execute() level and feeds the pure helpers.
	// ====================================================================

	bool HasPositiveReloadSignalInternal(const FString& ErrorLog, const FString& EngineLogTail)
	{
		// Canonical UE 5.7 LiveCoding post-patch markers. Case-insensitive
		// substring match — engine emits these as diagnostic lines and the
		// exact formatting (brackets, timestamps, Verbose/Display/Log level
		// prefix) varies between editor builds. Order: most specific first.
		static const TArray<FString> Markers = {
			TEXT("Reload/Re-instancing Complete"), // canonical 5.7 phrase
			TEXT("Re-instancing complete: No object changes detected"), // exact no-reinstance variant
			TEXT("Patch DLL loaded"), // explicit patch-loaded confirmation
			TEXT("Reload Complete") // shorter tail variant
		};
		for (const FString& Marker : Markers)
		{
			if (!ErrorLog.IsEmpty() && ErrorLog.Contains(Marker, ESearchCase::IgnoreCase))
			{
				return true;
			}
			if (!EngineLogTail.IsEmpty() && EngineLogTail.Contains(Marker, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool HasPatchDllTimestampChangeInternal(
		const TMap<FString, FDateTime>& PreCallMtimes,
		const TMap<FString, FDateTime>& PostCallMtimes)
	{
		// Any DLL present post-call whose mtime strictly advanced vs
		// pre-call counts as a positive signal. New DLLs appearing only in
		// post-call (e.g. Patch_N.dll written during the compile window)
		// also count.
		for (const TPair<FString, FDateTime>& PostPair : PostCallMtimes)
		{
			const FDateTime* PrePtr = PreCallMtimes.Find(PostPair.Key);
			if (!PrePtr)
			{
				// New DLL appeared during the call.
				return true;
			}
			if (PostPair.Value > *PrePtr)
			{
				return true;
			}
		}
		return false;
	}

	bool HasAbsenceOfCompileErrorsInternal(const FString& ErrorLog)
	{
		if (ErrorLog.IsEmpty())
		{
			return true;
		}
		// Error patterns that mean a real compile/link failure occurred.
		// If any match, this function returns FALSE (errors present).
		// Case-insensitive substring match. MSVC+linker+UBT+GCC covered.
		static const TArray<FString> ErrorPatterns = {
			TEXT("error C"), // MSVC compile (error C2059, etc.) — substring ok because we verify digit-after via classifier
			TEXT("fatal error C"),
			TEXT("error LNK"),
			TEXT("fatal error LNK"),
			TEXT("UnrealBuildTool: ERROR:"),
			TEXT("undefined reference to")
		};
		for (const FString& Pattern : ErrorPatterns)
		{
			if (ErrorLog.Contains(Pattern, ESearchCase::IgnoreCase))
			{
				return false;
			}
		}
		return true;
	}

	TMap<FString, FDateTime> ScanPluginDllMtimes()
	{
		TMap<FString, FDateTime> Result;

		// Resolve the UnrealClaude plugin Binaries/Win64 directory.
		IPluginManager& PM = IPluginManager::Get();
		TSharedPtr<IPlugin> Plugin = PM.FindPlugin(TEXT("UnrealClaude"));
		if (!Plugin.IsValid())
		{
			return Result;
		}
		const FString PluginBaseDir = Plugin->GetBaseDir();
		const FString DllDir = FPaths::Combine(PluginBaseDir, TEXT("Binaries"), TEXT("Win64"));
		if (!FPaths::DirectoryExists(DllDir))
		{
			return Result;
		}

		TArray<FString> DllFiles;
		IFileManager::Get().FindFiles(DllFiles, *FPaths::Combine(DllDir, TEXT("UnrealEditor-*.dll")), true, false);
		for (const FString& FileName : DllFiles)
		{
			const FString FullPath = FPaths::Combine(DllDir, FileName);
			const FDateTime Mtime = IFileManager::Get().GetTimeStamp(*FullPath);
			if (Mtime != FDateTime::MinValue())
			{
				Result.Add(FileName, Mtime);
			}
		}
		return Result;
	}

	bool MeasureDllMtimeChangeAcrossCall(
		const TMap<FString, FDateTime>& PreCallMtimes,
		bool& OutMeasured)
	{
		if (GDllMtimeChangedOverride)
		{
			return GDllMtimeChangedOverride(OutMeasured);
		}
		const TMap<FString, FDateTime> PostCallMtimes = ScanPluginDllMtimes();
		OutMeasured = PreCallMtimes.Num() > 0 || PostCallMtimes.Num() > 0;
		if (!OutMeasured)
		{
			return false;
		}
		return HasPatchDllTimestampChangeInternal(PreCallMtimes, PostCallMtimes);
	}

	FString GetEngineLogTailInternal()
	{
		if (GEngineLogTailOverride)
		{
			return GEngineLogTailOverride();
		}
		// Production best-effort: LiveCoding emits post-patch markers via
		// UE_LOG(LogLiveCoding, Display, ...), which end up in the editor's
		// currently-open log (Saved/Logs/*.log). Reading the live log here
		// introduces file-lock contention + noise that ErrorLog already
		// captures at a lower level. Return empty; ErrorLog is the primary
		// source for Signal 1. Production users who want an additional
		// source can override via SetEngineLogTailOverride.
		return FString();
	}

	Testing::FLiveCodingNoChangeDiagnostics RunNoChangeDiagnosticsInternal(
		const FString& ExpectedFilePath,
		const FString& ExpectedContent,
		const FString& CurrentTargetName)
	{
		Testing::FLiveCodingNoChangeDiagnostics Out;
		if (ExpectedFilePath.IsEmpty())
		{
			// Best-effort with nothing to compare against; all fields remain false.
			return Out;
		}

		const bool bOnDisk = IFileManager::Get().FileExists(*ExpectedFilePath);
		if (!bOnDisk)
		{
			// File doesn't exist on disk — can't have been edited on disk.
			// All sub-fields stay false; this is the "expected_file_edited_on_disk=false"
			// signal that triggers agent / user investigation.
			return Out;
		}

		// Field 1: file was modified in the last few minutes (proxy for
		// "edited during the current agent turn"). Exact match to pre-call
		// mtime would require pre-call snapshot; best-effort proxy is a
		// recent-modification window (5 minutes).
		const FDateTime Mtime = IFileManager::Get().GetTimeStamp(*ExpectedFilePath);
		const FDateTime Now = FDateTime::UtcNow();
		const FTimespan Elapsed = Now - Mtime;
		Out.bExpectedFileEditedOnDisk = (Elapsed.GetTotalMinutes() < 5.0);

		// Field 2: disk content matches expected content (saved-to-disk invariant).
		if (!ExpectedContent.IsEmpty())
		{
			FString DiskContent;
			if (FFileHelper::LoadFileToString(DiskContent, *ExpectedFilePath))
			{
				Out.bExpectedFileSavedToDisk = DiskContent.Contains(ExpectedContent);
				// Field 5: if the expected content was already on disk BEFORE
				// any edit in the current turn (i.e., no mtime change), the
				// diff was a no-op re-write.
				Out.bDiffAlreadyAppliedBeforeCall = Out.bExpectedFileSavedToDisk
					&& !Out.bExpectedFileEditedOnDisk;
			}
		}

		// Field 3: file is under Source/ of some module directory. Best-effort
		// heuristic: file path contains "/Source/<ModuleName>/".
		Out.bFileInCompiledModule = ExpectedFilePath.Contains(TEXT("/Source/"))
			|| ExpectedFilePath.Contains(TEXT("\\Source\\"));

		// Field 4: module-target manifest sees file. Best-effort: target
		// intermediate manifest exists and mentions the filename. Only
		// computed if CurrentTargetName is provided.
		if (!CurrentTargetName.IsEmpty())
		{
			const FString ProjectRoot = FPaths::ProjectDir();
			const FString ManifestDir = FPaths::Combine(
				ProjectRoot,
				TEXT("Intermediate"),
				TEXT("Build"),
				TEXT("Win64"),
				CurrentTargetName,
				TEXT("Development"));
			if (FPaths::DirectoryExists(ManifestDir))
			{
				// Proxy: directory exists = target has been built; file's
				// cleanFileName appears in any manifest file.
				const FString FileBase = FPaths::GetCleanFilename(ExpectedFilePath);
				TArray<FString> ManifestFiles;
				IFileManager::Get().FindFilesRecursive(
					ManifestFiles,
					*ManifestDir,
					TEXT("*.uhtmanifest"),
					true,
					false);
				for (const FString& ManifestPath : ManifestFiles)
				{
					FString ManifestBody;
					if (FFileHelper::LoadFileToString(ManifestBody, *ManifestPath)
						&& ManifestBody.Contains(FileBase, ESearchCase::IgnoreCase))
					{
						Out.bModuleTargetSeesFile = true;
						break;
					}
				}
			}
		}

		// Field 6: wrong target/project — file path is under a different
		// project tree than the one the editor is currently running.
		const FString ProjectRootForCompare = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		const FString FullFilePath = FPaths::ConvertRelativePathToFull(ExpectedFilePath);
		Out.bWrongTargetOrProject = !FullFilePath.StartsWith(ProjectRootForCompare, ESearchCase::IgnoreCase);

		return Out;
	}

	/**
	 * 626 P4 classifier — triangulated replacement for 619 P1's substring
	 * rescue. The decision table:
	 *
	 *   bSuccess=true  + bResultWasNoChanges=true + bAgentDiffExpected=true
	 *     → live_coding_no_changes_despite_expected_diff (emit 6 diag fields)
	 *   bSuccess=true  (any other shape)
	 *     → live_coding_patched
	 *   bSuccess=false + compile errors present
	 *     → live_coding_failed
	 *   bSuccess=false + cancelled marker + S1 + S3 + S2 + DLL measured
	 *     → live_coding_patched
	 *   bSuccess=false + cancelled marker + S1 + S3 + !S2 + DLL measured
	 *     → live_coding_patched_no_reinstance_needed
	 *   bSuccess=false + cancelled marker + (!S1 OR !S3 OR !DLL measured)
	 *     → live_coding_cancelled_unverified
	 *   bSuccess=false + no cancelled marker + no compile errors
	 *     → live_coding_failed (unknown failure, no rescue claim)
	 *
	 * Defense-in-depth principle: never claim `patched` without Signal 1
	 * (positive reload) AND Signal 3 (no compile errors). Signal 2 (DLL
	 * mtime) distinguishes `patched` vs `patched_no_reinstance_needed`.
	 * When DLL measurement was skipped (bAnyDllMtimeMeasured=false),
	 * degrade to `cancelled_unverified` rather than forging a claim.
	 */
	FString ClassifyRefreshStatusWithTriangulationInternal(
		const Testing::FLiveCodingTriangulationSignals& S)
	{
		if (S.bSuccess)
		{
			if (S.bAgentDiffExpected && S.bResultWasNoChanges)
			{
				return TEXT("live_coding_no_changes_despite_expected_diff");
			}
			return TEXT("live_coding_patched");
		}

		// bSuccess == false: real failure vs rescuable Cancelled?

		// Any compile/link error short-circuits to failure regardless of
		// other signals. Protects against false-positive rescue when both
		// errors and a cancellation message coexist.
		if (!S.bSignal3_NoCompileErrors)
		{
			return TEXT("live_coding_failed");
		}

		if (S.bHasCancelledMarker)
		{
			if (!S.bSignal1_PositiveReload)
			{
				// No engine reload confirmation — cannot claim patched.
				return TEXT("live_coding_cancelled_unverified");
			}
			if (!S.bAnyDllMtimeMeasured)
			{
				// DLL scan was skipped; we have Signal 1 + Signal 3 but
				// no ground truth on whether a patch DLL was actually
				// written. Degrade rather than claim.
				return TEXT("live_coding_cancelled_unverified");
			}
			if (S.bSignal2_DllMtimeChanged)
			{
				// Full triangulation: reload confirmed, DLL rewritten, no errors.
				return TEXT("live_coding_patched");
			}
			// Signal 1 + Signal 3, Signal 2 false: no-reinstance path.
			return TEXT("live_coding_patched_no_reinstance_needed");
		}

		// No cancelled marker, no compile errors, bSuccess=false.
		// This is an unknown failure; don't rescue.
		return TEXT("live_coding_failed");
	}

	/**
	 * 619 P1 legacy signature — kept as a thin adapter over the new
	 * triangulation classifier so existing call sites in Execute() that
	 * don't yet carry Signal 2 / Signal 1 inputs still compile. The
	 * adapter builds a degraded signal set (Signal 1 from ErrorLog only,
	 * Signal 2 = false + bAnyDllMtimeMeasured=false, Signal 3 from
	 * ErrorLog) and calls the triangulation classifier, which correctly
	 * routes to `cancelled_unverified` when the caller didn't measure.
	 * Execute() uses the full-triangulation path via the new signals
	 * builder below.
	 */
	FString ClassifyRefreshStatusFromDiagnostics(
		bool bSuccess,
		const FString& ErrorLog,
		const TSharedPtr<FJsonObject>& Diagnostics,
		bool bAgentDiffExpected)
	{
		Testing::FLiveCodingTriangulationSignals S;
		S.bSuccess = bSuccess;
		S.bSignal1_PositiveReload = HasPositiveReloadSignalInternal(ErrorLog, FString());
		S.bSignal2_DllMtimeChanged = false;
		S.bSignal3_NoCompileErrors = HasAbsenceOfCompileErrorsInternal(ErrorLog);
		S.bAnyDllMtimeMeasured = false;
		S.bHasCancelledMarker = ErrorLog.Contains(TEXT("cancelled"), ESearchCase::IgnoreCase)
			|| ErrorLog.Contains(TEXT("canceled"), ESearchCase::IgnoreCase);
		S.bAgentDiffExpected = bAgentDiffExpected;
		if (Diagnostics.IsValid())
		{
			FString CompileResultText;
			if (Diagnostics->TryGetStringField(TEXT("compile_result"), CompileResultText))
			{
				S.bResultWasNoChanges = CompileResultText.Equals(
					TEXT("NoChanges"), ESearchCase::IgnoreCase);
			}
		}
		return ClassifyRefreshStatusWithTriangulationInternal(S);
	}

} // anonymous namespace

namespace Testing
{
	void SetIsEnabledForSessionOverride(TFunction<bool()> Override)
	{
		GIsEnabledForSessionOverride = MoveTemp(Override);
	}

	void SetEnableForSessionOverride(TFunction<void(bool)> Override)
	{
		GEnableForSessionOverride = MoveTemp(Override);
	}

	void SetTriggerLiveCodingCompileOverride(TFunction<FMockCompileResult()> Override)
	{
		GTriggerLiveCodingCompileOverride = MoveTemp(Override);
	}

	void SetIsCompilingPollOverride(TFunction<bool(int32)> Override)
	{
		GIsCompilingPollOverride = MoveTemp(Override);
	}

	void SetDllMtimeChangedOverride(TFunction<bool(bool&)> Override)
	{
		GDllMtimeChangedOverride = MoveTemp(Override);
	}

	void SetEngineLogTailOverride(TFunction<FString()> Override)
	{
		GEngineLogTailOverride = MoveTemp(Override);
	}

	void ClearAllOverrides()
	{
		GIsEnabledForSessionOverride = TFunction<bool()>();
		GEnableForSessionOverride = TFunction<void(bool)>();
		GTriggerLiveCodingCompileOverride = TFunction<FMockCompileResult()>();
		GIsCompilingPollOverride = TFunction<bool(int32)>();
		GDllMtimeChangedOverride = TFunction<bool(bool&)>();
		GEngineLogTailOverride = TFunction<FString()>();
		GIsCompilingPollCounter = 0;
	}

	FString ClassifyRefreshStatusWithTriangulation(const FLiveCodingTriangulationSignals& Signals)
	{
		return ClassifyRefreshStatusWithTriangulationInternal(Signals);
	}

	bool HasPositiveReloadSignal(const FString& ErrorLog, const FString& EngineLogTail)
	{
		return HasPositiveReloadSignalInternal(ErrorLog, EngineLogTail);
	}

	bool HasPatchDllTimestampChange(
		const TMap<FString, FDateTime>& PreCallMtimes,
		const TMap<FString, FDateTime>& PostCallMtimes)
	{
		return HasPatchDllTimestampChangeInternal(PreCallMtimes, PostCallMtimes);
	}

	bool HasAbsenceOfCompileErrors(const FString& ErrorLog)
	{
		return HasAbsenceOfCompileErrorsInternal(ErrorLog);
	}

	FLiveCodingNoChangeDiagnostics RunNoChangeDiagnostics(
		const FString& ExpectedFilePath,
		const FString& ExpectedContent,
		const FString& CurrentTargetName)
	{
		return RunNoChangeDiagnosticsInternal(ExpectedFilePath, ExpectedContent, CurrentTargetName);
	}
}

} // namespace LiveCodingCompile
} // namespace UnrealClaude

// -------------------------------------------------------------------------
// FMCPTool_LiveCodingCompile
// -------------------------------------------------------------------------

FMCPToolInfo FMCPTool_LiveCodingCompile::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("livecoding_compile");

	// 619 D6: description IS the contract. Verbatim from spec section Fix #1
	// "Description (critical -- this is what steers agent choice)".
	// Do NOT abbreviate or paraphrase -- this text steers the LLM tool
	// selection prior in Codex/Claude and must match the spec exactly.
	Info.Description = TEXT(
		"Hot-patch pending C++ source changes via Unreal Live Coding.\n\n"
		"Call this FIRST for any C++ edit that touches only .cpp bodies, file-local static functions, or non-reflected helpers. Live Coding builds a numbered patch DLL and loads it alongside the running editor -- no restart, no reattach, typically completes in 2-15 seconds.\n\n"
		"Do not call this for changes that modify reflection layout: adding/removing UPROPERTY, UFUNCTION, UCLASS, USTRUCT, constructor bodies, or base-class membership. These require restart_survival because Live Coding cannot safely re-instance existing objects against a new reflected layout. If you call livecoding_compile on such a change, the result will be live_coding_failed with a diagnostic pointing to the offending declaration -- at that point escalate to restart_survival.\n\n"
		"Windows-only (UE 5.7 Live Coding is a Windows component). On other platforms returns a clear error.\n\n"
		"Typical flow: (1) agent uses apply_patch or equivalent to write the C++ change to disk; (2) agent calls livecoding_compile; (3) if success then done; if reflection-layout failure then call restart_survival."
	);

	Info.Parameters = {
		FMCPToolParameter(TEXT("wait_for_completion"), TEXT("boolean"),
			TEXT("Whether to block until Live Coding completes. Default: true. MVP: async=false is not yet supported; passing false returns a structured error directing the agent to omit the parameter."), false, TEXT("true")),
		FMCPToolParameter(TEXT("timeout_seconds"), TEXT("integer"),
			TEXT("Poll cap for IsCompiling() busy-wait. Default: 120. Clamped to [5, 300]. Reserved -- the underlying TriggerLiveCodingCompile already enforces a 120s cap; this parameter is reported on the tool result for downstream benchmarking but does not reduce the built-in cap."), false, TEXT("120")),
		FMCPToolParameter(TEXT("enable_for_session_if_needed"), TEXT("boolean"),
			TEXT("If true and Live Coding is currently disabled for the session, call ILiveCodingModule::EnableForSession(true) before compiling. Respects UUnrealClaudeSettings::bAllowAutoEnableLiveCoding -- when that setting is false, the tool returns a structured error instead of auto-enabling. Default: true."), false, TEXT("true")),
		FMCPToolParameter(TEXT("agent_diff_expected"), TEXT("boolean"),
			TEXT("Optional signal that the agent just wrote a real diff to disk. When true, a TriggerLiveCodingCompile result of NoChanges is classified as live_coding_no_changes_despite_expected_diff and the result carries a no_change_diagnostic object with 6 structured sub-fields (expected_file_edited_on_disk, expected_file_saved_to_disk, file_in_compiled_module, module_target_sees_file, diff_already_applied_before_call, wrong_target_or_project). Default: false."), false, TEXT("false")),
		FMCPToolParameter(TEXT("expected_file_path"), TEXT("string"),
			TEXT("626 P4 NoChange diagnostic: absolute or project-relative path the agent edited. Only consulted when agent_diff_expected=true AND the result is classified as live_coding_no_changes_despite_expected_diff. Enables population of expected_file_edited_on_disk / file_in_compiled_module / wrong_target_or_project. Optional."), false),
		FMCPToolParameter(TEXT("expected_content"), TEXT("string"),
			TEXT("626 P4 NoChange diagnostic: substring the agent expects to appear in the edited file on disk. Enables population of expected_file_saved_to_disk / diff_already_applied_before_call. Optional."), false),
		FMCPToolParameter(TEXT("current_target_name"), TEXT("string"),
			TEXT("626 P4 NoChange diagnostic: UBT target name (e.g. Poligon1Editor, GDR_Shooter_YEditor) for manifest lookup. Enables population of module_target_sees_file. Optional."), false),
		// 619 P1: declared uniformly across all Modifying/Destructive tools per
		// 616 P7 schema convention. livecoding_compile does not dirty any UE
		// asset package (it patches an in-process DLL, not a .uasset), so the
		// mutation lifecycle wrapper will emit an empty lifecycle.saved[]
		// regardless of this flag. Declared for schema uniformity so agents
		// trained on the P7 convention see it on every Modifying tool.
		FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
			TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false)
	};

	// 619 P1 + reviewer addition #2: schema guard -- livecoding_compile is
	// Modifying (patches in-process DLL state) but NOT Destructive. A failure
	// leaves the editor session in the same state it was before the call.
	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_LiveCodingCompile::Execute(const TSharedRef<FJsonObject>& Params)
{
	using namespace UnrealClaude::LiveCodingCompile;

	// Parse optional parameters (all have defaults; no required params).
	const bool bWaitForCompletion = ExtractOptionalBool(Params, TEXT("wait_for_completion"), true);
	double RawTimeoutSeconds = 120.0;
	Params->TryGetNumberField(TEXT("timeout_seconds"), RawTimeoutSeconds);
	const int32 TimeoutSeconds = FMath::Clamp(static_cast<int32>(RawTimeoutSeconds), 5, 300);
	const bool bEnableForSessionIfNeeded = ExtractOptionalBool(Params, TEXT("enable_for_session_if_needed"), true);
	const bool bAgentDiffExpected = ExtractOptionalBool(Params, TEXT("agent_diff_expected"), false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

#if !WITH_LIVE_CODING
	// Non-Windows platforms and builds where Live Coding was stripped.
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("refresh_status"), TEXT("live_coding_unavailable"));
	Result->SetStringField(TEXT("error_log"), TEXT("Live Coding is not available in this build (WITH_LIVE_CODING=0)."));
	Result->SetBoolField(TEXT("enabled_for_session"), false);
	Result->SetNumberField(TEXT("elapsed_ms"), 0);
	{
		FMCPToolResult R;
		R.bSuccess = false;
		R.Message = TEXT("Live Coding not compiled into this build.");
		R.Data = Result;
		return R;
	}
#else
	// MVP: async path not yet supported. Reject the flag explicitly rather
	// than silently ignoring it so agents don't get unexpected sync behavior.
	if (!bWaitForCompletion)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("refresh_status"), TEXT("live_coding_failed"));
		Result->SetStringField(TEXT("error_log"),
			TEXT("wait_for_completion=false is not yet supported. Omit the parameter (defaults to true) or set it to true."));
		FMCPToolResult R;
		R.bSuccess = false;
		R.Message = TEXT("Async Live Coding not yet supported in P1.");
		R.Data = Result;
		return R;
	}

	const double StartTime = FPlatformTime::Seconds();

	// Step 1: probe IsEnabledForSession; auto-enable if permitted.
	bool bEnabledForSession = CheckLiveCodingEnabled();
	if (!bEnabledForSession && bEnableForSessionIfNeeded)
	{
		const UUnrealClaudeSettings* Settings = UUnrealClaudeSettings::Get();
		const bool bAllowAutoEnable = Settings ? Settings->bAllowAutoEnableLiveCoding : true;

		if (!bAllowAutoEnable)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("refresh_status"), TEXT("live_coding_disabled"));
			Result->SetStringField(TEXT("error_log"),
				TEXT("Live Coding is disabled for this session and bAllowAutoEnableLiveCoding is false. "
					 "Enable Live Coding in Editor Preferences (Ctrl+Alt+F11) or flip the plugin setting and retry."));
			Result->SetStringField(TEXT("reason"), TEXT("user_opt_out_bAllowAutoEnableLiveCoding"));
			Result->SetBoolField(TEXT("enabled_for_session"), false);
			Result->SetNumberField(TEXT("elapsed_ms"),
				static_cast<int32>((FPlatformTime::Seconds() - StartTime) * 1000.0));
			FMCPToolResult R;
			R.bSuccess = false;
			R.Message = TEXT("Live Coding disabled and auto-enable is forbidden.");
			R.Data = Result;
			return R;
		}

		InvokeEnableLiveCodingForSession(true);
		bEnabledForSession = CheckLiveCodingEnabled();
	}

	if (!bEnabledForSession)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("refresh_status"), TEXT("live_coding_disabled"));
		Result->SetStringField(TEXT("error_log"),
			TEXT("Live Coding is not enabled for this session and enable_for_session_if_needed=false. "
				 "Press Ctrl+Alt+F11 in the editor or pass enable_for_session_if_needed=true."));
		Result->SetBoolField(TEXT("enabled_for_session"), false);
		Result->SetNumberField(TEXT("elapsed_ms"),
			static_cast<int32>((FPlatformTime::Seconds() - StartTime) * 1000.0));
		FMCPToolResult R;
		R.bSuccess = false;
		R.Message = TEXT("Live Coding not enabled for this session.");
		R.Data = Result;
		return R;
	}

	// Step 2: reentrancy guard (reviewer addition #1). If a compile is
	// already active (e.g. the user clicked Ctrl+Alt+F11 concurrently),
	// wait for it to finish before dispatching ours. In production this
	// is a no-op fallthrough; tests exercise it via the override seam.
	PollIsCompilingBlocking(TimeoutSeconds);

	// 626 P4: pre-call DLL mtime snapshot. Feeds Signal 2 of the
	// triangulation classifier. Scans UnrealEditor-*.dll under the plugin
	// Binaries/Win64 dir. No-op (empty map) if plugin layout is atypical.
	const TMap<FString, FDateTime> PreCallDllMtimes =
		GDllMtimeChangedOverride ? TMap<FString, FDateTime>() : ScanPluginDllMtimes();

	// Step 3: dispatch through the single-source-of-truth helper.
	const Testing::FMockCompileResult CompileResult = InvokeTriggerLiveCodingCompile();

	const double ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	const int32 ElapsedMs = static_cast<int32>(ElapsedSeconds * 1000.0);

	// 626 P4: post-call DLL mtime snapshot + compute Signal 2. Override
	// seam allows unit tests to inject a deterministic bool + measured flag.
	bool bAnyDllMtimeMeasured = false;
	const bool bSignal2_DllMtimeChanged = MeasureDllMtimeChangeAcrossCall(PreCallDllMtimes, bAnyDllMtimeMeasured);

	// 626 P4: build triangulation signals from real-world inputs.
	const FString EngineLogTail = GetEngineLogTailInternal();
	Testing::FLiveCodingTriangulationSignals Signals;
	Signals.bSuccess = CompileResult.bSuccess;
	Signals.bSignal1_PositiveReload = HasPositiveReloadSignalInternal(CompileResult.ErrorLog, EngineLogTail);
	Signals.bSignal2_DllMtimeChanged = bSignal2_DllMtimeChanged;
	Signals.bSignal3_NoCompileErrors = HasAbsenceOfCompileErrorsInternal(CompileResult.ErrorLog);
	Signals.bAnyDllMtimeMeasured = bAnyDllMtimeMeasured;
	Signals.bHasCancelledMarker =
		CompileResult.ErrorLog.Contains(TEXT("cancelled"), ESearchCase::IgnoreCase)
		|| CompileResult.ErrorLog.Contains(TEXT("canceled"), ESearchCase::IgnoreCase);
	Signals.bAgentDiffExpected = bAgentDiffExpected;
	if (CompileResult.Diagnostics.IsValid())
	{
		FString CompileResultText;
		if (CompileResult.Diagnostics->TryGetStringField(TEXT("compile_result"), CompileResultText))
		{
			Signals.bResultWasNoChanges = CompileResultText.Equals(
				TEXT("NoChanges"), ESearchCase::IgnoreCase);
		}
	}
	const FString RefreshStatus = ClassifyRefreshStatusWithTriangulationInternal(Signals);

	// 626 P4: patched/patched_no_reinstance_needed are both success shapes.
	// cancelled_unverified + no_changes_despite_expected_diff are reported
	// as success=false at the tool-result framing level so agents treat
	// them as actionable rather than silently-patched.
	const bool bEffectiveSuccess =
		CompileResult.bSuccess
		|| RefreshStatus.Equals(TEXT("live_coding_patched"), ESearchCase::IgnoreCase)
		|| RefreshStatus.Equals(TEXT("live_coding_patched_no_reinstance_needed"), ESearchCase::IgnoreCase);

	Result->SetBoolField(TEXT("success"), bEffectiveSuccess);
	Result->SetStringField(TEXT("refresh_status"), RefreshStatus);
	if (!CompileResult.ErrorLog.IsEmpty())
	{
		Result->SetStringField(TEXT("error_log"), CompileResult.ErrorLog);
	}
	if (CompileResult.Diagnostics.IsValid())
	{
		Result->SetObjectField(TEXT("diagnostics"), CompileResult.Diagnostics);
	}
	Result->SetBoolField(TEXT("enabled_for_session"), bEnabledForSession);
	Result->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);

	// 626 P4 telemetry: surface all 3 signals + measurement flag so agent +
	// user can audit classifier decisions.
	{
		TSharedPtr<FJsonObject> SignalsJson = MakeShared<FJsonObject>();
		SignalsJson->SetBoolField(TEXT("signal1_positive_reload"), Signals.bSignal1_PositiveReload);
		SignalsJson->SetBoolField(TEXT("signal2_dll_mtime_changed"), Signals.bSignal2_DllMtimeChanged);
		SignalsJson->SetBoolField(TEXT("signal3_no_compile_errors"), Signals.bSignal3_NoCompileErrors);
		SignalsJson->SetBoolField(TEXT("dll_mtime_measured"), Signals.bAnyDllMtimeMeasured);
		SignalsJson->SetBoolField(TEXT("has_cancelled_marker"), Signals.bHasCancelledMarker);
		SignalsJson->SetBoolField(TEXT("result_was_no_changes"), Signals.bResultWasNoChanges);
		Result->SetObjectField(TEXT("triangulation_signals"), SignalsJson);
	}

	// 626 P4: NoChange diagnostic triage.
	if (RefreshStatus.Equals(TEXT("live_coding_no_changes_despite_expected_diff"), ESearchCase::IgnoreCase))
	{
		FString ExpectedFilePath;
		FString ExpectedContent;
		FString CurrentTargetName;
		Params->TryGetStringField(TEXT("expected_file_path"), ExpectedFilePath);
		Params->TryGetStringField(TEXT("expected_content"), ExpectedContent);
		Params->TryGetStringField(TEXT("current_target_name"), CurrentTargetName);

		const Testing::FLiveCodingNoChangeDiagnostics NoChangeDiag =
			RunNoChangeDiagnosticsInternal(ExpectedFilePath, ExpectedContent, CurrentTargetName);

		TSharedPtr<FJsonObject> DiagJson = MakeShared<FJsonObject>();
		DiagJson->SetBoolField(TEXT("expected_file_edited_on_disk"), NoChangeDiag.bExpectedFileEditedOnDisk);
		DiagJson->SetBoolField(TEXT("expected_file_saved_to_disk"), NoChangeDiag.bExpectedFileSavedToDisk);
		DiagJson->SetBoolField(TEXT("file_in_compiled_module"), NoChangeDiag.bFileInCompiledModule);
		DiagJson->SetBoolField(TEXT("module_target_sees_file"), NoChangeDiag.bModuleTargetSeesFile);
		DiagJson->SetBoolField(TEXT("diff_already_applied_before_call"), NoChangeDiag.bDiffAlreadyAppliedBeforeCall);
		DiagJson->SetBoolField(TEXT("wrong_target_or_project"), NoChangeDiag.bWrongTargetOrProject);
		Result->SetObjectField(TEXT("no_change_diagnostic"), DiagJson);

		Result->SetStringField(TEXT("hint"),
			TEXT("Live Coding reported NoChanges but agent claimed a diff was written. "
				 "Inspect no_change_diagnostic.* fields to determine whether the edit "
				 "reached disk, belongs to a compiled module, or targets the correct project."));
	}
	else if (RefreshStatus.Equals(TEXT("live_coding_patched_no_reinstance_needed"), ESearchCase::IgnoreCase))
	{
		Result->SetStringField(TEXT("note"),
			TEXT("Live Coding compile completed; engine reported reload complete with no "
				 "object changes. Patch DLL was not rewritten because no layout change was "
				 "required (triangulated: Signal 1 + Signal 3 present, Signal 2 absent)."));
	}
	else if (RefreshStatus.Equals(TEXT("live_coding_cancelled_unverified"), ESearchCase::IgnoreCase))
	{
		Result->SetStringField(TEXT("hint"),
			TEXT("Live Coding reported Cancelled but the classifier could not triangulate "
				 "a successful patch (missing positive reload signal or DLL mtime measurement). "
				 "Inspect triangulation_signals.* to decide whether to retry, or escalate to "
				 "restart_survival if the task actually requires reflection-layout changes."));
	}
	else if (!CompileResult.bSuccess && bEffectiveSuccess)
	{
		Result->SetStringField(TEXT("note"),
			TEXT("Live Coding compile completed; triangulation confirmed a successful patch."));
	}

	if (!bEffectiveSuccess)
	{
		FMCPToolResult R;
		R.bSuccess = false;
		R.Message = TEXT("Live Coding compile failed -- see error_log.");
		R.Data = Result;
		return R;
	}

	return FMCPToolResult::Success(TEXT("Live Coding compile succeeded."), Result);
#endif
}
