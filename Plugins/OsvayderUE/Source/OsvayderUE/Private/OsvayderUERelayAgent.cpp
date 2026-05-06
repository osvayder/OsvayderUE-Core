// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUERelayAgent.h"

#include "CodexCliRunner.h"
#include "JsonUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "OsvayderUEModule.h"
#include "OsvayderUERestartSurvival.h"
#include "OsvayderUESettings.h"
#include "OsvayderUEStorageMigration.h"

namespace
{
	constexpr int32 RelaySchemaVersion = 1;
	constexpr TCHAR RelayAgentScriptName[] = TEXT("OsvayderUE-RelayAgent.ps1");
	constexpr TCHAR ActivePlanFileName[] = TEXT("active_plan.json");
	constexpr TCHAR PlanArchiveDirName[] = TEXT("PlanArchives");
	constexpr TCHAR RelayArchiveDirName[] = TEXT("RelayArchives");
	constexpr int32 DefaultArchiveRetention = 20;

	FString NormalizeFilePath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return FString();
		}

		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	bool EnsureParentDirectoryExists(const FString& FilePath, FString& OutError)
	{
		const FString Directory = FPaths::GetPath(FilePath);
		if (Directory.IsEmpty() || IFileManager::Get().MakeDirectory(*Directory, true))
		{
			return true;
		}

		OutError = FString::Printf(TEXT("Could not create relay parent directory: %s"), *Directory);
		return false;
	}

	FString GetLegacyRelayRootDir()
	{
		return OsvayderUEStorageMigration::DeriveLegacyRootFromPreferred(FOsvayderUERestartSurvivalManager::GetStateRootDir());
	}

	FString GetLegacyRelayPath(const FString& FileName)
	{
		return FPaths::Combine(GetLegacyRelayRootDir(), FileName);
	}

	bool DeleteManagedRelayArtifact(
		const FString& PreferredPath,
		const FString& LegacyPath,
		const FString& LogicalName,
		FString& OutError)
	{
		return OsvayderUEStorageMigration::DeleteManagedFileCopies(
			PreferredPath,
			LegacyPath,
			LogicalName,
			OutError);
	}

	FString GetStringFieldOrEmpty(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		FString Value;
		FJsonUtils::GetStringField(Object, FieldName, Value);
		return Value;
	}

	int32 GetIntegerFieldOrDefault(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const int32 DefaultValue)
	{
		if (!Object.IsValid())
		{
			return DefaultValue;
		}

		double Value = static_cast<double>(DefaultValue);
		Object->TryGetNumberField(FieldName, Value);
		return static_cast<int32>(Value);
	}

	double GetDoubleFieldOrDefault(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const double DefaultValue)
	{
		if (!Object.IsValid())
		{
			return DefaultValue;
		}

		double Value = DefaultValue;
		Object->TryGetNumberField(FieldName, Value);
		return Value;
	}

	bool GetBoolFieldOrDefault(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const bool DefaultValue)
	{
		if (!Object.IsValid())
		{
			return DefaultValue;
		}

		bool Value = DefaultValue;
		Object->TryGetBoolField(FieldName, Value);
		return Value;
	}

	TArray<FString> GetStringArrayFieldOrEmpty(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		if (!FJsonUtils::GetArrayField(Object, FieldName, JsonValues))
		{
			return {};
		}

		return FJsonUtils::JsonArrayToStrings(JsonValues);
	}

	TSharedRef<FJsonObject> MakeSettingsJson(const FOsvayderUERelaySettingsSnapshot& Snapshot)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), Snapshot.SchemaVersion);
		Root->SetStringField(TEXT("backend"), Snapshot.Backend);
		Root->SetStringField(TEXT("backend_display_name"), Snapshot.BackendDisplayName);
		Root->SetStringField(TEXT("model"), Snapshot.Model);
		Root->SetStringField(TEXT("profile"), Snapshot.Profile);
		Root->SetStringField(TEXT("requested_speed_mode"), Snapshot.RequestedSpeedMode);
		Root->SetStringField(TEXT("effective_speed_mode"), Snapshot.EffectiveSpeedMode);
		Root->SetStringField(TEXT("speed_support"), Snapshot.SpeedSupport);
		Root->SetStringField(TEXT("work_mode"), Snapshot.WorkMode);
		Root->SetStringField(TEXT("reasoning_effort"), Snapshot.ReasoningEffort);
		Root->SetStringField(TEXT("verbosity"), Snapshot.Verbosity);
		Root->SetStringField(TEXT("auth_mode"), Snapshot.AuthMode);
		Root->SetStringField(TEXT("auth_path"), Snapshot.AuthPath);
		Root->SetStringField(TEXT("auth_ownership"), Snapshot.AuthOwnership);
		Root->SetStringField(TEXT("codex_home_path"), Snapshot.CodexHomePath);
		Root->SetStringField(TEXT("codex_home_resolution_source"), Snapshot.CodexHomeResolutionSource);
		Root->SetStringField(TEXT("execution_transport"), Snapshot.ExecutionTransport);
		Root->SetBoolField(TEXT("persistent_app_server_enabled"), Snapshot.bPersistentAppServerEnabled);
		Root->SetBoolField(TEXT("clear_proxy_env_for_exec"), Snapshot.bClearProxyEnvForExec);
		Root->SetBoolField(TEXT("has_explicit_codex_home_override"), Snapshot.bHasExplicitCodexHomeOverride);
		return Root;
	}

	FOsvayderUERelaySettingsSnapshot ParseSettingsSnapshot(const TSharedPtr<FJsonObject>& Object)
	{
		FOsvayderUERelaySettingsSnapshot Snapshot;
		if (!Object.IsValid())
		{
			return Snapshot;
		}

		Snapshot.SchemaVersion = GetIntegerFieldOrDefault(Object, TEXT("schema_version"), RelaySchemaVersion);
		Snapshot.Backend = GetStringFieldOrEmpty(Object, TEXT("backend"));
		Snapshot.BackendDisplayName = GetStringFieldOrEmpty(Object, TEXT("backend_display_name"));
		Snapshot.Model = GetStringFieldOrEmpty(Object, TEXT("model"));
		Snapshot.Profile = GetStringFieldOrEmpty(Object, TEXT("profile"));
		Snapshot.RequestedSpeedMode = GetStringFieldOrEmpty(Object, TEXT("requested_speed_mode"));
		Snapshot.EffectiveSpeedMode = GetStringFieldOrEmpty(Object, TEXT("effective_speed_mode"));
		Snapshot.SpeedSupport = GetStringFieldOrEmpty(Object, TEXT("speed_support"));
		Snapshot.WorkMode = GetStringFieldOrEmpty(Object, TEXT("work_mode"));
		Snapshot.ReasoningEffort = GetStringFieldOrEmpty(Object, TEXT("reasoning_effort"));
		Snapshot.Verbosity = GetStringFieldOrEmpty(Object, TEXT("verbosity"));
		Snapshot.AuthMode = GetStringFieldOrEmpty(Object, TEXT("auth_mode"));
		Snapshot.AuthPath = GetStringFieldOrEmpty(Object, TEXT("auth_path"));
		Snapshot.AuthOwnership = GetStringFieldOrEmpty(Object, TEXT("auth_ownership"));
		Snapshot.CodexHomePath = GetStringFieldOrEmpty(Object, TEXT("codex_home_path"));
		Snapshot.CodexHomeResolutionSource = GetStringFieldOrEmpty(Object, TEXT("codex_home_resolution_source"));
		Snapshot.ExecutionTransport = GetStringFieldOrEmpty(Object, TEXT("execution_transport"));
		Snapshot.bPersistentAppServerEnabled = GetBoolFieldOrDefault(Object, TEXT("persistent_app_server_enabled"), true);
		Snapshot.bClearProxyEnvForExec = GetBoolFieldOrDefault(Object, TEXT("clear_proxy_env_for_exec"), false);
		Snapshot.bHasExplicitCodexHomeOverride = GetBoolFieldOrDefault(Object, TEXT("has_explicit_codex_home_override"), false);
		return Snapshot;
	}

	TSharedRef<FJsonObject> MakeCompileProofJson(const FOsvayderUECompileProofState& State)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), State.SchemaVersion);
		Root->SetBoolField(TEXT("compiled_module_mutation_observed"), State.bCompiledModuleMutationObserved);
		Root->SetStringField(TEXT("mutation_tool_family"), State.MutationToolFamily);
		Root->SetStringField(TEXT("last_mutation_at_utc"), State.LastMutationAtUtc);
		Root->SetStringField(TEXT("last_mutation_tool_call_id"), State.LastMutationToolCallId);
		Root->SetStringField(TEXT("last_mutation_tool_name"), State.LastMutationToolName);
		Root->SetStringField(TEXT("last_compile_proof_at_utc"), State.LastCompileProofAtUtc);
		Root->SetStringField(TEXT("last_compile_proof_tool_call_id"), State.LastCompileProofToolCallId);
		Root->SetStringField(TEXT("last_compile_proof_tool_name"), State.LastCompileProofToolName);
		Root->SetStringField(TEXT("last_compile_proof_outcome"), State.LastCompileProofOutcome);
		Root->SetStringField(TEXT("last_compile_proof_detail"), State.LastCompileProofDetail);
		Root->SetStringField(TEXT("last_post_compile_verification_at_utc"), State.LastPostCompileVerificationAtUtc);
		Root->SetStringField(TEXT("last_post_compile_verification_tool_call_id"), State.LastPostCompileVerificationToolCallId);
		Root->SetStringField(TEXT("last_post_compile_verification_tool_name"), State.LastPostCompileVerificationToolName);
		Root->SetStringField(TEXT("last_post_compile_verification_outcome"), State.LastPostCompileVerificationOutcome);
		Root->SetStringField(TEXT("last_closeout_gate_reason"), State.LastCloseoutGateReason);
		return Root;
	}

	FOsvayderUECompileProofState ParseCompileProofState(const TSharedPtr<FJsonObject>& Object)
	{
		FOsvayderUECompileProofState State;
		if (!Object.IsValid())
		{
			return State;
		}

		State.SchemaVersion = GetIntegerFieldOrDefault(Object, TEXT("schema_version"), RelaySchemaVersion);
		State.bCompiledModuleMutationObserved = GetBoolFieldOrDefault(Object, TEXT("compiled_module_mutation_observed"), false);
		State.MutationToolFamily = GetStringFieldOrEmpty(Object, TEXT("mutation_tool_family"));
		State.LastMutationAtUtc = GetStringFieldOrEmpty(Object, TEXT("last_mutation_at_utc"));
		State.LastMutationToolCallId = GetStringFieldOrEmpty(Object, TEXT("last_mutation_tool_call_id"));
		State.LastMutationToolName = GetStringFieldOrEmpty(Object, TEXT("last_mutation_tool_name"));
		State.LastCompileProofAtUtc = GetStringFieldOrEmpty(Object, TEXT("last_compile_proof_at_utc"));
		State.LastCompileProofToolCallId = GetStringFieldOrEmpty(Object, TEXT("last_compile_proof_tool_call_id"));
		State.LastCompileProofToolName = GetStringFieldOrEmpty(Object, TEXT("last_compile_proof_tool_name"));
		State.LastCompileProofOutcome = GetStringFieldOrEmpty(Object, TEXT("last_compile_proof_outcome"));
		State.LastCompileProofDetail = GetStringFieldOrEmpty(Object, TEXT("last_compile_proof_detail"));
		State.LastPostCompileVerificationAtUtc = GetStringFieldOrEmpty(Object, TEXT("last_post_compile_verification_at_utc"));
		State.LastPostCompileVerificationToolCallId = GetStringFieldOrEmpty(Object, TEXT("last_post_compile_verification_tool_call_id"));
		State.LastPostCompileVerificationToolName = GetStringFieldOrEmpty(Object, TEXT("last_post_compile_verification_tool_name"));
		State.LastPostCompileVerificationOutcome = GetStringFieldOrEmpty(Object, TEXT("last_post_compile_verification_outcome"));
		State.LastCloseoutGateReason = GetStringFieldOrEmpty(Object, TEXT("last_closeout_gate_reason"));
		return State;
	}

	TSharedRef<FJsonObject> MakeHandoffJson(const FOsvayderUERelayHandoffContext& Context)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), Context.SchemaVersion);
		Root->SetStringField(TEXT("task_id"), Context.TaskId);
		Root->SetStringField(TEXT("relay_session_id"), Context.RelaySessionId);
		Root->SetStringField(TEXT("project_root"), Context.ProjectRoot);
		Root->SetStringField(TEXT("uproject_path"), Context.UProjectPath);
		Root->SetStringField(TEXT("reattach_token"), Context.ReattachToken);
		Root->SetStringField(TEXT("reattach_notice"), Context.ReattachNotice);
		Root->SetStringField(TEXT("created_at_utc"), Context.CreatedAtUtc);
		Root->SetStringField(TEXT("original_user_prompt"), Context.OriginalUserPrompt);
		Root->SetStringField(TEXT("origin_prompt_hash"), Context.OriginPromptHash);
		Root->SetBoolField(TEXT("visual_proof_required"), Context.bVisualProofRequired);
		Root->SetBoolField(TEXT("visual_qa_manifest_required"), Context.bVisualQaManifestRequired);
		Root->SetArrayField(TEXT("attached_image_paths"), FJsonUtils::StringArrayToJson(Context.AttachedImagePaths));
		Root->SetArrayField(TEXT("attachment_names"), FJsonUtils::StringArrayToJson(Context.AttachmentNames));
		Root->SetStringField(TEXT("editor_agent_summary"), Context.EditorAgentSummary);
		Root->SetStringField(TEXT("last_known_blocker_family"), Context.LastKnownBlockerFamily);
		Root->SetStringField(TEXT("last_known_blocker_signature"), Context.LastKnownBlockerSignature);
		Root->SetArrayField(TEXT("known_facts"), FJsonUtils::StringArrayToJson(Context.KnownFacts));
		Root->SetArrayField(TEXT("relevant_artifact_paths"), FJsonUtils::StringArrayToJson(Context.RelevantArtifactPaths));
		Root->SetArrayField(TEXT("relevant_tool_receipts"), FJsonUtils::StringArrayToJson(Context.RelevantToolReceipts));
		Root->SetStringField(TEXT("next_attempt_hypothesis"), Context.NextAttemptHypothesis);
		Root->SetStringField(TEXT("bounded_objective"), Context.BoundedObjective);
		Root->SetStringField(TEXT("bounded_objective_detail"), Context.BoundedObjectiveDetail);
		Root->SetNumberField(TEXT("reasoning_iteration_budget"), Context.ReasoningIterationBudget);
		Root->SetNumberField(TEXT("wall_clock_budget_seconds"), Context.WallClockBudgetSeconds);
		Root->SetObjectField(TEXT("settings"), MakeSettingsJson(Context.Settings));
		return Root;
	}

	FOsvayderUERelayHandoffContext ParseHandoffContext(const TSharedPtr<FJsonObject>& Object)
	{
		FOsvayderUERelayHandoffContext Context;
		if (!Object.IsValid())
		{
			return Context;
		}

		Context.SchemaVersion = GetIntegerFieldOrDefault(Object, TEXT("schema_version"), RelaySchemaVersion);
		Context.TaskId = GetStringFieldOrEmpty(Object, TEXT("task_id"));
		Context.RelaySessionId = GetStringFieldOrEmpty(Object, TEXT("relay_session_id"));
		Context.ProjectRoot = GetStringFieldOrEmpty(Object, TEXT("project_root"));
		Context.UProjectPath = GetStringFieldOrEmpty(Object, TEXT("uproject_path"));
		Context.ReattachToken = GetStringFieldOrEmpty(Object, TEXT("reattach_token"));
		Context.ReattachNotice = GetStringFieldOrEmpty(Object, TEXT("reattach_notice"));
		Context.CreatedAtUtc = GetStringFieldOrEmpty(Object, TEXT("created_at_utc"));
		Context.OriginalUserPrompt = GetStringFieldOrEmpty(Object, TEXT("original_user_prompt"));
		Context.OriginPromptHash = GetStringFieldOrEmpty(Object, TEXT("origin_prompt_hash"));
		Context.bVisualProofRequired = GetBoolFieldOrDefault(Object, TEXT("visual_proof_required"), false);
		Context.bVisualQaManifestRequired = GetBoolFieldOrDefault(Object, TEXT("visual_qa_manifest_required"), false);
		Context.AttachedImagePaths = GetStringArrayFieldOrEmpty(Object, TEXT("attached_image_paths"));
		Context.AttachmentNames = GetStringArrayFieldOrEmpty(Object, TEXT("attachment_names"));
		Context.EditorAgentSummary = GetStringFieldOrEmpty(Object, TEXT("editor_agent_summary"));
		Context.LastKnownBlockerFamily = GetStringFieldOrEmpty(Object, TEXT("last_known_blocker_family"));
		Context.LastKnownBlockerSignature = GetStringFieldOrEmpty(Object, TEXT("last_known_blocker_signature"));
		Context.KnownFacts = GetStringArrayFieldOrEmpty(Object, TEXT("known_facts"));
		Context.RelevantArtifactPaths = GetStringArrayFieldOrEmpty(Object, TEXT("relevant_artifact_paths"));
		Context.RelevantToolReceipts = GetStringArrayFieldOrEmpty(Object, TEXT("relevant_tool_receipts"));
		Context.NextAttemptHypothesis = GetStringFieldOrEmpty(Object, TEXT("next_attempt_hypothesis"));
		Context.BoundedObjective = GetStringFieldOrEmpty(Object, TEXT("bounded_objective"));
		Context.BoundedObjectiveDetail = GetStringFieldOrEmpty(Object, TEXT("bounded_objective_detail"));
		Context.ReasoningIterationBudget = GetIntegerFieldOrDefault(Object, TEXT("reasoning_iteration_budget"), 10);
		Context.WallClockBudgetSeconds = GetIntegerFieldOrDefault(Object, TEXT("wall_clock_budget_seconds"), 900);

		const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
		if (Object->TryGetObjectField(TEXT("settings"), SettingsObject) && SettingsObject && SettingsObject->IsValid())
		{
			Context.Settings = ParseSettingsSnapshot(*SettingsObject);
		}

		return Context;
	}

	TSharedRef<FJsonObject> MakeProgressJson(const FOsvayderUERelayProgressEntry& Entry)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), Entry.SchemaVersion);
		Root->SetStringField(TEXT("task_id"), Entry.TaskId);
		Root->SetStringField(TEXT("plan_id"), Entry.PlanId);
		Root->SetStringField(TEXT("mechanic_id"), Entry.MechanicId);
		Root->SetStringField(TEXT("tool_call_id"), Entry.ToolCallId);
		Root->SetStringField(TEXT("relay_session_id"), Entry.RelaySessionId);
		Root->SetStringField(TEXT("timestamp_utc"), Entry.TimestampUtc);
		Root->SetStringField(TEXT("entry_kind"), Entry.EntryKind);
		Root->SetStringField(TEXT("summary"), Entry.Summary);
		Root->SetStringField(TEXT("summary_ru"), Entry.SummaryRu);
		Root->SetStringField(TEXT("technical_detail"), Entry.TechnicalDetail);
		Root->SetStringField(TEXT("current_action"), Entry.CurrentAction);
		Root->SetStringField(TEXT("current_action_ru"), Entry.CurrentActionRu);
		Root->SetStringField(TEXT("current_tool_name"), Entry.CurrentToolName);
		Root->SetNumberField(TEXT("iteration_index"), Entry.IterationIndex);
		Root->SetNumberField(TEXT("elapsed_seconds"), Entry.ElapsedSeconds);
		Root->SetNumberField(TEXT("heartbeat_age_seconds"), Entry.HeartbeatAgeSeconds);
		Root->SetBoolField(TEXT("is_stale"), Entry.bIsStale);
		Root->SetStringField(TEXT("terminal_outcome"), OsvayderUERelayTerminalOutcomeToString(Entry.TerminalOutcome));
		return Root;
	}

	FOsvayderUERelayProgressEntry ParseProgressEntry(const TSharedPtr<FJsonObject>& Object)
	{
		FOsvayderUERelayProgressEntry Entry;
		if (!Object.IsValid())
		{
			return Entry;
		}

		Entry.SchemaVersion = GetIntegerFieldOrDefault(Object, TEXT("schema_version"), RelaySchemaVersion);
		Entry.TaskId = GetStringFieldOrEmpty(Object, TEXT("task_id"));
		Entry.PlanId = GetStringFieldOrEmpty(Object, TEXT("plan_id"));
		Entry.MechanicId = GetStringFieldOrEmpty(Object, TEXT("mechanic_id"));
		Entry.ToolCallId = GetStringFieldOrEmpty(Object, TEXT("tool_call_id"));
		Entry.RelaySessionId = GetStringFieldOrEmpty(Object, TEXT("relay_session_id"));
		Entry.TimestampUtc = GetStringFieldOrEmpty(Object, TEXT("timestamp_utc"));
		Entry.EntryKind = GetStringFieldOrEmpty(Object, TEXT("entry_kind"));
		Entry.Summary = GetStringFieldOrEmpty(Object, TEXT("summary"));
		Entry.SummaryRu = GetStringFieldOrEmpty(Object, TEXT("summary_ru"));
		Entry.TechnicalDetail = GetStringFieldOrEmpty(Object, TEXT("technical_detail"));
		Entry.CurrentAction = GetStringFieldOrEmpty(Object, TEXT("current_action"));
		Entry.CurrentActionRu = GetStringFieldOrEmpty(Object, TEXT("current_action_ru"));
		Entry.CurrentToolName = GetStringFieldOrEmpty(Object, TEXT("current_tool_name"));
		Entry.IterationIndex = GetIntegerFieldOrDefault(Object, TEXT("iteration_index"), 0);
		Entry.ElapsedSeconds = GetDoubleFieldOrDefault(Object, TEXT("elapsed_seconds"), 0.0);
		Entry.HeartbeatAgeSeconds = GetDoubleFieldOrDefault(Object, TEXT("heartbeat_age_seconds"), 0.0);
		Entry.bIsStale = GetBoolFieldOrDefault(Object, TEXT("is_stale"), false);
		ParseOsvayderUERelayTerminalOutcome(GetStringFieldOrEmpty(Object, TEXT("terminal_outcome")), Entry.TerminalOutcome);
		return Entry;
	}

	TSharedRef<FJsonObject> MakeResultJson(const FOsvayderUERelayResult& Result)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), Result.SchemaVersion);
		Root->SetStringField(TEXT("artifact_type"), Result.ArtifactType.IsEmpty() ? TEXT("relay_result") : Result.ArtifactType);
		Root->SetStringField(TEXT("task_id"), Result.TaskId);
		Root->SetStringField(TEXT("plan_id"), Result.PlanId);
		Root->SetStringField(TEXT("workflow_id"), Result.WorkflowId);
		Root->SetStringField(TEXT("current_mechanic_id"), Result.CurrentMechanicId);
		Root->SetStringField(TEXT("current_tool_call_id"), Result.CurrentToolCallId);
		Root->SetStringField(TEXT("relay_session_id"), Result.RelaySessionId);
		Root->SetStringField(TEXT("completed_at_utc"), Result.CompletedAtUtc);
		Root->SetStringField(TEXT("terminal_outcome"), OsvayderUERelayTerminalOutcomeToString(Result.TerminalOutcome));
		Root->SetStringField(TEXT("status"), Result.Status);
		Root->SetStringField(TEXT("origin_prompt_hash"), Result.OriginPromptHash);
		Root->SetStringField(TEXT("summary"), Result.Summary);
		Root->SetStringField(TEXT("summary_ru"), Result.SummaryRu);
		Root->SetStringField(TEXT("technical_detail"), Result.TechnicalDetail);
		Root->SetStringField(TEXT("final_blocker_family"), Result.FinalBlockerFamily);
		Root->SetStringField(TEXT("final_blocker_signature"), Result.FinalBlockerSignature);
		Root->SetStringField(TEXT("plan_status"), Result.PlanStatus);
		Root->SetStringField(TEXT("next_resume_phase"), Result.NextResumePhase);
		Root->SetStringField(TEXT("build_command"), Result.BuildCommand);
		Root->SetStringField(TEXT("build_log_path"), Result.BuildLogPath);
		Root->SetStringField(TEXT("target_proof_status"), Result.TargetProofStatus);
		Root->SetBoolField(TEXT("requires_post_reattach_verification"), Result.bRequiresPostReattachVerification);
		Root->SetArrayField(TEXT("completed_mechanic_ids"), FJsonUtils::StringArrayToJson(Result.CompletedMechanicIds));
		Root->SetArrayField(TEXT("relevant_artifact_paths"), FJsonUtils::StringArrayToJson(Result.RelevantArtifactPaths));
		Root->SetArrayField(TEXT("changed_files"), FJsonUtils::StringArrayToJson(Result.ChangedFiles));
		Root->SetArrayField(TEXT("required_live_checks"), FJsonUtils::StringArrayToJson(Result.RequiredLiveChecks));
		Root->SetStringField(TEXT("relay_progress_path"), Result.RelayProgressPath);
		Root->SetNumberField(TEXT("iterations_used"), Result.IterationsUsed);
		Root->SetNumberField(TEXT("elapsed_seconds"), Result.ElapsedSeconds);
		Root->SetStringField(TEXT("reattach_summary"), Result.ReattachSummary);
		return Root;
	}

	FOsvayderUERelayResult ParseRelayResult(const TSharedPtr<FJsonObject>& Object)
	{
		FOsvayderUERelayResult Result;
		if (!Object.IsValid())
		{
			return Result;
		}

		Result.SchemaVersion = GetIntegerFieldOrDefault(Object, TEXT("schema_version"), RelaySchemaVersion);
		Result.ArtifactType = GetStringFieldOrEmpty(Object, TEXT("artifact_type"));
		Result.TaskId = GetStringFieldOrEmpty(Object, TEXT("task_id"));
		Result.PlanId = GetStringFieldOrEmpty(Object, TEXT("plan_id"));
		Result.WorkflowId = GetStringFieldOrEmpty(Object, TEXT("workflow_id"));
		Result.CurrentMechanicId = GetStringFieldOrEmpty(Object, TEXT("current_mechanic_id"));
		Result.CurrentToolCallId = GetStringFieldOrEmpty(Object, TEXT("current_tool_call_id"));
		Result.RelaySessionId = GetStringFieldOrEmpty(Object, TEXT("relay_session_id"));
		Result.CompletedAtUtc = GetStringFieldOrEmpty(Object, TEXT("completed_at_utc"));
		ParseOsvayderUERelayTerminalOutcome(GetStringFieldOrEmpty(Object, TEXT("terminal_outcome")), Result.TerminalOutcome);
		Result.Status = GetStringFieldOrEmpty(Object, TEXT("status"));
		Result.OriginPromptHash = GetStringFieldOrEmpty(Object, TEXT("origin_prompt_hash"));
		Result.Summary = GetStringFieldOrEmpty(Object, TEXT("summary"));
		Result.SummaryRu = GetStringFieldOrEmpty(Object, TEXT("summary_ru"));
		Result.TechnicalDetail = GetStringFieldOrEmpty(Object, TEXT("technical_detail"));
		Result.FinalBlockerFamily = GetStringFieldOrEmpty(Object, TEXT("final_blocker_family"));
		Result.FinalBlockerSignature = GetStringFieldOrEmpty(Object, TEXT("final_blocker_signature"));
		Result.PlanStatus = GetStringFieldOrEmpty(Object, TEXT("plan_status"));
		Result.NextResumePhase = GetStringFieldOrEmpty(Object, TEXT("next_resume_phase"));
		Result.BuildCommand = GetStringFieldOrEmpty(Object, TEXT("build_command"));
		Result.BuildLogPath = GetStringFieldOrEmpty(Object, TEXT("build_log_path"));
		Result.TargetProofStatus = GetStringFieldOrEmpty(Object, TEXT("target_proof_status"));
		Result.bRequiresPostReattachVerification = GetBoolFieldOrDefault(Object, TEXT("requires_post_reattach_verification"), false);
		Result.CompletedMechanicIds = GetStringArrayFieldOrEmpty(Object, TEXT("completed_mechanic_ids"));
		Result.RelevantArtifactPaths = GetStringArrayFieldOrEmpty(Object, TEXT("relevant_artifact_paths"));
		Result.ChangedFiles = GetStringArrayFieldOrEmpty(Object, TEXT("changed_files"));
		Result.RequiredLiveChecks = GetStringArrayFieldOrEmpty(Object, TEXT("required_live_checks"));
		Result.RelayProgressPath = GetStringFieldOrEmpty(Object, TEXT("relay_progress_path"));
		Result.IterationsUsed = GetIntegerFieldOrDefault(Object, TEXT("iterations_used"), 0);
		Result.ElapsedSeconds = GetDoubleFieldOrDefault(Object, TEXT("elapsed_seconds"), 0.0);
		Result.ReattachSummary = GetStringFieldOrEmpty(Object, TEXT("reattach_summary"));
		return Result;
	}

	bool LoadJsonFile(const FString& Path, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *Path))
		{
			OutError = FString::Printf(TEXT("Could not read relay JSON file: %s"), *Path);
			return false;
		}

		OutObject = FJsonUtils::Parse(JsonText);
		if (!OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("Could not parse relay JSON file: %s"), *Path);
			return false;
		}

		return true;
	}

	bool ValidateJsonFile(const FString& Path, FString& OutValidationError)
	{
		TSharedPtr<FJsonObject> Root;
		return LoadJsonFile(Path, Root, OutValidationError) && Root.IsValid();
	}

	bool LoadManagedJsonFile(
		const FString& PreferredPath,
		const FString& LegacyPath,
		const FString& LogicalName,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError)
	{
		OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
		if (!OsvayderUEStorageMigration::ResolveManagedReadPath(
			PreferredPath,
			LegacyPath,
			LogicalName,
			ValidateJsonFile,
			ManagedRead,
			OutError))
		{
			return false;
		}

		return LoadJsonFile(ManagedRead.ResolvedPath, OutObject, OutError);
	}

	bool ValidateJsonlFile(const FString& Path, FString& OutValidationError)
	{
		FString Jsonl;
		if (!FFileHelper::LoadFileToString(Jsonl, *Path))
		{
			OutValidationError = FString::Printf(TEXT("Could not read relay jsonl file: %s"), *Path);
			return false;
		}

		return true;
	}

	bool LoadManagedJsonlText(
		const FString& PreferredPath,
		const FString& LegacyPath,
		const FString& LogicalName,
		FString& OutJsonl,
		FString& OutResolvedPath,
		FString& OutError)
	{
		OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
		if (!OsvayderUEStorageMigration::ResolveManagedReadPath(
			PreferredPath,
			LegacyPath,
			LogicalName,
			ValidateJsonlFile,
			ManagedRead,
			OutError))
		{
			return false;
		}

		OutResolvedPath = ManagedRead.ResolvedPath;
		if (!FFileHelper::LoadFileToString(OutJsonl, *ManagedRead.ResolvedPath))
		{
			OutError = FString::Printf(TEXT("Could not read relay progress log: %s"), *ManagedRead.ResolvedPath);
			return false;
		}

		return true;
	}

	bool SaveJsonFile(const FString& Path, const TSharedRef<FJsonObject>& Object, FString& OutError)
	{
		if (!EnsureParentDirectoryExists(Path, OutError))
		{
			return false;
		}

		const FString TempPath = Path + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(FJsonUtils::Stringify(Object, true), *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Could not write relay JSON temp file: %s"), *TempPath);
			return false;
		}

		IFileManager::Get().Delete(*Path, false, true, true);
		if (!IFileManager::Get().Move(*Path, *TempPath, true, true, false, false))
		{
			OutError = FString::Printf(TEXT("Could not atomically replace relay JSON file: %s"), *Path);
			IFileManager::Get().Delete(*TempPath, false, true, true);
			return false;
		}

		return true;
	}

	TSharedRef<FJsonObject> MakeMechanicJson(const FOsvayderUEPlanMechanicEntry& Mechanic)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), Mechanic.SchemaVersion);
		Root->SetStringField(TEXT("mechanic_id"), Mechanic.MechanicId);
		Root->SetStringField(TEXT("label"), Mechanic.Label);
		Root->SetStringField(TEXT("label_ru"), Mechanic.LabelRu);
		Root->SetStringField(TEXT("status"), Mechanic.Status);
		Root->SetStringField(TEXT("started_at_utc"), Mechanic.StartedAtUtc);
		Root->SetStringField(TEXT("completed_at_utc"), Mechanic.CompletedAtUtc);
		Root->SetStringField(TEXT("last_summary"), Mechanic.LastSummary);
		Root->SetStringField(TEXT("last_summary_ru"), Mechanic.LastSummaryRu);
		Root->SetBoolField(TEXT("requires_closed_editor"), Mechanic.bRequiresClosedEditor);
		Root->SetBoolField(TEXT("requires_post_reattach_verification"), Mechanic.bRequiresPostReattachVerification);
		return Root;
	}

	FOsvayderUEPlanMechanicEntry ParseMechanicEntry(const TSharedPtr<FJsonObject>& Object)
	{
		FOsvayderUEPlanMechanicEntry Entry;
		if (!Object.IsValid())
		{
			return Entry;
		}

		Entry.SchemaVersion = GetIntegerFieldOrDefault(Object, TEXT("schema_version"), RelaySchemaVersion);
		Entry.MechanicId = GetStringFieldOrEmpty(Object, TEXT("mechanic_id"));
		Entry.Label = GetStringFieldOrEmpty(Object, TEXT("label"));
		Entry.LabelRu = GetStringFieldOrEmpty(Object, TEXT("label_ru"));
		Entry.Status = GetStringFieldOrEmpty(Object, TEXT("status"));
		Entry.StartedAtUtc = GetStringFieldOrEmpty(Object, TEXT("started_at_utc"));
		Entry.CompletedAtUtc = GetStringFieldOrEmpty(Object, TEXT("completed_at_utc"));
		Entry.LastSummary = GetStringFieldOrEmpty(Object, TEXT("last_summary"));
		Entry.LastSummaryRu = GetStringFieldOrEmpty(Object, TEXT("last_summary_ru"));
		Entry.bRequiresClosedEditor = GetBoolFieldOrDefault(Object, TEXT("requires_closed_editor"), false);
		Entry.bRequiresPostReattachVerification = GetBoolFieldOrDefault(Object, TEXT("requires_post_reattach_verification"), false);
		return Entry;
	}

	TSharedRef<FJsonObject> MakeToolCallJson(const FOsvayderUEPlanToolCallEntry& ToolCall)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), ToolCall.SchemaVersion);
		Root->SetStringField(TEXT("tool_call_id"), ToolCall.ToolCallId);
		Root->SetStringField(TEXT("mechanic_id"), ToolCall.MechanicId);
		Root->SetStringField(TEXT("tool_name"), ToolCall.ToolName);
		Root->SetStringField(TEXT("status"), ToolCall.Status);
		Root->SetStringField(TEXT("summary"), ToolCall.Summary);
		Root->SetStringField(TEXT("summary_ru"), ToolCall.SummaryRu);
		Root->SetStringField(TEXT("technical_detail"), ToolCall.TechnicalDetail);
		Root->SetStringField(TEXT("started_at_utc"), ToolCall.StartedAtUtc);
		Root->SetStringField(TEXT("completed_at_utc"), ToolCall.CompletedAtUtc);
		Root->SetStringField(TEXT("result_status"), ToolCall.ResultStatus);
		return Root;
	}

	FOsvayderUEPlanToolCallEntry ParseToolCallEntry(const TSharedPtr<FJsonObject>& Object)
	{
		FOsvayderUEPlanToolCallEntry Entry;
		if (!Object.IsValid())
		{
			return Entry;
		}

		Entry.SchemaVersion = GetIntegerFieldOrDefault(Object, TEXT("schema_version"), RelaySchemaVersion);
		Entry.ToolCallId = GetStringFieldOrEmpty(Object, TEXT("tool_call_id"));
		Entry.MechanicId = GetStringFieldOrEmpty(Object, TEXT("mechanic_id"));
		Entry.ToolName = GetStringFieldOrEmpty(Object, TEXT("tool_name"));
		Entry.Status = GetStringFieldOrEmpty(Object, TEXT("status"));
		Entry.Summary = GetStringFieldOrEmpty(Object, TEXT("summary"));
		Entry.SummaryRu = GetStringFieldOrEmpty(Object, TEXT("summary_ru"));
		Entry.TechnicalDetail = GetStringFieldOrEmpty(Object, TEXT("technical_detail"));
		Entry.StartedAtUtc = GetStringFieldOrEmpty(Object, TEXT("started_at_utc"));
		Entry.CompletedAtUtc = GetStringFieldOrEmpty(Object, TEXT("completed_at_utc"));
		Entry.ResultStatus = GetStringFieldOrEmpty(Object, TEXT("result_status"));
		return Entry;
	}

	TSharedRef<FJsonObject> MakeActivePlanJson(const FOsvayderUEActivePlan& Plan)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), Plan.SchemaVersion);
		Root->SetStringField(TEXT("plan_id"), Plan.PlanId);
		Root->SetStringField(TEXT("reviewer_plan_reference"), Plan.ReviewerPlanReference);
		Root->SetStringField(TEXT("original_user_task"), Plan.OriginalUserTask);
		Root->SetStringField(TEXT("created_at_utc"), Plan.CreatedAtUtc);
		Root->SetStringField(TEXT("updated_at_utc"), Plan.UpdatedAtUtc);
		Root->SetStringField(TEXT("status"), Plan.Status);
		Root->SetStringField(TEXT("result_status"), Plan.ResultStatus);
		Root->SetStringField(TEXT("summary"), Plan.Summary);
		Root->SetStringField(TEXT("summary_ru"), Plan.SummaryRu);
		Root->SetStringField(TEXT("technical_detail"), Plan.TechnicalDetail);
		Root->SetStringField(TEXT("current_mechanic_id"), Plan.CurrentMechanicId);
		Root->SetStringField(TEXT("current_tool_call_id"), Plan.CurrentToolCallId);
		Root->SetStringField(TEXT("current_action"), Plan.CurrentAction);
		Root->SetStringField(TEXT("current_action_ru"), Plan.CurrentActionRu);
		Root->SetStringField(TEXT("current_technical_detail"), Plan.CurrentTechnicalDetail);
		Root->SetStringField(TEXT("resume_hint"), Plan.ResumeHint);
		if (Plan.LaneState.HasAnySignal())
		{
			Root->SetObjectField(TEXT("lane_state"), Plan.LaneState.ToJsonObject());
		}
		Root->SetStringField(TEXT("handoff_policy"), Plan.HandoffPolicy);
		Root->SetStringField(TEXT("hybrid_split_reason"), Plan.HybridSplitReason);
		Root->SetStringField(TEXT("last_completed_tool_call_id"), Plan.LastCompletedToolCallId);
		Root->SetStringField(TEXT("archive_run_tag"), Plan.ArchiveRunTag);
		Root->SetBoolField(TEXT("hybrid_split_triggered"), Plan.bHybridSplitTriggered);
		Root->SetBoolField(TEXT("post_reattach_verification_required"), Plan.bPostReattachVerificationRequired);
		Root->SetBoolField(TEXT("visual_proof_required"), Plan.bVisualProofRequired);
		Root->SetBoolField(TEXT("visual_qa_manifest_required"), Plan.bVisualQaManifestRequired);
		Root->SetStringField(TEXT("visual_proof_requirement"), Plan.VisualProofRequirement);
		Root->SetStringField(TEXT("visual_proof_status"), Plan.VisualProofStatus);
		Root->SetStringField(TEXT("visual_proof_artifact_path"), Plan.VisualProofArtifactPath);
		Root->SetStringField(TEXT("visual_proof_blocker"), Plan.VisualProofBlocker);
		Root->SetStringField(TEXT("visual_qa_manifest_path"), Plan.VisualQaManifestPath);
		Root->SetStringField(TEXT("visual_qa_manifest_verdict"), Plan.VisualQaManifestVerdict);
		Root->SetObjectField(TEXT("settings"), MakeSettingsJson(Plan.Settings));
		Root->SetObjectField(TEXT("compile_proof"), MakeCompileProofJson(Plan.CompileProof));
		if (Plan.FeatureWorkflow.HasAnySignal())
		{
			Root->SetObjectField(TEXT("feature_workflow"), Plan.FeatureWorkflow.ToJsonObject());
		}
		Root->SetArrayField(TEXT("completed_mechanic_ids"), FJsonUtils::StringArrayToJson(Plan.CompletedMechanicIds));
		Root->SetArrayField(TEXT("verification_checklist"), FJsonUtils::StringArrayToJson(Plan.VerificationChecklist));
		Root->SetArrayField(TEXT("visual_reference_artifact_paths"), FJsonUtils::StringArrayToJson(Plan.VisualReferenceArtifactPaths));

		// 632 Task Recovery & Rehydration fields (always emit; absent on old files treated as empty/0 on read).
		Root->SetStringField(TEXT("interruption_detected_at_utc"), Plan.InterruptionDetectedAtUtc);
		Root->SetStringField(TEXT("interruption_reason"), Plan.InterruptionReason);
		Root->SetNumberField(TEXT("interruption_elapsed_seconds"), Plan.InterruptionElapsedSeconds);
		Root->SetStringField(TEXT("user_recovery_choice"), Plan.UserRecoveryChoice);
		Root->SetStringField(TEXT("user_closed_reason"), Plan.UserClosedReason);

		TArray<TSharedPtr<FJsonValue>> MechanicsArray;
		for (const FOsvayderUEPlanMechanicEntry& Entry : Plan.Mechanics)
		{
			MechanicsArray.Add(MakeShared<FJsonValueObject>(MakeMechanicJson(Entry)));
		}
		Root->SetArrayField(TEXT("mechanics"), MechanicsArray);

		TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
		for (const FOsvayderUEPlanToolCallEntry& Entry : Plan.ToolCalls)
		{
			ToolCallsArray.Add(MakeShared<FJsonValueObject>(MakeToolCallJson(Entry)));
		}
		Root->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
		return Root;
	}

	FOsvayderUEActivePlan ParseActivePlan(const TSharedPtr<FJsonObject>& Object)
	{
		FOsvayderUEActivePlan Plan;
		if (!Object.IsValid())
		{
			return Plan;
		}

		Plan.SchemaVersion = GetIntegerFieldOrDefault(Object, TEXT("schema_version"), RelaySchemaVersion);
		Plan.PlanId = GetStringFieldOrEmpty(Object, TEXT("plan_id"));
		Plan.ReviewerPlanReference = GetStringFieldOrEmpty(Object, TEXT("reviewer_plan_reference"));
		Plan.OriginalUserTask = GetStringFieldOrEmpty(Object, TEXT("original_user_task"));
		Plan.CreatedAtUtc = GetStringFieldOrEmpty(Object, TEXT("created_at_utc"));
		Plan.UpdatedAtUtc = GetStringFieldOrEmpty(Object, TEXT("updated_at_utc"));
		Plan.Status = GetStringFieldOrEmpty(Object, TEXT("status"));
		Plan.ResultStatus = GetStringFieldOrEmpty(Object, TEXT("result_status"));
		Plan.Summary = GetStringFieldOrEmpty(Object, TEXT("summary"));
		Plan.SummaryRu = GetStringFieldOrEmpty(Object, TEXT("summary_ru"));
		Plan.TechnicalDetail = GetStringFieldOrEmpty(Object, TEXT("technical_detail"));
		Plan.CurrentMechanicId = GetStringFieldOrEmpty(Object, TEXT("current_mechanic_id"));
		Plan.CurrentToolCallId = GetStringFieldOrEmpty(Object, TEXT("current_tool_call_id"));
		Plan.CurrentAction = GetStringFieldOrEmpty(Object, TEXT("current_action"));
		Plan.CurrentActionRu = GetStringFieldOrEmpty(Object, TEXT("current_action_ru"));
		Plan.CurrentTechnicalDetail = GetStringFieldOrEmpty(Object, TEXT("current_technical_detail"));
		Plan.ResumeHint = GetStringFieldOrEmpty(Object, TEXT("resume_hint"));
		const TSharedPtr<FJsonObject>* LaneStateObject = nullptr;
		if (Object->TryGetObjectField(TEXT("lane_state"), LaneStateObject) && LaneStateObject && LaneStateObject->IsValid())
		{
			Plan.LaneState = FOsvayderUETaskLaneState::FromJsonObject(*LaneStateObject);
		}
		Plan.HandoffPolicy = GetStringFieldOrEmpty(Object, TEXT("handoff_policy"));
		Plan.HybridSplitReason = GetStringFieldOrEmpty(Object, TEXT("hybrid_split_reason"));
		Plan.LastCompletedToolCallId = GetStringFieldOrEmpty(Object, TEXT("last_completed_tool_call_id"));
		Plan.ArchiveRunTag = GetStringFieldOrEmpty(Object, TEXT("archive_run_tag"));
		Plan.bHybridSplitTriggered = GetBoolFieldOrDefault(Object, TEXT("hybrid_split_triggered"), false);
		Plan.bPostReattachVerificationRequired = GetBoolFieldOrDefault(Object, TEXT("post_reattach_verification_required"), false);
		Plan.bVisualProofRequired = GetBoolFieldOrDefault(Object, TEXT("visual_proof_required"), false);
		Plan.bVisualQaManifestRequired = GetBoolFieldOrDefault(Object, TEXT("visual_qa_manifest_required"), false);
		Plan.VisualProofRequirement = GetStringFieldOrEmpty(Object, TEXT("visual_proof_requirement"));
		Plan.VisualProofStatus = GetStringFieldOrEmpty(Object, TEXT("visual_proof_status"));
		Plan.VisualProofArtifactPath = GetStringFieldOrEmpty(Object, TEXT("visual_proof_artifact_path"));
		Plan.VisualProofBlocker = GetStringFieldOrEmpty(Object, TEXT("visual_proof_blocker"));
		Plan.VisualQaManifestPath = GetStringFieldOrEmpty(Object, TEXT("visual_qa_manifest_path"));
		Plan.VisualQaManifestVerdict = GetStringFieldOrEmpty(Object, TEXT("visual_qa_manifest_verdict"));
		Plan.CompletedMechanicIds = GetStringArrayFieldOrEmpty(Object, TEXT("completed_mechanic_ids"));
		Plan.VerificationChecklist = GetStringArrayFieldOrEmpty(Object, TEXT("verification_checklist"));
		Plan.VisualReferenceArtifactPaths = GetStringArrayFieldOrEmpty(Object, TEXT("visual_reference_artifact_paths"));

		// 632 Task Recovery & Rehydration fields (back-compat: missing on old files → empty/0).
		Plan.InterruptionDetectedAtUtc = GetStringFieldOrEmpty(Object, TEXT("interruption_detected_at_utc"));
		Plan.InterruptionReason = GetStringFieldOrEmpty(Object, TEXT("interruption_reason"));
		Plan.InterruptionElapsedSeconds = GetIntegerFieldOrDefault(Object, TEXT("interruption_elapsed_seconds"), 0);
		Plan.UserRecoveryChoice = GetStringFieldOrEmpty(Object, TEXT("user_recovery_choice"));
		Plan.UserClosedReason = GetStringFieldOrEmpty(Object, TEXT("user_closed_reason"));

		const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
		if (Object->TryGetObjectField(TEXT("settings"), SettingsObject) && SettingsObject && SettingsObject->IsValid())
		{
			Plan.Settings = ParseSettingsSnapshot(*SettingsObject);
		}

		const TSharedPtr<FJsonObject>* CompileProofObject = nullptr;
		if (Object->TryGetObjectField(TEXT("compile_proof"), CompileProofObject) && CompileProofObject && CompileProofObject->IsValid())
		{
			Plan.CompileProof = ParseCompileProofState(*CompileProofObject);
		}

		const TSharedPtr<FJsonObject>* FeatureWorkflowObject = nullptr;
		if (Object->TryGetObjectField(TEXT("feature_workflow"), FeatureWorkflowObject) && FeatureWorkflowObject && FeatureWorkflowObject->IsValid())
		{
			Plan.FeatureWorkflow = FAgentFeatureWorkflowState::FromJsonObject(*FeatureWorkflowObject);
		}

		TArray<TSharedPtr<FJsonValue>> MechanicsArray;
		if (FJsonUtils::GetArrayField(Object, TEXT("mechanics"), MechanicsArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : MechanicsArray)
			{
				if (!Value.IsValid())
				{
					continue;
				}

				const TSharedPtr<FJsonObject> EntryObject = Value->AsObject();
				if (EntryObject.IsValid())
				{
					Plan.Mechanics.Add(ParseMechanicEntry(EntryObject));
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
		if (FJsonUtils::GetArrayField(Object, TEXT("tool_calls"), ToolCallsArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : ToolCallsArray)
			{
				if (!Value.IsValid())
				{
					continue;
				}

				const TSharedPtr<FJsonObject> EntryObject = Value->AsObject();
				if (EntryObject.IsValid())
				{
					Plan.ToolCalls.Add(ParseToolCallEntry(EntryObject));
				}
			}
		}

		return Plan;
	}

	FString MakeArchiveSafeFileTag(const FString& Value)
	{
		FString Safe = Value;
		for (const TCHAR Character : { TEXT('\\'), TEXT('/'), TEXT(':'), TEXT('*'), TEXT('?'), TEXT('"'), TEXT('<'), TEXT('>'), TEXT('|'), TEXT(' ') })
		{
			Safe.ReplaceCharInline(Character, TEXT('_'));
		}
		return Safe;
	}

	bool CopyFileIfPresent(const FString& SourcePath, const FString& DestinationPath, FString& OutError)
	{
		if (!IFileManager::Get().FileExists(*SourcePath))
		{
			return true;
		}

		if (!EnsureParentDirectoryExists(DestinationPath, OutError))
		{
			return false;
		}

		if (IFileManager::Get().Copy(*DestinationPath, *SourcePath, true, true) != COPY_OK)
		{
			OutError = FString::Printf(TEXT("Could not archive relay artifact %s to %s"), *SourcePath, *DestinationPath);
			return false;
		}

		return true;
	}

	bool PruneDirectoryFilesByCount(const FString& Directory, const int32 MaxFiles, FString& OutError)
	{
		if (MaxFiles <= 0 || !IFileManager::Get().DirectoryExists(*Directory))
		{
			return true;
		}

		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.*"), true, false, false);
		if (Files.Num() <= MaxFiles)
		{
			return true;
		}

		Files.Sort([](const FString& A, const FString& B)
		{
			return IFileManager::Get().GetTimeStamp(*A) < IFileManager::Get().GetTimeStamp(*B);
		});

		const int32 DeleteCount = Files.Num() - MaxFiles;
		for (int32 Index = 0; Index < DeleteCount; ++Index)
		{
			if (!IFileManager::Get().Delete(*Files[Index], false, true, true))
			{
				OutError = FString::Printf(TEXT("Could not prune archived relay artifact: %s"), *Files[Index]);
				return false;
			}
		}

		return true;
	}

	FString GetOsvayderUEPluginBaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("OsvayderUE"));
		if (Plugin.IsValid())
		{
			return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		}

		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("OsvayderUE")));
	}
}

