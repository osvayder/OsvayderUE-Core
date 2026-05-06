// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_Multiplayer.h"
#include "MCP/Tools/MCPTool_ReportExport.h"
#include "MCP/Tools/MCPTool_ReportArtifactStatus.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	bool DoesAssetPackageExist(const FString& PackagePath)
	{
		FString Filename;
		return FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetAssetPackageExtension())
			&& FPaths::FileExists(Filename);
	}

	bool TryGetCurrentActorBlueprintObjectPath(FString& OutObjectPath)
	{
		const TArray<FString> PreferredBlueprintPackages = {
			TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"),
			TEXT("/Game/Variant_Combat/Blueprints/AI/BP_CombatEnemy"),
			TEXT("/Game/Variant_Combat/Blueprints/Interactables/BP_CombatDamageableBox"),
			TEXT("/Game/Characters/BP_Hero_Base"),
			TEXT("/Game/GamePlay/Lobby/BP_LobbyCharacter"),
			TEXT("/Game/Inventory/BP_Container")
		};
		for (const FString& PreferredBlueprintPackage : PreferredBlueprintPackages)
		{
			if (DoesAssetPackageExist(PreferredBlueprintPackage))
			{
				const FString AssetName = FPackageName::GetLongPackageAssetName(PreferredBlueprintPackage);
				OutObjectPath = FString::Printf(TEXT("%s.%s"), *PreferredBlueprintPackage, *AssetName);
				return true;
			}
		}

		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReportArtifactExport_MultilingualRoundTrip,
	"OsvayderUE.ReportArtifacts.Export.MultilingualRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FReportArtifactExport_MultilingualRoundTrip::RunTest(const FString& Parameters)
{
	FMCPTool_ReportExport Tool;

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("report_name"), TEXT("ULTRA U1 UTF8 Probe"));
	ParamsObject->SetStringField(
		TEXT("markdown"),
		TEXT("# ULTRA U1 UTF-8 Probe\n\n")
		TEXT("\u041F\u0440\u0438\u0432\u0435\u0442, \u043C\u0438\u0440.\n")
		TEXT("UTF-8 round-trip should stay readable.\n")
		TEXT("\u65E5\u672C\u8A9E\u3068English\u3092\u6DF7\u305C\u3066\u3082\u58CA\u308C\u306A\u3044\u3053\u3068\u3092\u78BA\u8A8D\u3059\u308B.\n"));
	ParamsObject->SetStringField(TEXT("summary_text"), TEXT("Bounded multilingual export proof for the universal ULTRA U1 report path."));
	ParamsObject->SetStringField(TEXT("run_kind"), TEXT("audit"));
	ParamsObject->SetStringField(TEXT("execution_mode"), TEXT("read_only"));

	TArray<TSharedPtr<FJsonValue>> ToolNames;
	ToolNames.Add(MakeShared<FJsonValueString>(TEXT("plugin_settings")));
	ToolNames.Add(MakeShared<FJsonValueString>(TEXT("agent_trace_status")));
	ParamsObject->SetArrayField(TEXT("tool_names"), ToolNames);

	TArray<TSharedPtr<FJsonValue>> EvidenceClasses;
	EvidenceClasses.Add(MakeShared<FJsonValueString>(TEXT("config_surface")));
	EvidenceClasses.Add(MakeShared<FJsonValueString>(TEXT("observable_trace")));
	ParamsObject->SetArrayField(TEXT("evidence_classes"), EvidenceClasses);

	TSharedPtr<FJsonObject> TruthSummary = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PracticallyVerified;
	PracticallyVerified.Add(MakeShared<FJsonValueString>(TEXT("Plugin-owned multilingual Markdown export round-trips without mojibake.")));
	TruthSummary->SetArrayField(TEXT("practically_verified"), PracticallyVerified);

	TArray<TSharedPtr<FJsonValue>> Inspected;
	Inspected.Add(MakeShared<FJsonValueString>(TEXT("The sidecar captures report/run metadata without reopening the full Markdown.")));
	TruthSummary->SetArrayField(TEXT("inspected"), Inspected);

	TArray<TSharedPtr<FJsonValue>> Inferred;
	Inferred.Add(MakeShared<FJsonValueString>(TEXT("Future audit packs can reuse the same report artifact contract across projects.")));
	TruthSummary->SetArrayField(TEXT("inferred"), Inferred);

	TArray<TSharedPtr<FJsonValue>> NotVerified;
	NotVerified.Add(MakeShared<FJsonValueString>(TEXT("This automation test does not prove a live provider-backed discovery run.")));
	TruthSummary->SetArrayField(TEXT("not_verified"), NotVerified);
	ParamsObject->SetObjectField(TEXT("truth_summary"), TruthSummary);

	TSharedPtr<FJsonObject> ExtraMetadata = MakeShared<FJsonObject>();
	ExtraMetadata->SetStringField(TEXT("test_case"), TEXT("FReportArtifactExport_MultilingualRoundTrip"));
	ParamsObject->SetObjectField(TEXT("extra_metadata"), ExtraMetadata);

	const FMCPToolResult Result = Tool.Execute(ParamsObject);
	TestTrue(TEXT("report_export should succeed"), Result.bSuccess);
	TestTrue(TEXT("report_export should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	FString ReportId;
	FString MarkdownPath;
	FString SummaryPath;
	Result.Data->TryGetStringField(TEXT("report_id"), ReportId);
	Result.Data->TryGetStringField(TEXT("markdown_path"), MarkdownPath);
	Result.Data->TryGetStringField(TEXT("summary_path"), SummaryPath);

	TestFalse(TEXT("report_id should not be empty"), ReportId.IsEmpty());
	TestTrue(TEXT("markdown export should exist"), FPaths::FileExists(MarkdownPath));
	TestTrue(TEXT("summary export should exist"), FPaths::FileExists(SummaryPath));

	const TSharedPtr<FJsonObject>* ExportObject = nullptr;
	TestTrue(TEXT("summary should include export object"), Result.Data->TryGetObjectField(TEXT("export"), ExportObject) && ExportObject && (*ExportObject).IsValid());
	if (!ExportObject || !(*ExportObject).IsValid())
	{
		return false;
	}

	bool bRoundTripExact = false;
	(*ExportObject)->TryGetBoolField(TEXT("roundtrip_exact"), bRoundTripExact);
	TestTrue(TEXT("roundtrip_exact should be true"), bRoundTripExact);

	FString MarkdownEncoding;
	(*ExportObject)->TryGetStringField(TEXT("markdown_encoding"), MarkdownEncoding);
	TestEqual(TEXT("markdown should be saved with UTF-8 BOM"), MarkdownEncoding, FString(TEXT("utf-8-bom")));

	FString MarkdownContent;
	TestTrue(TEXT("markdown should load via FileHelper"), FFileHelper::LoadFileToString(MarkdownContent, *MarkdownPath));
	TestTrue(TEXT("markdown should preserve Cyrillic text"), MarkdownContent.Contains(TEXT("\u041F\u0440\u0438\u0432\u0435\u0442, \u043C\u0438\u0440.")));
	TestTrue(TEXT("markdown should preserve Japanese text"), MarkdownContent.Contains(TEXT("\u65E5\u672C\u8A9E\u3068English")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReportArtifactStatus_Readback,
	"OsvayderUE.ReportArtifacts.Status.Readback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FReportArtifactStatus_Readback::RunTest(const FString& Parameters)
{
	FMCPTool_ReportExport ExportTool;
	FMCPTool_ReportArtifactStatus Tool;

	TSharedRef<FJsonObject> ExportParams = MakeShared<FJsonObject>();
	ExportParams->SetStringField(TEXT("report_name"), TEXT("ULTRA U1 Status Probe"));
	ExportParams->SetStringField(
		TEXT("markdown"),
		TEXT("# ULTRA U1 Status Probe\n\n")
		TEXT("\u041F\u0440\u043E\u0432\u0435\u0440\u043A\u0430 bounded readback.\n")
		TEXT("Status tool should find this report by report_id.\n"));
	ExportParams->SetStringField(TEXT("summary_text"), TEXT("Readback probe for report_artifact_status."));
	ExportParams->SetStringField(TEXT("run_kind"), TEXT("status_probe"));
	ExportParams->SetStringField(TEXT("execution_mode"), TEXT("read_only"));

	const FMCPToolResult ExportResult = ExportTool.Execute(ExportParams);
	TestTrue(TEXT("status probe export should succeed"), ExportResult.bSuccess);
	TestTrue(TEXT("status probe export should return data"), ExportResult.Data.IsValid());
	if (!ExportResult.bSuccess || !ExportResult.Data.IsValid())
	{
		return false;
	}

	FString ReportId;
	ExportResult.Data->TryGetStringField(TEXT("report_id"), ReportId);
	TestFalse(TEXT("status probe report_id should not be empty"), ReportId.IsEmpty());
	if (ReportId.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("report_id"), ReportId);
	ParamsObject->SetBoolField(TEXT("latest_only"), false);
	ParamsObject->SetBoolField(TEXT("include_markdown_preview"), true);
	ParamsObject->SetNumberField(TEXT("preview_chars"), 240);

	const FMCPToolResult Result = Tool.Execute(ParamsObject);
	TestTrue(TEXT("report_artifact_status should succeed"), Result.bSuccess);
	TestTrue(TEXT("report_artifact_status should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Reports = nullptr;
	TestTrue(TEXT("reports array should exist"), Result.Data->TryGetArrayField(TEXT("reports"), Reports) && Reports);
	TestTrue(TEXT("should return exactly one matching report"), Reports && Reports->Num() == 1);
	if (!Reports || Reports->Num() != 1)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ReportObject = (*Reports)[0]->AsObject();
	TestTrue(TEXT("report object should be valid"), ReportObject.IsValid());
	if (!ReportObject.IsValid())
	{
		return false;
	}

	FString SummaryText;
	ReportObject->TryGetStringField(TEXT("summary_text"), SummaryText);
	TestFalse(TEXT("summary_text should not be empty"), SummaryText.IsEmpty());

	const TSharedPtr<FJsonObject>* TruthObject = nullptr;
	TestTrue(TEXT("truth object should exist"), ReportObject->TryGetObjectField(TEXT("truth"), TruthObject) && TruthObject && (*TruthObject).IsValid());

	FString Preview;
	ReportObject->TryGetStringField(TEXT("markdown_preview"), Preview);
	TestTrue(TEXT("markdown preview should contain UTF-8 text"), Preview.Contains(TEXT("\u041F\u0440\u043E\u0432\u0435\u0440\u043A\u0430")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReportArtifactIntegration_MultiplayerAuditExportReadback,
	"OsvayderUE.ReportArtifacts.Integration.MultiplayerAuditExportReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FReportArtifactIntegration_MultiplayerAuditExportReadback::RunTest(const FString& Parameters)
{
	FMCPTool_Multiplayer MultiplayerTool;
	FMCPTool_ReportArtifactStatus StatusTool;
	FString BlueprintObjectPath;
	TestTrue(TEXT("integration report artifact test should find a current actor blueprint"), TryGetCurrentActorBlueprintObjectPath(BlueprintObjectPath));
	if (BlueprintObjectPath.IsEmpty())
	{
		return false;
	}
	FString BlueprintPackagePath = BlueprintObjectPath;
	BlueprintPackagePath.Split(TEXT("."), &BlueprintPackagePath, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromStart);
	const FString BlueprintAssetName = FPackageName::GetLongPackageAssetName(BlueprintPackagePath);

	TSharedRef<FJsonObject> AuditParams = MakeShared<FJsonObject>();
	AuditParams->SetStringField(TEXT("operation"), TEXT("audit_persistence_placement"));
	AuditParams->SetStringField(TEXT("blueprint_path"), BlueprintObjectPath);
	AuditParams->SetBoolField(TEXT("export_report"), true);
	AuditParams->SetStringField(TEXT("report_name"), TEXT("ULTRA U1 Persistence Placement Integration Probe"));
	AuditParams->SetStringField(TEXT("report_slug"), TEXT("ultra_u1_persistence_placement_integration_probe"));

	const FMCPToolResult AuditResult = MultiplayerTool.Execute(AuditParams);
	TestTrue(TEXT("audit_persistence_placement should succeed"), AuditResult.bSuccess);
	TestTrue(TEXT("audit_persistence_placement should return data"), AuditResult.Data.IsValid());
	if (!AuditResult.bSuccess || !AuditResult.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ArtifactObject = nullptr;
	TestTrue(TEXT("audit result should include report_artifact"), AuditResult.Data->TryGetObjectField(TEXT("report_artifact"), ArtifactObject) && ArtifactObject && (*ArtifactObject).IsValid());
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

	TestFalse(TEXT("integration report_id should not be empty"), ReportId.IsEmpty());
	TestTrue(TEXT("integration markdown report should exist"), FPaths::FileExists(MarkdownPath));
	TestTrue(TEXT("integration summary report should exist"), FPaths::FileExists(SummaryPath));
	if (ReportId.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> StatusParams = MakeShared<FJsonObject>();
	StatusParams->SetStringField(TEXT("report_id"), ReportId);
	StatusParams->SetBoolField(TEXT("latest_only"), false);
	StatusParams->SetBoolField(TEXT("include_markdown_preview"), true);
	StatusParams->SetNumberField(TEXT("preview_chars"), 320);

	const FMCPToolResult StatusResult = StatusTool.Execute(StatusParams);
	TestTrue(TEXT("report_artifact_status readback should succeed"), StatusResult.bSuccess);
	TestTrue(TEXT("report_artifact_status readback should return data"), StatusResult.Data.IsValid());
	if (!StatusResult.bSuccess || !StatusResult.Data.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Reports = nullptr;
	TestTrue(TEXT("integration readback should return reports"), StatusResult.Data->TryGetArrayField(TEXT("reports"), Reports) && Reports);
	TestTrue(TEXT("integration readback should return one report"), Reports && Reports->Num() == 1);
	if (!Reports || Reports->Num() != 1)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ReportObject = (*Reports)[0]->AsObject();
	TestTrue(TEXT("integration report object should be valid"), ReportObject.IsValid());
	if (!ReportObject.IsValid())
	{
		return false;
	}

	FString RunKind;
	FString ExecutionMode;
	ReportObject->TryGetStringField(TEXT("run_kind"), RunKind);
	ReportObject->TryGetStringField(TEXT("execution_mode"), ExecutionMode);
	TestEqual(TEXT("integration run_kind should match the producer operation"), RunKind, FString(TEXT("audit_persistence_placement")));
	TestEqual(TEXT("integration execution_mode should stay truthful"), ExecutionMode, FString(TEXT("read_only")));

	const TArray<TSharedPtr<FJsonValue>>* ToolFamilies = nullptr;
	TestTrue(TEXT("integration report should include tool_families"), ReportObject->TryGetArrayField(TEXT("tool_families"), ToolFamilies) && ToolFamilies);
	if (!ToolFamilies)
	{
		return false;
	}

	bool bSawMultiplayerFamily = false;
	for (const TSharedPtr<FJsonValue>& ToolFamilyValue : *ToolFamilies)
	{
		FString ToolFamily;
		if (ToolFamilyValue.IsValid() && ToolFamilyValue->TryGetString(ToolFamily) && ToolFamily == TEXT("multiplayer"))
		{
			bSawMultiplayerFamily = true;
			break;
		}
	}
	TestTrue(TEXT("integration report should classify the multiplayer tool family"), bSawMultiplayerFamily);

	const TArray<TSharedPtr<FJsonValue>>* EvidenceClasses = nullptr;
	TestTrue(TEXT("integration report should include evidence_classes"), ReportObject->TryGetArrayField(TEXT("evidence_classes"), EvidenceClasses) && EvidenceClasses);
	if (!EvidenceClasses)
	{
		return false;
	}

	bool bSawReplicationMetadata = false;
	bool bSawPersistencePlacement = false;
	for (const TSharedPtr<FJsonValue>& EvidenceValue : *EvidenceClasses)
	{
		FString EvidenceClass;
		if (!EvidenceValue.IsValid() || !EvidenceValue->TryGetString(EvidenceClass))
		{
			continue;
		}

		bSawReplicationMetadata = bSawReplicationMetadata || EvidenceClass == TEXT("replication_metadata");
		bSawPersistencePlacement = bSawPersistencePlacement || EvidenceClass == TEXT("persistence_placement_analysis");
	}
	TestTrue(TEXT("integration report should auto-derive replication_metadata"), bSawReplicationMetadata);
	TestTrue(TEXT("integration report should include persistence_placement_analysis"), bSawPersistencePlacement);

	const TSharedPtr<FJsonObject>* TruthObject = nullptr;
	TestTrue(TEXT("integration report should include truth object"), ReportObject->TryGetObjectField(TEXT("truth"), TruthObject) && TruthObject && (*TruthObject).IsValid());
	if (!TruthObject || !(*TruthObject).IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Limited = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* NotVerified = nullptr;
	TestTrue(TEXT("integration report should include limited claims"), (*TruthObject)->TryGetArrayField(TEXT("limited"), Limited) && Limited && Limited->Num() > 0);
	TestTrue(TEXT("integration report should include not_verified claims"), (*TruthObject)->TryGetArrayField(TEXT("not_verified"), NotVerified) && NotVerified && NotVerified->Num() > 0);

	FString Preview;
	ReportObject->TryGetStringField(TEXT("markdown_preview"), Preview);
	TestTrue(TEXT("integration preview should mention the report title"), Preview.Contains(TEXT("Persistence Placement Audit")));
	TestTrue(TEXT("integration preview should mention the audited blueprint"), Preview.Contains(BlueprintAssetName));

	const TSharedPtr<FJsonObject>* ExtraMetadata = nullptr;
	TestTrue(TEXT("integration report should include extra_metadata"), ReportObject->TryGetObjectField(TEXT("extra_metadata"), ExtraMetadata) && ExtraMetadata && (*ExtraMetadata).IsValid());
	if (ExtraMetadata && (*ExtraMetadata).IsValid())
	{
		FString SourceOperation;
		(*ExtraMetadata)->TryGetStringField(TEXT("source_operation"), SourceOperation);
		TestEqual(TEXT("extra_metadata should preserve the source operation"), SourceOperation, FString(TEXT("audit_persistence_placement")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReportArtifactTools_Registered,
	"OsvayderUE.ReportArtifacts.Registry.ToolsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FReportArtifactTools_Registered::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestTrue(TEXT("report_export should be registered"), Registry.HasTool(TEXT("report_export")));
	TestTrue(TEXT("report_artifact_status should be registered"), Registry.HasTool(TEXT("report_artifact_status")));
	return true;
}

#endif
