// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationBlueprintUtils.h"
#include "MCP/Tools/MCPTool_BlueprintQuery.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MCPAnimationPreflightTests
{
	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	bool JsonArrayContainsString(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& Expected)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue) && StringValue == Expected)
			{
				return true;
			}
		}

		return false;
	}

	TSharedPtr<FJsonObject> FindRoleEntry(const TSharedPtr<FJsonObject>& Object, const FString& Role)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Roles = nullptr;
		if (!Object->TryGetArrayField(TEXT("role_inventory"), Roles) || !Roles)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& RoleValue : *Roles)
		{
			const TSharedPtr<FJsonObject> RoleObject = RoleValue.IsValid() ? RoleValue->AsObject() : nullptr;
			if (RoleObject.IsValid())
			{
				FString Candidate;
				if (RoleObject->TryGetStringField(TEXT("role"), Candidate) && Candidate == Role)
				{
					return RoleObject;
				}
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> FindActorRoleBindingEntry(const TSharedPtr<FJsonObject>& Object, const FString& Role)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!Object->TryGetArrayField(TEXT("actor_role_bindings"), Bindings) || !Bindings)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject> BindingObject = BindingValue.IsValid() ? BindingValue->AsObject() : nullptr;
			if (BindingObject.IsValid())
			{
				FString Candidate;
				if (BindingObject->TryGetStringField(TEXT("role"), Candidate) && Candidate == Role)
				{
					return BindingObject;
				}
			}
		}

		return nullptr;
	}

	TSharedRef<FJsonObject> MakePreflightParams(
		const FString& AnimBlueprintPath,
		const TArray<FString>& Roles,
		const FString& ActorBlueprintPath = FString(),
		const FString& AssetRoot = FString(),
		const FString& PackIdentifier = FString())
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), TEXT("animation_preflight"));
		Params->SetStringField(TEXT("blueprint_path"), AnimBlueprintPath);
		Params->SetArrayField(TEXT("required_animation_roles"), MakeStringArray(Roles));
		Params->SetNumberField(TEXT("match_limit"), 5);

		if (!ActorBlueprintPath.IsEmpty())
		{
			Params->SetStringField(TEXT("actor_blueprint_path"), ActorBlueprintPath);
		}
		if (!AssetRoot.IsEmpty())
		{
			Params->SetStringField(TEXT("asset_root"), AssetRoot);
		}
		if (!PackIdentifier.IsEmpty())
		{
			Params->SetStringField(TEXT("pack_identifier"), PackIdentifier);
		}

		return Params;
	}

	bool JsonObjectArrayHasField(const TSharedPtr<FJsonObject>& Object, const FString& ArrayFieldName, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(ArrayFieldName, Values) || !Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Entry.IsValid() && Entry->HasField(FieldName))
			{
				return true;
			}
		}

		return false;
	}

	int32 GetRoleMatchCount(const TSharedPtr<FJsonObject>& RoleObject)
	{
		if (!RoleObject.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		if (!RoleObject->TryGetArrayField(TEXT("matches"), Matches) || !Matches)
		{
			return 0;
		}
		return Matches->Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightInfoTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.Info",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightInfoTest::RunTest(const FString& Parameters)
{
	FMCPTool_BlueprintQuery Tool;
	const FMCPToolInfo Info = Tool.GetInfo();

	TestTrue(TEXT("Description should mention animation_preflight"), Info.Description.Contains(TEXT("animation_preflight")));

	bool bHasRequiredAnimationRoles = false;
	bool bHasAssetRoot = false;
	bool bHasImportedAssetRoot = false;
	bool bHasPackIdentifier = false;
	bool bHasImportedPackIdentifier = false;
	for (const FMCPToolParameter& Param : Info.Parameters)
	{
		if (Param.Name == TEXT("required_animation_roles"))
		{
			bHasRequiredAnimationRoles = true;
			TestEqual(TEXT("required_animation_roles should be array"), Param.Type, TEXT("array"));
		}
		else if (Param.Name == TEXT("asset_root"))
		{
			bHasAssetRoot = true;
			TestEqual(TEXT("asset_root should be string"), Param.Type, TEXT("string"));
		}
		else if (Param.Name == TEXT("imported_asset_root"))
		{
			bHasImportedAssetRoot = true;
			TestEqual(TEXT("imported_asset_root should be string"), Param.Type, TEXT("string"));
		}
		else if (Param.Name == TEXT("pack_identifier"))
		{
			bHasPackIdentifier = true;
			TestEqual(TEXT("pack_identifier should be string"), Param.Type, TEXT("string"));
		}
		else if (Param.Name == TEXT("imported_pack_identifier"))
		{
			bHasImportedPackIdentifier = true;
			TestEqual(TEXT("imported_pack_identifier should be string"), Param.Type, TEXT("string"));
		}
	}

	TestTrue(TEXT("Tool should declare required_animation_roles"), bHasRequiredAnimationRoles);
	TestTrue(TEXT("Tool should declare asset_root"), bHasAssetRoot);
	TestTrue(TEXT("Tool should declare imported_asset_root"), bHasImportedAssetRoot);
	TestTrue(TEXT("Tool should declare pack_identifier"), bHasPackIdentifier);
	TestTrue(TEXT("Tool should declare imported_pack_identifier"), bHasImportedPackIdentifier);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightMissingSkeletonTest,
	"OsvayderUE.Animation.Preflight.MissingSkeletonIsManualBlocker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightMissingSkeletonTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* TransientAnimBlueprint = NewObject<UAnimBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	FString Error;
	const TSharedPtr<FJsonObject> Report = FAnimationBlueprintUtils::BuildAnimationPreflightReport(
		TransientAnimBlueprint,
		TArray<FString>{},
		nullptr,
		FString(),
		5,
		FString(),
		FString(),
		Error);

	TestTrue(TEXT("Preflight report should be created"), Report.IsValid());
	if (!Report.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("Diagnostic report should succeed even when blocked"), Report->GetBoolField(TEXT("success")));
	TestEqual(TEXT("Missing target skeleton should classify as manual blocker"),
		Report->GetStringField(TEXT("proof_classification")),
		TEXT("manual_blocker"));
	TestTrue(TEXT("Missing target skeleton should emit unknown compatibility blocker"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Report, TEXT("blocker_codes"), TEXT("unknown_skeleton_compatibility")));
	TestFalse(TEXT("Missing skeleton should not be preflight-ready"),
		Report->GetBoolField(TEXT("preflight_ready_for_manual_verification")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightPlatformingMissingRolesTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.PlatformingMissingRoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightPlatformingMissingRolesTest::RunTest(const FString& Parameters)
{
	FMCPTool_BlueprintQuery Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationPreflightTests::MakePreflightParams(
		TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"),
		{TEXT("hover"), TEXT("fly"), TEXT("wallrun_left"), TEXT("wallrun_right"), TEXT("wall_jump"), TEXT("landing")},
		TEXT("/Game/Variant_Platforming/Blueprints/BP_PlatformingCharacter")));

	TestTrue(TEXT("animation_preflight should succeed for platforming AnimBP"), Result.bSuccess);
	TestTrue(TEXT("animation_preflight should return report data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Platforming preflight should block visual-ready claims"),
		Result.Data->GetStringField(TEXT("proof_classification")),
		TEXT("manual_blocker"));
	TestEqual(TEXT("Actor blueprint path should be echoed"),
		Result.Data->GetStringField(TEXT("actor_blueprint_path")),
		TEXT("/Game/Variant_Platforming/Blueprints/BP_PlatformingCharacter.BP_PlatformingCharacter"));
	TestTrue(TEXT("hover should be reported missing"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("missing_roles"), TEXT("hover")));
	TestTrue(TEXT("manual asset dependency blocker should be present"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("blocker_codes"), TEXT("manual_asset_dependency_blocker")));
	TestTrue(TEXT("read-only recommendation should be present"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("recommendation_codes"), TEXT("anim_preflight_read_only")));

	const TSharedPtr<FJsonObject>* ProofMatrix = nullptr;
	TestTrue(TEXT("proof_matrix should be present for platforming report"),
		Result.Data->TryGetObjectField(TEXT("proof_matrix"), ProofMatrix) && ProofMatrix && (*ProofMatrix).IsValid());
	if (ProofMatrix && *ProofMatrix)
	{
		TestTrue(TEXT("Platforming report should mark manual_blocker"),
			(*ProofMatrix)->GetBoolField(TEXT("manual_blocker")));
		TestFalse(TEXT("Platforming report should not claim gameplay_code_proof"),
			(*ProofMatrix)->GetBoolField(TEXT("gameplay_code_proof")));
		TestTrue(TEXT("Platforming report should expose animation_inventory_proof"),
			(*ProofMatrix)->GetBoolField(TEXT("animation_inventory_proof")));
	}

	const TSharedPtr<FJsonObject> WallJumpEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, TEXT("wall_jump"));
	TestTrue(TEXT("wall_jump role should have a report entry"), WallJumpEntry.IsValid());
	if (WallJumpEntry.IsValid())
	{
		TestEqual(TEXT("wall_jump should be satisfied from local assets"),
			WallJumpEntry->GetStringField(TEXT("status")),
			TEXT("matched"));
	}

	const TSharedPtr<FJsonObject> LandingEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, TEXT("landing"));
	TestTrue(TEXT("landing role should have a report entry"), LandingEntry.IsValid());
	if (LandingEntry.IsValid())
	{
		TestTrue(TEXT("landing should have local candidates even if retarget is required"),
			LandingEntry->GetStringField(TEXT("status")) == TEXT("matched") ||
			LandingEntry->GetStringField(TEXT("status")) == TEXT("matched_with_skeleton_mismatch"));
	}

	const TSharedPtr<FJsonObject> WallRunLeftEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, TEXT("wallrun_left"));
	TestTrue(TEXT("wallrun_left should have a report entry"), WallRunLeftEntry.IsValid());
	if (WallRunLeftEntry.IsValid())
	{
		TestTrue(TEXT("wallrun_left should not fail as missing when local pack candidates exist"),
			WallRunLeftEntry->GetStringField(TEXT("status")) != TEXT("missing_manual_blocker"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightUnarmedCompatibleRolesTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.UnarmedCompatibleRoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightUnarmedCompatibleRolesTest::RunTest(const FString& Parameters)
{
	FMCPTool_BlueprintQuery Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationPreflightTests::MakePreflightParams(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"),
		{TEXT("wall_jump"), TEXT("landing"), TEXT("dash")},
		FString(),
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed")));

	TestTrue(TEXT("animation_preflight should succeed for ABP_Unarmed"), Result.bSuccess);
	TestTrue(TEXT("animation_preflight should return report data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Fully matched roles should stay at gameplay_code_proof"),
		Result.Data->GetStringField(TEXT("proof_classification")),
		TEXT("gameplay_code_proof"));
	TestEqual(TEXT("Unarmed compatible set should have zero unsatisfied roles"),
		Result.Data->GetArrayField(TEXT("unsatisfied_roles")).Num(),
		0);
	TestEqual(TEXT("Scoped asset root should be preserved"),
		Result.Data->GetStringField(TEXT("asset_scope_root")),
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed"));
	TestFalse(TEXT("Whole-project local inventory should not be tagged as pending post-import validation"),
		Result.Data->GetBoolField(TEXT("needs_post_import_validation")));
	TestTrue(TEXT("Whole-project local inventory should discover animation assets"),
		Result.Data->GetNumberField(TEXT("discovered_animation_asset_count")) > 0);
	TestTrue(TEXT("dash should be listed as a compatible role"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("compatible_roles"), TEXT("dash")));

	const TSharedPtr<FJsonObject>* ProofMatrix = nullptr;
	TestTrue(TEXT("proof_matrix should be present"),
		Result.Data->TryGetObjectField(TEXT("proof_matrix"), ProofMatrix) && ProofMatrix && (*ProofMatrix).IsValid());
	if (ProofMatrix && *ProofMatrix)
	{
		TestFalse(TEXT("No compatible-role report should not emit manual blockers"),
			(*ProofMatrix)->GetBoolField(TEXT("manual_blocker")));
		TestTrue(TEXT("Compatible-role report should claim gameplay_code_proof"),
			(*ProofMatrix)->GetBoolField(TEXT("gameplay_code_proof")));
		TestTrue(TEXT("Compatible-role report should expose animation_inventory_proof"),
			(*ProofMatrix)->GetBoolField(TEXT("animation_inventory_proof")));
	}

	const TSharedPtr<FJsonObject> DashEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, TEXT("dash"));
	TestTrue(TEXT("dash should have a report entry"), DashEntry.IsValid());
	if (DashEntry.IsValid())
	{
		TestEqual(TEXT("dash should be matched"), DashEntry->GetStringField(TEXT("status")), TEXT("matched"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightLocalRootMissingRolesTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.LocalRootMissingRoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightLocalRootMissingRolesTest::RunTest(const FString& Parameters)
{
	FMCPTool_BlueprintQuery Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationPreflightTests::MakePreflightParams(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"),
		{TEXT("hover")},
		FString(),
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed"),
		TEXT("UnarmedLocalRoot")));

	TestTrue(TEXT("animation_preflight should succeed for local root missing-role diagnostics"), Result.bSuccess);
	TestTrue(TEXT("animation_preflight should return report data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Local root should be echoed"),
		Result.Data->GetStringField(TEXT("asset_scope_root")),
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed"));
	TestEqual(TEXT("Pack identifier should be echoed"),
		Result.Data->GetStringField(TEXT("imported_pack_identifier")),
		TEXT("UnarmedLocalRoot"));
	TestFalse(TEXT("Local validation should not stay at needs_post_import_validation"),
		Result.Data->GetBoolField(TEXT("needs_post_import_validation")));
	TestTrue(TEXT("Local root missing role should emit manual asset dependency blocker"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("blocker_codes"), TEXT("manual_asset_dependency_blocker")));
	TestTrue(TEXT("hover should be unsatisfied in scoped local root"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("unsatisfied_roles"), TEXT("hover")));
	TestTrue(TEXT("Scoped local root should discover existing local animation assets"),
		Result.Data->GetNumberField(TEXT("discovered_animation_asset_count")) > 0);

	TSharedRef<FJsonObject> AliasParams = MCPAnimationPreflightTests::MakePreflightParams(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"),
		{TEXT("hover")},
		FString(),
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed"));
	AliasParams->SetStringField(TEXT("imported_pack_identifier"), TEXT("AliasPackIdentifier"));

	const FMCPToolResult AliasResult = Tool.Execute(AliasParams);
	TestTrue(TEXT("animation_preflight should accept imported_pack_identifier alias"), AliasResult.bSuccess);
	TestTrue(TEXT("animation_preflight alias result should return data"), AliasResult.Data.IsValid());
	if (!AliasResult.bSuccess || !AliasResult.Data.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("imported_pack_identifier alias should be echoed"),
		AliasResult.Data->GetStringField(TEXT("imported_pack_identifier")),
		TEXT("AliasPackIdentifier"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightLocalRootSkeletonMismatchTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.LocalRootSkeletonMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightLocalRootSkeletonMismatchTest::RunTest(const FString& Parameters)
{
	FMCPTool_BlueprintQuery Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationPreflightTests::MakePreflightParams(
		TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"),
		{TEXT("relaxed_idle_ipc")},
		FString(),
		TEXT("/Game/MCO_Demo_Pack/Animation/Mobility/IPC"),
		TEXT("MCO_Demo_IPC")));

	TestTrue(TEXT("animation_preflight should succeed for local root skeleton mismatch diagnostics"), Result.bSuccess);
	TestTrue(TEXT("animation_preflight should return report data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("Scoped mismatch should emit skeleton_mismatch"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("blocker_codes"), TEXT("skeleton_mismatch")));
	TestTrue(TEXT("Scoped mismatch should emit retarget_required_not_implemented"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("blocker_codes"), TEXT("retarget_required_not_implemented")));
	TestFalse(TEXT("Scoped local mismatch validation should not request post-import validation"),
		Result.Data->GetBoolField(TEXT("needs_post_import_validation")));

	const TSharedPtr<FJsonObject>* SkeletonSummary = nullptr;
	TestTrue(TEXT("skeleton_summary should be present"),
		Result.Data->TryGetObjectField(TEXT("skeleton_summary"), SkeletonSummary) && SkeletonSummary && (*SkeletonSummary).IsValid());
	if (SkeletonSummary && *SkeletonSummary)
	{
		TestTrue(TEXT("Scoped mismatch should count incompatible assets"),
			(*SkeletonSummary)->GetNumberField(TEXT("incompatible_asset_count")) > 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightWallRunScopedRootTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.WallRunScopedRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightWallRunScopedRootTest::RunTest(const FString& Parameters)
{
	FMCPTool_BlueprintQuery Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationPreflightTests::MakePreflightParams(
		TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"),
		{TEXT("wallrun_left"), TEXT("wallrun_right")},
		FString(),
		TEXT("/Game/ParkourAnimations/Animations/UE5_Skelet/WallRun/WallRun_Horizontal"),
		TEXT("ParkourWallRunUE5")));

	TestTrue(TEXT("animation_preflight should succeed for scoped wallrun candidates"), Result.bSuccess);
	TestTrue(TEXT("animation_preflight should return report data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> LeftEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, TEXT("wallrun_left"));
	const TSharedPtr<FJsonObject> RightEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, TEXT("wallrun_right"));
	TestTrue(TEXT("wallrun_left should have scoped matches"), MCPAnimationPreflightTests::GetRoleMatchCount(LeftEntry) > 0);
	TestTrue(TEXT("wallrun_right should have scoped matches"), MCPAnimationPreflightTests::GetRoleMatchCount(RightEntry) > 0);
	TestFalse(TEXT("wallrun_left should not be missing when UE5 wallrun candidates exist"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("missing_roles"), TEXT("wallrun_left")));
	TestFalse(TEXT("wallrun_right should not be missing when UE5 wallrun candidates exist"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("missing_roles"), TEXT("wallrun_right")));
	TestTrue(TEXT("scoped wallrun should require retarget or already be compatible"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("blocker_codes"), TEXT("skeleton_mismatch")) ||
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("compatible_roles"), TEXT("wallrun_left")) ||
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("compatible_roles"), TEXT("wallrun_right")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightRootMotionMetadataTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.RootMotionMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightRootMotionMetadataTest::RunTest(const FString& Parameters)
{
	FMCPTool_BlueprintQuery Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationPreflightTests::MakePreflightParams(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"),
		{TEXT("idle_walk_run")},
		FString(),
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed")));

	TestTrue(TEXT("animation_preflight should succeed for root-motion metadata diagnostics"), Result.bSuccess);
	TestTrue(TEXT("animation_preflight should return report data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> RoleEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, TEXT("idle_walk_run"));
	TestTrue(TEXT("idle_walk_run role should have a report entry"), RoleEntry.IsValid());
	if (!RoleEntry.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
	TestTrue(TEXT("idle_walk_run should return local matches"),
		RoleEntry->TryGetArrayField(TEXT("matches"), Matches) && Matches && Matches->Num() > 0);
	if (!Matches || Matches->Num() == 0)
	{
		return false;
	}

	bool bSawBlendSpace = false;
	for (const TSharedPtr<FJsonValue>& MatchValue : *Matches)
	{
		const TSharedPtr<FJsonObject> MatchObject = MatchValue.IsValid() ? MatchValue->AsObject() : nullptr;
		TestTrue(TEXT("Every match should be a JSON object"), MatchObject.IsValid());
		if (!MatchObject.IsValid())
		{
			return false;
		}

		const FString AssetClass = MatchObject->GetStringField(TEXT("class"));
		if (AssetClass.Contains(TEXT("BlendSpace")))
		{
			bSawBlendSpace = true;
		}

		TestTrue(TEXT("Every match should preserve name"), MatchObject->HasTypedField<EJson::String>(TEXT("name")));
		TestTrue(TEXT("Every match should preserve path"), MatchObject->HasTypedField<EJson::String>(TEXT("path")));
		TestTrue(TEXT("Every match should preserve class"), MatchObject->HasTypedField<EJson::String>(TEXT("class")));
		TestTrue(TEXT("Every match should preserve skeleton when available"), MatchObject->HasTypedField<EJson::String>(TEXT("skeleton")));
		TestTrue(TEXT("Every match should preserve skeleton_path when available"), MatchObject->HasTypedField<EJson::String>(TEXT("skeleton_path")));

		const TSharedPtr<FJsonObject>* RootMotionMetadata = nullptr;
		TestTrue(TEXT("Every match should expose root_motion_metadata"),
			MatchObject->TryGetObjectField(TEXT("root_motion_metadata"), RootMotionMetadata) &&
			RootMotionMetadata && (*RootMotionMetadata).IsValid());
		if (!RootMotionMetadata || !(*RootMotionMetadata).IsValid())
		{
			return false;
		}

		TestTrue(TEXT("root_motion_signal should be explicit"),
			(*RootMotionMetadata)->HasTypedField<EJson::String>(TEXT("root_motion_signal")));
		TestTrue(TEXT("in_place_signal should be explicit"),
			(*RootMotionMetadata)->HasTypedField<EJson::String>(TEXT("in_place_signal")));
		TestEqual(TEXT("in-place should be unknown unless determinable locally"),
			(*RootMotionMetadata)->GetStringField(TEXT("in_place_signal")),
			TEXT("unknown_from_asset_metadata"));

		if (AssetClass.Contains(TEXT("BlendSpace")))
		{
			TestEqual(TEXT("BlendSpace root motion should be explicit unknown"),
				(*RootMotionMetadata)->GetStringField(TEXT("root_motion_signal")),
				TEXT("unknown_from_asset_metadata"));
			TestFalse(TEXT("BlendSpace should not claim root_motion_enabled"),
				(*RootMotionMetadata)->HasField(TEXT("root_motion_enabled")));
		}
		else
		{
			TestTrue(TEXT("AnimSequence/Montage should report root_motion_enabled when determinable"),
				(*RootMotionMetadata)->HasTypedField<EJson::Boolean>(TEXT("root_motion_enabled")));
		}
	}

	TestTrue(TEXT("idle_walk_run scoped report should include known local BlendSpace coverage"), bSawBlendSpace);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightPlatformingParkourActorBindingsTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.PlatformingParkourActorBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightPlatformingParkourActorBindingsTest::RunTest(const FString& Parameters)
{
	FMCPTool_BlueprintQuery Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationPreflightTests::MakePreflightParams(
		TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"),
		{TEXT("wall_climb"), TEXT("ledge_attach"), TEXT("ledge_idle"), TEXT("mantle")},
		TEXT("/Game/Variant_Platforming/Blueprints/BP_PlatformingCharacter")));

	TestTrue(TEXT("animation_preflight should succeed for platforming parkour bindings"), Result.bSuccess);
	TestTrue(TEXT("animation_preflight should return report data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	for (const FString& Role : TArray<FString>{TEXT("wall_climb"), TEXT("ledge_attach"), TEXT("ledge_idle"), TEXT("mantle")})
	{
		const TSharedPtr<FJsonObject> RoleEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, Role);
		TestTrue(FString::Printf(TEXT("%s role should have a report entry"), *Role), RoleEntry.IsValid());
		if (RoleEntry.IsValid())
		{
			TestEqual(
				FString::Printf(TEXT("%s should be satisfied by the actor binding"), *Role),
				RoleEntry->GetStringField(TEXT("status")),
				TEXT("matched_with_actor_binding"));
		}

		const TSharedPtr<FJsonObject> BindingEntry = MCPAnimationPreflightTests::FindActorRoleBindingEntry(Result.Data, Role);
		TestTrue(FString::Printf(TEXT("%s actor binding should be reported"), *Role), BindingEntry.IsValid());
		if (BindingEntry.IsValid())
		{
			AddInfo(FString::Printf(
				TEXT("%s binding status=%s asset=%s semantic_score=%.0f compatibility=%s"),
				*Role,
				*BindingEntry->GetStringField(TEXT("status")),
				*BindingEntry->GetStringField(TEXT("assigned_asset_path")),
				BindingEntry->GetNumberField(TEXT("semantic_match_score")),
				*BindingEntry->GetStringField(TEXT("skeleton_compatibility"))));
			TestEqual(
				FString::Printf(TEXT("%s actor binding should be ready"), *Role),
				BindingEntry->GetStringField(TEXT("status")),
				TEXT("assigned_reference_ready"));
			TestEqual(
				FString::Printf(TEXT("%s actor binding should be skeleton-compatible"), *Role),
				BindingEntry->GetStringField(TEXT("skeleton_compatibility")),
				TEXT("compatible"));
			TestTrue(
				FString::Printf(TEXT("%s actor binding should have semantic role fit"), *Role),
				BindingEntry->GetNumberField(TEXT("semantic_match_score")) > 0);
		}
	}

	TestEqual(TEXT("Bound parkour roles should have no unsatisfied roles"),
		Result.Data->GetArrayField(TEXT("unsatisfied_roles")).Num(),
		0);
	TestEqual(TEXT("Bound parkour roles should classify as gameplay code proof"),
		Result.Data->GetStringField(TEXT("proof_classification")),
		TEXT("gameplay_code_proof"));

	const TSharedPtr<FJsonObject> WallClimbBinding =
		MCPAnimationPreflightTests::FindActorRoleBindingEntry(Result.Data, TEXT("wall_climb"));
	if (WallClimbBinding.IsValid())
	{
		TestFalse(TEXT("Wall climb binding must not point at a wall-run clip"),
			WallClimbBinding->GetStringField(TEXT("assigned_asset_path")).Contains(TEXT("WallRun")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightPlatformingWallClimbWallRunBindingRegressionTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.PlatformingWallClimbWallRunBindingRegression",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightPlatformingWallClimbWallRunBindingRegressionTest::RunTest(const FString& Parameters)
{
	const FString ActorBlueprintPath = TEXT("/Game/Variant_Platforming/Blueprints/BP_PlatformingCharacter.BP_PlatformingCharacter");
	UBlueprint* ActorBlueprint = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *ActorBlueprintPath));
	TestTrue(TEXT("Platforming actor blueprint should load"), ActorBlueprint != nullptr);
	TestTrue(TEXT("Platforming actor blueprint should have generated class"), ActorBlueprint && ActorBlueprint->GeneratedClass);
	if (!ActorBlueprint || !ActorBlueprint->GeneratedClass)
	{
		return false;
	}

	UObject* CDO = ActorBlueprint->GeneratedClass->GetDefaultObject(false);
	TestTrue(TEXT("Platforming actor CDO should be available"), CDO != nullptr);
	if (!CDO)
	{
		return false;
	}

	FSoftObjectProperty* WallClimbProperty =
		CastField<FSoftObjectProperty>(ActorBlueprint->GeneratedClass->FindPropertyByName(TEXT("WallClimbAnimation")));
	TestTrue(TEXT("WallClimbAnimation should be a soft object property"), WallClimbProperty != nullptr);
	if (!WallClimbProperty)
	{
		return false;
	}

	UPackage* const Package = ActorBlueprint->GetOutermost();
	const bool bWasDirty = Package && Package->IsDirty();
	const FSoftObjectPtr OriginalValue = WallClimbProperty->GetPropertyValue_InContainer(CDO);
	const FSoftObjectPath WrongWallRunPath(TEXT("/Game/Variant_Platforming/Anims/WallClimb/Retargeted/A_WallRun_R_Two.A_WallRun_R_Two"));

	WallClimbProperty->SetPropertyValue_InContainer(CDO, FSoftObjectPtr(WrongWallRunPath));

	FMCPToolResult Result;
	{
		FMCPTool_BlueprintQuery Tool;
		Result = Tool.Execute(MCPAnimationPreflightTests::MakePreflightParams(
			TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"),
			{TEXT("wall_climb")},
			TEXT("/Game/Variant_Platforming/Blueprints/BP_PlatformingCharacter")));
	}

	WallClimbProperty->SetPropertyValue_InContainer(CDO, OriginalValue);
	if (Package)
	{
		Package->SetDirtyFlag(bWasDirty);
	}

	TestTrue(TEXT("animation_preflight should succeed while reporting the bad wall-climb binding"), Result.bSuccess);
	TestTrue(TEXT("animation_preflight should return bad-binding report data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> BindingEntry =
		MCPAnimationPreflightTests::FindActorRoleBindingEntry(Result.Data, TEXT("wall_climb"));
	TestTrue(TEXT("Bad wall-climb actor binding should be reported"), BindingEntry.IsValid());
	if (BindingEntry.IsValid())
	{
		AddInfo(FString::Printf(
			TEXT("wall_climb bad-binding status=%s asset=%s semantic_score=%.0f compatibility=%s"),
			*BindingEntry->GetStringField(TEXT("status")),
			*BindingEntry->GetStringField(TEXT("assigned_asset_path")),
			BindingEntry->GetNumberField(TEXT("semantic_match_score")),
			*BindingEntry->GetStringField(TEXT("skeleton_compatibility"))));
		TestEqual(TEXT("Bad wall-run clip should be classified as a semantic mismatch for wall_climb"),
			BindingEntry->GetStringField(TEXT("status")),
			TEXT("assigned_reference_semantic_mismatch"));
		TestEqual(TEXT("Bad wall-climb binding should expose the exact wall-run asset path"),
			BindingEntry->GetStringField(TEXT("assigned_asset_path")),
			WrongWallRunPath.ToString());
		TestEqual(TEXT("Bad wall-run binding should still be skeleton-compatible"),
			BindingEntry->GetStringField(TEXT("skeleton_compatibility")),
			TEXT("compatible"));
		TestEqual(TEXT("Bad wall-run binding should have no semantic wall_climb fit"),
			BindingEntry->GetNumberField(TEXT("semantic_match_score")),
			0.0);
	}

	const TSharedPtr<FJsonObject> RoleEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, TEXT("wall_climb"));
	TestTrue(TEXT("wall_climb role should have a report entry for bad-binding regression"), RoleEntry.IsValid());
	if (RoleEntry.IsValid())
	{
		TestNotEqual(TEXT("wall_climb must not be satisfied by the wrong wall-run actor binding"),
			RoleEntry->GetStringField(TEXT("status")),
			TEXT("matched_with_actor_binding"));
	}

	TestTrue(TEXT("Bad wall-run binding must make wall_climb unsatisfied"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("unsatisfied_roles"), TEXT("wall_climb")) ||
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("missing_roles"), TEXT("wall_climb")));
	TestTrue(TEXT("Bad wall-run binding must emit semantic mismatch blocker"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("blocker_codes"), TEXT("animation_role_binding_semantic_mismatch")));
	TestFalse(TEXT("Bad wall-run binding must not list wall_climb as compatible"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("compatible_roles"), TEXT("wall_climb")));
	TestNotEqual(TEXT("Bad wall-run binding must not classify as gameplay_code_proof"),
		Result.Data->GetStringField(TEXT("proof_classification")),
		TEXT("gameplay_code_proof"));

	const TSharedPtr<FJsonObject>* ProofMatrix = nullptr;
	TestTrue(TEXT("proof_matrix should be present for bad-binding regression"),
		Result.Data->TryGetObjectField(TEXT("proof_matrix"), ProofMatrix) && ProofMatrix && (*ProofMatrix).IsValid());
	if (ProofMatrix && *ProofMatrix)
	{
		TestFalse(TEXT("Bad wall-run binding must not claim gameplay_code_proof"),
			(*ProofMatrix)->GetBoolField(TEXT("gameplay_code_proof")));
		TestTrue(TEXT("Bad wall-run binding should classify as manual blocker"),
			(*ProofMatrix)->GetBoolField(TEXT("manual_blocker")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationPreflightMismatchTest,
	"OsvayderUE.MCP.Tools.BlueprintQuery.AnimationPreflight.MismatchRequiresRetarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPAnimationPreflightMismatchTest::RunTest(const FString& Parameters)
{
	FMCPTool_BlueprintQuery Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationPreflightTests::MakePreflightParams(
		TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"),
		{TEXT("relaxed_idle_ipc")}));

	TestTrue(TEXT("animation_preflight should succeed for mismatch diagnostics"), Result.bSuccess);
	TestTrue(TEXT("animation_preflight should return report data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Skeleton mismatch should classify as manual blocker"),
		Result.Data->GetStringField(TEXT("proof_classification")),
		TEXT("manual_blocker"));
	TestTrue(TEXT("Skeleton mismatch blocker should be present"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("blocker_codes"), TEXT("skeleton_mismatch")));
	TestTrue(TEXT("Retarget recommendation blocker should be present"),
		MCPAnimationPreflightTests::JsonArrayContainsString(Result.Data, TEXT("blocker_codes"), TEXT("retarget_required_not_implemented")));

	const TSharedPtr<FJsonObject>* ProofMatrix = nullptr;
	TestTrue(TEXT("proof_matrix should be present for mismatch report"),
		Result.Data->TryGetObjectField(TEXT("proof_matrix"), ProofMatrix) && ProofMatrix && (*ProofMatrix).IsValid());
	if (ProofMatrix && *ProofMatrix)
	{
		TestTrue(TEXT("Mismatch report should mark manual_blocker"),
			(*ProofMatrix)->GetBoolField(TEXT("manual_blocker")));
		TestFalse(TEXT("Mismatch report should not claim gameplay_code_proof"),
			(*ProofMatrix)->GetBoolField(TEXT("gameplay_code_proof")));
		TestTrue(TEXT("Mismatch report should expose animation_inventory_proof"),
			(*ProofMatrix)->GetBoolField(TEXT("animation_inventory_proof")));
	}

	const TSharedPtr<FJsonObject> RoleEntry = MCPAnimationPreflightTests::FindRoleEntry(Result.Data, TEXT("relaxed_idle_ipc"));
	TestTrue(TEXT("Mismatch role should have a report entry"), RoleEntry.IsValid());
	if (RoleEntry.IsValid())
	{
		TestEqual(TEXT("Mismatch-only role should be unsatisfied"),
			RoleEntry->GetStringField(TEXT("status")),
			TEXT("matched_with_skeleton_mismatch"));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
