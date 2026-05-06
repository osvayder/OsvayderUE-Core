// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_AnimationRetargetFixup.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"

namespace MCPAnimationRetargetFixupTests
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

	TSharedRef<FJsonObject> MakeParams(
		const FString& TargetAnimBlueprintPath,
		const FString& AssetRoot,
		const FString& DestinationRoot,
		const FString& Mode = TEXT("dry_run"))
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_anim_blueprint_path"), TargetAnimBlueprintPath);
		Params->SetStringField(TEXT("asset_root"), AssetRoot);
		Params->SetStringField(TEXT("destination_game_root"), DestinationRoot);
		Params->SetStringField(TEXT("mode"), Mode);
		Params->SetNumberField(TEXT("match_limit"), 20);
		return Params;
	}

	bool JsonStringArrayContains(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& Expected)
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
			FString Actual;
			if (Value.IsValid() && Value->TryGetString(Actual) && Actual == Expected)
			{
				return true;
			}
		}
		return false;
	}

	bool JsonStringArrayContainsAny(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const TArray<FString>& ExpectedValues)
	{
		for (const FString& Expected : ExpectedValues)
		{
			if (JsonStringArrayContains(Object, FieldName, Expected))
			{
				return true;
			}
		}
		return false;
	}

	TSharedRef<FJsonObject> MakeWallRunParams(const FString& Mode = TEXT("dry_run"))
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_anim_blueprint_path"), TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"));
		Params->SetStringField(TEXT("destination_game_root"), TEXT("/Game/Variant_Platforming/Anims/WallRun/Retargeted"));
		Params->SetStringField(TEXT("mode"), Mode);
		Params->SetStringField(TEXT("asset_root"), TEXT("/Game/ParkourAnimations/Animations/UE5_Skelet/WallRun/WallRun_Horizontal"));
		Params->SetArrayField(TEXT("candidate_animation_paths"), MakeStringArray({
			TEXT("/Game/ParkourAnimations/Animations/UE5_Skelet/WallRun/WallRun_Horizontal/A_WallRun_Horizontal_L_Cycle_IP_Retargeted"),
			TEXT("/Game/ParkourAnimations/Animations/UE5_Skelet/WallRun/WallRun_Horizontal/A_WallRun_Horizontal_R_Cycle_IP_Retargeted")}));
		Params->SetNumberField(TEXT("match_limit"), 2);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationRetargetFixupRegistryAndSchemaTest,
	"OsvayderUE.MCP.Tools.AnimationRetargetFixup.RegistryAndSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAnimationRetargetFixupRegistryAndSchemaTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestTrue(TEXT("registry should expose animation_retarget_fixup"), Registry.HasTool(TEXT("animation_retarget_fixup")));

	FMCPTool_AnimationRetargetFixup Tool;
	const FMCPToolInfo Info = Tool.GetInfo();
	TestEqual(TEXT("tool should use exact requested name"), Info.Name, FString(TEXT("animation_retarget_fixup")));
	TestTrue(TEXT("description should mention skeleton compatibility"),
		Info.Description.Contains(TEXT("skeleton compatibility"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("description should mention no overwrite behavior"),
		Info.Description.Contains(TEXT("Never overwrites"), ESearchCase::CaseSensitive));

	bool bHasTargetAnimBP = false;
	bool bHasTargetSkeleton = false;
	bool bHasCandidates = false;
	bool bHasAssetRoot = false;
	bool bHasDestination = false;
	bool bHasMode = false;
	bool bHasRetargeter = false;
	bool bHasSourceMesh = false;
	bool bHasTargetMesh = false;
	for (const FMCPToolParameter& Param : Info.Parameters)
	{
		bHasTargetAnimBP |= Param.Name == TEXT("target_anim_blueprint_path");
		bHasTargetSkeleton |= Param.Name == TEXT("target_skeleton_path");
		bHasCandidates |= Param.Name == TEXT("candidate_animation_paths");
		bHasAssetRoot |= Param.Name == TEXT("asset_root");
		bHasDestination |= Param.Name == TEXT("destination_game_root");
		bHasMode |= Param.Name == TEXT("mode");
		bHasRetargeter |= Param.Name == TEXT("retargeter_asset_path");
		bHasSourceMesh |= Param.Name == TEXT("source_mesh_path");
		bHasTargetMesh |= Param.Name == TEXT("target_mesh_path");
	}

	TestTrue(TEXT("schema should declare target_anim_blueprint_path"), bHasTargetAnimBP);
	TestTrue(TEXT("schema should declare target_skeleton_path"), bHasTargetSkeleton);
	TestTrue(TEXT("schema should declare candidate_animation_paths"), bHasCandidates);
	TestTrue(TEXT("schema should declare asset_root"), bHasAssetRoot);
	TestTrue(TEXT("schema should declare destination_game_root"), bHasDestination);
	TestTrue(TEXT("schema should declare mode"), bHasMode);
	TestTrue(TEXT("schema should declare retargeter_asset_path"), bHasRetargeter);
	TestTrue(TEXT("schema should declare source_mesh_path"), bHasSourceMesh);
	TestTrue(TEXT("schema should declare target_mesh_path"), bHasTargetMesh);

	const FString RouterPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins/OsvayderUE/Resources/mcp-bridge/tool-router.js")));
	FString RouterText;
	TestTrue(TEXT("tool router should be readable"), FFileHelper::LoadFileToString(RouterText, *RouterPath));
	TestTrue(TEXT("tool router should expose animation_retarget_fixup as a simple tool"),
		RouterText.Contains(TEXT("\"animation_retarget_fixup\""), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationRetargetFixupDetectsSkeletonMismatchTest,
	"OsvayderUE.MCP.Tools.AnimationRetargetFixup.DetectsSkeletonMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAnimationRetargetFixupDetectsSkeletonMismatchTest::RunTest(const FString& Parameters)
{
	FMCPTool_AnimationRetargetFixup Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationRetargetFixupTests::MakeParams(
		TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"),
		TEXT("/Game/MCO_Demo_Pack/Animation/Mobility/IPC"),
		TEXT("/Game/OsvayderUE/RetargetFixupTest")));

	TestTrue(TEXT("dry-run mismatch diagnostic should succeed"), Result.bSuccess);
	TestTrue(TEXT("dry-run mismatch should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("mismatch root should discover candidates"), Result.Data->GetNumberField(TEXT("candidate_count")) > 0);
	TestTrue(TEXT("mismatch root should report mismatched assets"), Result.Data->GetNumberField(TEXT("mismatched_count")) > 0);
	TestTrue(TEXT("mismatch root should emit skeleton_mismatch"),
		MCPAnimationRetargetFixupTests::JsonStringArrayContains(Result.Data, TEXT("blocker_codes"), TEXT("skeleton_mismatch")));
	TestTrue(TEXT("mismatch root should report discovered routes or exact missing route components"),
		Result.Data->GetNumberField(TEXT("discovered_route_count")) > 0 ||
		MCPAnimationRetargetFixupTests::JsonStringArrayContainsAny(Result.Data, TEXT("blocker_codes"), {
			TEXT("retargeter_asset_missing"),
			TEXT("source_ik_rig_missing"),
			TEXT("target_ik_rig_missing"),
			TEXT("target_mesh_missing"),
			TEXT("retarget_api_unavailable")}));
	TestTrue(TEXT("destination plan should be built for mismatches"), Result.Data->GetNumberField(TEXT("planned_retarget_count")) > 0);
	TestFalse(TEXT("mismatched assets should not be ready for hookup"),
		Result.Data->GetBoolField(TEXT("ready_for_anim_blueprint_hookup")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationRetargetFixupCompatibleNoopTest,
	"OsvayderUE.MCP.Tools.AnimationRetargetFixup.CompatibleNoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAnimationRetargetFixupCompatibleNoopTest::RunTest(const FString& Parameters)
{
	FMCPTool_AnimationRetargetFixup Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationRetargetFixupTests::MakeParams(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"),
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed"),
		TEXT("/Game/OsvayderUE/RetargetFixupTest")));

	TestTrue(TEXT("compatible dry-run should succeed"), Result.bSuccess);
	TestTrue(TEXT("compatible dry-run should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("compatible root should discover compatible assets"), Result.Data->GetNumberField(TEXT("compatible_count")) > 0);
	TestEqual(TEXT("compatible no-op should have no planned retargets"),
		static_cast<int32>(Result.Data->GetNumberField(TEXT("planned_retarget_count"))),
		0);
	TestFalse(TEXT("compatible no-op should not emit retarget unavailable"),
		MCPAnimationRetargetFixupTests::JsonStringArrayContains(Result.Data, TEXT("blocker_codes"), TEXT("retarget_tooling_unavailable")));
	TestTrue(TEXT("compatible no-op should be ready for hookup"),
		Result.Data->GetBoolField(TEXT("ready_for_anim_blueprint_hookup")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationRetargetFixupDestinationConflictTest,
	"OsvayderUE.MCP.Tools.AnimationRetargetFixup.DestinationConflictNoOverwrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAnimationRetargetFixupDestinationConflictTest::RunTest(const FString& Parameters)
{
	FMCPTool_AnimationRetargetFixup Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationRetargetFixupTests::MakeParams(
		TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"),
		TEXT("/Game/MCO_Demo_Pack/Animation/Mobility/IPC"),
		TEXT("/Game/MCO_Demo_Pack/Animation/Mobility/IPC")));

	TestTrue(TEXT("conflict dry-run should still return diagnostics"), Result.bSuccess);
	TestTrue(TEXT("conflict dry-run should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("existing destination packages should be conflicts"),
		Result.Data->GetNumberField(TEXT("destination_conflict_count")) > 0);
	TestTrue(TEXT("destination conflict blocker should be explicit"),
		MCPAnimationRetargetFixupTests::JsonStringArrayContains(Result.Data, TEXT("blocker_codes"), TEXT("destination_conflict")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationRetargetFixupExecuteBlocksWithoutRetargetToolingTest,
	"OsvayderUE.MCP.Tools.AnimationRetargetFixup.ExecuteVerifiedOrExactBlocker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAnimationRetargetFixupExecuteBlocksWithoutRetargetToolingTest::RunTest(const FString& Parameters)
{
	FMCPTool_AnimationRetargetFixup Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationRetargetFixupTests::MakeParams(
		TEXT("/Game/Variant_Platforming/Anims/ABP_Manny_Platforming"),
		TEXT("/Game/MCO_Demo_Pack/Animation/Mobility/IPC"),
		TEXT("/Game/OsvayderUE/RetargetFixupTest"),
		TEXT("execute")));

	TestFalse(TEXT("execute should block rather than fake retarget success"), Result.bSuccess);
	TestTrue(TEXT("execute blocker should return structured data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("execute blocker should be a precise retarget route blocker"),
		MCPAnimationRetargetFixupTests::JsonStringArrayContainsAny(Result.Data, TEXT("blocker_codes"), {
			TEXT("retargeter_asset_missing"),
			TEXT("source_ik_rig_missing"),
			TEXT("target_ik_rig_missing"),
			TEXT("source_mesh_missing"),
			TEXT("source_mesh_skeleton_mismatch"),
			TEXT("target_mesh_missing"),
			TEXT("target_mesh_skeleton_mismatch"),
			TEXT("retarget_api_unavailable"),
			TEXT("destination_conflict"),
			TEXT("retarget_execute_verification_failed")}));
	TestTrue(TEXT("execute blocker should keep next_tool_call recommendation"),
		Result.Data->HasTypedField<EJson::Object>(TEXT("next_tool_call")));
	TestFalse(TEXT("execute blocker should not claim retarget success"),
		Result.Data->GetBoolField(TEXT("ready_to_claim_retarget_success")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationRetargetFixupWallRunRouteDiscoveryTest,
	"OsvayderUE.MCP.Tools.AnimationRetargetFixup.WallRunRouteDiscovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAnimationRetargetFixupWallRunRouteDiscoveryTest::RunTest(const FString& Parameters)
{
	FMCPTool_AnimationRetargetFixup Tool;
	const FMCPToolResult Result = Tool.Execute(MCPAnimationRetargetFixupTests::MakeWallRunParams(TEXT("dry_run")));

	TestTrue(TEXT("wallrun dry-run should return diagnostics"), Result.bSuccess);
	TestTrue(TEXT("wallrun dry-run should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("wallrun route discovery should inspect route assets or report exact missing component"),
		Result.Data->GetNumberField(TEXT("discovered_route_count")) > 0 ||
		MCPAnimationRetargetFixupTests::JsonStringArrayContainsAny(Result.Data, TEXT("blocker_codes"), {
			TEXT("retargeter_asset_missing"),
			TEXT("source_ik_rig_missing"),
			TEXT("target_ik_rig_missing"),
			TEXT("target_mesh_missing"),
			TEXT("retarget_api_unavailable")}));
	TestTrue(TEXT("wallrun should have either retarget plan or compatible assets"),
		Result.Data->GetNumberField(TEXT("planned_retarget_count")) > 0 ||
		Result.Data->GetNumberField(TEXT("compatible_count")) > 0);
	TestTrue(TEXT("dry-run should expose selected route executability"),
		Result.Data->HasTypedField<EJson::Boolean>(TEXT("safe_route_executable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAnimationRetargetFixupBrokenCandidateSkippedTest,
	"OsvayderUE.MCP.Tools.AnimationRetargetFixup.BrokenCandidateSkipped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPAnimationRetargetFixupBrokenCandidateSkippedTest::RunTest(const FString& Parameters)
{
	FMCPTool_AnimationRetargetFixup Tool;
	TSharedRef<FJsonObject> Params = MCPAnimationRetargetFixupTests::MakeWallRunParams(TEXT("dry_run"));
	Params->SetArrayField(TEXT("candidate_animation_paths"), MCPAnimationRetargetFixupTests::MakeStringArray({
		TEXT("/Game/ParkourAnimations/Animations/UE4_Skelet/Drop/A_Dash_Drop"),
		TEXT("/Game/ParkourAnimations/Animations/UE5_Skelet/WallRun/WallRun_Horizontal/A_WallRun_Horizontal_L_Cycle_IP_Retargeted"),
		TEXT("/Game/ParkourAnimations/Animations/UE5_Skelet/WallRun/WallRun_Horizontal/A_WallRun_Horizontal_R_Cycle_IP_Retargeted")}));

	AddExpectedError(TEXT("Unable to retrieve target USkeleton"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("Unable to initialize FKControlRig"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("Unable to retrieve valid URigHierarchy"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("Unable to retrieve RigHierarchy for ControlRig"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("Invalid USkeleton supplied"), EAutomationExpectedErrorFlags::Contains, 1);

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("broken unrelated candidate should not make dry-run fatal"), Result.bSuccess);
	TestTrue(TEXT("broken candidate result should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("broken candidate should be reported as warning"),
		MCPAnimationRetargetFixupTests::JsonStringArrayContains(Result.Data, TEXT("warning_codes"), TEXT("broken_candidate_animation")));
	TestTrue(TEXT("valid wallrun candidates should still be considered"),
		Result.Data->GetNumberField(TEXT("mismatched_count")) > 0 ||
		Result.Data->GetNumberField(TEXT("compatible_count")) > 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
