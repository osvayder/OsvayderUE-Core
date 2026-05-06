// Copyright Natali Caggiano. All Rights Reserved.

#include "AnimationBlueprintUtils.h"
#include "OsvayderUEConstants.h"
#include "AnimTransitionConditionFactory.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateEntryNode.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_TransitionRuleGetter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SimpleConstructionScript.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Internationalization/Regex.h"

namespace
{
	void AddUniqueString(TArray<FString>& Values, const FString& Candidate)
	{
		if (!Candidate.IsEmpty() && !Values.Contains(Candidate))
		{
			Values.Add(Candidate);
		}
	}

	FString NormalizePreflightToken(const FString& Value)
	{
		FString Result;
		Result.Reserve(Value.Len());
		for (const TCHAR Char : Value.ToLower())
		{
			if (FChar::IsAlnum(Char))
			{
				Result.AppendChar(Char);
			}
		}
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringJsonArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	void AddRoleSpecificTerms(const FString& NormalizedRole, TArray<FString>& OutTerms)
	{
		if (NormalizedRole == TEXT("landing"))
		{
			AddUniqueString(OutTerms, TEXT("land"));
		}
		else if (NormalizedRole == TEXT("launch"))
		{
			AddUniqueString(OutTerms, TEXT("jumpstart"));
		}
		else if (NormalizedRole == TEXT("walljump"))
		{
			AddUniqueString(OutTerms, TEXT("walljump"));
		}
		else if (NormalizedRole == TEXT("wallclimb"))
		{
			AddUniqueString(OutTerms, TEXT("climb"));
			AddUniqueString(OutTerms, TEXT("climbup"));
			AddUniqueString(OutTerms, TEXT("climbeup"));
		}
		else if (NormalizedRole == TEXT("wallrunleft"))
		{
			AddUniqueString(OutTerms, TEXT("wallrun"));
			AddUniqueString(OutTerms, TEXT("left"));
		}
		else if (NormalizedRole == TEXT("wallrunright"))
		{
			AddUniqueString(OutTerms, TEXT("wallrun"));
			AddUniqueString(OutTerms, TEXT("right"));
		}
		else if (NormalizedRole == TEXT("fly"))
		{
			AddUniqueString(OutTerms, TEXT("flight"));
		}
		else if (NormalizedRole == TEXT("death"))
		{
			AddUniqueString(OutTerms, TEXT("die"));
		}
		else if (NormalizedRole == TEXT("hit"))
		{
			AddUniqueString(OutTerms, TEXT("hitreact"));
		}
		else if (NormalizedRole == TEXT("ledgegrab"))
		{
			AddUniqueString(OutTerms, TEXT("ledge"));
			AddUniqueString(OutTerms, TEXT("grab"));
			AddUniqueString(OutTerms, TEXT("hang"));
		}
		else if (NormalizedRole == TEXT("ledgeattach"))
		{
			AddUniqueString(OutTerms, TEXT("ledge"));
			AddUniqueString(OutTerms, TEXT("attach"));
			AddUniqueString(OutTerms, TEXT("grab"));
		}
		else if (NormalizedRole == TEXT("ledgeidle") || NormalizedRole == TEXT("ledgehold"))
		{
			AddUniqueString(OutTerms, TEXT("ledge"));
			AddUniqueString(OutTerms, TEXT("idle"));
			AddUniqueString(OutTerms, TEXT("hold"));
			AddUniqueString(OutTerms, TEXT("hang"));
		}
		else if (NormalizedRole == TEXT("mantle") || NormalizedRole == TEXT("climbup"))
		{
			AddUniqueString(OutTerms, TEXT("mantle"));
			AddUniqueString(OutTerms, TEXT("climbup"));
			AddUniqueString(OutTerms, TEXT("climbeup"));
			AddUniqueString(OutTerms, TEXT("ledge"));
		}
		else if (NormalizedRole == TEXT("enterbody"))
		{
			AddUniqueString(OutTerms, TEXT("possess"));
		}
		else if (NormalizedRole == TEXT("exitbody"))
		{
			AddUniqueString(OutTerms, TEXT("unpossess"));
		}
		else if (NormalizedRole == TEXT("throw"))
		{
			AddUniqueString(OutTerms, TEXT("toss"));
		}
	}

	TArray<FString> BuildRoleSearchTerms(const FString& Role)
	{
		TArray<FString> Terms;
		const FString NormalizedRole = NormalizePreflightToken(Role);
		AddUniqueString(Terms, NormalizedRole);

		FString TokenizedRole = Role;
		TokenizedRole.ReplaceInline(TEXT("-"), TEXT("_"));
		TokenizedRole.ReplaceInline(TEXT(" "), TEXT("_"));
		TArray<FString> RawTokens;
		TokenizedRole.ParseIntoArray(RawTokens, TEXT("_"), true);
		for (const FString& Token : RawTokens)
		{
			AddUniqueString(Terms, NormalizePreflightToken(Token));
		}

		AddRoleSpecificTerms(NormalizedRole, Terms);
		return Terms;
	}

	FString NormalizeAssetReferencePath(const FString& RawReference)
	{
		FString Reference = RawReference;
		Reference.TrimStartAndEndInline();
		Reference.ReplaceInline(TEXT("\\"), TEXT("/"));

		int32 QuoteStart = INDEX_NONE;
		int32 QuoteEnd = INDEX_NONE;
		if (Reference.FindChar(TEXT('\''), QuoteStart) && Reference.FindLastChar(TEXT('\''), QuoteEnd) && QuoteEnd > QuoteStart)
		{
			Reference = Reference.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
		}

		int32 DotIndex = INDEX_NONE;
		if (Reference.FindChar(TEXT('.'), DotIndex))
		{
			return Reference.Left(DotIndex);
		}
		return Reference;
	}

	int32 CountRoleBaseTokens(const FString& Role)
	{
		FString TokenizedRole = Role;
		TokenizedRole.ReplaceInline(TEXT("-"), TEXT("_"));
		TokenizedRole.ReplaceInline(TEXT(" "), TEXT("_"));
		TArray<FString> RawTokens;
		TokenizedRole.ParseIntoArray(RawTokens, TEXT("_"), true);
		int32 Count = 0;
		for (const FString& Token : RawTokens)
		{
			if (!NormalizePreflightToken(Token).IsEmpty())
			{
				++Count;
			}
		}
		return FMath::Max(Count, 1);
	}

	int32 ScoreAssetForRole(
		const FString& Role,
		const TArray<FString>& SearchTerms,
		const FString& NormalizedAssetName,
		const FString& NormalizedAssetPath,
		int32& OutMatchedTermCount)
	{
		OutMatchedTermCount = 0;
		int32 Score = 0;

		const FString NormalizedRole = NormalizePreflightToken(Role);
		const bool bIsWallRunLeft = NormalizedRole == TEXT("wallrunleft");
		const bool bIsWallRunRight = NormalizedRole == TEXT("wallrunright");
		const bool bHasWallRun = NormalizedAssetName.Contains(TEXT("wallrun")) || NormalizedAssetPath.Contains(TEXT("wallrun"));
		const bool bLooksLeft = NormalizedAssetName.Contains(TEXT("left")) || NormalizedAssetPath.Contains(TEXT("left")) || NormalizedAssetName.Contains(TEXT("lcycle")) || NormalizedAssetPath.Contains(TEXT("lcycle"));
		const bool bLooksRight = NormalizedAssetName.Contains(TEXT("right")) || NormalizedAssetPath.Contains(TEXT("right")) || NormalizedAssetName.Contains(TEXT("rcycle")) || NormalizedAssetPath.Contains(TEXT("rcycle"));

		if ((bIsWallRunLeft && bHasWallRun && bLooksLeft) || (bIsWallRunRight && bHasWallRun && bLooksRight))
		{
			Score += 120;
			OutMatchedTermCount = FMath::Max(OutMatchedTermCount, 2);
		}
		else if ((bIsWallRunLeft && bLooksRight) || (bIsWallRunRight && bLooksLeft))
		{
			Score -= 50;
		}

		if (!NormalizedRole.IsEmpty() && NormalizedAssetName.Contains(NormalizedRole))
		{
			Score += 100;
		}

		for (const FString& Term : SearchTerms)
		{
			if (Term.Len() < 3)
			{
				continue;
			}

			if (NormalizedAssetName.Contains(Term))
			{
				++OutMatchedTermCount;
				Score += (Term == NormalizedRole) ? 40 : 15;
			}
		}

		if (NormalizedAssetPath.Contains(TEXT("ue5skelet")))
		{
			Score += 25;
		}
		if (NormalizedAssetPath.Contains(TEXT("ue4skelet")))
		{
			Score -= 25;
		}

		const int32 RequiredMatches = CountRoleBaseTokens(Role) > 1 ? 2 : 1;
		if (Score < 100 && OutMatchedTermCount < RequiredMatches)
		{
			return 0;
		}

		return Score;
	}

	TArray<FString> BuildRoleBindingPropertyNames(const FString& Role)
	{
		TArray<FString> PropertyNames;
		const FString NormalizedRole = NormalizePreflightToken(Role);
		if (NormalizedRole == TEXT("wallrunleft"))
		{
			AddUniqueString(PropertyNames, TEXT("WallRunLeftAnimation"));
		}
		else if (NormalizedRole == TEXT("wallrunright"))
		{
			AddUniqueString(PropertyNames, TEXT("WallRunRightAnimation"));
		}
		else if (NormalizedRole == TEXT("wallclimb"))
		{
			AddUniqueString(PropertyNames, TEXT("WallClimbAnimation"));
		}
		else if (NormalizedRole == TEXT("ledgeattach") || NormalizedRole == TEXT("ledgegrab"))
		{
			AddUniqueString(PropertyNames, TEXT("LedgeAttachAnimation"));
		}
		else if (NormalizedRole == TEXT("ledgeidle") || NormalizedRole == TEXT("ledgehold"))
		{
			AddUniqueString(PropertyNames, TEXT("LedgeIdleAnimation"));
		}
		else if (NormalizedRole == TEXT("mantle") || NormalizedRole == TEXT("climbup"))
		{
			AddUniqueString(PropertyNames, TEXT("MantleAnimation"));
		}
		return PropertyNames;
	}

	FString GetObjectPathFromAnimationProperty(const FProperty* Property, const UObject* Container)
	{
		if (!Property || !Container)
		{
			return FString();
		}

		if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPtr Value = SoftObjectProperty->GetPropertyValue_InContainer(Container);
			return Value.ToSoftObjectPath().ToString();
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (const UObject* Value = ObjectProperty->GetObjectPropertyValue_InContainer(Container))
			{
				return Value->GetPathName();
			}
		}

		return FString();
	}

	TSharedPtr<FJsonObject> BuildActorRoleBindingReport(
		UBlueprint* ActorBlueprint,
		UAnimBlueprint* AnimBP,
		const FString& Role)
	{
		TSharedPtr<FJsonObject> Binding = MakeShared<FJsonObject>();
		Binding->SetStringField(TEXT("role"), Role);

		const TArray<FString> PropertyNames = BuildRoleBindingPropertyNames(Role);
		Binding->SetArrayField(TEXT("candidate_property_names"), MakeStringJsonArray(PropertyNames));
		if (PropertyNames.Num() == 0)
		{
			Binding->SetStringField(TEXT("status"), TEXT("no_role_property_mapping"));
			Binding->SetBoolField(TEXT("binding_evaluated"), false);
			return Binding;
		}

		Binding->SetBoolField(TEXT("binding_evaluated"), true);
		if (!ActorBlueprint || !ActorBlueprint->GeneratedClass)
		{
			Binding->SetStringField(TEXT("status"), TEXT("actor_blueprint_unavailable"));
			return Binding;
		}

		const UObject* CDO = ActorBlueprint->GeneratedClass->GetDefaultObject(false);
		if (!CDO)
		{
			Binding->SetStringField(TEXT("status"), TEXT("actor_cdo_unavailable"));
			return Binding;
		}

		const FProperty* FoundProperty = nullptr;
		for (const FString& PropertyName : PropertyNames)
		{
			FoundProperty = ActorBlueprint->GeneratedClass->FindPropertyByName(FName(*PropertyName));
			if (FoundProperty)
			{
				break;
			}
		}

		if (!FoundProperty)
		{
			Binding->SetStringField(TEXT("status"), TEXT("required_role_binding_missing"));
			return Binding;
		}

		Binding->SetStringField(TEXT("property_name"), FoundProperty->GetName());
		const FString AssignedPath = GetObjectPathFromAnimationProperty(FoundProperty, CDO);
		Binding->SetStringField(TEXT("assigned_asset_path"), AssignedPath);
		if (AssignedPath.IsEmpty() || AssignedPath == TEXT("None"))
		{
			Binding->SetStringField(TEXT("status"), TEXT("required_role_binding_empty"));
			return Binding;
		}

		UObject* LoadedObject = StaticLoadObject(UAnimationAsset::StaticClass(), nullptr, *AssignedPath);
		UAnimationAsset* AnimationAsset = Cast<UAnimationAsset>(LoadedObject);
		if (!AnimationAsset)
		{
			Binding->SetStringField(TEXT("status"), TEXT("assigned_reference_unresolved"));
			return Binding;
		}

		Binding->SetStringField(TEXT("assigned_asset_name"), AnimationAsset->GetName());
		Binding->SetStringField(TEXT("assigned_asset_class"), AnimationAsset->GetClass()->GetName());
		TSharedPtr<FJsonObject> CompatibilityInfo =
			FAnimAssetManager::DescribeAnimationCompatibility(AnimBP, AnimationAsset);
		const FString CompatibilityStatus =
			CompatibilityInfo.IsValid() ? CompatibilityInfo->GetStringField(TEXT("status")) : TEXT("unknown_unloaded_asset");
		Binding->SetStringField(TEXT("skeleton_compatibility"), CompatibilityStatus);
		if (CompatibilityInfo.IsValid())
		{
			Binding->SetObjectField(TEXT("compatibility"), CompatibilityInfo);
		}

		const TArray<FString> SearchTerms = BuildRoleSearchTerms(Role);
		int32 MatchedTerms = 0;
		const int32 SemanticScore = ScoreAssetForRole(
			Role,
			SearchTerms,
			NormalizePreflightToken(AnimationAsset->GetName()),
			NormalizePreflightToken(AnimationAsset->GetPathName()),
			MatchedTerms);
		Binding->SetNumberField(TEXT("semantic_match_score"), SemanticScore);
		Binding->SetNumberField(TEXT("semantic_matched_term_count"), MatchedTerms);

		if (CompatibilityStatus != TEXT("compatible"))
		{
			Binding->SetStringField(TEXT("status"), TEXT("assigned_reference_skeleton_mismatch"));
		}
		else if (SemanticScore <= 0)
		{
			Binding->SetStringField(TEXT("status"), TEXT("assigned_reference_semantic_mismatch"));
		}
		else
		{
			Binding->SetStringField(TEXT("status"), TEXT("assigned_reference_ready"));
		}

		return Binding;
	}

