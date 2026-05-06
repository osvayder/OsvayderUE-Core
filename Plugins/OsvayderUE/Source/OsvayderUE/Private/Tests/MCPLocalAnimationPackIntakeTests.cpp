// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_LocalAnimationPackIntake.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"

namespace
{
	FString MakeIntakeTestRoot(const FString& TestName)
	{
		FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("OsvayderUE"),
			TEXT("Automation"),
			TEXT("LocalAnimationPackIntake"),
			TestName);
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	bool WriteTextFile(const FString& Path, const FString& Text)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	TSharedRef<FJsonObject> MakeParams(
		const FString& SourceRoot,
		const FString& DestinationContentRoot,
		const FString& Mode = TEXT("dry_run"),
		const FString& DestinationGameRoot = TEXT("/Game/ParkourAnimations"))
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_root"), SourceRoot);
		Params->SetStringField(TEXT("mode"), Mode);
		Params->SetStringField(TEXT("destination_game_root"), DestinationGameRoot);
		Params->SetStringField(TEXT("test_destination_content_root"), DestinationContentRoot);
		Params->SetNumberField(TEXT("max_files"), 100);
		Params->SetNumberField(TEXT("max_total_size_mb"), 64);
		return Params;
	}

	bool JsonObjectArrayContainsString(
		const TSharedPtr<FJsonObject>& Object,
		const FString& ArrayFieldName,
		const FString& FieldName,
		const FString& ExpectedValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Object->TryGetArrayField(ArrayFieldName, Array) || !Array)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry || !(*Entry).IsValid())
			{
				continue;
			}

			FString Actual;
			if ((*Entry)->TryGetStringField(FieldName, Actual) && Actual.Equals(ExpectedValue, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool JsonStringArrayContains(const TSharedPtr<FJsonObject>& Object, const FString& ArrayFieldName, const FString& ExpectedValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Object->TryGetArrayField(ArrayFieldName, Array) || !Array)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			FString Actual;
			if (Value.IsValid() && Value->TryGetString(Actual) && Actual.Equals(ExpectedValue, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPLocalAnimationPackIntake_RegistryAndSchema,
	"OsvayderUE.LocalAnimationPackIntake.RegistryAndSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPLocalAnimationPackIntake_RejectsDriveRoot,
	"OsvayderUE.LocalAnimationPackIntake.RejectsDriveRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPLocalAnimationPackIntake_DryRunWrapperPreservesLayout,
	"OsvayderUE.LocalAnimationPackIntake.DryRunWrapperPreservesLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPLocalAnimationPackIntake_ExecuteSkipsMatchingAndCopiesMissing,
	"OsvayderUE.LocalAnimationPackIntake.ExecuteSkipsMatchingAndCopiesMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPLocalAnimationPackIntake_MismatchedExistingFileBlocks,
	"OsvayderUE.LocalAnimationPackIntake.MismatchedExistingFileBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPLocalAnimationPackIntake_CopyCallIsNonReplacing,
	"OsvayderUE.LocalAnimationPackIntake.CopyCallIsNonReplacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPLocalAnimationPackIntake_RegistryAndSchema::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestTrue(TEXT("registry should expose local_animation_pack_intake"), Registry.HasTool(TEXT("local_animation_pack_intake")));

	FMCPTool_LocalAnimationPackIntake Tool;
	const FMCPToolInfo Info = Tool.GetInfo();
	TestEqual(TEXT("tool should use exact requested name"), Info.Name, FString(TEXT("local_animation_pack_intake")));
	TestFalse(TEXT("tool should not be destructive"), Info.Annotations.bDestructiveHint);
	TestFalse(TEXT("tool is an execute-capable copy surface, not globally read-only"), Info.Annotations.bReadOnlyHint);
	TestTrue(TEXT("description should mention bounded source_root"), Info.Description.Contains(TEXT("source_root"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("description should mention no overwrite behavior"), Info.Description.Contains(TEXT("never overwrites"), ESearchCase::IgnoreCase));
	return true;
}

bool FMCPLocalAnimationPackIntake_RejectsDriveRoot::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeIntakeTestRoot(TEXT("RejectsDriveRoot"));
	const FString DestinationContentRoot = FPaths::Combine(TestRoot, TEXT("Content"));

	FMCPTool_LocalAnimationPackIntake Tool;
	const FMCPToolResult Result = Tool.Execute(MakeParams(TEXT("D:\\"), DestinationContentRoot));

	TestFalse(TEXT("drive root should be rejected"), Result.bSuccess);
	TestTrue(TEXT("drive-root rejection should return structured data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("drive-root blocker code should be explicit"),
		Result.Data->GetStringField(TEXT("blocker_code")),
		FString(TEXT("rejected_broad_root")));
	return true;
}

bool FMCPLocalAnimationPackIntake_DryRunWrapperPreservesLayout::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeIntakeTestRoot(TEXT("DryRunWrapperPreservesLayout"));
	const FString WrapperRoot = FPaths::Combine(TestRoot, TEXT("7 -Parkour Animation"));
	const FString PackRoot = FPaths::Combine(WrapperRoot, TEXT("ParkourAnimations"));
	const FString DestinationContentRoot = FPaths::Combine(TestRoot, TEXT("TempContent"));

	TestTrue(TEXT("animation asset fixture should be written"),
		WriteTextFile(FPaths::Combine(PackRoot, TEXT("Animations/Locomotion/WallRun.uasset")), TEXT("wallrun")));
	TestTrue(TEXT("nested mesh dependency fixture should be written"),
		WriteTextFile(FPaths::Combine(PackRoot, TEXT("Characters/Mannequins/Meshes/SK_Mannequin.uasset")), TEXT("mesh")));
	TestTrue(TEXT("map fixture should be written"),
		WriteTextFile(FPaths::Combine(PackRoot, TEXT("Levels/ParkourGym.umap")), TEXT("map")));

	FMCPTool_LocalAnimationPackIntake Tool;
	const FMCPToolResult Result = Tool.Execute(MakeParams(WrapperRoot, DestinationContentRoot));

	if (!Result.bSuccess)
	{
		AddError(FString::Printf(TEXT("dry-run intake failed: %s"), *Result.Message));
	}
	TestTrue(TEXT("dry-run should accept wrapper with real pack child"), Result.bSuccess);
	TestTrue(TEXT("dry-run should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("selected_source_root should choose real child pack root"),
		Result.Data->GetStringField(TEXT("selected_source_root")).EndsWith(TEXT("ParkourAnimations")));
	TestEqual(TEXT("destination_game_root should preserve inferred mount"),
		Result.Data->GetStringField(TEXT("destination_game_root")),
		FString(TEXT("/Game/ParkourAnimations")));
	TestEqual(TEXT("all three candidate package files should be missing"),
		static_cast<int32>(Result.Data->GetNumberField(TEXT("missing_count"))),
		3);
	TestTrue(TEXT("nested dependency layout should not be flattened"),
		JsonObjectArrayContainsString(
			Result.Data,
			TEXT("missing_files"),
			TEXT("relative_path"),
			TEXT("Characters/Mannequins/Meshes/SK_Mannequin.uasset")));
	TestTrue(TEXT("animation candidate package path should be reported"),
		JsonStringArrayContains(
			Result.Data,
			TEXT("candidate_animation_paths"),
			TEXT("/Game/ParkourAnimations/Animations/Locomotion/WallRun")));
	return true;
}

bool FMCPLocalAnimationPackIntake_ExecuteSkipsMatchingAndCopiesMissing::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeIntakeTestRoot(TEXT("ExecuteSkipsMatchingAndCopiesMissing"));
	const FString PackRoot = FPaths::Combine(TestRoot, TEXT("ParkourAnimations"));
	const FString DestinationContentRoot = FPaths::Combine(TestRoot, TEXT("TempContent"));
	const FString ExistingDestination = FPaths::Combine(DestinationContentRoot, TEXT("ParkourAnimations/Animations/Locomotion/WallRun.uasset"));
	const FString MissingDestination = FPaths::Combine(DestinationContentRoot, TEXT("ParkourAnimations/Characters/Mannequins/Meshes/SK_Mannequin.uasset"));

	TestTrue(TEXT("source animation should be written"),
		WriteTextFile(FPaths::Combine(PackRoot, TEXT("Animations/Locomotion/WallRun.uasset")), TEXT("same-bytes")));
	TestTrue(TEXT("source dependency should be written"),
		WriteTextFile(FPaths::Combine(PackRoot, TEXT("Characters/Mannequins/Meshes/SK_Mannequin.uasset")), TEXT("missing-dependency")));
	TestTrue(TEXT("matching destination should be pre-seeded"),
		WriteTextFile(ExistingDestination, TEXT("same-bytes")));

	FMCPTool_LocalAnimationPackIntake Tool;
	const FMCPToolResult Result = Tool.Execute(MakeParams(PackRoot, DestinationContentRoot, TEXT("execute")));

	if (!Result.bSuccess)
	{
		AddError(FString::Printf(TEXT("execute intake failed: %s"), *Result.Message));
	}
	TestTrue(TEXT("execute should succeed when existing files byte-match"), Result.bSuccess);
	TestTrue(TEXT("execute should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("matching existing file should be skipped"),
		static_cast<int32>(Result.Data->GetNumberField(TEXT("already_present_count"))),
		1);
	TestEqual(TEXT("one missing dependency should be copied"),
		static_cast<int32>(Result.Data->GetNumberField(TEXT("missing_count"))),
		1);
	TestTrue(TEXT("missing destination should exist after execute"), IFileManager::Get().FileExists(*MissingDestination));

	FString ExistingContents;
	TestTrue(TEXT("existing destination should remain readable"), FFileHelper::LoadFileToString(ExistingContents, *ExistingDestination));
	TestEqual(TEXT("existing destination should not be overwritten"), ExistingContents, FString(TEXT("same-bytes")));
	return true;
}

bool FMCPLocalAnimationPackIntake_MismatchedExistingFileBlocks::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeIntakeTestRoot(TEXT("MismatchedExistingFileBlocks"));
	const FString PackRoot = FPaths::Combine(TestRoot, TEXT("ParkourAnimations"));
	const FString DestinationContentRoot = FPaths::Combine(TestRoot, TEXT("TempContent"));
	const FString Destination = FPaths::Combine(DestinationContentRoot, TEXT("ParkourAnimations/Animations/Locomotion/WallRun.uasset"));

	TestTrue(TEXT("source animation should be written"),
		WriteTextFile(FPaths::Combine(PackRoot, TEXT("Animations/Locomotion/WallRun.uasset")), TEXT("source-bytes")));
	TestTrue(TEXT("mismatched destination should be pre-seeded"),
		WriteTextFile(Destination, TEXT("different-bytes")));

	FMCPTool_LocalAnimationPackIntake Tool;
	const FMCPToolResult Result = Tool.Execute(MakeParams(PackRoot, DestinationContentRoot, TEXT("execute")));

	TestFalse(TEXT("execute should block on mismatched existing file"), Result.bSuccess);
	TestTrue(TEXT("conflict result should return structured data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	if (!Result.Data->GetStringField(TEXT("blocker_code")).Equals(TEXT("destination_conflict"), ESearchCase::IgnoreCase))
	{
		AddError(FString::Printf(
			TEXT("unexpected blocker code %s: %s"),
			*Result.Data->GetStringField(TEXT("blocker_code")),
			*Result.Message));
	}
	TestEqual(TEXT("conflict blocker code should be explicit"),
		Result.Data->GetStringField(TEXT("blocker_code")),
		FString(TEXT("destination_conflict")));
	TestEqual(TEXT("conflict count should be one"),
		static_cast<int32>(Result.Data->GetNumberField(TEXT("conflict_count"))),
		1);
	TestTrue(TEXT("conflict row should name the relative path"),
		JsonObjectArrayContainsString(
			Result.Data,
			TEXT("conflict_files"),
			TEXT("relative_path"),
			TEXT("Animations/Locomotion/WallRun.uasset")));

	FString DestinationContents;
	TestTrue(TEXT("destination should remain readable"), FFileHelper::LoadFileToString(DestinationContents, *Destination));
	TestEqual(TEXT("mismatched destination should not be overwritten"), DestinationContents, FString(TEXT("different-bytes")));
	return true;
}

bool FMCPLocalAnimationPackIntake_CopyCallIsNonReplacing::RunTest(const FString& Parameters)
{
	const FString ToolSource = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins/OsvayderUE/Source/OsvayderUE/Private/MCP/Tools/MCPTool_LocalAnimationPackIntake.cpp")));

	FString SourceText;
	TestTrue(TEXT("local animation intake source should be readable"), FFileHelper::LoadFileToString(SourceText, *ToolSource));
	if (SourceText.IsEmpty())
	{
		return false;
	}

	TestFalse(
		TEXT("execute copy must not use replacing/read-only override flags"),
		SourceText.Contains(TEXT("Copy(*Row.DestinationFile, *Row.SourceFile, true, true)")));
	TestTrue(
		TEXT("execute copy must explicitly use non-replacing FileManager semantics"),
		SourceText.Contains(TEXT("Copy(*Row.DestinationFile, *Row.SourceFile, false, false)")));
	TestTrue(
		TEXT("execute copy must verify copied bytes after FileManager succeeds"),
		SourceText.Contains(TEXT("copy_verification_failed")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
