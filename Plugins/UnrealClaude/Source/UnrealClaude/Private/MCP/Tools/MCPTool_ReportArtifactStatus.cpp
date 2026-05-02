// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_ReportArtifactStatus.h"

#include "UnrealClaudeReportArtifacts.h"

FMCPToolResult FMCPTool_ReportArtifactStatus::Execute(const TSharedRef<FJsonObject>& Params)
{
	FUnrealClaudeReportQueryOptions QueryOptions;
	QueryOptions.ReportId = ExtractOptionalString(Params, TEXT("report_id"));
	QueryOptions.RunKind = ExtractOptionalString(Params, TEXT("run_kind"));
	QueryOptions.NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	QueryOptions.Count = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("count"), 10), 1, 50);
	QueryOptions.bLatestOnly = ExtractOptionalBool(Params, TEXT("latest_only"), true);
	QueryOptions.bIncludeMarkdownPreview = ExtractOptionalBool(Params, TEXT("include_markdown_preview"), false);
	QueryOptions.PreviewChars = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("preview_chars"), 600), 80, 4000);

	const TArray<TSharedPtr<FJsonObject>> Reports = FUnrealClaudeReportArtifacts::QueryReports(QueryOptions);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("reports_dir"), FUnrealClaudeReportArtifacts::GetReportsDirectory());
	Data->SetStringField(TEXT("summary_glob"), FUnrealClaudeReportArtifacts::GetReportSummaryGlob());
	Data->SetStringField(TEXT("note"), TEXT("Bounded report artifact readback only. Use this to understand what a run/report proved without reopening the full Markdown."));
	Data->SetNumberField(TEXT("returned_reports"), Reports.Num());
	Data->SetBoolField(TEXT("latest_only"), QueryOptions.bLatestOnly);
	Data->SetBoolField(TEXT("include_markdown_preview"), QueryOptions.bIncludeMarkdownPreview);
	Data->SetNumberField(TEXT("preview_chars"), QueryOptions.PreviewChars);

	TArray<TSharedPtr<FJsonValue>> ReportValues;
	ReportValues.Reserve(Reports.Num());
	for (const TSharedPtr<FJsonObject>& Report : Reports)
	{
		ReportValues.Add(MakeShared<FJsonValueObject>(Report));
	}
	Data->SetArrayField(TEXT("reports"), ReportValues);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Report artifacts: %d report(s)"), Reports.Num()),
		Data);
}
