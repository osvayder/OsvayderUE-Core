// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "Templates/Function.h"

/**
 * 619 P1: MCP tool `livecoding_compile` -- thin wrapper over
 * FScriptExecutionManager::TriggerLiveCodingCompile. Exposes Unreal Live
 * Coding as an agent-callable compile path for arbitrary pending C++
 * source changes (body edits, file-local statics, non-reflected helpers).
 *
 * Description text is the primary contract per spec 619 D6 -- it steers
 * Codex/Claude between `livecoding_compile` (fast path) and
 * `restart_survival` (escalation). Annotations = Modifying (not
 * Destructive): LC patches the in-process DLL, not on-disk data, and
 * failure leaves the editor session in the same state as before.
 *
 * Single source of truth (D2): all LC dispatch flows through the existing
 * helper at ScriptExecutionManager.cpp:349-598. This tool adds no new
 * ILiveCodingModule call sites on the success path.
 */
class FMCPTool_LiveCodingCompile : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};

namespace OsvayderUE
{
namespace LiveCodingCompile
{

/**
 * 619 P1 test-only seams. Setters are safe to call on the game thread.
 * Each setter-call must be paired with a Clear*() call (typically in a
 * ScopeExit lambda) so the next test starts from a clean state.
 *
 * These seams let automation tests substitute the three LC probes
 * (module presence, `IsEnabledForSession`, `EnableForSession`, and
 * `TriggerLiveCodingCompile`) without requiring a real Live Coding
 * module on the automation thread. Zero runtime cost when the
 * overrides are unset (null TFunction fallthrough).
 */
namespace Testing
{
	/** Result shape the override injects in place of the real helper. */
	struct FMockCompileResult
	{
		bool bSuccess = false;
		FString ErrorLog;
		TSharedPtr<FJsonObject> Diagnostics;
	};

	/**
	 * 626 P4 triangulated-signal container.
	 *
	 * Replaces the pre-P4 substring-only Cancelled rescue with three
	 * independent positive signals that must ALL agree before we claim
	 * `live_coding_patched`. Engineered as a plain struct so the classifier
	 * is pure + easily unit-testable without any file I/O or engine access.
	 *
	 * Signal 1 — positive reload markers in engine log / ErrorLog.
	 * Signal 2 — patch-DLL mtime advanced during the call window.
	 * Signal 3 — no MSVC/linker/UBT compile error patterns present.
	 */
	struct FLiveCodingTriangulationSignals
	{
		/** Mirror of the underlying helper's success bool. */
		bool bSuccess = false;

		/** Signal 1: ErrorLog or engine log contained a positive-reload marker. */
		bool bSignal1_PositiveReload = false;

		/**
		 * Signal 2: any `UnrealEditor-*.dll` under the plugin Binaries dir
		 * advanced its mtime between pre-call and post-call snapshots.
		 */
		bool bSignal2_DllMtimeChanged = false;

		/**
		 * Signal 3: inverse of HasAbsenceOfCompileErrors — TRUE when NO
		 * MSVC compile (`error C<digits>:`) / MSVC link (`error LNK<digits>:`)
		 * / UBT / GCC-style error pattern appears in ErrorLog.
		 */
		bool bSignal3_NoCompileErrors = false;

		/**
		 * Whether the DLL mtime signal was actually measurable. Distinguishes
		 * "Signal 2 false because measurement skipped" from "Signal 2 false
		 * because DLL really didn't change". Needed because
		 * `live_coding_patched_no_reinstance_needed` requires Signal 2 = FALSE
		 * AND measurement actually happened (otherwise we cannot claim the
		 * no-reinstance variant honestly).
		 */
		bool bAnyDllMtimeMeasured = false;

		/** ErrorLog contains a "cancelled" / "canceled" marker. */
		bool bHasCancelledMarker = false;

		/** Caller passed `agent_diff_expected=true` in the tool params. */
		bool bAgentDiffExpected = false;

		/**
		 * Underlying helper's diagnostics carried `compile_result=NoChanges`
		 * (the LC enum was NoChanges rather than Success).
		 */
		bool bResultWasNoChanges = false;
	};

