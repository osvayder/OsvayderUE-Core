// Copyright Natali Caggiano. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MCP/Tools/MCPTool_Material.h"
#include "MCP/Tools/MCPTool_Asset.h"
#include "MCP/MCPSavePipeline.h"
#include "CharacterDataTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/**
 * Tests for MCPTool_Material and MCPTool_Asset
 */

// ============================================================================
// Material Tool Tests
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolMaterialInfo,
	"OsvayderUE.MCP.Tools.Material.GetInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolMaterialInfo::RunTest(const FString& Parameters)
{
	FMCPTool_Material Tool;
	FMCPToolInfo Info = Tool.GetInfo();

	TestEqual("Tool name should be 'material'", Info.Name, TEXT("material"));
	TestTrue("Should have description", !Info.Description.IsEmpty());
	TestTrue("Should have parameters", Info.Parameters.Num() > 0);
	TestTrue("First param should be operation", Info.Parameters[0].Name == TEXT("operation"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolMaterialMissingOperation,
	"OsvayderUE.MCP.Tools.Material.MissingOperation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolMaterialMissingOperation::RunTest(const FString& Parameters)
{
	FMCPTool_Material Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	// No operation specified

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail without operation", Result.bSuccess);
	TestTrue("Error should mention 'operation'", Result.Message.Contains(TEXT("operation")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolMaterialInvalidOperation,
	"OsvayderUE.MCP.Tools.Material.InvalidOperation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolMaterialInvalidOperation::RunTest(const FString& Parameters)
{
	FMCPTool_Material Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("invalid_operation"));

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail with invalid operation", Result.bSuccess);
	TestTrue("Error should mention valid operations", Result.Message.Contains(TEXT("create_material_instance")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolMaterialCreateMissingParams,
	"OsvayderUE.MCP.Tools.Material.CreateMaterialInstanceMissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolMaterialCreateMissingParams::RunTest(const FString& Parameters)
{
	FMCPTool_Material Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("create_material_instance"));
	// Missing asset_name and parent_material

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail without required params", Result.bSuccess);
	TestTrue("Error should mention missing param", Result.Message.Contains(TEXT("asset_name")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolMaterialCreateInvalidParent,
	"OsvayderUE.MCP.Tools.Material.CreateMaterialInstanceInvalidParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolMaterialCreateInvalidParent::RunTest(const FString& Parameters)
{
	FMCPTool_Material Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("create_material_instance"));
	Params->SetStringField(TEXT("asset_name"), TEXT("MI_Test"));
	Params->SetStringField(TEXT("parent_material"), TEXT("/Game/NonExistent/Material"));

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail with non-existent parent", Result.bSuccess);
	TestTrue("Error should mention failed to load", Result.Message.Contains(TEXT("Failed to load")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolMaterialSetSkeletalMeshMissingParams,
	"OsvayderUE.MCP.Tools.Material.SetSkeletalMeshMaterialMissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolMaterialSetSkeletalMeshMissingParams::RunTest(const FString& Parameters)
{
	FMCPTool_Material Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("set_skeletal_mesh_material"));
	// Missing skeletal_mesh_path and material_path

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail without required params", Result.bSuccess);
	TestTrue("Error should mention missing param",
		Result.Message.Contains(TEXT("skeletal_mesh_path")) || Result.Message.Contains(TEXT("required")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolMaterialGetInfoMissingPath,
	"OsvayderUE.MCP.Tools.Material.GetMaterialInfoMissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolMaterialGetInfoMissingPath::RunTest(const FString& Parameters)
{
	FMCPTool_Material Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("get_material_info"));
	// Missing asset_path

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail without asset_path", Result.bSuccess);
	TestTrue("Error should mention asset_path", Result.Message.Contains(TEXT("asset_path")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolMaterialPathValidation,
	"OsvayderUE.MCP.Tools.Material.PathValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolMaterialPathValidation::RunTest(const FString& Parameters)
{
	FMCPTool_Material Tool;

	// Test path traversal rejection
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("get_material_info"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/../Engine/Materials/BadPath"));

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should reject path traversal", Result.bSuccess);

	// Test engine path rejection
	Params->SetStringField(TEXT("asset_path"), TEXT("/Engine/Materials/EngineMaterial"));
	Result = Tool.Execute(Params);

	TestFalse("Should reject engine paths", Result.bSuccess);

	return true;
}

// ============================================================================
// Asset Tool Tests
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetInfo,
	"OsvayderUE.MCP.Tools.Asset.GetInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetInfo::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;
	FMCPToolInfo Info = Tool.GetInfo();

	TestEqual("Tool name should be 'asset'", Info.Name, TEXT("asset"));
	TestTrue("Should have description", !Info.Description.IsEmpty());
	TestTrue("Should have parameters", Info.Parameters.Num() > 0);
	TestTrue("First param should be operation", Info.Parameters[0].Name == TEXT("operation"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetMissingOperation,
	"OsvayderUE.MCP.Tools.Asset.MissingOperation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetMissingOperation::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	// No operation specified

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail without operation", Result.bSuccess);
	TestTrue("Error should mention 'operation'", Result.Message.Contains(TEXT("operation")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetInvalidOperation,
	"OsvayderUE.MCP.Tools.Asset.InvalidOperation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetInvalidOperation::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("invalid_operation"));

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail with invalid operation", Result.bSuccess);
	TestTrue("Error should mention valid operations", Result.Message.Contains(TEXT("set_asset_property")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetSetPropertyMissingParams,
	"OsvayderUE.MCP.Tools.Asset.SetPropertyMissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetSetPropertyMissingParams::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("set_asset_property"));
	// Missing asset_path, property, value

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail without required params", Result.bSuccess);
	TestTrue("Error should mention missing param",
		Result.Message.Contains(TEXT("asset_path")) || Result.Message.Contains(TEXT("required")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetSetPropertyMissingValue,
	"OsvayderUE.MCP.Tools.Asset.SetPropertyMissingValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetSetPropertyMissingValue::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("set_asset_property"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test/Asset"));
	Params->SetStringField(TEXT("property"), TEXT("SomeProperty"));
	// Missing value

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail without value", Result.bSuccess);
	TestTrue("Error should mention value", Result.Message.Contains(TEXT("value")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetSaveAssetMissingPath,
	"OsvayderUE.MCP.Tools.Asset.SaveAssetMissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetSaveAssetMissingPath::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("save_asset"));
	// Missing asset_path

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail without asset_path", Result.bSuccess);
	TestTrue("Error should mention asset_path", Result.Message.Contains(TEXT("asset_path")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetGetInfoMissingPath,
	"OsvayderUE.MCP.Tools.Asset.GetAssetInfoMissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetGetInfoMissingPath::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("get_asset_info"));
	// Missing asset_path

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail without asset_path", Result.bSuccess);
	TestTrue("Error should mention asset_path", Result.Message.Contains(TEXT("asset_path")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetListAssetsDefault,
	"OsvayderUE.MCP.Tools.Asset.ListAssetsDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetListAssetsDefault::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("list_assets"));
	// Uses default directory /Game/

	FMCPToolResult Result = Tool.Execute(Params);

	// This should succeed even if no assets exist (returns empty list)
	TestTrue("Should succeed with defaults", Result.bSuccess);
	TestTrue("Should have result data", Result.Data.IsValid());
	TestTrue("Should have count field", Result.Data->HasField(TEXT("count")));
	TestTrue("Should have assets array", Result.Data->HasField(TEXT("assets")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetPathValidation,
	"OsvayderUE.MCP.Tools.Asset.PathValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetPathValidation::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	// Test path traversal rejection
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("get_asset_info"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/../Engine/BadPath"));

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should reject path traversal", Result.bSuccess);

	// Test engine path rejection
	Params->SetStringField(TEXT("asset_path"), TEXT("/Engine/SomeAsset"));
	Result = Tool.Execute(Params);

	TestFalse("Should reject engine paths", Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetPropertyPathValidation,
	"OsvayderUE.MCP.Tools.Asset.PropertyPathValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetPropertyPathValidation::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	// Test dangerous property path rejection
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("set_asset_property"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test/Asset"));
	Params->SetStringField(TEXT("property"), TEXT("PropertyWith;Semicolon"));
	Params->SetBoolField(TEXT("value"), true);

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should reject dangerous property path", Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAssetNonExistentAsset,
	"OsvayderUE.MCP.Tools.Asset.NonExistentAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPToolAssetNonExistentAsset::RunTest(const FString& Parameters)
{
	FMCPTool_Asset Tool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("get_asset_info"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/TestAsset12345"));

	FMCPToolResult Result = Tool.Execute(Params);

	TestFalse("Should fail for non-existent asset", Result.bSuccess);
	TestTrue("Error should mention failed to load", Result.Message.Contains(TEXT("Failed to load")));

	return true;
}

// ============================================================================
// P6 regression tests: save_asset delegates to MCPSavePipeline::Run
// ============================================================================

namespace
{
	/**
	 * P6 test helper: create a fresh `/Engine/Transient/<name>` package with a
	 * concrete UCharacterConfigDataAsset top-level asset. Returns the asset
	 * path `/Engine/Transient/<pkg>.<asset>` that LoadObject / LoadAssetByPath
	 * can resolve. Mirrors the P4/P5 pattern (UDataAsset itself is abstract in
	 * UE 5.7; the plugin ships UCharacterConfigDataAsset as a concrete subclass).
	 */
	UCharacterConfigDataAsset* CreateP6TestAssetPackage(
		const FString& ShortName,
		FString& OutAssetPath,
		UPackage*& OutPackage)
	{
		// /Game/ prefix is required — MCPParamValidator::ValidateBlueprintPathParam
		// rejects /Engine/ and /Script/ for security reasons. In-memory-only: we
		// never actually save to disk (tests set SetSavePackageOverride → true),
		// so the /Game/ path doesn't leave any on-disk artifact.
		const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = FString::Printf(
			TEXT("/Game/OsvayderUEP6SaveAssetTest_%s_%s"), *ShortName, *UniqueSuffix);
		OutPackage = CreatePackage(*PackageName);
		if (!OutPackage) return nullptr;
		OutPackage->SetDirtyFlag(false);

		const FName AssetName = FName(*FString::Printf(TEXT("DA_%s"), *ShortName));
		UCharacterConfigDataAsset* Asset = NewObject<UCharacterConfigDataAsset>(
			OutPackage, UCharacterConfigDataAsset::StaticClass(), AssetName, RF_Public | RF_Standalone);
		if (!Asset) return nullptr;

		OutAssetPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName.ToString());
		return Asset;
	}

	UBlueprint* CreateP6TestBlueprintPackage(
		const FString& ShortName,
		FString& OutAssetPath,
		UPackage*& OutPackage)
	{
		const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = FString::Printf(
			TEXT("/Game/OsvayderUEP6SaveAssetBPTest_%s_%s"), *ShortName, *UniqueSuffix);
		OutPackage = CreatePackage(*PackageName);
		if (!OutPackage) return nullptr;

		const FName BlueprintName = FName(*FString::Printf(TEXT("BP_%s"), *ShortName));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), OutPackage, BlueprintName,
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!Blueprint) return nullptr;
		OutPackage->SetDirtyFlag(false);

		OutAssetPath = FString::Printf(TEXT("%s.%s"), *PackageName, *BlueprintName.ToString());
		return Blueprint;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAsset_SaveAsset_Regression_SameBehaviorAsPrePipeline,
	"OsvayderUE.MCP.Tools.Asset.SaveAsset_Regression_SameBehaviorAsPrePipeline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPToolAsset_SaveAsset_Regression_SameBehaviorAsPrePipeline::RunTest(const FString& /*Parameters*/)
{
	// P6 A-P6-2 regression guard. `save_asset` tool output schema is preserved
	// post-refactor: a successful call on a loadable asset with a dirty package
	// must still return `{asset_path, saved=true, marked_dirty=<per-flag>}` and
	// bSuccess=true. No new required params.
	//
	// Phase C save is stubbed via Testing::SetSavePackageOverride so the test
	// does not depend on the transient-path on-disk save semantic.

	FString AssetPath;
	UPackage* Package = nullptr;
	UCharacterConfigDataAsset* Asset = CreateP6TestAssetPackage(TEXT("RegressionSame"), AssetPath, Package);
	if (!TestTrue(TEXT("test asset package must be creatable"), Asset != nullptr && Package != nullptr))
	{
		return false;
	}
	Package->SetDirtyFlag(true);

	OsvayderUE::SavePipeline::Testing::SetSavePackageOverride(
		[](UPackage* /*Pkg*/, const FString& /*Filename*/) -> bool { return true; });
	OsvayderUE::SavePipeline::Testing::SetAssetCreatedOverride([](UObject* /*A*/) {});
	ON_SCOPE_EXIT
	{
		OsvayderUE::SavePipeline::Testing::ClearSavePackageOverride();
		OsvayderUE::SavePipeline::Testing::ClearAssetCreatedOverride();
	};

	FMCPTool_Asset Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("save_asset"));
	Params->SetStringField(TEXT("asset_path"), AssetPath);

	const FMCPToolResult Result = Tool.Execute(Params);

	TestTrue(TEXT("save_asset on dirty loadable asset should succeed"), Result.bSuccess);
	TestTrue(TEXT("result message should read 'Saved asset: <path>'"),
		Result.Message.Contains(TEXT("Saved asset:")) && Result.Message.Contains(AssetPath));
	if (TestTrue(TEXT("result data must be present"), Result.Data.IsValid()))
	{
		FString ReturnedPath;
		bool bSaved = false;
		bool bMarkedDirty = true; // default non-zero so we can detect the flag was set
		Result.Data->TryGetStringField(TEXT("asset_path"), ReturnedPath);
		Result.Data->TryGetBoolField(TEXT("saved"), bSaved);
		Result.Data->TryGetBoolField(TEXT("marked_dirty"), bMarkedDirty);
		TestEqual(TEXT("result asset_path matches input"), ReturnedPath, AssetPath);
		TestTrue(TEXT("result.saved must be true"), bSaved);
		// Default path with save=true + mark_dirty omitted → bMarkDirty = !bSave = false
		TestFalse(TEXT("result.marked_dirty must be false when save=true and mark_dirty defaulted"), bMarkedDirty);
	}

	Package->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAsset_SaveAsset_NowCompilesBlueprint,
	"OsvayderUE.MCP.Tools.Asset.SaveAsset_NowCompilesBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPToolAsset_SaveAsset_NowCompilesBlueprint::RunTest(const FString& /*Parameters*/)
{
	// P6 A-P6-3 new-coverage guard. Pre-P6 `ExecuteSaveAsset` skipped the
	// compile step entirely. Post-P6 it delegates to MCPSavePipeline::Run
	// which runs Phase A compile-before-save for every Blueprint-containing
	// package. Observable proof: after `save_asset` returns, the Blueprint's
	// Status must be BS_UpToDate (or BS_UpToDateWithWarnings) — set by the
	// pipeline's `CompileBlueprint` call, NOT the mock tool.

	FString AssetPath;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateP6TestBlueprintPackage(TEXT("Compile"), AssetPath, Package);
	if (!TestTrue(TEXT("blueprint test package must be creatable"), Blueprint != nullptr && Package != nullptr))
	{
		return false;
	}
	// Force Blueprint status to BS_Dirty so we can observe the pipeline's
	// compile flip it to BS_UpToDate. FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified
	// (called by Phase A) transitions from BS_Dirty back to UpToDate on successful compile.
	Blueprint->Status = EBlueprintStatus::BS_Dirty;
	Package->SetDirtyFlag(true);

	OsvayderUE::SavePipeline::Testing::SetSavePackageOverride(
		[](UPackage* /*Pkg*/, const FString& /*Filename*/) -> bool { return true; });
	OsvayderUE::SavePipeline::Testing::SetAssetCreatedOverride([](UObject* /*A*/) {});
	ON_SCOPE_EXIT
	{
		OsvayderUE::SavePipeline::Testing::ClearSavePackageOverride();
		OsvayderUE::SavePipeline::Testing::ClearAssetCreatedOverride();
	};

	FMCPTool_Asset Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("save_asset"));
	Params->SetStringField(TEXT("asset_path"), AssetPath);

	const FMCPToolResult Result = Tool.Execute(Params);

	TestTrue(TEXT("save_asset on a Blueprint must succeed post-P6"), Result.bSuccess);
	TestTrue(TEXT("Blueprint status must be up-to-date after the pipeline's compile pass"),
		Blueprint->Status == EBlueprintStatus::BS_UpToDate
			|| Blueprint->Status == EBlueprintStatus::BS_UpToDateWithWarnings);

	Package->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPToolAsset_SaveAsset_NowNotifiesAssetRegistry,
	"OsvayderUE.MCP.Tools.Asset.SaveAsset_NowNotifiesAssetRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMCPToolAsset_SaveAsset_NowNotifiesAssetRegistry::RunTest(const FString& /*Parameters*/)
{
	// P6 A-P6-4 new-coverage guard. Pre-P6 `ExecuteSaveAsset` never touched
	// the AssetRegistry — brand-new assets saved via `save_asset` would land
	// on disk but stay invisible to registry-reader agents until the next
	// editor reboot. Post-P6 Phase D runs after Phase C, so a fresh asset
	// under a never-saved package gets `FAssetRegistryModule::AssetCreated`
	// fired for its main UObject. Captured here via the P4 Testing seam.

	FString AssetPath;
	UPackage* Package = nullptr;
	UCharacterConfigDataAsset* Asset = CreateP6TestAssetPackage(TEXT("RegistryNotify"), AssetPath, Package);
	if (!TestTrue(TEXT("test asset package must be creatable"), Asset != nullptr && Package != nullptr))
	{
		return false;
	}
	Package->SetDirtyFlag(true);

	int32 AssetCreatedCallCount = 0;
	UObject* CapturedAsset = nullptr;
	OsvayderUE::SavePipeline::Testing::SetSavePackageOverride(
		[](UPackage* /*Pkg*/, const FString& /*Filename*/) -> bool { return true; });
	OsvayderUE::SavePipeline::Testing::SetAssetCreatedOverride(
		[&AssetCreatedCallCount, &CapturedAsset](UObject* NewAsset)
		{
			++AssetCreatedCallCount;
			CapturedAsset = NewAsset;
		});
	ON_SCOPE_EXIT
	{
		OsvayderUE::SavePipeline::Testing::ClearSavePackageOverride();
		OsvayderUE::SavePipeline::Testing::ClearAssetCreatedOverride();
	};

	FMCPTool_Asset Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("save_asset"));
	Params->SetStringField(TEXT("asset_path"), AssetPath);

	const FMCPToolResult Result = Tool.Execute(Params);

	TestTrue(TEXT("save_asset on a brand-new asset must succeed post-P6"), Result.bSuccess);
	TestEqual(TEXT("FAssetRegistryModule::AssetCreated override must be invoked exactly once"),
		AssetCreatedCallCount, 1);
	TestTrue(TEXT("captured asset must be the UCharacterConfigDataAsset from the package"),
		CapturedAsset == Asset);

	Package->SetDirtyFlag(false);
	return true;
}
