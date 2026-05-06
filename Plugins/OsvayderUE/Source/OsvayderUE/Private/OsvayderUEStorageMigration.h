#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

namespace OsvayderUEStorageMigration
{
	extern const TCHAR* const PreferredStorageNamespace;
	extern const TCHAR* const LegacyStorageNamespace;

	struct FManagedReadResult
	{
		FString PreferredPath;
		FString LegacyPath;
		FString ResolvedPath;
		bool bResolved = false;
		bool bUsedLegacyFallback = false;
		bool bMigratedToPreferred = false;
		bool bRepairedPreferredFromLegacy = false;
		bool bPreferredWasInvalid = false;
	};

	FString GetPreferredSavedRoot();
	FString GetLegacySavedRoot();
	FString DeriveLegacyRootFromPreferred(const FString& PreferredRoot);
	FString GetMigrationReceiptLogPathForPreferredRoot(const FString& PreferredRoot);

	bool ResolveManagedReadPath(
		const FString& PreferredPath,
		const FString& LegacyPath,
		const FString& LogicalName,
		TFunctionRef<bool(const FString& CandidatePath, FString& OutValidationError)> ValidatePath,
		FManagedReadResult& OutResult,
		FString& OutError);

	bool EnsurePreferredFileHydrated(
		const FString& PreferredPath,
		const FString& LegacyPath,
		const FString& LogicalName,
		TFunctionRef<bool(const FString& CandidatePath, FString& OutValidationError)> ValidatePath,
		bool& bOutHydrated,
		FString& OutError);

	bool DeleteManagedFileCopies(
		const FString& PreferredPath,
		const FString& LegacyPath,
		const FString& LogicalName,
		FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
	enum class ETestCopyFailureMode : uint8
	{
		None,
		FailBeforePreferredReplace
	};

	void SetTestCopyFailureMode(ETestCopyFailureMode InFailureMode);
	void ClearTestCopyFailureMode();
#endif
}
