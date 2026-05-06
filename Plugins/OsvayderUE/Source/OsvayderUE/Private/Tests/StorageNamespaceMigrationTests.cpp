// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "OsvayderSessionManager.h"
#include "CodexCliRunner.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUERelayAgent.h"
#include "OsvayderUERestartSurvival.h"
#include "OsvayderUEScopePolicy.h"
#include "OsvayderUESettings.h"
#include "OsvayderUEStorageMigration.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString NormalizeFilePathForTest(const FString& InPath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	FString NormalizeDirectoryPathForTest(const FString& InPath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	void MakeFreshManagedRoots(const FString& TestName, FString& OutPreferredRoot, FString& OutLegacyRoot)
	{
		FString BaseRoot = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("OsvayderUE"),
			TEXT("Automation"),
			TEXT("StorageNamespaceMigration"),
			TestName);
		FPaths::NormalizeDirectoryName(BaseRoot);
		IFileManager::Get().DeleteDirectory(*BaseRoot, false, true);
		IFileManager::Get().MakeDirectory(*BaseRoot, true);

		OutPreferredRoot = FPaths::Combine(BaseRoot, TEXT("OsvayderUE"));
		OutLegacyRoot = FPaths::Combine(BaseRoot, TEXT("OsvayderUE"));
		IFileManager::Get().MakeDirectory(*OutPreferredRoot, true);
		IFileManager::Get().MakeDirectory(*OutLegacyRoot, true);
	}

	bool SaveTextFile(const FString& Path, const FString& Contents)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(Contents, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool SaveJsonFile(const FString& Path, const TSharedRef<FJsonObject>& RootObject)
	{
		FString JsonString;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonString);
		if (!FJsonSerializer::Serialize(RootObject, Writer))
		{
			return false;
		}

		return SaveTextFile(Path, JsonString);
	}

	bool LoadTextFile(const FString& Path, FString& OutContents)
	{
		return FFileHelper::LoadFileToString(OutContents, *Path);
	}

	struct FScopedSessionSaveDirOverride
	{
		explicit FScopedSessionSaveDirOverride(const FString& InDir)
		{
			FOsvayderSessionManager::SetTestSessionSaveDirOverride(InDir);
		}

		~FScopedSessionSaveDirOverride()
		{
			FOsvayderSessionManager::ClearTestSessionSaveDirOverride();
		}
	};

	struct FScopedNativeProjectsRootOverride
	{
		explicit FScopedNativeProjectsRootOverride(const FString& InDir)
		{
			FOsvayderSessionManager::SetTestNativeProjectsRootOverride(InDir);
		}

		~FScopedNativeProjectsRootOverride()
		{
			FOsvayderSessionManager::ClearTestNativeProjectsRootOverride();
		}
	};

	struct FScopedRestartRootOverride
	{
		explicit FScopedRestartRootOverride(const FString& InDir)
		{
			FOsvayderUERestartSurvivalManager::SetTestStateRootOverride(InDir);
		}

		~FScopedRestartRootOverride()
		{
			FOsvayderUERestartSurvivalManager::ClearTestStateRootOverride();
		}
	};

	struct FScopedTraceLogPathOverride
	{
		explicit FScopedTraceLogPathOverride(const FString& InPath)
		{
			FOsvayderUEAgentTraceLog::SetTestTraceLogPathOverride(InPath);
		}

		~FScopedTraceLogPathOverride()
		{
			FOsvayderUEAgentTraceLog::ClearTestTraceLogPathOverride();
		}
	};

	struct FScopedCopyFailureModeOverride
	{
		explicit FScopedCopyFailureModeOverride(const OsvayderUEStorageMigration::ETestCopyFailureMode InFailureMode)
		{
			OsvayderUEStorageMigration::SetTestCopyFailureMode(InFailureMode);
		}

		~FScopedCopyFailureModeOverride()
		{
			OsvayderUEStorageMigration::ClearTestCopyFailureMode();
		}
	};

	struct FScopedScopeModeOverride
	{
		UOsvayderUESettings* Settings = nullptr;
		EOsvayderUEScopeMode OriginalScopeMode = EOsvayderUEScopeMode::PluginOnly;

		explicit FScopedScopeModeOverride(const EOsvayderUEScopeMode NewScopeMode)
		{
			Settings = UOsvayderUESettings::GetMutable();
			if (Settings)
			{
				OriginalScopeMode = Settings->ScopeMode;
				Settings->ScopeMode = NewScopeMode;
			}
		}

		~FScopedScopeModeOverride()
		{
			if (Settings)
			{
				Settings->ScopeMode = OriginalScopeMode;
			}
		}
	};

	TSharedRef<FJsonObject> MakeSessionFixtureJson(const FString& StoreKind, const FString& UserPrompt, const FString& AssistantReply)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schema_version"), 3);
		Root->SetStringField(TEXT("backend"), TEXT("CodexCli"));
		Root->SetStringField(TEXT("backend_display_name"), TEXT("Codex CLI"));
		Root->SetStringField(TEXT("store_kind"), StoreKind);
		Root->SetStringField(TEXT("model"), TEXT("gpt-5"));
		Root->SetStringField(TEXT("last_updated"), TEXT("2026-05-05T00:00:00Z"));
		Root->SetNumberField(TEXT("message_count"), 1);

		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("user"), UserPrompt);
		Message->SetStringField(TEXT("assistant"), AssistantReply);

		TArray<TSharedPtr<FJsonValue>> Messages;
		Messages.Add(MakeShared<FJsonValueObject>(Message));
		Root->SetArrayField(TEXT("messages"), Messages);
		return Root;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStorageNamespaceMigration_SessionArtifactsMigrateFromLegacy,
	"OsvayderUE.StorageMigration.SessionArtifactsMigrateFromLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStorageNamespaceMigration_RestartSurvivalArtifactsMigrateFromLegacy,
	"OsvayderUE.StorageMigration.RestartSurvivalArtifactsMigrateFromLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStorageNamespaceMigration_SessionTerminalClearsSuppressLegacyFallback,
	"OsvayderUE.StorageMigration.SessionTerminalClearsSuppressLegacyFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStorageNamespaceMigration_RestartSurvivalTerminalDeletesSuppressLegacyFallback,
	"OsvayderUE.StorageMigration.RestartSurvivalTerminalDeletesSuppressLegacyFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStorageNamespaceMigration_RelayArtifactsMigrateFromLegacy,
	"OsvayderUE.StorageMigration.RelayArtifactsMigrateFromLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStorageNamespaceMigration_RelayArchiveCleanupSuppressesLegacyFallback,
	"OsvayderUE.StorageMigration.RelayArchiveCleanupSuppressesLegacyFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStorageNamespaceMigration_AgentTraceMigratesAndAppends,
	"OsvayderUE.StorageMigration.AgentTraceMigratesAndAppends",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStorageNamespaceMigration_ScopePolicyAllowsLegacyAndPreferredRoots,
	"OsvayderUE.StorageMigration.ScopePolicyAllowsLegacyAndPreferredRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStorageNamespaceMigration_RepairFailurePreservesPreferredArtifact,
	"OsvayderUE.StorageMigration.RepairFailurePreservesPreferredArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStorageNamespaceMigration_SessionArtifactsMigrateFromLegacy::RunTest(const FString& Parameters)
{
	FString PreferredRoot;
	FString LegacyRoot;
	MakeFreshManagedRoots(TEXT("SessionArtifacts"), PreferredRoot, LegacyRoot);
	FScopedSessionSaveDirOverride Override(PreferredRoot);

	const FString PreferredSessionPath = FPaths::Combine(PreferredRoot, TEXT("session_codex_cli.json"));
	const FString PreferredVisiblePath = FPaths::Combine(PreferredRoot, TEXT("visible_session_codex_cli.json"));
	const FString PreferredPersistentPath = FPaths::Combine(PreferredRoot, TEXT("claude_persistent_session.json"));
	const FString LegacySessionPath = FPaths::Combine(LegacyRoot, TEXT("session_codex_cli.json"));
	const FString LegacyVisiblePath = FPaths::Combine(LegacyRoot, TEXT("visible_session_codex_cli.json"));
	const FString LegacyPersistentPath = FPaths::Combine(LegacyRoot, TEXT("claude_persistent_session.json"));
	const FString ReceiptPath = FPaths::Combine(PreferredRoot, TEXT("MigrationReceipts"), TEXT("storage_namespace_migrations.jsonl"));

	TestTrue(TEXT("legacy provider session fixture should save"), SaveJsonFile(
		LegacySessionPath,
		MakeSessionFixtureJson(TEXT("normal_provider_session"), TEXT("legacy provider prompt"), TEXT("legacy provider reply"))));
	TestTrue(TEXT("legacy visible session fixture should save"), SaveJsonFile(
		LegacyVisiblePath,
		MakeSessionFixtureJson(TEXT("project_local_visible_restore"), TEXT("legacy visible prompt"), TEXT("legacy visible reply"))));

	TSharedRef<FJsonObject> PersistentRoot = MakeShared<FJsonObject>();
	PersistentRoot->SetNumberField(TEXT("schema_version"), 1);
	PersistentRoot->SetStringField(TEXT("session_id"), TEXT("legacy-persistent-session-id"));
	PersistentRoot->SetStringField(TEXT("model"), TEXT("gpt-5"));
	PersistentRoot->SetStringField(TEXT("created_utc"), TEXT("2026-05-05T00:00:00Z"));
	PersistentRoot->SetStringField(TEXT("last_used_utc"), TEXT("2026-05-05T00:00:01Z"));
	TestTrue(TEXT("legacy persistent fixture should save"), SaveJsonFile(LegacyPersistentPath, PersistentRoot));

	TestTrue(TEXT("invalid preferred provider session fixture should save"), SaveTextFile(PreferredSessionPath, TEXT("{ invalid json")));

	FOsvayderSessionManager Manager;
	const FAgentSessionRestoreResult ProviderResult = Manager.LoadSession(EOsvayderUEProviderBackend::CodexCli);
	const FAgentSessionRestoreResult VisibleResult = Manager.LoadVisibleSession(EOsvayderUEProviderBackend::CodexCli);
	const FString PersistentSessionId = Manager.ReadPersistentSessionId(EOsvayderUEProviderBackend::CodexCli);

	TestEqual(TEXT("provider session should restore one message"), Manager.GetProviderSessionHistory(EOsvayderUEProviderBackend::CodexCli).Num(), 1);
	TestEqual(TEXT("visible session should restore one message"), Manager.GetHistory(EOsvayderUEProviderBackend::CodexCli).Num(), 1);
	TestEqual(TEXT("persistent session id should migrate from legacy"), PersistentSessionId, FString(TEXT("legacy-persistent-session-id")));
	TestTrue(TEXT("preferred provider session should exist after repair"), IFileManager::Get().FileExists(*PreferredSessionPath));
	TestTrue(TEXT("preferred visible session should exist after migration"), IFileManager::Get().FileExists(*PreferredVisiblePath));
	TestTrue(TEXT("preferred persistent session artifact should exist after migration"), IFileManager::Get().FileExists(*PreferredPersistentPath));
	TestTrue(TEXT("legacy provider session should remain"), IFileManager::Get().FileExists(*LegacySessionPath));
	TestTrue(TEXT("legacy visible session should remain"), IFileManager::Get().FileExists(*LegacyVisiblePath));
	TestTrue(TEXT("legacy persistent session artifact should remain"), IFileManager::Get().FileExists(*LegacyPersistentPath));
	TestEqual(TEXT("provider restore should read preferred path after repair"), NormalizeFilePathForTest(ProviderResult.RequestedSession.SessionFilePath), NormalizeFilePathForTest(PreferredSessionPath));
	TestEqual(TEXT("visible restore should read preferred path after migration"), NormalizeFilePathForTest(VisibleResult.RequestedSession.SessionFilePath), NormalizeFilePathForTest(PreferredVisiblePath));
	TestTrue(TEXT("migration receipt should exist"), IFileManager::Get().FileExists(*ReceiptPath));

	FString ReceiptContents;
	TestTrue(TEXT("migration receipt should be readable"), LoadTextFile(ReceiptPath, ReceiptContents));
	TestTrue(TEXT("receipt should include repair action"), ReceiptContents.Contains(TEXT("repair_from_legacy")));
	TestTrue(TEXT("receipt should include migrate action"), ReceiptContents.Contains(TEXT("migrate_from_legacy")));
	return true;
}

