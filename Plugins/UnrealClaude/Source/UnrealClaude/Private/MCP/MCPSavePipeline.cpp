// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPSavePipeline.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "ISourceControlModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SourceControlHelpers.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "UnrealClaudeModule.h"

namespace UnrealClaude
{
namespace SavePipeline
{

namespace
{
	/** Test-only overrides for the source-control probes exercised by Phase B. Game-thread-only access. */
	TFunction<bool()> GTestingSourceControlIsEnabledOverride;
	TFunction<bool(const TArray<FString>&)> GTestingSourceControlCheckOutOverride;

	/** Test-only override for the Phase D AssetRegistry notify (P4). Game-thread-only access. */
	TFunction<void(UObject*)> GTestingAssetCreatedOverride;

	/** Test-only override for the Phase C UPackage::SavePackage call (P5). Game-thread-only access. */
	TFunction<bool(UPackage*, const FString&)> GTestingSavePackageOverride;

	bool ResolveSourceControlEnabled()
	{
		if (GTestingSourceControlIsEnabledOverride)
		{
			return GTestingSourceControlIsEnabledOverride();
		}
		return ISourceControlModule::Get().IsEnabled();
	}

	bool DispatchSourceControlCheckOut(const TArray<FString>& Files)
	{
		if (GTestingSourceControlCheckOutOverride)
		{
			return GTestingSourceControlCheckOutOverride(Files);
		}
		return USourceControlHelpers::CheckOutOrAddFiles(Files, /*bSilent=*/true);
	}

	/** Find the first UBlueprint owned directly by the package (top-level). Returns nullptr when the package carries no Blueprint. */
	UBlueprint* FindBlueprintInPackage(UPackage* Package)
	{
		UBlueprint* Found = nullptr;
		if (!Package)
		{
			return nullptr;
		}
		ForEachObjectWithPackage(Package, [&Found](UObject* Object) -> bool
		{
			if (UBlueprint* AsBlueprint = Cast<UBlueprint>(Object))
			{
				Found = AsBlueprint;
				return false;
			}
			return true;
		}, false);
		return Found;
	}