const TCHAR* OsvayderUERelayTerminalOutcomeToString(EOsvayderUERelayTerminalOutcome Outcome)
{
	switch (Outcome)
	{
	case EOsvayderUERelayTerminalOutcome::Success:
		return TEXT("success");
	case EOsvayderUERelayTerminalOutcome::TerminalFail:
		return TEXT("terminal_fail");
	case EOsvayderUERelayTerminalOutcome::Cancelled:
		return TEXT("cancelled");
	default:
		return TEXT("none");
	}
}

bool ParseOsvayderUERelayTerminalOutcome(const FString& Value, EOsvayderUERelayTerminalOutcome& OutOutcome)
{
	if (Value.Equals(TEXT("success"), ESearchCase::IgnoreCase))
	{
		OutOutcome = EOsvayderUERelayTerminalOutcome::Success;
		return true;
	}

	if (Value.Equals(TEXT("terminal_fail"), ESearchCase::IgnoreCase))
	{
		OutOutcome = EOsvayderUERelayTerminalOutcome::TerminalFail;
		return true;
	}

	if (Value.Equals(TEXT("cancelled"), ESearchCase::IgnoreCase))
	{
		OutOutcome = EOsvayderUERelayTerminalOutcome::Cancelled;
		return true;
	}

	if (Value.Equals(TEXT("none"), ESearchCase::IgnoreCase) || Value.IsEmpty())
	{
		OutOutcome = EOsvayderUERelayTerminalOutcome::None;
		return true;
	}

	return false;
}

