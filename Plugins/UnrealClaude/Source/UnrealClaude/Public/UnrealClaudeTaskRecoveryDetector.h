// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealClaudeRestartSurvival.h"

struct FUnrealClaudeActivePlan;

enum class ETaskRecoverySuppressionReason : uint8
{
	None,
	IntentionalRestartSurvivalContinuation,
	ActiveRestartSurvivalInProgress,
};

/**
 * 632 Task Recovery & Rehydration: interruption-detection result.
 * All fields are defaulted to empty/zero so callers can branch on "detected"
 * via the `bInterruptionDetected` flag.
 */
struct UNREALCLAUDE_API FTaskRecoveryDetectionResult
{
	bool bInterruptionDetected = false;
	bool bDialogSuppressed = false;
	ETaskRecoverySuppressionReason SuppressionReason = ETaskRecoverySuppressionReason::None;
	FString InterruptionReason;        // "previous_session_crashed" | "editor_startup_after_quiet_period" | "previous_restart_survival_failed" | "previous_restart_survival_stuck_or_crashed"
	FString DetectionTimestampUtc;     // ISO-8601 UTC at detection time
	int64 ElapsedSeconds = 0;          // Classification-relevant elapsed time in seconds
	FString PlanId;                    // copied from plan for quick log/event reference
};

/**
 * 632 Task Recovery & Rehydration: detect whether the plan-on-disk represents
 * an interrupted autonomous task. Pure-ish class (I/O for crash-report scan is
 * unavoidable, but computation is deterministic given inputs).
 *
 * Detection rule:
 *   Plan.Status == "active" AND
 *   Plan.ResultStatus == "incomplete" AND
 *   (
 *     (NowUtc - Plan.UpdatedAtUtc) > InterruptionThresholdSeconds
 *     OR crash evidence matches the plan window
 *   )
 * Crash evidence is an explicit override: if Saved/Crashes contains a crash
 * timestamp within the detector window for this plan, recovery should surface
 * immediately instead of waiting for the 5-minute quiet-period heuristic.
 */
class UNREALCLAUDE_API FUnrealClaudeTaskRecoveryDetector
{
public:
	/**
	 * Invoked from FUnrealClaudeModule::StartupModule() tail after the 628-v3
	 * engine patch probe. Reads active_plan.json if present, applies detection
	 * rule, writes interruption_* fields back to the plan, emits a
	 * task_recovery_detected agent_trace event, and logs a warning.
	 *
	 * NO-OP paths (no detection fired):
	 *   - active_plan.json absent or unreadable
	 *   - plan already has a user_recovery_choice set (already handled)
	 *   - plan status / result_status / updated_at_utc fail the detection rule
	 *   - plan is fresh (<= 5 minutes stale)
	 */
	static void RunStartupProbe();

	/**
	 * Exposed for tests: pure classifier on a plan + reference "now". Does NOT
	 * touch disk. Returns the detection result and leaves the plan untouched.
	 *
	 * @param Plan                The plan to classify.
	 * @param NowUtc              Reference "now" time.
	 * @param CrashReportMtimes   Array of UTC timestamps representing mtimes of
	 *                            crash reports in Saved/Crashes/. Empty for no
	 *                            crashes. Detector looks for any mtime within
	 *                            ±2 minutes of Plan.UpdatedAtUtc to classify
	 *                            reason = "previous_session_crashed".
	 * @param InterruptionThresholdSeconds
	 *                            Staleness threshold. Default 300 (5 minutes).
	 */
	static FTaskRecoveryDetectionResult ClassifyPlan(
		const FUnrealClaudeActivePlan& Plan,
		const FDateTime& NowUtc,
		const TArray<FDateTime>& CrashReportMtimes,
		const TOptional<FUnrealClaudeRestartSurvivalState>& RestartSurvivalState = TOptional<FUnrealClaudeRestartSurvivalState>(),
		int64 InterruptionThresholdSeconds = 300);

	/**
	 * Scan Saved/Crashes/ for crash-report mtimes. Returns a list of FDateTime
	 * values (UTC) for files/dirs whose LastWriteTime falls within a reasonable
	 * lookup window. No-op and returns empty on missing dir.
	 */
	static TArray<FDateTime> CollectCrashReportMtimes();
};
