// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "AgentOrchestrator.h"
#include "ClaudeSessionManager.h"
#include "MCP/Tools/MCPTool_PluginSettings.h"
#include "UnrealClaudeSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString NormalizeDirectoryPathForTest(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return FString();
		}

		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	FString MakeFreshTestRoot(const FString& TestName)
	{
		FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UnrealClaude"),
			TEXT("Automation"),
			TEXT("SessionRestore"),
			TestName);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	TSharedPtr<FJsonObject> GetObjectFieldOrNull(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* NestedObject = nullptr;
		if (!Object->TryGetObjectField(FieldName, NestedObject) || !NestedObject || !(*NestedObject).IsValid())
		{
			return nullptr;
		}

		return *NestedObject;
	}

	FAgentBackendStatus MakeBackendStatus(const EUnrealClaudeProviderBackend Backend)
	{
		FAgentBackendStatus Status;
		Status.Backend = Backend;
		Status.DisplayName = Backend == EUnrealClaudeProviderBackend::CodexCli
			? TEXT("Codex CLI")
			: TEXT("Claude CLI");
		Status.Capabilities.DisplayName = Status.DisplayName;
		return Status;
	}

	struct FScopedSessionSaveDirOverride
	{
		explicit FScopedSessionSaveDirOverride(const FString& InDir)
		{
			FClaudeSessionManager::SetTestSessionSaveDirOverride(InDir);
		}

		~FScopedSessionSaveDirOverride()
		{
			FClaudeSessionManager::ClearTestSessionSaveDirOverride();
		}
	};

	struct FScopedPreferredBackendOverride
	{
		UUnrealClaudeSettings* Settings = nullptr;
		EUnrealClaudeProviderBackend OriginalBackend = EUnrealClaudeProviderBackend::ClaudeCli;

		explicit FScopedPreferredBackendOverride(const EUnrealClaudeProviderBackend InBackend)
		{
			Settings = UUnrealClaudeSettings::GetMutable();
			if (Settings)
			{
				OriginalBackend = Settings->PreferredBackend;
				Settings->PreferredBackend = InBackend;
			}
		}

		~FScopedPreferredBackendOverride()
		{
			if (Settings)
			{
				Settings->PreferredBackend = OriginalBackend;
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionRestore_ProjectLocalVisibleReadbackTruth,
	"UnrealClaude.SessionRestore.ProjectLocalVisibleReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSessionRestore_ProjectLocalVisibleReadbackTruth::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("ProjectLocalVisibleReadback"));
	const FString SessionSaveDir = FPaths::Combine(TestRoot, TEXT("ProjectA"), TEXT("UnrealClaude"));

	FScopedPreferredBackendOverride PreferredBackendOverride(EUnrealClaudeProviderBackend::CodexCli);
	FScopedSessionSaveDirOverride SessionSaveDirOverride(SessionSaveDir);

	FClaudeSessionManager SeedManager;
	SeedManager.AddExchange(EUnrealClaudeProviderBackend::CodexCli, TEXT("visible_user"), TEXT("visible_assistant"));
	TestTrue(TEXT("project-local visible session should save"), SeedManager.SaveVisibleSession(
		EUnrealClaudeProviderBackend::CodexCli,
		MakeBackendStatus(EUnrealClaudeProviderBackend::CodexCli)));
	SeedManager.AddProviderSessionExchange(EUnrealClaudeProviderBackend::CodexCli, TEXT("expert_user"), TEXT("expert_assistant"));
	TestTrue(TEXT("expert provider session should save"), SeedManager.SaveSession(
		EUnrealClaudeProviderBackend::CodexCli,
		MakeBackendStatus(EUnrealClaudeProviderBackend::CodexCli)));

	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> BackendObject = GetObjectFieldOrNull(Result.Data, TEXT("assistant_backend"));
	const TSharedPtr<FJsonObject> SessionObject = GetObjectFieldOrNull(BackendObject, TEXT("session"));
	const TSharedPtr<FJsonObject> CurrentProviderObject = GetObjectFieldOrNull(SessionObject, TEXT("current_provider"));
	const TSharedPtr<FJsonObject> ExpertProviderObject = GetObjectFieldOrNull(SessionObject, TEXT("explicit_expert_opt_in_provider"));
	TestTrue(TEXT("assistant_backend should exist"), BackendObject.IsValid());
	TestTrue(TEXT("session should exist"), SessionObject.IsValid());
	TestTrue(TEXT("current_provider should exist"), CurrentProviderObject.IsValid());
	TestTrue(TEXT("explicit_expert_opt_in_provider should exist"), ExpertProviderObject.IsValid());
	if (!BackendObject.IsValid() || !SessionObject.IsValid() || !CurrentProviderObject.IsValid() || !ExpertProviderObject.IsValid())
	{
		return false;
	}

	FString CurrentProviderStoreKind;
	FString CurrentProviderPath;
	FString ExpertProviderStoreKind;
	FString ExpertProviderPath;
	TestTrue(TEXT("current_provider.store_kind should exist"), CurrentProviderObject->TryGetStringField(TEXT("store_kind"), CurrentProviderStoreKind));
	TestTrue(TEXT("current_provider.path should exist"), CurrentProviderObject->TryGetStringField(TEXT("path"), CurrentProviderPath));
	TestTrue(TEXT("explicit_expert_opt_in_provider.store_kind should exist"), ExpertProviderObject->TryGetStringField(TEXT("store_kind"), ExpertProviderStoreKind));
	TestTrue(TEXT("explicit_expert_opt_in_provider.path should exist"), ExpertProviderObject->TryGetStringField(TEXT("path"), ExpertProviderPath));

	TestEqual(TEXT("configured default readback should expose the project-local visible store kind"),
		CurrentProviderStoreKind,
		FString(TEXT("project_local_visible_restore")));
	TestEqual(TEXT("configured default readback should expose the visible session path"),
		NormalizeDirectoryPathForTest(CurrentProviderPath),
		NormalizeDirectoryPathForTest(SeedManager.GetVisibleSessionFilePath(EUnrealClaudeProviderBackend::CodexCli)));
	TestEqual(TEXT("explicit expert readback should stay normal provider session"),
		ExpertProviderStoreKind,
		FString(TEXT("normal_provider_session")));
	TestEqual(TEXT("explicit expert readback should keep the provider-session path"),
		NormalizeDirectoryPathForTest(ExpertProviderPath),
		NormalizeDirectoryPathForTest(SeedManager.GetSessionFilePath(EUnrealClaudeProviderBackend::CodexCli)));
	TestTrue(TEXT("configured default visible session path should differ from explicit expert provider path"),
		!NormalizeDirectoryPathForTest(CurrentProviderPath).Equals(
			NormalizeDirectoryPathForTest(ExpertProviderPath),
			ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionRestore_ConfiguredDefaultLoadsVisibleStore,
	"UnrealClaude.SessionRestore.ConfiguredDefaultLoadsVisibleStore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSessionRestore_ConfiguredDefaultLoadsVisibleStore::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("ConfiguredDefaultLoadsVisibleStore"));
	const FString SessionSaveDir = FPaths::Combine(TestRoot, TEXT("ProjectA"), TEXT("UnrealClaude"));

	FScopedPreferredBackendOverride PreferredBackendOverride(EUnrealClaudeProviderBackend::CodexCli);
	FScopedSessionSaveDirOverride SessionSaveDirOverride(SessionSaveDir);

	FClaudeSessionManager SeedManager;
	SeedManager.AddExchange(EUnrealClaudeProviderBackend::CodexCli, TEXT("visible_user"), TEXT("visible_assistant"));
	TestTrue(TEXT("project-local visible session should save"), SeedManager.SaveVisibleSession(
		EUnrealClaudeProviderBackend::CodexCli,
		MakeBackendStatus(EUnrealClaudeProviderBackend::CodexCli)));
	SeedManager.AddProviderSessionExchange(EUnrealClaudeProviderBackend::CodexCli, TEXT("expert_user"), TEXT("expert_assistant"));
	TestTrue(TEXT("expert provider session should save"), SeedManager.SaveSession(
		EUnrealClaudeProviderBackend::CodexCli,
		MakeBackendStatus(EUnrealClaudeProviderBackend::CodexCli)));

	FAgentOrchestrator Orchestrator;
	const FAgentSessionRestoreResult RestoreResult = Orchestrator.LoadSessionWithResult();
	TestTrue(TEXT("configured default restore should load"), RestoreResult.WasLoaded());
	TestEqual(TEXT("configured default restore should use the project-local visible store"),
		RestoreResult.RequestedSession.StoreKind,
		FString(TEXT("project_local_visible_restore")));
	TestEqual(TEXT("configured default restore should load from the visible session path"),
		NormalizeDirectoryPathForTest(RestoreResult.RequestedSession.SessionFilePath),
		NormalizeDirectoryPathForTest(SeedManager.GetVisibleSessionFilePath(EUnrealClaudeProviderBackend::CodexCli)));

	const TArray<TPair<FString, FString>>& History = Orchestrator.GetHistory();
	TestEqual(TEXT("configured default restore should recover one visible exchange"), History.Num(), 1);
	if (History.Num() != 1)
	{
		return false;
	}

	TestEqual(TEXT("configured default restore should recover the visible user prompt"), History[0].Key, FString(TEXT("visible_user")));
	TestEqual(TEXT("configured default restore should recover the visible assistant response"), History[0].Value, FString(TEXT("visible_assistant")));

	const FAgentSavedSessionIndex ExpertSessions = Orchestrator.DescribeSavedSessionsForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);
	TestTrue(TEXT("explicit expert provider session should still exist"), ExpertSessions.CurrentProviderSession.bHasSession);
	TestEqual(TEXT("explicit expert provider session should keep the expert store kind"),
		ExpertSessions.CurrentProviderSession.StoreKind,
		FString(TEXT("normal_provider_session")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionRestore_ProjectLocalIsolationAcrossRoots,
	"UnrealClaude.SessionRestore.ProjectLocalIsolationAcrossRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSessionRestore_ProjectLocalIsolationAcrossRoots::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("ProjectLocalIsolationAcrossRoots"));
	const FString ProjectARoot = FPaths::Combine(TestRoot, TEXT("ProjectA"), TEXT("UnrealClaude"));
	const FString ProjectBRoot = FPaths::Combine(TestRoot, TEXT("ProjectB"), TEXT("UnrealClaude"));

	FScopedPreferredBackendOverride PreferredBackendOverride(EUnrealClaudeProviderBackend::CodexCli);

	{
		FScopedSessionSaveDirOverride SessionSaveDirOverride(ProjectARoot);
		FClaudeSessionManager SeedManager;
		SeedManager.AddExchange(EUnrealClaudeProviderBackend::CodexCli, TEXT("project_a_user"), TEXT("project_a_assistant"));
		TestTrue(TEXT("project A visible session should save"), SeedManager.SaveVisibleSession(
			EUnrealClaudeProviderBackend::CodexCli,
			MakeBackendStatus(EUnrealClaudeProviderBackend::CodexCli)));
	}

	{
		FScopedSessionSaveDirOverride SessionSaveDirOverride(ProjectBRoot);
		FClaudeSessionManager SeedManager;
		SeedManager.AddExchange(EUnrealClaudeProviderBackend::CodexCli, TEXT("project_b_user"), TEXT("project_b_assistant"));
		TestTrue(TEXT("project B visible session should save"), SeedManager.SaveVisibleSession(
			EUnrealClaudeProviderBackend::CodexCli,
			MakeBackendStatus(EUnrealClaudeProviderBackend::CodexCli)));
	}

	{
		FScopedSessionSaveDirOverride SessionSaveDirOverride(ProjectARoot);
		FAgentOrchestrator ProjectAOrchestrator;
		const FAgentSessionRestoreResult RestoreResult = ProjectAOrchestrator.LoadSessionWithResult();
		TestTrue(TEXT("project A restore should load"), RestoreResult.WasLoaded());
		const TArray<TPair<FString, FString>>& History = ProjectAOrchestrator.GetHistory();
		TestEqual(TEXT("project A restore should recover exactly one exchange"), History.Num(), 1);
		if (History.Num() != 1)
		{
			return false;
		}

		TestEqual(TEXT("project A restore should recover only project A history"), History[0].Key, FString(TEXT("project_a_user")));
		TestEqual(TEXT("project A restore should recover only project A assistant response"), History[0].Value, FString(TEXT("project_a_assistant")));
	}

	{
		FScopedSessionSaveDirOverride SessionSaveDirOverride(ProjectBRoot);
		FAgentOrchestrator ProjectBOrchestrator;
		const FAgentSessionRestoreResult RestoreResult = ProjectBOrchestrator.LoadSessionWithResult();
		TestTrue(TEXT("project B restore should load"), RestoreResult.WasLoaded());
		const TArray<TPair<FString, FString>>& History = ProjectBOrchestrator.GetHistory();
		TestEqual(TEXT("project B restore should recover exactly one exchange"), History.Num(), 1);
		if (History.Num() != 1)
		{
			return false;
		}

		TestEqual(TEXT("project B restore should recover only project B history"), History[0].Key, FString(TEXT("project_b_user")));
		TestEqual(TEXT("project B restore should recover only project B assistant response"), History[0].Value, FString(TEXT("project_b_assistant")));
	}

	TestTrue(TEXT("project-local save roots should differ"),
		!NormalizeDirectoryPathForTest(ProjectARoot).Equals(
			NormalizeDirectoryPathForTest(ProjectBRoot),
			ESearchCase::IgnoreCase));
	return true;
}

#endif