bool FOsvayderUERelayAgentManager::LoadLatestProgressEntryForTask(
	const FString& TaskId,
	FOsvayderUERelayProgressEntry& OutEntry,
	FString& OutError)
{
	OutError.Reset();
	if (TaskId.IsEmpty())
	{
		OutError = TEXT("Relay progress task filter requires a non-empty task id.");
		return false;
	}

	FString Jsonl;
	FString ResolvedProgressPath;
	if (!LoadManagedJsonlText(
		GetRelayProgressPath(),
		GetLegacyRelayPath(TEXT("relay_progress.jsonl")),
		TEXT("relay_progress_log"),
		Jsonl,
		ResolvedProgressPath,
		OutError))
	{
		return false;
	}

	TArray<FString> Lines;
	Jsonl.ParseIntoArrayLines(Lines, false);
	for (int32 Index = Lines.Num() - 1; Index >= 0; --Index)
	{
		const FString Candidate = Lines[Index].TrimStartAndEnd();
		if (Candidate.IsEmpty())
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Object = FJsonUtils::Parse(Candidate);
		if (!Object.IsValid())
		{
			continue;
		}

		const FOsvayderUERelayProgressEntry Entry = ParseProgressEntry(Object);
		if (!Entry.TaskId.IsEmpty() && Entry.TaskId.Equals(TaskId, ESearchCase::CaseSensitive))
		{
			OutEntry = Entry;
			return true;
		}
	}

	OutError = FString::Printf(
		TEXT("Relay progress log has no parseable entry for task %s: %s"),
		*TaskId,
		*ResolvedProgressPath);
	return false;
}

