// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUETaskRecoveryDetector.h"

#include "OsvayderSubsystem.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUEModule.h"
#include "OsvayderUERelayAgent.h"

namespace
{
	const int64 GCrashMatchWindowSeconds = 120; // +/- 2 minutes per dispatch spec.
	const int64 GRestartSurvivalRecentSuccessSeconds = 30 * 60;
	const int64 GRestartSurvivalStuckThresholdSeconds = 60 * 60;

	bool TryParseIso8601Utc(const FString& Iso, FDateTime& OutDateTime)
	{
		if (Iso.IsEmpty())
		{
			return false;
		}
		return FDateTime::ParseIso8601(*Iso, OutDateTime);
	}

	bool IsWithinWindow(const FDateTime& A, const FDateTime& B, int64 WindowSeconds)
	{
		const FTimespan Delta = A - B;
		const int64 Abs = FMath::Abs(Delta.GetTotalSeconds());
		return Abs <= WindowSeconds;
	}

	const TCHAR* TaskRecoverySuppressionReasonToString(const ETaskRecoverySuppressionReason Reason)
	{
		switch (Reason)
		{
		case ETaskRecoverySuppressionReason::IntentionalRestartSurvivalContinuation:
			return TEXT("intentional_restart_survival_continuation");
		case ETaskRecoverySuppressionReason::ActiveRestartSurvivalInProgress:
			return TEXT("active_restart_survival_in_progress");
		case ETaskRecoverySuppressionReason::None:
		default:
			return TEXT("none");
		}
	}

	bool IsRestartSurvivalPhaseActiveForSuppression(const EOsvayderUERestartSurvivalPhase Phase)
	{
		return Phase == EOsvayderUERestartSurvivalPhase::Detaching
			|| Phase == EOsvayderUERestartSurvivalPhase::DetachedRunning
			|| Phase == EOsvayderUERestartSurvivalPhase::AwaitingRelaunch
			|| Phase == EOsvayderUERestartSurvivalPhase::Relaunching
			|| Phase == EOsvayderUERestartSurvivalPhase::AwaitingReattach;
	}

	int64 ResolveElapsedSeconds(
		const FDateTime& NowUtc,
		const FString& PreferredIsoTimestamp,
		const int64 FallbackSeconds)
	{
		FDateTime Parsed;
		if (!TryParseIso8601Utc(PreferredIsoTimestamp, Parsed))
		{
			return FallbackSeconds;
		}

		return (NowUtc - Parsed).GetTotalSeconds();
	}

	bool HasRestartSurvivalFailure(const FOsvayderUERestartSurvivalState& State)
	{
		return State.Phase == EOsvayderUERestartSurvivalPhase::FailedTerminal
			|| State.DetachedTerminalOutcome.Equals(TEXT("failed"), ESearchCase::IgnoreCase)
			|| !State.FailureReason.IsEmpty();
	}
}

