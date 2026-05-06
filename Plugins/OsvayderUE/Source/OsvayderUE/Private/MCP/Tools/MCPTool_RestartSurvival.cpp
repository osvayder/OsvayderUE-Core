// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_RestartSurvival.h"
#include "OsvayderSubsystem.h"
#include "CodexCliRunner.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "OsvayderUERestartSurvival.h"

namespace
{
	const TCHAR* PrepareTaskContinuationHandoffOperation = TEXT("prepare_task_continuation_handoff");

	FString MakePreparedRequestId()
	{
		return FString::Printf(TEXT("request_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	FString MakeAutonomousTaskId(const FString& ProviderSessionId)
	{
		FString SessionHint = ProviderSessionId;
		SessionHint.ReplaceInline(TEXT("-"), TEXT(""));
		SessionHint.ReplaceInline(TEXT("{"), TEXT(""));
		SessionHint.ReplaceInline(TEXT("}"), TEXT(""));
		if (SessionHint.IsEmpty())
		{
			SessionHint = TEXT("current");
		}

		SessionHint = SessionHint.Left(8);
		return FString::Printf(
			TEXT("task_autonomous_%s_%s"),
			*SessionHint,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	FString MakeSessionIdFromTaskId(const FString& TaskId)
	{
		return FString::Printf(TEXT("restart_survival_%s"), *TaskId);
	}

	FString BuildAutonomousClosedEditorPostReattachCompletionText(
		const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request)
	{
		FString SourcePath = Request.AutosaveSourcePath;
		FString TargetPath = Request.TargetPath;
		FString BackupPath = Request.BackupPath;
		FString FileWriteSourcePath = Request.FileWriteSourcePath;
		FString FileWriteTargetPath = Request.FileWriteTargetPath;
		FString FileWriteBackupPath = Request.FileWriteBackupPath;
		FPaths::MakeStandardFilename(SourcePath);
		FPaths::MakeStandardFilename(TargetPath);
		FPaths::MakeStandardFilename(BackupPath);
		FPaths::MakeStandardFilename(FileWriteSourcePath);
		FPaths::MakeStandardFilename(FileWriteTargetPath);
		FPaths::MakeStandardFilename(FileWriteBackupPath);

		FString Prompt =
			TEXT("Continue the same ordinary bounded project-local task after restart-survival reattach.\n")
			TEXT("Detached closed-editor restore/build work already completed while Unreal was closed.\n")
			TEXT("Do not call `restart_survival` again, do not close Unreal again, and do not use Stop-Process on UnrealEditor.\n")
			TEXT("Continue only in the reopened editor and verify the bounded result truthfully from project-local state and artifacts instead of rerunning closed-editor work.");

		if (Request.bRestoreEnabled
			&& !SourcePath.IsEmpty()
			&& !TargetPath.IsEmpty()
			&& !BackupPath.IsEmpty())
		{
			Prompt += FString::Printf(
				TEXT("\nThe detached restore already copied \"%s\" to \"%s\" after first saving the previous target content to \"%s\".")
				TEXT("\nVerify that \"%s\" now matches \"%s\" exactly and that \"%s\" preserves the previous target content."),
				*SourcePath,
				*TargetPath,
				*BackupPath,
				*TargetPath,
				*SourcePath,
				*BackupPath);
		}
		else if (Request.bFileWriteEnabled
			&& !FileWriteSourcePath.IsEmpty()
			&& !FileWriteTargetPath.IsEmpty())
		{
			if (!FileWriteBackupPath.IsEmpty())
			{
				Prompt += FString::Printf(
					TEXT("\nThe detached exact file write already copied \"%s\" to \"%s\" after first saving the previous target content to \"%s\".")
					TEXT("\nVerify that \"%s\" now matches \"%s\" exactly and that \"%s\" preserves the previous target content."),
					*FileWriteSourcePath,
					*FileWriteTargetPath,
					*FileWriteBackupPath,
					*FileWriteTargetPath,
					*FileWriteSourcePath,
					*FileWriteBackupPath);
			}
			else
			{
				Prompt += FString::Printf(
					TEXT("\nThe detached exact file write already copied \"%s\" to \"%s\".")
					TEXT("\nVerify that \"%s\" now matches \"%s\" exactly."),
					*FileWriteSourcePath,
					*FileWriteTargetPath,
					*FileWriteTargetPath,
					*FileWriteSourcePath);
			}
		}

		const FString ExactReplyMarker = TEXT("reply with exactly:");
		const int32 ExactReplyMarkerIndex = Request.ContinuationIntentPrompt.Find(ExactReplyMarker, ESearchCase::IgnoreCase);
		if (ExactReplyMarkerIndex != INDEX_NONE)
		{
			FString ExactReplyText = Request.ContinuationIntentPrompt.Mid(ExactReplyMarkerIndex + ExactReplyMarker.Len()).TrimStartAndEnd();
			while (ExactReplyText.EndsWith(TEXT(".")) || ExactReplyText.EndsWith(TEXT("\n")) || ExactReplyText.EndsWith(TEXT("\r")))
			{
				ExactReplyText.LeftChopInline(1, EAllowShrinking::No);
				ExactReplyText = ExactReplyText.TrimStartAndEnd();
			}

			if (!ExactReplyText.IsEmpty())
			{
				Prompt += FString::Printf(
					TEXT("\nWhen the verification is complete, reply with exactly: %s"),
					*ExactReplyText);
				return Prompt;
			}
		}

		Prompt += TEXT("\nWhen the verification is complete, continue the same task and respond concisely.");
		return Prompt;
	}
}

FMCPToolInfo FMCPTool_RestartSurvival::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("restart_survival");
	// 619 P2: description rewrite per spec Fix #2. Framing changed from
	// advisory ("Use this only when...") to gating ("ESCALATION PATH ONLY.
	// Do not call preemptively.") with explicit preconditions naming
	// livecoding_compile as the first-try path. This description is the
	// primary mechanism steering Codex/Claude tool-selection priors away
	// from preemptive restart_survival for simple .cpp body edits.
	// Do NOT paraphrase. Encoding: em-dash replaced with `--` per Russian-
	// locale build hygiene; newlines encoded as `\n\n` paragraph breaks.
	Info.Description = TEXT(
		"**ESCALATION PATH ONLY. Do not call preemptively.**\n\n"
		"Hand off the current task to the closed-editor task owner (Agent 2 supervisor). The supervisor closes the running editor, rebuilds via UBT, relaunches, and reattaches the session. Total wall time: 60-180 seconds.\n\n"
		"**Preconditions you must meet before calling this tool:**\n"
		"1. You have attempted `livecoding_compile` and it returned `live_coding_failed`, OR\n"
		"2. The change you need to apply is one of: adding/removing `UPROPERTY`, `UFUNCTION`, `UCLASS`, `USTRUCT`, constructor body, or base class; a new reflected type; or any change that `livecoding_compile`'s description explicitly excludes.\n\n"
		"If neither precondition is met, call `livecoding_compile` first. Calling this tool preemptively for a `.cpp` body edit wastes ~2 minutes, drops user session state, and violates the plugin's agent contract.\n\n"
		"Operation: `prepare_task_continuation_handoff`. Takes the active plan and produces a handoff envelope for Agent 2. After handoff, your turn ends. The supervisor drives the rebuild and reattach; when you see the user again in a new turn, the editor is fresh and the change is compiled."
	);
	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"),
			TEXT("Structured restart-survival action. Use exactly 'prepare_task_continuation_handoff'."), true),
		FMCPToolParameter(TEXT("task_id"), TEXT("string"),
			TEXT("Optional logical task_id that must survive restart and reattach. If omitted on the ordinary autonomous path, the plugin derives one from the current surviving task/session."), false),
		FMCPToolParameter(TEXT("session_id"), TEXT("string"),
			TEXT("Optional logical session_id that must survive restart and reattach. If omitted on the ordinary autonomous path, the plugin derives one from the current task_id."), false),
		FMCPToolParameter(TEXT("continuation_intent_prompt"), TEXT("string"),
			TEXT("Exact bounded continuation prompt to send automatically after reattach."), true),
		FMCPToolParameter(TEXT("detail"), TEXT("string"),
			TEXT("Short truthful detail for why the handoff exists."), false, TEXT("task_driven_structured_handoff")),
		FMCPToolParameter(TEXT("linked_provider_session_id"), TEXT("string"),
			TEXT("Optional exact provider session assertion. If supplied, it must match the current persistent Codex session."), false),
		FMCPToolParameter(TEXT("auto_start_after_response"), TEXT("boolean"),
			TEXT("Whether the widget should detach into restart survival immediately after the current response completes."), false, TEXT("true")),
		FMCPToolParameter(TEXT("restore_enabled"), TEXT("boolean"),
			TEXT("Whether the handoff also includes one exact restore intent."), false, TEXT("false")),
		FMCPToolParameter(TEXT("restore_autosave_source_path"), TEXT("string"),
			TEXT("Exact absolute autosave source path inside the current project root."), false),
		FMCPToolParameter(TEXT("restore_target_path"), TEXT("string"),
			TEXT("Exact absolute target path inside the current project root."), false),
		FMCPToolParameter(TEXT("restore_backup_path"), TEXT("string"),
			TEXT("Exact absolute backup path inside the current project root."), false),
		FMCPToolParameter(TEXT("restore_detail"), TEXT("string"),
			TEXT("Short truthful detail for the optional restore intent."), false, TEXT("explicit_autosave_backed_restore")),
		FMCPToolParameter(TEXT("file_write_enabled"), TEXT("boolean"),
			TEXT("Whether the handoff also includes one exact project-local file-write intent."), false, TEXT("false")),
		FMCPToolParameter(TEXT("file_write_source_path"), TEXT("string"),
			TEXT("Exact absolute project-local source path whose current contents should be copied while Unreal is closed."), false),
		FMCPToolParameter(TEXT("file_write_target_path"), TEXT("string"),
			TEXT("Exact absolute project-local target path to create or overwrite while Unreal is closed."), false),
		FMCPToolParameter(TEXT("file_write_backup_path"), TEXT("string"),
			TEXT("Optional exact absolute project-local backup path that should receive the previous target contents before overwrite."), false),
		FMCPToolParameter(TEXT("file_write_detail"), TEXT("string"),
			TEXT("Short truthful detail for the optional exact file-write intent."), false, TEXT("exact_project_local_file_write"))
	};

	// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
	Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
		TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_RestartSurvival::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	if (!Operation.Equals(PrepareTaskContinuationHandoffOperation, ESearchCase::CaseSensitive))
	{
		return FMCPToolResult::Error(TEXT("Unsupported restart_survival operation. Use prepare_task_continuation_handoff."));
	}

	const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
	if (BackendStatus.Backend != EOsvayderUEProviderBackend::CodexCli)
	{
		return FMCPToolResult::Error(TEXT("restart_survival handoff is currently supported only on the ordinary Codex runtime path."));
	}

	if (!FCodexCliRunner::ShouldUsePersistentConversationTransport())
	{
		return FMCPToolResult::Error(TEXT("restart_survival handoff requires the persistent Codex app-server transport."));
	}

	FCodexCliRunner* CodexRunner = static_cast<FCodexCliRunner*>(FOsvayderCodeSubsystem::Get().GetActiveBackend());
	if (CodexRunner == nullptr)
	{
		return FMCPToolResult::Error(TEXT("Configured backend is not an active Codex CLI runner."));
	}

	const FString ActiveProviderSessionId = CodexRunner->GetActivePersistentThreadId();
	if (ActiveProviderSessionId.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("restart_survival handoff requires one active persistent Codex task/session."));
	}

	FOsvayderUERestartSurvivalState ExistingState;
	if (FOsvayderUERestartSurvivalManager::DescribeCurrentState(ExistingState))
	{
		const bool bCanReplaceExistingState =
			ExistingState.Phase == EOsvayderUERestartSurvivalPhase::FailedTerminal
			|| (ExistingState.Phase == EOsvayderUERestartSurvivalPhase::Reattached && !ExistingState.bProviderThreadResumePending);
		if (!bCanReplaceExistingState)
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Restart survival is already active for this project in phase %s."),
				OsvayderUERestartSurvivalPhaseToString(ExistingState.Phase)));
		}
	}

	FString TaskId = ExtractOptionalString(Params, TEXT("task_id"));
	FString SessionId = ExtractOptionalString(Params, TEXT("session_id"));

	FString ContinuationIntentPrompt;
	if (!ExtractRequiredString(Params, TEXT("continuation_intent_prompt"), ContinuationIntentPrompt, Error))
	{
		return Error.GetValue();
	}

	const bool bAutonomousIdentityDefaultsApplied = TaskId.IsEmpty() || SessionId.IsEmpty();
	if (TaskId.IsEmpty())
	{
		TaskId = MakeAutonomousTaskId(ActiveProviderSessionId);
	}

	if (SessionId.IsEmpty())
	{
		SessionId = MakeSessionIdFromTaskId(TaskId);
	}

	const FString LinkedProviderSessionIdAssertion = ExtractOptionalString(Params, TEXT("linked_provider_session_id"));
	if (!LinkedProviderSessionIdAssertion.IsEmpty()
		&& !LinkedProviderSessionIdAssertion.Equals(ActiveProviderSessionId, ESearchCase::CaseSensitive))
	{
		return FMCPToolResult::Error(TEXT("restart_survival handoff linked_provider_session_id does not match the current persistent Codex session."));
	}

	FOsvayderUERestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = MakePreparedRequestId();
	Request.TaskId = TaskId;
	Request.SessionId = SessionId;
	Request.Backend = BackendStatus.Backend;
	Request.LinkedProviderSessionId = ActiveProviderSessionId;
	Request.ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Request.bTaskDrivenHandoff = true;
	Request.bAutoStartAfterResponse = ExtractOptionalBool(Params, TEXT("auto_start_after_response"), true);
	Request.bAutonomousClosedEditorEscalation = bAutonomousIdentityDefaultsApplied;
	Request.ContinuationIntentPrompt = ContinuationIntentPrompt;
	Request.Detail = ExtractOptionalString(
		Params,
		TEXT("detail"),
		bAutonomousIdentityDefaultsApplied
			? TEXT("autonomous_closed_editor_escalation")
			: TEXT("task_driven_structured_handoff"));
	Request.CreatedAtUtc = FDateTime::UtcNow().ToIso8601();

	Request.AutosaveSourcePath = ExtractOptionalString(Params, TEXT("restore_autosave_source_path"));
	Request.TargetPath = ExtractOptionalString(Params, TEXT("restore_target_path"));
	Request.BackupPath = ExtractOptionalString(Params, TEXT("restore_backup_path"));
	Request.bRestoreEnabled = ExtractOptionalBool(Params, TEXT("restore_enabled"), false)
		|| !Request.AutosaveSourcePath.IsEmpty()
		|| !Request.TargetPath.IsEmpty()
		|| !Request.BackupPath.IsEmpty();
	if (Request.bRestoreEnabled)
	{
		Request.Detail = ExtractOptionalString(Params, TEXT("restore_detail"), Request.Detail);
	}
	Request.FileWriteSourcePath = ExtractOptionalString(Params, TEXT("file_write_source_path"));
	Request.FileWriteTargetPath = ExtractOptionalString(Params, TEXT("file_write_target_path"));
	Request.FileWriteBackupPath = ExtractOptionalString(Params, TEXT("file_write_backup_path"));
	Request.bFileWriteEnabled = ExtractOptionalBool(Params, TEXT("file_write_enabled"), false)
		|| !Request.FileWriteSourcePath.IsEmpty()
		|| !Request.FileWriteTargetPath.IsEmpty()
		|| !Request.FileWriteBackupPath.IsEmpty();
	Request.FileWriteDetail = ExtractOptionalString(Params, TEXT("file_write_detail"), TEXT("exact_project_local_file_write"));
	if (Request.bFileWriteEnabled)
	{
		Request.Detail = Request.FileWriteDetail;
	}
	if (Request.bRestoreEnabled && Request.bFileWriteEnabled)
	{
		return FMCPToolResult::Error(TEXT("restart_survival handoff may carry either one exact restore intent or one exact file-write intent, but not both."));
	}
	Request.PostReattachCompletionText = bAutonomousIdentityDefaultsApplied
		? BuildAutonomousClosedEditorPostReattachCompletionText(Request)
		: ContinuationIntentPrompt;

	FString ValidationError;
	if (!FOsvayderUERestartSurvivalManager::ValidatePreparedRestoreRequestForStart(
		Request,
		BackendStatus.Backend,
		Request.ProjectRoot,
		ActiveProviderSessionId,
		ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	FString SaveError;
	if (!FOsvayderUERestartSurvivalManager::SavePreparedRestoreRequest(Request, SaveError))
	{
		return FMCPToolResult::Error(SaveError);
	}

	if (Request.bAutoStartAfterResponse)
	{
		FOsvayderUERestartSurvivalManager::ArmPreparedRequestAutoStart(
			Request.RequestId,
			Request.Backend,
			Request.ProjectRoot,
			Request.LinkedProviderSessionId);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), Operation);
	Data->SetStringField(TEXT("request_id"), Request.RequestId);
	Data->SetStringField(TEXT("task_id"), Request.TaskId);
	Data->SetStringField(TEXT("session_id"), Request.SessionId);
	Data->SetStringField(TEXT("backend"), OsvayderUEProviderBackendToString(Request.Backend));
	Data->SetStringField(TEXT("linked_provider_session_id"), Request.LinkedProviderSessionId);
	Data->SetStringField(TEXT("project_root"), Request.ProjectRoot);
	Data->SetStringField(TEXT("prepared_request_path"), FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath());
	Data->SetBoolField(TEXT("task_driven_handoff"), Request.bTaskDrivenHandoff);
	Data->SetBoolField(TEXT("auto_start_after_response"), Request.bAutoStartAfterResponse);
	Data->SetBoolField(TEXT("autonomous_identity_defaults_applied"), bAutonomousIdentityDefaultsApplied);
	Data->SetBoolField(TEXT("restore_enabled"), Request.bRestoreEnabled);
	Data->SetBoolField(TEXT("file_write_enabled"), Request.bFileWriteEnabled);
	Data->SetStringField(TEXT("continuation_intent_prompt"), Request.ContinuationIntentPrompt);

	return FMCPToolResult::Success(TEXT("Structured restart-survival handoff prepared."), Data);
}
