// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "AgentPromptContract.h"
#include "ClaudeCodeRunner.h"
#include "ClaudeSessionManager.h"
#include "UnrealClaudeSettings.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ClaudeSessionManagerTests
{
	FString MakeFreshSandbox(const FString& LeafName)
	{
		FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UnrealClaude"),
			TEXT("Automation"),
			TEXT("ClaudeSessionManager"),
			LeafName);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
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

	struct FScopedNativeProjectsRootOverride
	{
		explicit FScopedNativeProjectsRootOverride(const FString& InDir)
		{
			FClaudeSessionManager::SetTestNativeProjectsRootOverride(InDir);
		}

		~FScopedNativeProjectsRootOverride()
		{
			FClaudeSessionManager::ClearTestNativeProjectsRootOverride();
		}
	};

	struct FScopedClaudePersistentSessionToggle
	{
		UUnrealClaudeSettings* Settings = nullptr;
		bool OriginalPersistentSession = true;
		bool OriginalForwardLanguage = true;
		EUnrealClaudeDictationLanguage OriginalLanguage = EUnrealClaudeDictationLanguage::Auto;
		FString OriginalModel;
		EUnrealClaudeClaudeEffortLevel OriginalEffort = EUnrealClaudeClaudeEffortLevel::Default;

		FScopedClaudePersistentSessionToggle()
		{
			Settings = UUnrealClaudeSettings::GetMutable();
			if (Settings)
			{
				OriginalPersistentSession = Settings->bClaudeUsePersistentSession;
				OriginalForwardLanguage = Settings->bClaudeForwardLanguageToSystemPrompt;
				OriginalLanguage = Settings->DefaultDictationLanguage;
				OriginalModel = Settings->DefaultModel;
				OriginalEffort = Settings->DefaultClaudeEffortLevel;
			}
		}

		~FScopedClaudePersistentSessionToggle()
		{
			if (Settings)
			{
				Settings->bClaudeUsePersistentSession = OriginalPersistentSession;
				Settings->bClaudeForwardLanguageToSystemPrompt = OriginalForwardLanguage;
				Settings->DefaultDictationLanguage = OriginalLanguage;
				Settings->DefaultModel = OriginalModel;
				Settings->DefaultClaudeEffortLevel = OriginalEffort;
			}
		}
	};

	bool ArtifactHasFields(
		FAutomationTestBase& Test,
		const FString& ArtifactPath,
		const FString& ExpectedSessionId,
		const FString& ExpectedModel)
	{
		FString FileContents;
		if (!FFileHelper::LoadFileToString(FileContents, *ArtifactPath))
		{
			Test.AddError(FString::Printf(TEXT("Expected artifact to exist and be readable: %s"), *ArtifactPath));
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContents);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("Failed to parse artifact JSON at: %s"), *ArtifactPath));
			return false;
		}

		double SchemaVersion = 0.0;
		Test.TestTrue(TEXT("artifact has schema_version"), Root->TryGetNumberField(TEXT("schema_version"), SchemaVersion));
		Test.TestEqual(TEXT("artifact schema_version is 1"), static_cast<int32>(SchemaVersion), 1);

		FString SessionId;
		Test.TestTrue(TEXT("artifact has session_id"), Root->TryGetStringField(TEXT("session_id"), SessionId));
		if (!ExpectedSessionId.IsEmpty())
		{
			Test.TestEqual(TEXT("artifact session_id matches expected"), SessionId, ExpectedSessionId);
		}

		FString Model;
		Test.TestTrue(TEXT("artifact has model"), Root->TryGetStringField(TEXT("model"), Model));
		Test.TestEqual(TEXT("artifact model matches expected"), Model, ExpectedModel);

		FString CreatedUtc;
		Test.TestTrue(TEXT("artifact has created_utc"), Root->TryGetStringField(TEXT("created_utc"), CreatedUtc));
		Test.TestFalse(TEXT("created_utc is non-empty"), CreatedUtc.IsEmpty());

		FString LastUsedUtc;
		Test.TestTrue(TEXT("artifact has last_used_utc"), Root->TryGetStringField(TEXT("last_used_utc"), LastUsedUtc));
		Test.TestFalse(TEXT("last_used_utc is non-empty"), LastUsedUtc.IsEmpty());

		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_GetOrCreate_FirstCall_CreatesUuidAndArtifact,
	"UnrealClaude.ClaudeSession.GetOrCreate_FirstCall_CreatesUuidAndArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_GetOrCreate_FirstCall_CreatesUuidAndArtifact::RunTest(const FString& Parameters)
{
	using namespace ClaudeSessionManagerTests;

	const FString Sandbox = MakeFreshSandbox(TEXT("FirstCall"));
	FScopedSessionSaveDirOverride Override(Sandbox);

	FClaudeSessionManager Manager;
	bool bWasExisting = true;
	const FString Model = TEXT("claude-opus-4-7");
	const FString Id = Manager.GetOrCreatePersistentSessionId(EUnrealClaudeProviderBackend::ClaudeCli, Model, &bWasExisting);

	TestFalse(TEXT("first-call session id should be non-empty"), Id.IsEmpty());
	TestFalse(TEXT("first-call bWasExisting should be false"), bWasExisting);

	// UUID v4 shape: 8-4-4-4-12 lowercase hex, 36 chars total.
	TestEqual(TEXT("session id length is 36 (UUID v4 dashed form)"), Id.Len(), 36);
	TestTrue(TEXT("session id contains dashes at positions 8/13/18/23"),
		Id[8] == TEXT('-') && Id[13] == TEXT('-') && Id[18] == TEXT('-') && Id[23] == TEXT('-'));

	const FString ArtifactPath = Manager.GetPersistentSessionFilePath(EUnrealClaudeProviderBackend::ClaudeCli);
	TestTrue(TEXT("artifact file exists after first call"), IFileManager::Get().FileExists(*ArtifactPath));
	ArtifactHasFields(*this, ArtifactPath, Id, Model);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_GetOrCreate_SecondCall_ReturnsSameUuid,
	"UnrealClaude.ClaudeSession.GetOrCreate_SecondCall_ReturnsSameUuid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_GetOrCreate_SecondCall_ReturnsSameUuid::RunTest(const FString& Parameters)
{
	using namespace ClaudeSessionManagerTests;

	const FString Sandbox = MakeFreshSandbox(TEXT("SecondCall"));
	FScopedSessionSaveDirOverride Override(Sandbox);

	FClaudeSessionManager Manager;
	const FString Model = TEXT("claude-opus-4-7");

	bool bWasExistingFirst = true;
	const FString IdFirst = Manager.GetOrCreatePersistentSessionId(EUnrealClaudeProviderBackend::ClaudeCli, Model, &bWasExistingFirst);
	TestFalse(TEXT("first call should report no existing artifact"), bWasExistingFirst);
	TestFalse(TEXT("first call id non-empty"), IdFirst.IsEmpty());

	bool bWasExistingSecond = false;
	const FString IdSecond = Manager.GetOrCreatePersistentSessionId(EUnrealClaudeProviderBackend::ClaudeCli, Model, &bWasExistingSecond);
	TestTrue(TEXT("second call should report existing artifact"), bWasExistingSecond);
	TestEqual(TEXT("second call returns the same uuid as the first"), IdSecond, IdFirst);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_ModelChange_InvalidatesSession,
	"UnrealClaude.ClaudeSession.ModelChange_InvalidatesSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_ModelChange_InvalidatesSession::RunTest(const FString& Parameters)
{
	using namespace ClaudeSessionManagerTests;

	const FString Sandbox = MakeFreshSandbox(TEXT("ModelChange"));
	FScopedSessionSaveDirOverride Override(Sandbox);

	FClaudeSessionManager Manager;
	const FString ModelA = TEXT("claude-opus-4-7");
	const FString ModelB = TEXT("claude-opus-4-6");

	const FString IdA = Manager.GetOrCreatePersistentSessionId(EUnrealClaudeProviderBackend::ClaudeCli, ModelA);
	TestFalse(TEXT("first id (model A) non-empty"), IdA.IsEmpty());

	bool bWasExistingAfterChange = true;
	const FString IdB = Manager.GetOrCreatePersistentSessionId(EUnrealClaudeProviderBackend::ClaudeCli, ModelB, &bWasExistingAfterChange);
	TestFalse(TEXT("new id (model B) non-empty"), IdB.IsEmpty());
	TestNotEqual(TEXT("model change should mint a fresh uuid"), IdB, IdA);
	TestFalse(TEXT("model-change path reports no existing artifact after reset"), bWasExistingAfterChange);

	const FString ArtifactPath = Manager.GetPersistentSessionFilePath(EUnrealClaudeProviderBackend::ClaudeCli);
	ArtifactHasFields(*this, ArtifactPath, IdB, ModelB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_ResetPersistentSession_DeletesArtifact,
	"UnrealClaude.ClaudeSession.ResetPersistentSession_DeletesArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_ResetPersistentSession_DeletesArtifact::RunTest(const FString& Parameters)
{
	using namespace ClaudeSessionManagerTests;

	const FString Sandbox = MakeFreshSandbox(TEXT("Reset"));
	FScopedSessionSaveDirOverride Override(Sandbox);

	FClaudeSessionManager Manager;
	const FString Id = Manager.GetOrCreatePersistentSessionId(EUnrealClaudeProviderBackend::ClaudeCli, TEXT("claude-opus-4-7"));
	TestFalse(TEXT("seeded id non-empty"), Id.IsEmpty());

	const FString ArtifactPath = Manager.GetPersistentSessionFilePath(EUnrealClaudeProviderBackend::ClaudeCli);
	TestTrue(TEXT("artifact exists before reset"), IFileManager::Get().FileExists(*ArtifactPath));

	Manager.ResetPersistentSession(EUnrealClaudeProviderBackend::ClaudeCli);
	TestFalse(TEXT("artifact is gone after reset"), IFileManager::Get().FileExists(*ArtifactPath));

	// Reset again should be idempotent (no crash, no error).
	Manager.ResetPersistentSession(EUnrealClaudeProviderBackend::ClaudeCli);
	TestFalse(TEXT("artifact is still gone after idempotent second reset"), IFileManager::Get().FileExists(*ArtifactPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_BuildCommandLine_IncludesSessionIdOnFirstRun,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.BuildCommandLine_IncludesSessionIdOnFirstRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_BuildCommandLine_IncludesSessionIdOnFirstRun::RunTest(const FString& Parameters)
{
	const FString TestId = TEXT("11111111-1111-1111-1111-111111111111");
	const FString Fragment = FClaudeCodeRunner::ComposePersistentSessionFlags(true, TestId, false);
	TestFalse(TEXT("fragment should be non-empty when enabled with fresh session"), Fragment.IsEmpty());
	TestTrue(TEXT("first-turn fragment must include --session-id"), Fragment.Contains(TEXT("--session-id")));
	TestFalse(TEXT("first-turn fragment must NOT include --resume"), Fragment.Contains(TEXT("--resume")));
	TestTrue(TEXT("first-turn fragment should contain the uuid"), Fragment.Contains(TestId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_BuildCommandLine_IncludesResumeWhenArtifactExists,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.BuildCommandLine_IncludesResumeWhenArtifactExists",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_BuildCommandLine_IncludesResumeWhenArtifactExists::RunTest(const FString& Parameters)
{
	const FString TestId = TEXT("22222222-2222-2222-2222-222222222222");
	const FString Fragment = FClaudeCodeRunner::ComposePersistentSessionFlags(true, TestId, true);
	TestFalse(TEXT("fragment should be non-empty when resuming"), Fragment.IsEmpty());
	TestTrue(TEXT("subsequent-turn fragment must include --resume"), Fragment.Contains(TEXT("--resume")));
	TestFalse(TEXT("subsequent-turn fragment must NOT include --session-id"), Fragment.Contains(TEXT("--session-id")));
	TestTrue(TEXT("subsequent-turn fragment should contain the uuid"), Fragment.Contains(TestId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_BuildCommandLine_OmitsSessionFlagsWhenSettingDisabled,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.BuildCommandLine_OmitsSessionFlagsWhenSettingDisabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_BuildCommandLine_OmitsSessionFlagsWhenSettingDisabled::RunTest(const FString& Parameters)
{
	const FString TestId = TEXT("33333333-3333-3333-3333-333333333333");
	const FString FragmentOff = FClaudeCodeRunner::ComposePersistentSessionFlags(false, TestId, true);
	TestTrue(TEXT("fragment must be empty when persistent session is disabled"), FragmentOff.IsEmpty());

	const FString FragmentEmptyId = FClaudeCodeRunner::ComposePersistentSessionFlags(true, FString(), true);
	TestTrue(TEXT("fragment must be empty when session id is empty even if enabled"), FragmentEmptyId.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_MaterializeClaudeSystemPrompt_AppendsLanguageLineWhenEnabled,
	"UnrealClaude.AgentPromptContract.MaterializeClaudeSystemPrompt_AppendsLanguageLineWhenEnabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_MaterializeClaudeSystemPrompt_AppendsLanguageLineWhenEnabled::RunTest(const FString& Parameters)
{
	FAgentPromptContract Contract;
	Contract.AgentIdentity = TEXT("test-identity");

	const FString WithRussian = FAgentPromptMaterializer::MaterializeClaudeSystemPrompt(Contract, TEXT("Russian"));
	TestTrue(TEXT("materialized prompt still carries the canonical body"), WithRussian.Contains(TEXT("test-identity")));
	TestTrue(TEXT("Russian language directive line is appended"),
		WithRussian.Contains(TEXT("The user's preferred interaction language is Russian.")));
	TestTrue(TEXT("language directive preserves fallback clause for source code"),
		WithRussian.Contains(TEXT("unless the user explicitly asks otherwise or the content is source code.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_MaterializeClaudeSystemPrompt_NoLanguageLineWhenDisabled,
	"UnrealClaude.AgentPromptContract.MaterializeClaudeSystemPrompt_NoLanguageLineWhenDisabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_MaterializeClaudeSystemPrompt_NoLanguageLineWhenDisabled::RunTest(const FString& Parameters)
{
	FAgentPromptContract Contract;
	Contract.AgentIdentity = TEXT("test-identity");

	const FString Empty = FAgentPromptMaterializer::MaterializeClaudeSystemPrompt(Contract, FString());
	TestTrue(TEXT("empty language display name leaves canonical body intact"), Empty.Contains(TEXT("test-identity")));
	TestFalse(TEXT("empty language display name must not append language directive"),
		Empty.Contains(TEXT("The user's preferred interaction language is")));

	// Equivalence with the 1-arg overload.
	const FString OneArg = FAgentPromptMaterializer::MaterializeClaudeSystemPrompt(Contract);
	TestEqual(TEXT("empty language overload should equal the 1-arg overload"), Empty, OneArg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_BuildCommandLine_AppendsLanguageSystemPromptOnResume,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.BuildCommandLine_AppendsLanguageSystemPromptOnResume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_BuildCommandLine_AppendsLanguageSystemPromptOnResume::RunTest(const FString& Parameters)
{
	// Spec 621 reviewer 2026-04-17 14:10 fix [A1-2]: on --resume turns Claude CLI does not re-apply the session's
	// original system prompt. The per-turn --append-system-prompt fragment must ride every invocation so the
	// language directive actually reaches the model after session reattach.
	const FString TestId = TEXT("44444444-4444-4444-4444-444444444444");
	const FString ResumeFragment = FClaudeCodeRunner::ComposePersistentSessionFlags(true, TestId, /*bWasExisting=*/true);
	const FString LanguageFragment = FClaudeCodeRunner::ComposeLanguageHintFlag(TEXT("Russian"));

	// Concatenation matches the real BuildCommandLine order for this pair.
	const FString CommandLine = ResumeFragment + LanguageFragment;

	TestTrue(TEXT("resume fragment must carry --resume"), CommandLine.Contains(TEXT("--resume")));
	TestTrue(TEXT("resume fragment must carry the expected uuid"), CommandLine.Contains(TestId));
	TestTrue(TEXT("language hint must emit --append-system-prompt on resume turns"),
		CommandLine.Contains(TEXT("--append-system-prompt")));
	TestTrue(TEXT("language hint body must name the configured language"),
		CommandLine.Contains(TEXT("preferred interaction language is Russian")));
	TestTrue(TEXT("language hint body must preserve the source-code fallback clause"),
		CommandLine.Contains(TEXT("unless the user explicitly asks otherwise or the content is source code.")));

	// Disabled branch still returns empty so persistent-session OFF semantics are untouched.
	const FString EmptyLanguage = FClaudeCodeRunner::ComposeLanguageHintFlag(FString());
	TestTrue(TEXT("empty language display name must not emit --append-system-prompt"),
		EmptyLanguage.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_ComputeClaudeNativeProjectKey_EncodesDriveAndSlashes,
	"UnrealClaude.ClaudeSession.ComputeClaudeNativeProjectKey_EncodesDriveAndSlashes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_ComputeClaudeNativeProjectKey_EncodesDriveAndSlashes::RunTest(const FString& Parameters)
{
	// Spec 621 v3 §2a: the encoding is the single point most likely to silently drift. Observed
	// Claude CLI mapping: D:/VibeCode/Unreal/Poligon/Poligon1 -> D--VibeCode-Unreal-Poligon-Poligon1
	// (`:` -> `--`, `/` and `\` -> `-`, trailing slash/backslash dropped, drive-letter case preserved).
	const FString ExpectedPoligon1Key = TEXT("D--VibeCode-Unreal-Poligon-Poligon1");

	TestEqual(TEXT("forward-slash input encodes to the observed Claude CLI project key"),
		FClaudeSessionManager::ComputeClaudeNativeProjectKey(TEXT("D:/VibeCode/Unreal/Poligon/Poligon1")),
		ExpectedPoligon1Key);

	TestEqual(TEXT("windows-style backslash input encodes to the same project key"),
		FClaudeSessionManager::ComputeClaudeNativeProjectKey(TEXT("D:\\VibeCode\\Unreal\\Poligon\\Poligon1")),
		ExpectedPoligon1Key);

	TestEqual(TEXT("trailing forward slash is trimmed before encoding"),
		FClaudeSessionManager::ComputeClaudeNativeProjectKey(TEXT("D:/VibeCode/Unreal/Poligon/Poligon1/")),
		ExpectedPoligon1Key);

	TestEqual(TEXT("trailing backslash is trimmed before encoding"),
		FClaudeSessionManager::ComputeClaudeNativeProjectKey(TEXT("D:\\VibeCode\\Unreal\\Poligon\\Poligon1\\")),
		ExpectedPoligon1Key);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_ResetPersistentSession_DeletesMatchingNativeJsonl,
	"UnrealClaude.ClaudeSession.ResetPersistentSession_DeletesMatchingNativeJsonl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_ResetPersistentSession_DeletesMatchingNativeJsonl::RunTest(const FString& Parameters)
{
	using namespace ClaudeSessionManagerTests;

	// Spec 621 v3 §2a: when ResetPersistentSession rotates away from UUID X, it must UUID-match-delete
	// the corresponding `<native-projects-root>/<project-key>/X.jsonl`, and it must NOT touch any other
	// jsonl in that directory (the shared claude-desktop namespace lives there too).
	const FString Sandbox = MakeFreshSandbox(TEXT("ResetDeletesNativeJsonl"));
	FScopedSessionSaveDirOverride Override(Sandbox);

	// Point the native-projects root at a sibling of the session-save dir, both inside the sandbox so
	// FUnrealClaudeScopePolicy allows writes (Saved/UnrealClaude is classified as internal_state).
	const FString NativeProjectsRoot = FPaths::Combine(Sandbox, TEXT(".claude"), TEXT("projects"));
	IFileManager::Get().MakeDirectory(*NativeProjectsRoot, true);
	FScopedNativeProjectsRootOverride NativeOverride(NativeProjectsRoot);

	FClaudeSessionManager Manager;
	const FString Model = TEXT("claude-opus-4-7");

	// Seed the project-local artifact so ResetPersistentSession has a UUID to read before deletion.
	bool bWasExistingSeed = true;
	const FString TargetUuid = Manager.GetOrCreatePersistentSessionId(
		EUnrealClaudeProviderBackend::ClaudeCli, Model, &bWasExistingSeed);
	TestFalse(TEXT("seeded session id non-empty"), TargetUuid.IsEmpty());
	TestFalse(TEXT("seeded session is fresh"), bWasExistingSeed);

	// Compute the project-key the same way the manager will at delete time (real FPaths::ProjectDir()).
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString ProjectKey = FClaudeSessionManager::ComputeClaudeNativeProjectKey(ProjectRoot);
	TestFalse(TEXT("project-key must not be empty under test"), ProjectKey.IsEmpty());

	const FString ProjectKeyDir = FPaths::Combine(NativeProjectsRoot, ProjectKey);
	IFileManager::Get().MakeDirectory(*ProjectKeyDir, true);

	// Write the target jsonl (matching UUID) and a sibling jsonl (unrelated UUID) into the project-key dir.
	const FString TargetJsonlPath = FPaths::Combine(ProjectKeyDir,
		FString::Printf(TEXT("%s.jsonl"), *TargetUuid));
	const FString SiblingUuid = TEXT("ffffffff-ffff-ffff-ffff-ffffffffffff");
	const FString SiblingJsonlPath = FPaths::Combine(ProjectKeyDir,
		FString::Printf(TEXT("%s.jsonl"), *SiblingUuid));

	TestTrue(TEXT("could write fake target jsonl"),
		FFileHelper::SaveStringToFile(TEXT("{\"type\":\"user\"}\n"), *TargetJsonlPath));
	TestTrue(TEXT("could write fake sibling jsonl"),
		FFileHelper::SaveStringToFile(TEXT("{\"type\":\"user\"}\n"), *SiblingJsonlPath));
	TestTrue(TEXT("target jsonl exists before reset"), IFileManager::Get().FileExists(*TargetJsonlPath));
	TestTrue(TEXT("sibling jsonl exists before reset"), IFileManager::Get().FileExists(*SiblingJsonlPath));

	// Rotation: ResetPersistentSession should remove both the project-local artifact and the matching
	// native jsonl; the sibling jsonl must remain (UUID-match narrowing).
	Manager.ResetPersistentSession(EUnrealClaudeProviderBackend::ClaudeCli);

	const FString ArtifactPath = Manager.GetPersistentSessionFilePath(EUnrealClaudeProviderBackend::ClaudeCli);
	TestFalse(TEXT("project-local artifact is gone after reset"),
		IFileManager::Get().FileExists(*ArtifactPath));
	TestFalse(TEXT("UUID-matched native jsonl was deleted"),
		IFileManager::Get().FileExists(*TargetJsonlPath));
	TestTrue(TEXT("sibling native jsonl was preserved (UUID-match narrowing)"),
		IFileManager::Get().FileExists(*SiblingJsonlPath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_HandleResume400_InvalidRequest_TriggersAutoReset,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.HandleResume400_InvalidRequest_TriggersAutoReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_HandleResume400_InvalidRequest_TriggersAutoReset::RunTest(const FString& Parameters)
{
	using namespace ClaudeSessionManagerTests;

	// Spec 621 reviewer 2026-04-17 14:10 fix [A1-1]: Anthropic API 400 invalid_request_error during --resume
	// indicates a poisoned native-store message was replayed. ShouldAutoResetOnExit must classify that as a
	// reset trigger, and the subsequent GetOrCreatePersistentSessionId call must rotate the artifact UUID.
	const FString Sandbox = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealClaude"), TEXT("TestSandbox_ResetOn400"));
	IFileManager::Get().DeleteDirectory(*Sandbox, false, true);
	IFileManager::Get().MakeDirectory(*Sandbox, true);
	FScopedSessionSaveDirOverride Override(Sandbox);

	FClaudeSessionManager Manager;

	// Seed artifact on the current model.
	bool bWasExistingSeed = true;
	const FString InitialUuid = Manager.GetOrCreatePersistentSessionId(
		EUnrealClaudeProviderBackend::ClaudeCli, TEXT("claude-opus-4-7"), &bWasExistingSeed);
	TestFalse(TEXT("first call creates fresh artifact"), bWasExistingSeed);
	TestFalse(TEXT("initial uuid is non-empty"), InitialUuid.IsEmpty());

	// Simulated Anthropic 400 output body the CLI surfaces on a poisoned-replay resume.
	const FString Simulated400 = TEXT("API Error: 400 {\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"messages.4: user messages must have non-empty content\"}}");

	// Classifier: resume-scoped 400 fires.
	TestTrue(TEXT("ShouldAutoResetOnExit must fire on 400 invalid_request_error when --resume was used"),
		FClaudeCodeRunner::ShouldAutoResetOnExit(/*ExitCode=*/1, Simulated400, /*bUsedResume=*/true));

	// Classifier: first-turn 400 (no --resume) must NOT reset — avoids spurious artifact rotation on
	// unrelated prompt-shape errors.
	TestFalse(TEXT("ShouldAutoResetOnExit must NOT fire on 400 invalid_request_error when --resume was NOT used"),
		FClaudeCodeRunner::ShouldAutoResetOnExit(/*ExitCode=*/1, Simulated400, /*bUsedResume=*/false));

	// Classifier: exit 0 never resets even if output mentions keywords.
	TestFalse(TEXT("ShouldAutoResetOnExit must NOT fire on exit=0 regardless of output"),
		FClaudeCodeRunner::ShouldAutoResetOnExit(/*ExitCode=*/0, Simulated400, /*bUsedResume=*/true));

	// Artifact rotation: after reset, next call mints a fresh UUID and the on-disk file reflects it.
	Manager.ResetPersistentSession(EUnrealClaudeProviderBackend::ClaudeCli);
	bool bWasExistingAfterReset = true;
	const FString NewUuid = Manager.GetOrCreatePersistentSessionId(
		EUnrealClaudeProviderBackend::ClaudeCli, TEXT("claude-opus-4-7"), &bWasExistingAfterReset);
	TestFalse(TEXT("after reset, GetOrCreatePersistentSessionId must treat the call as fresh (bWasExisting=false)"),
		bWasExistingAfterReset);
	TestFalse(TEXT("new uuid is non-empty"), NewUuid.IsEmpty());
	TestNotEqual(TEXT("post-reset uuid must differ from the initial uuid"), NewUuid, InitialUuid);

	// On-disk confirmation — read-only accessor returns the rotated uuid.
	const FString StoredAfterReset = Manager.ReadPersistentSessionId(EUnrealClaudeProviderBackend::ClaudeCli);
	TestEqual(TEXT("artifact on disk reflects the rotated uuid"), StoredAfterReset, NewUuid);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_BuildCommandLine_ForwardsConfiguredModel,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.BuildCommandLine_ForwardsConfiguredModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_BuildCommandLine_ForwardsConfiguredModel::RunTest(const FString& Parameters)
{
	// Spec 621 reviewer 2026-04-17 15:30 v4 / P3: prior to this round, the plugin stored the configured
	// model in the persistent-session artifact but never forwarded it to the CLI, so every turn ran on
	// Claude CLI's default (Sonnet 4.6 per external advisor) rather than the configured Opus.
	const FString OpusFragment = FClaudeCodeRunner::ComposeModelFlag(TEXT("claude-opus-4-7"));
	TestFalse(TEXT("configured model fragment is non-empty"), OpusFragment.IsEmpty());
	TestTrue(TEXT("fragment starts with --model"), OpusFragment.StartsWith(TEXT("--model ")));
	TestTrue(TEXT("fragment contains the model name"), OpusFragment.Contains(TEXT("claude-opus-4-7")));
	TestTrue(TEXT("fragment ends with trailing space for concatenation"), OpusFragment.EndsWith(TEXT(" ")));

	const FString EmptyFragment = FClaudeCodeRunner::ComposeModelFlag(FString());
	TestTrue(TEXT("empty model name must produce empty fragment"), EmptyFragment.IsEmpty());

	const FString WhitespaceFragment = FClaudeCodeRunner::ComposeModelFlag(TEXT("   "));
	TestTrue(TEXT("whitespace-only model name must produce empty fragment"), WhitespaceFragment.IsEmpty());

	const FString TrimmedFragment = FClaudeCodeRunner::ComposeModelFlag(TEXT("  claude-opus-4-7  "));
	TestEqual(TEXT("surrounding whitespace is trimmed from the forwarded model name"),
		TrimmedFragment, TEXT("--model claude-opus-4-7 "));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_BuildStdinPayload_IsPlainTextAfterP1Fix,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.BuildStdinPayload_IsPlainTextAfterP1Fix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_BuildStdinPayload_IsPlainTextAfterP1Fix::RunTest(const FString& Parameters)
{
	// Spec 621 reviewer 2026-04-17 15:30 v4 / P1: stdin payload is literally the prompt text plus a trailing
	// newline. No `[CONTEXT]` wrap, no NDJSON envelope. Regression guard against re-introducing the wrap.
	const FString Payload = FClaudeCodeRunner::ComposeStdinPayloadForClaudePrint(TEXT("hello"));
	TestEqual(TEXT("payload equals \"hello\\n\" verbatim"), Payload, FString(TEXT("hello\n")));
	TestFalse(TEXT("payload must NOT contain the legacy [CONTEXT] wrap"),
		Payload.Contains(TEXT("[CONTEXT]")));
	TestFalse(TEXT("payload must NOT parse as JSON — it's plain text now"),
		Payload.StartsWith(TEXT("{")) || Payload.StartsWith(TEXT("[")));
	TestFalse(TEXT("payload must NOT be an NDJSON envelope"),
		Payload.Contains(TEXT("\"type\":\"user\"")) || Payload.Contains(TEXT("\"message\":")));

	// Trailing newline already present → no double-newline insertion.
	const FString AlreadyTerminated = FClaudeCodeRunner::ComposeStdinPayloadForClaudePrint(TEXT("hello\n"));
	TestEqual(TEXT("payload preserves caller-provided trailing newline"),
		AlreadyTerminated, FString(TEXT("hello\n")));

	// Empty prompt → empty payload (caller skips stdin write).
	const FString EmptyPayload = FClaudeCodeRunner::ComposeStdinPayloadForClaudePrint(FString());
	TestTrue(TEXT("empty prompt produces empty payload"), EmptyPayload.IsEmpty());

	// Russian/unicode prompt survives round-trip unchanged except for the newline.
	const FString RussianPayload = FClaudeCodeRunner::ComposeStdinPayloadForClaudePrint(TEXT("Запомни число 42."));
	TestEqual(TEXT("russian prompt passes through unchanged with trailing newline"),
		RussianPayload, FString(TEXT("Запомни число 42.\n")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_ParseStreamJsonOutput_HandlesCurrentCliStdoutShape,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.ParseStreamJsonOutput_HandlesCurrentCliStdoutShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_ParseStreamJsonOutput_HandlesCurrentCliStdoutShape::RunTest(const FString& Parameters)
{
	// Spec 621 reviewer 2026-04-17 16:00 v5 Step 4: regression guard for the stdout shape observed in the
	// post-v4 shell repro (Saved/Logs/p25_621_shell_repro_v5.stdout.log). Minimum expectation: when Claude CLI
	// emits NDJSON that contains either a `type:result` event with a `result` string OR a `type:assistant`
	// message with text content, ParseStreamJsonOutput must return a non-empty, non-error body — NOT the
	// "Failed to parse Claude's response" last-resort fallback string.
	FClaudeCodeRunner Runner;

	// Shape 1 — `type:result` present. This is the primary happy path.
	const FString WithResult =
		TEXT("{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"fake\",\"model\":\"claude-opus-4-7\"}\n")
		TEXT("{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Запомнил 42.\"}]}}\n")
		TEXT("{\"type\":\"result\",\"subtype\":\"success\",\"result\":\"Запомнил 42.\",\"is_error\":false}\n");
	const FString Parsed1 = Runner.ParseStreamJsonOutput(WithResult);
	TestFalse(TEXT("result-bearing stdout must parse to non-empty"), Parsed1.IsEmpty());
	TestFalse(TEXT("result-bearing stdout must NOT return the parser's last-resort error"),
		Parsed1.Contains(TEXT("Failed to parse Claude's response")));
	TestTrue(TEXT("result body flows through verbatim"), Parsed1.Contains(TEXT("Запомнил 42.")));

	// Shape 2 — only `type:assistant` text blocks (no `result`). This is the fallback accumulator path.
	const FString AssistantOnly =
		TEXT("{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"fake\"}\n")
		TEXT("{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"42\"}]}}\n");
	const FString Parsed2 = Runner.ParseStreamJsonOutput(AssistantOnly);
	TestFalse(TEXT("assistant-only stdout must parse via fallback"), Parsed2.IsEmpty());
	TestFalse(TEXT("assistant-only stdout must NOT return the parser's last-resort error"),
		Parsed2.Contains(TEXT("Failed to parse Claude's response")));
	TestTrue(TEXT("assistant text accumulates"), Parsed2.Contains(TEXT("42")));

	// Shape 3 — empty stdout (what user saw on v4 with empty stdin). Parser MUST produce the last-resort error;
	// this is the regression canary that told us v4's empty-stdin path was broken.
	const FString EmptyStdout;
	const FString Parsed3 = Runner.ParseStreamJsonOutput(EmptyStdout);
	TestTrue(TEXT("empty stdout must fall through to the last-resort error string (regression canary)"),
		Parsed3.Contains(TEXT("Failed to parse Claude's response")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_BuildCommandLine_EndToEndShapeLooksSane,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.BuildCommandLine_EndToEndShapeLooksSane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_BuildCommandLine_EndToEndShapeLooksSane::RunTest(const FString& Parameters)
{
	// Spec 621 reviewer 2026-04-17 16:00 v5 Step 4: structural assertion on what the command line SHOULD look
	// like post-v4+v5. Compose the flag fragments the same way BuildCommandLine does (same helpers, same
	// ordering). Not a full shell run — that's the worker's Step 1 shell-repro responsibility — but asserts
	// the ingredients are correct and no legacy flag leaked back in.
	const FString TestUuid = TEXT("11111111-2222-3333-4444-555555555555");
	const FString Model = TEXT("claude-opus-4-7");
	const FString Language = TEXT("Russian");

	// Concatenate the fragments BuildCommandLine emits, in order (baseline + session + language + model).
	FString Shape;
	Shape += TEXT("-p ");
	Shape += TEXT("--verbose ");
	Shape += TEXT("--output-format stream-json ");
	Shape += FClaudeCodeRunner::ComposePersistentSessionFlags(true, TestUuid, /*bWasExisting=*/true);
	Shape += FClaudeCodeRunner::ComposeLanguageHintFlag(Language);
	Shape += FClaudeCodeRunner::ComposeModelFlag(Model);

	TestTrue(TEXT("contains -p for print mode"), Shape.Contains(TEXT("-p ")));
	TestTrue(TEXT("contains --output-format stream-json"),
		Shape.Contains(TEXT("--output-format stream-json")));
	TestTrue(TEXT("contains --model with the configured model"),
		Shape.Contains(TEXT("--model claude-opus-4-7")));
	TestTrue(TEXT("contains persistent-session fragment (--resume on existing artifact)"),
		Shape.Contains(TEXT("--resume ")) && Shape.Contains(TestUuid));
	TestTrue(TEXT("contains --append-system-prompt with the Russian language directive"),
		Shape.Contains(TEXT("--append-system-prompt ")) &&
		Shape.Contains(TEXT("preferred interaction language is Russian")));

	// Regression guards — v4 dropped these two strings from the command line shape.
	TestFalse(TEXT("must NOT contain --input-format stream-json (dropped in v4 P2)"),
		Shape.Contains(TEXT("--input-format stream-json")));
	TestFalse(TEXT("must NOT contain legacy [CONTEXT] wrap (dropped in v4 P1)"),
		Shape.Contains(TEXT("[CONTEXT]")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_ComposeEffortFlag_EmitsExpectedFragments,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.ComposeEffortFlag_EmitsExpectedFragments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_ComposeEffortFlag_EmitsExpectedFragments::RunTest(const FString& Parameters)
{
	// Plan 623: ComposeEffortFlag mirrors ComposeModelFlag shape. Empty input (Default setting via
	// GetConfiguredClaudeEffortLevelName) → empty fragment so the --effort flag is omitted entirely.
	// Non-empty input → `--effort <level> ` with a single trailing space for safe concatenation.
	TestTrue(TEXT("empty level → empty fragment (Default setting ⇒ CLI default applies)"),
		FClaudeCodeRunner::ComposeEffortFlag(FString()).IsEmpty());
	TestTrue(TEXT("whitespace-only level → empty fragment"),
		FClaudeCodeRunner::ComposeEffortFlag(TEXT("   ")).IsEmpty());

	TestEqual(TEXT("low fragment"),
		FClaudeCodeRunner::ComposeEffortFlag(TEXT("low")), FString(TEXT("--effort low ")));
	TestEqual(TEXT("medium fragment"),
		FClaudeCodeRunner::ComposeEffortFlag(TEXT("medium")), FString(TEXT("--effort medium ")));
	TestEqual(TEXT("high fragment"),
		FClaudeCodeRunner::ComposeEffortFlag(TEXT("high")), FString(TEXT("--effort high ")));
	TestEqual(TEXT("xhigh fragment"),
		FClaudeCodeRunner::ComposeEffortFlag(TEXT("xhigh")), FString(TEXT("--effort xhigh ")));
	TestEqual(TEXT("max fragment"),
		FClaudeCodeRunner::ComposeEffortFlag(TEXT("max")), FString(TEXT("--effort max ")));

	// Surrounding whitespace is trimmed just like ComposeModelFlag.
	TestEqual(TEXT("surrounding whitespace is trimmed"),
		FClaudeCodeRunner::ComposeEffortFlag(TEXT("  high  ")), FString(TEXT("--effort high ")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaudeSession_BuildCommandLine_ForwardsConfiguredEffort,
	"UnrealClaude.MCP.Tools.ClaudeCodeRunner.BuildCommandLine_ForwardsConfiguredEffort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaudeSession_BuildCommandLine_ForwardsConfiguredEffort::RunTest(const FString& Parameters)
{
	// Plan 623: non-Default effort setting must materialize as `--effort <level>` in the emitted command
	// line; Default setting must omit the flag entirely. Composed via the same helpers BuildCommandLine uses.
	using namespace ClaudeSessionManagerTests;
	FScopedClaudePersistentSessionToggle SettingsGuard;

	UUnrealClaudeSettings* Settings = UUnrealClaudeSettings::GetMutable();
	if (!Settings)
	{
		AddError(TEXT("UUnrealClaudeSettings::GetMutable returned null — cannot exercise BuildCommandLine shape."));
		return false;
	}

	// Non-default (Max) — expect `--effort max` in the synthesized shape.
	Settings->DefaultClaudeEffortLevel = EUnrealClaudeClaudeEffortLevel::Max;
	const FString MaxLevel = Settings->GetConfiguredClaudeEffortLevelName();
	TestEqual(TEXT("Max setting must map to lowercase 'max' for the CLI"), MaxLevel, FString(TEXT("max")));
	FString MaxShape;
	MaxShape += TEXT("-p ");
	MaxShape += TEXT("--output-format stream-json ");
	MaxShape += FClaudeCodeRunner::ComposeModelFlag(TEXT("claude-opus-4-7"));
	MaxShape += FClaudeCodeRunner::ComposeEffortFlag(MaxLevel);
	TestTrue(TEXT("Max setting produces command line containing --effort max"),
		MaxShape.Contains(TEXT("--effort max")));

	// Default — flag must NOT appear anywhere in the synthesized shape.
	Settings->DefaultClaudeEffortLevel = EUnrealClaudeClaudeEffortLevel::Default;
	const FString DefaultLevel = Settings->GetConfiguredClaudeEffortLevelName();
	TestTrue(TEXT("Default setting must map to empty string"), DefaultLevel.IsEmpty());
	FString DefaultShape;
	DefaultShape += TEXT("-p ");
	DefaultShape += TEXT("--output-format stream-json ");
	DefaultShape += FClaudeCodeRunner::ComposeModelFlag(TEXT("claude-opus-4-7"));
	DefaultShape += FClaudeCodeRunner::ComposeEffortFlag(DefaultLevel);
	TestFalse(TEXT("Default setting must NOT emit --effort anywhere in the command line"),
		DefaultShape.Contains(TEXT("--effort")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