FTaskRecoveryDetectionResult FOsvayderUETaskRecoveryDetector::ClassifyPlan(
	const FOsvayderUEActivePlan& Plan,
	const FDateTime& NowUtc,
	const TArray<FDateTime>& CrashReportMtimes,
	const TOptional<FOsvayderUERestartSurvivalState>& RestartSurvivalState,
	int64 InterruptionThresholdSeconds)
{
	FTaskRecoveryDetectionResult Out;
	Out.PlanId = Plan.PlanId;

	// Short-circuit: user already handled this plan via the dialog.
	if (!Plan.UserRecoveryChoice.IsEmpty())
	{
		return Out;
	}

	// Only active incomplete plans participate in recovery classification.
	if (Plan.Status != TEXT("active"))
	{
		return Out;
	}
	if (Plan.ResultStatus != TEXT("incomplete"))
	{
		return Out;
	}

	FDateTime PlanUpdatedAt;
	if (!TryParseIso8601Utc(Plan.UpdatedAtUtc, PlanUpdatedAt))
	{
		return Out;
	}

	const int64 PlanElapsedSeconds = (NowUtc - PlanUpdatedAt).GetTotalSeconds();
	bool bCrashMatched = false;
	for (const FDateTime& CrashMtime : CrashReportMtimes)
	{
		if (IsWithinWindow(CrashMtime, PlanUpdatedAt, GCrashMatchWindowSeconds))
		{
			bCrashMatched = true;
			break;
		}
	}

	if (RestartSurvivalState.IsSet())
	{
		const FOsvayderUERestartSurvivalState& RestartState = RestartSurvivalState.GetValue();
		const int64 RestartElapsedSeconds = ResolveElapsedSeconds(
			NowUtc,
			RestartState.LastUpdatedAtUtc,
			PlanElapsedSeconds);

		if (RestartState.Phase == EOsvayderUERestartSurvivalPhase::Reattached
			&& (RestartState.bPostReattachCompletionPending || RestartState.bPostReattachCompletionDispatched))
		{
			Out.bDialogSuppressed = true;
			Out.SuppressionReason = ETaskRecoverySuppressionReason::IntentionalRestartSurvivalContinuation;
			Out.ElapsedSeconds = RestartElapsedSeconds;
			return Out;
		}

		if (IsRestartSurvivalPhaseActiveForSuppression(RestartState.Phase))
		{
			if (RestartElapsedSeconds > GRestartSurvivalStuckThresholdSeconds)
			{
				Out.bInterruptionDetected = true;
				Out.InterruptionReason = TEXT("previous_restart_survival_stuck_or_crashed");
				Out.DetectionTimestampUtc = NowUtc.ToIso8601();
				Out.ElapsedSeconds = RestartElapsedSeconds;
				return Out;
			}

			Out.bDialogSuppressed = true;
			Out.SuppressionReason = ETaskRecoverySuppressionReason::ActiveRestartSurvivalInProgress;
			Out.ElapsedSeconds = RestartElapsedSeconds;
			return Out;
		}

		if (HasRestartSurvivalFailure(RestartState))
		{
			Out.bInterruptionDetected = true;
			Out.InterruptionReason = TEXT("previous_restart_survival_failed");
			Out.DetectionTimestampUtc = NowUtc.ToIso8601();
			Out.ElapsedSeconds = RestartElapsedSeconds;
			return Out;
		}

		if (RestartState.DetachedTerminalOutcome.Equals(TEXT("success"), ESearchCase::IgnoreCase))
		{
			if (RestartElapsedSeconds > GRestartSurvivalRecentSuccessSeconds)
			{
				Out.bInterruptionDetected = true;
				Out.InterruptionReason = TEXT("editor_startup_after_quiet_period");
				Out.DetectionTimestampUtc = NowUtc.ToIso8601();
				Out.ElapsedSeconds = RestartElapsedSeconds;
				return Out;
			}

			Out.bDialogSuppressed = true;
			Out.SuppressionReason = ETaskRecoverySuppressionReason::IntentionalRestartSurvivalContinuation;
			Out.ElapsedSeconds = RestartElapsedSeconds;
			return Out;
		}
	}

	if (bCrashMatched)
	{
		// Crash reports are stronger evidence than the stale-time heuristic.
		// If the previous session crashed and the crash timestamp matches the
		// active plan window, surface recovery immediately instead of forcing
		// the user to wait for the quiet-period threshold.
		Out.bInterruptionDetected = true;
		Out.ElapsedSeconds = PlanElapsedSeconds;
		Out.DetectionTimestampUtc = NowUtc.ToIso8601();
		Out.InterruptionReason = TEXT("previous_session_crashed");
		return Out;
	}

	if (PlanElapsedSeconds <= InterruptionThresholdSeconds)
	{
		return Out;
	}

	Out.bInterruptionDetected = true;
	Out.ElapsedSeconds = PlanElapsedSeconds;
	Out.DetectionTimestampUtc = NowUtc.ToIso8601();
	Out.InterruptionReason = TEXT("editor_startup_after_quiet_period");

	return Out;
}

