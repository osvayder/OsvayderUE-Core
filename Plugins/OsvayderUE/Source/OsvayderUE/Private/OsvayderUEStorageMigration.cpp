// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUEStorageMigration.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "OsvayderUEModule.h"

namespace OsvayderUEStorageMigration
{
	const TCHAR* const PreferredStorageNamespace = TEXT("OsvayderUE");
	const TCHAR* const LegacyStorageNamespace = TEXT("OsvayderUE");
}

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
	OsvayderUEStorageMigration::ETestCopyFailureMode GTestCopyFailureMode =
		OsvayderUEStorageMigration::ETestCopyFailureMode::None;
#endif

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

	FString NormalizeDirectoryPath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return FString();
		}

		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	bool EnsureParentDirectoryExists(const FString& FilePath)
	{
		const FString Directory = FPaths::GetPath(FilePath);
		return Directory.IsEmpty() || IFileManager::Get().MakeDirectory(*Directory, true);
	}

	bool DeleteFileIfPresent(const FString& FilePath, const FString& LogicalName, const TCHAR* NamespaceLabel, FString& OutError)
	{
		if (FilePath.IsEmpty() || !IFileManager::Get().FileExists(*FilePath))
		{
			return true;
		}

		if (IFileManager::Get().Delete(*FilePath, false, true, true))
		{
			return true;
		}

		OutError = FString::Printf(
			TEXT("Could not delete %s copy for %s at %s"),
			NamespaceLabel,
			*LogicalName,
			*FilePath);
		return false;
	}

	FString MakeSiblingTempPath(const FString& TargetPath, const TCHAR* SuffixLabel)
	{
		return FString::Printf(
			TEXT("%s.%s.%s.tmp"),
			*TargetPath,
			SuffixLabel,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	bool AppendReceipt(
		const FString& PreferredPath,
		const FString& LegacyPath,
		const FString& LogicalName,
		const FString& Action,
		const bool bCopySucceeded,
		const FString& Detail)
	{
		const FString PreferredRoot = NormalizeDirectoryPath(FPaths::GetPath(PreferredPath));
		const FString ReceiptPath = OsvayderUEStorageMigration::GetMigrationReceiptLogPathForPreferredRoot(PreferredRoot);
		if (!EnsureParentDirectoryExists(ReceiptPath))
		{
			UE_LOG(
				LogOsvayderUE,
				Warning,
				TEXT("Storage namespace migration receipt parent directory could not be created for %s"),
				*ReceiptPath);
			return false;
		}

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("logical_name"), LogicalName);
		Root->SetStringField(TEXT("action"), Action);
		Root->SetStringField(TEXT("legacy_path"), NormalizeFilePath(LegacyPath));
		Root->SetStringField(TEXT("preferred_path"), NormalizeFilePath(PreferredPath));
		Root->SetBoolField(TEXT("copy_succeeded"), bCopySucceeded);
		if (!Detail.IsEmpty())
		{
			Root->SetStringField(TEXT("detail"), Detail);
		}

		FString JsonLine;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonLine);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			return false;
		}
		Writer->Close();
		JsonLine += TEXT("\n");

		return FFileHelper::SaveStringToFile(
			JsonLine,
			*ReceiptPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			EFileWrite::FILEWRITE_Append);
	}

	bool CopyLegacyToPreferred(
		const FString& PreferredPath,
		const FString& LegacyPath,
		const FString& LogicalName,
		TFunctionRef<bool(const FString& CandidatePath, FString& OutValidationError)> ValidatePath,
		const bool bRepairingPreferred,
		FString& OutError)
	{
		if (!EnsureParentDirectoryExists(PreferredPath))
		{
			OutError = FString::Printf(TEXT("Could not create preferred parent directory for %s"), *PreferredPath);
			return false;
		}

		const FString TempPath = MakeSiblingTempPath(PreferredPath, TEXT("migration"));
		const FString BackupPath = MakeSiblingTempPath(PreferredPath, TEXT("backup"));
		IFileManager& FileManager = IFileManager::Get();

		FileManager.Delete(*TempPath, false, true, true);
		FileManager.Delete(*BackupPath, false, true, true);

		const uint32 CopyResult = FileManager.Copy(*TempPath, *LegacyPath, true, true);
		if (CopyResult != COPY_OK)
		{
			OutError = FString::Printf(TEXT("Could not copy legacy storage from %s to %s"), *LegacyPath, *PreferredPath);
			FileManager.Delete(*TempPath, false, true, true);
			AppendReceipt(
				PreferredPath,
				LegacyPath,
				LogicalName,
				bRepairingPreferred ? TEXT("repair_from_legacy_failed") : TEXT("migrate_from_legacy_failed"),
				false,
				OutError);
			UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *OutError);
			return false;
		}

		if (!FileManager.FileExists(*TempPath))
		{
			OutError = FString::Printf(TEXT("Legacy storage temp copy was not materialized for %s"), *PreferredPath);
			FileManager.Delete(*TempPath, false, true, true);
			AppendReceipt(
				PreferredPath,
				LegacyPath,
				LogicalName,
				bRepairingPreferred ? TEXT("repair_from_legacy_failed") : TEXT("migrate_from_legacy_failed"),
				false,
				OutError);
			UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *OutError);
			return false;
		}

		FString TempValidationError;
		if (!ValidatePath(TempPath, TempValidationError))
		{
			OutError = FString::Printf(
				TEXT("Legacy storage temp copy for %s failed validation at %s (%s)"),
				*LogicalName,
				*TempPath,
				*TempValidationError);
			FileManager.Delete(*TempPath, false, true, true);
			AppendReceipt(
				PreferredPath,
				LegacyPath,
				LogicalName,
				bRepairingPreferred ? TEXT("repair_from_legacy_failed") : TEXT("migrate_from_legacy_failed"),
				false,
				OutError);
			UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *OutError);
			return false;
		}

