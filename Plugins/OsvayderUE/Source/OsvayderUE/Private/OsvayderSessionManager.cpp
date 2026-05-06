// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderSessionManager.h"
#include "JsonUtils.h"
#include "OsvayderUEConstants.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEModule.h"
#include "OsvayderUEScopePolicy.h"
#include "OsvayderUESettings.h"
#include "OsvayderUEStorageMigration.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	constexpr int32 SessionSchemaVersion = 3;
	const TCHAR* LegacySharedSessionStoreKind = TEXT("legacy_shared_blocked_session");

#if WITH_DEV_AUTOMATION_TESTS
	FString GTestSessionSaveDirOverride;
	FString GTestNativeProjectsRootOverride;
#endif

	FString GetSessionSaveDir()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!GTestSessionSaveDirOverride.IsEmpty())
		{
			FString OverrideDir = FPaths::ConvertRelativePathToFull(GTestSessionSaveDirOverride);
			FPaths::NormalizeDirectoryName(OverrideDir);
			return OverrideDir;
		}
#endif

		return OsvayderUEStorageMigration::GetPreferredSavedRoot();
	}

	FString GetLegacySessionSaveDir()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!GTestSessionSaveDirOverride.IsEmpty())
		{
			FString OverrideDir = FPaths::ConvertRelativePathToFull(GTestSessionSaveDirOverride);
			FPaths::NormalizeDirectoryName(OverrideDir);
			return OsvayderUEStorageMigration::DeriveLegacyRootFromPreferred(OverrideDir);
		}
#endif

		return OsvayderUEStorageMigration::GetLegacySavedRoot();
	}

	/**
	 * Return the native Claude CLI projects root, honoring the test override when set.
	 * Production: `<UserHome>/.claude/projects`.
	 */
	FString GetClaudeNativeProjectsRoot()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!GTestNativeProjectsRootOverride.IsEmpty())
		{
			FString OverrideDir = FPaths::ConvertRelativePathToFull(GTestNativeProjectsRootOverride);
			FPaths::NormalizeDirectoryName(OverrideDir);
			return OverrideDir;
		}
#endif

		FString HomeDir = FPlatformProcess::UserHomeDir();
		// Strip trailing slashes for consistent FPaths::Combine behavior across OSes.
		while (HomeDir.EndsWith(TEXT("/")) || HomeDir.EndsWith(TEXT("\\")))
		{
			HomeDir.LeftChopInline(1);
		}
		return FPaths::Combine(HomeDir, TEXT(".claude"), TEXT("projects"));
	}

	bool LoadSessionRoot(const FString& SessionPath, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
	{
		FString JsonString;
		if (!FFileHelper::LoadFileToString(JsonString, *SessionPath))
		{
			OutError = FString::Printf(TEXT("Failed to load session from: %s"), *SessionPath);
			return false;
		}

		OutRoot = FJsonUtils::Parse(JsonString);
		if (!OutRoot.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to parse session JSON at: %s"), *SessionPath);
			return false;
		}

		return true;
	}

	bool ValidateSessionFile(const FString& SessionPath, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		return LoadSessionRoot(SessionPath, Root, OutError) && Root.IsValid();
	}

	FString ReadPersistentSessionIdFromArtifactPath(const FString& ArtifactPath)
	{
		if (!IFileManager::Get().FileExists(*ArtifactPath))
		{
			return FString();
		}

		TSharedPtr<FJsonObject> Root;
		FString Error;
		if (!LoadSessionRoot(ArtifactPath, Root, Error) || !Root.IsValid())
		{
			return FString();
		}

		FString SessionId;
		FJsonUtils::GetStringField(Root, TEXT("session_id"), SessionId);
		SessionId.TrimStartAndEndInline();
		return SessionId;
	}

	FString GetSessionFilePathForRoot(
		const FString& RootDir,
		const EOsvayderUEProviderBackend Backend,
		const bool bVisibleSession)
	{
		const TCHAR* FilePrefix = bVisibleSession
			? TEXT("visible_session")
			: TEXT("session");
		return FPaths::Combine(
			RootDir,
			FString::Printf(TEXT("%s_%s.json"), FilePrefix, OsvayderUEProviderBackendToSessionSlug(Backend)));
	}

	FString GetLegacySessionFilePathForStore(
		const EOsvayderUEProviderBackend Backend,
		const bool bVisibleSession)
	{
		return GetSessionFilePathForRoot(GetLegacySessionSaveDir(), Backend, bVisibleSession);
	}

	FString GetLegacyPersistentSessionArtifactPath()
	{
		return FPaths::Combine(GetLegacySessionSaveDir(), TEXT("claude_persistent_session.json"));
	}

	EOsvayderUEProviderBackend ParseBackendIdentity(const FString& BackendName, const EOsvayderUEProviderBackend FallbackBackend)
	{
		if (BackendName.Equals(TEXT("CodexCli"), ESearchCase::IgnoreCase))
		{
			return EOsvayderUEProviderBackend::CodexCli;
		}

		if (BackendName.Equals(TEXT("ClaudeCli"), ESearchCase::IgnoreCase))
		{
			return EOsvayderUEProviderBackend::ClaudeCli;
		}

		return FallbackBackend;
	}
}

