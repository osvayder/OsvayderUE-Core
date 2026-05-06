// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_ReportExport.h"

#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEReportArtifacts.h"

namespace
{
	TArray<FString> ExtractStringArrayParam(const TSharedRef<FJsonObject>& Params, const FString& FieldName)
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* JsonValues = nullptr;
		if (!Params->TryGetArrayField(FieldName, JsonValues) || !JsonValues)
		{
			return Values;
		}

		for (const TSharedPtr<FJsonValue>& JsonValue : *JsonValues)
		{
			FString StringValue;
			if (JsonValue.IsValid() && JsonValue->TryGetString(StringValue))
			{
				Values.Add(StringValue);
			}
		}
		return Values;
	}

	TArray<FString> ExtractTruthBucket(const TSharedPtr<FJsonObject>& TruthObject, const FString& FieldName)
	{
		TArray<FString> Values;
		if (!TruthObject.IsValid())
		{
			return Values;
		}

		const TArray<TSharedPtr<FJsonValue>>* JsonValues = nullptr;
		if (!TruthObject->TryGetArrayField(FieldName, JsonValues) || !JsonValues)
		{
			return Values;
		}

		for (const TSharedPtr<FJsonValue>& JsonValue : *JsonValues)
		{
			FString StringValue;
			if (JsonValue.IsValid() && JsonValue->TryGetString(StringValue))
			{
				Values.Add(StringValue);
			}
		}

		return Values;
	}
}

FMCPToolResult FMCPTool_ReportExport::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString ReportName;
	TOptional<FMCPToolResult> ErrorResult;
	if (!ExtractRequiredString(Params, TEXT("report_name"), ReportName, ErrorResult))
	{
		return ErrorResult.GetValue();
	}

	FString Markdown;
	if (!ExtractRequiredString(Params, TEXT("markdown"), Markdown, ErrorResult))
	{
		return ErrorResult.GetValue();
	}

	FOsvayderUEReportExportRequest Request;
	Request.ReportName = ReportName;
	Request.Markdown = Markdown;
	Request.SummaryText = ExtractOptionalString(Params, TEXT("summary_text"));
	Request.RunKind = ExtractOptionalString(Params, TEXT("run_kind"), TEXT("generic_report"));
	Request.RunId = ExtractOptionalString(Params, TEXT("run_id"));
	Request.ReportSlug = ExtractOptionalString(Params, TEXT("report_slug"));
	Request.ExecutionMode = ExtractOptionalString(Params, TEXT("execution_mode"), TEXT("unknown"));
	Request.ToolNames = ExtractStringArrayParam(Params, TEXT("tool_names"));
	Request.ToolFamilies = ExtractStringArrayParam(Params, TEXT("tool_families"));
	Request.EvidenceClasses = ExtractStringArrayParam(Params, TEXT("evidence_classes"));

	const TSharedPtr<FJsonObject>* TruthObject = nullptr;
	if (Params->TryGetObjectField(TEXT("truth_summary"), TruthObject) && TruthObject && (*TruthObject).IsValid())
	{
		Request.TruthSummary.PracticallyVerified = ExtractTruthBucket(*TruthObject, TEXT("practically_verified"));
		Request.TruthSummary.Inspected = ExtractTruthBucket(*TruthObject, TEXT("inspected"));
		Request.TruthSummary.CodeReviewed = ExtractTruthBucket(*TruthObject, TEXT("code_reviewed"));
		Request.TruthSummary.Inferred = ExtractTruthBucket(*TruthObject, TEXT("inferred"));
		Request.TruthSummary.Limited = ExtractTruthBucket(*TruthObject, TEXT("limited"));
		Request.TruthSummary.NotVerified = ExtractTruthBucket(*TruthObject, TEXT("not_verified"));
	}

	const TSharedPtr<FJsonObject>* ExtraMetadataObject = nullptr;
	if (Params->TryGetObjectField(TEXT("extra_metadata"), ExtraMetadataObject) && ExtraMetadataObject && (*ExtraMetadataObject).IsValid())
	{
		Request.ExtraMetadata = *ExtraMetadataObject;
	}

	FOsvayderUEReportExportResult ExportResult;
	if (!FOsvayderUEReportArtifacts::ExportReport(Request, ExportResult))
	{
		FExecutionReceipt FailReceipt;
		FailReceipt.Tool = TEXT("report_export");
		FailReceipt.bSuccess = false;
		FailReceipt.TargetType = TEXT("file");
		if (!ExportResult.MarkdownPath.IsEmpty())
		{
			FailReceipt.Targets.Add(ExportResult.MarkdownPath);
		}
		if (!ExportResult.SummaryPath.IsEmpty())
		{
			FailReceipt.Targets.Add(ExportResult.SummaryPath);
		}
		FailReceipt.Classification = TEXT("internal_state");
		FailReceipt.Status = ExportResult.ExportStatus.IsEmpty() ? TEXT("failed") : ExportResult.ExportStatus;
		FailReceipt.ErrorOrDenialReason = ExportResult.ErrorMessage;
		FOsvayderUEExecutionLog::Get().AddReceipt(FailReceipt);
		return FMCPToolResult::Error(ExportResult.ErrorMessage.IsEmpty() ? TEXT("Report export failed") : ExportResult.ErrorMessage);
	}

	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("report_export");
	Receipt.bSuccess = true;
	Receipt.TargetType = TEXT("file");
	Receipt.Targets = { ExportResult.MarkdownPath, ExportResult.SummaryPath };
	Receipt.Created = { ExportResult.MarkdownPath, ExportResult.SummaryPath };
	Receipt.Classification = TEXT("internal_state");
	Receipt.Status = ExportResult.ExportStatus;
	Receipt.Summary = FString::Printf(
		TEXT("Report export: %s (%s, roundtrip=%s)"),
		*ExportResult.ReportId,
		*ExportResult.ExportStatus,
		ExportResult.bRoundTripExact ? TEXT("exact") : TEXT("mismatch"));
	FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);

	TSharedPtr<FJsonObject> Data = ExportResult.SummaryObject.IsValid()
		? ExportResult.SummaryObject
		: MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("reports_dir"), FOsvayderUEReportArtifacts::GetReportsDirectory());
	Data->SetStringField(TEXT("report_id"), ExportResult.ReportId);
	Data->SetStringField(TEXT("markdown_path"), ExportResult.MarkdownPath);
	Data->SetStringField(TEXT("summary_path"), ExportResult.SummaryPath);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Report exported: %s"), *ExportResult.ReportId),
		Data);
}
