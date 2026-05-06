// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_MetadataTruth.h"

#include "Algo/Sort.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "OsvayderUEReportArtifacts.h"

namespace
{
	constexpr int32 MaxEvidenceEntries = 12;

	struct FMetadataTruthConfigSnapshot
	{
		TArray<FString> MapsToCook;
	};

	struct FMetadataTruthSurfaceItem
	{
		FString SourceItemId;
		FString DisplayName;
		FString AssetPackage;
		FString RawValue;
		bool bPackageNormalized = false;
		TArray<FString> EvidenceBasis;
		TArray<FString> LimitationNotes;
	};

	struct FMetadataTruthComparisonItem
	{
		FString ComparisonKey;
		FString ItemId;
		FString DisplayName;
		FString AssetPackage;
		bool bDeclarationPresent = false;
		bool bShippingPresent = false;
		bool bDiscoveryChecked = false;
		bool bDiscoveryPresent = false;
		bool bFilePresent = false;
		bool bAssetRegistryPresent = false;
		FString ResolvedFilename;
		TArray<FString> DeclarationSourceIds;
		TArray<FString> ShippingSourceIds;
		TArray<FString> RawValues;
		TArray<FString> EvidenceBasis;
		TArray<FString> LimitationNotes;
		FString ConsistencyBucket;
		FString Confidence;
		FString RecommendedStepClass;
		FString RecommendedStepReason;
	};

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
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	}

	bool TryExtractBetween(const FString& Source, const FString& Prefix, const FString& Suffix, FString& OutValue)
	{
		const int32 PrefixIndex = Source.Find(Prefix, ESearchCase::CaseSensitive, ESearchDir::FromStart);
		if (PrefixIndex == INDEX_NONE)
		{
			return false;
		}

		const int32 StartIndex = PrefixIndex + Prefix.Len();
		const int32 SuffixIndex = Source.Find(Suffix, ESearchCase::CaseSensitive, ESearchDir::FromStart, StartIndex);
		if (SuffixIndex == INDEX_NONE || SuffixIndex < StartIndex)
		{
			return false;
		}

		OutValue = Source.Mid(StartIndex, SuffixIndex - StartIndex);
		return !OutValue.IsEmpty();
	}

	FString NormalizePackagePath(const FString& InValue)
	{
		FString Candidate = InValue.TrimStartAndEnd();
		if (Candidate.IsEmpty())
		{
			return FString();
		}

		Candidate.ReplaceInline(TEXT("\""), TEXT(""));

		FSoftObjectPath SoftObjectPath(Candidate);
		if (!SoftObjectPath.GetLongPackageName().IsEmpty())
		{
			Candidate = SoftObjectPath.GetLongPackageName();
		}
		else if (Candidate.Contains(TEXT(".")))
		{
			FString LeftSide;
			if (Candidate.Split(TEXT("."), &LeftSide, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart))
			{
				Candidate = LeftSide;
			}
		}

		FText UnusedReason;
		if (!FPackageName::IsValidLongPackageName(Candidate, false, &UnusedReason))
		{
			return FString();
		}

		return Candidate;
	}

	FString BuildPackagePathFromTemplate(const FString& TemplateValue, const FString& RawValue)
	{
		const FString TrimmedRaw = RawValue.TrimStartAndEnd();
		if (TrimmedRaw.IsEmpty())
		{
			return FString();
		}

		if (TemplateValue.TrimStartAndEnd().IsEmpty())
		{
			return NormalizePackagePath(TrimmedRaw);
		}

		FString Expanded = TemplateValue;
		Expanded.ReplaceInline(TEXT("{value}"), *TrimmedRaw, ESearchCase::CaseSensitive);
		return NormalizePackagePath(Expanded);
	}

	FString GetPackageLeafName(const FString& PackagePath)
	{
		FString Left;
		FString Right;
		if (PackagePath.Split(TEXT("/"), &Left, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			return Right;
		}
		return PackagePath;
	}

	FMetadataTruthConfigSnapshot LoadConfigSnapshot()
	{
		FMetadataTruthConfigSnapshot Snapshot;

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
			}
		}

		return Snapshot;
	}

	bool DoesPackageExistNow(
		const FString& PackagePath,
		bool& bOutFilePresent,
		bool& bOutAssetRegistryPresent,
		FString& OutResolvedFilename)
	{
		bOutFilePresent = false;
		bOutAssetRegistryPresent = false;
		OutResolvedFilename.Reset();

		if (PackagePath.IsEmpty())
		{
			return false;
		}

		FString CandidateFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, CandidateFilename, FPackageName::GetAssetPackageExtension())
			&& FPaths::FileExists(CandidateFilename))
		{
			bOutFilePresent = true;
			OutResolvedFilename = CandidateFilename;
		}
		else if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, CandidateFilename, FPackageName::GetMapPackageExtension())
			&& FPaths::FileExists(CandidateFilename))
		{
			bOutFilePresent = true;
			OutResolvedFilename = CandidateFilename;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> AssetDatas;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), AssetDatas, true);
		bOutAssetRegistryPresent = AssetDatas.Num() > 0;

		return bOutFilePresent || bOutAssetRegistryPresent;
	}

	const FProperty* ResolveDataTableField(
		const UScriptStruct* RowStruct,
		const FString& ExactFieldName,
		const FString& FieldNameContains,
		FString& OutResolvedFieldName,
		FString& OutError)
	{
		if (!RowStruct)
		{
			OutError = TEXT("DataTable RowStruct is missing");
			return nullptr;
		}

		if (!ExactFieldName.TrimStartAndEnd().IsEmpty())
		{
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				if (It->GetName() == ExactFieldName)
				{
					OutResolvedFieldName = It->GetName();
					return *It;
				}
			}

			OutError = FString::Printf(TEXT("DataTable field '%s' was not found on row struct '%s'"), *ExactFieldName, *RowStruct->GetName());
			return nullptr;
		}

		const FString Substring = FieldNameContains.TrimStartAndEnd();
		if (Substring.IsEmpty())
		{
			OutError = TEXT("Either data_table_field_name or data_table_field_name_contains is required for data_table_string_field");
			return nullptr;
		}

		const FProperty* Match = nullptr;
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			if (It->GetName().Contains(Substring, ESearchCase::IgnoreCase))
			{
				if (Match)
				{
					OutError = FString::Printf(
						TEXT("DataTable field name contains '%s' matched more than one field on '%s'"),
						*Substring,
						*RowStruct->GetName());
					return nullptr;
				}

				Match = *It;
				OutResolvedFieldName = It->GetName();
			}
		}

		if (!Match)
		{
			OutError = FString::Printf(
				TEXT("No DataTable field containing '%s' was found on row struct '%s'"),
				*Substring,
				*RowStruct->GetName());
		}

		return Match;
	}

	bool ExtractStringLikePropertyValue(const FProperty* Property, const uint8* RowData, FString& OutValue)
	{
		if (!Property || !RowData)
		{
			return false;
		}

		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			OutValue = StrProperty->GetPropertyValue_InContainer(RowData);
			return true;
		}

		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			OutValue = NameProperty->GetPropertyValue_InContainer(RowData).ToString();
			return true;
		}

		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			OutValue = TextProperty->GetPropertyValue_InContainer(RowData).ToString();
			return true;
		}

		return false;
	}

	void AddItemEvidence(FMetadataTruthComparisonItem& InOutItem, const FMetadataTruthSurfaceItem& SurfaceItem)
	{
		for (const FString& Basis : SurfaceItem.EvidenceBasis)
		{
			AddUniqueLimited(InOutItem.EvidenceBasis, Basis, MaxEvidenceEntries);
		}

		for (const FString& Note : SurfaceItem.LimitationNotes)
		{
			AddUniqueLimited(InOutItem.LimitationNotes, Note, MaxEvidenceEntries);
		}
	}

	FString BucketSortKey(const FString& Bucket)
	{
		if (Bucket == TEXT("advertised_but_not_currently_discovered"))
		{
			return TEXT("0");
		}
		if (Bucket == TEXT("advertised_but_not_shipped"))
		{
			return TEXT("1");
		}
		if (Bucket == TEXT("shipped_but_unadvertised"))
		{
			return TEXT("2");
		}
		if (Bucket == TEXT("uncertain"))
		{
			return TEXT("3");
		}
		return TEXT("4");
	}

	void FinalizeComparisonItem(FMetadataTruthComparisonItem& Item)
	{
		if (Item.AssetPackage.IsEmpty())
		{
			Item.ConsistencyBucket = TEXT("uncertain");
			Item.Confidence = TEXT("weak");
			Item.RecommendedStepClass = TEXT("normalize_metadata_input");
			Item.RecommendedStepReason = TEXT("Comparison item did not resolve to a valid long package path.");
			AddUniqueLimited(Item.LimitationNotes, TEXT("No valid long package path was derived for this item."), MaxEvidenceEntries);
			return;
		}

		if (Item.bDeclarationPresent && !Item.bDiscoveryPresent)
		{
			Item.ConsistencyBucket = TEXT("advertised_but_not_currently_discovered");
			Item.Confidence = Item.bDiscoveryChecked ? TEXT("strong") : TEXT("medium");
			Item.RecommendedStepClass = TEXT("current_state_discovery_review");
			Item.RecommendedStepReason = TEXT("Declaration metadata points at an asset package that does not currently resolve through file or asset-registry discovery.");
			AddUniqueLimited(Item.LimitationNotes, TEXT("Current package discovery is not runtime proof; a runtime packet would still be needed before making runtime claims."), MaxEvidenceEntries);
			return;
		}

		if (Item.bDeclarationPresent && !Item.bShippingPresent)
		{
			Item.ConsistencyBucket = TEXT("advertised_but_not_shipped");
			Item.Confidence = TEXT("strong");
			Item.RecommendedStepClass = TEXT("shipping_surface_review");
			Item.RecommendedStepReason = TEXT("Declaration metadata advertises the asset package, but the selected shipping surface does not.");
			AddUniqueLimited(Item.LimitationNotes, TEXT("A metadata/shipping mismatch is not automatically a runtime bug without runtime evidence."), MaxEvidenceEntries);
			return;
		}

		if (!Item.bDeclarationPresent && Item.bShippingPresent)
		{
			Item.ConsistencyBucket = TEXT("shipped_but_unadvertised");
			Item.Confidence = TEXT("strong");
			Item.RecommendedStepClass = TEXT("metadata_surface_review");
			Item.RecommendedStepReason = TEXT("The selected shipping surface includes the asset package, but the declaration surface does not advertise it.");
			AddUniqueLimited(Item.LimitationNotes, TEXT("Shipped-but-unadvertised content may be intentional; compare against the intended metadata surface before treating it as a defect."), MaxEvidenceEntries);
			return;
		}

		if (Item.bDeclarationPresent && Item.bShippingPresent && Item.bDiscoveryPresent)
		{
			Item.ConsistencyBucket = TEXT("consistent");
			Item.Confidence = TEXT("strong");
			Item.RecommendedStepClass = TEXT("no_action");
			Item.RecommendedStepReason = TEXT("Declaration metadata, shipping surface, and current package discovery agree for this asset package.");
			AddUniqueLimited(Item.LimitationNotes, TEXT("Current package discovery confirms package presence only; it does not prove runtime behavior."), MaxEvidenceEntries);
			return;
		}

		Item.ConsistencyBucket = TEXT("uncertain");
		Item.Confidence = TEXT("medium");
		Item.RecommendedStepClass = TEXT("evidence_review");
		Item.RecommendedStepReason = TEXT("Available evidence families do not align strongly enough for a narrower truth bucket.");
		AddUniqueLimited(Item.LimitationNotes, TEXT("Current evidence is insufficient to narrow this item beyond uncertain."), MaxEvidenceEntries);
	}

	TSharedPtr<FJsonObject> BuildSurfaceJson(
		const FString& SurfaceName,
		const FString& SurfaceKind,
		const FString& EvidenceFamily,
		const TArray<FMetadataTruthSurfaceItem>& Items)
	{
		TSharedPtr<FJsonObject> SurfaceObject = MakeShared<FJsonObject>();
		SurfaceObject->SetStringField(TEXT("name"), SurfaceName);
		SurfaceObject->SetStringField(TEXT("kind"), SurfaceKind);
		SurfaceObject->SetStringField(TEXT("evidence_family"), EvidenceFamily);
		SurfaceObject->SetNumberField(TEXT("item_count"), Items.Num());

		TArray<TSharedPtr<FJsonValue>> SampleItems;
		for (int32 Index = 0; Index < Items.Num() && Index < 8; ++Index)
		{
			const FMetadataTruthSurfaceItem& Item = Items[Index];
			TSharedPtr<FJsonObject> ItemObject = MakeShared<FJsonObject>();
			ItemObject->SetStringField(TEXT("item_id"), Item.SourceItemId);
			ItemObject->SetStringField(TEXT("display_name"), Item.DisplayName);
			ItemObject->SetStringField(TEXT("asset_package"), Item.AssetPackage);
			ItemObject->SetStringField(TEXT("raw_value"), Item.RawValue);
			ItemObject->SetBoolField(TEXT("package_normalized"), Item.bPackageNormalized);
			SetJsonStringArrayField(ItemObject, TEXT("evidence_basis"), Item.EvidenceBasis);
			SetJsonStringArrayField(ItemObject, TEXT("limitation_notes"), Item.LimitationNotes);
			SampleItems.Add(MakeShared<FJsonValueObject>(ItemObject));
		}
		SurfaceObject->SetArrayField(TEXT("sample_items"), SampleItems);
		return SurfaceObject;
	}

	TSharedPtr<FJsonObject> BuildComparisonItemJson(
		const FMetadataTruthComparisonItem& Item,
		const FString& DeclarationSurfaceName,
		const FString& ShippingSurfaceName)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("comparison_key"), Item.ComparisonKey);
		Object->SetStringField(TEXT("item_id"), Item.ItemId);
		Object->SetStringField(TEXT("display_name"), Item.DisplayName);
		Object->SetStringField(TEXT("asset_package"), Item.AssetPackage);
		Object->SetStringField(TEXT("consistency_bucket"), Item.ConsistencyBucket);
		Object->SetStringField(TEXT("confidence"), Item.Confidence);
		Object->SetBoolField(TEXT("declaration_present"), Item.bDeclarationPresent);
		Object->SetBoolField(TEXT("shipping_present"), Item.bShippingPresent);

		TArray<FString> ComparedSurfaces;
		if (Item.bDeclarationPresent)
		{
			ComparedSurfaces.Add(DeclarationSurfaceName);
		}
		if (Item.bShippingPresent)
		{
			ComparedSurfaces.Add(ShippingSurfaceName);
		}
		if (Item.bDiscoveryChecked)
		{
			ComparedSurfaces.Add(TEXT("current_package_presence"));
		}
		SetJsonStringArrayField(Object, TEXT("compared_surfaces"), ComparedSurfaces);
		SetJsonStringArrayField(Object, TEXT("declaration_source_ids"), Item.DeclarationSourceIds);
		SetJsonStringArrayField(Object, TEXT("shipping_source_ids"), Item.ShippingSourceIds);
		SetJsonStringArrayField(Object, TEXT("raw_values"), Item.RawValues);
		SetJsonStringArrayField(Object, TEXT("evidence_basis"), Item.EvidenceBasis);
		SetJsonStringArrayField(Object, TEXT("limitation_notes"), Item.LimitationNotes);

		TSharedPtr<FJsonObject> DiscoveryObject = MakeShared<FJsonObject>();
		DiscoveryObject->SetBoolField(TEXT("checked"), Item.bDiscoveryChecked);
		DiscoveryObject->SetBoolField(TEXT("present"), Item.bDiscoveryPresent);
		DiscoveryObject->SetBoolField(TEXT("file_present"), Item.bFilePresent);
		DiscoveryObject->SetBoolField(TEXT("asset_registry_present"), Item.bAssetRegistryPresent);
		DiscoveryObject->SetStringField(TEXT("resolved_filename"), Item.ResolvedFilename);
		Object->SetObjectField(TEXT("current_discovery"), DiscoveryObject);

		TSharedPtr<FJsonObject> RecommendedStep = MakeShared<FJsonObject>();
		RecommendedStep->SetStringField(TEXT("step_class"), Item.RecommendedStepClass);
		RecommendedStep->SetStringField(TEXT("reason"), Item.RecommendedStepReason);
		Object->SetObjectField(TEXT("recommended_next_step"), RecommendedStep);
		return Object;
	}

	FString BuildSummaryMessage(const TMap<FString, int32>& BucketCounts, const int32 ItemCount)
	{
		auto CountOrZero = [&BucketCounts](const FString& Bucket) -> int32
		{
			if (const int32* Count = BucketCounts.Find(Bucket))
			{
				return *Count;
			}
			return 0;
		};

		return FString::Printf(
			TEXT("Compared %d asset truth items | consistent=%d | advertised_but_not_shipped=%d | shipped_but_unadvertised=%d | advertised_but_not_currently_discovered=%d | uncertain=%d"),
			ItemCount,
			CountOrZero(TEXT("consistent")),
			CountOrZero(TEXT("advertised_but_not_shipped")),
			CountOrZero(TEXT("shipped_but_unadvertised")),
			CountOrZero(TEXT("advertised_but_not_currently_discovered")),
			CountOrZero(TEXT("uncertain")));
	}

	FString JoinShortList(const TArray<FString>& Values)
	{
		if (Values.Num() == 0)
		{
			return TEXT("none");
		}

		if (Values.Num() <= 4)
		{
			return FString::Join(Values, TEXT(", "));
		}

		TArray<FString> ShortValues;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			ShortValues.Add(Values[Index]);
		}
		return FString::Join(ShortValues, TEXT(", ")) + FString::Printf(TEXT(", ... +%d more"), Values.Num() - 4);
	}

	FString BuildMarkdownReport(
		const FString& DeclarationSurfaceName,
		const FString& DeclarationSurfaceKind,
		const FString& ShippingSurfaceName,
		const FString& ShippingSurfaceKind,
		const TMap<FString, int32>& BucketCounts,
		const TArray<FMetadataTruthComparisonItem>& Items)
	{
		auto CountOrZero = [&BucketCounts](const FString& Bucket) -> int32
		{
			if (const int32* Count = BucketCounts.Find(Bucket))
			{
				return *Count;
			}
			return 0;
		};

		FString Markdown = TEXT("# Metadata Truth Comparison\n\n");
		Markdown += TEXT("This report compares declaration metadata against a selected shipping surface and current package discovery.\n\n");
		Markdown += FString::Printf(TEXT("- Declaration Surface: `%s` (`%s`)\n"), *DeclarationSurfaceName, *DeclarationSurfaceKind);
		Markdown += FString::Printf(TEXT("- Shipping Surface: `%s` (`%s`)\n"), *ShippingSurfaceName, *ShippingSurfaceKind);
		Markdown += TEXT("- Current Discovery Surface: `current_package_presence` (`package/file + asset-registry presence`)\n");
		Markdown += TEXT("- Truth Boundary: current package discovery is not runtime proof.\n");
		Markdown += TEXT("\n## Bucket Summary\n\n");
		Markdown += FString::Printf(TEXT("- consistent: `%d`\n"), CountOrZero(TEXT("consistent")));
		Markdown += FString::Printf(TEXT("- advertised_but_not_shipped: `%d`\n"), CountOrZero(TEXT("advertised_but_not_shipped")));
		Markdown += FString::Printf(TEXT("- shipped_but_unadvertised: `%d`\n"), CountOrZero(TEXT("shipped_but_unadvertised")));
		Markdown += FString::Printf(TEXT("- advertised_but_not_currently_discovered: `%d`\n"), CountOrZero(TEXT("advertised_but_not_currently_discovered")));
		Markdown += FString::Printf(TEXT("- uncertain: `%d`\n"), CountOrZero(TEXT("uncertain")));
		Markdown += TEXT("\n## Compared Items\n\n");

		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			const FMetadataTruthComparisonItem& Item = Items[Index];
			Markdown += FString::Printf(TEXT("%d. `%s`\n"), Index + 1, Item.AssetPackage.IsEmpty() ? *Item.ItemId : *Item.AssetPackage);
			Markdown += FString::Printf(TEXT("   - Bucket: `%s`\n"), *Item.ConsistencyBucket);
			Markdown += FString::Printf(TEXT("   - Declaration Present: `%s`\n"), Item.bDeclarationPresent ? TEXT("true") : TEXT("false"));
			Markdown += FString::Printf(TEXT("   - Shipping Present: `%s`\n"), Item.bShippingPresent ? TEXT("true") : TEXT("false"));
			Markdown += FString::Printf(TEXT("   - Discovery Present: `%s`\n"), Item.bDiscoveryPresent ? TEXT("true") : TEXT("false"));
			Markdown += FString::Printf(TEXT("   - Confidence: `%s`\n"), *Item.Confidence);
			Markdown += FString::Printf(TEXT("   - Evidence Basis: `%s`\n"), *JoinShortList(Item.EvidenceBasis));
			Markdown += FString::Printf(TEXT("   - Next Step: `%s` - %s\n"), *Item.RecommendedStepClass, *Item.RecommendedStepReason);
			if (Item.LimitationNotes.Num() > 0)
			{
				Markdown += FString::Printf(TEXT("   - Limitations: `%s`\n"), *JoinShortList(Item.LimitationNotes));
			}
			Markdown += TEXT("\n");
		}

		return Markdown;
	}

	FOsvayderUEReportTruthSummary BuildTruthSummary(const TArray<FMetadataTruthComparisonItem>& Items)
	{
		FOsvayderUEReportTruthSummary TruthSummary;
		TruthSummary.PracticallyVerified.Add(TEXT("A bounded read-only metadata truth comparison ran across declaration metadata, a shipping surface, and current package discovery."));
		TruthSummary.Inspected.Add(TEXT("Current discovery uses package/file and asset-registry presence checks rather than runtime behavior."));

		bool bSawMismatch = false;
		for (const FMetadataTruthComparisonItem& Item : Items)
		{
			if (Item.ConsistencyBucket != TEXT("consistent"))
			{
				bSawMismatch = true;
				break;
			}
		}

		if (bSawMismatch)
		{
			TruthSummary.PracticallyVerified.Add(TEXT("The comparison surfaced at least one bounded metadata truth mismatch bucket in machine-readable form."));
		}

		TruthSummary.Limited.Add(TEXT("Current package discovery is not runtime proof and does not prove travel, load order, or gameplay behavior."));
		TruthSummary.NotVerified.Add(TEXT("This report does not prove runtime correctness; it only compares declaration, shipping, and current discovery evidence."));
		return TruthSummary;
	}

	TSharedPtr<FJsonObject> BuildExtraMetadata(
		const FString& DeclarationSurfaceName,
		const FString& DeclarationSurfaceKind,
		const FString& ShippingSurfaceName,
		const FString& ShippingSurfaceKind,
		const TMap<FString, int32>& BucketCounts)
	{
		TSharedPtr<FJsonObject> ExtraMetadata = MakeShared<FJsonObject>();
		ExtraMetadata->SetStringField(TEXT("declaration_surface_name"), DeclarationSurfaceName);
		ExtraMetadata->SetStringField(TEXT("declaration_surface_kind"), DeclarationSurfaceKind);
		ExtraMetadata->SetStringField(TEXT("shipping_surface_name"), ShippingSurfaceName);
		ExtraMetadata->SetStringField(TEXT("shipping_surface_kind"), ShippingSurfaceKind);

		TSharedPtr<FJsonObject> BucketObject = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : BucketCounts)
		{
			BucketObject->SetNumberField(Pair.Key, Pair.Value);
		}
		ExtraMetadata->SetObjectField(TEXT("bucket_counts"), BucketObject);
		return ExtraMetadata;
	}
}

