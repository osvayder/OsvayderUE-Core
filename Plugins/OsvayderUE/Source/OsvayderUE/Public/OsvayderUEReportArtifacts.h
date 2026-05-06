// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct OSVAYDERUE_API FOsvayderUEReportTruthSummary
{
	TArray<FString> PracticallyVerified;
	TArray<FString> Inspected;
	TArray<FString> CodeReviewed;
	TArray<FString> Inferred;
	TArray<FString> Limited;
	TArray<FString> NotVerified;

	TSharedPtr<FJsonObject> ToJson() const;
	int32 GetTotalClaimCount() const;
	bool HasAnyClaims() const;
};

struct OSVAYDERUE_API FOsvayderUEReportExportRequest
{
	FString ReportName;
	FString ReportSlug;
	FString ReportId;
	FString Markdown;
	FString SummaryText;
	FString RunKind;
	FString RunId;
	FString ExecutionMode = TEXT("unknown");
	TArray<FString> ToolNames;
	TArray<FString> ToolFamilies;
	TArray<FString> EvidenceClasses;
	FOsvayderUEReportTruthSummary TruthSummary;
	TSharedPtr<FJsonObject> ExtraMetadata;
};

struct OSVAYDERUE_API FOsvayderUEReportExportResult
{
	bool bSuccess = false;
	bool bRoundTripExact = false;
	FString ExportStatus;
	FString ErrorMessage;
	FString ReportId;
	FString MarkdownPath;
	FString SummaryPath;
	FString MarkdownEncoding;
	FString SummaryEncoding;
	TSharedPtr<FJsonObject> SummaryObject;
};

struct OSVAYDERUE_API FOsvayderUEReportQueryOptions
{
	FString ReportId;
	FString RunKind;
	FString NameFilter;
	int32 Count = 10;
	bool bLatestOnly = true;
	bool bIncludeMarkdownPreview = false;
	int32 PreviewChars = 600;
};

class OSVAYDERUE_API FOsvayderUEReportArtifacts
{
public:
	static FString GetReportsDirectory();
	static FString GetReportSummaryGlob();
	static bool ExportReport(const FOsvayderUEReportExportRequest& Request, FOsvayderUEReportExportResult& OutResult);
	static TArray<TSharedPtr<FJsonObject>> QueryReports(const FOsvayderUEReportQueryOptions& Options);

private:
	static FString MakeReportSlug(const FString& InValue);
	static FString MakeReportId(const FString& ReportName, const FString& ExplicitSlug);
	static FString MakeMarkdownPath(const FString& ReportId);
	static FString MakeSummaryPath(const FString& ReportId);
	static bool SaveUtf8MarkdownWithBom(const FString& Path, const FString& Markdown);
	static bool SaveUtf8JsonWithoutBom(const FString& Path, const TSharedPtr<FJsonObject>& JsonObject);
	static bool LoadUtf8Markdown(const FString& Path, FString& OutMarkdown);
	static bool LoadJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutJsonObject);
	static TSharedPtr<FJsonObject> BuildSummaryObject(const FOsvayderUEReportExportRequest& Request);
	static void NormalizeTruthSummary(FOsvayderUEReportTruthSummary& InOutTruthSummary);
	static void EnrichSummaryFromRunTrace(
		const FString& RunId,
		TArray<FString>& InOutToolNames,
		TArray<FString>& InOutToolFamilies,
		TArray<FString>& InOutEvidenceClasses,
		FString& InOutExecutionMode,
		TSharedPtr<FJsonObject>& OutObservedRunObject);
	static FString DeriveToolFamily(const FString& ToolName);
	static TArray<FString> DeriveEvidenceClasses(const TArray<FString>& ToolNames, const TArray<FString>& ToolFamilies);
	static FString NormalizeExecutionModeLabel(const FString& InExecutionMode);
	static TArray<FString> NormalizeUniqueStrings(const TArray<FString>& Values);
	static void SortSummaryObjectsNewestFirst(TArray<TSharedPtr<FJsonObject>>& Summaries);
};
