// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformAtomics.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * Phase timing entry for diagnostic traces.
 */
struct FPhaseTimingEntry
{
	FString Phase;
	double DurationMs = 0.0;
	double StartOffsetMs = 0.0; // offset from trace start
};

/**
 * Structured execution receipt with diagnostic trace fields.
 */
struct FExecutionReceipt
{
	// Core receipt fields
	FString Tool;
	bool bSuccess = false;
	bool bPartialSuccess = false;
	FString TargetType; // "blueprint", "actor", "asset", "file", etc.
	TArray<FString> Targets;
	TArray<FString> Created;
	TArray<FString> Modified;
	TArray<FString> Deleted;
	int32 WarningCount = 0;
	FString ValidationSummary;
	FString Classification; // "user_mutation", "internal_state", "denied"
	FDateTime Timestamp;

	// Diagnostic trace fields
	FString TraceId;
	FString TaskId; // if async (task queue)
	FString Status; // "success", "partial_success", "failed", "denied"
	double DurationMs = 0.0;
	FString Summary;
	FString ErrorOrDenialReason;
	TArray<FPhaseTimingEntry> PhaseTimings;
	bool bUsedTaskQueue = false;
	bool bWaitedOnGameThread = false;
	bool bHitCompile = false;
	FAgentCanonExecution CanonExecution;
	FString TransportFailureFamily;
	bool bTransportRetrySafe = false;
	bool bTransportRetryAttempted = false;
	FString TransportRetryBlockReason;
	FString TransportRetrySourceRunId;

	FExecutionReceipt()
		: Timestamp(FDateTime::Now())
	{}

	/** Generate a unique trace ID */
	static FString GenerateTraceId()
	{
		static volatile int32 Counter = 0;
		int32 Id = FPlatformAtomics::InterlockedIncrement(&Counter);
		return FString::Printf(TEXT("trace_%04d_%s"), Id, *FDateTime::Now().ToString(TEXT("%H%M%S")));
	}

	/** Add a phase timing entry */
	void AddPhaseTiming(const FString& Phase, double InDurationMs, double InStartOffsetMs = 0.0)
	{
		FPhaseTimingEntry Entry;
		Entry.Phase = Phase;
		Entry.DurationMs = InDurationMs;
		Entry.StartOffsetMs = InStartOffsetMs;
		PhaseTimings.Add(Entry);
	}

