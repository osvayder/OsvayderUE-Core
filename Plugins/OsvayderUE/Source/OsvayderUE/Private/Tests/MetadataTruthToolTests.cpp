// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "CharacterDataTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_MetadataTruth.h"
#include "MCP/Tools/MCPTool_ReportArtifactStatus.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	bool DoesPackageExistOnDisk(const FString& PackagePath)
	{
		FString Filename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetAssetPackageExtension())
			&& FPaths::FileExists(Filename))
		{
			return true;
		}

		return FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetMapPackageExtension())
			&& FPaths::FileExists(Filename);
	}

	bool TryGetCurrentProjectMapPackages(const int32 RequiredCount, TArray<FString>& OutPackages)
	{
		const TArray<FString> PreferredMaps = {
			TEXT("/Game/ThirdPerson/Lvl_ThirdPerson"),
			TEXT("/Game/Variant_Combat/Lvl_Combat"),
			TEXT("/Game/Variant_Platforming/Lvl_Platforming"),
			TEXT("/Game/Variant_SideScrolling/Lvl_SideScrolling")
		};
		for (const FString& PreferredMap : PreferredMaps)
		{
			if (DoesPackageExistOnDisk(PreferredMap))
			{
				OutPackages.Add(PreferredMap);
			}
		}

		if (OutPackages.Num() < RequiredCount)
		{
			TArray<FString> MapFiles;
			IFileManager::Get().FindFilesRecursive(MapFiles, *FPaths::ProjectContentDir(), TEXT("*.umap"), true, false, false);
			MapFiles.Sort();
			for (const FString& MapFile : MapFiles)
			{
				FString PackagePath;
				if (FPackageName::TryConvertFilenameToLongPackageName(MapFile, PackagePath)
					&& !OutPackages.Contains(PackagePath))
				{
					OutPackages.Add(PackagePath);
				}
			}
		}

		return OutPackages.Num() >= RequiredCount;
	}

	bool TryGetExistingCurrentAssetPackage(FString& OutPackage)
	{
		const TArray<FString> PreferredAssets = {
			TEXT("/Game/Input/IMC_Default"),
			TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"),
			TEXT("/Game/LevelPrototyping/Interactable/Door/BP_DoorFrame")
		};
		for (const FString& PreferredAsset : PreferredAssets)
		{
			if (DoesPackageExistOnDisk(PreferredAsset))
			{
				OutPackage = PreferredAsset;
				return true;
			}
		}

		TArray<FString> AssetFiles;
		IFileManager::Get().FindFilesRecursive(AssetFiles, *FPaths::ProjectContentDir(), TEXT("*.uasset"), true, false, false);
		AssetFiles.Sort();
		for (const FString& AssetFile : AssetFiles)
		{
			if (AssetFile.Contains(TEXT("__ExternalActors__")) || AssetFile.Contains(TEXT("__ExternalObjects__")))
			{
				continue;
			}

			FString PackagePath;
			if (FPackageName::TryConvertFilenameToLongPackageName(AssetFile, PackagePath))
			{
				OutPackage = PackagePath;
				return true;
			}
		}

		return false;
	}

	struct FScopedMetadataTruthMapsToCookFixture
	{
		TArray<FString> DeclarationMaps;
		TArray<FString> ShippingMaps;
		FString DataTableObjectPath;
		FString DefaultGameIniPath;
		FString OriginalDefaultGameIni;
		bool bHadOriginalDefaultGameIni = false;
		UDataTable* DataTable = nullptr;

		bool Initialize(FAutomationTestBase& Test)
		{
			DefaultGameIniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini"));
			bHadOriginalDefaultGameIni = FFileHelper::LoadFileToString(OriginalDefaultGameIni, *DefaultGameIniPath);

			TArray<FString> CurrentMaps;
			if (!TryGetCurrentProjectMapPackages(3, CurrentMaps))
			{
				Test.AddError(TEXT("metadata_truth fixture requires at least three current map packages"));
				return false;
			}

			DeclarationMaps = { CurrentMaps[0], CurrentMaps[1] };
			ShippingMaps = { CurrentMaps[0], CurrentMaps[1], CurrentMaps[2] };

			const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString DataTablePackagePath = FString::Printf(TEXT("/Game/OsvayderUEAutomation/DT_MetadataTruth_%s"), *UniqueSuffix);
			const FString DataTableAssetName = FPackageName::GetLongPackageAssetName(DataTablePackagePath);

			UPackage* Package = CreatePackage(*DataTablePackagePath);
			if (!Package)
			{
				Test.AddError(TEXT("metadata_truth fixture data table package should be created"));
				return false;
			}

			DataTable = NewObject<UDataTable>(Package, UDataTable::StaticClass(), *DataTableAssetName, RF_Public | RF_Standalone);
			if (!DataTable)
			{
				Test.AddError(TEXT("metadata_truth fixture data table should be created"));
				return false;
			}

			DataTable->RowStruct = FCharacterStatsRow::StaticStruct();
			for (int32 Index = 0; Index < DeclarationMaps.Num(); ++Index)
			{
				FCharacterStatsRow RowValue;
				RowValue.StatsId = FName(*FString::Printf(TEXT("Map_%d"), Index + 1));
				RowValue.DisplayName = DeclarationMaps[Index];
				DataTable->AddRow(FName(*FString::Printf(TEXT("Map_%d"), Index + 1)), RowValue);
			}
			DataTable->AddToRoot();

			DataTableObjectPath = FString::Printf(TEXT("%s.%s"), *DataTablePackagePath, *DataTableAssetName);

			FString UpdatedDefaultGameIni = bHadOriginalDefaultGameIni ? OriginalDefaultGameIni : FString();
			if (!UpdatedDefaultGameIni.IsEmpty() && !UpdatedDefaultGameIni.EndsWith(TEXT("\n")))
			{
				UpdatedDefaultGameIni += TEXT("\n");
			}
			UpdatedDefaultGameIni += TEXT("\n[/Script/OsvayderUE.MetadataTruthAutomationFixture]\n");
			for (const FString& ShippingMap : ShippingMaps)
			{
				UpdatedDefaultGameIni += FString::Printf(TEXT("MapsToCook=(FilePath=\"%s\")\n"), *ShippingMap);
			}

			if (!FFileHelper::SaveStringToFile(UpdatedDefaultGameIni, *DefaultGameIniPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Test.AddError(TEXT("metadata_truth fixture should update DefaultGame.ini"));
				DataTable->RemoveFromRoot();
				DataTable = nullptr;
				return false;
			}

			return true;
		}

		~FScopedMetadataTruthMapsToCookFixture()
		{
			if (DataTable)
			{
				DataTable->RemoveFromRoot();
			}

			if (DefaultGameIniPath.IsEmpty())
			{
				return;
			}

			if (bHadOriginalDefaultGameIni)
			{
				FFileHelper::SaveStringToFile(OriginalDefaultGameIni, *DefaultGameIniPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}
			else
			{
				IFileManager::Get().Delete(*DefaultGameIniPath, false, true);
			}
		}
	};

	TSharedPtr<FJsonObject> FindItemByPackage(const TSharedPtr<FJsonObject>& ResultData, const FString& PackagePath)
	{
		if (!ResultData.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!ResultData->TryGetArrayField(TEXT("items"), Items) || !Items)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& ItemValue : *Items)
		{
			const TSharedPtr<FJsonObject> ItemObject = ItemValue.IsValid() ? ItemValue->AsObject() : nullptr;
			if (!ItemObject.IsValid())
			{
				continue;
			}

			FString AssetPackage;
			if (ItemObject->TryGetStringField(TEXT("asset_package"), AssetPackage) && AssetPackage == PackagePath)
			{
				return ItemObject;
			}
		}

		return nullptr;
	}

	int32 GetBucketCount(const TSharedPtr<FJsonObject>& ResultData, const FString& BucketName)
	{
		if (!ResultData.IsValid())
		{
			return 0;
		}

		const TSharedPtr<FJsonObject>* BucketCounts = nullptr;
		if (!ResultData->TryGetObjectField(TEXT("bucket_counts"), BucketCounts) || !BucketCounts || !(*BucketCounts).IsValid())
		{
			return 0;
		}

		return static_cast<int32>((*BucketCounts)->GetNumberField(BucketName));
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMetadataTruth_RegistryToolsRegistered,
	"OsvayderUE.MetadataTruth.Registry.ToolsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMetadataTruth_RegistryToolsRegistered::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestTrue(TEXT("metadata_truth should be registered"), Registry.HasTool(TEXT("metadata_truth")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMetadataTruth_CompareExplicitBuckets,
	"OsvayderUE.MetadataTruth.CompareExplicitBuckets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMetadataTruth_CompareExplicitBuckets::RunTest(const FString& Parameters)
{
	FMCPTool_MetadataTruth Tool;
	TArray<FString> CurrentMapPackages;
	FString ExistingAssetPackage;
	TestTrue(TEXT("metadata_truth explicit comparison should find at least two current project maps"), TryGetCurrentProjectMapPackages(2, CurrentMapPackages));
	TestTrue(TEXT("metadata_truth explicit comparison should find an existing current project asset"), TryGetExistingCurrentAssetPackage(ExistingAssetPackage));
	if (CurrentMapPackages.Num() < 2 || ExistingAssetPackage.IsEmpty())
	{
		return false;
	}

	const FString ConsistentMapPackage = CurrentMapPackages[0];
	const FString ShippedOnlyMapPackage = CurrentMapPackages[1];
	const FString MissingPackage = TEXT("/Game/Maps/DefinitelyMissing_MetadataTruth");

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	const TArray<FString> DeclarationValues = {
		ConsistentMapPackage,
		ExistingAssetPackage,
		MissingPackage,
		TEXT("NotA/Package/Path")
	};
	const TArray<FString> ShippingValues = {
		ConsistentMapPackage,
		ShippedOnlyMapPackage,
		MissingPackage
	};
	Params->SetStringField(TEXT("declaration_surface_kind"), TEXT("explicit_assets"));
	Params->SetArrayField(TEXT("declaration_items"), MakeStringArray(DeclarationValues));
	Params->SetStringField(TEXT("shipping_surface_kind"), TEXT("explicit_assets"));
	Params->SetArrayField(TEXT("shipping_items"), MakeStringArray(ShippingValues));

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("explicit metadata_truth compare should succeed"), Result.bSuccess);
	TestTrue(TEXT("explicit metadata_truth compare should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("consistent bucket should contain one item"), GetBucketCount(Result.Data, TEXT("consistent")), 1);
	TestEqual(TEXT("advertised_but_not_shipped bucket should contain one item"), GetBucketCount(Result.Data, TEXT("advertised_but_not_shipped")), 1);
	TestEqual(TEXT("shipped_but_unadvertised bucket should contain one item"), GetBucketCount(Result.Data, TEXT("shipped_but_unadvertised")), 1);
	TestEqual(TEXT("advertised_but_not_currently_discovered bucket should contain one item"), GetBucketCount(Result.Data, TEXT("advertised_but_not_currently_discovered")), 1);
	TestEqual(TEXT("uncertain bucket should contain one item"), GetBucketCount(Result.Data, TEXT("uncertain")), 1);

	const TSharedPtr<FJsonObject> Consistent = FindItemByPackage(Result.Data, ConsistentMapPackage);
	TestTrue(TEXT("current project consistent map item should exist"), Consistent.IsValid());
	if (Consistent.IsValid())
	{
		TestEqual(TEXT("current project consistent map should be consistent"), Consistent->GetStringField(TEXT("consistency_bucket")), FString(TEXT("consistent")));
	}

	const TSharedPtr<FJsonObject> AdvertisedNotShipped = FindItemByPackage(Result.Data, ExistingAssetPackage);
	TestTrue(TEXT("current project declaration-only asset should exist"), AdvertisedNotShipped.IsValid());
	if (AdvertisedNotShipped.IsValid())
	{
		TestEqual(TEXT("current project declaration-only asset should be advertised_but_not_shipped"), AdvertisedNotShipped->GetStringField(TEXT("consistency_bucket")), FString(TEXT("advertised_but_not_shipped")));
	}

	const TSharedPtr<FJsonObject> MissingDiscovered = FindItemByPackage(Result.Data, MissingPackage);
	TestTrue(TEXT("missing package item should exist"), MissingDiscovered.IsValid());
	if (MissingDiscovered.IsValid())
	{
		TestEqual(TEXT("missing package should be advertised_but_not_currently_discovered"), MissingDiscovered->GetStringField(TEXT("consistency_bucket")), FString(TEXT("advertised_but_not_currently_discovered")));
	}

	const TSharedPtr<FJsonObject> ShippedOnly = FindItemByPackage(Result.Data, ShippedOnlyMapPackage);
	TestTrue(TEXT("current project shipping-only map item should exist"), ShippedOnly.IsValid());
	if (ShippedOnly.IsValid())
	{
		TestEqual(TEXT("current project shipping-only map should be shipped_but_unadvertised"), ShippedOnly->GetStringField(TEXT("consistency_bucket")), FString(TEXT("shipped_but_unadvertised")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMetadataTruth_CompareLevelInfoTableToMapsToCook,
	"OsvayderUE.MetadataTruth.CompareLevelInfoTableToMapsToCook",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMetadataTruth_CompareLevelInfoTableToMapsToCook::RunTest(const FString& Parameters)
{
	FMCPTool_MetadataTruth Tool;
	FScopedMetadataTruthMapsToCookFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("declaration_surface_kind"), TEXT("data_table_string_field"));
	Params->SetStringField(TEXT("data_table_path"), Fixture.DataTableObjectPath);
	Params->SetStringField(TEXT("data_table_field_name_contains"), TEXT("DisplayName"));
	Params->SetStringField(TEXT("metadata_surface_name"), TEXT("level_catalog"));
	Params->SetStringField(TEXT("shipping_surface_kind"), TEXT("maps_to_cook"));
	Params->SetStringField(TEXT("shipping_surface_name"), TEXT("maps_to_cook"));
	Params->SetBoolField(TEXT("include_unadvertised_shipped"), true);

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("LevelInfo metadata_truth compare should succeed"), Result.bSuccess);
	TestTrue(TEXT("LevelInfo metadata_truth compare should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("LevelInfo comparison should keep at least two consistent items"), GetBucketCount(Result.Data, TEXT("consistent")) >= 2);
	TestTrue(TEXT("LevelInfo comparison should surface shipped_but_unadvertised maps_to_cook entries"), GetBucketCount(Result.Data, TEXT("shipped_but_unadvertised")) >= 1);

	const TSharedPtr<FJsonObject> DeclarationMapA = FindItemByPackage(Result.Data, Fixture.DeclarationMaps[0]);
	const TSharedPtr<FJsonObject> DeclarationMapB = FindItemByPackage(Result.Data, Fixture.DeclarationMaps[1]);
	const TSharedPtr<FJsonObject> ShippingOnlyMap = FindItemByPackage(Result.Data, Fixture.ShippingMaps[2]);
	TestTrue(TEXT("fixture declaration map A should be present in comparison"), DeclarationMapA.IsValid());
	TestTrue(TEXT("fixture declaration map B should be present in comparison"), DeclarationMapB.IsValid());
	TestTrue(TEXT("fixture shipping-only map should be present in comparison"), ShippingOnlyMap.IsValid());
	if (DeclarationMapA.IsValid())
	{
		TestEqual(TEXT("fixture declaration map A should stay consistent"), DeclarationMapA->GetStringField(TEXT("consistency_bucket")), FString(TEXT("consistent")));
	}
	if (DeclarationMapB.IsValid())
	{
		TestEqual(TEXT("fixture declaration map B should stay consistent"), DeclarationMapB->GetStringField(TEXT("consistency_bucket")), FString(TEXT("consistent")));
	}
	if (ShippingOnlyMap.IsValid())
	{
		TestEqual(TEXT("fixture shipping-only map should be shipped_but_unadvertised"), ShippingOnlyMap->GetStringField(TEXT("consistency_bucket")), FString(TEXT("shipped_but_unadvertised")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMetadataTruth_ExportReportArtifact,
	"OsvayderUE.MetadataTruth.ExportReportArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMetadataTruth_ExportReportArtifact::RunTest(const FString& Parameters)
{
	FMCPTool_MetadataTruth Tool;
	FMCPTool_ReportArtifactStatus StatusTool;
	FScopedMetadataTruthMapsToCookFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("declaration_surface_kind"), TEXT("data_table_string_field"));
	Params->SetStringField(TEXT("data_table_path"), Fixture.DataTableObjectPath);
	Params->SetStringField(TEXT("data_table_field_name_contains"), TEXT("DisplayName"));
	Params->SetStringField(TEXT("metadata_surface_name"), TEXT("level_catalog"));
	Params->SetStringField(TEXT("shipping_surface_kind"), TEXT("maps_to_cook"));
	Params->SetStringField(TEXT("shipping_surface_name"), TEXT("maps_to_cook"));
	Params->SetBoolField(TEXT("include_unadvertised_shipped"), true);
	Params->SetBoolField(TEXT("export_report"), true);
	Params->SetStringField(TEXT("report_name"), TEXT("Metadata Truth Automation Probe"));
	Params->SetStringField(TEXT("report_slug"), TEXT("metadata_truth_automation_probe"));

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("metadata_truth export should succeed"), Result.bSuccess);
	TestTrue(TEXT("metadata_truth export should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ArtifactObject = nullptr;
	TestTrue(TEXT("metadata_truth result should include report_artifact"), Result.Data->TryGetObjectField(TEXT("report_artifact"), ArtifactObject) && ArtifactObject && (*ArtifactObject).IsValid());
	if (!ArtifactObject || !(*ArtifactObject).IsValid())
	{
		return false;
	}

	FString ReportId;
	FString MarkdownPath;
	FString SummaryPath;
	FString StatusToolName;
	(*ArtifactObject)->TryGetStringField(TEXT("report_id"), ReportId);
	(*ArtifactObject)->TryGetStringField(TEXT("markdown_path"), MarkdownPath);
	(*ArtifactObject)->TryGetStringField(TEXT("summary_path"), SummaryPath);
	(*ArtifactObject)->TryGetStringField(TEXT("status_tool"), StatusToolName);

	TestFalse(TEXT("report_id should not be empty"), ReportId.IsEmpty());
	TestTrue(TEXT("markdown report should exist"), FPaths::FileExists(MarkdownPath));
	TestTrue(TEXT("summary report should exist"), FPaths::FileExists(SummaryPath));
	TestEqual(TEXT("status tool should be report_artifact_status"), StatusToolName, FString(TEXT("report_artifact_status")));
	if (ReportId.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> StatusParams = MakeShared<FJsonObject>();
	StatusParams->SetStringField(TEXT("report_id"), ReportId);
	StatusParams->SetBoolField(TEXT("latest_only"), false);
	StatusParams->SetBoolField(TEXT("include_markdown_preview"), true);

	const FMCPToolResult StatusResult = StatusTool.Execute(StatusParams);
	TestTrue(TEXT("report_artifact_status readback should succeed"), StatusResult.bSuccess);
	TestTrue(TEXT("report_artifact_status readback should return data"), StatusResult.Data.IsValid());
	if (!StatusResult.bSuccess || !StatusResult.Data.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Reports = nullptr;
	TestTrue(TEXT("readback should return one report"), StatusResult.Data->TryGetArrayField(TEXT("reports"), Reports) && Reports && Reports->Num() == 1);
	if (!Reports || Reports->Num() != 1)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ReportObject = (*Reports)[0]->AsObject();
	TestTrue(TEXT("readback report object should be valid"), ReportObject.IsValid());
	if (ReportObject.IsValid())
	{
		TestEqual(TEXT("readback run_kind should match compare_asset_truth"), ReportObject->GetStringField(TEXT("run_kind")), FString(TEXT("compare_asset_truth")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
