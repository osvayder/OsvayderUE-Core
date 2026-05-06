// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_MechanicPreflight.h"

#include "Animation/AnimBlueprint.h"
#include "AnimationBlueprintUtils.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	struct FMechanicPreflightEvidence
	{
		FString File;
		int32 Line = 0;
		FString Text;
	};

	struct FMechanicPreflightKeyOwner
	{
		FString Key;
		FString Owner;
		FString Action;
		FMechanicPreflightEvidence Evidence;
	};

	FString NormalizePackagePath(const FString& Raw)
	{
		FString Value = Raw.TrimStartAndEnd();
		if (Value.Contains(TEXT(".")))
		{
			Value.Split(TEXT("."), &Value, nullptr);
		}
		return Value;
	}

	FString NormalizeObjectPath(const FString& RawPackagePath)
	{
		const FString PackagePath = NormalizePackagePath(RawPackagePath);
		if (!PackagePath.StartsWith(TEXT("/Game/")))
		{
			return RawPackagePath;
		}

		const FString AssetName = FPaths::GetBaseFilename(PackagePath);
		return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
	}

	FString NormalizeInputToken(const FString& Raw)
	{
		FString Value = Raw.TrimStartAndEnd();
		Value.ReplaceInline(TEXT(" "), TEXT(""));
		Value.ReplaceInline(TEXT("_"), TEXT(""));
		Value = Value.ToLower();

		if (Value == TEXT("spacebar") || Value == TEXT("jump"))
		{
			return TEXT("Space");
		}
		if (Value == TEXT("shift") || Value == TEXT("leftshift"))
		{
			return TEXT("LeftShift");
		}
		if (Value == TEXT("rightshift"))
		{
			return TEXT("RightShift");
		}
		if (Value == TEXT("e"))
		{
			return TEXT("E");
		}
		if (Value == TEXT("t"))
		{
			return TEXT("T");
		}
		if (Value == TEXT("y"))
		{
			return TEXT("Y");
		}
		if (Value == TEXT("i"))
		{
			return TEXT("I");
		}
		if (Value == TEXT("c"))
		{
			return TEXT("C");
		}
		if (Value == TEXT("r"))
		{
			return TEXT("R");
		}

		return Raw.TrimStartAndEnd();
	}

	TArray<FString> ExtractStringArray(const TSharedRef<FJsonObject>& Params, const TCHAR* FieldName)
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* JsonValues = nullptr;
		if (!Params->TryGetArrayField(FieldName, JsonValues) || JsonValues == nullptr)
		{
			return Values;
		}

		for (const TSharedPtr<FJsonValue>& JsonValue : *JsonValues)
		{
			FString Value;
			if (JsonValue.IsValid() && JsonValue->TryGetString(Value) && !Value.TrimStartAndEnd().IsEmpty())
			{
				Values.Add(Value.TrimStartAndEnd());
			}
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> StringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	TSharedPtr<FJsonObject> EvidenceJson(const FMechanicPreflightEvidence& Evidence)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("source_file"), Evidence.File);
		Object->SetNumberField(TEXT("line"), Evidence.Line);
		Object->SetStringField(TEXT("evidence"), Evidence.Text);
		return Object;
	}

	TSharedPtr<FJsonObject> SourceFactJson(
		const FString& Fact,
		const FString& Status,
		const FMechanicPreflightEvidence& Evidence)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("fact"), Fact);
		Object->SetStringField(TEXT("status"), Status);
		Object->SetObjectField(TEXT("evidence"), EvidenceJson(Evidence));
		return Object;
	}

	TSharedPtr<FJsonObject> ConflictJson(
		const FString& Severity,
		const FString& Reason,
		const FString& Owner,
		const FMechanicPreflightEvidence& Evidence,
		const TArray<FString>& ProposedChoices)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("severity"), Severity);
		Object->SetStringField(TEXT("reason"), Reason);
		Object->SetStringField(TEXT("conflicting_owner"), Owner);
		Object->SetObjectField(TEXT("evidence"), EvidenceJson(Evidence));
		Object->SetArrayField(TEXT("proposed_choices"), StringArrayJson(ProposedChoices));
		return Object;
	}

	FString ProjectPath(const FString& RelativePath)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), RelativePath));
	}

	FString PackagePathToContentFile(const FString& PackagePath, const FString& Extension)
	{
		const FString Normalized = NormalizePackagePath(PackagePath);
		if (!Normalized.StartsWith(TEXT("/Game/")))
		{
			return FString();
		}

		FString Relative = Normalized;
		Relative.RightChopInline(6);
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Relative + Extension));
	}

	bool FileExists(const FString& Path)
	{
		return !Path.IsEmpty() && IFileManager::Get().FileExists(*Path);
	}

	FMechanicPreflightEvidence FindEvidenceInFile(const FString& FilePath, const TArray<FString>& Needles)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
		{
			return FMechanicPreflightEvidence{FilePath, 0, TEXT("source_file_unreadable")};
		}

		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			bool bAllNeedlesPresent = true;
			for (const FString& Needle : Needles)
			{
				if (!Lines[LineIndex].Contains(Needle, ESearchCase::IgnoreCase))
				{
					bAllNeedlesPresent = false;
					break;
				}
			}

			if (bAllNeedlesPresent)
			{
				return FMechanicPreflightEvidence{
					FilePath,
					LineIndex + 1,
					Lines[LineIndex].TrimStartAndEnd()
				};
			}
		}

		return FMechanicPreflightEvidence{FilePath, 0, FString::Printf(TEXT("not_found: %s"), *FString::Join(Needles, TEXT(" + ")))};
	}

	FMechanicPreflightEvidence FindFirstEvidence(
		const TArray<FString>& RelativeFiles,
		const TArray<FString>& Needles)
	{
		for (const FString& RelativeFile : RelativeFiles)
		{
			const FString Path = ProjectPath(RelativeFile);
			const FMechanicPreflightEvidence Evidence = FindEvidenceInFile(Path, Needles);
			if (Evidence.Line > 0)
			{
				return Evidence;
			}
		}

		return FMechanicPreflightEvidence{
			RelativeFiles.Num() > 0 ? ProjectPath(RelativeFiles[0]) : FPaths::ProjectDir(),
			0,
			FString::Printf(TEXT("not_found: %s"), *FString::Join(Needles, TEXT(" + ")))
		};
	}

	bool BinaryFileContains(const FString& FilePath, const FString& Needle)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FilePath) || Bytes.Num() == 0)
		{
			return false;
		}

		FTCHARToUTF8 NeedleUtf8(*Needle);
		const uint8* NeedleBytes = reinterpret_cast<const uint8*>(NeedleUtf8.Get());
		const int32 NeedleLen = NeedleUtf8.Length();
		if (NeedleLen <= 0 || Bytes.Num() < NeedleLen)
		{
			return false;
		}

		for (int32 Index = 0; Index <= Bytes.Num() - NeedleLen; ++Index)
		{
			if (FMemory::Memcmp(Bytes.GetData() + Index, NeedleBytes, NeedleLen) == 0)
			{
				return true;
			}
		}
		return false;
	}

	void AddOwner(TArray<FMechanicPreflightKeyOwner>& Owners, const FString& Key, const FString& Owner, const FString& Action, const FMechanicPreflightEvidence& Evidence)
	{
		FMechanicPreflightKeyOwner Row;
		Row.Key = Key;
		Row.Owner = Owner;
		Row.Action = Action;
		Row.Evidence = Evidence;
		Owners.Add(Row);
	}

	void AddSourceFact(TArray<TSharedPtr<FJsonValue>>& Facts, const FString& Fact, const FString& Status, const FMechanicPreflightEvidence& Evidence)
	{
		Facts.Add(MakeShared<FJsonValueObject>(SourceFactJson(Fact, Status, Evidence)));
	}

	bool IsCombatMap(const FString& TargetMap)
	{
		return NormalizePackagePath(TargetMap).Contains(TEXT("Variant_Combat"), ESearchCase::IgnoreCase);
	}

	bool IsPlatformingMap(const FString& TargetMap)
	{
		return NormalizePackagePath(TargetMap).Contains(TEXT("Variant_Platforming"), ESearchCase::IgnoreCase);
	}

	bool HasRequestedKey(const TArray<FString>& NormalizedInputs, const FString& Key)
	{
		return NormalizedInputs.ContainsByPredicate([&Key](const FString& Value)
		{
			return Value.Equals(Key, ESearchCase::IgnoreCase);
		});
	}

	bool IsWallRunMechanic(const FString& MechanicName)
	{
		const FString Lower = MechanicName.ToLower();
		return Lower.Contains(TEXT("wallrun")) || Lower.Contains(TEXT("wall_run")) || Lower.Contains(TEXT("wall run")) || Lower.Contains(TEXT("parkour"));
	}

	bool IsTelekinesisMechanic(const FString& MechanicName)
	{
		return MechanicName.Contains(TEXT("telekinesis"), ESearchCase::IgnoreCase);
	}

	bool IsPossessionMechanic(const FString& MechanicName)
	{
		return MechanicName.Contains(TEXT("possession"), ESearchCase::IgnoreCase)
			|| MechanicName.Contains(TEXT("possess"), ESearchCase::IgnoreCase);
	}

	FString BuildReportPath(const FString& ReportSlug)
	{
		FString Slug = ReportSlug.TrimStartAndEnd();
		if (Slug.IsEmpty())
		{
			Slug = TEXT("mechanic_preflight_report");
		}

		const TCHAR* InvalidChars = TEXT("\\/:*?\"<>| ");
		for (const TCHAR* Cursor = InvalidChars; *Cursor; ++Cursor)
		{
			Slug.ReplaceCharInline(*Cursor, TCHAR('_'));
		}

		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("OsvayderUE"),
			TEXT("mechanic_preflight"),
			Slug + TEXT(".json")));
	}

	bool SaveReport(const TSharedPtr<FJsonObject>& Report, const FString& ReportPath, FString& OutError)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);

		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Report.ToSharedRef(), Writer))
		{
			OutError = TEXT("failed_to_serialize_report");
			return false;
		}

		if (!FFileHelper::SaveStringToFile(JsonText, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("failed_to_write_report:%s"), *ReportPath);
			return false;
		}

		return true;
	}
}

