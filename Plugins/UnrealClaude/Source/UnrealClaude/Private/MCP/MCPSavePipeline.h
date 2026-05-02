// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UObject;
class UPackage;

namespace UnrealClaude
{
namespace SavePipeline
{

/** Per-phase outcome for a single package. */
struct FLifecyclePhaseResult
{
	/** Long package name (e.g. "/Game/BP_Example"). */
	FString PackageName;

	/** Phase label (e.g. "save", "skipped"). */
	FString Phase;

	/** True when the phase succeeded. Meaningful for "save"; always true for "skipped". */
	bool bSuccess = false;

	/** Error description when bSuccess is false. */
	FString Error;

	/** Documented reason when the package was intentionally skipped (e.g. "level_package_deferred"). */
	FString SkippedReason;
};

/** Aggregate outcome of a lifecycle run. */
struct FLifecycleOutcome
{
	/** Packages successfully saved to disk. */
	TArray<FLifecyclePhaseResult> Saved;

	/** Packages that failed in some phase. */
	TArray<FLifecyclePhaseResult> Failed;

	/** Packages deferred by policy (e.g. level packages per A15). */
	TArray<FLifecyclePhaseResult> Deferred;

	/** Blueprint packages that compiled cleanly during Phase A (P2). */
	TArray<FLifecyclePhaseResult> Compiled;

	/** On-disk filenames successfully checked out by Phase B (P3). */
	TArray<FString> SourceControlCheckedOut;

	/** Per-file source-control warnings surfaced by Phase B (P3). */
	TArray<FLifecyclePhaseResult> SourceControlWarnings;

	/** True when ISourceControlModule::Get().IsEnabled() returned true during the run. */
	bool bSourceControlActive = false;

	/** True if the pipeline touched at least one asset package (level-only runs leave this false). */
	bool bAttemptedAssetSave = false;

	/**
	 * Long package names for which Phase D invoked FAssetRegistryModule::AssetCreated
	 * (P4). Populated for genuinely new assets — saves where bFileExistedBeforeSave
	 * was false AND the package reached Outcome.Saved (i.e. Phase C actually landed
	 * bytes on disk). Empty array when no new assets were saved in this run.
	 */
	TArray<FString> NewlyRegistered;
};

/** Input for a single lifecycle invocation. */
struct FSaveSpec
{
	/** Packages to process (already filtered by dirty-snapshot diff). */
	TArray<UPackage*> Packages;

	/** If true, the caller is the explicit save_asset tool rather than the lifecycle wrapper. */
	bool bIsExplicitToolCall = false;
};

/** Run the P1 save pipeline. Must be called on the game thread. */
FLifecycleOutcome Run(const FSaveSpec& Spec);

/** True when the package's top-level object is a UWorld, the PKG_ContainsMap flag is set, or the resolved filename resolves to a .umap on disk. */
bool IsLevelPackage(UPackage* Package);

/**
 * P4 Phase D: pure predicate — should the AssetRegistry notification fire for a
 * given save outcome? True only when the file was genuinely new on disk AND the
 * save phase reported success. Exposed for unit tests so the truth table can be
 * asserted directly without driving the full pipeline.
 */
bool ShouldNotifyAssetRegistryCreated(bool bFileExistedBeforeSave, bool bSavedSuccessfully);

/**
 * P4 Phase D: notify the asset registry that a newly-saved package now exists,
 * and record the package name in Outcome.NewlyRegistered. Silently no-ops when
 * the package has no top-level public+standalone object (e.g. stripped content)
 * to keep spec D2's "failure on one asset does not short-circuit others" guarantee.
 * Game-thread only. Testing::SetAssetCreatedOverride replaces the registry call
 * so automation tests can capture invocations without side-effects on the live
 * global asset registry.
 */
void NotifyAssetRegistryCreated(UPackage* Package, FLifecycleOutcome& Outcome);

/** Test-only hooks that let automation tests substitute the Phase B source-control probes without registering a live provider. Setters are safe to call on the game thread; calls must be paired with a Clear() call. */
namespace Testing
{
	/** Replace the default `ISourceControlModule::Get().IsEnabled()` probe. Pass a null TFunction to revert. */
	void SetSourceControlIsEnabledOverride(TFunction<bool()> Override);

	/** Replace the default `USourceControlHelpers::CheckOutOrAddFiles(..., bSilent=true)` call. Pass a null TFunction to revert. */
	void SetSourceControlCheckOutOverride(TFunction<bool(const TArray<FString>&)> Override);

	/** Clear both source-control overrides in one call. */
	void ClearSourceControlOverrides();

	/**
	 * P4: replace the default `FAssetRegistryModule::AssetCreated(UObject*)` call
	 * with a test-provided capture. Lets automation tests assert that Phase D
	 * notified the registry with the expected main asset without touching the
	 * live global registry. Pass a null TFunction to revert.
	 */
	void SetAssetCreatedOverride(TFunction<void(UObject*)> Override);

	/** P4: clear the AssetCreated override in one call. */
	void ClearAssetCreatedOverride();

	/**
	 * P5: replace the default `UPackage::SavePackage(...)` call in Phase C with
	 * a test-provided stub. Parameters are (Package, ResolvedFilename) and the
	 * return value decides Phase C's `bOk`. Lets automation tests exercise
	 * compile-ok + save-fail combinations without depending on filesystem
	 * permissions or transient-path save semantics. Pass a null TFunction to
	 * revert.
	 */
	void SetSavePackageOverride(TFunction<bool(UPackage*, const FString&)> Override);

	/** P5: clear the SavePackage override in one call. */
	void ClearSavePackageOverride();
}

} // namespace SavePipeline
} // namespace UnrealClaude