FString FOsvayderUERelayAgentManager::GetRelayRootDir()
{
	return FOsvayderUERestartSurvivalManager::GetStateRootDir();
}

FString FOsvayderUERelayAgentManager::GetActivePlanPath()
{
	return FPaths::Combine(GetRelayRootDir(), ActivePlanFileName);
}

FString FOsvayderUERelayAgentManager::GetPlanArchiveDir()
{
	return FPaths::Combine(GetRelayRootDir(), PlanArchiveDirName);
}

FString FOsvayderUERelayAgentManager::GetRelayArchiveDir()
{
	return FPaths::Combine(GetRelayRootDir(), RelayArchiveDirName);
}

FString FOsvayderUERelayAgentManager::GetHandoffContextPath()
{
	return FPaths::Combine(GetRelayRootDir(), TEXT("handoff_context.json"));
}

FString FOsvayderUERelayAgentManager::GetRelayProgressPath()
{
	return FPaths::Combine(GetRelayRootDir(), TEXT("relay_progress.jsonl"));
}

FString FOsvayderUERelayAgentManager::GetRelayResultPath()
{
	return FPaths::Combine(GetRelayRootDir(), TEXT("relay_result.json"));
}

FString FOsvayderUERelayAgentManager::GetRelayCancelRequestPath()
{
	return FPaths::Combine(GetRelayRootDir(), TEXT("relay_cancel_request.json"));
}

