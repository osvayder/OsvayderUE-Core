// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_DependencyHealth.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "OsvayderUEReportArtifacts.h"

namespace
{
	constexpr int32 MaxLogExcerptLines = 3;
	constexpr int32 MaxPackageExamples = 8;
	constexpr int32 MaxReferencerExamples = 10;

	struct FDependencyConfigSnapshot
	{
		TArray<FString> MapsToCook;
		TArray<FString> DirectoriesToAlwaysCook;
		FString DefaultPlatformService;
		FString GameNetDriver;
	};

	struct FDependencyFinding
	{
		FString FindingKey;
		FString FindingKind;
		FString Subject;
		TArray<FString> ObservedLogs;
		TArray<FString> EvidenceLines;
		TArray<FString> AssetPackages;
		TArray<FString> ReferencerPackages;
		TArray<FString> DirectCurrentReferencers;
		TArray<FString> MissingClasses;
		TArray<FString> EvidenceProvenance;
		TArray<FString> ActiveConfigHits;
		FString HistoricalPluginName;
		FString HistoricalModuleName;
		FString ContentImpactClass;
		FString ContentImpactReason;
		FString Severity;
		FString CurrentState;
		FString RecommendationClass;
		FString RecommendationLane;
		FString RecommendationActionFamily;
		FString Confidence;
		FString CurrentStateReason;
		FString CurrentStateProofStrength;
		int32 OccurrenceCount = 0;
		TArray<FString> CurrentStateProofInputs;
		TArray<FString> CurrentStateBasis;
		TArray<FString> ContentImpactBasis;
		TArray<FString> ConfidenceBasis;
		TArray<FString> CurrentPresentLoggedPackages;
		TArray<FString> CurrentMissingLoggedPackages;
		TSharedPtr<FJsonObject> CurrentStateDetail;
	};

	TArray<FString> ExtractStringArrayField(const TSharedRef<FJsonObject>& Params, const FString& FieldName)
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* JsonValues = nullptr;
		if (!Params->TryGetArrayField(FieldName, JsonValues) || !JsonValues)
		{
			return Values;
		}