FOsvayderSessionManager::FOsvayderSessionManager()
	: MaxHistorySize(OsvayderUEConstants::Session::MaxHistorySize)
{
}

const TArray<TPair<FString, FString>>& FOsvayderSessionManager::GetHistory(const EOsvayderUEProviderBackend Backend) const
{
	return GetHistoryForStore(Backend, ESessionStoreKind::ProjectLocalVisibleRestore);
}

const TArray<TPair<FString, FString>>& FOsvayderSessionManager::GetProviderSessionHistory(const EOsvayderUEProviderBackend Backend) const
{
	return GetHistoryForStore(Backend, ESessionStoreKind::NormalProviderSession);
}

const TArray<TPair<FString, FString>>& FOsvayderSessionManager::GetHistoryForStore(
	const EOsvayderUEProviderBackend Backend,
	const ESessionStoreKind StoreKind) const
{
	static const TArray<TPair<FString, FString>> EmptyHistory;

	if (const TArray<TPair<FString, FString>>* History = ConversationHistoryByStoreKey.Find(MakeHistoryKey(Backend, StoreKind)))
	{
		return *History;
	}

	return EmptyHistory;
}

void FOsvayderSessionManager::AddExchange(const EOsvayderUEProviderBackend Backend, const FString& Prompt, const FString& Response)
{
	AddExchangeForStore(Backend, Prompt, Response, ESessionStoreKind::ProjectLocalVisibleRestore);
}

void FOsvayderSessionManager::AddProviderSessionExchange(
	const EOsvayderUEProviderBackend Backend,
	const FString& Prompt,
	const FString& Response)
{
	AddExchangeForStore(Backend, Prompt, Response, ESessionStoreKind::NormalProviderSession);
}

void FOsvayderSessionManager::AddExchangeForStore(
	const EOsvayderUEProviderBackend Backend,
	const FString& Prompt,
	const FString& Response,
	const ESessionStoreKind StoreKind)
{
	TArray<TPair<FString, FString>>& History = ConversationHistoryByStoreKey.FindOrAdd(MakeHistoryKey(Backend, StoreKind));
	History.Add(TPair<FString, FString>(Prompt, Response));

	while (History.Num() > MaxHistorySize)
	{
		History.RemoveAt(0);
	}
}

void FOsvayderSessionManager::ClearHistory(const EOsvayderUEProviderBackend Backend)
{
	ClearHistoryForStore(Backend, ESessionStoreKind::ProjectLocalVisibleRestore);
}

void FOsvayderSessionManager::ClearProviderSessionHistory(const EOsvayderUEProviderBackend Backend)
{
	ClearHistoryForStore(Backend, ESessionStoreKind::NormalProviderSession);
}

void FOsvayderSessionManager::ClearHistoryForStore(
	const EOsvayderUEProviderBackend Backend,
	const ESessionStoreKind StoreKind)
{
	ConversationHistoryByStoreKey.Remove(MakeHistoryKey(Backend, StoreKind));
}

FString FOsvayderSessionManager::GetSessionFilePath(const EOsvayderUEProviderBackend Backend) const
{
	return GetSessionFilePathForStore(Backend, ESessionStoreKind::NormalProviderSession);
}

FString FOsvayderSessionManager::GetVisibleSessionFilePath(const EOsvayderUEProviderBackend Backend) const
{
	return GetSessionFilePathForStore(Backend, ESessionStoreKind::ProjectLocalVisibleRestore);
}

FString FOsvayderSessionManager::GetLegacySessionFilePath() const
{
	return FPaths::Combine(GetLegacySessionSaveDir(), TEXT("session.json"));
}

FString FOsvayderSessionManager::GetPersistentSessionFilePath(const EOsvayderUEProviderBackend Backend) const
{
	// Spec 621 §1: only ClaudeCli is wired for now; fall through to the Claude artifact name for any other value.
	(void)Backend;
	return FPaths::Combine(GetSessionSaveDir(), TEXT("claude_persistent_session.json"));
}

FString FOsvayderSessionManager::ReadPersistentSessionId(const EOsvayderUEProviderBackend Backend) const
{
	FString ResolveError;
	OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
	if (!OsvayderUEStorageMigration::ResolveManagedReadPath(
		GetPersistentSessionFilePath(Backend),
		GetLegacyPersistentSessionArtifactPath(),
		TEXT("session_persistent_artifact"),
		ValidateSessionFile,
		ManagedRead,
		ResolveError))
	{
		return FString();
	}

	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!LoadSessionRoot(ManagedRead.ResolvedPath, Root, Error) || !Root.IsValid())
	{
		return FString();
	}

	FString SessionId;
	FJsonUtils::GetStringField(Root, TEXT("session_id"), SessionId);
	return SessionId.TrimStartAndEnd();
}

