// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_MapRuntimeProof.h"
#include "MCP/Tools/MCPTool_ReportArtifactStatus.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	bool DoesMapPackageExist(const FString& PackagePath)
	{
		FString Filename;
		return FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetMapPackageExtension())
			&& FPaths::FileExists(Filename);
	}

	bool TryGetCurrentProjectMapPackage(FString& OutMapPackage)
	{
		const TArray<FString> PreferredMaps = {
			TEXT("/Game/ThirdPerson/Lvl_ThirdPerson"),
			TEXT("/Game/Variant_Combat/Lvl_Combat"),
			TEXT("/Game/Variant_Platforming/Lvl_Platforming"),
			TEXT("/Game/Variant_SideScrolling/Lvl_SideScrolling")
		};
		for (const FString& PreferredMap : PreferredMaps)
		{
			if (DoesMapPackageExist(PreferredMap))
			{
				OutMapPackage = PreferredMap;
				return true;
			}
		}

		TArray<FString> MapFiles;
		IFileManager::Get().FindFilesRecursive(MapFiles, *FPaths::ProjectContentDir(), TEXT("*.umap"), true, false, false);
		MapFiles.Sort();
		for (const FString& MapFile : MapFiles)
		{
			FString PackagePath;
			if (FPackageName::TryConvertFilenameToLongPackageName(MapFile, PackagePath))
			{
				OutMapPackage = PackagePath;
				return true;
			}
		}

		return false;
	}

	bool ContainsStringValue(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& ExpectedValue)
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
			if (Value.IsValid() && Value->TryGetString(StringValue) && StringValue == ExpectedValue)
			{
				return true;
			}
		}

		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMapRuntimeProof_RegistryToolsRegistered,
	"UnrealClaude.MapRuntimeProof.Registry.ToolsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMapRuntimeProof_RegistryToolsRegistered::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestTrue(TEXT("map_runtime_proof should be registered"), Registry.HasTool(TEXT("map_runtime_proof")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMapRuntimeProof_MissingMapProducesBoundedLimitation,
	"UnrealClaude.MapRuntimeProof.MissingMapProducesBoundedLimitation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMapRuntimeProof_MissingMapProducesBoundedLimitation::RunTest(const FString& Parameters)
{
	FMCPTool_MapRuntimeProof Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("map_path"), TEXT("/Game/Maps/DefinitelyMissing_MapRuntimeProof"));

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("missing map runtime proof should return structured success"), Result.bSuccess);
	TestTrue(TEXT("missing map runtime proof should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("proof_result should be runtime_probe_not_attempted"), Result.Data->GetStringField(TEXT("proof_result")), FString(TEXT("runtime_probe_not_attempted")));
	TestFalse(TEXT("runtime probe should not be attempted"), Result.Data->GetBoolField(TEXT("runtime_probe_attempted")));

	const TSharedPtr<FJsonObject>* PresenceObject = nullptr;
	TestTrue(TEXT("current_package_presence should exist"), Result.Data->TryGetObjectField(TEXT("current_package_presence"), PresenceObject) && PresenceObject && (*PresenceObject).IsValid());
	if (!PresenceObject || !(*PresenceObject).IsValid())
	{
		return false;
	}

	TestFalse(TEXT("missing map should not be present"), (*PresenceObject)->GetBoolField(TEXT("present")));
	TestEqual(TEXT("missing map next step should point at target/input review"), Result.Data->GetStringField(TEXT("recommended_next_step_class")), FString(TEXT("fix_target_map_input")));
	TestTrue(TEXT("limitations should mention non-attempt"), ContainsStringValue(Result.Data, TEXT("limitations"), TEXT("The runtime probe was intentionally not attempted because the target map is not currently present in a bounded current-state check.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMapRuntimeProof_HeadlessLoadExportReport,
	"UnrealClaude.MapRuntimeProof.HeadlessLoadExportReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMapRuntimeProof_HeadlessLoadExportReport::RunTest(const FString& Parameters)
{
	FMCPTool_MapRuntimeProof Tool;
	FMCPTool_ReportArtifactStatus StatusTool;
	FString TargetMapPackage;
	TestTrue(TEXT("current project should expose at least one loadable map package"), TryGetCurrentProjectMapPackage(TargetMapPackage));
	if (TargetMapPackage.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("map_path"), TargetMapPackage);
	Params->SetNumberField(TEXT("timeout_seconds"), 180.0);
	Params->SetBoolField(TEXT("export_report"), true);
	Params->SetStringField(TEXT("report_name"), TEXT("Map Runtime Proof Automation Probe"));
	Params->SetStringField(TEXT("report_slug"), TEXT("map_runtime_proof_automation_probe"));

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("headless map runtime proof should succeed"), Result.bSuccess);
	TestTrue(TEXT("headless map runtime proof should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("proof_result should be runtime_load_succeeded"), Result.Data->GetStringField(TEXT("proof_result")), FString(TEXT("runtime_load_succeeded")));
	TestTrue(TEXT("runtime probe should be attempted"), Result.Data->GetBoolField(TEXT("runtime_probe_attempted")));

	const TSharedPtr<FJsonObject>* RuntimeFacts = nullptr;
	TestTrue(TEXT("runtime facts should exist"), Result.Data->TryGetObjectField(TEXT("runtime_facts"), RuntimeFacts) && RuntimeFacts && (*RuntimeFacts).IsValid());
	if (RuntimeFacts && (*RuntimeFacts).IsValid())
	{
		TestTrue(TEXT("actor count should be positive"), (*RuntimeFacts)->GetNumberField(TEXT("actor_count")) > 0.0);
		TestTrue(TEXT("player start count should be non-negative"), (*RuntimeFacts)->GetNumberField(TEXT("player_start_count")) >= 0.0);
	}

	const TSharedPtr<FJsonObject>* ArtifactObject = nullptr;
	TestTrue(TEXT("report_artifact should exist"), Result.Data->TryGetObjectField(TEXT("report_artifact"), ArtifactObject) && ArtifactObject && (*ArtifactObject).IsValid());
	if (!ArtifactObject || !(*ArtifactObject).IsValid())
	{
		return false;
	}

	FString ReportId;
	FString MarkdownPath;
	FString SummaryPath;
	(*ArtifactObject)->TryGetStringField(TEXT("report_id"), ReportId);
	(*ArtifactObject)->TryGetStringField(TEXT("markdown_path"), MarkdownPath);
	(*ArtifactObject)->TryGetStringField(TEXT("summary_path"), SummaryPath);

	TestFalse(TEXT("report_id should not be empty"), ReportId.IsEmpty());
	TestTrue(TEXT("markdown artifact should exist"), FPaths::FileExists(MarkdownPath));
	TestTrue(TEXT("summary artifact should exist"), FPaths::FileExists(SummaryPath));
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
		TestEqual(TEXT("run_kind should be map_runtime_proof"), ReportObject->GetStringField(TEXT("run_kind")), FString(TEXT("map_runtime_proof")));
		TestEqual(TEXT("execution_mode should stay read_only"), ReportObject->GetStringField(TEXT("execution_mode")), FString(TEXT("read_only")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