bool FStorageNamespaceMigration_SessionTerminalClearsSuppressLegacyFallback::RunTest(const FString& Parameters)
{
	FString PreferredRoot;
	FString LegacyRoot;
	MakeFreshManagedRoots(TEXT("SessionTerminalClears"), PreferredRoot, LegacyRoot);
	FScopedSessionSaveDirOverride Override(PreferredRoot);
	FScopedNativeProjectsRootOverride NativeProjectsOverride(FPaths::Combine(PreferredRoot, TEXT("NativeClaudeProjects")));

	const FString LegacySessionPath = FPaths::Combine(LegacyRoot, TEXT("session_codex_cli.json"));
	const FString LegacyVisiblePath = FPaths::Combine(LegacyRoot, TEXT("visible_session_codex_cli.json"));
	const FString LegacyPersistentPath = FPaths::Combine(LegacyRoot, TEXT("claude_persistent_session.json"));
	const FString PreferredSessionPath = FPaths::Combine(PreferredRoot, TEXT("session_codex_cli.json"));
	const FString PreferredVisiblePath = FPaths::Combine(PreferredRoot, TEXT("visible_session_codex_cli.json"));
	const FString PreferredPersistentPath = FPaths::Combine(PreferredRoot, TEXT("claude_persistent_session.json"));

	TestTrue(TEXT("legacy provider session fixture should save"), SaveJsonFile(
		LegacySessionPath,
		MakeSessionFixtureJson(TEXT("normal_provider_session"), TEXT("legacy provider prompt"), TEXT("legacy provider reply"))));
	TestTrue(TEXT("legacy visible session fixture should save"), SaveJsonFile(
		LegacyVisiblePath,
		MakeSessionFixtureJson(TEXT("project_local_visible_restore"), TEXT("legacy visible prompt"), TEXT("legacy visible reply"))));

	TSharedRef<FJsonObject> PersistentRoot = MakeShared<FJsonObject>();
	PersistentRoot->SetNumberField(TEXT("schema_version"), 1);
	PersistentRoot->SetStringField(TEXT("session_id"), TEXT("legacy-reset-session-id"));
	PersistentRoot->SetStringField(TEXT("model"), TEXT("gpt-5"));
	PersistentRoot->SetStringField(TEXT("created_utc"), TEXT("2026-05-05T00:00:00Z"));
	PersistentRoot->SetStringField(TEXT("last_used_utc"), TEXT("2026-05-05T00:00:01Z"));
	TestTrue(TEXT("legacy persistent fixture should save"), SaveJsonFile(LegacyPersistentPath, PersistentRoot));

	const FString NativeProjectKey = FOsvayderSessionManager::ComputeClaudeNativeProjectKey(
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	const FString NativeJsonlPath = FPaths::Combine(
		PreferredRoot,
		TEXT("NativeClaudeProjects"),
		NativeProjectKey,
		TEXT("legacy-reset-session-id.jsonl"));
	TestTrue(TEXT("native Claude jsonl fixture should save"), SaveTextFile(NativeJsonlPath, TEXT("{\"type\":\"session\"}\n")));

	FOsvayderSessionManager Manager;
	FAgentBackendStatus BackendStatus;
	FString ErrorPersistentBeforeReset = Manager.ReadPersistentSessionId(EOsvayderUEProviderBackend::CodexCli);
	TestEqual(TEXT("persistent session should be readable before reset"), ErrorPersistentBeforeReset, FString(TEXT("legacy-reset-session-id")));

	TestTrue(TEXT("empty provider save should delete preferred and legacy provider session copies"),
		Manager.SaveSession(EOsvayderUEProviderBackend::CodexCli, BackendStatus));
	TestTrue(TEXT("empty visible save should delete preferred and legacy visible session copies"),
		Manager.SaveVisibleSession(EOsvayderUEProviderBackend::CodexCli, BackendStatus));
	Manager.ResetPersistentSession(EOsvayderUEProviderBackend::CodexCli);

	TestFalse(TEXT("preferred provider session should be absent after clear"), IFileManager::Get().FileExists(*PreferredSessionPath));
	TestFalse(TEXT("legacy provider session should be absent after clear"), IFileManager::Get().FileExists(*LegacySessionPath));
	TestFalse(TEXT("preferred visible session should be absent after clear"), IFileManager::Get().FileExists(*PreferredVisiblePath));
	TestFalse(TEXT("legacy visible session should be absent after clear"), IFileManager::Get().FileExists(*LegacyVisiblePath));
	TestFalse(TEXT("preferred persistent session artifact should be absent after reset"), IFileManager::Get().FileExists(*PreferredPersistentPath));
	TestFalse(TEXT("legacy persistent session artifact should be absent after reset"), IFileManager::Get().FileExists(*LegacyPersistentPath));
	TestFalse(TEXT("native Claude jsonl should be absent after reset"), IFileManager::Get().FileExists(*NativeJsonlPath));
	TestFalse(TEXT("provider session should not remain discoverable after clear"), Manager.HasSavedSession(EOsvayderUEProviderBackend::CodexCli));
	TestTrue(TEXT("persistent session id should stay empty after reset"), Manager.ReadPersistentSessionId(EOsvayderUEProviderBackend::CodexCli).IsEmpty());
	return true;
}

bool FStorageNamespaceMigration_RestartSurvivalArtifactsMigrateFromLegacy::RunTest(const FString& Parameters)
{
	FString PreferredRoot;
	FString LegacyRoot;
	MakeFreshManagedRoots(TEXT("RestartSurvivalArtifacts"), PreferredRoot, LegacyRoot);

	{
		FScopedRestartRootOverride LegacyOverride(LegacyRoot);
		FOsvayderUERestartSurvivalState State;
		State.SessionId = TEXT("legacy_session");
		State.TaskId = TEXT("legacy_task");
		State.ProjectRoot = TEXT("D:/LegacyProject");
		State.UProjectPath = TEXT("D:/LegacyProject/LegacyProject.uproject");
		State.Backend = EOsvayderUEProviderBackend::CodexCli;
		State.Phase = EOsvayderUERestartSurvivalPhase::AwaitingReattach;
		State.ProviderSessionId = TEXT("legacy_thread");
		State.bProviderThreadResumePending = true;

		FString Error;
		TestTrue(TEXT("legacy state fixture should save"), FOsvayderUERestartSurvivalManager::SaveState(State, Error));

		FOsvayderUERestartSurvivalPreparedRestoreRequest Request;
		Request.RequestId = TEXT("request_legacy");
		Request.TaskId = TEXT("legacy_task");
		Request.SessionId = TEXT("legacy_session");
		Request.LinkedProviderSessionId = TEXT("legacy_thread");
		Request.Backend = EOsvayderUEProviderBackend::CodexCli;
		Request.PostReattachCompletionText = TEXT("Continue the task.");
		Request.ContinuationIntentPrompt = TEXT("Continue the task.");
		TestTrue(TEXT("legacy prepared request fixture should save"), FOsvayderUERestartSurvivalManager::SavePreparedRestoreRequest(Request, Error));

		TestTrue(TEXT("legacy closed editor result fixture should save"), SaveTextFile(
			FPaths::Combine(LegacyRoot, TEXT("closed_editor_result.json")),
			TEXT("{\"result\":\"legacy_ok\"}\n")));
	}

	FScopedRestartRootOverride PreferredOverride(PreferredRoot);
	FOsvayderUERestartSurvivalState LoadedState;
	FString Error;
	TestTrue(TEXT("restart-survival state should load from legacy and migrate"), FOsvayderUERestartSurvivalManager::LoadState(LoadedState, Error));
	TestEqual(TEXT("loaded restart task id should match legacy fixture"), LoadedState.TaskId, FString(TEXT("legacy_task")));
	TestTrue(TEXT("prepared restore request presence should migrate"), FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest());

	FOsvayderUERestartSurvivalPreparedRestoreRequest LoadedRequest;
	TestTrue(TEXT("prepared restore request should load from legacy and migrate"), FOsvayderUERestartSurvivalManager::LoadPreparedRestoreRequest(LoadedRequest, Error));
	TestEqual(TEXT("loaded restore request id should match legacy fixture"), LoadedRequest.RequestId, FString(TEXT("request_legacy")));

	TSharedPtr<FJsonObject> Readback = FOsvayderUERestartSurvivalManager::BuildReadbackJson();
	TestTrue(TEXT("readback json should be valid"), Readback.IsValid());
	if (Readback.IsValid())
	{
		bool bClosedEditorResultPresent = false;
		TestTrue(TEXT("readback should expose closed editor result presence"), Readback->TryGetBoolField(TEXT("closed_editor_result_present"), bClosedEditorResultPresent));
		TestTrue(TEXT("closed editor result should migrate into preferred namespace"), bClosedEditorResultPresent);
	}

	const FString Summary = FOsvayderUERestartSurvivalManager::BuildWidgetDebugSummary();
	TestTrue(TEXT("widget summary should point at preferred closed editor result path"), Summary.Contains(FOsvayderUERestartSurvivalManager::GetClosedEditorResultPath()));
	TestTrue(TEXT("preferred state file should exist"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("restart_survival_state.json"))));
	TestTrue(TEXT("preferred restore request file should exist"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("restart_survival_restore_request.json"))));
	TestTrue(TEXT("preferred closed editor result file should exist"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("closed_editor_result.json"))));
	TestTrue(TEXT("legacy state file should remain"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("restart_survival_state.json"))));
	TestTrue(TEXT("legacy restore request file should remain"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("restart_survival_restore_request.json"))));
	TestTrue(TEXT("legacy closed editor result file should remain"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("closed_editor_result.json"))));
	return true;
}