	/** Build a short error summary from a compiler results log. `FCompilerResultsLog` in UE 5.7 exposes `NumErrors` and a `Messages` array but no `GetSummary()` method, so we synthesise one by counting errors and concatenating up to three tokenised error lines. */
	FString BuildCompileErrorSummary(const FCompilerResultsLog& Results, const FString& BlueprintName)
	{
		FString Summary = FString::Printf(TEXT("Blueprint compile failed for %s: %d error(s), %d warning(s)"),
			*BlueprintName,
			Results.NumErrors,
			Results.NumWarnings);

		int32 Quoted = 0;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			if (Quoted >= 3)
			{
				break;
			}
			if (Message->GetSeverity() == EMessageSeverity::Error)
			{
				Summary += FString::Printf(TEXT(" | %s"), *Message->ToText().ToString());
				++Quoted;
			}
		}
		return Summary;
	}
}

bool ShouldNotifyAssetRegistryCreated(const bool bFileExistedBeforeSave, const bool bSavedSuccessfully)
{
	// P4 Phase D truth table:
	// - bFileExistedBeforeSave=true  ⇒ not a new asset; registry already has it ⇒ skip
	// - bSavedSuccessfully=false     ⇒ save did not land ⇒ skip (no real asset to register)
	// - both false/true combos       ⇒ skip
	// - (new file AND save succeeded) ⇒ notify
	return !bFileExistedBeforeSave && bSavedSuccessfully;
}

void NotifyAssetRegistryCreated(UPackage* Package, FLifecycleOutcome& Outcome)
{
	if (!Package)
	{
		return;
	}

	// Find the top-level asset. SavePackageArgs.TopLevelFlags = RF_Public |
	// RF_Standalone (set by Phase C), so any main asset in the package carries
	// those flags. Mirrors the existing FindBlueprintInPackage shape in this
	// file but casts-generic — any UObject qualifies.
	UObject* MainAsset = nullptr;
	ForEachObjectWithPackage(Package, [&MainAsset](UObject* Obj) -> bool
	{
		if (Obj && Obj->HasAnyFlags(RF_Public | RF_Standalone))
		{
			MainAsset = Obj;
			return false;
		}
		return true;
	}, false);

	// Spec D2: if the package has no main asset, do NOT abort other Phase D
	// notifications. Surface a structured failure (P5 A-P5-5) so the low-
	// frequency "saved a package with no RF_Public|RF_Standalone top-level
	// asset" case is visible to agents instead of silently invisible. Caller
	// loops over the remainder regardless.
	if (!MainAsset)
	{
		FLifecyclePhaseResult RegistryFailure;
		RegistryFailure.PackageName = Package->GetName();
		RegistryFailure.Phase = TEXT("registry_notify");
		RegistryFailure.bSuccess = false;
		RegistryFailure.Error = TEXT("Package has no RF_Public|RF_Standalone top-level asset to notify the AssetRegistry about.");
		Outcome.Failed.Add(MoveTemp(RegistryFailure));
		return;
	}

	if (GTestingAssetCreatedOverride)
	{
		GTestingAssetCreatedOverride(MainAsset);
	}
	else
	{
		FAssetRegistryModule::AssetCreated(MainAsset);
	}
	Outcome.NewlyRegistered.Add(Package->GetName());
}

bool IsLevelPackage(UPackage* Package)
{
	if (!Package)
	{
		return false;
	}

	if (Package->ContainsMap())
	{
		return true;
	}

	bool bContainsWorld = false;
	ForEachObjectWithPackage(Package, [&bContainsWorld](UObject* Object) -> bool
	{
		if (Object && Object->IsA<UWorld>())
		{
			bContainsWorld = true;
			return false;
		}
		return true;
	}, false);

	if (bContainsWorld)
	{
		return true;
	}

	FString Filename;
	if (FPackageName::TryConvertLongPackageNameToFilename(
			Package->GetName(), Filename, FPackageName::GetMapPackageExtension()))
	{
		if (FPaths::FileExists(Filename))
		{
			return true;
		}
	}

	return false;
}

FLifecycleOutcome Run(const FSaveSpec& Spec)
{
	FLifecycleOutcome Outcome;

	struct FPendingSave
	{
		UPackage* Package = nullptr;
		FString Filename;
		bool bFileExistedBeforeSave = false;
	};
	TArray<FPendingSave> PendingSaves;

	// Phase A: classify and compile. Survivors are queued for Phase B + C.
	for (UPackage* Package : Spec.Packages)
	{
		if (!Package)
		{
			continue;
		}

		const bool bIsLevelPackage = IsLevelPackage(Package);

		// P6: explicit save_asset/tool calls (bIsExplicitToolCall=true) respect
		// user intent and persist level packages. The autonomous mutation-wrapper
		// path (bIsExplicitToolCall=false, the default set by
		// MCPToolRegistry::ExecuteTool at :1114) continues to defer level
		// packages per A15/D4.1: the wrapper auto-saves "everything dirty"
		// indiscriminately, so opting level packages out prevents accidental
		// mass-save of every loaded map. An explicit tool call is a targeted
		// ask — treat it as user-consented to whatever the package is.
		if (bIsLevelPackage && !Spec.bIsExplicitToolCall)
		{
			FLifecyclePhaseResult Skipped;
			Skipped.PackageName = Package->GetName();
			Skipped.Phase = TEXT("skipped");
			Skipped.bSuccess = true;
			Skipped.SkippedReason = TEXT("level_package_deferred");
			Outcome.Deferred.Add(MoveTemp(Skipped));
			continue;
		}

		// Phase A: compile-before-save for Blueprint-containing packages (spec D1).
		if (UBlueprint* Blueprint = FindBlueprintInPackage(Package))
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			FCompilerResultsLog Results;
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

			if (Results.NumErrors > 0)
			{
				FLifecyclePhaseResult CompileFailure;
				CompileFailure.PackageName = Package->GetName();
				CompileFailure.Phase = TEXT("compile");
				CompileFailure.bSuccess = false;
				CompileFailure.Error = BuildCompileErrorSummary(Results, Blueprint->GetName());
				Outcome.Failed.Add(MoveTemp(CompileFailure));
				continue;
			}

			FLifecyclePhaseResult CompileSuccess;
			CompileSuccess.PackageName = Package->GetName();
			CompileSuccess.Phase = TEXT("compile");
			CompileSuccess.bSuccess = true;
			Outcome.Compiled.Add(MoveTemp(CompileSuccess));

			Blueprint->PostEditChange();
		}

		// Resolve the on-disk filename up front so Phase B can enumerate existing files
		// without repeating the call during the save loop.
		FPendingSave Pending;
		Pending.Package = Package;
		const FString PackageExtension = bIsLevelPackage
			? FPackageName::GetMapPackageExtension()
			: FPackageName::GetAssetPackageExtension();
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				Package->GetName(),
				Pending.Filename,
				PackageExtension))
		{
			FLifecyclePhaseResult Failed;
			Failed.PackageName = Package->GetName();
			Failed.Phase = TEXT("save");
			Failed.bSuccess = false;
			Failed.Error = TEXT("Could not resolve package filename");
			Outcome.Failed.Add(MoveTemp(Failed));
			continue;
		}
		Pending.bFileExistedBeforeSave = FPaths::FileExists(Pending.Filename);
		PendingSaves.Add(MoveTemp(Pending));
	}

	// Phase B (P3): silent source-control checkout for existing-on-disk files.
	// Per spec D2: a checkout denial for any file MUST NOT short-circuit save of the
	// others — pipeline continues to Phase C regardless. Save of a read-only file will
	// surface in `Failed` with phase="save" naturally.
	Outcome.bSourceControlActive = ResolveSourceControlEnabled();
	if (Outcome.bSourceControlActive)
	{
		TArray<FString> FilesToCheckout;
		FilesToCheckout.Reserve(PendingSaves.Num());
		for (const FPendingSave& Pending : PendingSaves)
		{
			if (Pending.bFileExistedBeforeSave)
			{
				FilesToCheckout.Add(Pending.Filename);
			}
		}

		if (FilesToCheckout.Num() > 0)
		{
			const bool bCheckoutOk = DispatchSourceControlCheckOut(FilesToCheckout);
			if (bCheckoutOk)
			{
				Outcome.SourceControlCheckedOut.Append(FilesToCheckout);
			}
			else
			{
				// Coarse-grained warning: the batch call reported failure. Per-file
				// state querying is deferred — agent can still read the save-phase
				// failures to know which files ended up read-only on disk.
				FLifecyclePhaseResult Warning;
				Warning.PackageName = TEXT("(batch)");
				Warning.Phase = TEXT("source_control");
				Warning.bSuccess = false;
				Warning.Error = FString::Printf(
					TEXT("USourceControlHelpers::CheckOutOrAddFiles returned false for %d file(s)"),
					FilesToCheckout.Num());
				Outcome.SourceControlWarnings.Add(MoveTemp(Warning));
			}
		}
	}

	// Phase C: save. Each survivor is persisted independently; one failure does not
	// affect the others.
	for (const FPendingSave& Pending : PendingSaves)
	{
		Outcome.bAttemptedAssetSave = true;

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		SaveArgs.bSlowTask = false;

		const bool bOk = GTestingSavePackageOverride
			? GTestingSavePackageOverride(Pending.Package, Pending.Filename)
			: UPackage::SavePackage(Pending.Package, nullptr, *Pending.Filename, SaveArgs);

		FLifecyclePhaseResult Result;
		Result.PackageName = Pending.Package->GetName();
		Result.Phase = TEXT("save");
		Result.bSuccess = bOk;
		if (bOk)
		{
			Outcome.Saved.Add(MoveTemp(Result));
		}
		else
		{
			Result.Error = TEXT("UPackage::SavePackage returned false");
			Outcome.Failed.Add(MoveTemp(Result));
		}
	}

	// Phase D (P4): AssetRegistry notifications for genuinely new assets. Default
	// ordering per spec §"The pipeline": AFTER Phase C. If EMP-1 empirical test
	// (UnrealClaude.MutationLifecycle.Pipeline_Save_NewlyCreatedDataAsset_Silent)
	// surfaces a modal dialog on SavePackage for a never-saved /Game/ asset, the
	// mitigation branch flips this ordering to BEFORE save with a pre-resolved
	// filename via FPackageName::TryConvertLongPackageNameToFilename. The
	// filename resolution already happens at :198-202 of Phase A, so the flip
	// would be a code-move, not a new code site.
	//
	// Spec D2 preserved per-asset: NotifyAssetRegistryCreated silently no-ops
	// when a package has no main asset; the loop continues with the next one.
	{
		TSet<FString> SavedPackageNames;
		SavedPackageNames.Reserve(Outcome.Saved.Num());
		for (const FLifecyclePhaseResult& Saved : Outcome.Saved)
		{
			SavedPackageNames.Add(Saved.PackageName);
		}

		for (const FPendingSave& Pending : PendingSaves)
		{
			if (!Pending.Package)
			{
				continue;
			}
			const bool bSavedSuccessfully = SavedPackageNames.Contains(Pending.Package->GetName());
			if (!ShouldNotifyAssetRegistryCreated(Pending.bFileExistedBeforeSave, bSavedSuccessfully))
			{
				continue;
			}
			NotifyAssetRegistryCreated(Pending.Package, Outcome);
		}
	}

	UE_LOG(LogUnrealClaude, Log,
		TEXT("MCPSavePipeline::Run — saved=%d failed=%d deferred=%d compiled=%d sc_active=%s sc_checked_out=%d sc_warnings=%d attempted_asset_save=%s newly_registered=%d"),
		Outcome.Saved.Num(),
		Outcome.Failed.Num(),
		Outcome.Deferred.Num(),
		Outcome.Compiled.Num(),
		Outcome.bSourceControlActive ? TEXT("true") : TEXT("false"),
		Outcome.SourceControlCheckedOut.Num(),
		Outcome.SourceControlWarnings.Num(),
		Outcome.bAttemptedAssetSave ? TEXT("true") : TEXT("false"),
		Outcome.NewlyRegistered.Num());

	return Outcome;
}