FString FOsvayderSessionManager::GetOrCreatePersistentSessionId(
	const EOsvayderUEProviderBackend Backend,
	const FString& Model,
	bool* OutWasExisting)
{
	if (OutWasExisting)
	{
		*OutWasExisting = false;
	}

	const FString ArtifactPath = GetPersistentSessionFilePath(Backend);
	const FString AbsolutePath = FPaths::ConvertRelativePathToFull(ArtifactPath);
	const FString TrimmedModel = Model.TrimStartAndEnd();

	FString ExistingSessionId;
	FString ExistingModel;
	FString CreatedUtc;
	bool bArtifactValid = false;

	FString ResolveError;
	OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
	if (OsvayderUEStorageMigration::ResolveManagedReadPath(
		ArtifactPath,
		GetLegacyPersistentSessionArtifactPath(),
		TEXT("session_persistent_artifact"),
		ValidateSessionFile,
		ManagedRead,
		ResolveError))
	{
		TSharedPtr<FJsonObject> Root;
		FString LoadError;
		if (LoadSessionRoot(ManagedRead.ResolvedPath, Root, LoadError) && Root.IsValid())
		{
			FJsonUtils::GetStringField(Root, TEXT("session_id"), ExistingSessionId);
			FJsonUtils::GetStringField(Root, TEXT("model"), ExistingModel);
			FJsonUtils::GetStringField(Root, TEXT("created_utc"), CreatedUtc);
			ExistingSessionId = ExistingSessionId.TrimStartAndEnd();
			ExistingModel = ExistingModel.TrimStartAndEnd();
			bArtifactValid = !ExistingSessionId.IsEmpty();
		}
	}

	// Model change or missing/invalid artifact → reset
	const bool bModelMatches = bArtifactValid && ExistingModel.Equals(TrimmedModel, ESearchCase::CaseSensitive);
	if (!bArtifactValid || !bModelMatches)
	{
		if (IFileManager::Get().FileExists(*ArtifactPath))
		{
			IFileManager::Get().Delete(*ArtifactPath, false, true, true);
			if (bArtifactValid && !bModelMatches)
			{
				UE_LOG(LogOsvayderUE, Log,
					TEXT("Persistent Claude session model change detected (stored='%s' -> requested='%s'); resetting artifact: %s"),
					*ExistingModel, *TrimmedModel, *ArtifactPath);
			}
		}
		ExistingSessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		CreatedUtc = FDateTime::UtcNow().ToIso8601();
		bArtifactValid = false; // freshly minted; artifact did not exist before this call
	}

	if (OutWasExisting)
	{
		*OutWasExisting = bArtifactValid;
	}

	// Build / rewrite artifact (always, to bump last_used_utc)
	const FString SaveDir = FPaths::GetPath(ArtifactPath);
	if (!IFileManager::Get().DirectoryExists(*SaveDir))
	{
		IFileManager::Get().MakeDirectory(*SaveDir, true);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(TEXT("session_id"), ExistingSessionId);
	Root->SetStringField(TEXT("model"), TrimmedModel);
	Root->SetStringField(TEXT("created_utc"), CreatedUtc);
	Root->SetStringField(TEXT("last_used_utc"), FDateTime::UtcNow().ToIso8601());

	const FString JsonString = FJsonUtils::Stringify(Root, true);
	if (JsonString.IsEmpty())
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to serialize persistent Claude session JSON for: %s"), *ArtifactPath);
		return FString();
	}

	const FOsvayderUEScopePolicy::FScopeCheckResult ScopeCheck =
		FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(AbsolutePath);

	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("session_manager");
	Receipt.TargetType = TEXT("file");
	Receipt.Targets.Add(AbsolutePath);

	if (!ScopeCheck.bAllowed)
	{
		Receipt.bSuccess = false;
		Receipt.Classification = TEXT("denied");
		Receipt.ValidationSummary = ScopeCheck.DenialReason;
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
		UE_LOG(LogOsvayderUE, Warning,
			TEXT("Scope denied persistent Claude session write: %s"), *ScopeCheck.DenialReason);
		return FString();
	}

	if (!FFileHelper::SaveStringToFile(JsonString, *ArtifactPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Receipt.bSuccess = false;
		Receipt.Classification = TEXT("internal_state");
		Receipt.ValidationSummary = TEXT("File write failed");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
		UE_LOG(LogOsvayderUE, Error,
			TEXT("Failed to save persistent Claude session to: %s"), *ArtifactPath);
		return FString();
	}

	Receipt.bSuccess = true;
	Receipt.Classification = TEXT("internal_state");
	Receipt.ValidationSummary = TEXT("claude_persistent_session.json");
	Receipt.Modified.Add(AbsolutePath);
	FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);

	UE_LOG(LogOsvayderUE, Log,
		TEXT("Persistent Claude session ready: id=%s, model=%s, artifact_existed=%s, path=%s"),
		*ExistingSessionId,
		*TrimmedModel,
		bArtifactValid ? TEXT("yes") : TEXT("no"),
		*ArtifactPath);

	return ExistingSessionId;
}