	void QueryAllAnimationAssets(TArray<FAssetData>& OutAssets, const FString& AssetRoot)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FARFilter Filter;
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(*(AssetRoot.IsEmpty() ? FString(TEXT("/Game")) : AssetRoot)));
		Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UBlendSpace::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UBlendSpace1D::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UAnimMontage::StaticClass()->GetClassPathName());

		AssetRegistry.GetAssets(Filter, OutAssets);
	}

	FString NormalizeAssetRootForPreflight(const FString& RawRoot, FString& OutError)
	{
		FString AssetRoot = RawRoot;
		AssetRoot.TrimStartAndEndInline();

		if (AssetRoot.IsEmpty())
		{
			return FString();
		}

		AssetRoot.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (AssetRoot.EndsWith(TEXT("/")) && AssetRoot.Len() > 5)
		{
			AssetRoot.LeftChopInline(1);
		}

		if (AssetRoot != TEXT("/Game") && !AssetRoot.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(
				TEXT("animation_preflight asset_root must be a local project path under /Game, got '%s'"),
				*RawRoot);
			return FString();
		}

		if (AssetRoot.Contains(TEXT(".")))
		{
			OutError = FString::Printf(
				TEXT("animation_preflight asset_root must be a package root like '/Game/ImportedPack', not an object path: '%s'"),
				*RawRoot);
			return FString();
		}

		return AssetRoot;
	}

	TSharedPtr<FJsonObject> BuildSkeletonSummaryForAssets(
		const TArray<FAssetData>& AssetInventory,
		USkeleton* TargetSkeleton)
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("total_animation_asset_count"), AssetInventory.Num());

		if (TargetSkeleton)
		{
			Summary->SetStringField(TEXT("target_skeleton"), TargetSkeleton->GetName());
			Summary->SetStringField(TEXT("target_skeleton_path"), TargetSkeleton->GetPathName());
		}
		else
		{
			Summary->SetStringField(TEXT("target_skeleton"), TEXT(""));
			Summary->SetStringField(TEXT("target_skeleton_path"), TEXT(""));
		}

		int32 CompatibleCount = 0;
		int32 IncompatibleCount = 0;
		int32 UnknownCount = 0;
		int32 BrokenCount = 0;

		for (const FAssetData& AssetData : AssetInventory)
		{
			FString SkeletonTag;
			const bool bHasSkeletonTag = AssetData.GetTagValue(FName(TEXT("Skeleton")), SkeletonTag);
			if (!TargetSkeleton || !bHasSkeletonTag || SkeletonTag.IsEmpty())
			{
				++UnknownCount;
				++BrokenCount;
			}
			else if (NormalizeAssetReferencePath(SkeletonTag) == NormalizeAssetReferencePath(TargetSkeleton->GetPathName()))
			{
				++CompatibleCount;
			}
			else
			{
				++IncompatibleCount;
			}
		}

		Summary->SetNumberField(TEXT("compatible_asset_count"), CompatibleCount);
		Summary->SetNumberField(TEXT("incompatible_asset_count"), IncompatibleCount);
		Summary->SetNumberField(TEXT("unknown_skeleton_asset_count"), UnknownCount);
		Summary->SetNumberField(TEXT("broken_animation_asset_count"), BrokenCount);
		return Summary;
	}

	USCS_Node* FindTargetSkeletalMeshNode(UBlueprint* ActorBlueprint, const FString& ComponentName)
	{
		if (!ActorBlueprint || !ActorBlueprint->SimpleConstructionScript)
		{
			return nullptr;
		}

		if (!ComponentName.IsEmpty())
		{
			if (USCS_Node* ExplicitNode = ActorBlueprint->SimpleConstructionScript->FindSCSNode(FName(*ComponentName)))
			{
				if (ExplicitNode->ComponentClass && ExplicitNode->ComponentClass->IsChildOf(USkeletalMeshComponent::StaticClass()))
				{
					return ExplicitNode;
				}
			}
			return nullptr;
		}

		for (USCS_Node* Node : ActorBlueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf(USkeletalMeshComponent::StaticClass()))
			{
				return Node;
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> BuildActorBlueprintInventory(
		UBlueprint* ActorBlueprint,
		UAnimBlueprint* TargetAnimBlueprint,
		const FString& ComponentName)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

		if (!ActorBlueprint)
		{
			return Json;
		}

		Json->SetStringField(TEXT("name"), ActorBlueprint->GetName());
		Json->SetStringField(TEXT("path"), ActorBlueprint->GetPathName());

		USCS_Node* MeshNode = FindTargetSkeletalMeshNode(ActorBlueprint, ComponentName);
		if (!MeshNode || !MeshNode->ComponentTemplate)
		{
			Json->SetStringField(TEXT("assignment_status"), TEXT("no_skeletal_mesh_component"));
			return Json;
		}

		USkeletalMeshComponent* MeshComponent = Cast<USkeletalMeshComponent>(MeshNode->ComponentTemplate);
		if (!MeshComponent)
		{
			Json->SetStringField(TEXT("assignment_status"), TEXT("no_skeletal_mesh_component"));
			return Json;
		}

		TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
		ComponentJson->SetStringField(TEXT("component_name"), MeshNode->GetVariableName().ToString());
		ComponentJson->SetStringField(TEXT("component_class"), MeshComponent->GetClass()->GetName());

		if (USkeletalMesh* SkeletalMesh = MeshComponent->GetSkeletalMeshAsset())
		{
			ComponentJson->SetStringField(TEXT("skeletal_mesh"), SkeletalMesh->GetName());
			ComponentJson->SetStringField(TEXT("skeletal_mesh_path"), SkeletalMesh->GetPathName());

			if (USkeleton* MeshSkeleton = SkeletalMesh->GetSkeleton())
			{
				ComponentJson->SetStringField(TEXT("skeletal_mesh_skeleton"), MeshSkeleton->GetName());
				ComponentJson->SetStringField(TEXT("skeletal_mesh_skeleton_path"), MeshSkeleton->GetPathName());
			}
		}

		FString AssignmentStatus = TEXT("missing_anim_blueprint_assignment");
		if (MeshComponent->AnimClass)
		{
			ComponentJson->SetStringField(TEXT("anim_class"), MeshComponent->AnimClass->GetName());
			ComponentJson->SetStringField(TEXT("anim_class_path"), MeshComponent->AnimClass->GetPathName());

			if (const UBlueprint* AssignedBlueprint = Cast<UBlueprint>(MeshComponent->AnimClass->ClassGeneratedBy))
			{
				ComponentJson->SetStringField(TEXT("assigned_anim_blueprint"), AssignedBlueprint->GetName());
				ComponentJson->SetStringField(TEXT("assigned_anim_blueprint_path"), AssignedBlueprint->GetPathName());
			}

			if (TargetAnimBlueprint && TargetAnimBlueprint->GeneratedClass && MeshComponent->AnimClass == TargetAnimBlueprint->GeneratedClass)
			{
				AssignmentStatus = TEXT("matches_target_anim_blueprint");
			}
			else
			{
				AssignmentStatus = TEXT("different_anim_blueprint");
			}
		}

		Json->SetStringField(TEXT("assignment_status"), AssignmentStatus);
		Json->SetObjectField(TEXT("skeletal_mesh_component"), ComponentJson);
		return Json;
	}
}

// ===== AnimBlueprint Access (Level 1) =====

UAnimBlueprint* FAnimationBlueprintUtils::LoadAnimBlueprint(const FString& BlueprintPath, FString& OutError)
{
	if (BlueprintPath.IsEmpty())
	{
		OutError = TEXT("Blueprint path is empty");
		return nullptr;
	}

	// Try to load the asset
	UObject* LoadedAsset = StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath);

	if (!LoadedAsset)
	{
		// Try with different path formats
		FString AdjustedPath = BlueprintPath;
		if (!AdjustedPath.StartsWith(TEXT("/")))
		{
			AdjustedPath = TEXT("/Game/") + AdjustedPath;
		}
		if (!AdjustedPath.EndsWith(TEXT(".")) && !AdjustedPath.Contains(TEXT(".")))
		{
			AdjustedPath += TEXT(".") + FPaths::GetBaseFilename(AdjustedPath);
		}

		LoadedAsset = StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *AdjustedPath);
	}

	if (!LoadedAsset)
	{
		OutError = FString::Printf(TEXT("Failed to load Animation Blueprint: %s"), *BlueprintPath);
		return nullptr;
	}

	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(LoadedAsset);
	if (!AnimBP)
	{
		OutError = FString::Printf(TEXT("Asset is not an Animation Blueprint: %s"), *BlueprintPath);
		return nullptr;
	}

	return AnimBP;
}

bool FAnimationBlueprintUtils::IsAnimationBlueprint(UBlueprint* Blueprint)
{
	return Blueprint && Blueprint->IsA<UAnimBlueprint>();
}

bool FAnimationBlueprintUtils::CompileAnimBlueprint(UAnimBlueprint* AnimBP, FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("AnimBlueprint is null");
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(AnimBP);

	if (AnimBP->Status == BS_Error)
	{
		OutError = TEXT("Animation Blueprint compilation failed with errors");
		return false;
	}

	// Auto-save after successful compile
	FString AssetPath = AnimBP->GetPathName();
	if (!UEditorAssetLibrary::SaveAsset(AssetPath, false))
	{
		UE_LOG(LogTemp, Warning, TEXT("CompileAnimBlueprint: Failed to auto-save asset %s"), *AssetPath);
	}

	return true;
}

void FAnimationBlueprintUtils::MarkAnimBlueprintModified(UAnimBlueprint* AnimBP)
{
	if (AnimBP)
	{
		AnimBP->MarkPackageDirty();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	}
}

bool FAnimationBlueprintUtils::ValidateAnimBlueprintForOperation(UAnimBlueprint* AnimBP, FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("AnimBlueprint is null");
		return false;
	}

	if (!AnimBP->TargetSkeleton)
	{
		OutError = TEXT("AnimBlueprint has no target skeleton");
		return false;
	}

	return true;
}

// ===== State Machine Operations (Level 2) =====

UAnimGraphNode_StateMachine* FAnimationBlueprintUtils::CreateStateMachine(
	UAnimBlueprint* AnimBP,
	const FString& MachineName,
	FVector2D Position,
	FString& OutNodeId,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return nullptr;
	}

	UAnimGraphNode_StateMachine* Result = FAnimStateMachineEditor::CreateStateMachine(
		AnimBP, MachineName, Position, OutNodeId, OutError);

	if (Result)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return Result;
}

UAnimGraphNode_StateMachine* FAnimationBlueprintUtils::FindStateMachine(
	UAnimBlueprint* AnimBP,
	const FString& MachineName,
	FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("AnimBlueprint is null");
		return nullptr;
	}

	return FAnimStateMachineEditor::FindStateMachine(AnimBP, MachineName, OutError);
}

TArray<UAnimGraphNode_StateMachine*> FAnimationBlueprintUtils::GetAllStateMachines(UAnimBlueprint* AnimBP)
{
	if (!AnimBP)
	{
		return TArray<UAnimGraphNode_StateMachine*>();
	}

	return FAnimStateMachineEditor::GetAllStateMachines(AnimBP);
}

UAnimationStateMachineGraph* FAnimationBlueprintUtils::GetStateMachineGraph(
	UAnimGraphNode_StateMachine* StateMachineNode,
	FString& OutError)
{
	return FAnimStateMachineEditor::GetStateMachineGraph(StateMachineNode, OutError);
}

// ===== State Operations (Level 3) =====

UAnimStateNode* FAnimationBlueprintUtils::AddState(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& StateName,
	FVector2D Position,
	bool bIsEntryState,
	FString& OutNodeId,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return nullptr;
	}

	UAnimStateNode* Result = FAnimStateMachineEditor::AddState(
		AnimBP, StateMachineName, StateName, Position, bIsEntryState, OutNodeId, OutError);

	if (Result)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return Result;
}