bool FStorageNamespaceMigration_RestartSurvivalTerminalDeletesSuppressLegacyFallback::RunTest(const FString& Parameters)
{
	FString PreferredRoot;
	FString LegacyRoot;
	MakeFreshManagedRoots(TEXT("RestartSurvivalTerminalDeletes"), PreferredRoot, LegacyRoot);

	{
		FScopedRestartRootOverride LegacyOverride(LegacyRoot);
		FString Error;

		FOsvayderUERestartSurvivalState State;
		State.SessionId = TEXT("legacy_session");
		State.TaskId = TEXT("legacy_task");
		State.ProjectRoot = TEXT("D:/LegacyProject");
		State.UProjectPath = TEXT("D:/LegacyProject/LegacyProject.uproject");
		State.Backend = EOsvayderUEProviderBackend::CodexCli;
		State.Phase = EOsvayderUERestartSurvivalPhase::AwaitingReattach;
		State.ProviderSessionId = TEXT("legacy_thread");
		TestTrue(TEXT("legacy state fixture should save"), FOsvayderUERestartSurvivalManager::SaveState(State, Error));

		FOsvayderUERestartSurvivalPreparedRestoreRequest Request;
		Request.RequestId = TEXT("request_legacy_delete");
		Request.TaskId = TEXT("legacy_task");
		Request.SessionId = TEXT("legacy_session");
		Request.LinkedProviderSessionId = TEXT("legacy_thread");
		Request.Backend = EOsvayderUEProviderBackend::CodexCli;
		Request.PostReattachCompletionText = TEXT("Continue the task.");
		Request.ContinuationIntentPrompt = TEXT("Continue the task.");
		TestTrue(TEXT("legacy prepared request fixture should save"), FOsvayderUERestartSurvivalManager::SavePreparedRestoreRequest(Request, Error));

		TestTrue(TEXT("legacy closed editor result evidence should save"), SaveTextFile(
			FPaths::Combine(LegacyRoot, TEXT("closed_editor_result.json")),
			TEXT("{\"result\":\"legacy_ok\"}\n")));
	}

	FScopedRestartRootOverride PreferredOverride(PreferredRoot);
	FString Error;
	TestTrue(TEXT("delete prepared request should succeed"), FOsvayderUERestartSurvivalManager::DeletePreparedRestoreRequest(Error));
	TestTrue(TEXT("delete state should succeed"), FOsvayderUERestartSurvivalManager::DeleteState(Error));

	FOsvayderUERestartSurvivalState LoadedState;
	TestFalse(TEXT("state should not reload from legacy after delete"), FOsvayderUERestartSurvivalManager::LoadState(LoadedState, Error));
	TestFalse(TEXT("prepared restore request should not reload from legacy after delete"), FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest());
	TestFalse(TEXT("preferred state should be absent after delete"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("restart_survival_state.json"))));
	TestFalse(TEXT("legacy state should be absent after delete"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("restart_survival_state.json"))));
	TestFalse(TEXT("preferred request should be absent after delete"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("restart_survival_restore_request.json"))));
	TestFalse(TEXT("legacy request should be absent after delete"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("restart_survival_restore_request.json"))));
	TestTrue(TEXT("legacy closed editor evidence should remain after terminal deletes"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("closed_editor_result.json"))));
	return true;
}