void FOsvayderSessionManager::ResetPersistentSession(const EOsvayderUEProviderBackend Backend)
{
	const FString ArtifactPath = GetPersistentSessionFilePath(Backend);
	const FString LegacyArtifactPath = GetLegacyPersistentSessionArtifactPath();

	// Spec 621 v3 §2a: read the UUID we are about to rotate away from BEFORE deleting the project-local
	// artifact, so we can UUID-match the native Claude CLI jsonl and remove only that specific file. This
	// prevents Claude CLI from replaying the rotated-out UUID via --resume if anything still points at it.
	// Strict UUID match — no age sweep, no broad directory cleanup — because `~/.claude/projects/<key>/`
	// is shared with the claude-desktop entrypoint and must not be touched wholesale.
	TArray<FString> RotatingUuids;
	const FString PreferredUuid = ReadPersistentSessionIdFromArtifactPath(ArtifactPath);
	if (!PreferredUuid.IsEmpty())
	{
		RotatingUuids.AddUnique(PreferredUuid);
	}

	const FString LegacyUuid = ReadPersistentSessionIdFromArtifactPath(LegacyArtifactPath);
	if (!LegacyUuid.IsEmpty())
	{
		RotatingUuids.AddUnique(LegacyUuid);
	}

	FString DeleteError;
	if (!OsvayderUEStorageMigration::DeleteManagedFileCopies(
		ArtifactPath,
		LegacyArtifactPath,
		TEXT("session_persistent_artifact"),
		DeleteError))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *DeleteError);
	}

	for (const FString& RotatingUuid : RotatingUuids)
	{
		DeleteMatchingNativeClaudeJsonl(RotatingUuid);
	}
}

FString FOsvayderSessionManager::ComputeClaudeNativeProjectKey(const FString& ProjectRootPath)
{
	// Observed Claude CLI encoding (Step 1e evidence):
	//   X:/PublicExample/Unreal/SampleProject -> X--PublicExample-Unreal-SampleProject
	// Rules:
	//   1. Normalize backslashes to forward slashes.
	//   2. Trim trailing slashes.
	//   3. Drive-letter `<L>:/` becomes `<L>--` (the path-separator after `:` is consumed by the substitution;
	//      otherwise we would emit `<L>---...` once the remaining `/` also became `-`).
	//   4. Any remaining bare `:` becomes `--`, and bare `/` becomes `-`.
	FString Key = ProjectRootPath;
	Key.ReplaceInline(TEXT("\\"), TEXT("/"));
	while (Key.EndsWith(TEXT("/")))
	{
		Key.LeftChopInline(1);
	}
	Key.ReplaceInline(TEXT(":/"), TEXT("--"));
	Key.ReplaceInline(TEXT(":"), TEXT("--"));
	Key.ReplaceInline(TEXT("/"), TEXT("-"));
	return Key;
}

void FOsvayderSessionManager::DeleteMatchingNativeClaudeJsonl(const FString& UuidToDelete) const
{
	if (UuidToDelete.IsEmpty())
	{
		return;
	}

	// Resolve the project root the same way the Claude CLI child process does (it is spawned with
	// FPaths::ProjectDir() as cwd — see OsvayderCodeRunner.cpp:556/1664). This keeps the computed
	// project-key aligned with whatever jsonl Claude CLI would read/write.
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString ProjectKey = ComputeClaudeNativeProjectKey(ProjectRoot);
	if (ProjectKey.IsEmpty())
	{
		return;
	}

	const FString NativeProjectsRoot = GetClaudeNativeProjectsRoot();
	const FString TargetJsonlPath = FPaths::Combine(
		NativeProjectsRoot,
		ProjectKey,
		FString::Printf(TEXT("%s.jsonl"), *UuidToDelete));
	const FString AbsoluteTargetPath = FPaths::ConvertRelativePathToFull(TargetJsonlPath);

	if (!IFileManager::Get().FileExists(*AbsoluteTargetPath))
	{
		// Silent no-op: the native jsonl never existed (fresh UUID, or CLI has not materialized the
		// session file yet). No receipt, no log noise.
		return;
	}

	// Scope policy lives OUTSIDE Saved/OsvayderUE/ — `~/.claude/projects/...` will deny under PluginOnly
	// mode. Attempt is still made, denial is logged gracefully and the worker continues. Under
	// PluginAndProject mode or when the test override points the root inside the sandbox, the write is
	// allowed and the jsonl is removed.
	const FOsvayderUEScopePolicy::FScopeCheckResult ScopeCheck =
		FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(AbsoluteTargetPath);

	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("session_manager");
	Receipt.TargetType = TEXT("file");
	Receipt.Targets.Add(AbsoluteTargetPath);

	if (!ScopeCheck.bAllowed)
	{
		Receipt.bSuccess = false;
		Receipt.Classification = TEXT("denied");
		Receipt.ValidationSummary = FString::Printf(
			TEXT("Scope denied native Claude jsonl cleanup (UUID-match): %s"), *ScopeCheck.DenialReason);
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
		UE_LOG(LogOsvayderUE, Warning,
			TEXT("Scope denied native Claude jsonl cleanup at %s; continuing without deletion (%s)"),
			*AbsoluteTargetPath, *ScopeCheck.DenialReason);
		return;
	}

	if (IFileManager::Get().Delete(*AbsoluteTargetPath, false, true, true))
	{
		Receipt.bSuccess = true;
		Receipt.Classification = TEXT("internal_state");
		Receipt.ValidationSummary = TEXT("native_claude_jsonl_uuid_match_deleted");
		Receipt.Modified.Add(AbsoluteTargetPath);
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
		UE_LOG(LogOsvayderUE, Log,
			TEXT("Native Claude jsonl (UUID-match) deleted during persistent-session reset: %s"),
			*AbsoluteTargetPath);
	}
	else
	{
		Receipt.bSuccess = false;
		Receipt.Classification = TEXT("internal_state");
		Receipt.ValidationSummary = TEXT("File delete failed");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
		UE_LOG(LogOsvayderUE, Warning,
			TEXT("Failed to delete native Claude jsonl (UUID-match): %s"),
			*AbsoluteTargetPath);
	}
}