FMCPToolResult FMCPTool_MetadataTruth::Execute(const TSharedRef<FJsonObject>& Params)
{
	const FString Operation = ExtractOptionalString(Params, TEXT("operation"), TEXT("compare_asset_truth"));
	if (!Operation.Equals(TEXT("compare_asset_truth"), ESearchCase::IgnoreCase))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Unsupported operation '%s'. Supported: compare_asset_truth"), *Operation));
	}

	const FString DeclarationSurfaceKind = ExtractOptionalString(Params, TEXT("declaration_surface_kind")).TrimStartAndEnd();
	const FString ShippingSurfaceKind = ExtractOptionalString(Params, TEXT("shipping_surface_kind")).TrimStartAndEnd();
	if (DeclarationSurfaceKind.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("declaration_surface_kind is required"));
	}
	if (ShippingSurfaceKind.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("shipping_surface_kind is required"));
	}

	const FString DeclarationSurfaceName = ExtractOptionalString(Params, TEXT("metadata_surface_name"), DeclarationSurfaceKind);
	const FString ShippingSurfaceName = ExtractOptionalString(Params, TEXT("shipping_surface_name"), ShippingSurfaceKind);
	const bool bIncludeUnadvertisedShipped = ExtractOptionalBool(Params, TEXT("include_unadvertised_shipped"), true);
	const bool bExportReport = ExtractOptionalBool(Params, TEXT("export_report"), false);
	const FString CustomReportName = ExtractOptionalString(Params, TEXT("report_name"));
	const FString CustomReportSlug = ExtractOptionalString(Params, TEXT("report_slug"));

	TArray<FMetadataTruthSurfaceItem> DeclarationItems;
	if (DeclarationSurfaceKind.Equals(TEXT("explicit_assets"), ESearchCase::IgnoreCase))
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonItems = nullptr;
		if (!Params->TryGetArrayField(TEXT("declaration_items"), JsonItems) || !JsonItems)
		{
			return FMCPToolResult::Error(TEXT("declaration_items array is required when declaration_surface_kind=explicit_assets"));
		}

		for (const TSharedPtr<FJsonValue>& Value : *JsonItems)
		{
			FMetadataTruthSurfaceItem Item;
			FString AssetToken;

			if (!Value.IsValid())
			{
				continue;
			}

			if (Value->Type == EJson::String)
			{
				AssetToken = Value->AsString();
				Item.SourceItemId = AssetToken;
				Item.DisplayName = AssetToken;
			}
			else if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> ItemObject = Value->AsObject();
				if (!ItemObject.IsValid())
				{
					continue;
				}

				ItemObject->TryGetStringField(TEXT("asset_path"), AssetToken);
				ItemObject->TryGetStringField(TEXT("item_id"), Item.SourceItemId);
				ItemObject->TryGetStringField(TEXT("display_name"), Item.DisplayName);
			}
			else
			{
				continue;
			}

			Item.RawValue = AssetToken;
			if (Item.SourceItemId.IsEmpty())
			{
				Item.SourceItemId = !AssetToken.IsEmpty() ? AssetToken : TEXT("explicit_item");
			}
			if (Item.DisplayName.IsEmpty())
			{
				Item.DisplayName = Item.SourceItemId;
			}

			Item.AssetPackage = NormalizePackagePath(AssetToken);
			Item.bPackageNormalized = !Item.AssetPackage.IsEmpty();
			AddUniqueLimited(Item.EvidenceBasis, TEXT("declaration_surface:explicit_assets"), MaxEvidenceEntries);
			if (!Item.AssetPackage.IsEmpty())
			{
				AddUniqueLimited(Item.EvidenceBasis, FString::Printf(TEXT("declaration_asset_package:%s"), *Item.AssetPackage), MaxEvidenceEntries);
			}
			else
			{
				AddUniqueLimited(Item.LimitationNotes, TEXT("Explicit declaration item did not normalize into a valid long package path."), MaxEvidenceEntries);
			}
			DeclarationItems.Add(Item);
		}
	}
	else if (DeclarationSurfaceKind.Equals(TEXT("data_table_string_field"), ESearchCase::IgnoreCase))
	{
		const FString DataTablePath = ExtractOptionalString(Params, TEXT("data_table_path"));
		if (DataTablePath.TrimStartAndEnd().IsEmpty())
		{
			return FMCPToolResult::Error(TEXT("data_table_path is required when declaration_surface_kind=data_table_string_field"));
		}

		FString TableObjectPath = DataTablePath;
		if (!DataTablePath.Contains(TEXT(".")))
		{
			const FString PackagePath = NormalizePackagePath(DataTablePath);
			if (PackagePath.IsEmpty())
			{
				return FMCPToolResult::Error(TEXT("data_table_path must resolve to a valid DataTable asset path"));
			}
			const FString AssetName = GetPackageLeafName(PackagePath);
			TableObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
		}

		UDataTable* DataTable = LoadObject<UDataTable>(nullptr, *TableObjectPath);
		if (!DataTable)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load DataTable '%s'"), *TableObjectPath));
		}

		FString ResolvedFieldName;
		FString FieldError;
		const FProperty* FieldProperty = ResolveDataTableField(
			DataTable->GetRowStruct(),
			ExtractOptionalString(Params, TEXT("data_table_field_name")),
			ExtractOptionalString(Params, TEXT("data_table_field_name_contains")),
			ResolvedFieldName,
			FieldError);
		if (!FieldProperty)
		{
			return FMCPToolResult::Error(FieldError);
		}

		const FString PackageTemplate = ExtractOptionalString(Params, TEXT("data_table_package_template"));
		for (const TPair<FName, uint8*>& Pair : DataTable->GetRowMap())
		{
			FString RawValue;
			if (!ExtractStringLikePropertyValue(FieldProperty, Pair.Value, RawValue))
			{
				return FMCPToolResult::Error(FString::Printf(
					TEXT("DataTable field '%s' must be FString, FName, or FText for data_table_string_field mode"),
					*ResolvedFieldName));
			}

			FMetadataTruthSurfaceItem Item;
			Item.SourceItemId = Pair.Key.ToString();
			Item.DisplayName = Pair.Key.ToString();
			Item.RawValue = RawValue;
			Item.AssetPackage = BuildPackagePathFromTemplate(PackageTemplate, RawValue);
			Item.bPackageNormalized = !Item.AssetPackage.IsEmpty();
			AddUniqueLimited(Item.EvidenceBasis, FString::Printf(TEXT("declaration_surface:data_table:%s"), *TableObjectPath), MaxEvidenceEntries);
			AddUniqueLimited(Item.EvidenceBasis, FString::Printf(TEXT("declaration_row:%s"), *Pair.Key.ToString()), MaxEvidenceEntries);
			AddUniqueLimited(Item.EvidenceBasis, FString::Printf(TEXT("declaration_field:%s"), *ResolvedFieldName), MaxEvidenceEntries);
			if (!PackageTemplate.TrimStartAndEnd().IsEmpty())
			{
				AddUniqueLimited(Item.EvidenceBasis, FString::Printf(TEXT("declaration_package_template:%s"), *PackageTemplate), MaxEvidenceEntries);
			}
			if (!Item.AssetPackage.IsEmpty())
			{
				AddUniqueLimited(Item.EvidenceBasis, FString::Printf(TEXT("declaration_asset_package:%s"), *Item.AssetPackage), MaxEvidenceEntries);
			}
			else
			{
				AddUniqueLimited(Item.LimitationNotes, TEXT("DataTable row value did not normalize into a valid long package path."), MaxEvidenceEntries);
			}
			DeclarationItems.Add(Item);
		}
	}
	else
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Unsupported declaration_surface_kind '%s'. Supported: explicit_assets, data_table_string_field"),
			*DeclarationSurfaceKind));
	}

	FMetadataTruthConfigSnapshot ConfigSnapshot = LoadConfigSnapshot();
	TArray<FMetadataTruthSurfaceItem> ShippingItems;
	if (ShippingSurfaceKind.Equals(TEXT("maps_to_cook"), ESearchCase::IgnoreCase))
	{
		for (const FString& MapPath : ConfigSnapshot.MapsToCook)
		{
			FMetadataTruthSurfaceItem Item;
			Item.SourceItemId = MapPath;
			Item.DisplayName = GetPackageLeafName(MapPath);
			Item.AssetPackage = NormalizePackagePath(MapPath);
			Item.RawValue = MapPath;
			Item.bPackageNormalized = !Item.AssetPackage.IsEmpty();
			AddUniqueLimited(Item.EvidenceBasis, TEXT("shipping_surface:maps_to_cook"), MaxEvidenceEntries);
			AddUniqueLimited(Item.EvidenceBasis, FString::Printf(TEXT("shipping_asset_package:%s"), *Item.AssetPackage), MaxEvidenceEntries);
			ShippingItems.Add(Item);
		}
	}
	else if (ShippingSurfaceKind.Equals(TEXT("explicit_assets"), ESearchCase::IgnoreCase))
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonItems = nullptr;
		if (!Params->TryGetArrayField(TEXT("shipping_items"), JsonItems) || !JsonItems)
		{
			return FMCPToolResult::Error(TEXT("shipping_items array is required when shipping_surface_kind=explicit_assets"));
		}

		for (const TSharedPtr<FJsonValue>& Value : *JsonItems)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			FMetadataTruthSurfaceItem Item;
			FString AssetToken;
			if (Value->Type == EJson::String)
			{
				AssetToken = Value->AsString();
				Item.SourceItemId = AssetToken;
				Item.DisplayName = AssetToken;
			}
			else if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> ItemObject = Value->AsObject();
				if (!ItemObject.IsValid())
				{
					continue;
				}

				ItemObject->TryGetStringField(TEXT("asset_path"), AssetToken);
				ItemObject->TryGetStringField(TEXT("item_id"), Item.SourceItemId);
				ItemObject->TryGetStringField(TEXT("display_name"), Item.DisplayName);
			}
			else
			{
				continue;
			}

			Item.RawValue = AssetToken;
			if (Item.SourceItemId.IsEmpty())
			{
				Item.SourceItemId = !AssetToken.IsEmpty() ? AssetToken : TEXT("shipping_item");
			}
			if (Item.DisplayName.IsEmpty())
			{
				Item.DisplayName = Item.SourceItemId;
			}

			Item.AssetPackage = NormalizePackagePath(AssetToken);
			Item.bPackageNormalized = !Item.AssetPackage.IsEmpty();
			AddUniqueLimited(Item.EvidenceBasis, TEXT("shipping_surface:explicit_assets"), MaxEvidenceEntries);
			if (!Item.AssetPackage.IsEmpty())
			{
				AddUniqueLimited(Item.EvidenceBasis, FString::Printf(TEXT("shipping_asset_package:%s"), *Item.AssetPackage), MaxEvidenceEntries);
			}
			else
			{
				AddUniqueLimited(Item.LimitationNotes, TEXT("Explicit shipping item did not normalize into a valid long package path."), MaxEvidenceEntries);
			}
			ShippingItems.Add(Item);
		}
	}
	else
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Unsupported shipping_surface_kind '%s'. Supported: maps_to_cook, explicit_assets"),
			*ShippingSurfaceKind));
	}

	TMap<FString, FMetadataTruthComparisonItem> ItemsByKey;
	TArray<FMetadataTruthComparisonItem> UncertainItems;
	int32 UncertainSequence = 0;

	auto FindOrAddItem = [&ItemsByKey](const FString& Key, const FString& FallbackDisplayName) -> FMetadataTruthComparisonItem&
	{
		FMetadataTruthComparisonItem& Item = ItemsByKey.FindOrAdd(Key);
		if (Item.ComparisonKey.IsEmpty())
		{
			Item.ComparisonKey = Key;
		}
		if (Item.ItemId.IsEmpty())
		{
			Item.ItemId = Key;
		}
		if (Item.DisplayName.IsEmpty())
		{
			Item.DisplayName = FallbackDisplayName;
		}
		if (Item.AssetPackage.IsEmpty())
		{
			Item.AssetPackage = Key;
		}
		return Item;
	};

	for (const FMetadataTruthSurfaceItem& SurfaceItem : DeclarationItems)
	{
		if (!SurfaceItem.AssetPackage.IsEmpty())
		{
			FMetadataTruthComparisonItem& Item = FindOrAddItem(SurfaceItem.AssetPackage, SurfaceItem.DisplayName);
			Item.bDeclarationPresent = true;
			Item.ItemId = SurfaceItem.SourceItemId;
			Item.DisplayName = SurfaceItem.DisplayName;
			Item.AssetPackage = SurfaceItem.AssetPackage;
			AddUniqueLimited(Item.DeclarationSourceIds, SurfaceItem.SourceItemId, MaxEvidenceEntries);
			AddUniqueLimited(Item.RawValues, SurfaceItem.RawValue, MaxEvidenceEntries);
			AddItemEvidence(Item, SurfaceItem);
		}
		else
		{
			FMetadataTruthComparisonItem Item;
			Item.ComparisonKey = FString::Printf(TEXT("uncertain_declaration_%d"), ++UncertainSequence);
			Item.ItemId = SurfaceItem.SourceItemId;
			Item.DisplayName = SurfaceItem.DisplayName;
			Item.AssetPackage = SurfaceItem.AssetPackage;
			Item.bDeclarationPresent = true;
			AddUniqueLimited(Item.DeclarationSourceIds, SurfaceItem.SourceItemId, MaxEvidenceEntries);
			AddUniqueLimited(Item.RawValues, SurfaceItem.RawValue, MaxEvidenceEntries);
			AddItemEvidence(Item, SurfaceItem);
			FinalizeComparisonItem(Item);
			UncertainItems.Add(Item);
		}
	}

	for (const FMetadataTruthSurfaceItem& SurfaceItem : ShippingItems)
	{
		if (!SurfaceItem.AssetPackage.IsEmpty())
		{
			FMetadataTruthComparisonItem& Item = FindOrAddItem(SurfaceItem.AssetPackage, SurfaceItem.DisplayName);
			Item.bShippingPresent = true;
			if (Item.DisplayName.IsEmpty())
			{
				Item.DisplayName = SurfaceItem.DisplayName;
			}
			Item.AssetPackage = SurfaceItem.AssetPackage;
			AddUniqueLimited(Item.ShippingSourceIds, SurfaceItem.SourceItemId, MaxEvidenceEntries);
			AddUniqueLimited(Item.RawValues, SurfaceItem.RawValue, MaxEvidenceEntries);
			AddItemEvidence(Item, SurfaceItem);
		}
		else
		{
			FMetadataTruthComparisonItem Item;
			Item.ComparisonKey = FString::Printf(TEXT("uncertain_shipping_%d"), ++UncertainSequence);
			Item.ItemId = SurfaceItem.SourceItemId;
			Item.DisplayName = SurfaceItem.DisplayName;
			Item.AssetPackage = SurfaceItem.AssetPackage;
			Item.bShippingPresent = true;
			AddUniqueLimited(Item.ShippingSourceIds, SurfaceItem.SourceItemId, MaxEvidenceEntries);
			AddUniqueLimited(Item.RawValues, SurfaceItem.RawValue, MaxEvidenceEntries);
			AddItemEvidence(Item, SurfaceItem);
			FinalizeComparisonItem(Item);
			UncertainItems.Add(Item);
		}
	}

	TArray<FMetadataTruthComparisonItem> Items;
	Items.Reserve(ItemsByKey.Num() + UncertainItems.Num());
	for (TPair<FString, FMetadataTruthComparisonItem>& Pair : ItemsByKey)
	{
		FMetadataTruthComparisonItem& Item = Pair.Value;
		if (!bIncludeUnadvertisedShipped && !Item.bDeclarationPresent && Item.bShippingPresent)
		{
			continue;
		}

		Item.bDiscoveryChecked = !Item.AssetPackage.IsEmpty();
		if (Item.bDiscoveryChecked)
		{
			Item.bDiscoveryPresent = DoesPackageExistNow(Item.AssetPackage, Item.bFilePresent, Item.bAssetRegistryPresent, Item.ResolvedFilename);
			AddUniqueLimited(Item.EvidenceBasis, TEXT("current_discovery:package_presence"), MaxEvidenceEntries);
			if (Item.bFilePresent)
			{
				AddUniqueLimited(Item.EvidenceBasis, TEXT("current_discovery:file_exists"), MaxEvidenceEntries);
			}
			if (Item.bAssetRegistryPresent)
			{
				AddUniqueLimited(Item.EvidenceBasis, TEXT("current_discovery:asset_registry_presence"), MaxEvidenceEntries);
			}
		}

		FinalizeComparisonItem(Item);
		Items.Add(Item);
	}

	for (const FMetadataTruthComparisonItem& Item : UncertainItems)
	{
		Items.Add(Item);
	}

	Algo::Sort(Items, [](const FMetadataTruthComparisonItem& A, const FMetadataTruthComparisonItem& B)
	{
		const FString ABucket = BucketSortKey(A.ConsistencyBucket);
		const FString BBucket = BucketSortKey(B.ConsistencyBucket);
		if (ABucket != BBucket)
		{
			return ABucket < BBucket;
		}

		const FString AKey = !A.AssetPackage.IsEmpty() ? A.AssetPackage : A.ItemId;
		const FString BKey = !B.AssetPackage.IsEmpty() ? B.AssetPackage : B.ItemId;
		return AKey < BKey;
	});

	TMap<FString, int32> BucketCounts;
	BucketCounts.Add(TEXT("consistent"), 0);
	BucketCounts.Add(TEXT("advertised_but_not_shipped"), 0);
	BucketCounts.Add(TEXT("shipped_but_unadvertised"), 0);
	BucketCounts.Add(TEXT("advertised_but_not_currently_discovered"), 0);
	BucketCounts.Add(TEXT("uncertain"), 0);

	TArray<TSharedPtr<FJsonValue>> ItemValues;
	ItemValues.Reserve(Items.Num());
	for (const FMetadataTruthComparisonItem& Item : Items)
	{
		if (int32* BucketCount = BucketCounts.Find(Item.ConsistencyBucket))
		{
			++(*BucketCount);
		}
		ItemValues.Add(MakeShared<FJsonValueObject>(BuildComparisonItemJson(Item, DeclarationSurfaceName, ShippingSurfaceName)));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("compare_asset_truth"));
	Data->SetStringField(TEXT("declaration_surface_name"), DeclarationSurfaceName);
	Data->SetStringField(TEXT("declaration_surface_kind"), DeclarationSurfaceKind);
	Data->SetStringField(TEXT("shipping_surface_name"), ShippingSurfaceName);
	Data->SetStringField(TEXT("shipping_surface_kind"), ShippingSurfaceKind);
	Data->SetStringField(TEXT("current_discovery_surface_name"), TEXT("current_package_presence"));
	Data->SetStringField(TEXT("current_discovery_surface_kind"), TEXT("package_presence"));
	Data->SetBoolField(TEXT("current_discovery_is_runtime_proof"), false);
	Data->SetArrayField(TEXT("items"), ItemValues);

	TSharedPtr<FJsonObject> BucketObject = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : BucketCounts)
	{
		BucketObject->SetNumberField(Pair.Key, Pair.Value);
	}
	Data->SetObjectField(TEXT("bucket_counts"), BucketObject);
	Data->SetObjectField(TEXT("declaration_surface"), BuildSurfaceJson(DeclarationSurfaceName, DeclarationSurfaceKind, TEXT("metadata_declaration"), DeclarationItems));
	Data->SetObjectField(TEXT("shipping_surface"), BuildSurfaceJson(ShippingSurfaceName, ShippingSurfaceKind, TEXT("shipping_config"), ShippingItems));

	TSharedPtr<FJsonObject> DiscoverySurface = MakeShared<FJsonObject>();
	DiscoverySurface->SetStringField(TEXT("name"), TEXT("current_package_presence"));
	DiscoverySurface->SetStringField(TEXT("kind"), TEXT("package_presence"));
	DiscoverySurface->SetStringField(TEXT("evidence_family"), TEXT("current_state_discovery"));
	DiscoverySurface->SetBoolField(TEXT("runtime_proof"), false);
	DiscoverySurface->SetStringField(TEXT("limitation_note"), TEXT("Current package discovery uses file/package and asset-registry presence only."));
	Data->SetObjectField(TEXT("current_discovery_surface"), DiscoverySurface);

	const FString Message = BuildSummaryMessage(BucketCounts, Items.Num());
	if (bExportReport)
	{
		FOsvayderUEReportExportRequest ExportRequest;
		ExportRequest.ReportName = !CustomReportName.TrimStartAndEnd().IsEmpty()
			? CustomReportName
			: FString::Printf(TEXT("Metadata Truth Comparison - %s vs %s"), *DeclarationSurfaceName, *ShippingSurfaceName);
		ExportRequest.ReportSlug = !CustomReportSlug.TrimStartAndEnd().IsEmpty()
			? CustomReportSlug
			: TEXT("metadata_truth_comparison");
		ExportRequest.Markdown = BuildMarkdownReport(
			DeclarationSurfaceName,
			DeclarationSurfaceKind,
			ShippingSurfaceName,
			ShippingSurfaceKind,
			BucketCounts,
			Items);
		ExportRequest.SummaryText = Message;
		ExportRequest.RunKind = TEXT("compare_asset_truth");
		ExportRequest.ExecutionMode = TEXT("read_only");
		ExportRequest.ToolNames = { TEXT("metadata_truth") };
		ExportRequest.ToolFamilies = { TEXT("metadata_truth") };
		ExportRequest.EvidenceClasses = { TEXT("metadata_declaration"), TEXT("shipping_config"), TEXT("current_state_discovery") };
		ExportRequest.TruthSummary = BuildTruthSummary(Items);
		ExportRequest.ExtraMetadata = BuildExtraMetadata(
			DeclarationSurfaceName,
			DeclarationSurfaceKind,
			ShippingSurfaceName,
			ShippingSurfaceKind,
			BucketCounts);

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
		ArtifactObject->SetStringField(TEXT("status_tool"), TEXT("report_artifact_status"));
		Data->SetObjectField(TEXT("report_artifact"), ArtifactObject);
	}

	return FMCPToolResult::Success(Message, Data);
}
