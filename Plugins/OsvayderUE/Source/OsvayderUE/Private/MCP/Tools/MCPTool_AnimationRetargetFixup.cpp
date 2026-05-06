// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_AnimationRetargetFixup.h"

#include "AnimAssetManager.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "UObject/MetaData.h"

namespace
{
	const TCHAR* ToolSchemaVersion = TEXT("animation_retarget_fixup.v2");
	const TCHAR* RetargetSourceMetaKey = TEXT("OsvayderUE.Retarget.SourceAnimationPath");
	const TCHAR* RetargeterMetaKey = TEXT("OsvayderUE.Retarget.RetargeterPath");
	const TCHAR* RetargetTargetSkeletonMetaKey = TEXT("OsvayderUE.Retarget.TargetSkeletonPath");

	struct FLoadedCandidate
	{
		FString RequestedPath;
		UAnimSequence* Sequence = nullptr;
		USkeleton* SourceSkeleton = nullptr;
		TSharedPtr<FJsonObject> Entry;
	};

	struct FDestinationPlanItem
	{
		FLoadedCandidate* Candidate = nullptr;
		FString DestinationPackage;
		FString DestinationObjectPath;
		bool bDestinationExists = false;
		bool bExistingGeneratedCompatible = false;
	};

	struct FRetargetRoute
	{
		FString RetargeterPath;
		UIKRetargeter* Retargeter = nullptr;
		const UIKRigDefinition* SourceIKRig = nullptr;
		const UIKRigDefinition* TargetIKRig = nullptr;
		USkeletalMesh* SourceMesh = nullptr;
		USkeletalMesh* TargetMesh = nullptr;
		TArray<FString> ReasonCodes;
		bool bExecutable = false;
	};

	void AddUniqueString(TArray<FString>& Values, const FString& Candidate)
	{
		if (!Candidate.IsEmpty() && !Values.Contains(Candidate))
		{
			Values.Add(Candidate);
		}
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	FString NormalizeGameRoot(const FString& RawRoot, FString& OutError)
	{
		FString Root = RawRoot;
		Root.TrimStartAndEndInline();
		Root.ReplaceInline(TEXT("\\"), TEXT("/"));

		while (Root.EndsWith(TEXT("/")) && Root.Len() > 5)
		{
			Root.LeftChopInline(1);
		}

		if (Root.IsEmpty())
		{
			OutError = TEXT("destination_game_root is required");
			return FString();
		}
		if (Root != TEXT("/Game") && !Root.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("destination_game_root must be under /Game, got '%s'"), *RawRoot);
			return FString();
		}
		if (Root.Contains(TEXT(".")))
		{
			OutError = FString::Printf(TEXT("destination_game_root must be a package root, not an object path: '%s'"), *RawRoot);
			return FString();
		}
		return Root;
	}

	FString NormalizeAssetRoot(const FString& RawRoot, FString& OutError)
	{
		if (RawRoot.TrimStartAndEnd().IsEmpty())
		{
			return FString();
		}
		return NormalizeGameRoot(RawRoot, OutError);
	}

	FString NormalizeObjectPath(const FString& RawPath)
	{
		FString Path = RawPath;
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (!Path.StartsWith(TEXT("/")))
		{
			Path = TEXT("/Game/") + Path;
		}

		if (Path.StartsWith(TEXT("/Game/")) && !Path.Contains(TEXT(".")))
		{
			Path += TEXT(".") + FPaths::GetBaseFilename(Path);
		}

		return Path;
	}

	void AddUniqueAssetObjectPath(TArray<FString>& Values, const FString& Candidate)
	{
		if (!Candidate.TrimStartAndEnd().IsEmpty())
		{
			AddUniqueString(Values, NormalizeObjectPath(Candidate));
		}
	}

	FString PackagePathFromObjectOrPackagePath(const FString& Path)
	{
		FString Normalized = Path;
		Normalized.TrimStartAndEndInline();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		int32 DotIndex = INDEX_NONE;
		if (Normalized.FindChar(TEXT('.'), DotIndex))
		{
			return Normalized.Left(DotIndex);
		}
		return Normalized;
	}

	USkeleton* LoadSkeletonByPath(const FString& SkeletonPath, FString& OutError)
	{
		if (SkeletonPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("target_skeleton_path is empty");
			return nullptr;
		}

		UObject* Loaded = StaticLoadObject(USkeleton::StaticClass(), nullptr, *NormalizeObjectPath(SkeletonPath));
		USkeleton* Skeleton = Cast<USkeleton>(Loaded);
		if (!Skeleton)
		{
			OutError = FString::Printf(TEXT("Failed to load target Skeleton: %s"), *SkeletonPath);
		}
		return Skeleton;
	}