bool FOsvayderSessionManager::HasSavedSession(const EOsvayderUEProviderBackend Backend) const
{
	FString ResolveError;
	OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
	return OsvayderUEStorageMigration::ResolveManagedReadPath(
		GetSessionFilePath(Backend),
		GetLegacySessionFilePathForStore(Backend, false),
		TEXT("provider_session"),
		ValidateSessionFile,
		ManagedRead,
		ResolveError);
}

FAgentSavedSessionIndex FOsvayderSessionManager::DescribeSavedSessions(const EOsvayderUEProviderBackend Backend) const
{
	return DescribeSavedSessionsForStore(Backend, ESessionStoreKind::NormalProviderSession);
}

FAgentSavedSessionIndex FOsvayderSessionManager::DescribeVisibleSavedSessions(const EOsvayderUEProviderBackend Backend) const
{
	return DescribeSavedSessionsForStore(Backend, ESessionStoreKind::ProjectLocalVisibleRestore);
}

FAgentSavedSessionIndex FOsvayderSessionManager::DescribeSavedSessionsForStore(
	const EOsvayderUEProviderBackend Backend,
	const ESessionStoreKind StoreKind) const
{
	const FString StoreKindLabel = StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore
		? TEXT("project_local_visible_restore")
		: TEXT("normal_provider_session");
	const bool bVisibleSession = StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore;

	FAgentSavedSessionIndex Index;
	{
		FString ResolveError;
		OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
		if (OsvayderUEStorageMigration::ResolveManagedReadPath(
			GetSessionFilePathForStore(Backend, StoreKind),
			GetLegacySessionFilePathForStore(Backend, bVisibleSession),
			StoreKindLabel + TEXT("_current_provider"),
			ValidateSessionFile,
			ManagedRead,
			ResolveError))
		{
			Index.CurrentProviderSession = ReadSessionMetadata(
				ManagedRead.ResolvedPath,
				Backend,
				false,
				StoreKindLabel);
		}
		else
		{
			Index.CurrentProviderSession = ReadSessionMetadata(
				GetSessionFilePathForStore(Backend, StoreKind),
				Backend,
				false,
				StoreKindLabel);
		}
	}

	{
		const EOsvayderUEProviderBackend OtherBackend = GetOtherBackend(Backend);
		FString ResolveError;
		OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
		if (OsvayderUEStorageMigration::ResolveManagedReadPath(
			GetSessionFilePathForStore(OtherBackend, StoreKind),
			GetLegacySessionFilePathForStore(OtherBackend, bVisibleSession),
			StoreKindLabel + TEXT("_other_provider"),
			ValidateSessionFile,
			ManagedRead,
			ResolveError))
		{
			Index.OtherProviderSession = ReadSessionMetadata(
				ManagedRead.ResolvedPath,
				OtherBackend,
				false,
				StoreKindLabel);
		}
		else
		{
			Index.OtherProviderSession = ReadSessionMetadata(
				GetSessionFilePathForStore(OtherBackend, StoreKind),
				OtherBackend,
				false,
				StoreKindLabel);
		}
	}
	Index.LegacySharedSession = ReadSessionMetadata(
		GetLegacySessionFilePath(),
		Backend,
		true,
		LegacySharedSessionStoreKind);
	return Index;
}

FAgentSessionMetadata FOsvayderSessionManager::BuildRuntimeSessionMetadata(
	const EOsvayderUEProviderBackend Backend,
	const FAgentBackendStatus& BackendStatus,
	const int32 MessageCount,
	const FString& SessionPath,
	const FString& StoreKind) const
{
	FAgentSessionMetadata Metadata;
	Metadata.bHasSession = MessageCount > 0;
	Metadata.bIsReadable = MessageCount > 0;
	Metadata.Backend = Backend;
	Metadata.BackendDisplayName = BackendStatus.DisplayName.IsEmpty()
		? BackendStatus.Capabilities.DisplayName
		: BackendStatus.DisplayName;
	Metadata.StoreKind = StoreKind;
	Metadata.SessionFilePath = SessionPath;
	Metadata.MessageCount = MessageCount;
	Metadata.LastUpdated = FDateTime::UtcNow().ToString(TEXT("%Y-%m-%dT%H:%M:%SZ"));

	if (const UOsvayderUESettings* Settings = UOsvayderUESettings::Get())
	{
		Metadata.Model = Settings->GetConfiguredModelForBackend(Backend);
		Metadata.Profile = Settings->GetConfiguredProfileLabelForBackend(Backend);
		Metadata.AuthMode = Settings->GetConfiguredAuthModeLabelForBackend(Backend);
	}

	return Metadata;
}

