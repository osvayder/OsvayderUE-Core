// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_OssSessionProof.h"
#include "MCP/Tools/MCPTool_ReportArtifactStatus.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString GetExpectedConfiguredBaselineClassification()
	{
		FString DefaultPlatformService;
		if (GConfig && GConfig->GetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), DefaultPlatformService, GEngineIni))
		{
			DefaultPlatformService = DefaultPlatformService.TrimStartAndEnd();
			if (!DefaultPlatformService.IsEmpty())
			{
				return DefaultPlatformService.Equals(TEXT("NULL"), ESearchCase::IgnoreCase)
					? TEXT("configured_null_offline_baseline")
					: TEXT("configured_named_service_baseline");
			}
		}

		return TEXT("configured_baseline_unspecified");
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
	FOssSessionProof_RegistryToolsRegistered,
	"OsvayderUE.OssSessionProof.Registry.ToolsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FOssSessionProof_RegistryToolsRegistered::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestTrue(TEXT("oss_session_proof should be registered"), Registry.HasTool(TEXT("oss_session_proof")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOssSessionProof_RuntimeNotActiveProducesBoundedLimitation,
	"OsvayderUE.OssSessionProof.RuntimeNotActiveProducesBoundedLimitation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FOssSessionProof_RuntimeNotActiveProducesBoundedLimitation::RunTest(const FString& Parameters)
{
	FMCPTool_OssSessionProof Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("oss_session_proof should return structured success"), Result.bSuccess);
	TestTrue(TEXT("oss_session_proof should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("configured_baseline_classification should stay truthful to current config"), Result.Data->GetStringField(TEXT("configured_baseline_classification")), GetExpectedConfiguredBaselineClassification());
	TestEqual(TEXT("active_session_classification should reflect non-runtime editor context"), Result.Data->GetStringField(TEXT("active_session_classification")), FString(TEXT("runtime_not_active")));
	TestEqual(TEXT("proof_result should stay bounded"), Result.Data->GetStringField(TEXT("proof_result")), FString(TEXT("baseline_runtime_not_active")));
	TestTrue(TEXT("limitations should mention non-runtime context"), ContainsStringValue(Result.Data, TEXT("limitations"), TEXT("The current context is not runtime active, so active-session behavior is not proven in this slice.")));
	TestTrue(TEXT("limitations should mention config is not proof"), ContainsStringValue(Result.Data, TEXT("limitations"), TEXT("Configured/default OSS state is not active-session proof.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOssSessionProof_CurrentContextExportReport,
	"OsvayderUE.OssSessionProof.CurrentContextExportReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FOssSessionProof_CurrentContextExportReport::RunTest(const FString& Parameters)
{
	FMCPTool_OssSessionProof Tool;
	FMCPTool_ReportArtifactStatus StatusTool;

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetBoolField(TEXT("export_report"), true);
	Params->SetStringField(TEXT("report_name"), TEXT("OSS Session Proof Automation Probe"));
	Params->SetStringField(TEXT("report_slug"), TEXT("oss_session_proof_automation_probe"));

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("oss_session_proof should succeed"), Result.bSuccess);
	TestTrue(TEXT("oss_session_proof should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
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
		TestEqual(TEXT("run_kind should be oss_session_proof"), ReportObject->GetStringField(TEXT("run_kind")), FString(TEXT("oss_session_proof")));
		TestEqual(TEXT("execution_mode should stay read_only"), ReportObject->GetStringField(TEXT("execution_mode")), FString(TEXT("read_only")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
