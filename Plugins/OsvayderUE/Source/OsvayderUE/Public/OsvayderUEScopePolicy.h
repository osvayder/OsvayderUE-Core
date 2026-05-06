// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OsvayderUESettings.h"

/**
 * Runtime scope enforcement for the Osvayder UE plugin.
 * Validates file paths against current ScopeMode settings.
 */
/** Classification of a write path */
enum class EScopeClassification : uint8
{
	/** Allowed as user-facing mutation (plugin/docs/bridge) */
	UserMutation,
	/** Allowed as internal runtime state (Saved/OsvayderUE and legacy Saved/OsvayderUE) */
	InternalState,
	/** Denied — outside all allowed roots */
	Denied,
};

class OSVAYDERUE_API FOsvayderUEScopePolicy
{
public:
	struct FScopeCheckResult
	{
		bool bAllowed = false;
		EScopeClassification Classification = EScopeClassification::Denied;
		FString MatchedRoot;
		FString DenialReason;
	};

	/**
	 * Check if a project-relative path is allowed for write under current scope policy.
	 * @param ProjectRelativePath - path relative to project root
	 * @return structured result with allowed/denied + reason
	 */
	static FScopeCheckResult IsWriteAllowed(const FString& ProjectRelativePath);

	/**
	 * Check if an absolute path is allowed for write under current scope policy.
	 * @param AbsolutePath - full filesystem path
	 * @return structured result
	 */
	static FScopeCheckResult IsAbsoluteWriteAllowed(const FString& AbsolutePath);

	/** Get list of currently allowed write roots (project-relative) */
	static TArray<FString> GetAllowedWriteRoots();

private:
	static const TArray<FString>& GetPluginOnlyRoots();
};
