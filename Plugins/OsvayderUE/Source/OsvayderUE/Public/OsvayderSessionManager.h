// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"

/**
 * Manages provider-scoped conversation session persistence and history.
 */
class OSVAYDERUE_API FOsvayderSessionManager
{
public:
	FOsvayderSessionManager();

	/** Get project-local visible conversation history for a specific provider */
	const TArray<TPair<FString, FString>>& GetHistory(EOsvayderUEProviderBackend Backend) const;

	/** Get normal provider-session conversation history for a specific provider */
	const TArray<TPair<FString, FString>>& GetProviderSessionHistory(EOsvayderUEProviderBackend Backend) const;

	/** Add a new exchange to the project-local visible history */
	void AddExchange(EOsvayderUEProviderBackend Backend, const FString& Prompt, const FString& Response);

	/** Add a new exchange to the normal provider-session history */
	void AddProviderSessionExchange(EOsvayderUEProviderBackend Backend, const FString& Prompt, const FString& Response);

	/** Clear project-local visible history (in memory only) */
	void ClearHistory(EOsvayderUEProviderBackend Backend);

	/** Clear normal provider-session history (in memory only) */
	void ClearProviderSessionHistory(EOsvayderUEProviderBackend Backend);

	/** Save project-local visible session to disk */
	bool SaveVisibleSession(EOsvayderUEProviderBackend Backend, const FAgentBackendStatus& BackendStatus);

	/** Load project-local visible session from disk */
	FAgentSessionRestoreResult LoadVisibleSession(EOsvayderUEProviderBackend Backend);

	/** Describe project-local visible sessions for the active and alternate providers */
	FAgentSavedSessionIndex DescribeVisibleSavedSessions(EOsvayderUEProviderBackend Backend) const;

	/** Get project-local visible session file path */
	FString GetVisibleSessionFilePath(EOsvayderUEProviderBackend Backend) const;

	/** Save provider session to disk */
	bool SaveSession(EOsvayderUEProviderBackend Backend, const FAgentBackendStatus& BackendStatus);

	/** Load provider session from disk */
	FAgentSessionRestoreResult LoadSession(EOsvayderUEProviderBackend Backend);

	/** Describe current/alternate/legacy saved session visibility for the active provider */
	FAgentSavedSessionIndex DescribeSavedSessions(EOsvayderUEProviderBackend Backend) const;

	/** Check if a provider-scoped session exists */
	bool HasSavedSession(EOsvayderUEProviderBackend Backend) const;

	/** Get provider-scoped session file path */
	FString GetSessionFilePath(EOsvayderUEProviderBackend Backend) const;

	/** Get the legacy shared session file path */
	FString GetLegacySessionFilePath() const;

	/** Get the disk-backed persistent session artifact path (spec 621 §1 / Saved/OsvayderUE/claude_persistent_session.json, with legacy Saved/OsvayderUE read fallback) */
	FString GetPersistentSessionFilePath(EOsvayderUEProviderBackend Backend) const;

	/** Read the existing persistent session id (no create, no side-effects). Returns empty if artifact missing/unreadable. */
	FString ReadPersistentSessionId(EOsvayderUEProviderBackend Backend) const;

	/**
	 * Return the stable persistent session id for (Backend, Model). Creates a fresh UUID + artifact on first use or when the stored model differs.
	 * Always bumps last_used_utc when called. Returns empty on scope deny or write failure.
	 * OutWasExisting (optional) reports whether a valid artifact existed before this call (i.e. we emit --resume instead of --session-id).
	 */
	FString GetOrCreatePersistentSessionId(EOsvayderUEProviderBackend Backend, const FString& Model, bool* OutWasExisting = nullptr);

	/**
	 * Delete the persistent session artifact if present (idempotent).
	 * Spec 621 v3 §2a: also UUID-match-delete the corresponding native Claude CLI jsonl under
	 * `~/.claude/projects/<project-key>/<uuid>.jsonl` so a rotated-out UUID cannot be replayed by the CLI.
	 * Cleanup is strict UUID-match only (no age sweep, no broad directory cleanup). Denials from the scope
	 * policy are logged and swallowed — see OsvayderSessionManager.cpp for the receipt.
	 */
	void ResetPersistentSession(EOsvayderUEProviderBackend Backend);