	TSharedPtr<FJsonObject> ToJson() const
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("tool"), Tool);
		Obj->SetBoolField(TEXT("success"), bSuccess);
		if (bPartialSuccess) Obj->SetBoolField(TEXT("partial_success"), true);
		Obj->SetStringField(TEXT("target_type"), TargetType);
		Obj->SetStringField(TEXT("timestamp"), Timestamp.ToString());

		auto ToJsonArray = [](const TArray<FString>& Arr) {
			TArray<TSharedPtr<FJsonValue>> JArr;
			for (const FString& S : Arr) JArr.Add(MakeShared<FJsonValueString>(S));
			return JArr;
		};

		if (Targets.Num() > 0) Obj->SetArrayField(TEXT("targets"), ToJsonArray(Targets));
		if (Created.Num() > 0) Obj->SetArrayField(TEXT("created"), ToJsonArray(Created));
		if (Modified.Num() > 0) Obj->SetArrayField(TEXT("modified"), ToJsonArray(Modified));
		if (Deleted.Num() > 0) Obj->SetArrayField(TEXT("deleted"), ToJsonArray(Deleted));
		if (WarningCount > 0) Obj->SetNumberField(TEXT("warnings"), WarningCount);
		if (!ValidationSummary.IsEmpty()) Obj->SetStringField(TEXT("validation"), ValidationSummary);
		if (!Classification.IsEmpty()) Obj->SetStringField(TEXT("classification"), Classification);

		// Trace fields
		if (!TraceId.IsEmpty()) Obj->SetStringField(TEXT("trace_id"), TraceId);
		if (!TaskId.IsEmpty()) Obj->SetStringField(TEXT("task_id"), TaskId);
		if (!Status.IsEmpty()) Obj->SetStringField(TEXT("status"), Status);
		if (DurationMs > 0.0) Obj->SetNumberField(TEXT("duration_ms"), DurationMs);
		if (!Summary.IsEmpty()) Obj->SetStringField(TEXT("summary"), Summary);
		if (!ErrorOrDenialReason.IsEmpty()) Obj->SetStringField(TEXT("error_or_denial_reason"), ErrorOrDenialReason);
		if (bUsedTaskQueue) Obj->SetBoolField(TEXT("used_task_queue"), true);
		if (bWaitedOnGameThread) Obj->SetBoolField(TEXT("waited_on_game_thread"), true);
		if (bHitCompile) Obj->SetBoolField(TEXT("hit_compile"), true);
		if (CanonExecution.HasAnySignal())
		{
			TSharedPtr<FJsonObject> CanonObject = MakeShared<FJsonObject>();
			if (!CanonExecution.DetectedSubsystem.IsEmpty()) CanonObject->SetStringField(TEXT("detected_subsystem"), CanonExecution.DetectedSubsystem);
			if (!CanonExecution.TaskMode.IsEmpty()) CanonObject->SetStringField(TEXT("task_mode"), CanonExecution.TaskMode);
			CanonObject->SetBoolField(TEXT("canon_discovery_used"), CanonExecution.bCanonDiscoveryUsed);
			CanonObject->SetBoolField(TEXT("brief_was_produced"), CanonExecution.bBriefWasProduced);
			CanonObject->SetBoolField(TEXT("brief_part_b_required"), CanonExecution.bBriefPartBRequired);
			CanonObject->SetBoolField(TEXT("brief_part_b_produced"), CanonExecution.bBriefPartBProduced);
			CanonObject->SetBoolField(TEXT("brief_part_b_produced_before_first_mutating_tool"), CanonExecution.bBriefPartBProducedBeforeFirstMutatingTool);
			CanonObject->SetBoolField(TEXT("approved_pattern_found"), CanonExecution.bApprovedPatternFound);
			CanonObject->SetBoolField(TEXT("tool_exposure_adjusted"), CanonExecution.bToolExposureAdjusted);
			if (!CanonExecution.RequestedToolFamily.IsEmpty()) CanonObject->SetStringField(TEXT("requested_tool_family"), CanonExecution.RequestedToolFamily);
			if (CanonExecution.EnabledToolFamilyIds.Num() > 0) CanonObject->SetArrayField(TEXT("enabled_tool_family_ids"), ToJsonArray(CanonExecution.EnabledToolFamilyIds));
			if (!CanonExecution.ActualToolFamily.IsEmpty()) CanonObject->SetStringField(TEXT("actual_tool_family"), CanonExecution.ActualToolFamily);
			if (!CanonExecution.PrimaryMutationToolFamily.IsEmpty()) CanonObject->SetStringField(TEXT("primary_mutation_tool_family"), CanonExecution.PrimaryMutationToolFamily);
			if (CanonExecution.AuxiliaryToolFamilies.Num() > 0) CanonObject->SetArrayField(TEXT("auxiliary_tool_families"), ToJsonArray(CanonExecution.AuxiliaryToolFamilies));
			CanonObject->SetBoolField(TEXT("fallback_tool_used"), CanonExecution.bFallbackToolUsed);
			CanonObject->SetBoolField(TEXT("mutating_fallback_used"), CanonExecution.bMutatingFallbackUsed);
			CanonObject->SetBoolField(TEXT("policy_deny_occurred"), CanonExecution.bPolicyDenyOccurred);
			if (!CanonExecution.VerificationOutcome.IsEmpty()) CanonObject->SetStringField(TEXT("verification_outcome"), CanonExecution.VerificationOutcome);
			// 631 Agent self-retrospective: plugin-side validation signal.
			// Emitted whenever bSelfVerificationAttempted is true OR the result
			// string is non-empty (distinguishes "never attempted" from "skipped
			// but classified").
			if (CanonExecution.bSelfVerificationAttempted) CanonObject->SetBoolField(TEXT("self_verification_attempted"), CanonExecution.bSelfVerificationAttempted);
			if (!CanonExecution.SelfVerificationResult.IsEmpty()) CanonObject->SetStringField(TEXT("self_verification_result"), CanonExecution.SelfVerificationResult);
			if (CanonExecution.ImplementationBriefLines.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> BriefValues;
				for (const FString& Line : CanonExecution.ImplementationBriefLines)
				{
					BriefValues.Add(MakeShared<FJsonValueString>(Line));
				}
				CanonObject->SetArrayField(TEXT("implementation_brief_lines"), BriefValues);
			}
			if (CanonExecution.ImplementationBriefPartBLines.Num() > 0) CanonObject->SetArrayField(TEXT("implementation_brief_part_b_lines"), ToJsonArray(CanonExecution.ImplementationBriefPartBLines));
			if (!CanonExecution.ApprovedPatternKey.IsEmpty()) CanonObject->SetStringField(TEXT("approved_pattern_key"), CanonExecution.ApprovedPatternKey);
			if (!CanonExecution.LedgerPath.IsEmpty()) CanonObject->SetStringField(TEXT("ledger_path"), CanonExecution.LedgerPath);
			if (CanonExecution.FeatureWorkflow.HasAnySignal()) CanonObject->SetObjectField(TEXT("feature_workflow"), CanonExecution.FeatureWorkflow.ToJsonObject());
			Obj->SetObjectField(TEXT("canon_execution"), CanonObject);
		}
		if (CanonExecution.FeatureWorkflow.HasAnySignal())
		{
			Obj->SetObjectField(TEXT("feature_workflow"), CanonExecution.FeatureWorkflow.ToJsonObject());
		}
		if (!TransportFailureFamily.IsEmpty()
			|| !TransportRetrySourceRunId.IsEmpty()
			|| bTransportRetryAttempted
			|| !TransportRetryBlockReason.IsEmpty())
		{
			if (!TransportFailureFamily.IsEmpty())
			{
				Obj->SetStringField(TEXT("transport_failure_family"), TransportFailureFamily);
			}
			Obj->SetBoolField(TEXT("transport_retry_safe"), bTransportRetrySafe);
			Obj->SetBoolField(TEXT("transport_retry_attempted"), bTransportRetryAttempted);
			if (!TransportRetryBlockReason.IsEmpty())
			{
				Obj->SetStringField(TEXT("transport_retry_block_reason"), TransportRetryBlockReason);
			}
			if (!TransportRetrySourceRunId.IsEmpty())
			{
				Obj->SetStringField(TEXT("transport_retry_source_run_id"), TransportRetrySourceRunId);
			}
		}

		// Phase timings
		if (PhaseTimings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> PhasesArr;
			for (const FPhaseTimingEntry& P : PhaseTimings)
			{
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("phase"), P.Phase);
				PObj->SetNumberField(TEXT("duration_ms"), P.DurationMs);
				PObj->SetNumberField(TEXT("start_offset_ms"), P.StartOffsetMs);
				PhasesArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			Obj->SetArrayField(TEXT("phase_timings"), PhasesArr);
		}

		return Obj;
	}

	/** Serialize to single-line JSON for JSONL logging */
	FString ToJsonLine() const
	{
		FString JsonLine;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonLine);
		FJsonSerializer::Serialize(ToJson().ToSharedRef(), Writer);
		Writer->Close();
		return JsonLine;
	}
};