FAgentSessionMetadata FOsvayderSessionManager::ReadSessionMetadata(
	const FString& SessionPath,
	const EOsvayderUEProviderBackend ExpectedBackend,
	const bool bIsLegacySharedFile,
	const FString& StoreKind) const
{
	FAgentSessionMetadata Metadata;
	Metadata.SessionFilePath = SessionPath;
	Metadata.Backend = ExpectedBackend;
	Metadata.bIsLegacySharedFile = bIsLegacySharedFile;
	Metadata.StoreKind = StoreKind;

	if (!IFileManager::Get().FileExists(*SessionPath))
	{
		return Metadata;
	}

	Metadata.bHasSession = true;

	TSharedPtr<FJsonObject> RootObject;
	FString Error;
	if (!LoadSessionRoot(SessionPath, RootObject, Error))
	{
		Metadata.Detail = Error;
		return Metadata;
	}

	Metadata.bIsReadable = true;

	FString BackendName;
	if (FJsonUtils::GetStringField(RootObject, TEXT("backend"), BackendName))
	{
		Metadata.Backend = ParseBackendIdentity(BackendName, ExpectedBackend);
	}
	else if (bIsLegacySharedFile)
	{
		Metadata.Detail = TEXT("Legacy shared session has no provider identity and will not be auto-restored.");
	}

	FJsonUtils::GetStringField(RootObject, TEXT("backend_display_name"), Metadata.BackendDisplayName);
	FJsonUtils::GetStringField(RootObject, TEXT("store_kind"), Metadata.StoreKind);
	FJsonUtils::GetStringField(RootObject, TEXT("model"), Metadata.Model);
	FJsonUtils::GetStringField(RootObject, TEXT("profile"), Metadata.Profile);
	FJsonUtils::GetStringField(RootObject, TEXT("auth_mode"), Metadata.AuthMode);
	FJsonUtils::GetStringField(RootObject, TEXT("last_updated"), Metadata.LastUpdated);

	double MessageCount = 0.0;
	if (RootObject->TryGetNumberField(TEXT("message_count"), MessageCount))
	{
		Metadata.MessageCount = static_cast<int32>(MessageCount);
	}
	else
	{
		TArray<TSharedPtr<FJsonValue>> MessagesArray;
		if (FJsonUtils::GetArrayField(RootObject, TEXT("messages"), MessagesArray))
		{
			Metadata.MessageCount = MessagesArray.Num();
		}
	}

	if (Metadata.BackendDisplayName.IsEmpty())
	{
		Metadata.BackendDisplayName = Metadata.Backend == EOsvayderUEProviderBackend::CodexCli
			? TEXT("Codex CLI")
			: TEXT("Claude CLI");
	}

	if (Metadata.Detail.IsEmpty() && bIsLegacySharedFile)
	{
		Metadata.Detail = TEXT("Legacy shared session remains readable but is blocked from automatic provider restore.");
	}

	return Metadata;
}

bool FOsvayderSessionManager::SaveSession(const EOsvayderUEProviderBackend Backend, const FAgentBackendStatus& BackendStatus)
{
	return SaveSessionForStore(Backend, BackendStatus, ESessionStoreKind::NormalProviderSession);
}

bool FOsvayderSessionManager::SaveVisibleSession(const EOsvayderUEProviderBackend Backend, const FAgentBackendStatus& BackendStatus)
{
	return SaveSessionForStore(Backend, BackendStatus, ESessionStoreKind::ProjectLocalVisibleRestore);
}