namespace Testing
{
	void SetSourceControlIsEnabledOverride(TFunction<bool()> Override)
	{
		GTestingSourceControlIsEnabledOverride = MoveTemp(Override);
	}

	void SetSourceControlCheckOutOverride(TFunction<bool(const TArray<FString>&)> Override)
	{
		GTestingSourceControlCheckOutOverride = MoveTemp(Override);
	}

	void ClearSourceControlOverrides()
	{
		GTestingSourceControlIsEnabledOverride = TFunction<bool()>();
		GTestingSourceControlCheckOutOverride = TFunction<bool(const TArray<FString>&)>();
	}

	void SetAssetCreatedOverride(TFunction<void(UObject*)> Override)
	{
		GTestingAssetCreatedOverride = MoveTemp(Override);
	}

	void ClearAssetCreatedOverride()
	{
		GTestingAssetCreatedOverride = TFunction<void(UObject*)>();
	}

	void SetSavePackageOverride(TFunction<bool(UPackage*, const FString&)> Override)
	{
		GTestingSavePackageOverride = MoveTemp(Override);
	}

	void ClearSavePackageOverride()
	{
		GTestingSavePackageOverride = TFunction<bool(UPackage*, const FString&)>();
	}
}

} // namespace SavePipeline
} // namespace UnrealClaude