#if WITH_DEV_AUTOMATION_TESTS
		if (GTestCopyFailureMode == OsvayderUEStorageMigration::ETestCopyFailureMode::FailBeforePreferredReplace)
		{
			OutError = FString::Printf(
				TEXT("Test-only storage migration failure injected before replacing preferred copy for %s"),
				*LogicalName);
			FileManager.Delete(*TempPath, false, true, true);
			AppendReceipt(
				PreferredPath,
				LegacyPath,
				LogicalName,
				bRepairingPreferred ? TEXT("repair_from_legacy_failed") : TEXT("migrate_from_legacy_failed"),
				false,
				OutError);
			UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *OutError);
			return false;
		}
#endif

		const bool bPreferredExists = FileManager.FileExists(*PreferredPath);
		if (bPreferredExists)
		{
			if (!FileManager.Move(*BackupPath, *PreferredPath, true, true, true, true))
			{
				OutError = FString::Printf(TEXT("Could not stage preferred backup for safe migration at %s"), *PreferredPath);
				FileManager.Delete(*TempPath, false, true, true);
				FileManager.Delete(*BackupPath, false, true, true);
				AppendReceipt(
					PreferredPath,
					LegacyPath,
					LogicalName,
					bRepairingPreferred ? TEXT("repair_from_legacy_failed") : TEXT("migrate_from_legacy_failed"),
					false,
					OutError);
				UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *OutError);
				return false;
			}
		}

		const uint32 PromoteResult = FileManager.Copy(*PreferredPath, *TempPath, true, true);
		if (PromoteResult != COPY_OK || !FileManager.FileExists(*PreferredPath))
		{
			FileManager.Delete(*PreferredPath, false, true, true);
			bool bRestoreSucceeded = !bPreferredExists;
			if (bPreferredExists)
			{
				bRestoreSucceeded = FileManager.Move(*PreferredPath, *BackupPath, true, true, true, true);
			}

			OutError = FString::Printf(
				TEXT("Could not promote migrated storage into preferred path %s%s"),
				*PreferredPath,
				bRestoreSucceeded ? TEXT("") : TEXT(" and could not restore the staged preferred backup."));
			FileManager.Delete(*TempPath, false, true, true);
			if (bRestoreSucceeded)
			{
				FileManager.Delete(*BackupPath, false, true, true);
			}
			AppendReceipt(
				PreferredPath,
				LegacyPath,
				LogicalName,
				bRepairingPreferred ? TEXT("repair_from_legacy_failed") : TEXT("migrate_from_legacy_failed"),
				false,
				OutError);
			UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *OutError);
			return false;
		}

		FileManager.Delete(*TempPath, false, true, true);
		if (bPreferredExists)
		{
			FileManager.Delete(*BackupPath, false, true, true);
		}

		const FString Action = bRepairingPreferred ? TEXT("repair_from_legacy") : TEXT("migrate_from_legacy");
		const FString Detail = bRepairingPreferred
			? TEXT("Preferred copy was invalid; repaired from legacy namespace.")
			: TEXT("Preferred copy was missing; hydrated from legacy namespace.");
		AppendReceipt(PreferredPath, LegacyPath, LogicalName, Action, true, Detail);
		UE_LOG(
			LogOsvayderUE,
			Log,
			TEXT("Storage namespace %s completed for %s (%s -> %s)"),
			bRepairingPreferred ? TEXT("repair") : TEXT("migration"),
			*LogicalName,
			*LegacyPath,
			*PreferredPath);
		return true;
	}
}