	/**
	 * Compute the native Claude CLI project-key directory name from a project root path.
	 * Observed CLI encoding: `X:/PublicExample/Unreal/SampleProject` -> `X--PublicExample-Unreal-SampleProject`
	 * (`:` → `--`, `/` and `\` → `-`, trailing slash/backslash dropped). Case on the drive letter is preserved.
	 * Public-static so OsvayderSessionManagerTests can validate the encoding directly.
	 */
	static FString ComputeClaudeNativeProjectKey(const FString& ProjectRootPath);

	/** Get max history size */
	int32 GetMaxHistorySize() const { return MaxHistorySize; }

	/** Set max history size */
	void SetMaxHistorySize(int32 NewMax) { MaxHistorySize = FMath::Max(1, NewMax); }

#if WITH_DEV_AUTOMATION_TESTS
	static void SetTestSessionSaveDirOverride(const FString& InDir);
	static void ClearTestSessionSaveDirOverride();
	/**
	 * Spec 621 v3 §2a test-only override for the native-projects root. When set, ResetPersistentSession
	 * computes the per-project key against this override instead of `~/.claude/projects`. Pattern mirrors
	 * SetTestSessionSaveDirOverride above so scope-policy sandboxing works inside the project Saved dir.
	 */
	static void SetTestNativeProjectsRootOverride(const FString& InDir);
	static void ClearTestNativeProjectsRootOverride();
#endif

private:
	enum class ESessionStoreKind : uint8
	{
		ProjectLocalVisibleRestore,
		NormalProviderSession
	};

	const TArray<TPair<FString, FString>>& GetHistoryForStore(
		EOsvayderUEProviderBackend Backend,
		ESessionStoreKind StoreKind) const;

	void AddExchangeForStore(
		EOsvayderUEProviderBackend Backend,
		const FString& Prompt,
		const FString& Response,
		ESessionStoreKind StoreKind);

	void ClearHistoryForStore(
		EOsvayderUEProviderBackend Backend,
		ESessionStoreKind StoreKind);

	FAgentSessionMetadata ReadSessionMetadata(
		const FString& SessionPath,
		EOsvayderUEProviderBackend ExpectedBackend,
		bool bIsLegacySharedFile,
		const FString& StoreKind) const;

	bool LoadConversationHistory(
		const FString& SessionPath,
		EOsvayderUEProviderBackend Backend,
		ESessionStoreKind StoreKind,
		FAgentSessionRestoreResult& OutResult);

	FAgentSessionMetadata BuildRuntimeSessionMetadata(
		EOsvayderUEProviderBackend Backend,
		const FAgentBackendStatus& BackendStatus,
		int32 MessageCount,
		const FString& SessionPath,
		const FString& StoreKind) const;

	FString GetSessionFilePathForStore(
		EOsvayderUEProviderBackend Backend,
		ESessionStoreKind StoreKind) const;

	FAgentSavedSessionIndex DescribeSavedSessionsForStore(
		EOsvayderUEProviderBackend Backend,
		ESessionStoreKind StoreKind) const;

	bool SaveSessionForStore(
		EOsvayderUEProviderBackend Backend,
		const FAgentBackendStatus& BackendStatus,
		ESessionStoreKind StoreKind);

	FAgentSessionRestoreResult LoadSessionForStore(
		EOsvayderUEProviderBackend Backend,
		ESessionStoreKind StoreKind);

	FString MakeHistoryKey(
		EOsvayderUEProviderBackend Backend,
		ESessionStoreKind StoreKind) const;

	EOsvayderUEProviderBackend GetOtherBackend(EOsvayderUEProviderBackend Backend) const;

	/**
	 * Spec 621 v3 §2a: UUID-match deletion of the native Claude CLI jsonl
	 * (`<native-projects-root>/<project-key>/<uuid>.jsonl`). Silent no-op when the file is missing.
	 * Scope-denial is logged via a receipt and swallowed. Never touches sibling jsonls.
	 */
	void DeleteMatchingNativeClaudeJsonl(const FString& UuidToDelete) const;

	TMap<FString, TArray<TPair<FString, FString>>> ConversationHistoryByStoreKey;
	int32 MaxHistorySize;
};