FMCPToolResult FMCPTool_MechanicPreflight::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}
	if (!Operation.Equals(TEXT("inspect"), ESearchCase::IgnoreCase))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Unknown operation: '%s'. Valid: inspect"), *Operation));
	}

	const FString TargetMap = ExtractOptionalString(Params, TEXT("target_map"), TEXT("/Game/ThirdPerson/Lvl_ThirdPerson"));
	const FString MechanicName = ExtractOptionalString(Params, TEXT("mechanic_name"), TEXT("unknown"));
	const FString TargetCharacterClass = ExtractOptionalString(Params, TEXT("target_character_class"));
	const FString TargetControllerClass = ExtractOptionalString(Params, TEXT("target_controller_class"));
	const bool bIncludeAnimationPreflight = ExtractOptionalBool(Params, TEXT("include_animation_preflight"), true);
	const bool bIncludeRuntimeMapProbe = ExtractOptionalBool(Params, TEXT("include_runtime_map_probe"), false);
	const bool bExportReport = ExtractOptionalBool(Params, TEXT("export_report"), false);
	const FString ReportSlug = ExtractOptionalString(Params, TEXT("report_slug"));

	TArray<FString> RequestedInputs = ExtractStringArray(Params, TEXT("requested_inputs"));
	TArray<FString> NormalizedInputs;
	for (const FString& Input : RequestedInputs)
	{
		NormalizedInputs.AddUnique(NormalizeInputToken(Input));
	}

	TArray<FString> RequestedAnimationRoles = ExtractStringArray(Params, TEXT("requested_animation_roles"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("success"), true);
	Data->SetBoolField(TEXT("read_only"), true);
	Data->SetBoolField(TEXT("mutates_assets"), false);
	Data->SetStringField(TEXT("operation"), TEXT("inspect"));
	Data->SetStringField(TEXT("target_map"), NormalizePackagePath(TargetMap));
	Data->SetStringField(TEXT("mechanic_name"), MechanicName);
	Data->SetArrayField(TEXT("requested_inputs"), StringArrayJson(RequestedInputs));
	Data->SetArrayField(TEXT("normalized_requested_inputs"), StringArrayJson(NormalizedInputs));
	Data->SetArrayField(TEXT("requested_animation_roles"), StringArrayJson(RequestedAnimationRoles));
	Data->SetStringField(TEXT("target_character_class"), TargetCharacterClass.IsEmpty() ? TEXT("unknown") : TargetCharacterClass);
	Data->SetStringField(TEXT("target_controller_class"), TargetControllerClass.IsEmpty() ? TEXT("unknown") : TargetControllerClass);

	const FString NormalizedTargetMap = NormalizePackagePath(TargetMap);
	const FString MapFile = PackagePathToContentFile(NormalizedTargetMap, TEXT(".umap"));
	const FString DefaultEnginePath = ProjectPath(TEXT("Config/DefaultEngine.ini"));

	TSharedPtr<FJsonObject> MapFacts = MakeShared<FJsonObject>();
	MapFacts->SetStringField(TEXT("target_map_file"), MapFile);
	const bool bTargetMapPackagePath = NormalizedTargetMap.StartsWith(TEXT("/Game/"));
	const bool bTargetMapExists = FileExists(MapFile);
	MapFacts->SetBoolField(TEXT("target_map_exists"), bTargetMapExists);
	MapFacts->SetBoolField(TEXT("target_map_package_path"), bTargetMapPackagePath);

	FString DefaultEngineText;
	FFileHelper::LoadFileToString(DefaultEngineText, *DefaultEnginePath);
	const bool bIsDefaultMap = DefaultEngineText.Contains(NormalizedTargetMap, ESearchCase::IgnoreCase);
	MapFacts->SetStringField(TEXT("default_engine_ini"), DefaultEnginePath);
	MapFacts->SetBoolField(TEXT("is_default_map"), bIsDefaultMap);
	MapFacts->SetStringField(TEXT("default_map_package"), DefaultEngineText.Contains(TEXT("/Game/ThirdPerson/Lvl_ThirdPerson"), ESearchCase::IgnoreCase)
		? TEXT("/Game/ThirdPerson/Lvl_ThirdPerson")
		: TEXT("unknown"));

	TArray<TSharedPtr<FJsonValue>> VariantFacts;
	const TArray<FString> KnownVariants = {
		TEXT("/Game/Variant_Combat/Lvl_Combat"),
		TEXT("/Game/Variant_Platforming/Lvl_Platforming"),
		TEXT("/Game/Variant_SideScrolling/Lvl_SideScrolling")
	};
	for (const FString& Variant : KnownVariants)
	{
		TSharedPtr<FJsonObject> VariantObj = MakeShared<FJsonObject>();
		VariantObj->SetStringField(TEXT("map"), Variant);
		VariantObj->SetStringField(TEXT("file"), PackagePathToContentFile(Variant, TEXT(".umap")));
		VariantObj->SetBoolField(TEXT("exists"), FileExists(PackagePathToContentFile(Variant, TEXT(".umap"))));
		VariantFacts.Add(MakeShared<FJsonValueObject>(VariantObj));
	}
	MapFacts->SetArrayField(TEXT("known_variant_maps"), VariantFacts);
	Data->SetObjectField(TEXT("map_facts"), MapFacts);

	const bool bCombat = IsCombatMap(NormalizedTargetMap);
	const bool bPlatforming = IsPlatformingMap(NormalizedTargetMap);
	const bool bSupportedTargetSurface = bCombat || bPlatforming;
	const TArray<FString> CombatSourceFiles = {
		TEXT("Source/Poligon1/Variant_Combat/CombatCharacter.cpp"),
		TEXT("Source/Poligon1/Variant_Combat/CombatCharacter.h"),
		TEXT("Source/Poligon1/Variant_Combat/CombatPlayerController.cpp"),
		TEXT("Source/Poligon1/Variant_Combat/CombatPlayerController.h")
	};
	const TArray<FString> PlatformingSourceFiles = {
		TEXT("Source/Poligon1/Variant_Platforming/PlatformingCharacter.cpp"),
		TEXT("Source/Poligon1/Variant_Platforming/PlatformingCharacter.h"),
		TEXT("Source/Poligon1/Variant_Platforming/PlatformingPlayerController.cpp"),
		TEXT("Source/Poligon1/Variant_Platforming/PlatformingPlayerController.h")
	};
	const TArray<FString>& SourceFiles = bPlatforming ? PlatformingSourceFiles : CombatSourceFiles;

	TSharedPtr<FJsonObject> ClassFacts = MakeShared<FJsonObject>();
	bool bExpectedGameModeAssetExists = true;
	bool bExpectedCharacterAssetExists = true;
	bool bExpectedAnimBPAssetExists = true;
	if (bCombat)
	{
		bExpectedGameModeAssetExists = FileExists(PackagePathToContentFile(TEXT("/Game/Variant_Combat/Blueprints/BP_CombatGameMode"), TEXT(".uasset")));
		bExpectedCharacterAssetExists = FileExists(PackagePathToContentFile(TEXT("/Game/Variant_Combat/Blueprints/BP_CombatCharacter"), TEXT(".uasset")));
		bExpectedAnimBPAssetExists = FileExists(PackagePathToContentFile(TEXT("/Game/Variant_Combat/Anims/ABP_Manny_Combat"), TEXT(".uasset")));
		ClassFacts->SetStringField(TEXT("game_mode"), TEXT("/Game/Variant_Combat/Blueprints/BP_CombatGameMode"));
		ClassFacts->SetStringField(TEXT("pawn_or_character"), TEXT("ACombatCharacter / /Game/Variant_Combat/Blueprints/BP_CombatCharacter"));
		ClassFacts->SetStringField(TEXT("controller"), TEXT("ACombatPlayerController"));
		ClassFacts->SetStringField(TEXT("anim_bp"), TEXT("/Game/Variant_Combat/Anims/ABP_Manny_Combat"));
		ClassFacts->SetStringField(TEXT("determination"), TEXT("variant source files plus local Blueprint/AnimBP asset existence checks; map override not loaded"));
		ClassFacts->SetBoolField(TEXT("game_mode_asset_exists"), bExpectedGameModeAssetExists);
		ClassFacts->SetBoolField(TEXT("character_asset_exists"), bExpectedCharacterAssetExists);
		ClassFacts->SetBoolField(TEXT("anim_bp_asset_exists"), bExpectedAnimBPAssetExists);
	}
	else if (bPlatforming)
	{
		bExpectedGameModeAssetExists = FileExists(PackagePathToContentFile(TEXT("/Game/Variant_Platforming/Blueprints/BP_PlatformingGameMode"), TEXT(".uasset")));
		bExpectedCharacterAssetExists = FileExists(PackagePathToContentFile(TEXT("/Game/Variant_Platforming/Blueprints/BP_PlatformingCharacter"), TEXT(".uasset")));
		bExpectedAnimBPAssetExists = FileExists(PackagePathToContentFile(TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"), TEXT(".uasset")));
		ClassFacts->SetStringField(TEXT("game_mode"), TEXT("/Game/Variant_Platforming/Blueprints/BP_PlatformingGameMode"));
		ClassFacts->SetStringField(TEXT("pawn_or_character"), TEXT("APlatformingCharacter / /Game/Variant_Platforming/Blueprints/BP_PlatformingCharacter"));
		ClassFacts->SetStringField(TEXT("controller"), TEXT("APlatformingPlayerController"));
		ClassFacts->SetStringField(TEXT("anim_bp"), TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"));
		ClassFacts->SetStringField(TEXT("determination"), TEXT("variant source files plus local Blueprint/AnimBP asset existence checks; map override not loaded"));
		ClassFacts->SetBoolField(TEXT("game_mode_asset_exists"), bExpectedGameModeAssetExists);
		ClassFacts->SetBoolField(TEXT("character_asset_exists"), bExpectedCharacterAssetExists);
		ClassFacts->SetBoolField(TEXT("anim_bp_asset_exists"), bExpectedAnimBPAssetExists);
	}
	else
	{
		ClassFacts->SetStringField(TEXT("game_mode"), TEXT("unknown"));
		ClassFacts->SetStringField(TEXT("pawn_or_character"), TEXT("unknown"));
		ClassFacts->SetStringField(TEXT("controller"), TEXT("unknown"));
		ClassFacts->SetStringField(TEXT("anim_bp"), TEXT("unknown"));
		ClassFacts->SetStringField(TEXT("determination"), TEXT("unknown_map_surface"));
	}
	ClassFacts->SetBoolField(TEXT("expected_class_context_complete"), bExpectedGameModeAssetExists && bExpectedCharacterAssetExists && bExpectedAnimBPAssetExists);
	Data->SetObjectField(TEXT("class_facts"), ClassFacts);

	TSharedPtr<FJsonObject> InputFacts = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EnhancedInputAssets;
	bool bExpectedInputAssetsComplete = true;
	auto AddInputAsset = [&EnhancedInputAssets](const FString& PackagePath)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), PackagePath);
		const FString File = PackagePathToContentFile(PackagePath, TEXT(".uasset"));
		const bool bExists = FileExists(File);
		Obj->SetStringField(TEXT("file"), File);
		Obj->SetBoolField(TEXT("exists"), bExists);
		EnhancedInputAssets.Add(MakeShared<FJsonValueObject>(Obj));
		return bExists;
	};

	if (bCombat)
	{
		bExpectedInputAssetsComplete &= AddInputAsset(TEXT("/Game/Variant_Combat/Input/IMC_Combat"));
		bExpectedInputAssetsComplete &= AddInputAsset(TEXT("/Game/Variant_Combat/Input/Actions/IA_ComboAttack"));
		bExpectedInputAssetsComplete &= AddInputAsset(TEXT("/Game/Variant_Combat/Input/Actions/IA_ChargedAttack"));
		bExpectedInputAssetsComplete &= AddInputAsset(TEXT("/Game/Variant_Combat/Input/Actions/IA_ToggleCameraSide"));
	}
	else if (bPlatforming)
	{
		bExpectedInputAssetsComplete &= AddInputAsset(TEXT("/Game/Variant_Platforming/Input/IMC_Platforming"));
		bExpectedInputAssetsComplete &= AddInputAsset(TEXT("/Game/Variant_Platforming/Input/Actions/IA_Dash"));
		bExpectedInputAssetsComplete &= AddInputAsset(TEXT("/Game/Input/Actions/IA_Jump"));
		bExpectedInputAssetsComplete &= AddInputAsset(TEXT("/Game/Input/Actions/IA_Move"));
	}
	InputFacts->SetArrayField(TEXT("enhanced_input_assets"), EnhancedInputAssets);
	InputFacts->SetStringField(TEXT("enhanced_input_mapping_readback"), TEXT("best_effort_local_asset_and_source_scan; no mutation"));
	InputFacts->SetBoolField(TEXT("expected_input_assets_complete"), bExpectedInputAssetsComplete);
	Data->SetObjectField(TEXT("input_facts"), InputFacts);

	TArray<FMechanicPreflightKeyOwner> Owners;
	if (bCombat)
	{
		AddOwner(Owners, TEXT("I"), TEXT("ACombatPlayerController"), TEXT("ToggleInventory"), FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::I")}));
		AddOwner(Owners, TEXT("Y"), TEXT("ACombatPlayerController"), TEXT("ToggleCombatBodyPossession"), FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::Y"), TEXT("ToggleCombatBodyPossession")}));
		AddOwner(Owners, TEXT("E"), TEXT("ACombatCharacter"), TEXT("TelekinesisPressed/Released"), FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::E"), TEXT("Telekinesis")}));
		AddOwner(Owners, TEXT("T"), TEXT("ACombatCharacter"), TEXT("TelekinesisModeTogglePressed"), FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::T"), TEXT("TelekinesisModeTogglePressed")}));
		AddOwner(Owners, TEXT("Space"), TEXT("ACombatCharacter"), TEXT("DoJumpStart/DoJumpEnd: flight ascent and wall-run jump hold"), FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::SpaceBar"), TEXT("DoJumpStart")}));
		AddOwner(Owners, TEXT("LeftShift"), TEXT("ACombatCharacter"), TEXT("SprintPressed/SprintReleased: wall-run sprint hold"), FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::LeftShift"), TEXT("SprintPressed")}));
		AddOwner(Owners, TEXT("RightShift"), TEXT("ACombatCharacter"), TEXT("SprintPressed/SprintReleased: wall-run sprint hold"), FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::RightShift"), TEXT("SprintPressed")}));
		AddOwner(Owners, TEXT("C"), TEXT("ACombatCharacter"), TEXT("crouch or flight descend while flying"), FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::C")}));
		AddOwner(Owners, TEXT("R"), TEXT("ACombatCharacter"), TEXT("ReloadPressed"), FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::R"), TEXT("ReloadPressed")}));
	}
	else if (bPlatforming)
	{
		AddOwner(Owners, TEXT("Space"), TEXT("APlatformingCharacter"), TEXT("JumpAction -> DoJumpStart/DoJumpEnd: jump, double jump, wall jump, jump-hold wallrun"), FindFirstEvidence(PlatformingSourceFiles, {TEXT("BindAction(JumpAction"), TEXT("DoJumpStart")}));
		AddOwner(Owners, TEXT("LeftShift"), TEXT("APlatformingCharacter"), TEXT("IA_Dash/DashAction -> Dash/DoDash"), FindFirstEvidence(PlatformingSourceFiles, {TEXT("BindAction(DashAction"), TEXT("Dash")}));

		const FString PlatformingIMCFile = PackagePathToContentFile(TEXT("/Game/Variant_Platforming/Input/IMC_Platforming"), TEXT(".uasset"));
		if (BinaryFileContains(PlatformingIMCFile, TEXT("LeftShift")) && BinaryFileContains(PlatformingIMCFile, TEXT("IA_Dash")))
		{
			AddOwner(Owners, TEXT("LeftShift"), TEXT("IMC_Platforming"), TEXT("LeftShift maps to IA_Dash"), FMechanicPreflightEvidence{PlatformingIMCFile, 0, TEXT("binary asset contains LeftShift and IA_Dash")});
		}
	}

	TSharedPtr<FJsonObject> OccupiedKeys = MakeShared<FJsonObject>();
	for (const FMechanicPreflightKeyOwner& Owner : Owners)
	{
		TArray<TSharedPtr<FJsonValue>> Existing;
		const TArray<TSharedPtr<FJsonValue>>* ExistingPtr = nullptr;
		if (OccupiedKeys->TryGetArrayField(Owner.Key, ExistingPtr) && ExistingPtr != nullptr)
		{
			Existing = *ExistingPtr;
		}

		TSharedPtr<FJsonObject> OwnerObj = MakeShared<FJsonObject>();
		OwnerObj->SetStringField(TEXT("owner"), Owner.Owner);
		OwnerObj->SetStringField(TEXT("action"), Owner.Action);
		OwnerObj->SetObjectField(TEXT("evidence"), EvidenceJson(Owner.Evidence));
		Existing.Add(MakeShared<FJsonValueObject>(OwnerObj));
		OccupiedKeys->SetArrayField(Owner.Key, Existing);
	}
	Data->SetObjectField(TEXT("occupied_keys"), OccupiedKeys);

	TArray<TSharedPtr<FJsonValue>> MechanicFacts;
	TArray<TSharedPtr<FJsonValue>> ReplicationFacts;
	TArray<TSharedPtr<FJsonValue>> CameraFacts;

	if (bCombat)
	{
		AddSourceFact(MechanicFacts, TEXT("combat flight uses MOVE_Flying and bFlightInputHeld; Space triggers DoJumpStart"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("bFlightInputHeld")}));
		AddSourceFact(MechanicFacts, TEXT("combat wall-run exists and requires jump plus sprint hold"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("bWallRunJumpInputHeld"), TEXT("bWallRunSprintInputHeld")}));
		AddSourceFact(MechanicFacts, TEXT("combat telekinesis and enemy telekinesis exist"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("ActiveTelekinesisEnemy")}));
		AddSourceFact(MechanicFacts, TEXT("combat body possession exists on Y in ACombatPlayerController"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("ToggleCombatBodyPossession")}));
		AddSourceFact(MechanicFacts, TEXT("combat inventory exists and owns raw I in ACombatPlayerController"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("ToggleInventory")}));
		AddSourceFact(MechanicFacts, TEXT("combat crouch exists and shares C with flight descend while flying"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("CrouchPressedBinding")}));

		AddSourceFact(CameraFacts, TEXT("ACombatCharacter owns CameraBoom/FollowCamera and combat camera profile state"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("ApplyCombatCameraProfile")}));
		AddSourceFact(CameraFacts, TEXT("combat camera has flight/wallrun/crouch profile offsets"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("CombatCameraWallRun")}));

		AddSourceFact(ReplicationFacts, TEXT("ACombatCharacter replicates inventory, telekinesis mode, and wall-run state"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("DOREPLIFETIME"), TEXT("bIsWallRunning")}));
		AddSourceFact(ReplicationFacts, TEXT("telekinesis and wall-run use server RPCs"), TEXT("detected"), FindFirstEvidence(CombatSourceFiles, {TEXT("ServerSetWallRunInput")}));
		AddSourceFact(ReplicationFacts, TEXT("combat body possession Y path is authority-only and lacks client Server RPC start path"), TEXT("authority_caveat"), FindFirstEvidence(CombatSourceFiles, {TEXT("client_without_server_rpc")}));
	}
	else if (bPlatforming)
	{
		AddSourceFact(MechanicFacts, TEXT("platforming jump path includes jump hold, double jump, wall jump, and coyote time"), TEXT("detected"), FindFirstEvidence(PlatformingSourceFiles, {TEXT("MultiJump")}));
		AddSourceFact(MechanicFacts, TEXT("platforming dash exists and DashAction remains the dash path"), TEXT("detected"), FindFirstEvidence(PlatformingSourceFiles, {TEXT("DashAction"), TEXT("Dash")}));
		AddSourceFact(MechanicFacts, TEXT("platforming jump-hold wall-run exists without requiring Shift"), TEXT("detected"), FindFirstEvidence(PlatformingSourceFiles, {TEXT("Jump-hold owns wall-run")}));

		AddSourceFact(CameraFacts, TEXT("APlatformingCharacter owns CameraBoom/FollowCamera"), TEXT("detected"), FindFirstEvidence(PlatformingSourceFiles, {TEXT("CameraBoom")}));
		AddSourceFact(ReplicationFacts, TEXT("platforming source has no detected replicated wall-run properties or RPCs"), TEXT("local_only_or_unknown"), FindFirstEvidence(PlatformingSourceFiles, {TEXT("Replicated")}));
	}
	else
	{
		AddSourceFact(MechanicFacts, TEXT("mechanic facts for this map are unknown"), TEXT("unknown"), FMechanicPreflightEvidence{FPaths::ProjectDir(), 0, TEXT("unsupported_map_surface")});
		AddSourceFact(CameraFacts, TEXT("camera facts for this map are unknown"), TEXT("unknown"), FMechanicPreflightEvidence{FPaths::ProjectDir(), 0, TEXT("unsupported_map_surface")});
		AddSourceFact(ReplicationFacts, TEXT("replication facts for this map are unknown"), TEXT("unknown"), FMechanicPreflightEvidence{FPaths::ProjectDir(), 0, TEXT("unsupported_map_surface")});
	}
	Data->SetArrayField(TEXT("mechanic_facts"), MechanicFacts);
	Data->SetArrayField(TEXT("camera_facts"), CameraFacts);
	Data->SetArrayField(TEXT("replication_facts"), ReplicationFacts);

	TSharedPtr<FJsonObject> AnimationFacts = MakeShared<FJsonObject>();
	AnimationFacts->SetBoolField(TEXT("requested"), bIncludeAnimationPreflight && RequestedAnimationRoles.Num() > 0);
	AnimationFacts->SetBoolField(TEXT("read_only"), true);
	AnimationFacts->SetBoolField(TEXT("mutates_assets"), false);
	AnimationFacts->SetBoolField(TEXT("visual_proof"), false);
	AnimationFacts->SetBoolField(TEXT("production_ready_visual_claim"), false);
	AnimationFacts->SetStringField(TEXT("claim_boundary"), TEXT("local inventory/preflight only; no runtime, visual, or production-ready animation proof"));

	if (bIncludeAnimationPreflight && RequestedAnimationRoles.Num() > 0)
	{
		const FString AnimBPPath = bCombat
			? TEXT("/Game/Variant_Combat/Anims/ABP_Manny_Combat")
			: bPlatforming
				? TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming")
				: FString();
		const FString ActorBPPath = bCombat
			? TEXT("/Game/Variant_Combat/Blueprints/BP_CombatCharacter")
			: bPlatforming
				? TEXT("/Game/Variant_Platforming/Blueprints/BP_PlatformingCharacter")
				: FString();
		if (!AnimBPPath.IsEmpty())
		{
			UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *NormalizeObjectPath(AnimBPPath));
			UBlueprint* ActorBP = LoadObject<UBlueprint>(nullptr, *NormalizeObjectPath(ActorBPPath));
			FString PreflightError;
			TSharedPtr<FJsonObject> Preflight = FAnimationBlueprintUtils::BuildAnimationPreflightReport(
				AnimBP,
				RequestedAnimationRoles,
				ActorBP,
				FString(),
				5,
				FString(),
				FString(),
				PreflightError);
			if (Preflight.IsValid())
			{
				AnimationFacts->SetObjectField(TEXT("animation_preflight_summary"), Preflight);
			}
			else
			{
				AnimationFacts->SetStringField(TEXT("animation_preflight_error"), PreflightError.IsEmpty() ? TEXT("unknown_animation_preflight_error") : PreflightError);
			}
		}
		else
		{
			AnimationFacts->SetStringField(TEXT("animation_preflight_error"), TEXT("unknown_anim_bp_for_target_map"));
		}
	}
	Data->SetObjectField(TEXT("animation_facts"), AnimationFacts);

	TArray<TSharedPtr<FJsonValue>> Conflicts;
	TArray<TSharedPtr<FJsonValue>> RequiredChoices;
	bool bHasBlockingConflict = false;

	auto AddConflict = [&Conflicts, &RequiredChoices, &bHasBlockingConflict](
		const FString& Severity,
		const FString& Reason,
		const FString& Owner,
		const FMechanicPreflightEvidence& Evidence,
		const TArray<FString>& Choices)
	{
		Conflicts.Add(MakeShared<FJsonValueObject>(ConflictJson(Severity, Reason, Owner, Evidence, Choices)));
		if (Severity.Equals(TEXT("high"), ESearchCase::IgnoreCase) || Severity.Equals(TEXT("critical"), ESearchCase::IgnoreCase))
		{
			bHasBlockingConflict = true;
			TSharedPtr<FJsonObject> ChoiceObj = MakeShared<FJsonObject>();
			ChoiceObj->SetStringField(TEXT("conflict"), Reason);
			ChoiceObj->SetArrayField(TEXT("options"), StringArrayJson(Choices));
			RequiredChoices.Add(MakeShared<FJsonValueObject>(ChoiceObj));
		}
	};

	if (!bSupportedTargetSurface)
	{
		AddConflict(
			TEXT("high"),
			TEXT("Unsupported or unknown target map context; mechanic preflight could not determine the target GameMode, pawn/controller, input, movement, camera, replication, or animation surface."),
			TEXT("mechanic_preflight context integrity"),
			FMechanicPreflightEvidence{FPaths::ProjectDir(), 0, TEXT("unsupported_map_surface")},
			{TEXT("Select a supported target map such as /Game/Variant_Combat/Lvl_Combat or /Game/Variant_Platforming/Lvl_Platforming"), TEXT("Provide target_character_class and target_controller_class with source-backed context"), TEXT("Run a map/class discovery pass before mutation")});
	}

	if (bTargetMapPackagePath && !bTargetMapExists)
	{
		AddConflict(
			TEXT("high"),
			FString::Printf(TEXT("Target map file is missing for %s; mechanic preflight cannot verify the level context."), *NormalizedTargetMap),
			TEXT("mechanic_preflight map existence"),
			FMechanicPreflightEvidence{MapFile, 0, TEXT("missing_target_map_file")},
			{TEXT("Select an existing supported target map"), TEXT("Repair or restore the missing map asset"), TEXT("Run a map/class discovery pass before mutation")});
	}

	if (bSupportedTargetSurface && (!bExpectedGameModeAssetExists || !bExpectedCharacterAssetExists || !bExpectedAnimBPAssetExists))
	{
		AddConflict(
			TEXT("high"),
			TEXT("Known target map context is incomplete because expected GameMode, character, or AnimBP assets are missing."),
			TEXT("mechanic_preflight class context integrity"),
			FMechanicPreflightEvidence{FPaths::ProjectContentDir(), 0, TEXT("missing_expected_class_or_animation_asset")},
			{TEXT("Repair expected GameMode/character/AnimBP assets"), TEXT("Provide explicit target class/context override"), TEXT("Run a map/class discovery pass before mutation")});
	}

	if (bSupportedTargetSurface && !bExpectedInputAssetsComplete)
	{
		AddConflict(
			TEXT("high"),
			TEXT("Known target map input context is incomplete because expected Enhanced Input assets are missing."),
			TEXT("mechanic_preflight input context integrity"),
			FMechanicPreflightEvidence{FPaths::ProjectContentDir(), 0, TEXT("missing_expected_input_asset")},
			{TEXT("Repair expected Enhanced Input assets"), TEXT("Provide explicit input mapping context"), TEXT("Run an input discovery pass before mutation")});
	}

	if (bCombat && IsWallRunMechanic(MechanicName))
	{
		if (HasRequestedKey(NormalizedInputs, TEXT("Space")))
		{
			AddConflict(
				TEXT("critical"),
				TEXT("Combat wallrun requested on Space, but Space already drives flight ascent and wall-run jump hold on ACombatCharacter."),
				TEXT("ACombatCharacter flight / wall-run input"),
				FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::SpaceBar"), TEXT("DoJumpStart")}),
				{TEXT("Move parkour validation to Platforming jump-hold wallrun"), TEXT("Choose a non-Space start input"), TEXT("Provide explicit resolution for flight-vs-wallrun ownership")});
		}
		if (HasRequestedKey(NormalizedInputs, TEXT("LeftShift")) || HasRequestedKey(NormalizedInputs, TEXT("RightShift")))
		{
			AddConflict(
				TEXT("high"),
				TEXT("Combat wallrun requested on Shift while Shift is already the existing wall-run sprint-hold context and movement-state modifier."),
				TEXT("ACombatCharacter wall-run sprint input"),
				FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::LeftShift"), TEXT("SprintPressed")}),
				{TEXT("Do not add another Shift-owned mechanic without explicit resolution"), TEXT("Use Platforming jump-hold policy"), TEXT("Choose a non-Shift modifier")});
		}
	}

	if (bPlatforming && IsWallRunMechanic(MechanicName))
	{
		if (HasRequestedKey(NormalizedInputs, TEXT("LeftShift")) || HasRequestedKey(NormalizedInputs, TEXT("RightShift")))
		{
			AddConflict(
				TEXT("high"),
				TEXT("Platforming wallrun requested on Shift, but Shift/IA_Dash is already owned by DashAction."),
				TEXT("APlatformingCharacter DashAction / IMC_Platforming"),
				FindFirstEvidence(PlatformingSourceFiles, {TEXT("DashAction"), TEXT("Dash")}),
				{TEXT("Use Jump-hold wallrun and keep Shift as dash"), TEXT("Choose a non-Shift wallrun modifier"), TEXT("Make Dash an optional boost/exit only, not the start condition")});
		}
		else if (HasRequestedKey(NormalizedInputs, TEXT("Space")))
		{
			AddConflict(
				TEXT("info"),
				TEXT("Platforming Space/Jump already owns jump, double jump, wall jump, coyote time, and jump-hold wallrun context; no Shift conflict is blocking this request."),
				TEXT("APlatformingCharacter JumpAction"),
				FindFirstEvidence(PlatformingSourceFiles, {TEXT("BindAction(JumpAction"), TEXT("DoJumpStart")}),
				{TEXT("Preserve existing jump/double-jump/wall-jump/coyote-time behavior"), TEXT("Continue using jump-hold wallrun without Shift")});
		}
	}

	if (bCombat && (IsPossessionMechanic(MechanicName) || HasRequestedKey(NormalizedInputs, TEXT("Y"))))
	{
		AddConflict(
			HasRequestedKey(NormalizedInputs, TEXT("Y")) ? TEXT("high") : TEXT("info"),
			TEXT("Combat Y is owned by body possession in ACombatPlayerController."),
			TEXT("ACombatPlayerController body possession"),
			FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::Y"), TEXT("ToggleCombatBodyPossession")}),
			{TEXT("Reuse existing body possession flow"), TEXT("Choose a different key"), TEXT("Explicitly modify possession only in a separate mechanic packet")});
	}

	if (bCombat && (IsTelekinesisMechanic(MechanicName) || HasRequestedKey(NormalizedInputs, TEXT("E")) || HasRequestedKey(NormalizedInputs, TEXT("T"))))
	{
		AddConflict(
			HasRequestedKey(NormalizedInputs, TEXT("E")) || HasRequestedKey(NormalizedInputs, TEXT("T")) ? TEXT("high") : TEXT("info"),
			TEXT("Combat E/T are owned by telekinesis start/release and telekinesis mode toggle; enemy telekinesis already exists."),
			TEXT("ACombatCharacter telekinesis / enemy telekinesis"),
			FindFirstEvidence(CombatSourceFiles, {TEXT("EKeys::E"), TEXT("Telekinesis")}),
			{TEXT("Extend existing telekinesis architecture"), TEXT("Choose different requested inputs"), TEXT("Resolve E/T ownership explicitly before mutation")});
	}

	Data->SetArrayField(TEXT("conflicts"), Conflicts);
	Data->SetBoolField(TEXT("may_implement"), !bHasBlockingConflict);
	Data->SetArrayField(TEXT("required_user_choice"), RequiredChoices);

	TSharedPtr<FJsonObject> ProofMatrix = MakeShared<FJsonObject>();
	ProofMatrix->SetBoolField(TEXT("code_proof"), true);
	ProofMatrix->SetStringField(TEXT("code_proof_detail"), TEXT("source/config/local-asset scan completed"));
	ProofMatrix->SetBoolField(TEXT("build_proof"), false);
	ProofMatrix->SetStringField(TEXT("build_proof_detail"), TEXT("not run by read-only mechanic_preflight"));
	ProofMatrix->SetBoolField(TEXT("runtime_proof"), false);
	ProofMatrix->SetStringField(TEXT("runtime_proof_detail"), bIncludeRuntimeMapProbe
		? TEXT("runtime map probe requested but not executed by this read-only preflight implementation")
		: TEXT("runtime map probe not requested"));
	ProofMatrix->SetBoolField(TEXT("visual_proof"), false);
	ProofMatrix->SetStringField(TEXT("visual_proof_detail"), TEXT("not applicable; no viewport or animation visual proof"));
	ProofMatrix->SetBoolField(TEXT("manual_blocker"), bHasBlockingConflict);
	ProofMatrix->SetStringField(TEXT("manual_blocker_detail"), bHasBlockingConflict
		? TEXT("unresolved high/critical mechanic/input conflict requires explicit choice")
		: TEXT("none from preflight conflict rules"));
	Data->SetObjectField(TEXT("proof_matrix"), ProofMatrix);

	if (bExportReport)
	{
		const FString ReportPath = BuildReportPath(ReportSlug);
		FString SaveError;
		if (SaveReport(Data, ReportPath, SaveError))
		{
			Data->SetStringField(TEXT("report_path"), ReportPath);
		}
		else
		{
			Data->SetStringField(TEXT("report_export_error"), SaveError);
		}
	}

	return FMCPToolResult::Success(TEXT("Mechanic preflight inspect completed"), Data);
}