	UAnimBlueprint* LoadAnimBlueprintByPath(const FString& BlueprintPath, FString& OutError)
	{
		UObject* Loaded = StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *NormalizeObjectPath(BlueprintPath));
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Loaded);
		if (!AnimBP)
		{
			OutError = FString::Printf(TEXT("Failed to load target AnimBlueprint: %s"), *BlueprintPath);
		}
		return AnimBP;
	}

	template <typename TObject>
	TObject* LoadObjectByPath(const FString& RawPath)
	{
		if (RawPath.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		return Cast<TObject>(StaticLoadObject(TObject::StaticClass(), nullptr, *NormalizeObjectPath(RawPath)));
	}

	UAnimSequence* LoadAnimSequenceByPath(const FString& CandidatePath)
	{
		return LoadObjectByPath<UAnimSequence>(CandidatePath);
	}

	void QueryAssetsUnderRoot(const FString& AssetRoot, const FTopLevelAssetPath& ClassPath, bool bRecursivePaths, TArray<FAssetData>& OutAssets)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FARFilter Filter;
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = bRecursivePaths;
		Filter.PackagePaths.Add(FName(*AssetRoot));
		Filter.ClassPaths.Add(ClassPath);
		AssetRegistry.GetAssets(Filter, OutAssets);
	}

	void QueryAnimSequencesUnderRoot(const FString& AssetRoot, int32 Limit, TArray<FAssetData>& OutAssets)
	{
		TArray<FAssetData> AllAssets;
		QueryAssetsUnderRoot(AssetRoot, UAnimSequence::StaticClass()->GetClassPathName(), true, AllAssets);
		AllAssets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetObjectPathString() < B.GetObjectPathString();
		});

		for (const FAssetData& Asset : AllAssets)
		{
			if (OutAssets.Num() >= Limit)
			{
				break;
			}
			OutAssets.Add(Asset);
		}
	}

	bool DoesPackageExist(const FString& PackagePath)
	{
		return FPackageName::DoesPackageExist(PackagePath);
	}

	TSharedPtr<FJsonObject> MakeNextPreflightCall(
		const FString& TargetAnimBlueprintPath,
		const FString& AssetRoot)
	{
		TSharedPtr<FJsonObject> NextCall = MakeShared<FJsonObject>();
		NextCall->SetStringField(TEXT("tool"), TEXT("blueprint_query"));
		NextCall->SetStringField(TEXT("operation"), TEXT("animation_preflight"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), TEXT("animation_preflight"));
		if (!TargetAnimBlueprintPath.IsEmpty())
		{
			Params->SetStringField(TEXT("blueprint_path"), PackagePathFromObjectOrPackagePath(TargetAnimBlueprintPath));
		}
		if (!AssetRoot.IsEmpty())
		{
			Params->SetStringField(TEXT("asset_root"), AssetRoot);
		}
		NextCall->SetObjectField(TEXT("params"), Params);
		return NextCall;
	}

	TSharedPtr<FJsonObject> MakeBlocker(
		const FString& Code,
		const FString& Message)
	{
		TSharedPtr<FJsonObject> Blocker = MakeShared<FJsonObject>();
		Blocker->SetStringField(TEXT("code"), Code);
		Blocker->SetStringField(TEXT("severity"), TEXT("blocker"));
		Blocker->SetStringField(TEXT("message"), Message);
		return Blocker;
	}

	FMCPToolResult MakeStructuredErrorResult(const FString& Message, const TSharedPtr<FJsonObject>& Data)
	{
		FMCPToolResult Result = FMCPToolResult::Error(Message);
		if (Data.IsValid())
		{
			Result.Data = Data;
		}
		return Result;
	}

	void AddRouteRootCandidates(const FString& AssetRoot, TArray<FString>& OutRoots)
	{
		AddUniqueString(OutRoots, TEXT("/Game/ParkourAnimations/Characters/Mannequins/Rigs"));
		AddUniqueString(OutRoots, TEXT("/Game/ParkourAnimations/Characters/Mannequin_UE4/Rigs"));
		AddUniqueString(OutRoots, TEXT("/Game/Characters/Mannequins/Rigs"));

		if (AssetRoot.StartsWith(TEXT("/Game/ParkourAnimations/")))
		{
			AddUniqueString(OutRoots, TEXT("/Game/ParkourAnimations/Characters/Mannequins/Rigs"));
			AddUniqueString(OutRoots, TEXT("/Game/ParkourAnimations/Characters/Mannequin_UE4/Rigs"));
		}
	}

	void CollectRetargeterPaths(const FString& ExplicitPath, const FString& AssetRoot, TArray<FString>& OutPaths)
	{
		AddUniqueString(OutPaths, ExplicitPath);
		AddUniqueString(OutPaths, TEXT("/Game/ParkourAnimations/Characters/Mannequins/Rigs/RTG_Mannequin"));
		AddUniqueString(OutPaths, TEXT("/Game/ParkourAnimations/Characters/Mannequin_UE4/Rigs/RTG_UE4Manny_UE5Manny"));
		AddUniqueString(OutPaths, TEXT("/Game/ParkourAnimations/Characters/Mannequin_UE4/Rigs/RTG_UE5Manny_UE4Manny"));

		TArray<FString> Roots;
		AddRouteRootCandidates(AssetRoot, Roots);
		for (const FString& Root : Roots)
		{
			TArray<FAssetData> Assets;
			QueryAssetsUnderRoot(Root, UIKRetargeter::StaticClass()->GetClassPathName(), false, Assets);
			for (const FAssetData& Asset : Assets)
			{
				AddUniqueString(OutPaths, Asset.GetObjectPathString());
			}
		}
	}

	bool IsSkeletonCompatible(USkeleton* TargetSkeleton, USkeleton* CandidateSkeleton)
	{
		return TargetSkeleton && CandidateSkeleton && TargetSkeleton->IsCompatibleForEditor(CandidateSkeleton);
	}

	bool IsExactOrCompatibleTargetMesh(USkeletalMesh* Mesh, USkeleton* TargetSkeleton)
	{
		return Mesh && IsSkeletonCompatible(TargetSkeleton, Mesh->GetSkeleton());
	}

	USkeletalMesh* FindSkeletalMeshForSkeleton(const TArray<FString>& Roots, USkeleton* DesiredSkeleton)
	{
		if (!DesiredSkeleton)
		{
			return nullptr;
		}

		for (const FString& Root : Roots)
		{
			TArray<FAssetData> Assets;
			QueryAssetsUnderRoot(Root, USkeletalMesh::StaticClass()->GetClassPathName(), true, Assets);
			Assets.Sort([](const FAssetData& A, const FAssetData& B)
			{
				return A.GetObjectPathString() < B.GetObjectPathString();
			});

			for (const FAssetData& Asset : Assets)
			{
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
				if (Mesh && IsSkeletonCompatible(DesiredSkeleton, Mesh->GetSkeleton()))
				{
					return Mesh;
				}
			}
		}
		return nullptr;
	}

	USkeletalMesh* ResolveSourceMesh(
		const FString& ExplicitPath,
		const FRetargetRoute& Route,
		USkeleton* SourceSkeleton)
	{
		if (USkeletalMesh* Explicit = LoadObjectByPath<USkeletalMesh>(ExplicitPath))
		{
			return Explicit;
		}
		if (Route.Retargeter)
		{
			if (USkeletalMesh* Mesh = Route.Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source))
			{
				return Mesh;
			}
		}
		if (Route.SourceIKRig)
		{
			if (USkeletalMesh* Mesh = Route.SourceIKRig->GetPreviewMesh())
			{
				return Mesh;
			}
		}

		return FindSkeletalMeshForSkeleton({TEXT("/Game/ParkourAnimations/Characters")}, SourceSkeleton);
	}

	USkeletalMesh* ResolveTargetMesh(
		const FString& ExplicitPath,
		const FRetargetRoute& Route,
		UAnimBlueprint* TargetAnimBP,
		USkeleton* TargetSkeleton)
	{
		if (USkeletalMesh* Explicit = LoadObjectByPath<USkeletalMesh>(ExplicitPath))
		{
			return Explicit;
		}
		if (TargetAnimBP)
		{
			if (USkeletalMesh* PreviewMesh = TargetAnimBP->GetPreviewMesh())
			{
				if (IsExactOrCompatibleTargetMesh(PreviewMesh, TargetSkeleton))
				{
					return PreviewMesh;
				}
			}
		}
		if (Route.Retargeter)
		{
			if (USkeletalMesh* Mesh = Route.Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target))
			{
				if (IsExactOrCompatibleTargetMesh(Mesh, TargetSkeleton))
				{
					return Mesh;
				}
			}
		}
		if (Route.TargetIKRig)
		{
			if (USkeletalMesh* Mesh = Route.TargetIKRig->GetPreviewMesh())
			{
				if (IsExactOrCompatibleTargetMesh(Mesh, TargetSkeleton))
				{
					return Mesh;
				}
			}
		}

		TArray<FString> Roots;
		AddUniqueString(Roots, TEXT("/Game/Characters/Mannequins/Meshes"));
		AddUniqueString(Roots, TEXT("/Game/Characters/Mannequins"));
		AddUniqueString(Roots, TEXT("/Game/Variant_Platforming"));
		return FindSkeletalMeshForSkeleton(Roots, TargetSkeleton);
	}

	bool ExistingDestinationMatchesGeneratedAsset(
		const FString& DestinationObjectPath,
		const FString& SourceAnimationPath,
		const FString& RetargeterPath,
		USkeleton* TargetSkeleton)
	{
		UAnimSequence* Existing = LoadObjectByPath<UAnimSequence>(DestinationObjectPath);
		if (!Existing || !TargetSkeleton || !IsSkeletonCompatible(TargetSkeleton, Existing->GetSkeleton()))
		{
			return false;
		}

		if (!Existing->GetOutermost())
		{
			return false;
		}

		FMetaData& MetaData = Existing->GetOutermost()->GetMetaData();
		const FString StoredSource = MetaData.GetValue(Existing, RetargetSourceMetaKey);
		const FString StoredRetargeter = MetaData.GetValue(Existing, RetargeterMetaKey);
		const FString StoredTargetSkeleton = MetaData.GetValue(Existing, RetargetTargetSkeletonMetaKey);
		return StoredSource == SourceAnimationPath
			&& StoredRetargeter == RetargeterPath
			&& StoredTargetSkeleton == TargetSkeleton->GetPathName();
	}

	TSharedPtr<FJsonObject> MakeRouteJson(const FRetargetRoute& Route)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("retargeter_path"), Route.Retargeter ? Route.Retargeter->GetPathName() : Route.RetargeterPath);
		Json->SetBoolField(TEXT("retargeter_loaded"), Route.Retargeter != nullptr);
		Json->SetBoolField(TEXT("executable"), Route.bExecutable);
		Json->SetArrayField(TEXT("non_executable_reason_codes"), MakeStringArray(Route.ReasonCodes));

		if (Route.SourceIKRig)
		{
			Json->SetStringField(TEXT("source_ik_rig_path"), Route.SourceIKRig->GetPathName());
			if (USkeletalMesh* PreviewMesh = Route.SourceIKRig->GetPreviewMesh())
			{
				Json->SetStringField(TEXT("source_ik_rig_preview_mesh_path"), PreviewMesh->GetPathName());
			}
		}
		if (Route.TargetIKRig)
		{
			Json->SetStringField(TEXT("target_ik_rig_path"), Route.TargetIKRig->GetPathName());
			if (USkeletalMesh* PreviewMesh = Route.TargetIKRig->GetPreviewMesh())
			{
				Json->SetStringField(TEXT("target_ik_rig_preview_mesh_path"), PreviewMesh->GetPathName());
			}
		}
		if (Route.SourceMesh)
		{
			Json->SetStringField(TEXT("source_mesh_path"), Route.SourceMesh->GetPathName());
			if (USkeleton* Skeleton = Route.SourceMesh->GetSkeleton())
			{
				Json->SetStringField(TEXT("source_mesh_skeleton_path"), Skeleton->GetPathName());
			}
		}
		if (Route.TargetMesh)
		{
			Json->SetStringField(TEXT("target_mesh_path"), Route.TargetMesh->GetPathName());
			if (USkeleton* Skeleton = Route.TargetMesh->GetSkeleton())
			{
				Json->SetStringField(TEXT("target_mesh_skeleton_path"), Skeleton->GetPathName());
			}
		}
		return Json;
	}

	TArray<FRetargetRoute> DiscoverRetargetRoutes(
		const FString& ExplicitRetargeterPath,
		const FString& ExplicitSourceMeshPath,
		const FString& ExplicitTargetMeshPath,
		const FString& AssetRoot,
		UAnimBlueprint* TargetAnimBP,
		USkeleton* TargetSkeleton,
		USkeleton* SourceSkeleton)
	{
		TArray<FString> RetargeterPaths;
		CollectRetargeterPaths(ExplicitRetargeterPath, AssetRoot, RetargeterPaths);

		TArray<FRetargetRoute> Routes;
		for (const FString& RetargeterPath : RetargeterPaths)
		{
			FRetargetRoute Route;
			Route.RetargeterPath = RetargeterPath;
			Route.Retargeter = LoadObjectByPath<UIKRetargeter>(RetargeterPath);
			if (!Route.Retargeter)
			{
				AddUniqueString(Route.ReasonCodes, TEXT("retargeter_asset_missing"));
				Routes.Add(Route);
				continue;
			}

			Route.SourceIKRig = Route.Retargeter->GetIKRig(ERetargetSourceOrTarget::Source);
			Route.TargetIKRig = Route.Retargeter->GetIKRig(ERetargetSourceOrTarget::Target);
			if (!Route.SourceIKRig)
			{
				AddUniqueString(Route.ReasonCodes, TEXT("source_ik_rig_missing"));
			}
			if (!Route.TargetIKRig)
			{
				AddUniqueString(Route.ReasonCodes, TEXT("target_ik_rig_missing"));
			}

			Route.SourceMesh = ResolveSourceMesh(ExplicitSourceMeshPath, Route, SourceSkeleton);
			Route.TargetMesh = ResolveTargetMesh(ExplicitTargetMeshPath, Route, TargetAnimBP, TargetSkeleton);

			if (!Route.SourceMesh)
			{
				AddUniqueString(Route.ReasonCodes, TEXT("source_mesh_missing"));
			}
			else if (!IsSkeletonCompatible(SourceSkeleton, Route.SourceMesh->GetSkeleton()))
			{
				AddUniqueString(Route.ReasonCodes, TEXT("source_mesh_skeleton_mismatch"));
			}

			if (!Route.TargetMesh)
			{
				AddUniqueString(Route.ReasonCodes, TEXT("target_mesh_missing"));
			}
			else if (!IsExactOrCompatibleTargetMesh(Route.TargetMesh, TargetSkeleton))
			{
				AddUniqueString(Route.ReasonCodes, TEXT("target_mesh_skeleton_mismatch"));
			}

			Route.bExecutable = Route.ReasonCodes.Num() == 0;
			Routes.Add(Route);
		}
		return Routes;
	}

	void ApplyGenerationMetadata(
		UAnimSequence* Generated,
		const FString& SourceAnimationPath,
		const FString& RetargeterPath,
		USkeleton* TargetSkeleton)
	{
		if (!Generated || !Generated->GetOutermost() || !TargetSkeleton)
		{
			return;
		}

		FMetaData& MetaData = Generated->GetOutermost()->GetMetaData();
		MetaData.SetValue(Generated, RetargetSourceMetaKey, *SourceAnimationPath);
		MetaData.SetValue(Generated, RetargeterMetaKey, *RetargeterPath);
		MetaData.SetValue(Generated, RetargetTargetSkeletonMetaKey, *TargetSkeleton->GetPathName());
		Generated->MarkPackageDirty();
	}
}

