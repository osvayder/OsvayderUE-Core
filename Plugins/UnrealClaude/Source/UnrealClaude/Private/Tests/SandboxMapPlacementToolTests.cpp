#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "MCP/MCPSavePipeline.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_MapRuntimeProof.h"
#include "MCP/Tools/MCPTool_ReportArtifactStatus.h"
#include "MCP/Tools/MCPTool_SandboxMapPlacement.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString Packet691UniqueSuffix()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	}

	FString Packet692UniqueSuffix()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	}

	FString MakePacket691MapPath(const FString& TestName)
	{
		return FString::Printf(
			TEXT("/Game/__UnrealClaudeTestSandbox/packet691_%s_%s/M_Packet691_%s"),
			*TestName,
			*Packet691UniqueSuffix(),
			*TestName);
	}

	FString MakePacket692MapPath(const FString& TestName)
	{
		return FString::Printf(
			TEXT("/Game/__UnrealClaudeTestSandbox/packet692_%s_%s/M_Packet692_%s"),
			*TestName,
			*Packet692UniqueSuffix(),
			*TestName);
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
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue) && StringValue == Expected)
			{
				return true;
			}
		}
		return false;
	}

	FMCPToolResult RunSandboxPlacement(FMCPToolRegistry& Registry, const FString& MapPath, const FString& ActorName, const bool bExportProof = true)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("map_path"), MapPath);
		Params->SetStringField(TEXT("actor_name"), ActorName);
		Params->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.StaticMeshActor"));
		Params->SetBoolField(TEXT("replace_existing"), true);
		Params->SetBoolField(TEXT("export_proof"), bExportProof);

		TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
		Location->SetNumberField(TEXT("x"), 100.0);
		Location->SetNumberField(TEXT("y"), 50.0);
		Location->SetNumberField(TEXT("z"), 150.0);
		Params->SetObjectField(TEXT("location"), Location);

		return Registry.ExecuteTool(TEXT("sandbox_map_placement"), Params);
	}

	bool DoesMapFileExist(const FString& MapPath)
	{
		FString Filename;
		return FPackageName::TryConvertLongPackageNameToFilename(MapPath, Filename, FPackageName::GetMapPackageExtension())
			&& FPaths::FileExists(Filename);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSandboxMapPlacement_RegistryToolRegistered,
	"UnrealClaude.SandboxMapPlacement.Registry.ToolRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSandboxMapPlacement_RegistryToolRegistered::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestTrue(TEXT("sandbox_map_placement should be registered"), Registry.HasTool(TEXT("sandbox_map_placement")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSandboxMapPlacement_RejectsNonSandboxMapPath,
	"UnrealClaude.SandboxMapPlacement.Guardrails.RejectsNonSandboxMapPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSandboxMapPlacement_RejectsNonSandboxMapPath::RunTest(const FString& Parameters)
{
	FMCPTool_SandboxMapPlacement Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("map_path"), TEXT("/Game/Variant_Combat/Lvl_Combat"));

	const FMCPToolResult Result = Tool.Execute(Params);
	TestFalse(TEXT("Production/non-sandbox map path must be rejected"), Result.bSuccess);
	TestTrue(TEXT("Structured denial data should exist"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("denial reason should be non_sandbox_map_path"),
			Result.Data->GetStringField(TEXT("denial_reason")),
			FString(TEXT("non_sandbox_map_path")));
		TestTrue(TEXT("production mutation should be prevented"),
			Result.Data->GetBoolField(TEXT("production_map_mutation_prevented")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSandboxMapPlacement_RejectsRawDiskAndTraversalPaths,
	"UnrealClaude.SandboxMapPlacement.Guardrails.RejectsRawDiskAndTraversalPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSandboxMapPlacement_RejectsRawDiskAndTraversalPaths::RunTest(const FString& Parameters)
{
	FMCPTool_SandboxMapPlacement Tool;

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("map_path"), TEXT("D:/Temp/Packet691_Bad.umap"));
		const FMCPToolResult Result = Tool.Execute(Params);
		TestFalse(TEXT("Raw disk map path must be rejected"), Result.bSuccess);
		TestTrue(TEXT("Raw disk denial data should exist"), Result.Data.IsValid());
		if (Result.Data.IsValid())
		{
			TestEqual(TEXT("raw disk denial reason"),
				Result.Data->GetStringField(TEXT("denial_reason")),
				FString(TEXT("raw_disk_path")));
		}
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("map_path"), TEXT("/Game/__UnrealClaudeTestSandbox/packet691_bad/../M_Bad"));
		const FMCPToolResult Result = Tool.Execute(Params);
		TestFalse(TEXT("Path traversal map path must be rejected"), Result.bSuccess);
		TestTrue(TEXT("Traversal denial data should exist"), Result.Data.IsValid());
		if (Result.Data.IsValid())
		{
			TestEqual(TEXT("traversal denial reason"),
				Result.Data->GetStringField(TEXT("denial_reason")),
				FString(TEXT("path_traversal")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSandboxMapPlacement_AcceptsCurrentPacketSandboxNamespace,
	"UnrealClaude.SandboxMapPlacement.Guardrails.AcceptsCurrentPacketSandboxNamespace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSandboxMapPlacement_AcceptsCurrentPacketSandboxNamespace::RunTest(const FString& Parameters)
{
	FString NormalizedPath;
	FString ValidationError;
	FString DenialReason;
	const FString Packet692MapPath = MakePacket692MapPath(TEXT("CurrentNamespace"));
	TestTrue(TEXT("packet692 sandbox namespace should validate"),
		FMCPTool_SandboxMapPlacement::NormalizeAndValidateSandboxMapPath(Packet692MapPath, NormalizedPath, ValidationError, DenialReason));
	TestEqual(TEXT("normalized packet692 path"), NormalizedPath, Packet692MapPath);

	FString PacketLabel;
	TestTrue(TEXT("packet label should be extracted"),
		FMCPTool_SandboxMapPlacement::TryExtractPacketLabelFromSandboxMapPath(Packet692MapPath, PacketLabel));
	TestEqual(TEXT("packet label should match packet692"), PacketLabel, FString(TEXT("packet692")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSandboxMapPlacement_PositiveCreateOpenPlaceReadbackSave,
	"UnrealClaude.SandboxMapPlacement.Positive.CreateOpenPlaceReadbackSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSandboxMapPlacement_PositiveCreateOpenPlaceReadbackSave::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	const FString MapPath = MakePacket691MapPath(TEXT("Positive"));
	const FString ActorName = FString::Printf(TEXT("Packet691_PlacedActor_%s"), *Packet691UniqueSuffix());
	const FString UnrelatedPackageName = TEXT("/Engine/Transient/Packet691_UnrelatedDirtyPackage");

	UPackage* UnrelatedDirtyPackage = CreatePackage(*UnrelatedPackageName);
	if (UnrelatedDirtyPackage)
	{
		UnrelatedDirtyPackage->SetDirtyFlag(true);
	}

	const FMCPToolResult Result = RunSandboxPlacement(Registry, MapPath, ActorName, true);

	TestTrue(TEXT("sandbox placement should succeed"), Result.bSuccess);
	TestTrue(TEXT("sandbox placement should return data"), Result.Data.IsValid());
	TestTrue(TEXT("sandbox map file should exist on disk"), DoesMapFileExist(MapPath));
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		if (UnrelatedDirtyPackage)
		{
			UnrelatedDirtyPackage->SetDirtyFlag(false);
		}
		return false;
	}

	TestEqual(TEXT("result_type"), Result.Data->GetStringField(TEXT("result_type")), FString(TEXT("sandbox_map_placement")));
	TestTrue(TEXT("actor readback should find placed actor"), Result.Data->GetBoolField(TEXT("readback_actor_found")));
	TestTrue(TEXT("proof artifact should be written"), Result.Data->GetBoolField(TEXT("proof_artifact_written")));

	FString ProofArtifactPath;
	Result.Data->TryGetStringField(TEXT("proof_artifact_path"), ProofArtifactPath);
	TestTrue(TEXT("proof artifact path should exist"), FPaths::FileExists(ProofArtifactPath));

	const TSharedPtr<FJsonObject>* Lifecycle = nullptr;
	TestTrue(TEXT("sandbox lifecycle object should exist"),
		Result.Data->TryGetObjectField(TEXT("sandbox_lifecycle"), Lifecycle) && Lifecycle && (*Lifecycle).IsValid());
	if (Lifecycle && (*Lifecycle).IsValid())
	{
		TestTrue(TEXT("target map should be saved"), (*Lifecycle)->GetBoolField(TEXT("target_map_saved")));
		TestTrue(TEXT("only sandbox packages should be saved"), (*Lifecycle)->GetBoolField(TEXT("only_sandbox_packages_saved")));
		TestFalse(TEXT("unrelated dirty packages should not be saved"), (*Lifecycle)->GetBoolField(TEXT("unrelated_dirty_packages_saved")));
		TestTrue(TEXT("saved packages should include target map"), JsonStringArrayContains(*Lifecycle, TEXT("saved_packages"), MapPath));
		TestTrue(TEXT("dirty_before_tool should include unrelated dirty package"), JsonStringArrayContains(*Lifecycle, TEXT("dirty_before_tool"), UnrelatedPackageName));
		TestFalse(TEXT("saved packages should exclude unrelated dirty package"), JsonStringArrayContains(*Lifecycle, TEXT("saved_packages"), UnrelatedPackageName));
	}

	if (UnrelatedDirtyPackage)
	{
		UnrelatedDirtyPackage->SetDirtyFlag(false);
	}

	TSharedRef<FJsonObject> ReadbackParams = MakeShared<FJsonObject>();
	ReadbackParams->SetStringField(TEXT("name_filter"), ActorName);
	ReadbackParams->SetBoolField(TEXT("include_hidden"), true);
	ReadbackParams->SetBoolField(TEXT("brief"), false);
	const FMCPToolResult ReadbackResult = Registry.ExecuteTool(TEXT("get_level_actors"), ReadbackParams);
	TestTrue(TEXT("get_level_actors readback should succeed"), ReadbackResult.bSuccess);
	TestTrue(TEXT("readback data should exist"), ReadbackResult.Data.IsValid());
	if (ReadbackResult.Data.IsValid())
	{
		TestTrue(TEXT("readback should find at least one matching actor"), ReadbackResult.Data->GetNumberField(TEXT("total")) >= 1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSandboxMapPlacement_SavePipelineExplicitLevelUsesUmap,
	"UnrealClaude.SandboxMapPlacement.Lifecycle.ExplicitLevelSaveUsesUmap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSandboxMapPlacement_SavePipelineExplicitLevelUsesUmap::RunTest(const FString& Parameters)
{
	const FString MapPath = MakePacket691MapPath(TEXT("SavePipeline"));
	UPackage* Package = CreatePackage(*MapPath);
	TestNotNull(TEXT("test level package should be created"), Package);
	if (!Package)
	{
		return false;
	}

	Package->ThisContainsMap();
	Package->SetDirtyFlag(true);

	FString CapturedFilename;
	UnrealClaude::SavePipeline::Testing::SetSavePackageOverride(
		[&CapturedFilename](UPackage* InPackage, const FString& Filename) -> bool
		{
			CapturedFilename = Filename;
			if (InPackage)
			{
				InPackage->SetDirtyFlag(false);
			}
			return true;
		});
	ON_SCOPE_EXIT
	{
		UnrealClaude::SavePipeline::Testing::ClearSavePackageOverride();
		if (Package)
		{
			Package->SetDirtyFlag(false);
		}
	};

	UnrealClaude::SavePipeline::FSaveSpec Spec;
	Spec.Packages.Add(Package);
	Spec.bIsExplicitToolCall = true;
	const UnrealClaude::SavePipeline::FLifecycleOutcome Outcome = UnrealClaude::SavePipeline::Run(Spec);

	TestEqual(TEXT("explicit level save should report one saved package"), Outcome.Saved.Num(), 1);
	TestTrue(TEXT("explicit level save should resolve .umap filename"), CapturedFilename.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("explicit level save should not resolve .uasset filename"), CapturedFilename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSandboxMapPlacement_RuntimeProofForPlacedSandboxMap,
	"UnrealClaude.SandboxMapPlacement.Practical.RuntimeProofForPlacedSandboxMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSandboxMapPlacement_RuntimeProofForPlacedSandboxMap::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	const FString MapPath = MakePacket691MapPath(TEXT("RuntimeProof"));
	const FString ActorName = FString::Printf(TEXT("Packet691_RuntimeActor_%s"), *Packet691UniqueSuffix());
	const FMCPToolResult PlacementResult = RunSandboxPlacement(Registry, MapPath, ActorName, true);
	TestTrue(TEXT("runtime proof placement setup should succeed"), PlacementResult.bSuccess);
	if (!PlacementResult.bSuccess)
	{
		return false;
	}

	FMCPTool_MapRuntimeProof RuntimeProofTool;
	TSharedRef<FJsonObject> RuntimeParams = MakeShared<FJsonObject>();
	RuntimeParams->SetStringField(TEXT("map_path"), MapPath);
	RuntimeParams->SetNumberField(TEXT("timeout_seconds"), 180.0);
	RuntimeParams->SetBoolField(TEXT("export_report"), true);
	RuntimeParams->SetStringField(TEXT("report_name"), TEXT("Packet691 Sandbox Map Runtime Proof"));
	RuntimeParams->SetStringField(TEXT("report_slug"), TEXT("packet691_sandbox_map_runtime_proof"));

	const FMCPToolResult RuntimeResult = RuntimeProofTool.Execute(RuntimeParams);
	TestTrue(TEXT("map_runtime_proof should succeed for placed sandbox map"), RuntimeResult.bSuccess);
	TestTrue(TEXT("runtime proof should return data"), RuntimeResult.Data.IsValid());
	if (!RuntimeResult.bSuccess || !RuntimeResult.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("runtime proof result should be load succeeded"),
		RuntimeResult.Data->GetStringField(TEXT("proof_result")),
		FString(TEXT("runtime_load_succeeded")));

	const TSharedPtr<FJsonObject>* ArtifactObject = nullptr;
	TestTrue(TEXT("runtime proof report artifact should exist"),
		RuntimeResult.Data->TryGetObjectField(TEXT("report_artifact"), ArtifactObject) && ArtifactObject && (*ArtifactObject).IsValid());
	if (ArtifactObject && (*ArtifactObject).IsValid())
	{
		FString MarkdownPath;
		FString SummaryPath;
		(*ArtifactObject)->TryGetStringField(TEXT("markdown_path"), MarkdownPath);
		(*ArtifactObject)->TryGetStringField(TEXT("summary_path"), SummaryPath);
		TestTrue(TEXT("runtime proof markdown should exist"), FPaths::FileExists(MarkdownPath));
		TestTrue(TEXT("runtime proof summary should exist"), FPaths::FileExists(SummaryPath));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