bool FStorageNamespaceMigration_RelayArtifactsMigrateFromLegacy::RunTest(const FString& Parameters)
{
	FString PreferredRoot;
	FString LegacyRoot;
	MakeFreshManagedRoots(TEXT("RelayArtifacts"), PreferredRoot, LegacyRoot);

	{
		FScopedRestartRootOverride LegacyOverride(LegacyRoot);
		FOsvayderUEActivePlan Plan;
		Plan.PlanId = TEXT("legacy_plan");
		Plan.OriginalUserTask = TEXT("Repair the build and continue.");
		Plan.Status = TEXT("running");
		Plan.CreatedAtUtc = TEXT("2026-05-05T00:00:00Z");
		FString Error;
		TestTrue(TEXT("legacy active plan fixture should save"), FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error));

		FOsvayderUERelayHandoffContext Context;
		Context.TaskId = TEXT("relay_task");
		Context.RelaySessionId = TEXT("relay_session");
		Context.ProjectRoot = TEXT("D:/LegacyProject");
		Context.UProjectPath = TEXT("D:/LegacyProject/LegacyProject.uproject");
		Context.OriginalUserPrompt = TEXT("Repair the build and continue.");
		Context.CreatedAtUtc = TEXT("2026-05-05T00:00:00Z");
		TestTrue(TEXT("legacy handoff fixture should save"), FOsvayderUERelayAgentManager::SaveHandoffContext(Context, Error));

		FOsvayderUERelayResult Result;
		Result.TaskId = TEXT("relay_task");
		Result.PlanId = TEXT("legacy_plan");
		Result.RelaySessionId = TEXT("relay_session");
		Result.TerminalOutcome = EOsvayderUERelayTerminalOutcome::Success;
		Result.Status = TEXT("success");
		Result.Summary = TEXT("Legacy relay result.");
		Result.CompletedAtUtc = TEXT("2026-05-05T00:00:10Z");
		TestTrue(TEXT("legacy result fixture should save"), FOsvayderUERelayAgentManager::SaveRelayResult(Result, Error));

		FOsvayderUERelayProgressEntry Entry;
		Entry.TaskId = TEXT("relay_task");
		Entry.RelaySessionId = TEXT("relay_session");
		Entry.TimestampUtc = TEXT("2026-05-05T00:00:05Z");
		Entry.EntryKind = TEXT("summary");
		Entry.Summary = TEXT("Legacy relay progress.");
		Entry.IterationIndex = 1;
		TestTrue(TEXT("legacy progress fixture should append"), FOsvayderUERelayAgentManager::AppendProgressEntry(Entry, Error));

		TestTrue(TEXT("legacy cancel request fixture should save"), FOsvayderUERelayAgentManager::WriteCancelRequest(TEXT("legacy cancel"), Error));
	}

	FScopedRestartRootOverride PreferredOverride(PreferredRoot);
	FString Error;
	FOsvayderUEActivePlan LoadedPlan;
	TestTrue(TEXT("active plan should load from legacy and migrate"), FOsvayderUERelayAgentManager::LoadActivePlan(LoadedPlan, Error));
	TestEqual(TEXT("loaded active plan id should match legacy fixture"), LoadedPlan.PlanId, FString(TEXT("legacy_plan")));

	FOsvayderUERelayHandoffContext LoadedContext;
	TestTrue(TEXT("handoff should load from legacy and migrate"), FOsvayderUERelayAgentManager::LoadHandoffContext(LoadedContext, Error));
	TestEqual(TEXT("loaded handoff task id should match legacy fixture"), LoadedContext.TaskId, FString(TEXT("relay_task")));

	FOsvayderUERelayResult LoadedResult;
	TestTrue(TEXT("relay result should load from legacy and migrate"), FOsvayderUERelayAgentManager::LoadRelayResult(LoadedResult, Error));
	TestEqual(TEXT("loaded relay result summary should match legacy fixture"), LoadedResult.Summary, FString(TEXT("Legacy relay result.")));

	FOsvayderUERelayProgressEntry LoadedProgress;
	TestTrue(TEXT("relay progress should load from legacy and migrate"), FOsvayderUERelayAgentManager::LoadLatestProgressEntry(LoadedProgress, Error));
	TestEqual(TEXT("loaded relay progress summary should match legacy fixture"), LoadedProgress.Summary, FString(TEXT("Legacy relay progress.")));
	TestTrue(TEXT("cancel request should load from legacy and migrate"), FOsvayderUERelayAgentManager::HasCancelRequest());

	FOsvayderUERelayProgressEntry NewEntry;
	NewEntry.TaskId = TEXT("relay_task");
	NewEntry.RelaySessionId = TEXT("relay_session");
	NewEntry.TimestampUtc = TEXT("2026-05-05T00:00:15Z");
	NewEntry.EntryKind = TEXT("tool_result");
	NewEntry.Summary = TEXT("New preferred progress entry.");
	NewEntry.IterationIndex = 2;
	TestTrue(TEXT("new preferred progress should append after hydration"), FOsvayderUERelayAgentManager::AppendProgressEntry(NewEntry, Error));

	TArray<FOsvayderUERelayProgressEntry> Entries;
	TestTrue(TEXT("progress task filter should see both legacy and new entries"), FOsvayderUERelayAgentManager::LoadProgressEntriesForTask(TEXT("relay_task"), Entries, Error));
	TestEqual(TEXT("progress entry count should include hydrated legacy plus new append"), Entries.Num(), 2);
	TestTrue(TEXT("preferred active plan should exist"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("active_plan.json"))));
	TestTrue(TEXT("preferred progress log should exist"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("relay_progress.jsonl"))));
	TestTrue(TEXT("preferred relay result should exist"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("relay_result.json"))));
	TestTrue(TEXT("preferred handoff context should exist"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("handoff_context.json"))));
	TestTrue(TEXT("preferred cancel request should exist"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("relay_cancel_request.json"))));
	TestTrue(TEXT("legacy active plan should remain"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("active_plan.json"))));
	TestTrue(TEXT("legacy progress log should remain"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("relay_progress.jsonl"))));
	return true;
}