FString FOsvayderUERelayAgentManager::GetRelayAgentScriptPath()
{
	return NormalizeFilePath(FPaths::Combine(GetOsvayderUEPluginBaseDir(), TEXT("Script"), RelayAgentScriptName));
}

bool FOsvayderUERelayAgentManager::LoadActivePlan(FOsvayderUEActivePlan& OutPlan, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	if (!LoadManagedJsonFile(
		GetActivePlanPath(),
		GetLegacyRelayPath(ActivePlanFileName),
		TEXT("relay_active_plan"),
		Root,
		OutError))
	{
		return false;
	}

	OutPlan = ParseActivePlan(Root);
	return true;
}

namespace
{
	// 619 P3 Fix #3 helper: synthesize a minimal active plan so the
	// post-reattach continuation flow can proceed when active_plan.json is
	// absent. Mirrors the shape of BuildDefaultActivePlan (widget-side) but
	// locally-defined here because RelayAgent.cpp is the single source of
	// truth for plan-schema semantics. Keeps the test surface observable
	// without the widget dependency.
	FOsvayderUEActivePlan BuildFallbackPlan(const FString& OriginalUserTask, const FString& ResumeHintOverride)
	{
		FOsvayderUEActivePlan Plan;
		Plan.SchemaVersion = 1;
		Plan.PlanId = FString::Printf(TEXT("plan_fallback_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Plan.ReviewerPlanReference = TEXT("post_reattach_state_json_fallback_plan_v1");
		Plan.OriginalUserTask = OriginalUserTask;
		Plan.CreatedAtUtc = FDateTime::UtcNow().ToIso8601();
		Plan.UpdatedAtUtc = Plan.CreatedAtUtc;
		Plan.Status = TEXT("active");
		Plan.ResultStatus = TEXT("incomplete");
		Plan.Summary = TEXT("Synthesized fallback plan after active_plan.json was not recoverable post-reattach.");
		Plan.SummaryRu = TEXT("Synthesized fallback plan after active_plan.json was not recoverable post-reattach.");
		Plan.TechnicalDetail = TEXT("Agent 1 recovered the continuation prompt from state.json.post_reattach_completion_text; active_plan.json was absent.");
		Plan.CurrentMechanicId = TEXT("inspect_current_state");
		Plan.CurrentAction = TEXT("persist_active_plan");
		Plan.CurrentActionRu = TEXT("persist_active_plan");
		Plan.CurrentTechnicalDetail = TEXT("Synthesizing active_plan.json from the restart_survival state.json fallback.");
		Plan.ResumeHint = ResumeHintOverride.IsEmpty()
			? TEXT("Resume the exact continuation prompt embedded in the restart-survival state.")
			: ResumeHintOverride;
		Plan.LaneState.CurrentLane = TEXT("live_editor");
		Plan.LaneState.TransitionKind = TEXT("none");
		Plan.LaneState.TransitionState = TEXT("steady");
		Plan.LaneState.ContinuityPlanId = Plan.PlanId;
		Plan.LaneState.ContinuationIntent = Plan.ResumeHint;
		Plan.LaneState.ExpectedReturnCondition =
			TEXT("Finish the current task in the editor unless a bounded closed-editor continuation is armed.");
		Plan.HandoffPolicy = TEXT("full_batch_default");
		Plan.Settings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
		Plan.Mechanics = {
			{ 1, TEXT("inspect_current_state"), TEXT("Inspect current state"), TEXT("Inspect current state"), TEXT("pending") },
			{ 1, TEXT("perform_bounded_work"), TEXT("Perform bounded work"), TEXT("Perform bounded work"), TEXT("pending") },
			{ 1, TEXT("verify_and_report"), TEXT("Verify and report"), TEXT("Verify and report"), TEXT("pending") }
		};
		Plan.VerificationChecklist = {
			TEXT("Open the affected project or asset."),
			TEXT("Trigger the updated behavior once."),
			TEXT("Confirm the observed result matches the final summary.")
		};
		return Plan;
	}
}

bool FOsvayderUERelayAgentManager::LoadActivePlanWithFallback(
	const FString& Prompt,
	FOsvayderUEActivePlan& OutPlan,
	EActivePlanFallbackSource& OutSource,
	FString& OutError)
{
	// Path 1 (preferred, back-compat): active_plan.json on disk.
	FString ActivePlanLoadError;
	if (LoadActivePlan(OutPlan, ActivePlanLoadError))
	{
		OutSource = EActivePlanFallbackSource::ActivePlanJson;
		OutError.Reset();
		return true;
	}

	// Path 2 (new fallback): state.json.post_reattach_completion_text.
	FOsvayderUERestartSurvivalState State;
	FString StateLoadError;
	const bool bStateLoaded = FOsvayderUERestartSurvivalManager::LoadState(State, StateLoadError);
	if (bStateLoaded && !State.PostReattachCompletionText.IsEmpty())
	{
		const FString SeedTask = Prompt.IsEmpty() ? State.PostReattachCompletionText : Prompt;
		OutPlan = BuildFallbackPlan(SeedTask, FString());
		if (State.LaneState.HasAnySignal())
		{
			OutPlan.LaneState = State.LaneState;
			OutPlan.LaneState.ContinuityPlanId = OutPlan.PlanId;
			OutPlan.LaneState.ContinuationIntent = OutPlan.ResumeHint;
		}
		OutSource = EActivePlanFallbackSource::RestartSurvivalStateJsonFallback;
		OutError.Reset();
		UE_LOG(LogOsvayderUE, Display,
			TEXT("OsvayderUERelayAgent: active_plan.json absent; using state.json.post_reattach_completion_text fallback (expected for certain supervisor branches)."));
		return true;
	}

	// Path 3 (graceful warning): neither recoverable. Synthesize a default
	// plan for `Prompt` and return true so the caller does NOT route through
	// the [failed] UI surface. The widget sees "fresh session" semantics,
	// which is correct when no continuation is recoverable.
	OutPlan = BuildFallbackPlan(Prompt, TEXT("No continuation plan recoverable; starting a fresh session."));
	OutSource = EActivePlanFallbackSource::NoneRecoverable;
	OutError.Reset();
	UE_LOG(LogOsvayderUE, Warning,
		TEXT("OsvayderUERelayAgent: No continuation plan recoverable; reverting to fresh session. (active_plan.json missing; state.json %s)"),
		bStateLoaded ? TEXT("loaded but has no post_reattach_completion_text") : TEXT("also missing or unreadable"));
	return true;
}

bool FOsvayderUERelayAgentManager::SaveActivePlan(const FOsvayderUEActivePlan& Plan, FString& OutError)
{
	return SavePlanToPath(GetActivePlanPath(), Plan, OutError);
}

bool FOsvayderUERelayAgentManager::DeleteActivePlan(FString& OutError)
{
	return DeleteManagedRelayArtifact(
		GetActivePlanPath(),
		GetLegacyRelayPath(ActivePlanFileName),
		TEXT("relay_active_plan"),
		OutError);
}

bool FOsvayderUERelayAgentManager::LoadPlanFromPath(
	const FString& PlanPath,
	FOsvayderUEActivePlan& OutPlan,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	if (!LoadJsonFile(PlanPath, Root, OutError))
	{
		return false;
	}

	OutPlan = ParseActivePlan(Root);
	return true;
}

bool FOsvayderUERelayAgentManager::SavePlanToPath(
	const FString& PlanPath,
	const FOsvayderUEActivePlan& Plan,
	FString& OutError)
{
	return SaveJsonFile(PlanPath, MakeActivePlanJson(Plan), OutError);
}

bool FOsvayderUERelayAgentManager::LoadProgressEntriesForTask(
	const FString& TaskId,
	TArray<FOsvayderUERelayProgressEntry>& OutEntries,
	FString& OutError)
{
	OutEntries.Reset();
	if (TaskId.IsEmpty())
	{
		OutError = TEXT("Relay progress task filter requires a non-empty task id.");
		return false;
	}

	FString Jsonl;
	FString ResolvedProgressPath;
	if (!LoadManagedJsonlText(
		GetRelayProgressPath(),
		GetLegacyRelayPath(TEXT("relay_progress.jsonl")),
		TEXT("relay_progress_log"),
		Jsonl,
		ResolvedProgressPath,
		OutError))
	{
		return false;
	}

	TArray<FString> Lines;
	Jsonl.ParseIntoArrayLines(Lines, false);
	for (const FString& RawLine : Lines)
	{
		const FString Candidate = RawLine.TrimStartAndEnd();
		if (Candidate.IsEmpty())
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Object = FJsonUtils::Parse(Candidate);
		if (!Object.IsValid())
		{
			continue;
		}

		const FOsvayderUERelayProgressEntry Entry = ParseProgressEntry(Object);
		if (!Entry.TaskId.IsEmpty() && Entry.TaskId.Equals(TaskId, ESearchCase::CaseSensitive))
		{
			OutEntries.Add(Entry);
		}
	}

	if (OutEntries.Num() == 0)
	{
		OutError = FString::Printf(TEXT("Relay progress log has no parseable entries for task %s: %s"), *TaskId, *ResolvedProgressPath);
		return false;
	}

	return true;
}

bool FOsvayderUERelayAgentManager::ArchiveTerminalArtifacts(
	const FOsvayderUEActivePlan& Plan,
	TArray<FString>& OutArchivedPaths,
	FString& OutError)
{
	OutArchivedPaths.Reset();

	const TArray<TPair<FString, FString>> PreferredLegacyHydrationPairs = {
		TPair<FString, FString>(GetActivePlanPath(), GetLegacyRelayPath(ActivePlanFileName)),
		TPair<FString, FString>(GetRelayProgressPath(), GetLegacyRelayPath(TEXT("relay_progress.jsonl"))),
		TPair<FString, FString>(GetRelayResultPath(), GetLegacyRelayPath(TEXT("relay_result.json"))),
		TPair<FString, FString>(GetHandoffContextPath(), GetLegacyRelayPath(TEXT("handoff_context.json"))),
		TPair<FString, FString>(GetRelayCancelRequestPath(), GetLegacyRelayPath(TEXT("relay_cancel_request.json")))
	};

	for (const TPair<FString, FString>& Pair : PreferredLegacyHydrationPairs)
	{
		bool bHydrated = false;
		if (!OsvayderUEStorageMigration::EnsurePreferredFileHydrated(
			Pair.Key,
			Pair.Value,
			TEXT("relay_archive_source"),
			[](const FString& CandidatePath, FString& OutValidationError)
			{
				if (CandidatePath.EndsWith(TEXT(".jsonl"), ESearchCase::IgnoreCase))
				{
					return ValidateJsonlFile(CandidatePath, OutValidationError);
				}

				return ValidateJsonFile(CandidatePath, OutValidationError);
			},
			bHydrated,
			OutError))
		{
			return false;
		}
	}

	const FString EffectiveTaskId = !Plan.PlanId.IsEmpty() ? Plan.PlanId : TEXT("plan");
	const FString EffectiveRunTag = !Plan.ArchiveRunTag.IsEmpty()
		? Plan.ArchiveRunTag
		: FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
	const FString SafePrefix = MakeArchiveSafeFileTag(FString::Printf(TEXT("%s-%s"), *EffectiveRunTag, *EffectiveTaskId));

	const FString PlanArchivePath = FPaths::Combine(GetPlanArchiveDir(), SafePrefix + TEXT(".active_plan.json"));
	if (!CopyFileIfPresent(GetActivePlanPath(), PlanArchivePath, OutError))
	{
		return false;
	}
	if (IFileManager::Get().FileExists(*PlanArchivePath))
	{
		OutArchivedPaths.Add(PlanArchivePath);
	}

	const TArray<TPair<FString, FString>> RelayArtifacts = {
		TPair<FString, FString>(GetRelayProgressPath(), SafePrefix + TEXT(".relay_progress.jsonl")),
		TPair<FString, FString>(GetRelayResultPath(), SafePrefix + TEXT(".relay_result.json")),
		TPair<FString, FString>(GetHandoffContextPath(), SafePrefix + TEXT(".handoff_context.json")),
		TPair<FString, FString>(GetRelayCancelRequestPath(), SafePrefix + TEXT(".relay_cancel_request.json"))
	};

	for (const TPair<FString, FString>& Artifact : RelayArtifacts)
	{
		const FString DestinationPath = FPaths::Combine(GetRelayArchiveDir(), Artifact.Value);
		if (!CopyFileIfPresent(Artifact.Key, DestinationPath, OutError))
		{
			return false;
		}

		if (IFileManager::Get().FileExists(*DestinationPath))
		{
			OutArchivedPaths.Add(DestinationPath);
		}
	}

	if (!DeleteActivePlan(OutError))
	{
		return false;
	}

	if (!DeleteProgressLog(OutError))
	{
		return false;
	}

	if (!DeleteRelayResult(OutError))
	{
		return false;
	}

	if (!DeleteHandoffContext(OutError))
	{
		return false;
	}

	if (!DeleteCancelRequest(OutError))
	{
		return false;
	}

	return PruneArchivedArtifacts(DefaultArchiveRetention, DefaultArchiveRetention, OutError);
}

bool FOsvayderUERelayAgentManager::PruneArchivedArtifacts(
	const int32 MaxPlanSnapshots,
	const int32 MaxRelayBundles,
	FString& OutError)
{
	return PruneDirectoryFilesByCount(GetPlanArchiveDir(), MaxPlanSnapshots, OutError)
		&& PruneDirectoryFilesByCount(GetRelayArchiveDir(), MaxRelayBundles, OutError);
}

bool FOsvayderUERelayAgentManager::LoadHandoffContext(FOsvayderUERelayHandoffContext& OutContext, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	if (!LoadManagedJsonFile(
		GetHandoffContextPath(),
		GetLegacyRelayPath(TEXT("handoff_context.json")),
		TEXT("relay_handoff_context"),
		Root,
		OutError))
	{
		return false;
	}

	OutContext = ParseHandoffContext(Root);
	return true;
}

bool FOsvayderUERelayAgentManager::SaveHandoffContext(const FOsvayderUERelayHandoffContext& Context, FString& OutError)
{
	return SaveJsonFile(GetHandoffContextPath(), MakeHandoffJson(Context), OutError);
}

bool FOsvayderUERelayAgentManager::DeleteHandoffContext(FString& OutError)
{
	return DeleteManagedRelayArtifact(
		GetHandoffContextPath(),
		GetLegacyRelayPath(TEXT("handoff_context.json")),
		TEXT("relay_handoff_context"),
		OutError);
}

bool FOsvayderUERelayAgentManager::AppendProgressEntry(const FOsvayderUERelayProgressEntry& Entry, FString& OutError)
{
	const FString Path = GetRelayProgressPath();
	bool bHydrated = false;
	if (!OsvayderUEStorageMigration::EnsurePreferredFileHydrated(
		Path,
		GetLegacyRelayPath(TEXT("relay_progress.jsonl")),
		TEXT("relay_progress_log"),
		ValidateJsonlFile,
		bHydrated,
		OutError))
	{
		return false;
	}

	if (!EnsureParentDirectoryExists(Path, OutError))
	{
		return false;
	}

	FString Serialized = FJsonUtils::Stringify(MakeProgressJson(Entry), false);
	Serialized += LINE_TERMINATOR;
	if (!FFileHelper::SaveStringToFile(
		Serialized,
		*Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(),
		FILEWRITE_Append))
	{
		OutError = FString::Printf(TEXT("Could not append relay progress entry: %s"), *Path);
		return false;
	}

	return true;
}

bool FOsvayderUERelayAgentManager::LoadLatestProgressEntry(FOsvayderUERelayProgressEntry& OutEntry, FString& OutError)
{
	FString Jsonl;
	FString ResolvedProgressPath;
	if (!LoadManagedJsonlText(
		GetRelayProgressPath(),
		GetLegacyRelayPath(TEXT("relay_progress.jsonl")),
		TEXT("relay_progress_log"),
		Jsonl,
		ResolvedProgressPath,
		OutError))
	{
		return false;
	}

	TArray<FString> Lines;
	Jsonl.ParseIntoArrayLines(Lines, false);
	for (int32 Index = Lines.Num() - 1; Index >= 0; --Index)
	{
		const FString Candidate = Lines[Index].TrimStartAndEnd();
		if (Candidate.IsEmpty())
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Object = FJsonUtils::Parse(Candidate);
		if (!Object.IsValid())
		{
			continue;
		}

		OutEntry = ParseProgressEntry(Object);
		return true;
	}

	OutError = FString::Printf(TEXT("Relay progress log has no parseable entries: %s"), *ResolvedProgressPath);
	return false;
}

bool FOsvayderUERelayAgentManager::DeleteProgressLog(FString& OutError)
{
	return DeleteManagedRelayArtifact(
		GetRelayProgressPath(),
		GetLegacyRelayPath(TEXT("relay_progress.jsonl")),
		TEXT("relay_progress_log"),
		OutError);
}

bool FOsvayderUERelayAgentManager::LoadRelayResult(FOsvayderUERelayResult& OutResult, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	if (!LoadManagedJsonFile(
		GetRelayResultPath(),
		GetLegacyRelayPath(TEXT("relay_result.json")),
		TEXT("relay_result"),
		Root,
		OutError))
	{
		return false;
	}

	OutResult = ParseRelayResult(Root);
	return true;
}

bool FOsvayderUERelayAgentManager::LoadRelayResultForTask(
	const FString& TaskId,
	FOsvayderUERelayResult& OutResult,
	FString& OutError)
{
	if (!LoadRelayResult(OutResult, OutError))
	{
		return false;
	}

	if (TaskId.IsEmpty())
	{
		OutError = TEXT("Relay result task filter requires a non-empty task id.");
		return false;
	}

	if (!OutResult.TaskId.IsEmpty() && OutResult.TaskId.Equals(TaskId, ESearchCase::CaseSensitive))
	{
		return true;
	}

	OutError = FString::Printf(
		TEXT("Relay result at %s does not belong to task %s."),
		*GetRelayResultPath(),
		*TaskId);
	return false;
}

bool FOsvayderUERelayAgentManager::SaveRelayResult(const FOsvayderUERelayResult& Result, FString& OutError)
{
	return SaveJsonFile(GetRelayResultPath(), MakeResultJson(Result), OutError);
}

bool FOsvayderUERelayAgentManager::DeleteRelayResult(FString& OutError)
{
	return DeleteManagedRelayArtifact(
		GetRelayResultPath(),
		GetLegacyRelayPath(TEXT("relay_result.json")),
		TEXT("relay_result"),
		OutError);
}

bool FOsvayderUERelayAgentManager::HasCancelRequest()
{
	FString ResolveError;
	OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
	return OsvayderUEStorageMigration::ResolveManagedReadPath(
		GetRelayCancelRequestPath(),
		GetLegacyRelayPath(TEXT("relay_cancel_request.json")),
		TEXT("relay_cancel_request"),
		ValidateJsonFile,
		ManagedRead,
		ResolveError);
}

bool FOsvayderUERelayAgentManager::WriteCancelRequest(const FString& Reason, FString& OutError)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), RelaySchemaVersion);
	Root->SetStringField(TEXT("requested_at_utc"), FDateTime::UtcNow().ToIso8601());
	Root->SetStringField(TEXT("reason"), Reason);
	return SaveJsonFile(GetRelayCancelRequestPath(), Root, OutError);
}