FMCPToolResult FMCPTool_AnimationRetargetFixup::Execute(const TSharedRef<FJsonObject>& Params)
{
	TOptional<FMCPToolResult> ErrorResult;

	FString DestinationRootRaw;
	if (!ExtractRequiredString(Params, TEXT("destination_game_root"), DestinationRootRaw, ErrorResult))
	{
		return ErrorResult.GetValue();
	}

	FString RootError;
	const FString DestinationRoot = NormalizeGameRoot(DestinationRootRaw, RootError);
	if (!RootError.IsEmpty())
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("result_type"), TEXT("animation_retarget_fixup"));
		Data->SetStringField(TEXT("schema_version"), ToolSchemaVersion);
		Data->SetStringField(TEXT("blocker_code"), TEXT("invalid_destination_root"));
		return MakeStructuredErrorResult(RootError, Data);
	}

	FString Mode = ExtractOptionalString(Params, TEXT("mode"), TEXT("dry_run")).ToLower();
	if (Mode != TEXT("dry_run") && Mode != TEXT("execute"))
	{
		return FMCPToolResult::Error(TEXT("mode must be 'dry_run' or 'execute'"));
	}

	const FString TargetAnimBlueprintPath = ExtractOptionalString(Params, TEXT("target_anim_blueprint_path"));
	const FString TargetSkeletonPath = ExtractOptionalString(Params, TEXT("target_skeleton_path"));
	if (TargetAnimBlueprintPath.IsEmpty() && TargetSkeletonPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("target_anim_blueprint_path or target_skeleton_path is required"));
	}

	UAnimBlueprint* TargetAnimBP = nullptr;
	USkeleton* TargetSkeleton = nullptr;
	FString LoadError;
	if (!TargetAnimBlueprintPath.IsEmpty())
	{
		TargetAnimBP = LoadAnimBlueprintByPath(TargetAnimBlueprintPath, LoadError);
		if (!TargetAnimBP)
		{
			return FMCPToolResult::Error(LoadError);
		}
		TargetSkeleton = FAnimAssetManager::GetTargetSkeleton(TargetAnimBP);
		if (!TargetSkeleton)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("AnimBlueprint has no target skeleton: %s"), *TargetAnimBlueprintPath));
		}
	}
	else
	{
		TargetSkeleton = LoadSkeletonByPath(TargetSkeletonPath, LoadError);
		if (!TargetSkeleton)
		{
			return FMCPToolResult::Error(LoadError);
		}
	}

	const int32 MatchLimit = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("match_limit"), 200), 1, 1000);
	FString AssetRootError;
	const FString AssetRoot = NormalizeAssetRoot(ExtractOptionalString(Params, TEXT("asset_root")), AssetRootError);
	if (!AssetRootError.IsEmpty())
	{
		return FMCPToolResult::Error(AssetRootError);
	}

	TArray<FString> CandidatePaths;
	const TArray<TSharedPtr<FJsonValue>>* CandidateValues = nullptr;
	if (Params->TryGetArrayField(TEXT("candidate_animation_paths"), CandidateValues) && CandidateValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *CandidateValues)
		{
			FString Candidate;
			if (Value.IsValid() && Value->TryGetString(Candidate) && !Candidate.IsEmpty())
			{
				AddUniqueAssetObjectPath(CandidatePaths, Candidate);
			}
		}
	}

	if (!AssetRoot.IsEmpty())
	{
		TArray<FAssetData> RootAssets;
		QueryAnimSequencesUnderRoot(AssetRoot, MatchLimit, RootAssets);
		for (const FAssetData& Asset : RootAssets)
		{
			AddUniqueAssetObjectPath(CandidatePaths, Asset.GetObjectPathString());
		}
	}

	if (CandidatePaths.Num() == 0)
	{
		return FMCPToolResult::Error(TEXT("No candidate_animation_paths and no AnimSequence assets found under asset_root"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("result_type"), TEXT("animation_retarget_fixup"));
	Data->SetStringField(TEXT("schema_version"), ToolSchemaVersion);
	Data->SetStringField(TEXT("mode"), Mode);
	Data->SetBoolField(TEXT("mutates_source_assets"), false);
	Data->SetBoolField(TEXT("overwrites_existing_destination"), false);
	Data->SetStringField(TEXT("destination_game_root"), DestinationRoot);
	Data->SetStringField(TEXT("target_skeleton"), TargetSkeleton->GetName());
	Data->SetStringField(TEXT("target_skeleton_path"), TargetSkeleton->GetPathName());
	if (TargetAnimBP)
	{
		Data->SetStringField(TEXT("target_anim_blueprint_path"), TargetAnimBP->GetPathName());
	}
	if (!AssetRoot.IsEmpty())
	{
		Data->SetStringField(TEXT("asset_root"), AssetRoot);
	}

	TArray<TSharedPtr<FJsonValue>> CompatibleAssets;
	TArray<TSharedPtr<FJsonValue>> MismatchedAssets;
	TArray<TSharedPtr<FJsonValue>> UnknownAssets;
	TArray<TSharedPtr<FJsonValue>> DestinationPlan;
	TArray<TSharedPtr<FJsonValue>> Conflicts;
	TArray<TSharedPtr<FJsonValue>> GeneratedAssets;
	TArray<TSharedPtr<FJsonValue>> VerificationFailures;
	TArray<FString> BlockerCodes;
	TArray<FString> WarningCodes;
	TArray<TSharedPtr<FJsonValue>> Blockers;
	TArray<FLoadedCandidate> LoadedCandidates;
	TArray<FDestinationPlanItem> PlanItems;
	USkeleton* FirstMismatchedSourceSkeleton = nullptr;
	LoadedCandidates.Reserve(CandidatePaths.Num());
	PlanItems.Reserve(CandidatePaths.Num());

	for (const FString& CandidatePath : CandidatePaths)
	{
		FLoadedCandidate& LoadedCandidate = LoadedCandidates.AddDefaulted_GetRef();
		LoadedCandidate.RequestedPath = CandidatePath;
		LoadedCandidate.Sequence = LoadAnimSequenceByPath(CandidatePath);
		LoadedCandidate.Entry = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Entry = LoadedCandidate.Entry;
		Entry->SetStringField(TEXT("requested_path"), CandidatePath);

		if (!LoadedCandidate.Sequence)
		{
			Entry->SetStringField(TEXT("status"), TEXT("broken_candidate_animation"));
			Entry->SetStringField(TEXT("message"), TEXT("Candidate is not a loadable AnimSequence"));
			UnknownAssets.Add(MakeShared<FJsonValueObject>(Entry));
			AddUniqueString(WarningCodes, TEXT("broken_candidate_animation"));
			continue;
		}

		UAnimSequence* Sequence = LoadedCandidate.Sequence;
		Entry->SetStringField(TEXT("name"), Sequence->GetName());
		Entry->SetStringField(TEXT("path"), Sequence->GetPathName());
		Entry->SetStringField(TEXT("class"), Sequence->GetClass()->GetName());
		Entry->SetNumberField(TEXT("length"), Sequence->GetPlayLength());
		Entry->SetNumberField(TEXT("num_frames"), Sequence->GetNumberOfSampledKeys());
		Entry->SetBoolField(TEXT("root_motion_enabled"), Sequence->HasRootMotion());

		LoadedCandidate.SourceSkeleton = Sequence->GetSkeleton();
		if (!LoadedCandidate.SourceSkeleton)
		{
			Entry->SetStringField(TEXT("status"), TEXT("broken_candidate_animation"));
			Entry->SetStringField(TEXT("message"), TEXT("AnimSequence loaded but GetSkeleton() returned null"));
			UnknownAssets.Add(MakeShared<FJsonValueObject>(Entry));
			AddUniqueString(WarningCodes, TEXT("broken_candidate_animation"));
			continue;
		}

		USkeleton* SourceSkeleton = LoadedCandidate.SourceSkeleton;
		Entry->SetStringField(TEXT("source_skeleton"), SourceSkeleton->GetName());
		Entry->SetStringField(TEXT("source_skeleton_path"), SourceSkeleton->GetPathName());

		const bool bCompatible = TargetSkeleton->IsCompatibleForEditor(SourceSkeleton);
		Entry->SetBoolField(TEXT("is_compatible"), bCompatible);
		Entry->SetBoolField(TEXT("retarget_required"), !bCompatible);

		if (bCompatible)
		{
			Entry->SetStringField(TEXT("status"), TEXT("compatible"));
			Entry->SetStringField(TEXT("usable_animation_path"), Sequence->GetPathName());
			CompatibleAssets.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		if (!FirstMismatchedSourceSkeleton)
		{
			FirstMismatchedSourceSkeleton = SourceSkeleton;
		}

		Entry->SetStringField(TEXT("status"), TEXT("skeleton_mismatch"));
		MismatchedAssets.Add(MakeShared<FJsonValueObject>(Entry));
		AddUniqueString(BlockerCodes, TEXT("skeleton_mismatch"));

		const FString DestinationPackage = DestinationRoot / Sequence->GetName();
		const FString DestinationObjectPath = DestinationPackage + TEXT(".") + Sequence->GetName();
		const bool bDestinationExists = DoesPackageExist(DestinationPackage);

		FDestinationPlanItem& PlanItem = PlanItems.AddDefaulted_GetRef();
		PlanItem.Candidate = &LoadedCandidate;
		PlanItem.DestinationPackage = DestinationPackage;
		PlanItem.DestinationObjectPath = DestinationObjectPath;
		PlanItem.bDestinationExists = bDestinationExists;

		TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("source_animation_path"), Sequence->GetPathName());
		Plan->SetStringField(TEXT("source_skeleton_path"), SourceSkeleton->GetPathName());
		Plan->SetStringField(TEXT("target_skeleton_path"), TargetSkeleton->GetPathName());
		Plan->SetStringField(TEXT("destination_package_path"), DestinationPackage);
		Plan->SetStringField(TEXT("destination_object_path"), DestinationObjectPath);
		Plan->SetStringField(TEXT("planned_action"), TEXT("retarget_copy_missing_only"));
		Plan->SetBoolField(TEXT("destination_exists"), bDestinationExists);
		Plan->SetBoolField(TEXT("can_overwrite"), false);
		DestinationPlan.Add(MakeShared<FJsonValueObject>(Plan));
	}

	const FString ExplicitRetargeterPath = ExtractOptionalString(Params, TEXT("retargeter_asset_path"), ExtractOptionalString(Params, TEXT("retargeter_path")));
	const FString ExplicitSourceMeshPath = ExtractOptionalString(Params, TEXT("source_mesh_path"));
	const FString ExplicitTargetMeshPath = ExtractOptionalString(Params, TEXT("target_mesh_path"));
	TArray<FRetargetRoute> Routes = DiscoverRetargetRoutes(
		ExplicitRetargeterPath,
		ExplicitSourceMeshPath,
		ExplicitTargetMeshPath,
		AssetRoot,
		TargetAnimBP,
		TargetSkeleton,
		FirstMismatchedSourceSkeleton);

	FRetargetRoute* ExecutableRoute = nullptr;
	TArray<TSharedPtr<FJsonValue>> RouteArray;
	TArray<FString> RouteReasonCodes;
	for (FRetargetRoute& Route : Routes)
	{
		if (Route.bExecutable && !ExecutableRoute)
		{
			ExecutableRoute = &Route;
		}
		for (const FString& ReasonCode : Route.ReasonCodes)
		{
			AddUniqueString(RouteReasonCodes, ReasonCode);
		}
		RouteArray.Add(MakeShared<FJsonValueObject>(MakeRouteJson(Route)));
	}
	Data->SetArrayField(TEXT("discovered_routes"), RouteArray);
	Data->SetNumberField(TEXT("discovered_route_count"), RouteArray.Num());
	Data->SetBoolField(TEXT("retarget_api_available"), FModuleManager::Get().ModuleExists(TEXT("IKRigEditor")));
	Data->SetBoolField(TEXT("safe_route_executable"), ExecutableRoute != nullptr);
	if (ExecutableRoute)
	{
		Data->SetObjectField(TEXT("selected_route"), MakeRouteJson(*ExecutableRoute));
	}

	for (FDestinationPlanItem& PlanItem : PlanItems)
	{
		if (!PlanItem.bDestinationExists)
		{
			continue;
		}
		const FString RetargeterPathForComparison = ExecutableRoute && ExecutableRoute->Retargeter
			? ExecutableRoute->Retargeter->GetPathName()
			: FString();
		PlanItem.bExistingGeneratedCompatible = ExistingDestinationMatchesGeneratedAsset(
			PlanItem.DestinationObjectPath,
			PlanItem.Candidate && PlanItem.Candidate->Sequence ? PlanItem.Candidate->Sequence->GetPathName() : FString(),
			RetargeterPathForComparison,
			TargetSkeleton);

		if (!PlanItem.bExistingGeneratedCompatible)
		{
			TSharedPtr<FJsonObject> Conflict = MakeShared<FJsonObject>();
			Conflict->SetStringField(TEXT("source_animation_path"), PlanItem.Candidate && PlanItem.Candidate->Sequence ? PlanItem.Candidate->Sequence->GetPathName() : FString());
			Conflict->SetStringField(TEXT("destination_package_path"), PlanItem.DestinationPackage);
			Conflict->SetStringField(TEXT("destination_object_path"), PlanItem.DestinationObjectPath);
			Conflict->SetStringField(TEXT("blocker_code"), TEXT("destination_conflict"));
			Conflict->SetStringField(TEXT("message"), TEXT("Destination already exists; missing-only retarget fixup will not overwrite it"));
			Conflicts.Add(MakeShared<FJsonValueObject>(Conflict));
			AddUniqueString(BlockerCodes, TEXT("destination_conflict"));
		}
	}

	if (MismatchedAssets.Num() > 0 && !ExecutableRoute)
	{
		if (Routes.Num() == 0 || RouteReasonCodes.Contains(TEXT("retargeter_asset_missing")))
		{
			AddUniqueString(BlockerCodes, TEXT("retargeter_asset_missing"));
		}
		for (const FString& Code : RouteReasonCodes)
		{
			AddUniqueString(BlockerCodes, Code);
		}
		if (!FModuleManager::Get().ModuleExists(TEXT("IKRigEditor")))
		{
			AddUniqueString(BlockerCodes, TEXT("retarget_api_unavailable"));
		}
	}

	if (UnknownAssets.Num() > 0 && MismatchedAssets.Num() == 0 && CompatibleAssets.Num() == 0)
	{
		AddUniqueString(BlockerCodes, TEXT("broken_candidate_animation"));
	}

	if (Conflicts.Num() > 0)
	{
		Blockers.Add(MakeShared<FJsonValueObject>(MakeBlocker(
			TEXT("destination_conflict"),
			TEXT("One or more planned destination packages already exist; this tool is missing-only and will not overwrite."))));
	}
	if (MismatchedAssets.Num() > 0 && !ExecutableRoute)
	{
		Blockers.Add(MakeShared<FJsonValueObject>(MakeBlocker(
			BlockerCodes.Contains(TEXT("retargeter_asset_missing")) ? TEXT("retargeter_asset_missing") : TEXT("retarget_api_unavailable"),
			TEXT("No executable IK Retargeter route was found after inspecting bounded local route assets."))));
	}
	if (BlockerCodes.Contains(TEXT("broken_candidate_animation")))
	{
		Blockers.Add(MakeShared<FJsonValueObject>(MakeBlocker(
			TEXT("broken_candidate_animation"),
			TEXT("All candidates are broken or missing skeletons; no valid role candidate can be retargeted."))));
	}

	Data->SetArrayField(TEXT("candidate_animation_paths"), MakeStringArray(CandidatePaths));
	Data->SetNumberField(TEXT("candidate_count"), CandidatePaths.Num());
	Data->SetArrayField(TEXT("compatible_assets"), CompatibleAssets);
	Data->SetNumberField(TEXT("compatible_count"), CompatibleAssets.Num());
	Data->SetArrayField(TEXT("mismatched_assets"), MismatchedAssets);
	Data->SetNumberField(TEXT("mismatched_count"), MismatchedAssets.Num());
	Data->SetArrayField(TEXT("unknown_assets"), UnknownAssets);
	Data->SetNumberField(TEXT("unknown_count"), UnknownAssets.Num());
	Data->SetArrayField(TEXT("destination_plan"), DestinationPlan);
	Data->SetNumberField(TEXT("planned_retarget_count"), DestinationPlan.Num());
	Data->SetArrayField(TEXT("destination_conflicts"), Conflicts);
	Data->SetNumberField(TEXT("destination_conflict_count"), Conflicts.Num());
	Data->SetArrayField(TEXT("blocker_codes"), MakeStringArray(BlockerCodes));
	Data->SetArrayField(TEXT("warning_codes"), MakeStringArray(WarningCodes));
	Data->SetArrayField(TEXT("blockers"), Blockers);

	const bool bHasBlockingPreExecuteIssue = BlockerCodes.Contains(TEXT("destination_conflict"))
		|| BlockerCodes.Contains(TEXT("retargeter_asset_missing"))
		|| BlockerCodes.Contains(TEXT("source_ik_rig_missing"))
		|| BlockerCodes.Contains(TEXT("target_ik_rig_missing"))
		|| BlockerCodes.Contains(TEXT("target_mesh_missing"))
		|| BlockerCodes.Contains(TEXT("source_mesh_missing"))
		|| BlockerCodes.Contains(TEXT("source_mesh_skeleton_mismatch"))
		|| BlockerCodes.Contains(TEXT("target_mesh_skeleton_mismatch"))
		|| BlockerCodes.Contains(TEXT("retarget_api_unavailable"))
		|| BlockerCodes.Contains(TEXT("broken_candidate_animation"));

	if (Mode == TEXT("execute") && bHasBlockingPreExecuteIssue)
	{
		Data->SetStringField(TEXT("blocker_code"), BlockerCodes.Contains(TEXT("destination_conflict"))
			? TEXT("destination_conflict")
			: BlockerCodes.Last());
		Data->SetBoolField(TEXT("ready_for_anim_blueprint_hookup"), false);
		Data->SetBoolField(TEXT("ready_to_claim_retarget_success"), false);
		Data->SetStringField(TEXT("proof_classification"), TEXT("manual_blocker"));
		Data->SetObjectField(TEXT("next_tool_call"), MakeNextPreflightCall(
			TargetAnimBP ? TargetAnimBP->GetPathName() : TargetAnimBlueprintPath,
			DestinationPlan.Num() > 0 ? DestinationRoot : AssetRoot));
		return MakeStructuredErrorResult(
			FString::Printf(TEXT("Animation retarget fixup blocked: %s"), *FString::Join(BlockerCodes, TEXT(", "))),
			Data);
	}

	if (Mode == TEXT("execute") && ExecutableRoute && PlanItems.Num() > 0)
	{
		FIKRetargetBatchOperationContext Context;
		Context.SourceMesh = ExecutableRoute->SourceMesh;
		Context.TargetMesh = ExecutableRoute->TargetMesh;
		Context.IKRetargetAsset = ExecutableRoute->Retargeter;
		Context.NameRule.FolderPath = DestinationRoot;
		Context.bIncludeReferencedAssets = false;
		Context.bOverwriteExistingFiles = false;
		Context.bExportOnlyAnimatedBones = true;
		Context.bRetainAdditiveFlags = true;

		for (const FDestinationPlanItem& PlanItem : PlanItems)
		{
			if (PlanItem.bDestinationExists && PlanItem.bExistingGeneratedCompatible)
			{
				continue;
			}
			if (PlanItem.Candidate && PlanItem.Candidate->Sequence)
			{
				Context.AssetsToRetarget.Add(PlanItem.Candidate->Sequence);
			}
		}

		UIKRetargetBatchOperation* BatchOperation = NewObject<UIKRetargetBatchOperation>();
		BatchOperation->AddToRoot();
		BatchOperation->RunRetarget(Context);
		BatchOperation->RemoveFromRoot();

		for (const FDestinationPlanItem& PlanItem : PlanItems)
		{
			if (!PlanItem.Candidate || !PlanItem.Candidate->Sequence)
			{
				continue;
			}

			UAnimSequence* Generated = LoadObjectByPath<UAnimSequence>(PlanItem.DestinationObjectPath);
			TSharedPtr<FJsonObject> GeneratedJson = MakeShared<FJsonObject>();
			GeneratedJson->SetStringField(TEXT("source_animation_path"), PlanItem.Candidate->Sequence->GetPathName());
			GeneratedJson->SetStringField(TEXT("destination_object_path"), PlanItem.DestinationObjectPath);

			if (!Generated)
			{
				GeneratedJson->SetStringField(TEXT("status"), TEXT("verification_failed_missing_generated_asset"));
				VerificationFailures.Add(MakeShared<FJsonValueObject>(GeneratedJson));
				AddUniqueString(BlockerCodes, TEXT("retarget_execute_verification_failed"));
				continue;
			}

			ApplyGenerationMetadata(Generated, PlanItem.Candidate->Sequence->GetPathName(), ExecutableRoute->Retargeter->GetPathName(), TargetSkeleton);
			UEditorAssetLibrary::SaveLoadedAsset(Generated, false);

			USkeleton* GeneratedSkeleton = Generated->GetSkeleton();
			const bool bVerified = IsSkeletonCompatible(TargetSkeleton, GeneratedSkeleton);
			GeneratedJson->SetStringField(TEXT("status"), bVerified ? TEXT("verified_target_skeleton_compatible") : TEXT("verification_failed_skeleton_mismatch"));
			if (GeneratedSkeleton)
			{
				GeneratedJson->SetStringField(TEXT("generated_skeleton_path"), GeneratedSkeleton->GetPathName());
			}
			GeneratedJson->SetBoolField(TEXT("target_skeleton_compatible"), bVerified);

			if (bVerified)
			{
				GeneratedAssets.Add(MakeShared<FJsonValueObject>(GeneratedJson));
			}
			else
			{
				VerificationFailures.Add(MakeShared<FJsonValueObject>(GeneratedJson));
				AddUniqueString(BlockerCodes, TEXT("retarget_execute_verification_failed"));
			}
		}
	}

	Data->SetArrayField(TEXT("generated_assets"), GeneratedAssets);
	Data->SetNumberField(TEXT("generated_asset_count"), GeneratedAssets.Num());
	Data->SetArrayField(TEXT("verification_failures"), VerificationFailures);
	Data->SetNumberField(TEXT("verification_failure_count"), VerificationFailures.Num());
	Data->SetArrayField(TEXT("blocker_codes"), MakeStringArray(BlockerCodes));

	const bool bExecutionVerified = Mode == TEXT("execute")
		&& PlanItems.Num() > 0
		&& GeneratedAssets.Num() == PlanItems.Num()
		&& VerificationFailures.Num() == 0
		&& !BlockerCodes.Contains(TEXT("retarget_execute_verification_failed"));
	const bool bBlocked = BlockerCodes.Num() > 0 && !bExecutionVerified;

	Data->SetBoolField(TEXT("ready_for_anim_blueprint_hookup"),
		bExecutionVerified || (CompatibleAssets.Num() > 0 && MismatchedAssets.Num() == 0 && !BlockerCodes.Contains(TEXT("broken_candidate_animation"))));
	Data->SetBoolField(TEXT("ready_to_claim_retarget_success"), bExecutionVerified);
	Data->SetStringField(TEXT("proof_classification"), bBlocked ? TEXT("manual_blocker") : TEXT("animation_inventory_proof"));
	Data->SetObjectField(TEXT("next_tool_call"), MakeNextPreflightCall(
		TargetAnimBP ? TargetAnimBP->GetPathName() : TargetAnimBlueprintPath,
		(DestinationPlan.Num() > 0 || GeneratedAssets.Num() > 0) ? DestinationRoot : AssetRoot));

	if (Mode == TEXT("execute") && bBlocked)
	{
		Data->SetStringField(TEXT("blocker_code"), BlockerCodes.Last());
		return MakeStructuredErrorResult(
			FString::Printf(TEXT("Animation retarget fixup blocked after execute: %s"), *FString::Join(BlockerCodes, TEXT(", "))),
			Data);
	}

	const FString Message = FString::Printf(
		TEXT("Animation retarget fixup %s: %d compatible, %d mismatched, %d generated, %d destination conflicts"),
		*Mode,
		CompatibleAssets.Num(),
		MismatchedAssets.Num(),
		GeneratedAssets.Num(),
		Conflicts.Num());
	return FMCPToolResult::Success(Message, Data);
}