bool FStorageNamespaceMigration_RelayArchiveCleanupSuppressesLegacyFallback::RunTest(const FString& Parameters)
{
	FString PreferredRoot;
	FString LegacyRoot;
	MakeFreshManagedRoots(TEXT("RelayArchiveCleanup"), PreferredRoot, LegacyRoot);

	FOsvayderUEActivePlan Plan;
	Plan.PlanId = TEXT("legacy_plan");
	Plan.OriginalUserTask = TEXT("Repair the build and continue.");
	Plan.Status = TEXT("success");
	Plan.CreatedAtUtc = TEXT("2026-05-05T00:00:00Z");
	Plan.ArchiveRunTag = TEXT("archive_cleanup");

	{
		FScopedRestartRootOverride LegacyOverride(LegacyRoot);
		FString Error;
		TestTrue(TEXT("legacy active plan fixture should save"), FOsvayderUERelayAgentManager::SaveActivePlan(Plan, Error));

		FOsvayderUERelayHandoffContext Context;
		Context.TaskId = TEXT("relay_task");
		Context.RelaySessionId = TEXT("relay_session");
		Context.ProjectRoot = TEXT("D:/LegacyProject");
		Context.UProjectPath = TEXT("D:/LegacyProject/LegacyProject.uproject");
		Context.OriginalUserPrompt = TEXT("Repair the build and continue.");
		Context.CreatedAtUtc = TEXT("2026-05-05T00:00:00Z");
		TestTrue(TEXT("legacy handoff fixture should save"), FOsvayderUERelayAgentManager::SaveHandoffContext(Context, Error));

		FOsvayderUERelayResult Result;
		Result.TaskId = TEXT("relay_task");
		Result.PlanId = TEXT("legacy_plan");
		Result.RelaySessionId = TEXT("relay_session");
		Result.TerminalOutcome = EOsvayderUERelayTerminalOutcome::Success;
		Result.Status = TEXT("success");
		Result.Summary = TEXT("Legacy relay result.");
		Result.CompletedAtUtc = TEXT("2026-05-05T00:00:10Z");
		TestTrue(TEXT("legacy result fixture should save"), FOsvayderUERelayAgentManager::SaveRelayResult(Result, Error));

		FOsvayderUERelayProgressEntry Entry;
		Entry.TaskId = TEXT("relay_task");
		Entry.RelaySessionId = TEXT("relay_session");
		Entry.TimestampUtc = TEXT("2026-05-05T00:00:05Z");
		Entry.EntryKind = TEXT("summary");
		Entry.Summary = TEXT("Legacy relay progress.");
		Entry.IterationIndex = 1;
		TestTrue(TEXT("legacy progress fixture should append"), FOsvayderUERelayAgentManager::AppendProgressEntry(Entry, Error));

		TestTrue(TEXT("legacy cancel request fixture should save"), FOsvayderUERelayAgentManager::WriteCancelRequest(TEXT("legacy cancel"), Error));
	}

	FScopedRestartRootOverride PreferredOverride(PreferredRoot);
	FString Error;
	TArray<FString> ArchivedPaths;
	TestTrue(TEXT("terminal archive should succeed"), FOsvayderUERelayAgentManager::ArchiveTerminalArtifacts(Plan, ArchivedPaths, Error));
	TestEqual(TEXT("terminal archive should preserve five archived artifacts"), ArchivedPaths.Num(), 5);
	for (const FString& ArchivedPath : ArchivedPaths)
	{
		TestTrue(TEXT("each archived relay artifact should exist"), IFileManager::Get().FileExists(*ArchivedPath));
	}

	TestFalse(TEXT("preferred active plan should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("active_plan.json"))));
	TestFalse(TEXT("legacy active plan should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("active_plan.json"))));
	TestFalse(TEXT("preferred relay progress should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("relay_progress.jsonl"))));
	TestFalse(TEXT("legacy relay progress should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("relay_progress.jsonl"))));
	TestFalse(TEXT("preferred relay result should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("relay_result.json"))));
	TestFalse(TEXT("legacy relay result should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("relay_result.json"))));
	TestFalse(TEXT("preferred handoff context should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("handoff_context.json"))));
	TestFalse(TEXT("legacy handoff context should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("handoff_context.json"))));
	TestFalse(TEXT("preferred cancel request should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(PreferredRoot, TEXT("relay_cancel_request.json"))));
	TestFalse(TEXT("legacy cancel request should be absent after archive cleanup"), IFileManager::Get().FileExists(*FPaths::Combine(LegacyRoot, TEXT("relay_cancel_request.json"))));

	FOsvayderUEActivePlan LoadedPlan;
	TestFalse(TEXT("active plan should not reload from legacy after terminal archive"), FOsvayderUERelayAgentManager::LoadActivePlan(LoadedPlan, Error));

	FOsvayderUERelayHandoffContext LoadedContext;
	TestFalse(TEXT("handoff context should not reload from legacy after terminal archive"), FOsvayderUERelayAgentManager::LoadHandoffContext(LoadedContext, Error));

	FOsvayderUERelayResult LoadedResult;
	TestFalse(TEXT("relay result should not reload from legacy after terminal archive"), FOsvayderUERelayAgentManager::LoadRelayResult(LoadedResult, Error));

	FOsvayderUERelayProgressEntry LoadedProgress;
	TestFalse(TEXT("relay progress should not reload from legacy after terminal archive"), FOsvayderUERelayAgentManager::LoadLatestProgressEntry(LoadedProgress, Error));
	TestFalse(TEXT("cancel request should not reload from legacy after terminal archive"), FOsvayderUERelayAgentManager::HasCancelRequest());
	return true;
}

bool FStorageNamespaceMigration_AgentTraceMigratesAndAppends::RunTest(const FString& Parameters)
{
	FString PreferredRoot;
	FString LegacyRoot;
	MakeFreshManagedRoots(TEXT("AgentTrace"), PreferredRoot, LegacyRoot);
	const FString PreferredTracePath = FPaths::Combine(PreferredRoot, TEXT("agent_trace.jsonl"));
	const FString LegacyTracePath = FPaths::Combine(LegacyRoot, TEXT("agent_trace.jsonl"));

	{
		FScopedTraceLogPathOverride LegacyOverride(LegacyTracePath);
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("summary"), TEXT("legacy trace entry"));
		FOsvayderUEAgentTraceLog::Get().AppendEvent(
			TEXT("legacy_event"),
			EOsvayderUEProviderBackend::CodexCli,
			Payload,
			TEXT("run_storage_migration"));
	}

	FScopedTraceLogPathOverride PreferredOverride(PreferredTracePath);
	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = TEXT("run_storage_migration");
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 10;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	TArray<TSharedPtr<FJsonObject>> Events = FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);
	TestEqual(TEXT("legacy trace should migrate and load"), TotalLoaded, 1);
	TestTrue(TEXT("preferred trace log should exist after migration"), IFileManager::Get().FileExists(*PreferredTracePath));
	TestTrue(TEXT("legacy trace log should remain"), IFileManager::Get().FileExists(*LegacyTracePath));

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("summary"), TEXT("preferred trace entry"));
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("preferred_event"),
		EOsvayderUEProviderBackend::CodexCli,
		Payload,
		TEXT("run_storage_migration"));

	Events = FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);
	TestEqual(TEXT("trace query should include legacy and new preferred event"), TotalLoaded, 2);
	return true;
}

bool FStorageNamespaceMigration_ScopePolicyAllowsLegacyAndPreferredRoots::RunTest(const FString& Parameters)
{
	FScopedScopeModeOverride ScopeModeOverride(EOsvayderUEScopeMode::PluginOnly);

	const FOsvayderUEScopePolicy::FScopeCheckResult PreferredResult =
		FOsvayderUEScopePolicy::IsWriteAllowed(TEXT("Saved/OsvayderUE/migrated_state.json"));
	const FOsvayderUEScopePolicy::FScopeCheckResult LegacyResult =
		FOsvayderUEScopePolicy::IsWriteAllowed(TEXT("Saved/OsvayderUE/legacy_state.json"));

	TestTrue(TEXT("preferred saved namespace should be write-allowed"), PreferredResult.bAllowed);
	TestEqual(TEXT("preferred saved namespace should classify as internal state"), PreferredResult.Classification, EScopeClassification::InternalState);
	TestTrue(TEXT("legacy saved namespace should remain write-allowed"), LegacyResult.bAllowed);
	TestEqual(TEXT("legacy saved namespace should classify as internal state"), LegacyResult.Classification, EScopeClassification::InternalState);
	return true;
}

bool FStorageNamespaceMigration_RepairFailurePreservesPreferredArtifact::RunTest(const FString& Parameters)
{
	FString PreferredRoot;
	FString LegacyRoot;
	MakeFreshManagedRoots(TEXT("RepairFailurePreservesPreferredArtifact"), PreferredRoot, LegacyRoot);
	FScopedCopyFailureModeOverride FailureModeOverride(
		OsvayderUEStorageMigration::ETestCopyFailureMode::FailBeforePreferredReplace);

	const FString PreferredPath = FPaths::Combine(PreferredRoot, TEXT("session_codex_cli.json"));
	const FString LegacyPath = FPaths::Combine(LegacyRoot, TEXT("session_codex_cli.json"));
	const FString OriginalPreferredContents = TEXT("{ invalid preferred json");
	TestTrue(TEXT("invalid preferred fixture should save"), SaveTextFile(PreferredPath, OriginalPreferredContents));
	TestTrue(TEXT("valid legacy fixture should save"), SaveJsonFile(
		LegacyPath,
		MakeSessionFixtureJson(TEXT("normal_provider_session"), TEXT("legacy provider prompt"), TEXT("legacy provider reply"))));

	FString ResolveError;
	OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
	TestTrue(TEXT("managed read should fall back to legacy when repair fails"),
		OsvayderUEStorageMigration::ResolveManagedReadPath(
			PreferredPath,
			LegacyPath,
			TEXT("repair_failure_preserves_preferred_artifact"),
			[](const FString& CandidatePath, FString& OutValidationError)
			{
				FString JsonText;
				if (!FFileHelper::LoadFileToString(JsonText, *CandidatePath))
				{
					OutValidationError = FString::Printf(TEXT("Could not read %s"), *CandidatePath);
					return false;
				}

				TSharedPtr<FJsonObject> RootObject;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
				if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
				{
					OutValidationError = FString::Printf(TEXT("Could not parse %s"), *CandidatePath);
					return false;
				}

				return true;
			},
			ManagedRead,
			ResolveError));

	TestTrue(TEXT("managed read should report legacy fallback usage"), ManagedRead.bUsedLegacyFallback);
	TestEqual(TEXT("resolved path should remain the legacy artifact after failed repair"), NormalizeFilePathForTest(ManagedRead.ResolvedPath), NormalizeFilePathForTest(LegacyPath));
	TestTrue(TEXT("preferred artifact should remain after failed repair"), IFileManager::Get().FileExists(*PreferredPath));

	FString PreferredContentsAfterFailure;
	TestTrue(TEXT("preferred artifact should remain readable after failed repair"), LoadTextFile(PreferredPath, PreferredContentsAfterFailure));
	TestEqual(TEXT("preferred artifact contents should remain untouched after failed repair"), PreferredContentsAfterFailure, OriginalPreferredContents);

	const FString TempPattern = PreferredPath + TEXT(".*.tmp");
	TArray<FString> TempFiles;
	IFileManager::Get().FindFiles(TempFiles, *TempPattern, true, false);
	TestEqual(TEXT("failed repair should not leak temp files in the preferred directory"), TempFiles.Num(), 0);
	return true;
}

#endif