	/**
	 * 626 P4 NoChange triage diagnostic sub-fields.
	 *
	 * Emitted when LC returns NoChanges and the caller signalled a real
	 * diff was written (`agent_diff_expected=true`). Six bool checks
	 * expose *why* LC saw no changes, so the agent / user can reason
	 * about where the turn went wrong (did the edit never reach disk?
	 * Wrong module? Already applied before the call? Wrong target?).
	 *
	 * Each field is individually unit-testable via the `RunNoChangeDiagnostics`
	 * helper with hand-crafted inputs.
	 */
	struct FLiveCodingNoChangeDiagnostics
	{
		/** File the agent claimed to edit actually changed on disk (mtime / content). */
		bool bExpectedFileEditedOnDisk = false;

		/** Disk content is in-sync (no un-saved editor buffer diverging from disk). */
		bool bExpectedFileSavedToDisk = false;

		/** Edited file lives under a module folder that is part of the active target. */
		bool bFileInCompiledModule = false;

		/** UBT target's intermediate manifest sees this file. */
		bool bModuleTargetSeesFile = false;

		/** Pre-call disk content was already in its post-edit state (null edit / re-write). */
		bool bDiffAlreadyAppliedBeforeCall = false;

		/** Edited file belongs to a target / project that the current UBT invocation is NOT building. */
		bool bWrongTargetOrProject = false;
	};

	/** Override for `ILiveCodingModule::IsEnabledForSession()`. Return true/false. */
	void SetIsEnabledForSessionOverride(TFunction<bool()> Override);

	/** Override for `ILiveCodingModule::EnableForSession(bool)`. Lets tests count invocations. */
	void SetEnableForSessionOverride(TFunction<void(bool)> Override);

	/** Override for `FScriptExecutionManager::TriggerLiveCodingCompile(...)`. Returns mock result. */
	void SetTriggerLiveCodingCompileOverride(TFunction<FMockCompileResult()> Override);

	/**
	 * Override for `ILiveCodingModule::IsCompiling()` polling. Returns a bool sequence
	 * indexed by call count (e.g. [true, true, false] simulates a compile finishing on
	 * the 3rd poll). Also used by the reentrancy test.
	 */
	void SetIsCompilingPollOverride(TFunction<bool(int32 /*PollCallIndex*/)> Override);

	/**
	 * 626 P4 override: substitute the DLL mtime signal. Tests inject a bool
	 * directly instead of scanning `Binaries/Win64/`. Null override = real
	 * file-system scan on the production path.
	 */
	void SetDllMtimeChangedOverride(TFunction<bool(bool& /*OutMeasured*/)> Override);

	/**
	 * 626 P4 override: substitute the engine-log-tail source that feeds
	 * HasPositiveReloadSignal. Tests inject fixture text; production reads
	 * engine log by best-effort (if empty, only ErrorLog is scanned).
	 */
	void SetEngineLogTailOverride(TFunction<FString()> Override);

	/** Clear all LC overrides in one call. */
	void ClearAllOverrides();

	/**
	 * 626 P4 pure classifier — takes triangulation signals, returns
	 * `refresh_status` string. Pure function, no I/O, safe to unit-test
	 * directly from automation harness.
	 */
	FString ClassifyRefreshStatusWithTriangulation(const FLiveCodingTriangulationSignals& Signals);

	/** 626 P4: true if ErrorLog or EngineLogTail contains any positive-reload marker. */
	bool HasPositiveReloadSignal(const FString& ErrorLog, const FString& EngineLogTail);

	/**
	 * 626 P4: true if any DLL mtime in PostCallMtimes is strictly greater
	 * than its PreCallMtimes counterpart (keyed by filename). Also true
	 * for keys that appear only in PostCallMtimes (new DLL written during
	 * the call, e.g. Patch_N.dll).
	 */
	bool HasPatchDllTimestampChange(
		const TMap<FString, FDateTime>& PreCallMtimes,
		const TMap<FString, FDateTime>& PostCallMtimes);

	/** 626 P4: true if ErrorLog carries NO MSVC/link/UBT/GCC compile-error pattern. */
	bool HasAbsenceOfCompileErrors(const FString& ErrorLog);

	/** 626 P4: compute the 6 NoChange diagnostic sub-fields from best-effort inputs. */
	FLiveCodingNoChangeDiagnostics RunNoChangeDiagnostics(
		const FString& ExpectedFilePath,
		const FString& ExpectedContent,
		const FString& CurrentTargetName);
}

} // namespace LiveCodingCompile
} // namespace OsvayderUE