bool FOsvayderSessionManager::SaveSessionForStore(
	const EOsvayderUEProviderBackend Backend,
	const FAgentBackendStatus& BackendStatus,
	const ESessionStoreKind StoreKind)
{
	const FString SessionPath = GetSessionFilePathForStore(Backend, StoreKind);
	const FString AbsoluteSessionPath = FPaths::ConvertRelativePathToFull(SessionPath);
	const TArray<TPair<FString, FString>>& History = GetHistoryForStore(Backend, StoreKind);
	const FString StoreKindLabel = StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore
		? TEXT("project_local_visible_restore")
		: TEXT("normal_provider_session");
	const FString StoreDisplayName = StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore
		? TEXT("project-local visible session")
		: TEXT("provider session");

	if (History.Num() == 0)
	{
		FString DeleteError;
		if (!OsvayderUEStorageMigration::DeleteManagedFileCopies(
			SessionPath,
			GetLegacySessionFilePathForStore(Backend, StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore),
			StoreKindLabel,
			DeleteError))
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *DeleteError);
			return false;
		}

		UE_LOG(LogOsvayderUE, Log, TEXT("Deleted empty %s managed files at: %s"), *StoreDisplayName, *SessionPath);
		return true;
	}

	const FString SaveDir = FPaths::GetPath(SessionPath);
	if (!IFileManager::Get().DirectoryExists(*SaveDir))
	{
		IFileManager::Get().MakeDirectory(*SaveDir, true);
	}

	const FAgentSessionMetadata Metadata = BuildRuntimeSessionMetadata(Backend, BackendStatus, History.Num(), SessionPath, StoreKindLabel);

	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetNumberField(TEXT("schema_version"), SessionSchemaVersion);
	RootObject->SetStringField(TEXT("backend"), OsvayderUEProviderBackendToString(Backend));
	RootObject->SetStringField(TEXT("backend_display_name"), Metadata.BackendDisplayName);
	RootObject->SetStringField(TEXT("store_kind"), Metadata.StoreKind);
	RootObject->SetStringField(TEXT("model"), Metadata.Model);
	if (!Metadata.Profile.IsEmpty())
	{
		RootObject->SetStringField(TEXT("profile"), Metadata.Profile);
	}
	if (!Metadata.AuthMode.IsEmpty())
	{
		RootObject->SetStringField(TEXT("auth_mode"), Metadata.AuthMode);
	}
	RootObject->SetNumberField(TEXT("message_count"), Metadata.MessageCount);
	RootObject->SetStringField(TEXT("last_updated"), Metadata.LastUpdated);

	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	for (const TPair<FString, FString>& Exchange : History)
	{
		TSharedPtr<FJsonObject> MessageObject = MakeShared<FJsonObject>();
		MessageObject->SetStringField(TEXT("user"), Exchange.Key);
		MessageObject->SetStringField(TEXT("assistant"), Exchange.Value);
		MessagesArray.Add(MakeShared<FJsonValueObject>(MessageObject));
	}
	RootObject->SetArrayField(TEXT("messages"), MessagesArray);

	const FString JsonString = FJsonUtils::Stringify(RootObject, true);
	if (JsonString.IsEmpty())
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to serialize session JSON for %s"), *SessionPath);
		return false;
	}

	const FOsvayderUEScopePolicy::FScopeCheckResult ScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(AbsoluteSessionPath);
	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("session_manager");
	Receipt.TargetType = TEXT("file");
	Receipt.Targets.Add(AbsoluteSessionPath);

	if (!ScopeCheck.bAllowed)
	{
		Receipt.bSuccess = false;
		Receipt.Classification = TEXT("denied");
		Receipt.ValidationSummary = ScopeCheck.DenialReason;
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
		UE_LOG(LogOsvayderUE, Warning, TEXT("Scope denied session save: %s"), *ScopeCheck.DenialReason);
		return false;
	}

	Receipt.bSuccess = true;
	Receipt.Classification = ScopeCheck.Classification == EScopeClassification::InternalState
		? TEXT("internal_state")
		: TEXT("user_mutation");
	Receipt.ValidationSummary = ScopeCheck.Classification == EScopeClassification::InternalState
		? TEXT("internal runtime state")
		: TEXT("user mutation");

	if (!FFileHelper::SaveStringToFile(JsonString, *SessionPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Receipt.bSuccess = false;
		Receipt.Classification = TEXT("internal_state");
		Receipt.ValidationSummary = TEXT("File write failed");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to save session to: %s"), *SessionPath);
		return false;
	}

	Receipt.Modified.Add(AbsoluteSessionPath);
	FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);

	UE_LOG(
		LogOsvayderUE,
		Log,
		TEXT("%s saved to: %s (%d messages, backend=%s)"),
		*StoreDisplayName,
		*SessionPath,
		History.Num(),
		OsvayderUEProviderBackendToString(Backend));
	return true;
}

bool FOsvayderSessionManager::LoadConversationHistory(
	const FString& SessionPath,
	const EOsvayderUEProviderBackend Backend,
	const ESessionStoreKind StoreKind,
	FAgentSessionRestoreResult& OutResult)
{
	const FString StoreKindLabel = StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore
		? TEXT("project_local_visible_restore")
		: TEXT("normal_provider_session");

	TSharedPtr<FJsonObject> RootObject;
	FString Error;
	if (!LoadSessionRoot(SessionPath, RootObject, Error))
	{
		OutResult.Outcome = EAgentSessionRestoreOutcome::Failed;
		OutResult.FailureReason = Error;
		OutResult.RequestedSession = ReadSessionMetadata(SessionPath, Backend, false, StoreKindLabel);
		return false;
	}

	TArray<TPair<FString, FString>> LoadedHistory;
	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	if (FJsonUtils::GetArrayField(RootObject, TEXT("messages"), MessagesArray))
	{
		for (const TSharedPtr<FJsonValue>& MessageValue : MessagesArray)
		{
			const TSharedPtr<FJsonObject>* MessageObject = nullptr;
			if (!MessageValue->TryGetObject(MessageObject) || !MessageObject)
			{
				continue;
			}

			FString UserMessage;
			FString AssistantMessage;
			if (FJsonUtils::GetStringField(*MessageObject, TEXT("user"), UserMessage) &&
				FJsonUtils::GetStringField(*MessageObject, TEXT("assistant"), AssistantMessage))
			{
				LoadedHistory.Add(TPair<FString, FString>(UserMessage, AssistantMessage));
			}
		}
	}

	while (LoadedHistory.Num() > MaxHistorySize)
	{
		LoadedHistory.RemoveAt(0);
	}

	ConversationHistoryByStoreKey.FindOrAdd(MakeHistoryKey(Backend, StoreKind)) = LoadedHistory;

	OutResult.Outcome = EAgentSessionRestoreOutcome::Loaded;
	OutResult.RequestedSession = ReadSessionMetadata(SessionPath, Backend, false, StoreKindLabel);
	OutResult.RequestedSession.MessageCount = LoadedHistory.Num();
	OutResult.RequestedSession.bHasSession = true;
	OutResult.RequestedSession.bIsReadable = true;

	const FString StoreDisplayName = StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore
		? TEXT("project-local visible session")
		: TEXT("provider session");

	UE_LOG(
		LogOsvayderUE,
		Log,
		TEXT("%s loaded from: %s (%d messages, backend=%s)"),
		*StoreDisplayName,
		*SessionPath,
		LoadedHistory.Num(),
		OsvayderUEProviderBackendToString(Backend));
	return true;
}