		for (const TSharedPtr<FJsonValue>& JsonValue : *JsonValues)
		{
			FString Value;
			if (JsonValue.IsValid() && JsonValue->TryGetString(Value))
			{
				Values.Add(Value);
			}
		}
		return Values;
	}

	void AddUniqueLimited(TArray<FString>& InOutValues, const FString& Value, const int32 MaxCount = INDEX_NONE)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || InOutValues.Contains(Trimmed))
		{
			return;
		}

		if (MaxCount != INDEX_NONE && InOutValues.Num() >= MaxCount)
		{
			return;
		}

		InOutValues.Add(Trimmed);
	}

	void SetJsonStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const TArray<FString>& Values)
	{
		if (!Object.IsValid())
		{
			return;
		}

		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	}

	FString NormalizeLogPath(const FString& InPath)
	{
		if (InPath.TrimStartAndEnd().IsEmpty())
		{
			return FString();
		}

		FString Candidate = InPath;
		if (FPaths::IsRelative(Candidate))
		{
			Candidate = FPaths::Combine(FPaths::ProjectDir(), Candidate);
		}

		Candidate = FPaths::ConvertRelativePathToFull(Candidate);
		FPaths::NormalizeFilename(Candidate);
		return Candidate;
	}

	bool TryExtractBetween(const FString& Source, const FString& Prefix, const FString& Suffix, FString& OutValue)
	{
		const int32 PrefixIndex = Source.Find(Prefix, ESearchCase::CaseSensitive, ESearchDir::FromStart);
		if (PrefixIndex == INDEX_NONE)
		{
			return false;
		}

		const int32 StartIndex = PrefixIndex + Prefix.Len();
		if (Suffix.IsEmpty())
		{
			OutValue = Source.Mid(StartIndex);
			return !OutValue.IsEmpty();
		}

		const int32 SuffixIndex = Source.Find(Suffix, ESearchCase::CaseSensitive, ESearchDir::FromStart, StartIndex);
		if (SuffixIndex == INDEX_NONE || SuffixIndex < StartIndex)
		{
			return false;
		}

		OutValue = Source.Mid(StartIndex, SuffixIndex - StartIndex);
		return !OutValue.IsEmpty();
	}

	bool ParseScriptPackageFailureLine(const FString& Line, FString& OutAssetFilename, FString& OutScriptPackage)
	{
		const FString Prefix = TEXT("[AssetLog] ");
		const FString Middle = TEXT(": VerifyImport: Failed to find script package for import object 'Package ");
		FString AssetPart;
		if (!TryExtractBetween(Line, Prefix, Middle, AssetPart))
		{
			return false;
		}

		FString ScriptPart;
		if (!TryExtractBetween(Line, Middle, TEXT("'"), ScriptPart))
		{
			return false;
		}

		OutAssetFilename = AssetPart;
		OutScriptPackage = ScriptPart;
		return true;
	}

	bool ParseMissingDependentPackageLine(const FString& Line, FString& OutReferencerPackage, FString& OutMissingPackage)
	{
		const FString Prefix = TEXT("While trying to load package ");
		const FString Middle = TEXT(", a dependent package ");
		FString ReferencerPart;
		if (!TryExtractBetween(Line, Prefix, Middle, ReferencerPart))
		{
			return false;
		}

		FString MissingPart;
		if (!TryExtractBetween(Line, Middle, TEXT(" was not available"), MissingPart))
		{
			return false;
		}

		OutReferencerPackage = ReferencerPart;
		OutMissingPackage = MissingPart;
		return true;
	}

	bool ParsePluginNotMountedLine(const FString& Line, FString& OutMissingPackage, FString& OutPluginName)
	{
		const FString Prefix = TEXT("Skipped package ");
		const FString Middle = TEXT(" was expected to be found in plugin ");
		FString MissingPart;
		if (!TryExtractBetween(Line, Prefix, Middle, MissingPart))
		{
			return false;
		}

		FString PluginPart;
		if (!TryExtractBetween(Line, Middle, TEXT(", however that plugin is not mounted."), PluginPart))
		{
			return false;
		}

		OutMissingPackage = MissingPart;
		OutPluginName = PluginPart;
		return true;
	}

	bool ParseMissingClassLine(const FString& Line, FString& OutAssetPackage, FString& OutMissingClass)
	{
		const FString Prefix = TEXT("with outer Package ");
		const FString Middle = TEXT(" because its class (");
		FString AssetPart;
		if (!TryExtractBetween(Line, Prefix, Middle, AssetPart))
		{
			return false;
		}

		FString ClassPart;
		if (!TryExtractBetween(Line, Middle, TEXT(") does not exist"), ClassPart))
		{
			return false;
		}

		OutAssetPackage = AssetPart;
		OutMissingClass = ClassPart;
		return true;
	}

	bool ParseGameNetDriverLine(const FString& Line, FString& OutDriverClassName)
	{
		return TryExtractBetween(Line, TEXT("DefName=\"GameNetDriver\",DriverClassName=\""), TEXT("\""), OutDriverClassName);
	}

	FString AssetFilenameToPackagePath(const FString& AssetFilename)
	{
		FString StandardFilename = AssetFilename;
		FPaths::NormalizeFilename(StandardFilename);

		FString LongPackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(StandardFilename, LongPackageName))
		{
			return LongPackageName;
		}

		FString ProjectContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
		FPaths::NormalizeFilename(ProjectContentDir);
		if (StandardFilename.StartsWith(ProjectContentDir))
		{
			FString Relative = StandardFilename.RightChop(ProjectContentDir.Len());
			Relative.RemoveFromStart(TEXT("/"));
			Relative = FPaths::ChangeExtension(Relative, TEXT(""));
			return FString::Printf(TEXT("/Game/%s"), *Relative);
		}

		return FString();
	}

	FString MakeFindingId(const FDependencyFinding& Finding)
	{
		FString SafeKey = Finding.FindingKey;
		SafeKey.ReplaceInline(TEXT("/"), TEXT("_"));
		SafeKey.ReplaceInline(TEXT(":"), TEXT("_"));
		SafeKey.ReplaceInline(TEXT("."), TEXT("_"));
		return FString::Printf(TEXT("%s_%s"), *Finding.FindingKind, *SafeKey);
	}

	FString DetermineRootSegment(const FString& PackagePath)
	{
		if (!PackagePath.StartsWith(TEXT("/")))
		{
			return FString();
		}

		FString Working = PackagePath.RightChop(1);
		FString RootSegment;
		if (Working.Split(TEXT("/"), &RootSegment, nullptr))
		{
			return RootSegment;
		}

		return Working;
	}

	bool IsUnderRoot(const FString& PackagePath, const FString& RootPath)
	{
		return !RootPath.IsEmpty() && (PackagePath.Equals(RootPath, ESearchCase::IgnoreCase) || PackagePath.StartsWith(RootPath + TEXT("/"), ESearchCase::IgnoreCase));
	}

	FDependencyConfigSnapshot LoadConfigSnapshot()
	{
		FDependencyConfigSnapshot Snapshot;

		TArray<FString> GameLines;
		const FString DefaultGamePath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini"));
		if (FFileHelper::LoadFileToStringArray(GameLines, *DefaultGamePath))
		{
			for (const FString& Line : GameLines)
			{
				FString Value;
				if (TryExtractBetween(Line, TEXT("MapsToCook=(FilePath=\""), TEXT("\")"), Value))
				{
					AddUniqueLimited(Snapshot.MapsToCook, Value);
				}
				else if (TryExtractBetween(Line, TEXT("DirectoriesToAlwaysCook=(Path=\""), TEXT("\")"), Value))
				{
					AddUniqueLimited(Snapshot.DirectoriesToAlwaysCook, Value);
				}
			}
		}

		TArray<FString> EngineLines;
		const FString DefaultEnginePath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini"));
		if (FFileHelper::LoadFileToStringArray(EngineLines, *DefaultEnginePath))
		{
			for (const FString& Line : EngineLines)
			{
				if (Snapshot.DefaultPlatformService.IsEmpty() && Line.StartsWith(TEXT("DefaultPlatformService=")))
				{
					Snapshot.DefaultPlatformService = Line.RightChop(FString(TEXT("DefaultPlatformService=")).Len()).TrimStartAndEnd();
				}

				if (Snapshot.GameNetDriver.IsEmpty())
				{
					FString DriverClassName;
					if (ParseGameNetDriverLine(Line, DriverClassName))
					{
						Snapshot.GameNetDriver = DriverClassName;
					}
				}
			}
		}

		return Snapshot;
	}

	void AppendDirectReferencers(const FString& PackagePath, TArray<FString>& InOutReferencers)
	{
		if (PackagePath.IsEmpty())
		{
			return;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FName> Referencers;
		AssetRegistry.GetReferencers(
			FName(*PackagePath),
			Referencers,
			UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::FDependencyQuery());

		for (const FName& ReferencerName : Referencers)
		{
			const FString ReferencerPath = ReferencerName.ToString();
			if (!ReferencerPath.StartsWith(TEXT("/Script/")) && !ReferencerPath.StartsWith(TEXT("/Engine/")))
			{
				AddUniqueLimited(InOutReferencers, ReferencerPath, MaxReferencerExamples);
			}
		}
	}

	bool GetCurrentAssetClassPaths(const FString& PackagePath, TArray<FString>& OutClassPaths, TArray<FString>& OutResolvedClassPaths, TArray<FString>& OutUnresolvedClassPaths)
	{
		if (PackagePath.IsEmpty())
		{
			return false;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> AssetDatas;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), AssetDatas, true);
		if (AssetDatas.Num() == 0)
		{
			return false;
		}

		for (const FAssetData& AssetData : AssetDatas)
		{
			if (!AssetData.IsValid())
			{
				continue;
			}

			const FString ClassPath = AssetData.AssetClassPath.ToString();
			AddUniqueLimited(OutClassPaths, ClassPath, MaxPackageExamples);

			UClass* ResolvedClass = !ClassPath.IsEmpty()
				? FindObject<UClass>(nullptr, *ClassPath)
				: nullptr;
			if (ResolvedClass)
			{
				AddUniqueLimited(OutResolvedClassPaths, ClassPath, MaxPackageExamples);
			}
			else
			{
				AddUniqueLimited(OutUnresolvedClassPaths, ClassPath, MaxPackageExamples);
			}
		}

		return true;
	}

	bool TryResolveModulePlugin(const FString& ModuleName, FString& OutPluginName, bool& bOutEnabled)
	{
		if (ModuleName.IsEmpty())
		{
			return false;
		}

		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
		{
			for (const FModuleDescriptor& ModuleDescriptor : Plugin->GetDescriptor().Modules)
			{
				if (ModuleDescriptor.Name.ToString().Equals(ModuleName, ESearchCase::IgnoreCase))
				{
					OutPluginName = Plugin->GetName();
					bOutEnabled = Plugin->IsEnabled();
					return true;
				}
			}
		}

		return false;
	}

	bool TryResolveRootPlugin(const FString& RootName, FString& OutPluginName, bool& bOutEnabled)
	{
		if (RootName.IsEmpty())
		{
			return false;
		}

		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
		{
			if (Plugin->GetName().Equals(RootName, ESearchCase::IgnoreCase))
			{
				OutPluginName = Plugin->GetName();
				bOutEnabled = Plugin->IsEnabled();
				return true;
			}
		}

		return false;
	}

	bool DoesPackageExistNow(const FString& PackagePath, FString& OutResolvedFilename)
	{
		if (PackagePath.IsEmpty())
		{
			return false;
		}

		FString CandidateFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, CandidateFilename, FPackageName::GetAssetPackageExtension()))
		{
			if (FPaths::FileExists(CandidateFilename))
			{
				OutResolvedFilename = CandidateFilename;
				return true;
			}
		}

		if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, CandidateFilename, FPackageName::GetMapPackageExtension()))
		{
			if (FPaths::FileExists(CandidateFilename))
			{
				OutResolvedFilename = CandidateFilename;
				return true;
			}
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> AssetDatas;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), AssetDatas, true);
		if (AssetDatas.Num() > 0)
		{
			return true;
		}

		return false;
	}

	TSharedPtr<FJsonObject> BuildCurrentStateSnapshotJson(const FDependencyConfigSnapshot& Snapshot)
	{
		TSharedPtr<FJsonObject> SnapshotObject = MakeShared<FJsonObject>();
		SnapshotObject->SetStringField(TEXT("default_platform_service"), Snapshot.DefaultPlatformService);
		SnapshotObject->SetStringField(TEXT("game_net_driver"), Snapshot.GameNetDriver);

		TArray<TSharedPtr<FJsonValue>> MapsJson;
		for (const FString& MapPath : Snapshot.MapsToCook)
		{
			MapsJson.Add(MakeShared<FJsonValueString>(MapPath));
		}
		SnapshotObject->SetArrayField(TEXT("maps_to_cook"), MapsJson);

		TArray<TSharedPtr<FJsonValue>> AlwaysCookJson;
		for (const FString& DirPath : Snapshot.DirectoriesToAlwaysCook)
		{
			AlwaysCookJson.Add(MakeShared<FJsonValueString>(DirPath));
		}
		SnapshotObject->SetArrayField(TEXT("directories_to_always_cook"), AlwaysCookJson);
		return SnapshotObject;
	}

	FString BuildFindingContentImpactClass(
		FDependencyFinding& Finding,
		const FDependencyConfigSnapshot& Snapshot,
		bool& bOutHasStrongActiveEvidence)
	{
		bOutHasStrongActiveEvidence = false;

		auto HasStrongConfigHit = [&Snapshot](const FString& PackagePath, TArray<FString>& InOutConfigHits) -> bool
		{
			for (const FString& MapPath : Snapshot.MapsToCook)
			{
				if (PackagePath.Equals(MapPath, ESearchCase::IgnoreCase))
				{
					AddUniqueLimited(InOutConfigHits, FString::Printf(TEXT("maps_to_cook:%s"), *MapPath), MaxPackageExamples);
					return true;
				}
			}

			for (const FString& AlwaysCookDir : Snapshot.DirectoriesToAlwaysCook)
			{
				if (IsUnderRoot(PackagePath, AlwaysCookDir))
				{
					AddUniqueLimited(InOutConfigHits, FString::Printf(TEXT("directories_to_always_cook:%s"), *AlwaysCookDir), MaxPackageExamples);
					return true;
				}
			}

			return false;
		};

		for (const FString& PackagePath : Finding.AssetPackages)
		{
			if (HasStrongConfigHit(PackagePath, Finding.ActiveConfigHits))
			{
				bOutHasStrongActiveEvidence = true;
			}
		}
		for (const FString& PackagePath : Finding.ReferencerPackages)
		{
			if (HasStrongConfigHit(PackagePath, Finding.ActiveConfigHits))
			{
				bOutHasStrongActiveEvidence = true;
			}
		}
		for (const FString& PackagePath : Finding.DirectCurrentReferencers)
		{
			if (HasStrongConfigHit(PackagePath, Finding.ActiveConfigHits))
			{
				bOutHasStrongActiveEvidence = true;
			}
		}

		if (bOutHasStrongActiveEvidence)
		{
			Finding.ContentImpactReason = TEXT("strong_active_evidence_from_current_packaging");
			for (const FString& ActiveConfigHit : Finding.ActiveConfigHits)
			{
				AddUniqueLimited(Finding.ContentImpactBasis, ActiveConfigHit, MaxPackageExamples);
			}
			AddUniqueLimited(
				Finding.ContentImpactBasis,
				FString::Printf(TEXT("current_direct_referencer_count:%d"), Finding.DirectCurrentReferencers.Num()),
				MaxPackageExamples);
			return TEXT("active_content_blocker");
		}

		if (Finding.DirectCurrentReferencers.Num() > 0)
		{
			Finding.ContentImpactReason = TEXT("current_referencers_without_strong_packaging_hit");
			AddUniqueLimited(
				Finding.ContentImpactBasis,
				FString::Printf(TEXT("current_direct_referencer_count:%d"), Finding.DirectCurrentReferencers.Num()),
				MaxPackageExamples);
			for (const FString& Referencer : Finding.DirectCurrentReferencers)
			{
				AddUniqueLimited(
					Finding.ContentImpactBasis,
					FString::Printf(TEXT("current_direct_referencer:%s"), *Referencer),
					MaxPackageExamples);
			}
			return TEXT("candidate_active");
		}

		if (Finding.CurrentState != TEXT("resolved_in_current_state")
			&& (Finding.AssetPackages.Num() > 0 || Finding.ReferencerPackages.Num() > 0)
			&& Finding.CurrentPresentLoggedPackages.Num() == 0
			&& Finding.CurrentMissingLoggedPackages.Num() > 0)
		{
			Finding.ContentImpactReason = TEXT("historical_logged_packages_missing_in_current_content_state");
			AddUniqueLimited(
				Finding.ContentImpactBasis,
				FString::Printf(TEXT("present_logged_package_count:%d"), Finding.CurrentPresentLoggedPackages.Num()),
				MaxPackageExamples);
			AddUniqueLimited(
				Finding.ContentImpactBasis,
				FString::Printf(TEXT("missing_logged_package_count:%d"), Finding.CurrentMissingLoggedPackages.Num()),
				MaxPackageExamples);
			for (const FString& MissingPackage : Finding.CurrentMissingLoggedPackages)
			{
				AddUniqueLimited(
					Finding.ContentImpactBasis,
					FString::Printf(TEXT("missing_logged_package:%s"), *MissingPackage),
					MaxPackageExamples);
			}
			return TEXT("likely_legacy_or_resolved");
		}

		if (Finding.CurrentState == TEXT("resolved_in_current_state"))
		{
			Finding.ContentImpactReason = TEXT("resolved_without_current_active_hits");
			AddUniqueLimited(
				Finding.ContentImpactBasis,
				FString::Printf(TEXT("logged_asset_package_count:%d"), Finding.AssetPackages.Num()),
				MaxPackageExamples);
			AddUniqueLimited(
				Finding.ContentImpactBasis,
				FString::Printf(TEXT("logged_referencer_package_count:%d"), Finding.ReferencerPackages.Num()),
				MaxPackageExamples);
			return TEXT("likely_legacy_or_resolved");
		}

		if (Finding.AssetPackages.Num() == 0 && Finding.ReferencerPackages.Num() == 0)
		{
			Finding.ContentImpactReason = TEXT("insufficient_current_asset_context");
			AddUniqueLimited(Finding.ContentImpactBasis, TEXT("no_logged_asset_or_referencer_packages"), MaxPackageExamples);
			return TEXT("uncertain");
		}

		Finding.ContentImpactReason = TEXT("logged_assets_without_strong_current_activity");
		AddUniqueLimited(
			Finding.ContentImpactBasis,
			FString::Printf(TEXT("logged_asset_package_count:%d"), Finding.AssetPackages.Num()),
			MaxPackageExamples);
		AddUniqueLimited(
			Finding.ContentImpactBasis,
			FString::Printf(TEXT("logged_referencer_package_count:%d"), Finding.ReferencerPackages.Num()),
			MaxPackageExamples);
		return TEXT("candidate_active");
	}

	void EnrichFindingCurrentState(FDependencyFinding& Finding, const FDependencyConfigSnapshot& Snapshot)
	{
		Finding.CurrentStateDetail = MakeShared<FJsonObject>();

		if (Finding.FindingKind == TEXT("online_subsystem_failure"))
		{
			AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("engine_config_snapshot"), MaxPackageExamples);
			Finding.CurrentStateDetail->SetStringField(TEXT("default_platform_service"), Snapshot.DefaultPlatformService);
			Finding.CurrentStateDetail->SetStringField(TEXT("game_net_driver"), Snapshot.GameNetDriver);
			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("default_platform_service=%s"), *Snapshot.DefaultPlatformService),
				MaxPackageExamples);
			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("game_net_driver=%s"), *Snapshot.GameNetDriver),
				MaxPackageExamples);

			if (!Snapshot.DefaultPlatformService.Equals(TEXT("Steam"), ESearchCase::IgnoreCase))
			{
				Finding.CurrentState = TEXT("resolved_in_current_state");
				Finding.CurrentStateReason = TEXT("Current engine config no longer uses Steam as the active default platform service.");
			}
			else
			{
				Finding.CurrentState = TEXT("unresolved");
				Finding.CurrentStateReason = TEXT("Current engine config still points at Steam as the active default platform service.");
			}
		}
		else if (Finding.FindingKey.StartsWith(TEXT("/Script/")))
		{
			const FString ModuleName = Finding.FindingKey.RightChop(FString(TEXT("/Script/")).Len());
			Finding.HistoricalModuleName = ModuleName;

			AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("module_load_state"), MaxPackageExamples);
			const bool bModuleLoaded = FModuleManager::Get().IsModuleLoaded(*ModuleName);
			Finding.CurrentStateDetail->SetStringField(TEXT("module_name"), ModuleName);
			Finding.CurrentStateDetail->SetBoolField(TEXT("module_loaded"), bModuleLoaded);
			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("module_name=%s"), *ModuleName),
				MaxPackageExamples);
			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("module_loaded=%s"), bModuleLoaded ? TEXT("true") : TEXT("false")),
				MaxPackageExamples);

			FString PluginName;
			bool bPluginEnabled = false;
			if (TryResolveModulePlugin(ModuleName, PluginName, bPluginEnabled))
			{
				AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("plugin_enablement_state"), MaxPackageExamples);
				Finding.CurrentStateDetail->SetStringField(TEXT("plugin_name"), PluginName);
				Finding.CurrentStateDetail->SetBoolField(TEXT("plugin_enabled"), bPluginEnabled);
				AddUniqueLimited(
					Finding.CurrentStateBasis,
					FString::Printf(TEXT("plugin_name=%s"), *PluginName),
					MaxPackageExamples);
				AddUniqueLimited(
					Finding.CurrentStateBasis,
					FString::Printf(TEXT("plugin_enabled=%s"), bPluginEnabled ? TEXT("true") : TEXT("false")),
					MaxPackageExamples);
			}

			if (bModuleLoaded)
			{
				Finding.CurrentState = TEXT("resolved_in_current_state");
				Finding.CurrentStateReason = TEXT("The referenced script package module is currently loaded.");
			}
			else
			{
				Finding.CurrentState = TEXT("unresolved");
				Finding.CurrentStateReason = TEXT("The referenced script package module is not currently loaded.");
			}
		}
		else if (Finding.FindingKind == TEXT("missing_class_import"))
		{
			AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("asset_registry_class_resolution"), MaxPackageExamples);
			AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("loaded_class_resolution"), MaxPackageExamples);
			TArray<FString> ResolvedAssetPackages;
			TArray<FString> UnresolvedAssetPackages;
			TArray<FString> AssetClassPaths;
			TArray<FString> ResolvedAssetClassPaths;
			TArray<FString> UnresolvedAssetClassPaths;

			for (const FString& AssetPackage : Finding.AssetPackages)
			{
				TArray<FString> PackageClassPaths;
				TArray<FString> PackageResolvedClassPaths;
				TArray<FString> PackageUnresolvedClassPaths;
				const bool bHasAssetData = GetCurrentAssetClassPaths(AssetPackage, PackageClassPaths, PackageResolvedClassPaths, PackageUnresolvedClassPaths);
				for (const FString& ClassPath : PackageClassPaths)
				{
					AddUniqueLimited(AssetClassPaths, ClassPath, MaxPackageExamples);
				}
				for (const FString& ClassPath : PackageResolvedClassPaths)
				{
					AddUniqueLimited(ResolvedAssetClassPaths, ClassPath, MaxPackageExamples);
				}
				for (const FString& ClassPath : PackageUnresolvedClassPaths)
				{
					AddUniqueLimited(UnresolvedAssetClassPaths, ClassPath, MaxPackageExamples);
				}

				if (bHasAssetData && PackageResolvedClassPaths.Num() > 0)
				{
					AddUniqueLimited(ResolvedAssetPackages, AssetPackage, MaxPackageExamples);
				}
				else
				{
					AddUniqueLimited(UnresolvedAssetPackages, AssetPackage, MaxPackageExamples);
				}
			}

			Finding.CurrentStateDetail->SetNumberField(TEXT("resolved_asset_package_count"), ResolvedAssetPackages.Num());
			Finding.CurrentStateDetail->SetNumberField(TEXT("unresolved_asset_package_count"), UnresolvedAssetPackages.Num());
			SetJsonStringArrayField(Finding.CurrentStateDetail, TEXT("resolved_asset_packages"), ResolvedAssetPackages);
			SetJsonStringArrayField(Finding.CurrentStateDetail, TEXT("unresolved_asset_packages"), UnresolvedAssetPackages);
			SetJsonStringArrayField(Finding.CurrentStateDetail, TEXT("asset_class_paths"), AssetClassPaths);
			SetJsonStringArrayField(Finding.CurrentStateDetail, TEXT("resolved_asset_class_paths"), ResolvedAssetClassPaths);
			SetJsonStringArrayField(Finding.CurrentStateDetail, TEXT("unresolved_asset_class_paths"), UnresolvedAssetClassPaths);
			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("resolved_asset_package_count=%d"), ResolvedAssetPackages.Num()),
				MaxPackageExamples);
			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("unresolved_asset_package_count=%d"), UnresolvedAssetPackages.Num()),
				MaxPackageExamples);
			for (const FString& ClassPath : ResolvedAssetClassPaths)
			{
				AddUniqueLimited(
					Finding.CurrentStateBasis,
					FString::Printf(TEXT("resolved_asset_class_path=%s"), *ClassPath),
					MaxPackageExamples);
			}
			for (const FString& ClassPath : UnresolvedAssetClassPaths)
			{
				AddUniqueLimited(
					Finding.CurrentStateBasis,
					FString::Printf(TEXT("unresolved_asset_class_path=%s"), *ClassPath),
					MaxPackageExamples);
			}

			if (ResolvedAssetPackages.Num() > 0 && UnresolvedAssetPackages.Num() == 0)
			{
				Finding.CurrentState = TEXT("resolved_in_current_state");
				Finding.CurrentStateReason = TEXT("The affected asset packages now resolve through the asset registry with currently loaded asset classes.");
			}
			else if (ResolvedAssetPackages.Num() > 0)
			{
				Finding.CurrentState = TEXT("unresolved");
				Finding.CurrentStateReason = TEXT("Only part of the affected asset set now resolves with currently loaded asset classes.");
			}
			else
			{
				Finding.CurrentState = TEXT("unresolved");
				Finding.CurrentStateReason = TEXT("The affected asset packages still do not resolve with currently loaded asset classes.");
			}
		}
		else
		{
			const FString RootSegment = DetermineRootSegment(Finding.FindingKey);
			FString PluginName;
			bool bPluginEnabled = false;
			AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("package_path_resolution"), MaxPackageExamples);
			if (TryResolveRootPlugin(RootSegment, PluginName, bPluginEnabled))
			{
				AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("plugin_enablement_state"), MaxPackageExamples);
				Finding.CurrentStateDetail->SetStringField(TEXT("plugin_name"), PluginName);
				Finding.CurrentStateDetail->SetBoolField(TEXT("plugin_enabled"), bPluginEnabled);
				AddUniqueLimited(
					Finding.CurrentStateBasis,
					FString::Printf(TEXT("plugin_name=%s"), *PluginName),
					MaxPackageExamples);
				AddUniqueLimited(
					Finding.CurrentStateBasis,
					FString::Printf(TEXT("plugin_enabled=%s"), bPluginEnabled ? TEXT("true") : TEXT("false")),
					MaxPackageExamples);
			}

			FString ResolvedFilename;
			const bool bPackageExists = DoesPackageExistNow(Finding.FindingKey, ResolvedFilename);
			Finding.CurrentStateDetail->SetStringField(TEXT("package_root"), RootSegment);
			Finding.CurrentStateDetail->SetBoolField(TEXT("package_exists"), bPackageExists);
			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("package_root=%s"), *RootSegment),
				MaxPackageExamples);
			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("package_exists=%s"), bPackageExists ? TEXT("true") : TEXT("false")),
				MaxPackageExamples);
			if (!ResolvedFilename.IsEmpty())
			{
				Finding.CurrentStateDetail->SetStringField(TEXT("resolved_filename"), ResolvedFilename);
				AddUniqueLimited(
					Finding.CurrentStateBasis,
					FString::Printf(TEXT("resolved_filename=%s"), *ResolvedFilename),
					MaxPackageExamples);
			}

			if (bPackageExists || bPluginEnabled)
			{
				Finding.CurrentState = TEXT("resolved_in_current_state");
				Finding.CurrentStateReason = TEXT("The missing package root now resolves in the current project state.");
			}
			else
			{
				Finding.CurrentState = TEXT("unresolved");
				Finding.CurrentStateReason = TEXT("The missing package root still does not resolve in the current project state.");
			}
		}

		if (Finding.AssetPackages.Num() > 0 || Finding.ReferencerPackages.Num() > 0)
		{
			AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("logged_package_presence"), MaxPackageExamples);

			for (const FString& AssetPackage : Finding.AssetPackages)
			{
				FString ResolvedFilename;
				if (DoesPackageExistNow(AssetPackage, ResolvedFilename))
				{
					AddUniqueLimited(Finding.CurrentPresentLoggedPackages, AssetPackage, MaxPackageExamples);
				}
				else
				{
					AddUniqueLimited(Finding.CurrentMissingLoggedPackages, AssetPackage, MaxPackageExamples);
				}
			}

			for (const FString& ReferencerPackage : Finding.ReferencerPackages)
			{
				FString ResolvedFilename;
				if (DoesPackageExistNow(ReferencerPackage, ResolvedFilename))
				{
					AddUniqueLimited(Finding.CurrentPresentLoggedPackages, ReferencerPackage, MaxPackageExamples);
				}
				else
				{
					AddUniqueLimited(Finding.CurrentMissingLoggedPackages, ReferencerPackage, MaxPackageExamples);
				}
			}

			Finding.CurrentStateDetail->SetNumberField(TEXT("present_logged_package_count"), Finding.CurrentPresentLoggedPackages.Num());
			Finding.CurrentStateDetail->SetNumberField(TEXT("missing_logged_package_count"), Finding.CurrentMissingLoggedPackages.Num());
			SetJsonStringArrayField(Finding.CurrentStateDetail, TEXT("present_logged_packages"), Finding.CurrentPresentLoggedPackages);
			SetJsonStringArrayField(Finding.CurrentStateDetail, TEXT("missing_logged_packages"), Finding.CurrentMissingLoggedPackages);

			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("present_logged_package_count=%d"), Finding.CurrentPresentLoggedPackages.Num()),
				MaxPackageExamples);
			AddUniqueLimited(
				Finding.CurrentStateBasis,
				FString::Printf(TEXT("missing_logged_package_count=%d"), Finding.CurrentMissingLoggedPackages.Num()),
				MaxPackageExamples);
			for (const FString& PresentPackage : Finding.CurrentPresentLoggedPackages)
			{
				AddUniqueLimited(
					Finding.CurrentStateBasis,
					FString::Printf(TEXT("present_logged_package=%s"), *PresentPackage),
					MaxPackageExamples);
			}
			for (const FString& MissingPackage : Finding.CurrentMissingLoggedPackages)
			{
				AddUniqueLimited(
					Finding.CurrentStateBasis,
					FString::Printf(TEXT("missing_logged_package=%s"), *MissingPackage),
					MaxPackageExamples);
			}
		}

		bool bHasStrongActiveEvidence = false;
		Finding.ContentImpactClass = BuildFindingContentImpactClass(Finding, Snapshot, bHasStrongActiveEvidence);
		if (Finding.ActiveConfigHits.Num() > 0)
		{
			AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("project_packaging_config"), MaxPackageExamples);
		}
		if (Finding.DirectCurrentReferencers.Num() > 0)
		{
			AddUniqueLimited(Finding.CurrentStateProofInputs, TEXT("asset_registry_referencers"), MaxPackageExamples);
		}

		if (Finding.CurrentState == TEXT("resolved_in_current_state"))
		{
			Finding.Severity = TEXT("info");
			Finding.RecommendationClass = TEXT("resolved_no_action");
			Finding.RecommendationLane = TEXT("no_action");
			Finding.RecommendationActionFamily = TEXT("historical_resolution");
			Finding.Confidence = TEXT("high");
		}
		else if (Finding.ContentImpactClass == TEXT("active_content_blocker"))
		{
			Finding.Severity = TEXT("blocker");
			Finding.RecommendationClass = TEXT("repair_candidate");
			Finding.RecommendationLane = TEXT("repair_now");
			Finding.RecommendationActionFamily = TEXT("dependency_repair");
			Finding.Confidence = TEXT("high");
		}
		else if (Finding.ContentImpactClass == TEXT("candidate_active"))
		{
			Finding.Severity = TEXT("warning");
			Finding.RecommendationClass = TEXT("needs_runtime_proof");
			Finding.RecommendationLane = TEXT("runtime_proof_next");
			Finding.RecommendationActionFamily = TEXT("runtime_validation");
			Finding.Confidence = TEXT("medium");
		}
		else if (Finding.ContentImpactClass == TEXT("likely_legacy_or_resolved"))
		{
			Finding.Severity = TEXT("info");
			Finding.RecommendationClass = TEXT("demote_or_defer_candidate");
			Finding.RecommendationLane = TEXT("demote_or_defer");
			Finding.RecommendationActionFamily = TEXT("scope_review");
			Finding.Confidence = TEXT("medium");
		}
		else
		{
			Finding.Severity = TEXT("warning");
			Finding.RecommendationClass = TEXT("needs_runtime_proof");
			Finding.RecommendationLane = TEXT("runtime_proof_next");
			Finding.RecommendationActionFamily = TEXT("runtime_validation");
			Finding.Confidence = TEXT("low");
		}

		if (Finding.CurrentStateProofInputs.Contains(TEXT("asset_registry_class_resolution"))
			|| Finding.CurrentStateProofInputs.Contains(TEXT("module_load_state"))
			|| Finding.CurrentStateProofInputs.Contains(TEXT("engine_config_snapshot"))
			|| Finding.CurrentStateProofInputs.Contains(TEXT("package_path_resolution")))
		{
			Finding.CurrentStateProofStrength = TEXT("strong");
		}
		else if (Finding.CurrentStateProofInputs.Num() > 0)
		{
			Finding.CurrentStateProofStrength = TEXT("moderate");
		}
		else
		{
			Finding.CurrentStateProofStrength = TEXT("limited");
		}

		AddUniqueLimited(
			Finding.ConfidenceBasis,
			FString::Printf(TEXT("confidence_level:%s"), *Finding.Confidence),
			MaxPackageExamples);
		AddUniqueLimited(
			Finding.ConfidenceBasis,
			FString::Printf(TEXT("current_state_proof_strength:%s"), *Finding.CurrentStateProofStrength),
			MaxPackageExamples);
		AddUniqueLimited(
			Finding.ConfidenceBasis,
			FString::Printf(TEXT("content_impact_reason:%s"), *Finding.ContentImpactReason),
			MaxPackageExamples);
		if (Finding.CurrentState == TEXT("resolved_in_current_state"))
		{
			AddUniqueLimited(Finding.ConfidenceBasis, TEXT("resolved_in_current_state"), MaxPackageExamples);
		}
		else if (Finding.ContentImpactClass == TEXT("active_content_blocker"))
		{
			AddUniqueLimited(
				Finding.ConfidenceBasis,
				FString::Printf(TEXT("strong_active_config_hit_count:%d"), Finding.ActiveConfigHits.Num()),
				MaxPackageExamples);
		}
		else if (Finding.DirectCurrentReferencers.Num() > 0)
		{
			AddUniqueLimited(
				Finding.ConfidenceBasis,
				FString::Printf(TEXT("current_direct_referencer_count:%d"), Finding.DirectCurrentReferencers.Num()),
				MaxPackageExamples);
		}
		else
		{
			AddUniqueLimited(Finding.ConfidenceBasis, TEXT("no_strong_current_packaging_hit"), MaxPackageExamples);
		}
	}

	TArray<FString> BuildRecommendationTargetExamples(const FDependencyFinding& Finding)
	{
		TArray<FString> Targets;
		for (const FString& Referencer : Finding.DirectCurrentReferencers)
		{
			AddUniqueLimited(Targets, Referencer, MaxPackageExamples);
		}
		for (const FString& AssetPackage : Finding.AssetPackages)
		{
			AddUniqueLimited(Targets, AssetPackage, MaxPackageExamples);
		}
		for (const FString& ReferencerPackage : Finding.ReferencerPackages)
		{
			AddUniqueLimited(Targets, ReferencerPackage, MaxPackageExamples);
		}
		for (const FString& MissingClass : Finding.MissingClasses)
		{
			AddUniqueLimited(Targets, MissingClass, MaxPackageExamples);
		}
		if (Targets.Num() == 0)
		{
			AddUniqueLimited(Targets, Finding.FindingKey, MaxPackageExamples);
		}
		return Targets;
	}

	FString BuildRecommendationRationale(const FDependencyFinding& Finding)
	{
		if (Finding.RecommendationLane == TEXT("repair_now"))
		{
			return FString::Printf(
				TEXT("Current-state proof is %s and the finding still has strong active-content evidence, so this unresolved dependency is a repair-now candidate."),
				*Finding.CurrentStateProofStrength);
		}
		if (Finding.RecommendationLane == TEXT("runtime_proof_next"))
		{
			return TEXT("Current-state proof shows the dependency is still unresolved, but active impact is only a candidate signal, so a focused runtime proof is the next bounded step.");
		}
		if (Finding.RecommendationLane == TEXT("demote_or_defer"))
		{
			return TEXT("The finding remains lower-confidence without strong current active-content evidence, so scope review or deferment is safer than immediate repair.");
		}
		return TEXT("Current-state proof says the finding is already resolved now, so it should stay as historical evidence unless a fresh log reproduces it.");
	}

	TSharedPtr<FJsonObject> BuildCurrentStateProofJson(const FDependencyFinding& Finding)
	{
		TSharedPtr<FJsonObject> ProofObject = MakeShared<FJsonObject>();
		ProofObject->SetStringField(TEXT("proof_strength"), Finding.CurrentStateProofStrength);
		ProofObject->SetStringField(TEXT("proof_scope"), TEXT("current_state_only"));
		ProofObject->SetStringField(TEXT("content_impact_reason"), Finding.ContentImpactReason);
		SetJsonStringArrayField(ProofObject, TEXT("proof_inputs_used"), Finding.CurrentStateProofInputs);
		SetJsonStringArrayField(ProofObject, TEXT("current_state_basis"), Finding.CurrentStateBasis);
		SetJsonStringArrayField(ProofObject, TEXT("content_impact_basis"), Finding.ContentImpactBasis);
		ProofObject->SetBoolField(TEXT("runtime_impact_proven"), false);
		return ProofObject;
	}

	TSharedPtr<FJsonObject> BuildRecommendationDetailJson(const FDependencyFinding& Finding)
	{
		TSharedPtr<FJsonObject> DetailObject = MakeShared<FJsonObject>();
		DetailObject->SetStringField(TEXT("lane"), Finding.RecommendationLane);
		DetailObject->SetStringField(TEXT("action_family"), Finding.RecommendationActionFamily);
		DetailObject->SetStringField(TEXT("rationale"), BuildRecommendationRationale(Finding));
		SetJsonStringArrayField(DetailObject, TEXT("confidence_basis"), Finding.ConfidenceBasis);
		SetJsonStringArrayField(DetailObject, TEXT("current_state_proof_inputs_used"), Finding.CurrentStateProofInputs);

		TSharedPtr<FJsonObject> NextStepObject = MakeShared<FJsonObject>();
		if (Finding.RecommendationLane == TEXT("repair_now"))
		{
			NextStepObject->SetStringField(TEXT("step_class"), TEXT("dependency_repair_review"));
			NextStepObject->SetStringField(TEXT("summary"), TEXT("Review the owning plugin/module/package for the unresolved active dependency, then rerun dependency_health before any broader runtime pass."));
		}
		else if (Finding.RecommendationLane == TEXT("runtime_proof_next"))
		{
			NextStepObject->SetStringField(TEXT("step_class"), TEXT("runtime_validation_probe"));
			NextStepObject->SetStringField(TEXT("summary"), TEXT("Run a focused load or runtime proof on the current referencers before deciding between repair and deferment."));
		}
		else if (Finding.RecommendationLane == TEXT("demote_or_defer"))
		{
			NextStepObject->SetStringField(TEXT("step_class"), TEXT("scope_review"));
			NextStepObject->SetStringField(TEXT("summary"), TEXT("Confirm the finding is outside the active ship/runtime scope before scheduling repair work."));
		}
		else
		{
			NextStepObject->SetStringField(TEXT("step_class"), TEXT("no_action_recheck_on_regression"));
			NextStepObject->SetStringField(TEXT("summary"), TEXT("Keep this as resolved historical evidence and only revisit it if a fresh log reproduces the same dependency failure."));
		}
		SetJsonStringArrayField(NextStepObject, TEXT("target_examples"), BuildRecommendationTargetExamples(Finding));
		NextStepObject->SetBoolField(TEXT("auto_fix_supported"), false);
		NextStepObject->SetBoolField(TEXT("requires_runtime_evidence"), Finding.RecommendationLane == TEXT("runtime_proof_next"));
		DetailObject->SetObjectField(TEXT("next_step"), NextStepObject);
		return DetailObject;
	}

	TSharedPtr<FJsonObject> FindingToJson(const FDependencyFinding& Finding, const bool bIncludeLogExcerpt)
	{
		TSharedPtr<FJsonObject> FindingObject = MakeShared<FJsonObject>();
		FindingObject->SetStringField(TEXT("finding_id"), MakeFindingId(Finding));
		FindingObject->SetStringField(TEXT("finding_kind"), Finding.FindingKind);
		FindingObject->SetStringField(TEXT("dependency_key"), Finding.FindingKey);
		FindingObject->SetStringField(TEXT("subject"), Finding.Subject);
		FindingObject->SetStringField(TEXT("content_impact_class"), Finding.ContentImpactClass);
		FindingObject->SetStringField(TEXT("content_impact_reason"), Finding.ContentImpactReason);
		FindingObject->SetStringField(TEXT("severity"), Finding.Severity);
		FindingObject->SetStringField(TEXT("current_state"), Finding.CurrentState);
		FindingObject->SetStringField(TEXT("recommendation_class"), Finding.RecommendationClass);
		FindingObject->SetStringField(TEXT("recommendation_lane"), Finding.RecommendationLane);
		FindingObject->SetStringField(TEXT("recommendation_action_family"), Finding.RecommendationActionFamily);
		FindingObject->SetStringField(TEXT("confidence"), Finding.Confidence);
		FindingObject->SetStringField(TEXT("current_state_reason"), Finding.CurrentStateReason);
		FindingObject->SetStringField(TEXT("current_state_proof_strength"), Finding.CurrentStateProofStrength);
		FindingObject->SetNumberField(TEXT("occurrence_count"), Finding.OccurrenceCount);

		SetJsonStringArrayField(FindingObject, TEXT("observed_logs"), Finding.ObservedLogs);
		SetJsonStringArrayField(FindingObject, TEXT("asset_packages"), Finding.AssetPackages);
		SetJsonStringArrayField(FindingObject, TEXT("referencer_packages"), Finding.ReferencerPackages);
		SetJsonStringArrayField(FindingObject, TEXT("direct_current_referencers"), Finding.DirectCurrentReferencers);
		SetJsonStringArrayField(FindingObject, TEXT("missing_classes"), Finding.MissingClasses);
		SetJsonStringArrayField(FindingObject, TEXT("evidence_provenance"), Finding.EvidenceProvenance);
		SetJsonStringArrayField(FindingObject, TEXT("active_config_hits"), Finding.ActiveConfigHits);
		SetJsonStringArrayField(FindingObject, TEXT("current_state_proof_inputs_used"), Finding.CurrentStateProofInputs);
		SetJsonStringArrayField(FindingObject, TEXT("current_state_basis"), Finding.CurrentStateBasis);
		SetJsonStringArrayField(FindingObject, TEXT("content_impact_basis"), Finding.ContentImpactBasis);
		SetJsonStringArrayField(FindingObject, TEXT("confidence_basis"), Finding.ConfidenceBasis);

		if (bIncludeLogExcerpt)
		{
			SetJsonStringArrayField(FindingObject, TEXT("log_excerpt"), Finding.EvidenceLines);
		}

		if (Finding.CurrentStateDetail.IsValid())
		{
			FindingObject->SetObjectField(TEXT("current_state_detail"), Finding.CurrentStateDetail);
		}
		FindingObject->SetObjectField(TEXT("current_state_proof"), BuildCurrentStateProofJson(Finding));
		FindingObject->SetObjectField(TEXT("recommendation_detail"), BuildRecommendationDetailJson(Finding));

		return FindingObject;
	}

	FString JoinShortList(const TArray<FString>& Values)
	{
		return Values.Num() == 0 ? TEXT("none") : FString::Join(Values, TEXT(", "));
	}

	FString BuildDependencyReportMarkdown(
		const TArray<FString>& SourceLogPaths,
		const FDependencyConfigSnapshot& Snapshot,
		const TArray<TSharedPtr<FDependencyFinding>>& Findings)
	{
		FString Markdown;
		Markdown += TEXT("# Dependency Health Classification\n\n");
		Markdown += TEXT("## Inputs\n\n");
		for (const FString& SourceLogPath : SourceLogPaths)
		{
			Markdown += FString::Printf(TEXT("- `%s`\n"), *SourceLogPath);
		}

		int32 BlockerCount = 0;
		int32 WarningCount = 0;
		int32 InfoCount = 0;
		for (const TSharedPtr<FDependencyFinding>& Finding : Findings)
		{
			if (!Finding.IsValid())
			{
				continue;
			}
			if (Finding->Severity == TEXT("blocker"))
			{
				++BlockerCount;
			}
			else if (Finding->Severity == TEXT("warning"))
			{
				++WarningCount;
			}
			else
			{
				++InfoCount;
			}
		}

		Markdown += TEXT("\n## Summary\n\n");
		Markdown += FString::Printf(TEXT("- Findings: `%d`\n"), Findings.Num());
		Markdown += FString::Printf(TEXT("- Blockers: `%d`\n"), BlockerCount);
		Markdown += FString::Printf(TEXT("- Warnings: `%d`\n"), WarningCount);
		Markdown += FString::Printf(TEXT("- Info: `%d`\n"), InfoCount);
		Markdown += TEXT("- This slice is log-first classification with current-state enrichment. It does not auto-repair dependencies.\n");

		Markdown += TEXT("\n## Current State Snapshot\n\n");
		Markdown += FString::Printf(TEXT("- DefaultPlatformService: `%s`\n"), *Snapshot.DefaultPlatformService);
		Markdown += FString::Printf(TEXT("- GameNetDriver: `%s`\n"), *Snapshot.GameNetDriver);
		Markdown += FString::Printf(TEXT("- MapsToCook: `%s`\n"), *JoinShortList(Snapshot.MapsToCook));
		Markdown += FString::Printf(TEXT("- DirectoriesToAlwaysCook: `%s`\n"), *JoinShortList(Snapshot.DirectoriesToAlwaysCook));

		Markdown += TEXT("\n## Findings\n\n");
		int32 Index = 1;
		for (const TSharedPtr<FDependencyFinding>& Finding : Findings)
		{
			if (!Finding.IsValid())
			{
				continue;
			}

			Markdown += FString::Printf(TEXT("### %d. `%s`\n\n"), Index++, *Finding->FindingKey);
			Markdown += FString::Printf(TEXT("- Kind: `%s`\n"), *Finding->FindingKind);
			Markdown += FString::Printf(TEXT("- Severity: `%s`\n"), *Finding->Severity);
			Markdown += FString::Printf(TEXT("- Content impact: `%s`\n"), *Finding->ContentImpactClass);
			Markdown += FString::Printf(TEXT("- Content impact reason: `%s`\n"), *Finding->ContentImpactReason);
			Markdown += FString::Printf(TEXT("- Current state: `%s`\n"), *Finding->CurrentState);
			Markdown += FString::Printf(TEXT("- Current-state proof strength: `%s`\n"), *Finding->CurrentStateProofStrength);
			Markdown += FString::Printf(TEXT("- Recommendation: `%s`\n"), *Finding->RecommendationClass);
			Markdown += FString::Printf(TEXT("- Recommendation lane: `%s`\n"), *Finding->RecommendationLane);
			Markdown += FString::Printf(TEXT("- Action family: `%s`\n"), *Finding->RecommendationActionFamily);
			Markdown += FString::Printf(TEXT("- Confidence: `%s`\n"), *Finding->Confidence);
			Markdown += FString::Printf(TEXT("- Reason: %s\n"), *Finding->CurrentStateReason);
			Markdown += FString::Printf(TEXT("- Recommendation rationale: %s\n"), *BuildRecommendationRationale(*Finding));
			if (Finding->AssetPackages.Num() > 0)
			{
				Markdown += FString::Printf(TEXT("- Asset packages: `%s`\n"), *JoinShortList(Finding->AssetPackages));
			}
			if (Finding->ReferencerPackages.Num() > 0)
			{
				Markdown += FString::Printf(TEXT("- Referencer packages: `%s`\n"), *JoinShortList(Finding->ReferencerPackages));
			}
			if (Finding->DirectCurrentReferencers.Num() > 0)
			{
				Markdown += FString::Printf(TEXT("- Current referencers: `%s`\n"), *JoinShortList(Finding->DirectCurrentReferencers));
			}
			if (Finding->ActiveConfigHits.Num() > 0)
			{
				Markdown += FString::Printf(TEXT("- Active config hits: `%s`\n"), *JoinShortList(Finding->ActiveConfigHits));
			}
			if (Finding->CurrentStateProofInputs.Num() > 0)
			{
				Markdown += FString::Printf(TEXT("- Current-state proof inputs: `%s`\n"), *JoinShortList(Finding->CurrentStateProofInputs));
			}
			if (Finding->CurrentStateBasis.Num() > 0)
			{
				Markdown += FString::Printf(TEXT("- Current-state basis: `%s`\n"), *JoinShortList(Finding->CurrentStateBasis));
			}
			if (Finding->ContentImpactBasis.Num() > 0)
			{
				Markdown += FString::Printf(TEXT("- Content-impact basis: `%s`\n"), *JoinShortList(Finding->ContentImpactBasis));
			}
			if (Finding->ConfidenceBasis.Num() > 0)
			{
				Markdown += FString::Printf(TEXT("- Confidence basis: `%s`\n"), *JoinShortList(Finding->ConfidenceBasis));
			}
			{
				const TArray<FString> Targets = BuildRecommendationTargetExamples(*Finding);
				Markdown += FString::Printf(TEXT("- Next step targets: `%s`\n"), *JoinShortList(Targets));
			}
			for (const FString& EvidenceLine : Finding->EvidenceLines)
			{
				Markdown += FString::Printf(TEXT("- Evidence: `%s`\n"), *EvidenceLine);
			}
			Markdown += TEXT("\n");
		}

		Markdown += TEXT("## Limits\n\n");
		Markdown += TEXT("- This slice classifies dependency health from logs plus current-state enrichment. It does not prove full dependency semantics from logs alone.\n");
		Markdown += TEXT("- Active-vs-legacy classification is strongest when findings hit current cook roots, current direct referencers, or logged-package presence checks; otherwise the tool prefers cautious buckets such as `candidate_active` or `uncertain`.\n");
		Markdown += TEXT("- Runtime impact is not proven here; unresolved findings outside strong cook roots may still need runtime proof before repair or demotion decisions.\n");
		return Markdown;
	}

	FOsvayderUEReportTruthSummary BuildTruthSummary(const TArray<TSharedPtr<FDependencyFinding>>& Findings)
	{
		FOsvayderUEReportTruthSummary TruthSummary;
		TruthSummary.PracticallyVerified.Add(TEXT("Dependency findings were classified from real Unreal log evidence and enriched with current project config, plugin/module state, and asset-registry facts."));
		TruthSummary.PracticallyVerified.Add(TEXT("Per-finding outputs now include structured recommendation lanes, bounded next-step suggestions, confidence basis, and current-state proof inputs."));

		for (const TSharedPtr<FDependencyFinding>& Finding : Findings)
		{
			if (!Finding.IsValid())
			{
				continue;
			}

			if (Finding->CurrentState == TEXT("resolved_in_current_state"))
			{
				AddUniqueLimited(TruthSummary.Inspected, FString::Printf(TEXT("%s is no longer active in the current project state."), *Finding->FindingKey), MaxPackageExamples);
			}
			else if (Finding->ContentImpactClass == TEXT("active_content_blocker"))
			{
				AddUniqueLimited(TruthSummary.Inspected, FString::Printf(TEXT("%s has strong active-content evidence via current cook roots or always-cook directories."), *Finding->FindingKey), MaxPackageExamples);
			}
			else if (Finding->RecommendationLane == TEXT("demote_or_defer"))
			{
				AddUniqueLimited(TruthSummary.Inferred, FString::Printf(TEXT("%s stays unresolved, but the logged packages no longer resolve in the current content state, so demote/defer is safer than immediate repair."), *Finding->FindingKey), MaxPackageExamples);
			}
			else
			{
				AddUniqueLimited(TruthSummary.Inferred, FString::Printf(TEXT("%s remains a lower-confidence dependency candidate that may need runtime proof."), *Finding->FindingKey), MaxPackageExamples);
			}
		}

		TruthSummary.Limited.Add(TEXT("This U2 slice is log-first classification and current-state enrichment, not full dependency understanding or automatic repair."));
		TruthSummary.Limited.Add(TEXT("Current-state proof is about current config/module/package/asset facts only; it does not prove runtime gameplay impact."));
		TruthSummary.NotVerified.Add(TEXT("Runtime gameplay impact is not proven by this slice unless a later runtime-proof packet confirms it."));
		return TruthSummary;
	}

	TSharedPtr<FJsonObject> BuildExtraMetadata(const TArray<FString>& SourceLogPaths, const FDependencyConfigSnapshot& Snapshot)
	{
		TSharedPtr<FJsonObject> ExtraMetadata = MakeShared<FJsonObject>();
		ExtraMetadata->SetStringField(TEXT("source_operation"), TEXT("classify_dependency_evidence"));
		ExtraMetadata->SetNumberField(TEXT("log_count"), SourceLogPaths.Num());

		TArray<TSharedPtr<FJsonValue>> LogValues;
		for (const FString& SourceLogPath : SourceLogPaths)
		{
			LogValues.Add(MakeShared<FJsonValueString>(SourceLogPath));
		}
		ExtraMetadata->SetArrayField(TEXT("log_paths"), LogValues);
		ExtraMetadata->SetStringField(TEXT("default_platform_service"), Snapshot.DefaultPlatformService);
		ExtraMetadata->SetStringField(TEXT("game_net_driver"), Snapshot.GameNetDriver);
		ExtraMetadata->SetBoolField(TEXT("recommendation_detail_enabled"), true);
		ExtraMetadata->SetBoolField(TEXT("current_state_proof_enabled"), true);
		return ExtraMetadata;
	}
}