FString OsvayderUEStorageMigration::GetPreferredSavedRoot()
{
	return NormalizeDirectoryPath(FPaths::Combine(FPaths::ProjectSavedDir(), PreferredStorageNamespace));
}

FString OsvayderUEStorageMigration::GetLegacySavedRoot()
{
	return NormalizeDirectoryPath(FPaths::Combine(FPaths::ProjectSavedDir(), LegacyStorageNamespace));
}

FString OsvayderUEStorageMigration::DeriveLegacyRootFromPreferred(const FString& PreferredRoot)
{
	const FString NormalizedPreferredRoot = NormalizeDirectoryPath(PreferredRoot);
	if (NormalizedPreferredRoot.IsEmpty())
	{
		return GetLegacySavedRoot();
	}

	const FString ParentDir = NormalizeDirectoryPath(FPaths::GetPath(NormalizedPreferredRoot));
	return ParentDir.IsEmpty()
		? GetLegacySavedRoot()
		: NormalizeDirectoryPath(FPaths::Combine(ParentDir, LegacyStorageNamespace));
}

FString OsvayderUEStorageMigration::GetMigrationReceiptLogPathForPreferredRoot(const FString& PreferredRoot)
{
	return NormalizeFilePath(FPaths::Combine(PreferredRoot, TEXT("MigrationReceipts"), TEXT("storage_namespace_migrations.jsonl")));
}

bool OsvayderUEStorageMigration::ResolveManagedReadPath(
	const FString& PreferredPath,
	const FString& LegacyPath,
	const FString& LogicalName,
	TFunctionRef<bool(const FString& CandidatePath, FString& OutValidationError)> ValidatePath,
	FManagedReadResult& OutResult,
	FString& OutError)
{
	OutResult = FManagedReadResult();
	OutResult.PreferredPath = NormalizeFilePath(PreferredPath);
	OutResult.LegacyPath = NormalizeFilePath(LegacyPath);
	OutError.Reset();

	FString PreferredValidationError;
	const bool bPreferredExists = IFileManager::Get().FileExists(*OutResult.PreferredPath);
	const bool bPreferredValid = bPreferredExists && ValidatePath(OutResult.PreferredPath, PreferredValidationError);
	if (bPreferredValid)
	{
		OutResult.ResolvedPath = OutResult.PreferredPath;
		OutResult.bResolved = true;
		return true;
	}

	OutResult.bPreferredWasInvalid = bPreferredExists;

	FString LegacyValidationError;
	const bool bLegacyExists = IFileManager::Get().FileExists(*OutResult.LegacyPath);
		const bool bLegacyValid = bLegacyExists && ValidatePath(OutResult.LegacyPath, LegacyValidationError);
	if (bLegacyValid)
	{
		FString CopyError;
		const bool bCopySucceeded = CopyLegacyToPreferred(
			OutResult.PreferredPath,
			OutResult.LegacyPath,
			LogicalName,
			ValidatePath,
			bPreferredExists,
			CopyError);
		OutResult.bUsedLegacyFallback = !bCopySucceeded;
		OutResult.bMigratedToPreferred = bCopySucceeded && !bPreferredExists;
		OutResult.bRepairedPreferredFromLegacy = bCopySucceeded && bPreferredExists;
		OutResult.ResolvedPath = bCopySucceeded ? OutResult.PreferredPath : OutResult.LegacyPath;
		OutResult.bResolved = true;
		if (!bCopySucceeded)
		{
			OutError = CopyError;
		}
		return true;
	}

	if (bPreferredExists && !PreferredValidationError.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Preferred storage for %s is invalid at %s (%s) and legacy fallback was unavailable (%s)."),
			*LogicalName,
			*OutResult.PreferredPath,
			*PreferredValidationError,
			bLegacyExists ? *LegacyValidationError : TEXT("legacy file missing"));
		return false;
	}

	OutError = FString::Printf(
		TEXT("Managed storage for %s was not found. Preferred path: %s. Legacy path: %s."),
		*LogicalName,
		*OutResult.PreferredPath,
		*OutResult.LegacyPath);
	return false;
}