bool FAnimationBlueprintUtils::RemoveState(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& StateName,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	bool bResult = FAnimStateMachineEditor::RemoveState(AnimBP, StateMachineName, StateName, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

UAnimStateNode* FAnimationBlueprintUtils::FindState(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& StateName,
	FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("AnimBlueprint is null");
		return nullptr;
	}

	return FAnimStateMachineEditor::FindState(AnimBP, StateMachineName, StateName, OutError);
}

TArray<UAnimStateNode*> FAnimationBlueprintUtils::GetAllStates(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("AnimBlueprint is null");
		return TArray<UAnimStateNode*>();
	}

	return FAnimStateMachineEditor::GetAllStates(AnimBP, StateMachineName, OutError);
}

bool FAnimationBlueprintUtils::SetEntryState(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& StateName,
	FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("AnimBlueprint is null");
		return false;
	}

	return FAnimStateMachineEditor::SetEntryState(AnimBP, StateMachineName, StateName, OutError);
}

// ===== Transition Operations (Level 3) =====

UAnimStateTransitionNode* FAnimationBlueprintUtils::CreateTransition(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	FString& OutNodeId,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return nullptr;
	}

	UAnimStateTransitionNode* Result = FAnimStateMachineEditor::CreateTransition(
		AnimBP, StateMachineName, FromStateName, ToStateName, OutNodeId, OutError);

	if (Result)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return Result;
}