FAgentSessionRestoreResult FOsvayderSessionManager::LoadSession(const EOsvayderUEProviderBackend Backend)
{
	return LoadSessionForStore(Backend, ESessionStoreKind::NormalProviderSession);
}

FAgentSessionRestoreResult FOsvayderSessionManager::LoadVisibleSession(const EOsvayderUEProviderBackend Backend)
{
	return LoadSessionForStore(Backend, ESessionStoreKind::ProjectLocalVisibleRestore);
}

FAgentSessionRestoreResult FOsvayderSessionManager::LoadSessionForStore(
	const EOsvayderUEProviderBackend Backend,
	const ESessionStoreKind StoreKind)
{
	const FString StoreKindLabel = StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore
		? TEXT("project_local_visible_restore")
		: TEXT("normal_provider_session");
	const FString StoreDisplayName = StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore
		? TEXT("project-local visible")
		: TEXT("provider");

	FAgentSessionRestoreResult Result;
	Result.SavedSessions = DescribeSavedSessionsForStore(Backend, StoreKind);
	Result.RequestedSession = Result.SavedSessions.CurrentProviderSession;

	if (Result.SavedSessions.CurrentProviderSession.bHasSession)
	{
		LoadConversationHistory(Result.SavedSessions.CurrentProviderSession.SessionFilePath, Backend, StoreKind, Result);
		return Result;
	}

	if (Result.SavedSessions.LegacySharedSession.bHasSession)
	{
		Result.Outcome = EAgentSessionRestoreOutcome::LegacySharedSessionBlocked;
		Result.RequestedSession = Result.SavedSessions.LegacySharedSession;
		Result.FailureReason = Result.SavedSessions.LegacySharedSession.Detail.IsEmpty()
			? TEXT("Legacy shared session detected, but it is not restored because its provider identity is unsafe.")
			: Result.SavedSessions.LegacySharedSession.Detail;
		return Result;
	}

	Result.Outcome = EAgentSessionRestoreOutcome::NoSession;
	if (Result.SavedSessions.OtherProviderSession.bHasSession)
	{
		Result.FailureReason = FString::Printf(
			TEXT("No saved %s %s session found. A separate %s %s session exists and can be restored after switching providers."),
			*StoreDisplayName,
			Result.SavedSessions.CurrentProviderSession.Backend == EOsvayderUEProviderBackend::CodexCli
				? TEXT("Codex CLI")
				: TEXT("Claude CLI"),
			*StoreDisplayName,
			*Result.SavedSessions.OtherProviderSession.BackendDisplayName);
	}
	else
	{
		Result.FailureReason = FString::Printf(TEXT("No saved %s session was found for the active provider."), *StoreDisplayName);
	}

	return Result;
}

FString FOsvayderSessionManager::GetSessionFilePathForStore(
	const EOsvayderUEProviderBackend Backend,
	const ESessionStoreKind StoreKind) const
{
	return GetSessionFilePathForRoot(
		GetSessionSaveDir(),
		Backend,
		StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore);
}

FString FOsvayderSessionManager::MakeHistoryKey(
	const EOsvayderUEProviderBackend Backend,
	const ESessionStoreKind StoreKind) const
{
	const TCHAR* StoreKey = StoreKind == ESessionStoreKind::ProjectLocalVisibleRestore
		? TEXT("visible")
		: TEXT("provider");
	return FString::Printf(TEXT("%s|%s"), OsvayderUEProviderBackendToString(Backend), StoreKey);
}

EOsvayderUEProviderBackend FOsvayderSessionManager::GetOtherBackend(const EOsvayderUEProviderBackend Backend) const
{
	return Backend == EOsvayderUEProviderBackend::CodexCli
		? EOsvayderUEProviderBackend::ClaudeCli
		: EOsvayderUEProviderBackend::CodexCli;
}

#if WITH_DEV_AUTOMATION_TESTS
void FOsvayderSessionManager::SetTestSessionSaveDirOverride(const FString& InDir)
{
	GTestSessionSaveDirOverride = InDir;
}

void FOsvayderSessionManager::ClearTestSessionSaveDirOverride()
{
	GTestSessionSaveDirOverride.Empty();
}

void FOsvayderSessionManager::SetTestNativeProjectsRootOverride(const FString& InDir)
{
	GTestNativeProjectsRootOverride = InDir;
}

void FOsvayderSessionManager::ClearTestNativeProjectsRootOverride()
{
	GTestNativeProjectsRootOverride.Empty();
}
#endif
