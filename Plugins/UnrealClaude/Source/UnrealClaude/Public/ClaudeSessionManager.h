// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"

/**
 * Manages provider-scoped conversation session persistence and history.
 */
class UNREALCLAUDE_API FClaudeSessionManager
{
public:
	FClaudeSessionManager();

	/** Get project-local visible conversation history for a specific provider */
	const TArray<TPair<FString, FString>>& GetHistory(EUnrealClaudeProviderBackend Backend) const;

	/** Get normal provider-session conversation history for a specific provider */
	const TArray<TPair<FString, FString>>& GetProviderSessionHistory(EUnrealClaudeProviderBackend Backend) const;

	/** Add a new exchange to the project-local visible history */
	void AddExchange(EUnrealClaudeProviderBackend Backend, const FString& Prompt, const FString& Response);

	/** Add a new exchange to the normal provider-session history */
	void AddProviderSessionExchange(EUnrealClaudeProviderBackend Backend, const FString& Prompt, const FString& Response);

	/** Clear project-local visible history (in memory only) */
	void ClearHistory(EUnrealClaudeProviderBackend Backend);

	/** Clear normal provider-session history (in memory only) */
	void ClearProviderSessionHistory(EUnrealClaudeProviderBackend Backend);

	/** Save project-local visible session to disk */
	bool SaveVisibleSession(EUnrealClaudeProviderBackend Backend, const FAgentBackendStatus& BackendStatus);

	/** Load project-local visible session from disk */
	FAgentSessionRestoreResult LoadVisibleSession(EUnrealClaudeProviderBackend Backend);

	/** Describe project-local visible sessions for the active and alternate providers */
	FAgentSavedSessionIndex DescribeVisibleSavedSessions(EUnrealClaudeProviderBackend Backend) const;

	/** Get project-local visible session file path */
	FString GetVisibleSessionFilePath(EUnrealClaudeProviderBackend Backend) const;

	/** Save provider session to disk */
	bool SaveSession(EUnrealClaudeProviderBackend Backend, const FAgentBackendStatus& BackendStatus);

	/** Load provider session from disk */
	FAgentSessionRestoreResult LoadSession(EUnrealClaudeProviderBackend Backend);

	/** Describe current/alternate/legacy saved session visibility for the active provider */
	FAgentSavedSessionIndex DescribeSavedSessions(EUnrealClaudeProviderBackend Backend) const;

	/** Check if a provider-scoped session exists */
	bool HasSavedSession(EUnrealClaudeProviderBackend Backend) const;

	/** Get provider-scoped session file path */
	FString GetSessionFilePath(EUnrealClaudeProviderBackend Backend) const;

	/** Get the legacy shared session file path */
	FString GetLegacySessionFilePath() const;

	/** Get the disk-backed persistent session artifact path (spec 621 §1 / Saved/UnrealClaude/claude_persistent_session.json) */
	FString GetPersistentSessionFilePath(EUnrealClaudeProviderBackend Backend) const;

	/** Read the existing persistent session id (no create, no side-effects). Returns empty if artifact missing/unreadable. */
	FString ReadPersistentSessionId(EUnrealClaudeProviderBackend Backend) const;

	/**
	 * Return the stable persistent session id for (Backend, Model). Creates a fresh UUID + artifact on first use or when the stored model differs.
	 * Always bumps last_used_utc when called. Returns empty on scope deny or write failure.
	 * OutWasExisting (optional) reports whether a valid artifact existed before this call (i.e. we emit --resume instead of --session-id).
	 */
	FString GetOrCreatePersistentSessionId(EUnrealClaudeProviderBackend Backend, const FString& Model, bool* OutWasExisting = nullptr);

	/**
	 * Delete the persistent session artifact if present (idempotent).
	 * Spec 621 v3 §2a: also UUID-match-delete the corresponding native Claude CLI jsonl under
	 * `~/.claude/projects/<project-key>/<uuid>.jsonl` so a rotated-out UUID cannot be replayed by the CLI.
	 * Cleanup is strict UUID-match only (no age sweep, no broad directory cleanup). Denials from the scope
	 * policy are logged and swallowed — see ClaudeSessionManager.cpp for the receipt.
	 */
	void ResetPersistentSession(EUnrealClaudeProviderBackend Backend);

	/**
	 * Compute the native Claude CLI project-key directory name from a project root path.
	 * Observed CLI encoding: `D:/VibeCode/Unreal/Poligon/Poligon1` → `D--VibeCode-Unreal-Poligon-Poligon1`
	 * (`:` → `--`, `/` and `\` → `-`, trailing slash/backslash dropped). Case on the drive letter is preserved.
	 * Public-static so ClaudeSessionManagerTests can validate the encoding directly.
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
		EUnrealClaudeProviderBackend Backend,
		ESessionStoreKind StoreKind) const;

	void AddExchangeForStore(
		EUnrealClaudeProviderBackend Backend,
		const FString& Prompt,
		const FString& Response,
		ESessionStoreKind StoreKind);

	void ClearHistoryForStore(
		EUnrealClaudeProviderBackend Backend,
		ESessionStoreKind StoreKind);

	FAgentSessionMetadata ReadSessionMetadata(
		const FString& SessionPath,
		EUnrealClaudeProviderBackend ExpectedBackend,
		bool bIsLegacySharedFile,
		const FString& StoreKind) const;

	bool LoadConversationHistory(
		const FString& SessionPath,
		EUnrealClaudeProviderBackend Backend,
		ESessionStoreKind StoreKind,
		FAgentSessionRestoreResult& OutResult);

	FAgentSessionMetadata BuildRuntimeSessionMetadata(
		EUnrealClaudeProviderBackend Backend,
		const FAgentBackendStatus& BackendStatus,
		int32 MessageCount,
		const FString& SessionPath,
		const FString& StoreKind) const;

	FString GetSessionFilePathForStore(
		EUnrealClaudeProviderBackend Backend,
		ESessionStoreKind StoreKind) const;

	FAgentSavedSessionIndex DescribeSavedSessionsForStore(
		EUnrealClaudeProviderBackend Backend,
		ESessionStoreKind StoreKind) const;

	bool SaveSessionForStore(
		EUnrealClaudeProviderBackend Backend,
		const FAgentBackendStatus& BackendStatus,
		ESessionStoreKind StoreKind);

	FAgentSessionRestoreResult LoadSessionForStore(
		EUnrealClaudeProviderBackend Backend,
		ESessionStoreKind StoreKind);

	FString MakeHistoryKey(
		EUnrealClaudeProviderBackend Backend,
		ESessionStoreKind StoreKind) const;

	EUnrealClaudeProviderBackend GetOtherBackend(EUnrealClaudeProviderBackend Backend) const;

	/**
	 * Spec 621 v3 §2a: UUID-match deletion of the native Claude CLI jsonl
	 * (`<native-projects-root>/<project-key>/<uuid>.jsonl`). Silent no-op when the file is missing.
	 * Scope-denial is logged via a receipt and swallowed. Never touches sibling jsonls.
	 */
	void DeleteMatchingNativeClaudeJsonl(const FString& UuidToDelete) const;

	TMap<FString, TArray<TPair<FString, FString>>> ConversationHistoryByStoreKey;
	int32 MaxHistorySize;
};