bool FOsvayderUERelayAgentManager::DeleteCancelRequest(FString& OutError)
{
	return DeleteManagedRelayArtifact(
		GetRelayCancelRequestPath(),
		GetLegacyRelayPath(TEXT("relay_cancel_request.json")),
		TEXT("relay_cancel_request"),
		OutError);
}

FOsvayderUERelaySettingsSnapshot FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot()
{
	FOsvayderUERelaySettingsSnapshot Snapshot;
	Snapshot.SchemaVersion = RelaySchemaVersion;
	Snapshot.Backend = TEXT("CodexCli");
	Snapshot.BackendDisplayName = FCodexCliRunner().GetBackendDisplayName();

	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	if (Settings)
	{
		Snapshot.Model = Settings->GetConfiguredCodexModel();
		Snapshot.Profile = Settings->GetEffectiveCodexProfileLabel();
		Snapshot.WorkMode = Settings->GetConfiguredCodexWorkModeName();
		Snapshot.ReasoningEffort = Settings->GetConfiguredCodexReasoningEffortName();
		Snapshot.Verbosity = Settings->GetConfiguredCodexVerbosityName();
		Snapshot.RequestedSpeedMode = Settings->GetConfiguredCodexSpeedModeName();
		Snapshot.AuthMode = Settings->GetConfiguredCodexAuthModeName();
		Snapshot.bPersistentAppServerEnabled = Settings->ShouldUsePersistentCodexAppServer();
		Snapshot.bClearProxyEnvForExec = Settings->ShouldClearProxyEnvForCodexExec();
		Snapshot.bHasExplicitCodexHomeOverride = Settings->HasExplicitCodexHomeOverride();
	}

	Snapshot.EffectiveSpeedMode = FCodexCliRunner::GetEffectiveSpeedModeName();
	Snapshot.SpeedSupport = FCodexCliRunner::GetSpeedModeSupportLabel();
	Snapshot.AuthPath = FCodexCliRunner::GetEffectiveAuthEntryPath();
	Snapshot.AuthOwnership = FCodexCliRunner::GetEffectiveAuthOwnershipModel();
	Snapshot.CodexHomePath = FCodexCliRunner::GetConfiguredCodexHomePath();
	Snapshot.CodexHomeResolutionSource = FCodexCliRunner::GetConfiguredCodexHomeResolutionSource();
	Snapshot.ExecutionTransport = FCodexCliRunner::ShouldUsePersistentConversationTransport()
		? TEXT("persistent_app_server")
		: TEXT("process_exec");
	return Snapshot;
}