FMCPToolResult FMCPTool_DependencyHealth::Execute(const TSharedRef<FJsonObject>& Params)
{
	const FString Operation = ExtractOptionalString(Params, TEXT("operation"), TEXT("classify_dependency_evidence"));
	if (!Operation.Equals(TEXT("classify_dependency_evidence"), ESearchCase::IgnoreCase))
	{
		return FMCPToolResult::Error(TEXT("Unsupported operation. Expected classify_dependency_evidence."));
	}

	TArray<FString> InputLogPaths = ExtractStringArrayField(Params, TEXT("log_paths"));
	const FString SingleLogPath = ExtractOptionalString(Params, TEXT("log_path"));
	if (!SingleLogPath.TrimStartAndEnd().IsEmpty())
	{
		InputLogPaths.Insert(SingleLogPath, 0);
	}

	TArray<FString> ResolvedLogPaths;
	for (const FString& InputLogPath : InputLogPaths)
	{
		const FString Normalized = NormalizeLogPath(InputLogPath);
		if (!Normalized.IsEmpty())
		{
			AddUniqueLimited(ResolvedLogPaths, Normalized);
		}
	}

	if (ResolvedLogPaths.Num() == 0)
	{
		ResolvedLogPaths.Add(NormalizeLogPath(FPaths::Combine(FPaths::ProjectLogDir(), FString::Printf(TEXT("%s.log"), FApp::GetProjectName()))));
	}

	for (const FString& ResolvedLogPath : ResolvedLogPaths)
	{
		if (!FPaths::FileExists(ResolvedLogPath))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Log file not found: %s"), *ResolvedLogPath));
		}
	}

	const int32 MaxFindings = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("max_findings"), 20), 1, 100);
	const bool bIncludeLogExcerpt = ExtractOptionalBool(Params, TEXT("include_log_excerpt"), true);
	const bool bExportReport = ExtractOptionalBool(Params, TEXT("export_report"), false);
	const FString CustomReportName = ExtractOptionalString(Params, TEXT("report_name"));
	const FString CustomReportSlug = ExtractOptionalString(Params, TEXT("report_slug"));

	FDependencyConfigSnapshot Snapshot = LoadConfigSnapshot();

	TMap<FString, TSharedPtr<FDependencyFinding>> FindingsByKey;
	TMap<FString, FString> AssetPackageToFindingKey;

	auto GetOrCreateFinding = [&FindingsByKey](const FString& Key, const FString& Kind, const FString& Subject) -> TSharedPtr<FDependencyFinding>
	{
		if (const TSharedPtr<FDependencyFinding>* Existing = FindingsByKey.Find(Key))
		{
			return *Existing;
		}

		TSharedPtr<FDependencyFinding> NewFinding = MakeShared<FDependencyFinding>();
		NewFinding->FindingKey = Key;
		NewFinding->FindingKind = Kind;
		NewFinding->Subject = Subject;
		FindingsByKey.Add(Key, NewFinding);
		return NewFinding;
	};

	for (const FString& ResolvedLogPath : ResolvedLogPaths)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *ResolvedLogPath))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to read log file: %s"), *ResolvedLogPath));
		}

		for (const FString& Line : Lines)
		{
			FString AssetFilename;
			FString ScriptPackage;
			if (ParseScriptPackageFailureLine(Line, AssetFilename, ScriptPackage))
			{
				const FString AssetPackage = AssetFilenameToPackagePath(AssetFilename);
				TSharedPtr<FDependencyFinding> Finding = GetOrCreateFinding(ScriptPackage, TEXT("missing_script_package"), ScriptPackage);
				++Finding->OccurrenceCount;
				AddUniqueLimited(Finding->ObservedLogs, ResolvedLogPath);
				AddUniqueLimited(Finding->EvidenceProvenance, TEXT("output_log"));
				AddUniqueLimited(Finding->EvidenceLines, Line, MaxLogExcerptLines);
				AddUniqueLimited(Finding->AssetPackages, AssetPackage, MaxPackageExamples);
				if (!AssetPackage.IsEmpty())
				{
					AssetPackageToFindingKey.FindOrAdd(AssetPackage) = ScriptPackage;
				}
				continue;
			}

			FString ReferencerPackage;
			FString MissingPackage;
			if (ParseMissingDependentPackageLine(Line, ReferencerPackage, MissingPackage))
			{
				TSharedPtr<FDependencyFinding> Finding = GetOrCreateFinding(MissingPackage, TEXT("missing_package_mount"), MissingPackage);
				++Finding->OccurrenceCount;
				AddUniqueLimited(Finding->ObservedLogs, ResolvedLogPath);
				AddUniqueLimited(Finding->EvidenceProvenance, TEXT("output_log"));
				AddUniqueLimited(Finding->EvidenceLines, Line, MaxLogExcerptLines);
				AddUniqueLimited(Finding->ReferencerPackages, ReferencerPackage, MaxPackageExamples);
				continue;
			}

			FString PluginPackage;
			FString PluginName;
			if (ParsePluginNotMountedLine(Line, PluginPackage, PluginName))
			{
				TSharedPtr<FDependencyFinding> Finding = GetOrCreateFinding(PluginPackage, TEXT("missing_package_mount"), PluginPackage);
				++Finding->OccurrenceCount;
				AddUniqueLimited(Finding->ObservedLogs, ResolvedLogPath);
				AddUniqueLimited(Finding->EvidenceProvenance, TEXT("output_log"));
				AddUniqueLimited(Finding->EvidenceLines, Line, MaxLogExcerptLines);
				Finding->HistoricalPluginName = PluginName;
				continue;
			}

			FString MissingClassAssetPackage;
			FString MissingClassName;
			if (ParseMissingClassLine(Line, MissingClassAssetPackage, MissingClassName))
			{
				FString FindingKey;
				if (const FString* ExistingFindingKey = AssetPackageToFindingKey.Find(MissingClassAssetPackage))
				{
					FindingKey = *ExistingFindingKey;
				}
				else
				{
					FindingKey = FString::Printf(TEXT("Class:%s"), *MissingClassName);
				}

				TSharedPtr<FDependencyFinding> Finding = GetOrCreateFinding(FindingKey, TEXT("missing_class_import"), MissingClassName);
				++Finding->OccurrenceCount;
				AddUniqueLimited(Finding->ObservedLogs, ResolvedLogPath);
				AddUniqueLimited(Finding->EvidenceProvenance, TEXT("output_log"));
				AddUniqueLimited(Finding->EvidenceLines, Line, MaxLogExcerptLines);
				AddUniqueLimited(Finding->AssetPackages, MissingClassAssetPackage, MaxPackageExamples);
				AddUniqueLimited(Finding->MissingClasses, MissingClassName, MaxPackageExamples);
				continue;
			}

			if (Line.Contains(TEXT("Unable to create OnlineSubsystem instance Steam"), ESearchCase::IgnoreCase))
			{
				TSharedPtr<FDependencyFinding> Finding = GetOrCreateFinding(TEXT("OSS:Steam"), TEXT("online_subsystem_failure"), TEXT("Steam OnlineSubsystem initialization"));
				++Finding->OccurrenceCount;
				AddUniqueLimited(Finding->ObservedLogs, ResolvedLogPath);
				AddUniqueLimited(Finding->EvidenceProvenance, TEXT("output_log"));
				AddUniqueLimited(Finding->EvidenceLines, Line, MaxLogExcerptLines);
				continue;
			}
		}
	}

	TArray<TSharedPtr<FDependencyFinding>> Findings;
	Findings.Reserve(FindingsByKey.Num());
	for (TPair<FString, TSharedPtr<FDependencyFinding>>& Pair : FindingsByKey)
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}

		for (const FString& AssetPackage : Pair.Value->AssetPackages)
		{
			AppendDirectReferencers(AssetPackage, Pair.Value->DirectCurrentReferencers);
			AddUniqueLimited(Pair.Value->EvidenceProvenance, TEXT("asset_registry"));
		}
		for (const FString& ReferencerPackage : Pair.Value->ReferencerPackages)
		{
			FString ResolvedFilename;
			if (DoesPackageExistNow(ReferencerPackage, ResolvedFilename))
			{
				AddUniqueLimited(Pair.Value->DirectCurrentReferencers, ReferencerPackage, MaxReferencerExamples);
			}
		}

		if (!Snapshot.MapsToCook.IsEmpty() || !Snapshot.DirectoriesToAlwaysCook.IsEmpty())
		{
			AddUniqueLimited(Pair.Value->EvidenceProvenance, TEXT("project_packaging_config"));
		}

		EnrichFindingCurrentState(*Pair.Value, Snapshot);
		if (Pair.Value->FindingKind == TEXT("online_subsystem_failure"))
		{
			AddUniqueLimited(Pair.Value->EvidenceProvenance, TEXT("engine_config"));
		}
		else if (Pair.Value->FindingKey.StartsWith(TEXT("/Script/")))
		{
			AddUniqueLimited(Pair.Value->EvidenceProvenance, TEXT("module_manager"));
			AddUniqueLimited(Pair.Value->EvidenceProvenance, TEXT("plugin_manager"));
		}
		else
		{
			AddUniqueLimited(Pair.Value->EvidenceProvenance, TEXT("plugin_manager"));
		}

		Findings.Add(Pair.Value);
	}

	Findings.Sort([](const TSharedPtr<FDependencyFinding>& A, const TSharedPtr<FDependencyFinding>& B)
	{
		auto SeverityRank = [](const FString& Severity) -> int32
		{
			if (Severity == TEXT("blocker")) return 0;
			if (Severity == TEXT("warning")) return 1;
			return 2;
		};

		const int32 RankA = A.IsValid() ? SeverityRank(A->Severity) : 99;
		const int32 RankB = B.IsValid() ? SeverityRank(B->Severity) : 99;
		if (RankA != RankB)
		{
			return RankA < RankB;
		}
		if (A.IsValid() && B.IsValid() && A->OccurrenceCount != B->OccurrenceCount)
		{
			return A->OccurrenceCount > B->OccurrenceCount;
		}
		return A.IsValid() && B.IsValid() ? A->FindingKey < B->FindingKey : A.IsValid();
	});

	if (Findings.Num() > MaxFindings)
	{
		Findings.SetNum(MaxFindings);
	}

	int32 BlockerCount = 0;
	int32 WarningCount = 0;
	int32 InfoCount = 0;
	int32 ResolvedCount = 0;
	int32 ActiveContentBlockerCount = 0;
	int32 RepairNowCount = 0;
	int32 RuntimeProofNextCount = 0;
	int32 DemoteOrDeferCount = 0;
	int32 NoActionCount = 0;
	for (const TSharedPtr<FDependencyFinding>& Finding : Findings)
	{
		if (!Finding.IsValid())
		{
			continue;
		}
		if (Finding->Severity == TEXT("blocker")) ++BlockerCount;
		else if (Finding->Severity == TEXT("warning")) ++WarningCount;
		else ++InfoCount;

		if (Finding->CurrentState == TEXT("resolved_in_current_state")) ++ResolvedCount;
		if (Finding->ContentImpactClass == TEXT("active_content_blocker")) ++ActiveContentBlockerCount;
		if (Finding->RecommendationLane == TEXT("repair_now")) ++RepairNowCount;
		else if (Finding->RecommendationLane == TEXT("runtime_proof_next")) ++RuntimeProofNextCount;
		else if (Finding->RecommendationLane == TEXT("demote_or_defer")) ++DemoteOrDeferCount;
		else if (Finding->RecommendationLane == TEXT("no_action")) ++NoActionCount;
	}

	TSharedPtr<FJsonObject> SummaryObject = MakeShared<FJsonObject>();
	SummaryObject->SetNumberField(TEXT("finding_count"), Findings.Num());
	SummaryObject->SetNumberField(TEXT("blocker_count"), BlockerCount);
	SummaryObject->SetNumberField(TEXT("warning_count"), WarningCount);
	SummaryObject->SetNumberField(TEXT("info_count"), InfoCount);
	SummaryObject->SetNumberField(TEXT("resolved_in_current_state_count"), ResolvedCount);
	SummaryObject->SetNumberField(TEXT("active_content_blocker_count"), ActiveContentBlockerCount);
	SummaryObject->SetNumberField(TEXT("repair_now_count"), RepairNowCount);
	SummaryObject->SetNumberField(TEXT("runtime_proof_next_count"), RuntimeProofNextCount);
	SummaryObject->SetNumberField(TEXT("demote_or_defer_count"), DemoteOrDeferCount);
	SummaryObject->SetNumberField(TEXT("no_action_count"), NoActionCount);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("classify_dependency_evidence"));
	ResultData->SetObjectField(TEXT("summary"), SummaryObject);
	ResultData->SetObjectField(TEXT("current_state_snapshot"), BuildCurrentStateSnapshotJson(Snapshot));

	TArray<TSharedPtr<FJsonValue>> InputLogJson;
	for (const FString& ResolvedLogPath : ResolvedLogPaths)
	{
		InputLogJson.Add(MakeShared<FJsonValueString>(ResolvedLogPath));
	}
	ResultData->SetArrayField(TEXT("log_paths"), InputLogJson);

	TArray<TSharedPtr<FJsonValue>> FindingsJson;
	for (const TSharedPtr<FDependencyFinding>& Finding : Findings)
	{
		FindingsJson.Add(MakeShared<FJsonValueObject>(FindingToJson(*Finding, bIncludeLogExcerpt)));
	}
	ResultData->SetArrayField(TEXT("findings"), FindingsJson);

	FString Message = FString::Printf(
		TEXT("Classified %d dependency findings across %d log%s (%d blocker, %d warning, %d info)"),
		Findings.Num(),
		ResolvedLogPaths.Num(),
		ResolvedLogPaths.Num() == 1 ? TEXT("") : TEXT("s"),
		BlockerCount,
		WarningCount,
		InfoCount);

	if (bExportReport)
	{
		FOsvayderUEReportExportRequest ExportRequest;
		ExportRequest.ReportName = !CustomReportName.TrimStartAndEnd().IsEmpty()
			? CustomReportName
			: FString::Printf(TEXT("Dependency Health Classification - %d Findings"), Findings.Num());
		ExportRequest.ReportSlug = !CustomReportSlug.TrimStartAndEnd().IsEmpty()
			? CustomReportSlug
			: TEXT("dependency_health_classification");
		ExportRequest.Markdown = BuildDependencyReportMarkdown(ResolvedLogPaths, Snapshot, Findings);
		ExportRequest.SummaryText = Message;
		ExportRequest.RunKind = TEXT("dependency_health_classification");
		ExportRequest.ExecutionMode = TEXT("read_only");
		ExportRequest.ToolNames = { TEXT("dependency_health") };
		ExportRequest.EvidenceClasses = {
			TEXT("output_log_dependency_evidence"),
			TEXT("asset_registry_referencers"),
			TEXT("project_packaging_config"),
			TEXT("plugin_module_state")
		};
		ExportRequest.TruthSummary = BuildTruthSummary(Findings);
		ExportRequest.ExtraMetadata = BuildExtraMetadata(ResolvedLogPaths, Snapshot);

		FOsvayderUEReportExportResult ExportResult;
		if (!FOsvayderUEReportArtifacts::ExportReport(ExportRequest, ExportResult))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("%s | report export failed: %s"), *Message, *ExportResult.ErrorMessage));
		}

		TSharedPtr<FJsonObject> ArtifactObject = MakeShared<FJsonObject>();
		ArtifactObject->SetStringField(TEXT("report_id"), ExportResult.ReportId);
		ArtifactObject->SetStringField(TEXT("markdown_path"), ExportResult.MarkdownPath);
		ArtifactObject->SetStringField(TEXT("summary_path"), ExportResult.SummaryPath);
		ArtifactObject->SetStringField(TEXT("export_status"), ExportResult.ExportStatus);
		ArtifactObject->SetBoolField(TEXT("roundtrip_exact"), ExportResult.bRoundTripExact);
		ArtifactObject->SetStringField(TEXT("status_tool"), TEXT("report_artifact_status"));
		ArtifactObject->SetStringField(TEXT("status_query_report_id"), ExportResult.ReportId);
		ResultData->SetObjectField(TEXT("report_artifact"), ArtifactObject);
		Message = FString::Printf(TEXT("%s | report=%s"), *Message, *ExportResult.ReportId);
	}

	return FMCPToolResult::Success(Message, ResultData);
}