bool OsvayderUEStorageMigration::EnsurePreferredFileHydrated(
	const FString& PreferredPath,
	const FString& LegacyPath,
	const FString& LogicalName,
	TFunctionRef<bool(const FString& CandidatePath, FString& OutValidationError)> ValidatePath,
	bool& bOutHydrated,
	FString& OutError)
{
	FManagedReadResult Result;
	const bool bResolved = ResolveManagedReadPath(
		PreferredPath,
		LegacyPath,
		LogicalName,
		ValidatePath,
		Result,
		OutError);

	if (!bResolved)
	{
		bOutHydrated = false;
		if (!IFileManager::Get().FileExists(*NormalizeFilePath(PreferredPath))
			&& !IFileManager::Get().FileExists(*NormalizeFilePath(LegacyPath)))
		{
			OutError.Reset();
			return true;
		}

		return false;
	}

	bOutHydrated = Result.bMigratedToPreferred || Result.bRepairedPreferredFromLegacy;
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
void OsvayderUEStorageMigration::SetTestCopyFailureMode(const ETestCopyFailureMode InFailureMode)
{
	GTestCopyFailureMode = InFailureMode;
}

void OsvayderUEStorageMigration::ClearTestCopyFailureMode()
{
	GTestCopyFailureMode = ETestCopyFailureMode::None;
}
#endif

bool OsvayderUEStorageMigration::DeleteManagedFileCopies(
	const FString& PreferredPath,
	const FString& LegacyPath,
	const FString& LogicalName,
	FString& OutError)
{
	const FString NormalizedPreferredPath = NormalizeFilePath(PreferredPath);
	const FString NormalizedLegacyPath = NormalizeFilePath(LegacyPath);
	const bool bPreferredExisted = !NormalizedPreferredPath.IsEmpty() && IFileManager::Get().FileExists(*NormalizedPreferredPath);
	const bool bLegacyExisted = !NormalizedLegacyPath.IsEmpty() && IFileManager::Get().FileExists(*NormalizedLegacyPath);
	OutError.Reset();

	if (!DeleteFileIfPresent(NormalizedPreferredPath, LogicalName, TEXT("preferred"), OutError))
	{
		AppendReceipt(
			NormalizedPreferredPath,
			NormalizedLegacyPath,
			LogicalName,
			TEXT("delete_live_state_failed"),
			false,
			OutError);
		UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *OutError);
		return false;
	}

	const bool bSameManagedPath = !NormalizedPreferredPath.IsEmpty()
		&& !NormalizedLegacyPath.IsEmpty()
		&& NormalizedPreferredPath.Equals(NormalizedLegacyPath, ESearchCase::IgnoreCase);
	if (!bSameManagedPath
		&& !DeleteFileIfPresent(NormalizedLegacyPath, LogicalName, TEXT("legacy"), OutError))
	{
		AppendReceipt(
			NormalizedPreferredPath,
			NormalizedLegacyPath,
			LogicalName,
			TEXT("delete_live_state_failed"),
			false,
			OutError);
		UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *OutError);
		return false;
	}

	if (!bPreferredExisted && !bLegacyExisted)
	{
		return true;
	}

	AppendReceipt(
		NormalizedPreferredPath,
		NormalizedLegacyPath,
		LogicalName,
		TEXT("delete_live_state"),
		true,
		TEXT("Terminal cleanup deleted the live preferred/legacy managed state copies."));
	UE_LOG(
		LogOsvayderUE,
		Log,
		TEXT("Storage namespace terminal cleanup deleted live state for %s (%s | %s)"),
		*LogicalName,
		*NormalizedPreferredPath,
		*NormalizedLegacyPath);
	return true;
}