TArray<FDateTime> FOsvayderUETaskRecoveryDetector::CollectCrashReportMtimes()
{
	TArray<FDateTime> Out;

	const FString CrashDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Crashes"));
	if (!IFileManager::Get().DirectoryExists(*CrashDir))
	{
		return Out;
	}

	// Enumerate immediate-child directories of Saved/Crashes/ (UE creates a
	// per-crash subdir). We use the subdir's LastWriteTime as the crash mtime.
	TArray<FString> ChildDirs;
	IFileManager::Get().FindFiles(ChildDirs, *FPaths::Combine(CrashDir, TEXT("*")), false, true);
	for (const FString& Child : ChildDirs)
	{
		const FString FullPath = FPaths::Combine(CrashDir, Child);
		const FDateTime Mtime = IFileManager::Get().GetTimeStamp(*FullPath);
		if (Mtime != FDateTime::MinValue())
		{
			Out.Add(Mtime);
		}
	}

	// Also check any loose files at Saved/Crashes/ (defensive - UE normally uses
	// subdirs but policy / older versions might drop files).
	TArray<FString> LooseFiles;
	IFileManager::Get().FindFiles(LooseFiles, *FPaths::Combine(CrashDir, TEXT("*")), true, false);
	for (const FString& File : LooseFiles)
	{
		const FString FullPath = FPaths::Combine(CrashDir, File);
		const FDateTime Mtime = IFileManager::Get().GetTimeStamp(*FullPath);
		if (Mtime != FDateTime::MinValue())
		{
			Out.Add(Mtime);
		}
	}

	return Out;
}