bool FAnimationBlueprintUtils::RemoveTransition(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	bool bResult = FAnimStateMachineEditor::RemoveTransition(
		AnimBP, StateMachineName, FromStateName, ToStateName, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

UAnimStateTransitionNode* FAnimationBlueprintUtils::FindTransition(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("AnimBlueprint is null");
		return nullptr;
	}

	return FAnimStateMachineEditor::FindTransition(
		AnimBP, StateMachineName, FromStateName, ToStateName, OutError);
}

bool FAnimationBlueprintUtils::SetTransitionDuration(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	float Duration,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	bool bResult = FAnimStateMachineEditor::SetTransitionDuration(
		AnimBP, StateMachineName, FromStateName, ToStateName, Duration, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

bool FAnimationBlueprintUtils::SetTransitionPriority(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	int32 Priority,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	bool bResult = FAnimStateMachineEditor::SetTransitionPriority(
		AnimBP, StateMachineName, FromStateName, ToStateName, Priority, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

TArray<UAnimStateTransitionNode*> FAnimationBlueprintUtils::GetAllTransitions(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("AnimBlueprint is null");
		return TArray<UAnimStateTransitionNode*>();
	}

	return FAnimStateMachineEditor::GetAllTransitions(AnimBP, StateMachineName, OutError);
}

// ===== Transition Condition Graph Operations (Level 4) =====

UEdGraph* FAnimationBlueprintUtils::GetTransitionGraph(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	FString& OutError)
{
	return FAnimGraphEditor::FindTransitionGraph(AnimBP, StateMachineName, FromStateName, ToStateName, OutError);
}

UEdGraphNode* FAnimationBlueprintUtils::AddConditionNode(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	const FString& NodeType,
	const TSharedPtr<FJsonObject>& Params,
	FVector2D Position,
	FString& OutNodeId,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return nullptr;
	}

	// Find transition graph
	UEdGraph* TransitionGraph = FAnimGraphEditor::FindTransitionGraph(
		AnimBP, StateMachineName, FromStateName, ToStateName, OutError);

	if (!TransitionGraph)
	{
		return nullptr;
	}

	UEdGraphNode* Result = FAnimGraphEditor::CreateTransitionConditionNode(
		TransitionGraph, NodeType, Params, Position.X, Position.Y, OutNodeId, OutError);

	if (Result)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return Result;
}

bool FAnimationBlueprintUtils::DeleteConditionNode(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	const FString& NodeId,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	// Find transition graph
	UEdGraph* TransitionGraph = FAnimGraphEditor::FindTransitionGraph(
		AnimBP, StateMachineName, FromStateName, ToStateName, OutError);

	if (!TransitionGraph)
	{
		return false;
	}

	// Find the node by ID
	UEdGraphNode* NodeToDelete = FAnimGraphEditor::FindNodeById(TransitionGraph, NodeId);
	if (!NodeToDelete)
	{
		OutError = FString::Printf(TEXT("Node with ID '%s' not found in transition graph"), *NodeId);
		return false;
	}

	// Don't allow deleting the result node
	UEdGraphNode* ResultNode = FAnimGraphEditor::FindResultNode(TransitionGraph);
	if (NodeToDelete == ResultNode)
	{
		OutError = TEXT("Cannot delete the transition result node");
		return false;
	}

	// Break all connections first
	NodeToDelete->BreakAllNodeLinks();

	// Remove the node from the graph
	TransitionGraph->RemoveNode(NodeToDelete);

	MarkAnimBlueprintModified(AnimBP);

	return true;
}

bool FAnimationBlueprintUtils::ConnectConditionNodes(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	const FString& SourceNodeId,
	const FString& SourcePinName,
	const FString& TargetNodeId,
	const FString& TargetPinName,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	UEdGraph* TransitionGraph = FAnimGraphEditor::FindTransitionGraph(
		AnimBP, StateMachineName, FromStateName, ToStateName, OutError);

	if (!TransitionGraph)
	{
		return false;
	}

	bool bResult = FAnimGraphEditor::ConnectTransitionNodes(
		TransitionGraph, SourceNodeId, SourcePinName, TargetNodeId, TargetPinName, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

bool FAnimationBlueprintUtils::ConnectToTransitionResult(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromStateName,
	const FString& ToStateName,
	const FString& ConditionNodeId,
	const FString& ConditionPinName,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	UEdGraph* TransitionGraph = FAnimGraphEditor::FindTransitionGraph(
		AnimBP, StateMachineName, FromStateName, ToStateName, OutError);

	if (!TransitionGraph)
	{
		return false;
	}

	bool bResult = FAnimGraphEditor::ConnectToTransitionResult(
		TransitionGraph, ConditionNodeId, ConditionPinName, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

// ===== Animation Assignment Operations (Level 5) =====

bool FAnimationBlueprintUtils::SetStateAnimSequence(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& StateName,
	const FString& AnimSequencePath,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	// Load the animation sequence
	UAnimSequence* AnimSequence = FAnimAssetManager::LoadAnimSequence(AnimSequencePath, OutError);
	if (!AnimSequence)
	{
		return false;
	}

	// Validate compatibility
	if (!FAnimAssetManager::ValidateAnimationCompatibility(AnimBP, AnimSequence, OutError))
	{
		return false;
	}

	// Set the animation
	bool bResult = FAnimAssetManager::SetStateAnimSequence(
		AnimBP, StateMachineName, StateName, AnimSequence, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

bool FAnimationBlueprintUtils::SetStateBlendSpace(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& StateName,
	const FString& BlendSpacePath,
	const TMap<FString, FString>& ParameterBindings,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	UBlendSpace* BlendSpace = FAnimAssetManager::LoadBlendSpace(BlendSpacePath, OutError);
	if (!BlendSpace)
	{
		return false;
	}

	if (!FAnimAssetManager::ValidateAnimationCompatibility(AnimBP, Cast<UAnimationAsset>(BlendSpace), OutError))
	{
		return false;
	}

	bool bResult = FAnimAssetManager::SetStateBlendSpace(
		AnimBP, StateMachineName, StateName, BlendSpace, ParameterBindings, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

bool FAnimationBlueprintUtils::SetStateBlendSpace1D(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& StateName,
	const FString& BlendSpacePath,
	const FString& ParameterBinding,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	UBlendSpace1D* BlendSpace = FAnimAssetManager::LoadBlendSpace1D(BlendSpacePath, OutError);
	if (!BlendSpace)
	{
		return false;
	}

	if (!FAnimAssetManager::ValidateAnimationCompatibility(AnimBP, Cast<UAnimationAsset>(BlendSpace), OutError))
	{
		return false;
	}

	bool bResult = FAnimAssetManager::SetStateBlendSpace1D(
		AnimBP, StateMachineName, StateName, BlendSpace, ParameterBinding, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

bool FAnimationBlueprintUtils::SetStateMontage(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& StateName,
	const FString& MontagePath,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	UAnimMontage* Montage = FAnimAssetManager::LoadMontage(MontagePath, OutError);
	if (!Montage)
	{
		return false;
	}

	if (!FAnimAssetManager::ValidateAnimationCompatibility(AnimBP, Montage, OutError))
	{
		return false;
	}

	bool bResult = FAnimAssetManager::SetStateMontage(
		AnimBP, StateMachineName, StateName, Montage, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

TArray<FString> FAnimationBlueprintUtils::FindAnimationAssets(
	const FString& SearchPattern,
	const FString& AssetType,
	UAnimBlueprint* AnimBPForSkeleton)
{
	USkeleton* Skeleton = nullptr;
	if (AnimBPForSkeleton)
	{
		Skeleton = FAnimAssetManager::GetTargetSkeleton(AnimBPForSkeleton);
	}

	return FAnimAssetManager::FindAnimationAssets(SearchPattern, AssetType, Skeleton);
}

// ===== Serialization =====

FString FAnimationBlueprintUtils::GetBlueprintCompileStatusString(const UAnimBlueprint* AnimBP)
{
	if (!AnimBP)
	{
		return TEXT("Unknown");
	}

	switch (AnimBP->Status)
	{
	case BS_Unknown:
		return TEXT("Unknown");
	case BS_Dirty:
		return TEXT("Dirty");
	case BS_Error:
		return TEXT("Error");
	case BS_UpToDate:
		return TEXT("UpToDate");
	case BS_UpToDateWithWarnings:
		return TEXT("UpToDateWithWarnings");
	default:
		return TEXT("Unknown");
	}
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::SerializeAnimBlueprintInfo(UAnimBlueprint* AnimBP)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!AnimBP)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("AnimBlueprint is null"));
		return Result;
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), AnimBP->GetName());
	Result->SetStringField(TEXT("path"), AnimBP->GetPathName());
	Result->SetStringField(TEXT("compile_status"), GetBlueprintCompileStatusString(AnimBP));

	if (AnimBP->ParentClass)
	{
		Result->SetStringField(TEXT("parent_class"), AnimBP->ParentClass->GetName());
		Result->SetStringField(TEXT("parent_class_path"), AnimBP->ParentClass->GetPathName());
	}

	if (AnimBP->GeneratedClass)
	{
		Result->SetStringField(TEXT("generated_class"), AnimBP->GeneratedClass->GetName());
		Result->SetStringField(TEXT("generated_class_path"), AnimBP->GeneratedClass->GetPathName());
	}

	// Skeleton info
	USkeleton* Skeleton = FAnimAssetManager::GetTargetSkeleton(AnimBP);
	if (Skeleton)
	{
		Result->SetStringField(TEXT("skeleton"), Skeleton->GetName());
		Result->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
	}

	if (USkeletalMesh* PreviewMesh = AnimBP->GetPreviewMesh())
	{
		Result->SetStringField(TEXT("preview_mesh"), PreviewMesh->GetName());
		Result->SetStringField(TEXT("preview_mesh_path"), PreviewMesh->GetPathName());
	}

	// State machines
	TArray<TSharedPtr<FJsonValue>> StateMachinesArray;
	TArray<UAnimGraphNode_StateMachine*> StateMachines = GetAllStateMachines(AnimBP);
	for (UAnimGraphNode_StateMachine* SM : StateMachines)
	{
		TSharedPtr<FJsonObject> SMInfo = FAnimStateMachineEditor::SerializeStateMachineInfo(SM);
		StateMachinesArray.Add(MakeShared<FJsonValueObject>(SMInfo));
	}
	Result->SetArrayField(TEXT("state_machines"), StateMachinesArray);

	return Result;
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::BuildAnimationPreflightReport(
	UAnimBlueprint* AnimBP,
	const TArray<FString>& RequiredRoles,
	UBlueprint* ActorBlueprint,
	const FString& ComponentName,
	int32 MatchLimit,
	const FString& ImportedAssetRoot,
	const FString& PackIdentifier,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!AnimBP)
	{
		OutError = TEXT("Invalid Animation Blueprint");
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	MatchLimit = FMath::Clamp(MatchLimit, 1, 20);

	FString RootValidationError;
	const FString ScopedAssetRoot = NormalizeAssetRootForPreflight(ImportedAssetRoot, RootValidationError);
	if (!ImportedAssetRoot.TrimStartAndEnd().IsEmpty() && !RootValidationError.IsEmpty())
	{
		OutError = RootValidationError;
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	const bool bHasImportedRoot = !ScopedAssetRoot.IsEmpty();
	const FString EffectiveAssetRoot = bHasImportedRoot ? ScopedAssetRoot : FString(TEXT("/Game"));

	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(TEXT("mutates_assets"), false);
	Result->SetBoolField(TEXT("local_validation_performed"), true);
	Result->SetBoolField(TEXT("needs_post_import_validation"), false);
	Result->SetStringField(TEXT("post_import_validation_status"), TEXT("local_validation_performed"));
	Result->SetStringField(TEXT("target_anim_blueprint_path"), AnimBP->GetPathName());
	Result->SetStringField(TEXT("target_anim_blueprint_name"), AnimBP->GetName());
	Result->SetStringField(TEXT("actor_blueprint_path"), ActorBlueprint ? ActorBlueprint->GetPathName() : FString());
	Result->SetArrayField(TEXT("required_animation_roles"), MakeStringJsonArray(RequiredRoles));
	Result->SetObjectField(TEXT("anim_blueprint"), SerializeAnimBlueprintInfo(AnimBP));
	Result->SetStringField(TEXT("asset_scope_root"), EffectiveAssetRoot);
	if (bHasImportedRoot)
	{
		Result->SetStringField(TEXT("imported_asset_root"), EffectiveAssetRoot);
	}
	if (!PackIdentifier.IsEmpty())
	{
		Result->SetStringField(TEXT("imported_pack_identifier"), PackIdentifier);
	}

	TSharedPtr<FJsonObject> AssetScope = MakeShared<FJsonObject>();
	AssetScope->SetStringField(TEXT("type"), bHasImportedRoot ? TEXT("local_imported_asset_root") : TEXT("whole_project_local_assets"));
	AssetScope->SetStringField(TEXT("root"), EffectiveAssetRoot);
	AssetScope->SetBoolField(TEXT("root_filter_applied"), bHasImportedRoot);
	AssetScope->SetBoolField(TEXT("local_only"), true);
	Result->SetObjectField(TEXT("asset_scope"), AssetScope);

	if (ActorBlueprint)
	{
		Result->SetObjectField(TEXT("actor_blueprint"), BuildActorBlueprintInventory(ActorBlueprint, AnimBP, ComponentName));
	}

	TArray<TSharedPtr<FJsonValue>> StateMachineArray;
	int32 TotalStates = 0;
	int32 TotalTransitions = 0;
	const TArray<UAnimGraphNode_StateMachine*> StateMachines = GetAllStateMachines(AnimBP);
	for (UAnimGraphNode_StateMachine* StateMachine : StateMachines)
	{
		if (!StateMachine)
		{
			continue;
		}

		TSharedPtr<FJsonObject> StateMachineInfo =
			SerializeStateMachineInfo(AnimBP, StateMachine->GetStateMachineName());
		if (!StateMachineInfo.IsValid())
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
		if (StateMachineInfo->TryGetArrayField(TEXT("states"), States) && States)
		{
			StateMachineInfo->SetNumberField(TEXT("state_count"), States->Num());
			TotalStates += States->Num();
		}

		const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
		if (StateMachineInfo->TryGetArrayField(TEXT("transitions"), Transitions) && Transitions)
		{
			StateMachineInfo->SetNumberField(TEXT("transition_count"), Transitions->Num());
			TotalTransitions += Transitions->Num();
		}

		StateMachineArray.Add(MakeShared<FJsonValueObject>(StateMachineInfo));
	}

	Result->SetArrayField(TEXT("state_machines"), StateMachineArray);
	Result->SetNumberField(TEXT("state_machine_count"), StateMachineArray.Num());
	Result->SetNumberField(TEXT("state_count"), TotalStates);
	Result->SetNumberField(TEXT("transition_count"), TotalTransitions);

	TArray<FString> BlockerCodes;
	TArray<FString> RecommendationCodes;
	TArray<FString> MissingRoles;
	TArray<FString> UnsatisfiedRoles;
	AddUniqueString(RecommendationCodes, TEXT("anim_preflight_read_only"));

	USkeleton* TargetSkeleton = FAnimAssetManager::GetTargetSkeleton(AnimBP);
	if (!TargetSkeleton)
	{
		AddUniqueString(BlockerCodes, TEXT("unknown_skeleton_compatibility"));
		AddUniqueString(UnsatisfiedRoles, TEXT("target_skeleton_missing"));
	}

	TArray<FAssetData> AssetInventory;
	QueryAllAnimationAssets(AssetInventory, EffectiveAssetRoot);
	Result->SetNumberField(TEXT("discovered_animation_asset_count"), AssetInventory.Num());
	Result->SetObjectField(TEXT("skeleton_summary"), BuildSkeletonSummaryForAssets(AssetInventory, TargetSkeleton));

	TArray<TSharedPtr<FJsonValue>> RoleInventoryArray;
	TArray<TSharedPtr<FJsonValue>> ActorRoleBindingArray;
	TArray<FString> CompatibleRoles;
	for (const FString& Role : RequiredRoles)
	{
		TSharedPtr<FJsonObject> RoleObject = MakeShared<FJsonObject>();
		RoleObject->SetStringField(TEXT("role"), Role);
		TSharedPtr<FJsonObject> ActorRoleBinding;
		if (ActorBlueprint && !bHasImportedRoot)
		{
			ActorRoleBinding = BuildActorRoleBindingReport(ActorBlueprint, AnimBP, Role);
			ActorRoleBindingArray.Add(MakeShared<FJsonValueObject>(ActorRoleBinding));
			RoleObject->SetObjectField(TEXT("actor_role_binding"), ActorRoleBinding);
		}

		const TArray<FString> SearchTerms = BuildRoleSearchTerms(Role);
		RoleObject->SetArrayField(TEXT("search_terms"), MakeStringJsonArray(SearchTerms));

		struct FRoleCandidate
		{
			FAssetData AssetData;
			int32 Score = 0;
			int32 MatchedTerms = 0;
		};

		TArray<FRoleCandidate> Candidates;
		for (const FAssetData& AssetData : AssetInventory)
		{
			const FString NormalizedAssetName = NormalizePreflightToken(AssetData.AssetName.ToString());
			const FString NormalizedAssetPath = NormalizePreflightToken(AssetData.GetObjectPathString());
			int32 MatchedTerms = 0;
			const int32 Score = ScoreAssetForRole(Role, SearchTerms, NormalizedAssetName, NormalizedAssetPath, MatchedTerms);
			if (Score > 0)
			{
				FRoleCandidate& Candidate = Candidates.AddDefaulted_GetRef();
				Candidate.AssetData = AssetData;
				Candidate.Score = Score;
				Candidate.MatchedTerms = MatchedTerms;
			}
		}

		Candidates.Sort([](const FRoleCandidate& A, const FRoleCandidate& B)
		{
			if (A.Score != B.Score)
			{
				return A.Score > B.Score;
			}
			return A.AssetData.AssetName.ToString() < B.AssetData.AssetName.ToString();
		});

		TArray<TSharedPtr<FJsonValue>> MatchesArray;
		bool bHasCompatibleMatch = false;
		bool bHasIncompatibleMatch = false;
		bool bHasUnknownCompatibility = false;

		for (int32 Index = 0; Index < Candidates.Num() && MatchesArray.Num() < MatchLimit; ++Index)
		{
			UAnimationAsset* AnimationAsset = Cast<UAnimationAsset>(Candidates[Index].AssetData.GetAsset());
			TSharedPtr<FJsonObject> AssetInfo = FAnimAssetManager::SerializeAnimAssetInfo(AnimationAsset);
			if (!AssetInfo.IsValid())
			{
				AssetInfo = MakeShared<FJsonObject>();
			}

			FString ExistingPath;
			if (!AssetInfo->TryGetStringField(TEXT("path"), ExistingPath) || ExistingPath.IsEmpty())
			{
				AssetInfo->SetStringField(TEXT("name"), Candidates[Index].AssetData.AssetName.ToString());
				AssetInfo->SetStringField(TEXT("path"), Candidates[Index].AssetData.GetObjectPathString());
				AssetInfo->SetStringField(TEXT("class"), Candidates[Index].AssetData.AssetClassPath.ToString());
			}

			AssetInfo->SetStringField(TEXT("asset_type"), AnimationAsset ? AnimationAsset->GetClass()->GetName() : Candidates[Index].AssetData.AssetClassPath.ToString());
			AssetInfo->SetNumberField(TEXT("match_score"), Candidates[Index].Score);
			AssetInfo->SetNumberField(TEXT("matched_term_count"), Candidates[Index].MatchedTerms);

			TSharedPtr<FJsonObject> CompatibilityInfo =
				FAnimAssetManager::DescribeAnimationCompatibility(AnimBP, AnimationAsset);
			const FString CompatibilityStatus =
				CompatibilityInfo.IsValid() ? CompatibilityInfo->GetStringField(TEXT("status")) : TEXT("unknown_unloaded_asset");
			const bool bBrokenCandidate = CompatibilityStatus == TEXT("unknown_missing_asset_skeleton")
				|| CompatibilityStatus == TEXT("unknown_unloaded_asset");

			AssetInfo->SetStringField(TEXT("skeleton_compatibility"), bBrokenCandidate ? TEXT("broken_candidate_animation") : CompatibilityStatus);
			if (bBrokenCandidate)
			{
				AssetInfo->SetStringField(TEXT("asset_status"), TEXT("broken_candidate_animation"));
			}
			if (CompatibilityInfo.IsValid())
			{
				AssetInfo->SetObjectField(TEXT("compatibility"), CompatibilityInfo);
			}

			if (CompatibilityStatus == TEXT("compatible"))
			{
				bHasCompatibleMatch = true;
			}
			else if (CompatibilityStatus == TEXT("incompatible"))
			{
				bHasIncompatibleMatch = true;
			}
			else
			{
				bHasUnknownCompatibility = true;
			}

			MatchesArray.Add(MakeShared<FJsonValueObject>(AssetInfo));
		}

		RoleObject->SetArrayField(TEXT("matches"), MatchesArray);

		FString RoleStatus = TEXT("matched");
		if (MatchesArray.Num() == 0)
		{
			RoleStatus = TEXT("missing_manual_blocker");
			AddUniqueString(MissingRoles, Role);
			AddUniqueString(UnsatisfiedRoles, Role);
			AddUniqueString(BlockerCodes, TEXT("manual_asset_dependency_blocker"));
			AddUniqueString(BlockerCodes, TEXT("animation_placeholder_mode"));
		}
		else if (!bHasCompatibleMatch)
		{
			AddUniqueString(UnsatisfiedRoles, Role);
			if (bHasIncompatibleMatch)
			{
				RoleStatus = TEXT("matched_with_skeleton_mismatch");
				AddUniqueString(BlockerCodes, TEXT("skeleton_mismatch"));
				AddUniqueString(BlockerCodes, TEXT("retarget_required_not_implemented"));
				AddUniqueString(RecommendationCodes, TEXT("run_animation_retarget_fixup"));
			}
			else
			{
				RoleStatus = TEXT("matched_with_broken_candidate_animation");
				AddUniqueString(BlockerCodes, TEXT("broken_candidate_animation"));
				AddUniqueString(BlockerCodes, TEXT("unknown_skeleton_compatibility"));
			}
		}

		if (ActorRoleBinding.IsValid() && ActorRoleBinding->GetBoolField(TEXT("binding_evaluated")))
		{
			const FString BindingStatus = ActorRoleBinding->GetStringField(TEXT("status"));
			if (BindingStatus == TEXT("assigned_reference_ready"))
			{
				RoleStatus = TEXT("matched_with_actor_binding");
				UnsatisfiedRoles.Remove(Role);
				MissingRoles.Remove(Role);
			}
			else
			{
				RoleStatus = BindingStatus;
				AddUniqueString(UnsatisfiedRoles, Role);
				if (BindingStatus == TEXT("assigned_reference_skeleton_mismatch"))
				{
					AddUniqueString(BlockerCodes, TEXT("animation_compatibility_gate_failed"));
					AddUniqueString(BlockerCodes, TEXT("skeleton_mismatch"));
				}
				else if (BindingStatus == TEXT("assigned_reference_semantic_mismatch"))
				{
					AddUniqueString(BlockerCodes, TEXT("animation_role_binding_semantic_mismatch"));
				}
				else
				{
					AddUniqueString(BlockerCodes, TEXT("animation_role_binding_missing"));
				}
			}
		}

		RoleObject->SetStringField(TEXT("status"), RoleStatus);
		if (RoleStatus == TEXT("matched") || RoleStatus == TEXT("matched_with_actor_binding"))
		{
			AddUniqueString(CompatibleRoles, Role);
		}
		RoleInventoryArray.Add(MakeShared<FJsonValueObject>(RoleObject));
	}

	if (RequiredRoles.Num() > 0 && UnsatisfiedRoles.Num() == 0 && MissingRoles.Num() == 0)
	{
		BlockerCodes.Remove(TEXT("manual_asset_dependency_blocker"));
		BlockerCodes.Remove(TEXT("animation_placeholder_mode"));
		BlockerCodes.Remove(TEXT("skeleton_mismatch"));
		BlockerCodes.Remove(TEXT("retarget_required_not_implemented"));
		BlockerCodes.Remove(TEXT("broken_candidate_animation"));
		BlockerCodes.Remove(TEXT("unknown_skeleton_compatibility"));
	}

	Result->SetArrayField(TEXT("role_inventory"), RoleInventoryArray);
	Result->SetArrayField(TEXT("actor_role_bindings"), ActorRoleBindingArray);
	Result->SetArrayField(TEXT("compatible_roles"), MakeStringJsonArray(CompatibleRoles));
	Result->SetArrayField(TEXT("missing_roles"), MakeStringJsonArray(MissingRoles));
	Result->SetArrayField(TEXT("unsatisfied_roles"), MakeStringJsonArray(UnsatisfiedRoles));
	Result->SetArrayField(TEXT("blocker_codes"), MakeStringJsonArray(BlockerCodes));
	Result->SetArrayField(TEXT("recommendation_codes"), MakeStringJsonArray(RecommendationCodes));

	const bool bManualBlocker = BlockerCodes.Num() > 0;
	Result->SetBoolField(TEXT("preflight_ready_for_manual_verification"), !bManualBlocker);
	Result->SetBoolField(TEXT("ready_to_claim_visual_animation"), false);
	Result->SetStringField(TEXT("proof_classification"), bManualBlocker ? TEXT("manual_blocker") : TEXT("gameplay_code_proof"));

	TSharedPtr<FJsonObject> ProofMatrix = MakeShared<FJsonObject>();
	ProofMatrix->SetBoolField(TEXT("gameplay_code_proof"), !bManualBlocker);
	ProofMatrix->SetBoolField(TEXT("runtime_proof"), false);
	ProofMatrix->SetBoolField(TEXT("visual_animation_proof"), false);
	ProofMatrix->SetBoolField(TEXT("manual_blocker"), bManualBlocker);
	ProofMatrix->SetBoolField(TEXT("animation_inventory_proof"), true);
	Result->SetObjectField(TEXT("proof_matrix"), ProofMatrix);

	Result->SetStringField(
		TEXT("summary"),
		FString::Printf(
			TEXT("Read-only animation preflight for '%s': %d required roles, %d missing, %d unsatisfied, proof=%s"),
			*AnimBP->GetName(),
			RequiredRoles.Num(),
			MissingRoles.Num(),
			UnsatisfiedRoles.Num(),
			bManualBlocker ? TEXT("manual_blocker") : TEXT("gameplay_code_proof"))
	);

	return Result;
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::SerializeStateMachineInfo(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName)
{
	FString Error;
	UAnimGraphNode_StateMachine* SM = FindStateMachine(AnimBP, StateMachineName, Error);

	if (!SM)
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetBoolField(TEXT("success"), false);
		ErrorResult->SetStringField(TEXT("error"), Error);
		return ErrorResult;
	}

	TSharedPtr<FJsonObject> Result = FAnimStateMachineEditor::SerializeStateMachineInfo(SM);
	Result->SetBoolField(TEXT("success"), true);

	// Add states
	TArray<TSharedPtr<FJsonValue>> StatesArray;
	TArray<UAnimStateNode*> States = GetAllStates(AnimBP, StateMachineName, Error);
	for (UAnimStateNode* State : States)
	{
		StatesArray.Add(MakeShared<FJsonValueObject>(SerializeStateInfo(State)));
	}
	Result->SetArrayField(TEXT("states"), StatesArray);

	// Add transitions
	TArray<TSharedPtr<FJsonValue>> TransitionsArray;
	TArray<UAnimStateTransitionNode*> Transitions = GetAllTransitions(AnimBP, StateMachineName, Error);
	for (UAnimStateTransitionNode* Transition : Transitions)
	{
		TransitionsArray.Add(MakeShared<FJsonValueObject>(SerializeTransitionInfo(Transition)));
	}
	Result->SetArrayField(TEXT("transitions"), TransitionsArray);

	return Result;
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::SerializeStateInfo(UAnimStateNode* StateNode)
{
	return FAnimStateMachineEditor::SerializeStateInfo(StateNode);
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::SerializeTransitionInfo(UAnimStateTransitionNode* TransitionNode)
{
	return FAnimStateMachineEditor::SerializeTransitionInfo(TransitionNode);
}

// ===== Batch Operations =====

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::ExecuteBatchOperations(
	UAnimBlueprint* AnimBP,
	const TArray<TSharedPtr<FJsonValue>>& Operations,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;
	int32 FailureCount = 0;

	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	for (const TSharedPtr<FJsonValue>& OpValue : Operations)
	{
		TSharedPtr<FJsonObject> OpResult = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject>* OpObj;

		if (!OpValue->TryGetObject(OpObj) || !OpObj->IsValid())
		{
			OpResult->SetBoolField(TEXT("success"), false);
			OpResult->SetStringField(TEXT("error"), TEXT("Invalid operation format"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(OpResult));
			FailureCount++;
			continue;
		}

		FString OpType = (*OpObj)->GetStringField(TEXT("type"));
		const TSharedPtr<FJsonObject>* ParamsObj;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if ((*OpObj)->TryGetObjectField(TEXT("params"), ParamsObj))
		{
			Params = *ParamsObj;
		}

		FString OpError;
		bool bOpSuccess = false;

		// Execute operation based on type
		if (OpType == TEXT("add_state"))
		{
			FString NodeId;
			UAnimStateNode* State = AddState(
				AnimBP,
				Params->GetStringField(TEXT("state_machine")),
				Params->GetStringField(TEXT("state_name")),
				FVector2D(Params->GetNumberField(TEXT("x")), Params->GetNumberField(TEXT("y"))),
				Params->GetBoolField(TEXT("is_entry")),
				NodeId,
				OpError
			);
			bOpSuccess = (State != nullptr);
			if (bOpSuccess)
			{
				OpResult->SetStringField(TEXT("node_id"), NodeId);
			}
		}
		else if (OpType == TEXT("remove_state"))
		{
			bOpSuccess = RemoveState(
				AnimBP,
				Params->GetStringField(TEXT("state_machine")),
				Params->GetStringField(TEXT("state_name")),
				OpError
			);
		}
		else if (OpType == TEXT("add_transition"))
		{
			FString NodeId;
			UAnimStateTransitionNode* Transition = CreateTransition(
				AnimBP,
				Params->GetStringField(TEXT("state_machine")),
				Params->GetStringField(TEXT("from_state")),
				Params->GetStringField(TEXT("to_state")),
				NodeId,
				OpError
			);
			bOpSuccess = (Transition != nullptr);
			if (bOpSuccess)
			{
				OpResult->SetStringField(TEXT("node_id"), NodeId);
			}
		}
		else if (OpType == TEXT("remove_transition"))
		{
			bOpSuccess = RemoveTransition(
				AnimBP,
				Params->GetStringField(TEXT("state_machine")),
				Params->GetStringField(TEXT("from_state")),
				Params->GetStringField(TEXT("to_state")),
				OpError
			);
		}
		else if (OpType == TEXT("set_transition_duration"))
		{
			bOpSuccess = SetTransitionDuration(
				AnimBP,
				Params->GetStringField(TEXT("state_machine")),
				Params->GetStringField(TEXT("from_state")),
				Params->GetStringField(TEXT("to_state")),
				Params->GetNumberField(TEXT("duration")),
				OpError
			);
		}
		else if (OpType == TEXT("set_state_animation"))
		{
			FString AnimType = Params->GetStringField(TEXT("animation_type"));
			if (AnimType == TEXT("sequence") || AnimType.IsEmpty())
			{
				bOpSuccess = SetStateAnimSequence(
					AnimBP,
					Params->GetStringField(TEXT("state_machine")),
					Params->GetStringField(TEXT("state_name")),
					Params->GetStringField(TEXT("animation_path")),
					OpError
				);
			}
			else if (AnimType == TEXT("blendspace"))
			{
				TMap<FString, FString> Bindings;
				const TSharedPtr<FJsonObject>* BindingsObj;
				if (Params->TryGetObjectField(TEXT("parameter_bindings"), BindingsObj))
				{
					for (const auto& Pair : (*BindingsObj)->Values)
					{
						Bindings.Add(Pair.Key, Pair.Value->AsString());
					}
				}
				bOpSuccess = SetStateBlendSpace(
					AnimBP,
					Params->GetStringField(TEXT("state_machine")),
					Params->GetStringField(TEXT("state_name")),
					Params->GetStringField(TEXT("animation_path")),
					Bindings,
					OpError
				);
			}
			else if (AnimType == TEXT("blendspace1d"))
			{
				bOpSuccess = SetStateBlendSpace1D(
					AnimBP,
					Params->GetStringField(TEXT("state_machine")),
					Params->GetStringField(TEXT("state_name")),
					Params->GetStringField(TEXT("animation_path")),
					Params->GetStringField(TEXT("parameter_binding")),
					OpError
				);
			}
			else if (AnimType == TEXT("montage"))
			{
				bOpSuccess = SetStateMontage(
					AnimBP,
					Params->GetStringField(TEXT("state_machine")),
					Params->GetStringField(TEXT("state_name")),
					Params->GetStringField(TEXT("animation_path")),
					OpError
				);
			}
		}
		else if (OpType == TEXT("add_comparison_chain"))
		{
			FVector2D Position(
				Params->GetNumberField(TEXT("x")),
				Params->GetNumberField(TEXT("y"))
			);
			TSharedPtr<FJsonObject> ChainResult = AddComparisonChain(
				AnimBP,
				Params->GetStringField(TEXT("state_machine")),
				Params->GetStringField(TEXT("from_state")),
				Params->GetStringField(TEXT("to_state")),
				Params->GetStringField(TEXT("variable_name")),
				Params->GetStringField(TEXT("comparison_type")),
				Params->GetStringField(TEXT("compare_value")),
				Position,
				OpError
			);
			bOpSuccess = ChainResult.IsValid() && ChainResult->GetBoolField(TEXT("success"));
			if (bOpSuccess)
			{
				// Include node IDs in the result
				if (ChainResult->HasField(TEXT("variable_node_id")))
				{
					OpResult->SetStringField(TEXT("variable_node_id"), ChainResult->GetStringField(TEXT("variable_node_id")));
				}
				if (ChainResult->HasField(TEXT("comparison_node_id")))
				{
					OpResult->SetStringField(TEXT("comparison_node_id"), ChainResult->GetStringField(TEXT("comparison_node_id")));
				}
			}
			else if (ChainResult.IsValid() && ChainResult->HasField(TEXT("error")))
			{
				OpError = ChainResult->GetStringField(TEXT("error"));
			}
		}
		else if (OpType == TEXT("add_condition_node"))
		{
			FVector2D Position(
				Params->GetNumberField(TEXT("x")),
				Params->GetNumberField(TEXT("y"))
			);
			FString NodeId;
			TSharedPtr<FJsonObject> NodeParams;
			const TSharedPtr<FJsonObject>* NodeParamsPtr;
			if (Params->TryGetObjectField(TEXT("node_params"), NodeParamsPtr))
			{
				NodeParams = *NodeParamsPtr;
			}
			UEdGraphNode* Node = AddConditionNode(
				AnimBP,
				Params->GetStringField(TEXT("state_machine")),
				Params->GetStringField(TEXT("from_state")),
				Params->GetStringField(TEXT("to_state")),
				Params->GetStringField(TEXT("node_type")),
				NodeParams,
				Position,
				NodeId,
				OpError
			);
			bOpSuccess = (Node != nullptr);
			if (bOpSuccess)
			{
				OpResult->SetStringField(TEXT("node_id"), NodeId);
			}
		}
		else if (OpType == TEXT("connect_condition_nodes"))
		{
			bOpSuccess = ConnectConditionNodes(
				AnimBP,
				Params->GetStringField(TEXT("state_machine")),
				Params->GetStringField(TEXT("from_state")),
				Params->GetStringField(TEXT("to_state")),
				Params->GetStringField(TEXT("source_node_id")),
				Params->GetStringField(TEXT("source_pin")),
				Params->GetStringField(TEXT("target_node_id")),
				Params->GetStringField(TEXT("target_pin")),
				OpError
			);
		}
		else if (OpType == TEXT("connect_to_result"))
		{
			bOpSuccess = ConnectToTransitionResult(
				AnimBP,
				Params->GetStringField(TEXT("state_machine")),
				Params->GetStringField(TEXT("from_state")),
				Params->GetStringField(TEXT("to_state")),
				Params->GetStringField(TEXT("source_node_id")),
				Params->GetStringField(TEXT("source_pin")),
				OpError
			);
		}
		else
		{
			OpError = FString::Printf(TEXT("Unknown operation type: %s"), *OpType);
		}

		OpResult->SetStringField(TEXT("type"), OpType);
		OpResult->SetBoolField(TEXT("success"), bOpSuccess);
		if (!bOpSuccess)
		{
			OpResult->SetStringField(TEXT("error"), OpError);
			FailureCount++;
		}
		else
		{
			SuccessCount++;
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(OpResult));
	}

	// Compile after all operations
	FString CompileError;
	bool bCompiled = CompileAnimBlueprint(AnimBP, CompileError);

	Result->SetBoolField(TEXT("success"), FailureCount == 0 && bCompiled);
	Result->SetNumberField(TEXT("success_count"), SuccessCount);
	Result->SetNumberField(TEXT("failure_count"), FailureCount);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	if (!bCompiled)
	{
		Result->SetStringField(TEXT("compile_error"), CompileError);
	}
	Result->SetArrayField(TEXT("results"), ResultsArray);

	return Result;
}

// ===== New Operations for MCP Tool Enhancements =====

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::GetTransitionNodes(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromState,
	const FString& ToState,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetBoolField(TEXT("success"), false);
		ErrorResult->SetStringField(TEXT("error"), OutError);
		return ErrorResult;
	}

	// If FromState and ToState are empty, get all transitions in state machine
	if (FromState.IsEmpty() && ToState.IsEmpty())
	{
		return FAnimGraphEditor::GetAllTransitionNodes(AnimBP, StateMachineName, OutError);
	}

	// Get single transition graph nodes
	UEdGraph* TransitionGraph = FAnimGraphEditor::FindTransitionGraph(
		AnimBP, StateMachineName, FromState, ToState, OutError);

	if (!TransitionGraph)
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetBoolField(TEXT("success"), false);
		ErrorResult->SetStringField(TEXT("error"), OutError);
		return ErrorResult;
	}

	TSharedPtr<FJsonObject> Result = FAnimGraphEditor::GetTransitionGraphNodes(TransitionGraph, OutError);
	Result->SetStringField(TEXT("from_state"), FromState);
	Result->SetStringField(TEXT("to_state"), ToState);

	return Result;
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::InspectNodePins(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromState,
	const FString& ToState,
	const FString& NodeId,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	// Find transition graph
	UEdGraph* TransitionGraph = FAnimGraphEditor::FindTransitionGraph(
		AnimBP, StateMachineName, FromState, ToState, OutError);

	if (!TransitionGraph)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	// Find the node
	UEdGraphNode* Node = FAnimGraphEditor::FindNodeById(TransitionGraph, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node with ID '%s' not found"), *NodeId);
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	Result->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	Result->SetNumberField(TEXT("pos_x"), Node->NodePosX);
	Result->SetNumberField(TEXT("pos_y"), Node->NodePosY);

	// Detailed pins with full type info
	TArray<TSharedPtr<FJsonValue>> InputPins;
	TArray<TSharedPtr<FJsonValue>> OutputPins;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;

		TSharedPtr<FJsonObject> PinObj = FAnimGraphEditor::SerializeDetailedPinInfo(Pin);

		if (Pin->Direction == EGPD_Input)
		{
			InputPins.Add(MakeShared<FJsonValueObject>(PinObj));
		}
		else
		{
			OutputPins.Add(MakeShared<FJsonValueObject>(PinObj));
		}
	}

	Result->SetArrayField(TEXT("input_pins"), InputPins);
	Result->SetArrayField(TEXT("output_pins"), OutputPins);

	return Result;
}

bool FAnimationBlueprintUtils::SetPinDefaultValue(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromState,
	const FString& ToState,
	const FString& NodeId,
	const FString& PinName,
	const FString& Value,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		return false;
	}

	// Find transition graph
	UEdGraph* TransitionGraph = FAnimGraphEditor::FindTransitionGraph(
		AnimBP, StateMachineName, FromState, ToState, OutError);

	if (!TransitionGraph)
	{
		return false;
	}

	bool bResult = FAnimGraphEditor::SetPinDefaultValueWithValidation(
		TransitionGraph, NodeId, PinName, Value, OutError);

	if (bResult)
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return bResult;
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::AddComparisonChain(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const FString& FromState,
	const FString& ToState,
	const FString& VariableName,
	const FString& ComparisonType,
	const FString& CompareValue,
	FVector2D Position,
	FString& OutError)
{
	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetBoolField(TEXT("success"), false);
		ErrorResult->SetStringField(TEXT("error"), OutError);
		return ErrorResult;
	}

	// Find transition graph
	UEdGraph* TransitionGraph = FAnimGraphEditor::FindTransitionGraph(
		AnimBP, StateMachineName, FromState, ToState, OutError);

	if (!TransitionGraph)
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetBoolField(TEXT("success"), false);
		ErrorResult->SetStringField(TEXT("error"), OutError);
		return ErrorResult;
	}

	TSharedPtr<FJsonObject> Result = FAnimGraphEditor::CreateComparisonChain(
		AnimBP, TransitionGraph, VariableName, ComparisonType, CompareValue, Position, OutError);

	if (Result->GetBoolField(TEXT("success")))
	{
		MarkAnimBlueprintModified(AnimBP);
	}

	return Result;
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::ValidateBlueprint(
	UAnimBlueprint* AnimBP,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!AnimBP)
	{
		OutError = TEXT("Invalid Animation Blueprint");
		Result->SetBoolField(TEXT("success"), false);
		Result->SetBoolField(TEXT("is_valid"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint_path"), AnimBP->GetPathName());
	Result->SetStringField(TEXT("blueprint_name"), AnimBP->GetName());

	// Compile the blueprint to get fresh status
	FKismetEditorUtilities::CompileBlueprint(AnimBP);

	// Build result based on status
	bool bIsValid = (AnimBP->Status != BS_Error);
	Result->SetBoolField(TEXT("is_valid"), bIsValid);

	// Status string
	FString StatusStr;
	switch (AnimBP->Status)
	{
	case BS_Unknown: StatusStr = TEXT("Unknown"); break;
	case BS_Dirty: StatusStr = TEXT("Dirty"); break;
	case BS_Error: StatusStr = TEXT("Error"); break;
	case BS_UpToDate: StatusStr = TEXT("UpToDate"); break;
	case BS_UpToDateWithWarnings: StatusStr = TEXT("UpToDateWithWarnings"); break;
	default: StatusStr = TEXT("Unknown"); break;
	}
	Result->SetStringField(TEXT("status"), StatusStr);

	// Collect info about state machines and their states
	TArray<TSharedPtr<FJsonValue>> StateMachinesInfo;
	TArray<UAnimGraphNode_StateMachine*> StateMachines = GetAllStateMachines(AnimBP);
	int32 TotalStates = 0;
	int32 TotalTransitions = 0;

	for (UAnimGraphNode_StateMachine* SM : StateMachines)
	{
		if (!SM) continue;

		TSharedPtr<FJsonObject> SMObj = MakeShared<FJsonObject>();
		FString SMName = SM->GetStateMachineName();
		SMObj->SetStringField(TEXT("name"), SMName);

		// Get states
		FString Error;
		TArray<UAnimStateNode*> States = FAnimStateMachineEditor::GetAllStates(AnimBP, SMName, Error);
		SMObj->SetNumberField(TEXT("state_count"), States.Num());
		TotalStates += States.Num();

		// Get transitions
		TArray<UAnimStateTransitionNode*> Transitions = FAnimStateMachineEditor::GetAllTransitions(AnimBP, SMName, Error);
		SMObj->SetNumberField(TEXT("transition_count"), Transitions.Num());
		TotalTransitions += Transitions.Num();

		StateMachinesInfo.Add(MakeShared<FJsonValueObject>(SMObj));
	}
	Result->SetArrayField(TEXT("state_machines"), StateMachinesInfo);
	Result->SetNumberField(TEXT("total_state_machines"), StateMachines.Num());
	Result->SetNumberField(TEXT("total_states"), TotalStates);
	Result->SetNumberField(TEXT("total_transitions"), TotalTransitions);

	// Check for common issues
	TArray<TSharedPtr<FJsonValue>> IssuesArray;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	// Check if skeleton is assigned
	if (!AnimBP->TargetSkeleton)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("type"), TEXT("MissingSkeleton"));
		Issue->SetStringField(TEXT("message"), TEXT("Animation Blueprint has no target skeleton assigned"));
		Issue->SetStringField(TEXT("severity"), TEXT("Error"));
		IssuesArray.Add(MakeShared<FJsonValueObject>(Issue));
		ErrorCount++;
	}

	// Check for empty state machines
	for (UAnimGraphNode_StateMachine* SM : StateMachines)
	{
		if (!SM) continue;
		FString Error;
		FString SMName = SM->GetStateMachineName();
		TArray<UAnimStateNode*> States = FAnimStateMachineEditor::GetAllStates(AnimBP, SMName, Error);
		if (States.Num() == 0)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("EmptyStateMachine"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("State machine '%s' has no states"),
				*SMName));
			Issue->SetStringField(TEXT("severity"), TEXT("Warning"));
			IssuesArray.Add(MakeShared<FJsonValueObject>(Issue));
			WarningCount++;
		}
	}

	// Update is_valid based on errors found
	if (ErrorCount > 0)
	{
		bIsValid = false;
		Result->SetBoolField(TEXT("is_valid"), false);
	}

	Result->SetArrayField(TEXT("issues"), IssuesArray);
	Result->SetNumberField(TEXT("error_count"), ErrorCount);
	Result->SetNumberField(TEXT("warning_count"), WarningCount);

	return Result;
}

// ===== Bulk Transition Condition Setup =====

bool FAnimationBlueprintUtils::MatchesWildcard(const FString& StateName, const FString& Pattern)
{
	// "*" matches anything
	if (Pattern == TEXT("*"))
	{
		return true;
	}

	// "Attack_*" matches "Attack_1H_1", "Attack_2H_2", etc.
	if (Pattern.EndsWith(TEXT("*")))
	{
		FString Prefix = Pattern.Left(Pattern.Len() - 1);
		return StateName.StartsWith(Prefix);
	}

	// "*_Idle" matches "Sword_Idle", "Axe_Idle", etc.
	if (Pattern.StartsWith(TEXT("*")))
	{
		FString Suffix = Pattern.Right(Pattern.Len() - 1);
		return StateName.EndsWith(Suffix);
	}

	// "*Combat*" matches "InCombatIdle", "CombatRun", etc.
	if (Pattern.StartsWith(TEXT("*")) && Pattern.EndsWith(TEXT("*")))
	{
		FString Middle = Pattern.Mid(1, Pattern.Len() - 2);
		return StateName.Contains(Middle);
	}

	// Exact match
	return StateName.Equals(Pattern, ESearchCase::IgnoreCase);
}

bool FAnimationBlueprintUtils::MatchesRegex(const FString& StateName, const FString& Pattern)
{
	// Check if pattern looks like regex (starts with ^ or contains special chars)
	bool bIsRegex = Pattern.StartsWith(TEXT("^")) || Pattern.EndsWith(TEXT("$")) ||
		Pattern.Contains(TEXT("\\d")) || Pattern.Contains(TEXT("\\w")) ||
		Pattern.Contains(TEXT("[")) || Pattern.Contains(TEXT("+")) ||
		Pattern.Contains(TEXT("?")) || (Pattern.Contains(TEXT(".")) && Pattern.Contains(TEXT("*")));

	if (!bIsRegex)
	{
		return false;
	}

	FRegexPattern RegexPattern(Pattern);
	FRegexMatcher Matcher(RegexPattern, StateName);
	return Matcher.FindNext();
}

bool FAnimationBlueprintUtils::MatchesPattern(const FString& StateName, const TSharedPtr<FJsonValue>& Pattern)
{
	if (!Pattern.IsValid())
	{
		return false;
	}

	// String pattern (exact, wildcard, or regex)
	if (Pattern->Type == EJson::String)
	{
		FString PatternStr = Pattern->AsString();

		// Try regex first (if it looks like regex)
		if (MatchesRegex(StateName, PatternStr))
		{
			return true;
		}

		// Try wildcard matching
		return MatchesWildcard(StateName, PatternStr);
	}

	// Array pattern (list of states)
	if (Pattern->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& PatternList = Pattern->AsArray();
		for (const TSharedPtr<FJsonValue>& Item : PatternList)
		{
			if (Item.IsValid() && Item->Type == EJson::String)
			{
				if (StateName.Equals(Item->AsString(), ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	return false;
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::SetupTransitionConditions(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	const TArray<TSharedPtr<FJsonValue>>& Rules,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;
	int32 FailureCount = 0;
	int32 MatchedTransitions = 0;

	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	// Get all transitions in the state machine
	TArray<UAnimStateTransitionNode*> AllTransitions = FAnimStateMachineEditor::GetAllTransitions(
		AnimBP, StateMachineName, OutError);

	if (AllTransitions.Num() == 0)
	{
		OutError = FString::Printf(TEXT("No transitions found in state machine '%s'"), *StateMachineName);
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	// Process each rule
	for (int32 RuleIndex = 0; RuleIndex < Rules.Num(); RuleIndex++)
	{
		const TSharedPtr<FJsonValue>& RuleValue = Rules[RuleIndex];
		const TSharedPtr<FJsonObject>* RuleObjPtr;

		if (!RuleValue->TryGetObject(RuleObjPtr) || !RuleObjPtr->IsValid())
		{
			TSharedPtr<FJsonObject> RuleResult = MakeShared<FJsonObject>();
			RuleResult->SetNumberField(TEXT("rule_index"), RuleIndex);
			RuleResult->SetBoolField(TEXT("success"), false);
			RuleResult->SetStringField(TEXT("error"), TEXT("Invalid rule format"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(RuleResult));
			FailureCount++;
			continue;
		}

		const TSharedPtr<FJsonObject>& RuleObj = *RuleObjPtr;

		// Get match patterns
		const TSharedPtr<FJsonObject>* MatchObjPtr;
		if (!RuleObj->TryGetObjectField(TEXT("match"), MatchObjPtr) || !MatchObjPtr->IsValid())
		{
			TSharedPtr<FJsonObject> RuleResult = MakeShared<FJsonObject>();
			RuleResult->SetNumberField(TEXT("rule_index"), RuleIndex);
			RuleResult->SetBoolField(TEXT("success"), false);
			RuleResult->SetStringField(TEXT("error"), TEXT("Rule missing 'match' field"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(RuleResult));
			FailureCount++;
			continue;
		}

		const TSharedPtr<FJsonObject>& MatchObj = *MatchObjPtr;
		TSharedPtr<FJsonValue> FromPattern = MatchObj->TryGetField(TEXT("from"));
		TSharedPtr<FJsonValue> ToPattern = MatchObj->TryGetField(TEXT("to"));

		if (!FromPattern.IsValid() || !ToPattern.IsValid())
		{
			TSharedPtr<FJsonObject> RuleResult = MakeShared<FJsonObject>();
			RuleResult->SetNumberField(TEXT("rule_index"), RuleIndex);
			RuleResult->SetBoolField(TEXT("success"), false);
			RuleResult->SetStringField(TEXT("error"), TEXT("Rule match missing 'from' or 'to' patterns"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(RuleResult));
			FailureCount++;
			continue;
		}

		// Get conditions
		const TArray<TSharedPtr<FJsonValue>>* ConditionsPtr;
		if (!RuleObj->TryGetArrayField(TEXT("conditions"), ConditionsPtr) || !ConditionsPtr)
		{
			TSharedPtr<FJsonObject> RuleResult = MakeShared<FJsonObject>();
			RuleResult->SetNumberField(TEXT("rule_index"), RuleIndex);
			RuleResult->SetBoolField(TEXT("success"), false);
			RuleResult->SetStringField(TEXT("error"), TEXT("Rule missing 'conditions' array"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(RuleResult));
			FailureCount++;
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>& Conditions = *ConditionsPtr;
		FString Logic = RuleObj->GetStringField(TEXT("logic"));
		if (Logic.IsEmpty()) Logic = TEXT("AND");

		// Track matched transitions for this rule
		TArray<TSharedPtr<FJsonValue>> RuleMatchedTransitions;

		// Find matching transitions
		for (UAnimStateTransitionNode* Transition : AllTransitions)
		{
			if (!Transition) continue;

			UAnimStateNodeBase* PrevState = Transition->GetPreviousState();
			UAnimStateNodeBase* NextState = Transition->GetNextState();

			if (!PrevState || !NextState) continue;

			FString FromStateName = PrevState->GetStateName();
			FString ToStateName = NextState->GetStateName();

			// Check if this transition matches the pattern
			bool bFromMatches = MatchesPattern(FromStateName, FromPattern);
			bool bToMatches = MatchesPattern(ToStateName, ToPattern);

			if (!bFromMatches || !bToMatches) continue;

			MatchedTransitions++;

			// Apply conditions to this transition
			TSharedPtr<FJsonObject> TransitionResult = MakeShared<FJsonObject>();
			TransitionResult->SetStringField(TEXT("from_state"), FromStateName);
			TransitionResult->SetStringField(TEXT("to_state"), ToStateName);

			FString ConditionError;
			bool bConditionSuccess = true;
			TArray<TSharedPtr<FJsonValue>> AppliedConditions;

			// Get transition graph
			UEdGraph* TransitionGraph = GetTransitionGraph(AnimBP, StateMachineName, FromStateName, ToStateName, ConditionError);
			if (!TransitionGraph)
			{
				TransitionResult->SetBoolField(TEXT("success"), false);
				TransitionResult->SetStringField(TEXT("error"), ConditionError);
				RuleMatchedTransitions.Add(MakeShared<FJsonValueObject>(TransitionResult));
				FailureCount++;
				continue;
			}

			// Track node IDs for connecting with logic operators
			TArray<FString> ConditionNodeIds;
			int32 PosX = OsvayderUEConstants::AnimDiagram::ConditionNodeStartX;
			int32 PosY = OsvayderUEConstants::AnimDiagram::ConditionNodeStartY;

			// Apply each condition
			for (const TSharedPtr<FJsonValue>& CondValue : Conditions)
			{
				const TSharedPtr<FJsonObject>* CondObjPtr;
				if (!CondValue->TryGetObject(CondObjPtr) || !CondObjPtr->IsValid()) continue;

				const TSharedPtr<FJsonObject>& CondObj = *CondObjPtr;
				TSharedPtr<FJsonObject> CondResult = MakeShared<FJsonObject>();

				FString CondType = CondObj->GetStringField(TEXT("type"));
				FString Variable = CondObj->GetStringField(TEXT("variable"));
				FString Comparison = CondObj->GetStringField(TEXT("comparison"));
				FString Value = CondObj->GetStringField(TEXT("value"));

				// Handle TimeRemaining condition
				if (CondType.Equals(TEXT("TimeRemaining"), ESearchCase::IgnoreCase))
				{
					FString NodeError;
					FString TimeNodeId;

					// Create TimeRemaining node
					UEdGraphNode* TimeNode = AddConditionNode(
						AnimBP, StateMachineName, FromStateName, ToStateName,
						TEXT("TimeRemaining"), nullptr, FVector2D(PosX, PosY),
						TimeNodeId, NodeError);

					if (!TimeNode)
					{
						CondResult->SetBoolField(TEXT("success"), false);
						CondResult->SetStringField(TEXT("error"), NodeError);
						bConditionSuccess = false;
					}
					else
					{
						// Create comparison node
						FString CompNodeId;
						UEdGraphNode* CompNode = AddConditionNode(
							AnimBP, StateMachineName, FromStateName, ToStateName,
							Comparison, nullptr, FVector2D(PosX + OsvayderUEConstants::AnimDiagram::ConditionNodeSpacing, PosY),
							CompNodeId, NodeError);

						if (CompNode)
						{
							// Connect TimeRemaining to comparison A
							ConnectConditionNodes(
								AnimBP, StateMachineName, FromStateName, ToStateName,
								TimeNodeId, TEXT("ReturnValue"),
								CompNodeId, TEXT("A"),
								NodeError);

							// Set comparison value on B
							SetPinDefaultValue(AnimBP, StateMachineName, FromStateName, ToStateName,
								CompNodeId, TEXT("B"), Value, NodeError);

							ConditionNodeIds.Add(CompNodeId);
							CondResult->SetBoolField(TEXT("success"), true);
							CondResult->SetStringField(TEXT("time_node_id"), TimeNodeId);
							CondResult->SetStringField(TEXT("comparison_node_id"), CompNodeId);
						}
						else
						{
							CondResult->SetBoolField(TEXT("success"), false);
							CondResult->SetStringField(TEXT("error"), NodeError);
							bConditionSuccess = false;
						}
					}
					CondResult->SetStringField(TEXT("type"), TEXT("TimeRemaining"));
				}
				// Handle variable comparison condition
				else if (!Variable.IsEmpty())
				{
					FString ChainError;
					TSharedPtr<FJsonObject> ChainResult = FAnimTransitionConditionFactory::CreateComparisonChain(
						AnimBP, TransitionGraph, Variable, Comparison, Value,
						FVector2D(PosX, PosY), ChainError);

					if (ChainResult.IsValid() && ChainResult->GetBoolField(TEXT("success")))
					{
						FString CompNodeId = ChainResult->GetStringField(TEXT("comparison_node_id"));
						ConditionNodeIds.Add(CompNodeId);
						CondResult->SetBoolField(TEXT("success"), true);
						CondResult->SetStringField(TEXT("variable"), Variable);
						CondResult->SetStringField(TEXT("comparison"), Comparison);
						CondResult->SetStringField(TEXT("value"), Value);
						CondResult->SetStringField(TEXT("comparison_node_id"), CompNodeId);
					}
					else
					{
						CondResult->SetBoolField(TEXT("success"), false);
						CondResult->SetStringField(TEXT("error"), ChainError);
						bConditionSuccess = false;
					}
				}
				else
				{
					CondResult->SetBoolField(TEXT("success"), false);
					CondResult->SetStringField(TEXT("error"), TEXT("Condition must have 'type' or 'variable'"));
					bConditionSuccess = false;
				}

				AppliedConditions.Add(MakeShared<FJsonValueObject>(CondResult));
				PosY += 150;
			}

			// If multiple conditions and using OR logic, we need to connect with OR nodes
			// (AND logic is handled automatically by CreateComparisonChain)
			if (ConditionNodeIds.Num() > 1 && Logic.Equals(TEXT("OR"), ESearchCase::IgnoreCase))
			{
				// Create OR chain
				FString OrError;
				FString PreviousNodeId = ConditionNodeIds[0];

				for (int32 i = 1; i < ConditionNodeIds.Num(); i++)
				{
					FString OrNodeId;
					UEdGraphNode* OrNode = AddConditionNode(
						AnimBP, StateMachineName, FromStateName, ToStateName,
						TEXT("Or"), nullptr, FVector2D(PosX + 400, 100 + (i * 100)),
						OrNodeId, OrError);

					if (OrNode)
					{
						// Connect previous result to OR input A
						ConnectConditionNodes(
							AnimBP, StateMachineName, FromStateName, ToStateName,
							PreviousNodeId, TEXT("ReturnValue"),
							OrNodeId, TEXT("A"),
							OrError);

						// Connect current condition to OR input B
						ConnectConditionNodes(
							AnimBP, StateMachineName, FromStateName, ToStateName,
							ConditionNodeIds[i], TEXT("ReturnValue"),
							OrNodeId, TEXT("B"),
							OrError);

						PreviousNodeId = OrNodeId;
					}
				}

				// Connect final OR output to result
				if (!PreviousNodeId.IsEmpty())
				{
					ConnectToTransitionResult(
						AnimBP, StateMachineName, FromStateName, ToStateName,
						PreviousNodeId, TEXT("ReturnValue"), OrError);
				}
			}

			TransitionResult->SetBoolField(TEXT("success"), bConditionSuccess);
			TransitionResult->SetArrayField(TEXT("conditions"), AppliedConditions);
			RuleMatchedTransitions.Add(MakeShared<FJsonValueObject>(TransitionResult));

			if (bConditionSuccess)
			{
				SuccessCount++;
			}
			else
			{
				FailureCount++;
			}
		}

		// Add rule result
		TSharedPtr<FJsonObject> RuleResult = MakeShared<FJsonObject>();
		RuleResult->SetNumberField(TEXT("rule_index"), RuleIndex);
		RuleResult->SetNumberField(TEXT("matched_count"), RuleMatchedTransitions.Num());
		RuleResult->SetArrayField(TEXT("transitions"), RuleMatchedTransitions);
		ResultsArray.Add(MakeShared<FJsonValueObject>(RuleResult));
	}

	// Compile once after all operations
	FString CompileError;
	bool bCompiled = CompileAnimBlueprint(AnimBP, CompileError);

	Result->SetBoolField(TEXT("success"), FailureCount == 0 && bCompiled);
	Result->SetNumberField(TEXT("total_transitions_in_machine"), AllTransitions.Num());
	Result->SetNumberField(TEXT("matched_transitions"), MatchedTransitions);
	Result->SetNumberField(TEXT("success_count"), SuccessCount);
	Result->SetNumberField(TEXT("failure_count"), FailureCount);
	Result->SetNumberField(TEXT("rules_processed"), Rules.Num());
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	if (!bCompiled)
	{
		Result->SetStringField(TEXT("compile_error"), CompileError);
	}
	Result->SetArrayField(TEXT("results"), ResultsArray);

	return Result;
}

// ===== ASCII Diagram Generation =====

FString FAnimationBlueprintUtils::AbbreviateTransitionCondition(UAnimStateTransitionNode* TransitionNode)
{
	if (!TransitionNode)
	{
		return TEXT("(none)");
	}

	UEdGraph* TransitionGraph = TransitionNode->BoundGraph;
	if (!TransitionGraph)
	{
		return TEXT("(none)");
	}

	// Find what's connected to the result node
	TArray<FString> ConditionParts;

	for (UEdGraphNode* Node : TransitionGraph->Nodes)
	{
		if (!Node) continue;

		// Check for UK2Node_CallFunction comparison nodes
		if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			FName FunctionName = CallNode->FunctionReference.GetMemberName();
			FString FuncStr = FunctionName.ToString();

			// Detect comparison functions
			FString CompOp;
			bool bIsComparison = false;

			if (FuncStr.Contains(TEXT("Greater_")) && !FuncStr.Contains(TEXT("Equal")))
			{
				CompOp = TEXT(">");
				bIsComparison = true;
			}
			else if (FuncStr.Contains(TEXT("Less_")) && !FuncStr.Contains(TEXT("Equal")))
			{
				CompOp = TEXT("<");
				bIsComparison = true;
			}
			else if (FuncStr.Contains(TEXT("GreaterEqual")))
			{
				CompOp = TEXT(">=");
				bIsComparison = true;
			}
			else if (FuncStr.Contains(TEXT("LessEqual")))
			{
				CompOp = TEXT("<=");
				bIsComparison = true;
			}
			else if (FuncStr.Contains(TEXT("EqualEqual")))
			{
				CompOp = TEXT("==");
				bIsComparison = true;
			}
			else if (FuncStr.Contains(TEXT("NotEqual")))
			{
				CompOp = TEXT("!=");
				bIsComparison = true;
			}

			if (bIsComparison)
			{
				// Get what's connected to input A (usually variable)
				FString LeftSide = TEXT("?");
				FString RightSide = TEXT("?");

				for (UEdGraphPin* Pin : CallNode->Pins)
				{
					if (!Pin) continue;

					if (Pin->GetFName() == TEXT("A") && Pin->Direction == EGPD_Input)
					{
						if (Pin->LinkedTo.Num() > 0)
						{
							UEdGraphPin* SourcePin = Pin->LinkedTo[0];
							if (SourcePin && SourcePin->GetOwningNode())
							{
								// Check if source is a variable get node
								if (UK2Node_VariableGet* VarNode = Cast<UK2Node_VariableGet>(SourcePin->GetOwningNode()))
								{
									LeftSide = VarNode->GetVarName().ToString();
								}
								else if (UK2Node_TransitionRuleGetter* GetterNode = Cast<UK2Node_TransitionRuleGetter>(SourcePin->GetOwningNode()))
								{
									// It's a TimeRemaining node
									LeftSide = TEXT("TimeRem");
								}
							}
						}
						else if (!Pin->DefaultValue.IsEmpty())
						{
							LeftSide = Pin->DefaultValue;
						}
					}
					else if (Pin->GetFName() == TEXT("B") && Pin->Direction == EGPD_Input)
					{
						if (!Pin->DefaultValue.IsEmpty())
						{
							RightSide = Pin->DefaultValue;
						}
						else if (Pin->LinkedTo.Num() > 0)
						{
							RightSide = TEXT("(link)");
						}
					}
				}

				ConditionParts.Add(FString::Printf(TEXT("%s%s%s"), *LeftSide, *CompOp, *RightSide));
			}
		}
		// Check for TimeRemaining getter that's directly connected
		else if (UK2Node_TransitionRuleGetter* GetterNode = Cast<UK2Node_TransitionRuleGetter>(Node))
		{
			// Check if output is directly connected to result (no comparison)
			for (UEdGraphPin* Pin : GetterNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
				{
					// If connected directly to result node input
					UEdGraphNode* TargetNode = Pin->LinkedTo[0]->GetOwningNode();
					if (TargetNode && TargetNode->IsA<UAnimGraphNode_TransitionResult>())
					{
						ConditionParts.Add(TEXT("TimeRem"));
					}
				}
			}
		}
	}

	if (ConditionParts.Num() == 0)
	{
		return TEXT("(auto)");
	}

	return FString::Join(ConditionParts, TEXT(" & "));
}

void FAnimationBlueprintUtils::CalculateStateLayout(
	TArray<FDiagramState>& States,
	const TArray<FDiagramTransition>& Transitions)
{
	if (States.Num() == 0) return;

	// Simple layout: find entry state, put it on left
	// Then place connected states to the right, spreading vertically

	// Find entry state and put at (0, 0)
	int32 EntryIndex = -1;
	for (int32 i = 0; i < States.Num(); i++)
	{
		if (States[i].bIsEntry)
		{
			EntryIndex = i;
			States[i].GridX = 0;
			States[i].GridY = 0;
			break;
		}
	}

	// If no entry state found, use first state
	if (EntryIndex == -1 && States.Num() > 0)
	{
		EntryIndex = 0;
		States[0].GridX = 0;
		States[0].GridY = 0;
	}

	// Build adjacency for BFS layout
	TMap<FString, TArray<FString>> Outgoing;
	for (const FDiagramTransition& Trans : Transitions)
	{
		Outgoing.FindOrAdd(Trans.FromState).Add(Trans.ToState);
	}

	// BFS to assign grid positions
	TSet<FString> Visited;
	TArray<FString> Queue;

	if (EntryIndex >= 0)
	{
		Queue.Add(States[EntryIndex].Name);
		Visited.Add(States[EntryIndex].Name);
	}

	int32 CurrentX = 0;
	while (Queue.Num() > 0)
	{
		// Get all states at current X level
		TArray<FString> CurrentLevel = Queue;
		Queue.Empty();

		int32 YOffset = 0;
		for (const FString& StateName : CurrentLevel)
		{
			// Find and set Y position for states at this X level
			for (FDiagramState& State : States)
			{
				if (State.Name == StateName && State.GridX == CurrentX)
				{
					State.GridY = YOffset;
					YOffset++;
				}
			}

			// Add connected states to next level
			if (TArray<FString>* Connected = Outgoing.Find(StateName))
			{
				for (const FString& NextState : *Connected)
				{
					if (!Visited.Contains(NextState))
					{
						Visited.Add(NextState);
						Queue.Add(NextState);

						// Assign X position
						for (FDiagramState& State : States)
						{
							if (State.Name == NextState)
							{
								State.GridX = CurrentX + 1;
								break;
							}
						}
					}
				}
			}
		}
		CurrentX++;
	}

	// Handle disconnected states
	int32 DisconnectedY = 0;
	for (FDiagramState& State : States)
	{
		if (!Visited.Contains(State.Name))
		{
			State.GridX = CurrentX;
			State.GridY = DisconnectedY++;
		}
	}
}

FString FAnimationBlueprintUtils::GenerateASCIIDiagram(
	const TArray<FDiagramState>& States,
	const TArray<FDiagramTransition>& Transitions)
{
	if (States.Num() == 0)
	{
		return TEXT("(empty state machine)");
	}

	// Find grid dimensions
	int32 MaxX = 0, MaxY = 0;
	for (const FDiagramState& State : States)
	{
		MaxX = FMath::Max(MaxX, State.GridX);
		MaxY = FMath::Max(MaxY, State.GridY);
	}

	// Build a grid-based diagram
	// Each cell is roughly: "[ StateName ]" with transitions between
	const int32 CellWidth = OsvayderUEConstants::AnimDiagram::DiagramCellWidth;
	const int32 CellHeight = OsvayderUEConstants::AnimDiagram::DiagramCellHeight;

	// Create output lines
	TArray<FString> Lines;
	FString TitleLine = TEXT("State Machine Diagram:");
	Lines.Add(TitleLine);
	Lines.Add(TEXT(""));

	// Build state map by grid position
	TMap<TPair<int32, int32>, const FDiagramState*> StateGrid;
	for (const FDiagramState& State : States)
	{
		StateGrid.Add(TPair<int32, int32>(State.GridX, State.GridY), &State);
	}

	// Build transition map
	TMap<TPair<FString, FString>, const FDiagramTransition*> TransitionMap;
	for (const FDiagramTransition& Trans : Transitions)
	{
		TransitionMap.Add(TPair<FString, FString>(Trans.FromState, Trans.ToState), &Trans);
	}

	// Generate diagram row by row
	for (int32 y = 0; y <= MaxY; y++)
	{
		// State row
		FString StateRow;
		FString ArrowRow;
		FString ConditionRow;

		for (int32 x = 0; x <= MaxX; x++)
		{
			if (const FDiagramState* const* StatePtr = StateGrid.Find(TPair<int32, int32>(x, y)))
			{
				const FDiagramState& State = **StatePtr;

				// Format state name with brackets
				FString StateName = State.Name;
				if (StateName.Len() > OsvayderUEConstants::AnimDiagram::MaxStateNameDisplayLength)
				{
					StateName = StateName.Left(OsvayderUEConstants::AnimDiagram::MaxStateNameDisplayLength - 1) + TEXT(".");
				}

				FString StateBox;
				if (State.bIsEntry)
				{
					StateBox = FString::Printf(TEXT("->[ %-12s ]"), *StateName);
				}
				else
				{
					StateBox = FString::Printf(TEXT("  [ %-12s ]"), *StateName);
				}
				StateRow += StateBox;

				// Find transition to next column
				if (x < MaxX)
				{
					// Look for any transition from this state to states in next column
					bool bFoundTransition = false;
					for (int32 nextY = 0; nextY <= MaxY; nextY++)
					{
						if (const FDiagramState* const* NextStatePtr = StateGrid.Find(TPair<int32, int32>(x + 1, nextY)))
						{
							const FDiagramState& NextState = **NextStatePtr;
							if (const FDiagramTransition* const* TransPtr = TransitionMap.Find(
								TPair<FString, FString>(State.Name, NextState.Name)))
							{
								ArrowRow += TEXT(" ----> ");
								// Add abbreviated condition
								FString Cond = (*TransPtr)->ConditionAbbrev;
								if (Cond.Len() > 15)
								{
									Cond = Cond.Left(14) + TEXT(".");
								}
								ConditionRow += FString::Printf(TEXT(" %-15s"), *Cond);
								bFoundTransition = true;
								break;
							}
						}
					}

					if (!bFoundTransition)
					{
						ArrowRow += TEXT("       ");
						ConditionRow += TEXT("               ");
					}
				}
			}
			else
			{
				// Empty cell
				StateRow += FString::Printf(TEXT("%*s"), CellWidth, TEXT(""));
				if (x < MaxX)
				{
					ArrowRow += TEXT("       ");
					ConditionRow += TEXT("               ");
				}
			}
		}

		Lines.Add(StateRow);
		if (!ArrowRow.IsEmpty())
		{
			Lines.Add(ArrowRow);
		}
		if (!ConditionRow.TrimStartAndEnd().IsEmpty())
		{
			Lines.Add(ConditionRow);
		}
		Lines.Add(TEXT(""));
	}

	// Add legend
	Lines.Add(TEXT("Legend:"));
	Lines.Add(TEXT("  -> = Entry state"));
	Lines.Add(TEXT("  [...] = State name"));
	Lines.Add(TEXT("  ----> = Transition (condition below)"));

	// Add transition list
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("All Transitions:"));
	for (const FDiagramTransition& Trans : Transitions)
	{
		Lines.Add(FString::Printf(TEXT("  %s -> %s: %s (%.2fs)"),
			*Trans.FromState, *Trans.ToState, *Trans.ConditionAbbrev, Trans.Duration));
	}

	return FString::Join(Lines, TEXT("\n"));
}

TSharedPtr<FJsonObject> FAnimationBlueprintUtils::GetStateMachineDiagram(
	UAnimBlueprint* AnimBP,
	const FString& StateMachineName,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!ValidateAnimBlueprintForOperation(AnimBP, OutError))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	// Find state machine
	UAnimGraphNode_StateMachine* StateMachine = FindStateMachine(AnimBP, StateMachineName, OutError);
	if (!StateMachine)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), OutError);
		return Result;
	}

	// Get all states
	TArray<UAnimStateNode*> States = GetAllStates(AnimBP, StateMachineName, OutError);
	TArray<UAnimStateTransitionNode*> Transitions = GetAllTransitions(AnimBP, StateMachineName, OutError);

	// Build diagram data structures
	TArray<FDiagramState> DiagramStates;
	TArray<FDiagramTransition> DiagramTransitions;

	// Find entry state
	UAnimationStateMachineGraph* Graph = nullptr;
	if (StateMachine->EditorStateMachineGraph)
	{
		Graph = StateMachine->EditorStateMachineGraph;
	}

	FString EntryStateName;
	if (Graph && Graph->EntryNode)
	{
		// Find what the entry node connects to
		for (UEdGraphPin* Pin : Graph->EntryNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
			{
				UEdGraphNode* ConnectedNode = Pin->LinkedTo[0]->GetOwningNode();
				if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(ConnectedNode))
				{
					EntryStateName = StateNode->GetStateName();
					break;
				}
			}
		}
	}

	// Populate states
	for (UAnimStateNode* State : States)
	{
		if (!State) continue;

		FDiagramState DS;
		DS.Name = State->GetStateName();
		DS.bIsEntry = (DS.Name == EntryStateName);

		// Get animation name if available
		UEdGraph* StateGraph = State->GetBoundGraph();
		if (StateGraph)
		{
			for (UEdGraphNode* Node : StateGraph->Nodes)
			{
				if (UAnimGraphNode_SequencePlayer* SeqNode = Cast<UAnimGraphNode_SequencePlayer>(Node))
				{
					// Get animation sequence name
					if (SeqNode->GetAnimationAsset())
					{
						DS.AnimationName = SeqNode->GetAnimationAsset()->GetName();
					}
					break;
				}
				else if (UAnimGraphNode_BlendSpacePlayer* BSNode = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
				{
					if (BSNode->GetAnimationAsset())
					{
						DS.AnimationName = BSNode->GetAnimationAsset()->GetName();
					}
					break;
				}
			}
		}

		DiagramStates.Add(DS);
	}

	// Populate transitions
	for (UAnimStateTransitionNode* Trans : Transitions)
	{
		if (!Trans) continue;

		UAnimStateNodeBase* PrevState = Trans->GetPreviousState();
		UAnimStateNodeBase* NextState = Trans->GetNextState();

		if (!PrevState || !NextState) continue;

		FDiagramTransition DT;
		DT.FromState = PrevState->GetStateName();
		DT.ToState = NextState->GetStateName();
		DT.Duration = Trans->CrossfadeDuration;
		DT.ConditionAbbrev = AbbreviateTransitionCondition(Trans);

		DiagramTransitions.Add(DT);
	}

	// Calculate layout
	CalculateStateLayout(DiagramStates, DiagramTransitions);

	// Generate ASCII diagram
	FString ASCIIDiagram = GenerateASCIIDiagram(DiagramStates, DiagramTransitions);

	// Build enhanced JSON info
	TSharedPtr<FJsonObject> EnhancedInfo = MakeShared<FJsonObject>();

	// States with positions
	TArray<TSharedPtr<FJsonValue>> StatesArray;
	for (const FDiagramState& DS : DiagramStates)
	{
		TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
		StateObj->SetStringField(TEXT("name"), DS.Name);
		StateObj->SetNumberField(TEXT("grid_x"), DS.GridX);
		StateObj->SetNumberField(TEXT("grid_y"), DS.GridY);
		StateObj->SetBoolField(TEXT("is_entry"), DS.bIsEntry);
		if (!DS.AnimationName.IsEmpty())
		{
			StateObj->SetStringField(TEXT("animation"), DS.AnimationName);
		}
		StatesArray.Add(MakeShared<FJsonValueObject>(StateObj));
	}
	EnhancedInfo->SetArrayField(TEXT("states"), StatesArray);

	// Transitions with conditions
	TArray<TSharedPtr<FJsonValue>> TransitionsArray;
	for (const FDiagramTransition& DT : DiagramTransitions)
	{
		TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();
		TransObj->SetStringField(TEXT("from"), DT.FromState);
		TransObj->SetStringField(TEXT("to"), DT.ToState);
		TransObj->SetStringField(TEXT("condition_summary"), DT.ConditionAbbrev);
		TransObj->SetNumberField(TEXT("duration"), DT.Duration);
		TransitionsArray.Add(MakeShared<FJsonValueObject>(TransObj));
	}
	EnhancedInfo->SetArrayField(TEXT("transitions"), TransitionsArray);

	// Build result
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("state_machine"), StateMachineName);
	Result->SetStringField(TEXT("ascii_diagram"), ASCIIDiagram);
	Result->SetObjectField(TEXT("enhanced_info"), EnhancedInfo);

	return Result;
}