/**
 * Session-scoped execution log with persistent JSONL storage.
 */
class UNREALCLAUDE_API FUnrealClaudeExecutionLog
{
public:
	static FUnrealClaudeExecutionLog& Get()
	{
		static FUnrealClaudeExecutionLog Instance;
		return Instance;
	}

	/** Add a receipt to the log and persist to JSONL */
	void AddReceipt(const FExecutionReceipt& Receipt)
	{
		Receipts.Add(Receipt);
		// Keep last 200 entries in memory
		if (Receipts.Num() > 200)
		{
			Receipts.RemoveAt(0, Receipts.Num() - 200);
		}

		// Persist to JSONL file
		PersistReceipt(Receipt);
	}

	/** Get all receipts */
	const TArray<FExecutionReceipt>& GetReceipts() const { return Receipts; }

	/** Get recent N receipts */
	TArray<FExecutionReceipt> GetRecent(int32 Count = 20) const
	{
		int32 Start = FMath::Max(0, Receipts.Num() - Count);
		TArray<FExecutionReceipt> Recent;
		for (int32 i = Start; i < Receipts.Num(); i++)
		{
			Recent.Add(Receipts[i]);
		}
		return Recent;
	}

	/** Clear all receipts */
	void Clear() { Receipts.Empty(); }

	/** Get path to the persistent session log */
	FString GetSessionLogPath() const
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealClaude"), TEXT("diagnostic_trace.jsonl"));
	}

private:
	TArray<FExecutionReceipt> Receipts;

	void PersistReceipt(const FExecutionReceipt& Receipt)
	{
		FString SessionLogFile = GetSessionLogPath();
		FString SessionLogDir = FPaths::GetPath(SessionLogFile);
		IFileManager::Get().MakeDirectory(*SessionLogDir, true);

		FString JsonLine = Receipt.ToJsonLine() + TEXT("\n");
		FFileHelper::SaveStringToFile(JsonLine, *SessionLogFile,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			EFileWrite::FILEWRITE_Append);

		// Rotate if file exceeds 5MB
		int64 FileSize = IFileManager::Get().FileSize(*SessionLogFile);
		if (FileSize > 5 * 1024 * 1024)
		{
			FString RotatedFile = SessionLogFile + TEXT(".old");
			IFileManager::Get().Delete(*RotatedFile);
			IFileManager::Get().Move(*RotatedFile, *SessionLogFile);
		}
	}
};