void FOsvayderUETaskRecoveryDetector::RunStartupProbe()
{
	// Load plan. Missing/unreadable -> silent no-op (pre-632 projects, fresh
	// installs, and projects that simply have no interrupted work all hit this).
	FOsvayderUEActivePlan Plan;
	FString LoadError;
	if (!FOsvayderUERelayAgentManager::LoadActivePlan(Plan, LoadError))
	{
		UE_LOG(LogOsvayderUE, Verbose,
			TEXT("TaskRecoveryDetector: no active_plan.json present (%s). No-op."),
			*LoadError);
		return;
	}

	const FDateTime NowUtc = FDateTime::UtcNow();
	const TArray<FDateTime> CrashMtimes = CollectCrashReportMtimes();

	TOptional<FOsvayderUERestartSurvivalState> RestartState;
	const FString RestartStatePath = FOsvayderUERestartSurvivalManager::GetStatePath();
	if (IFileManager::Get().FileExists(*RestartStatePath))
	{
		FOsvayderUERestartSurvivalState LoadedRestartState;
		FString RestartLoadError;
		if (FOsvayderUERestartSurvivalManager::LoadState(LoadedRestartState, RestartLoadError))
		{
			RestartState = LoadedRestartState;
		}
		else
		{
			UE_LOG(LogOsvayderUE, Warning,
				TEXT("TaskRecoveryDetector: restart_survival_state.json exists but could not be read (%s). Falling back to plan-only classification."),
				*RestartLoadError);
		}
	}

	const FTaskRecoveryDetectionResult Result =
		FOsvayderUETaskRecoveryDetector::ClassifyPlan(Plan, NowUtc, CrashMtimes, RestartState);

	if (Result.bDialogSuppressed)
	{
		const bool bHadInterruptionMetadata = !Plan.InterruptionDetectedAtUtc.IsEmpty()
			|| !Plan.InterruptionReason.IsEmpty()
			|| Plan.InterruptionElapsedSeconds != 0;

		if (bHadInterruptionMetadata)
		{
			Plan.InterruptionDetectedAtUtc.Reset();
			Plan.InterruptionReason.Reset();
			Plan.InterruptionElapsedSeconds = 0;

			FString SaveError;
			if (!FOsvayderUERelayAgentManager::SaveActivePlan(Plan, SaveError))
			{
				UE_LOG(LogOsvayderUE, Warning,
					TEXT("TaskRecoveryDetector: suppression fired but clearing stale interruption metadata failed: %s"),
					*SaveError);
			}
		}

		UE_LOG(LogOsvayderUE, Verbose,
			TEXT("TaskRecoveryDetector: dialog suppressed for plan_id=%s, reason=%s, elapsed=%llds."),
			*Result.PlanId,
			TaskRecoverySuppressionReasonToString(Result.SuppressionReason),
			Result.ElapsedSeconds);

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("plan_id"), Result.PlanId);
		Payload->SetStringField(TEXT("suppression_reason"), TaskRecoverySuppressionReasonToString(Result.SuppressionReason));
		Payload->SetNumberField(TEXT("elapsed_seconds"), Result.ElapsedSeconds);
		Payload->SetStringField(TEXT("plan_updated_at_utc"), Plan.UpdatedAtUtc);
		if (RestartState.IsSet())
		{
			const FOsvayderUERestartSurvivalState& State = RestartState.GetValue();
			Payload->SetStringField(TEXT("restart_phase"), OsvayderUERestartSurvivalPhaseToString(State.Phase));
			Payload->SetStringField(TEXT("restart_last_updated_at_utc"), State.LastUpdatedAtUtc);
			Payload->SetStringField(TEXT("restart_terminal_outcome"), State.DetachedTerminalOutcome);
			Payload->SetStringField(TEXT("restart_failure_reason"), State.FailureReason);
			Payload->SetBoolField(TEXT("post_reattach_completion_pending"), State.bPostReattachCompletionPending);
			Payload->SetBoolField(TEXT("post_reattach_completion_dispatched"), State.bPostReattachCompletionDispatched);
		}

		const EOsvayderUEProviderBackend Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
		FOsvayderUEAgentTraceLog::Get().AppendEvent(
			TEXT("task_recovery_suppressed"),
			Backend,
			Payload);
		return;
	}

	if (!Result.bInterruptionDetected)
	{
		UE_LOG(LogOsvayderUE, Verbose,
			TEXT("TaskRecoveryDetector: plan not classified as interrupted (status=%s, result_status=%s, updated=%s). No-op."),
			*Plan.Status, *Plan.ResultStatus, *Plan.UpdatedAtUtc);
		return;
	}

	// Persist detection metadata into the plan so the widget dialog can read
	// it on first Tick without re-running the detector.
	Plan.InterruptionDetectedAtUtc = Result.DetectionTimestampUtc;
	Plan.InterruptionReason = Result.InterruptionReason;
	Plan.InterruptionElapsedSeconds = static_cast<int32>(Result.ElapsedSeconds);

	FString SaveError;
	if (!FOsvayderUERelayAgentManager::SaveActivePlan(Plan, SaveError))
	{
		UE_LOG(LogOsvayderUE, Warning,
			TEXT("TaskRecoveryDetector: detection fired but SaveActivePlan failed: %s. "
				 "Widget dialog will not show on this boot."),
			*SaveError);
		// Fall through - still emit log + trace event so forensic record exists.
	}

	UE_LOG(LogOsvayderUE, Warning,
		TEXT("Interrupted task detected: plan_id=%s, reason=%s, elapsed=%llds."),
		*Result.PlanId,
		*Result.InterruptionReason,
		Result.ElapsedSeconds);

	// Emit agent_trace event for forensic record.
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("plan_id"), Result.PlanId);
	Payload->SetStringField(TEXT("reason"), Result.InterruptionReason);
	Payload->SetNumberField(TEXT("elapsed_seconds"), Result.ElapsedSeconds);
	Payload->SetStringField(TEXT("detection_timestamp_utc"), Result.DetectionTimestampUtc);
	Payload->SetStringField(TEXT("plan_updated_at_utc"), Plan.UpdatedAtUtc);

	const EOsvayderUEProviderBackend Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("task_recovery_detected"),
		Backend,
		Payload);
}
