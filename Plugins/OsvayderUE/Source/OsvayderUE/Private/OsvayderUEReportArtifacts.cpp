// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUEReportArtifacts.h"

#include "Containers/StringConv.h"
#include "MCP/MCPToolRegistry.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "MCP/OsvayderUEMCPServer.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUEModule.h"
#include "OsvayderUEScopePolicy.h"

namespace
{
	constexpr uint8 Utf8BomBytes[] = { 0xEF, 0xBB, 0xBF };
	constexpr TCHAR ReportMarkdownEncoding[] = TEXT("utf-8-bom");
	constexpr TCHAR ReportJsonEncoding[] = TEXT("utf-8-no-bom");
	constexpr int32 MaxReportQueryCount = 50;
	constexpr int32 MaxPreviewChars = 4000;

	void SetStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	}

	int32 CountNonAsciiCharacters(const FString& Value)
	{
		int32 Count = 0;
		for (const TCHAR Character : Value)
		{
			if (Character > 127)
			{
				++Count;
			}
		}
		return Count;
	}

	int32 CountLines(const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return 0;
		}

		int32 LineCount = 1;
		for (const TCHAR Character : Value)
		{
			if (Character == TEXT('\n'))
			{
				++LineCount;
			}
		}
		return LineCount;
	}

	FString TruncateForPreview(const FString& Value, const int32 PreviewChars)
	{
		if (PreviewChars <= 0 || Value.Len() <= PreviewChars)
		{
			return Value;
		}

		return Value.Left(PreviewChars) + FString::Printf(TEXT("... [truncated %d chars]"), Value.Len() - PreviewChars);
	}

	bool TryParseIsoTimestamp(const TSharedPtr<FJsonObject>& SummaryObject, FDateTime& OutTimestamp)
	{
		if (!SummaryObject.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ExportObject = nullptr;
		if (!SummaryObject->TryGetObjectField(TEXT("export"), ExportObject) || !ExportObject || !(*ExportObject).IsValid())
		{
			return false;
		}

		FString TimestampText;
		if (!(*ExportObject)->TryGetStringField(TEXT("exported_at_utc"), TimestampText))
		{
			return false;
		}

		return FDateTime::ParseIso8601(*TimestampText, OutTimestamp);
	}

	FString SerializeJsonPretty(const TSharedPtr<FJsonObject>& JsonObject)
	{
		if (!JsonObject.IsValid())
		{
			return FString();
		}

		FString JsonText;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		Writer->Close();
		return JsonText;
	}

	void AddTraceEvidenceClass(TArray<FString>& InOutEvidenceClasses, const FString& EvidenceClass)
	{
		if (!EvidenceClass.IsEmpty())
		{
			InOutEvidenceClasses.Add(EvidenceClass);
		}
	}

	FString DetermineExecutionModeFromTools(const TArray<FString>& ToolNames)
	{
		if (ToolNames.Num() == 0)
		{
			return TEXT("unknown");
		}

		bool bSawReadOnly = false;
		bool bSawMutationCapable = false;

		TSharedPtr<FOsvayderUEMCPServer> MCPServer;
		if (FOsvayderUEModule::IsAvailable())
		{
			MCPServer = FOsvayderUEModule::Get().GetMCPServer();
		}

		for (const FString& ToolName : ToolNames)
		{
			bool bResolved = false;
			bool bReadOnly = false;

			if (MCPServer.IsValid() && MCPServer->GetToolRegistry().IsValid())
			{
				if (IMCPTool* Tool = MCPServer->GetToolRegistry()->FindTool(ToolName))
				{
					bReadOnly = Tool->GetInfo().Annotations.bReadOnlyHint;
					bResolved = true;
				}
			}

			if (!bResolved)
			{
				const FString Normalized = ToolName.TrimStartAndEnd().ToLower();
				const bool bLooksMutating =
					Normalized.Contains(TEXT("modify")) ||
					Normalized.Contains(TEXT("set_")) ||
					Normalized.Contains(TEXT("configure")) ||
					Normalized.Contains(TEXT("spawn")) ||
					Normalized.Contains(TEXT("delete")) ||
					Normalized.Contains(TEXT("move")) ||
					Normalized.Contains(TEXT("execute_script")) ||
					Normalized.Contains(TEXT("cleanup")) ||
					Normalized.Equals(TEXT("asset")) ||
					Normalized.Equals(TEXT("character")) ||
					Normalized.Equals(TEXT("character_data")) ||
					Normalized.Equals(TEXT("material")) ||
					Normalized.Equals(TEXT("open_level"));
				bReadOnly = !bLooksMutating;
			}

			if (bReadOnly)
			{
				bSawReadOnly = true;
			}
			else
			{
				bSawMutationCapable = true;
			}
		}

		if (bSawMutationCapable && bSawReadOnly)
		{
			return TEXT("mixed");
		}
		if (bSawMutationCapable)
		{
			return TEXT("mutation_capable");
		}
		if (bSawReadOnly)
		{
			return TEXT("read_only");
		}
		return TEXT("unknown");
	}
}

TSharedPtr<FJsonObject> FOsvayderUEReportTruthSummary::ToJson() const
{
	TSharedPtr<FJsonObject> TruthObject = MakeShared<FJsonObject>();
	SetStringArrayField(TruthObject, TEXT("practically_verified"), PracticallyVerified);
	SetStringArrayField(TruthObject, TEXT("inspected"), Inspected);
	SetStringArrayField(TruthObject, TEXT("code_reviewed"), CodeReviewed);
	SetStringArrayField(TruthObject, TEXT("inferred"), Inferred);
	SetStringArrayField(TruthObject, TEXT("limited"), Limited);
	SetStringArrayField(TruthObject, TEXT("not_verified"), NotVerified);

	TSharedPtr<FJsonObject> CountsObject = MakeShared<FJsonObject>();
	CountsObject->SetNumberField(TEXT("practically_verified"), PracticallyVerified.Num());
	CountsObject->SetNumberField(TEXT("inspected"), Inspected.Num());
	CountsObject->SetNumberField(TEXT("code_reviewed"), CodeReviewed.Num());
	CountsObject->SetNumberField(TEXT("inferred"), Inferred.Num());
	CountsObject->SetNumberField(TEXT("limited"), Limited.Num());
	CountsObject->SetNumberField(TEXT("not_verified"), NotVerified.Num());
	CountsObject->SetNumberField(TEXT("total_claims"), GetTotalClaimCount());
	TruthObject->SetObjectField(TEXT("counts"), CountsObject);

	return TruthObject;
}

int32 FOsvayderUEReportTruthSummary::GetTotalClaimCount() const
{
	return PracticallyVerified.Num()
		+ Inspected.Num()
		+ CodeReviewed.Num()
		+ Inferred.Num()
		+ Limited.Num()
		+ NotVerified.Num();
}

bool FOsvayderUEReportTruthSummary::HasAnyClaims() const
{
	return GetTotalClaimCount() > 0;
}

FString FOsvayderUEReportArtifacts::GetReportsDirectory()
{
	FString SavedDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	FPaths::NormalizeDirectoryName(SavedDir);
	const FString ReportsDir = FPaths::Combine(SavedDir, TEXT("OsvayderUE"), TEXT("Reports"));
	IFileManager::Get().MakeDirectory(*ReportsDir, true);
	return ReportsDir;
}

FString FOsvayderUEReportArtifacts::GetReportSummaryGlob()
{
	return FPaths::Combine(GetReportsDirectory(), TEXT("*.summary.json"));
}

bool FOsvayderUEReportArtifacts::ExportReport(const FOsvayderUEReportExportRequest& Request, FOsvayderUEReportExportResult& OutResult)
{
	OutResult = FOsvayderUEReportExportResult();
	OutResult.MarkdownEncoding = ReportMarkdownEncoding;
	OutResult.SummaryEncoding = ReportJsonEncoding;

	if (Request.ReportName.TrimStartAndEnd().IsEmpty())
	{
		OutResult.ErrorMessage = TEXT("report_name is required");
		OutResult.ExportStatus = TEXT("invalid_request");
		return false;
	}

	if (Request.Markdown.IsEmpty())
	{
		OutResult.ErrorMessage = TEXT("markdown is required");
		OutResult.ExportStatus = TEXT("invalid_request");
		return false;
	}

	FOsvayderUEReportExportRequest EffectiveRequest = Request;
	if (EffectiveRequest.ReportId.TrimStartAndEnd().IsEmpty())
	{
		EffectiveRequest.ReportId = MakeReportId(Request.ReportName, Request.ReportSlug);
	}

	EffectiveRequest.ToolNames = NormalizeUniqueStrings(Request.ToolNames);
	EffectiveRequest.ToolFamilies = NormalizeUniqueStrings(Request.ToolFamilies);
	EffectiveRequest.EvidenceClasses = NormalizeUniqueStrings(Request.EvidenceClasses);
	EffectiveRequest.ExecutionMode = NormalizeExecutionModeLabel(Request.ExecutionMode);
	NormalizeTruthSummary(EffectiveRequest.TruthSummary);

	TSharedPtr<FJsonObject> ObservedRunObject;
	EnrichSummaryFromRunTrace(
		EffectiveRequest.RunId,
		EffectiveRequest.ToolNames,
		EffectiveRequest.ToolFamilies,
		EffectiveRequest.EvidenceClasses,
		EffectiveRequest.ExecutionMode,
		ObservedRunObject);

	const FString MarkdownPath = MakeMarkdownPath(EffectiveRequest.ReportId);
	const FString SummaryPath = MakeSummaryPath(EffectiveRequest.ReportId);
	const FOsvayderUEScopePolicy::FScopeCheckResult MarkdownScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(MarkdownPath);
	const FOsvayderUEScopePolicy::FScopeCheckResult SummaryScopeCheck = FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(SummaryPath);
	if (!MarkdownScopeCheck.bAllowed || !SummaryScopeCheck.bAllowed)
	{
		OutResult.ErrorMessage = !MarkdownScopeCheck.bAllowed ? MarkdownScopeCheck.DenialReason : SummaryScopeCheck.DenialReason;
		OutResult.ExportStatus = TEXT("denied");
		return false;
	}

	TSharedPtr<FJsonObject> SummaryObject = BuildSummaryObject(EffectiveRequest);
	if (ObservedRunObject.IsValid())
	{
		SummaryObject->SetObjectField(TEXT("observed_run"), ObservedRunObject);
	}

	if (!SaveUtf8MarkdownWithBom(MarkdownPath, EffectiveRequest.Markdown))
	{
		OutResult.ErrorMessage = FString::Printf(TEXT("Failed to save markdown to %s"), *MarkdownPath);
		OutResult.ExportStatus = TEXT("markdown_write_failed");
		return false;
	}

	FString RoundTripMarkdown;
	const bool bRoundTripLoaded = LoadUtf8Markdown(MarkdownPath, RoundTripMarkdown);
	const bool bRoundTripExact = bRoundTripLoaded && RoundTripMarkdown.Equals(EffectiveRequest.Markdown, ESearchCase::CaseSensitive);

	TSharedPtr<FJsonObject> ExportObject = MakeShared<FJsonObject>();
	ExportObject->SetStringField(TEXT("status"), bRoundTripExact ? TEXT("ok") : TEXT("roundtrip_mismatch"));
	ExportObject->SetStringField(TEXT("exported_at_utc"), FDateTime::UtcNow().ToIso8601());
	ExportObject->SetStringField(TEXT("markdown_path"), MarkdownPath);
	ExportObject->SetStringField(TEXT("summary_path"), SummaryPath);
	ExportObject->SetStringField(TEXT("markdown_encoding"), ReportMarkdownEncoding);
	ExportObject->SetStringField(TEXT("summary_encoding"), ReportJsonEncoding);
	ExportObject->SetBoolField(TEXT("markdown_saved"), true);
	ExportObject->SetBoolField(TEXT("roundtrip_loaded"), bRoundTripLoaded);
	ExportObject->SetBoolField(TEXT("roundtrip_exact"), bRoundTripExact);
	ExportObject->SetNumberField(TEXT("markdown_char_count"), EffectiveRequest.Markdown.Len());
	ExportObject->SetNumberField(TEXT("markdown_line_count"), CountLines(EffectiveRequest.Markdown));
	ExportObject->SetNumberField(TEXT("non_ascii_char_count"), CountNonAsciiCharacters(EffectiveRequest.Markdown));
	FTCHARToUTF8 Utf8Markdown(*EffectiveRequest.Markdown);
	ExportObject->SetNumberField(TEXT("utf8_payload_bytes"), Utf8Markdown.Length());
	FMD5 MarkdownMd5;
	MarkdownMd5.Update(reinterpret_cast<const uint8*>(Utf8Markdown.Get()), Utf8Markdown.Length());
	uint8 MarkdownDigest[16];
	MarkdownMd5.Final(MarkdownDigest);
	ExportObject->SetStringField(TEXT("markdown_md5"), BytesToHex(MarkdownDigest, UE_ARRAY_COUNT(MarkdownDigest)));
	SummaryObject->SetObjectField(TEXT("export"), ExportObject);

	if (!SaveUtf8JsonWithoutBom(SummaryPath, SummaryObject))
	{
		OutResult.ErrorMessage = FString::Printf(TEXT("Failed to save summary to %s"), *SummaryPath);
		OutResult.ExportStatus = TEXT("summary_write_failed");
		return false;
	}

	ExportObject->SetBoolField(TEXT("summary_saved"), true);
	const FString FinalStatus = bRoundTripExact ? TEXT("ok") : TEXT("roundtrip_mismatch");
	ExportObject->SetStringField(TEXT("status"), FinalStatus);
	SaveUtf8JsonWithoutBom(SummaryPath, SummaryObject);

	OutResult.bSuccess = true;
	OutResult.bRoundTripExact = bRoundTripExact;
	OutResult.ExportStatus = FinalStatus;
	OutResult.ReportId = EffectiveRequest.ReportId;
	OutResult.MarkdownPath = MarkdownPath;
	OutResult.SummaryPath = SummaryPath;
	OutResult.SummaryObject = SummaryObject;
	return true;
}

TArray<TSharedPtr<FJsonObject>> FOsvayderUEReportArtifacts::QueryReports(const FOsvayderUEReportQueryOptions& Options)
{
	TArray<TSharedPtr<FJsonObject>> Summaries;
	TArray<FString> SummaryFiles;
	IFileManager::Get().FindFiles(SummaryFiles, *GetReportSummaryGlob(), true, false);

	for (const FString& SummaryFileName : SummaryFiles)
	{
		const FString SummaryPath = FPaths::Combine(GetReportsDirectory(), SummaryFileName);
		TSharedPtr<FJsonObject> SummaryObject;
		if (!LoadJsonObject(SummaryPath, SummaryObject) || !SummaryObject.IsValid())
		{
			continue;
		}

		if (!Options.ReportId.IsEmpty())
		{
			FString ReportId;
			if (!SummaryObject->TryGetStringField(TEXT("report_id"), ReportId) || ReportId != Options.ReportId)
			{
				continue;
			}
		}

		if (!Options.RunKind.IsEmpty())
		{
			FString RunKind;
			if (!SummaryObject->TryGetStringField(TEXT("run_kind"), RunKind) || RunKind != Options.RunKind)
			{
				continue;
			}
		}

		if (!Options.NameFilter.IsEmpty())
		{
			FString ReportName;
			SummaryObject->TryGetStringField(TEXT("report_name"), ReportName);
			if (!ReportName.Contains(Options.NameFilter))
			{
				continue;
			}
		}

		if (Options.bIncludeMarkdownPreview)
		{
			const TSharedPtr<FJsonObject>* ExportObject = nullptr;
			if (SummaryObject->TryGetObjectField(TEXT("export"), ExportObject) && ExportObject && (*ExportObject).IsValid())
			{
				FString MarkdownPath;
				if ((*ExportObject)->TryGetStringField(TEXT("markdown_path"), MarkdownPath))
				{
					FString MarkdownContent;
					if (LoadUtf8Markdown(MarkdownPath, MarkdownContent))
					{
						SummaryObject->SetStringField(TEXT("markdown_preview"), TruncateForPreview(MarkdownContent, FMath::Clamp(Options.PreviewChars, 80, MaxPreviewChars)));
					}
				}
			}
		}

		Summaries.Add(SummaryObject);
	}

	SortSummaryObjectsNewestFirst(Summaries);
	const int32 MaxCount = FMath::Clamp(Options.Count, 1, MaxReportQueryCount);
	if (Options.bLatestOnly && Summaries.Num() > 1)
	{
		Summaries.SetNum(1);
	}
	else if (Summaries.Num() > MaxCount)
	{
		Summaries.SetNum(MaxCount);
	}

	return Summaries;
}

FString FOsvayderUEReportArtifacts::MakeReportSlug(const FString& InValue)
{
	FString Slug = InValue.TrimStartAndEnd().ToLower();
	if (Slug.IsEmpty())
	{
		return TEXT("report");
	}

	for (TCHAR& Character : Slug)
	{
		if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-')))
		{
			Character = TEXT('_');
		}
	}

	while (Slug.Contains(TEXT("__")))
	{
		Slug.ReplaceInline(TEXT("__"), TEXT("_"));
	}

	return Slug.TrimChar(TEXT('_'));
}

FString FOsvayderUEReportArtifacts::MakeReportId(const FString& ReportName, const FString& ExplicitSlug)
{
	const FString Slug = MakeReportSlug(!ExplicitSlug.TrimStartAndEnd().IsEmpty() ? ExplicitSlug : ReportName);
	const FDateTime Now = FDateTime::UtcNow();
	return FString::Printf(
		TEXT("%04d%02d%02d_%02d%02d%02d_%s"),
		Now.GetYear(), Now.GetMonth(), Now.GetDay(),
		Now.GetHour(), Now.GetMinute(), Now.GetSecond(),
		*Slug);
}

FString FOsvayderUEReportArtifacts::MakeMarkdownPath(const FString& ReportId)
{
	return FPaths::Combine(GetReportsDirectory(), ReportId + TEXT(".md"));
}

FString FOsvayderUEReportArtifacts::MakeSummaryPath(const FString& ReportId)
{
	return FPaths::Combine(GetReportsDirectory(), ReportId + TEXT(".summary.json"));
}

bool FOsvayderUEReportArtifacts::SaveUtf8MarkdownWithBom(const FString& Path, const FString& Markdown)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	TArray<uint8> Bytes;
	Bytes.Append(Utf8BomBytes, UE_ARRAY_COUNT(Utf8BomBytes));
	FTCHARToUTF8 Utf8Markdown(*Markdown);
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8Markdown.Get()), Utf8Markdown.Length());
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

bool FOsvayderUEReportArtifacts::SaveUtf8JsonWithoutBom(const FString& Path, const TSharedPtr<FJsonObject>& JsonObject)
{
	return FFileHelper::SaveStringToFile(SerializeJsonPretty(JsonObject), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FOsvayderUEReportArtifacts::LoadUtf8Markdown(const FString& Path, FString& OutMarkdown)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() == 0)
	{
		return false;
	}

	int32 Offset = 0;
	if (Bytes.Num() >= 3 && Bytes[0] == Utf8BomBytes[0] && Bytes[1] == Utf8BomBytes[1] && Bytes[2] == Utf8BomBytes[2])
	{
		Offset = 3;
	}

	const int32 Utf8Length = Bytes.Num() - Offset;
	FUTF8ToTCHAR Utf8Converter(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData() + Offset), Utf8Length);
	OutMarkdown = FString(Utf8Converter.Length(), Utf8Converter.Get());
	return true;
}

bool FOsvayderUEReportArtifacts::LoadJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutJsonObject)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *Path))
	{
		return false;
	}

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	return FJsonSerializer::Deserialize(Reader, OutJsonObject) && OutJsonObject.IsValid();
}

TSharedPtr<FJsonObject> FOsvayderUEReportArtifacts::BuildSummaryObject(const FOsvayderUEReportExportRequest& Request)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("osvayderue_report_artifact"));
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(TEXT("report_id"), Request.ReportId);
	Root->SetStringField(TEXT("report_name"), Request.ReportName);
	Root->SetStringField(TEXT("report_slug"), MakeReportSlug(!Request.ReportSlug.IsEmpty() ? Request.ReportSlug : Request.ReportName));
	Root->SetStringField(TEXT("run_kind"), Request.RunKind.IsEmpty() ? TEXT("generic_report") : Request.RunKind);
	Root->SetStringField(TEXT("summary_text"), Request.SummaryText);
	Root->SetStringField(TEXT("run_id"), Request.RunId);
	Root->SetStringField(TEXT("execution_mode"), NormalizeExecutionModeLabel(Request.ExecutionMode));
	SetStringArrayField(Root, TEXT("tool_names"), Request.ToolNames);
	SetStringArrayField(Root, TEXT("tool_families"), Request.ToolFamilies);
	SetStringArrayField(Root, TEXT("evidence_classes"), Request.EvidenceClasses);
	Root->SetObjectField(TEXT("truth"), Request.TruthSummary.ToJson());
	if (Request.ExtraMetadata.IsValid())
	{
		Root->SetObjectField(TEXT("extra_metadata"), Request.ExtraMetadata);
	}
	return Root;
}

void FOsvayderUEReportArtifacts::NormalizeTruthSummary(FOsvayderUEReportTruthSummary& InOutTruthSummary)
{
	InOutTruthSummary.PracticallyVerified = NormalizeUniqueStrings(InOutTruthSummary.PracticallyVerified);
	InOutTruthSummary.Inspected = NormalizeUniqueStrings(InOutTruthSummary.Inspected);
	InOutTruthSummary.CodeReviewed = NormalizeUniqueStrings(InOutTruthSummary.CodeReviewed);
	InOutTruthSummary.Inferred = NormalizeUniqueStrings(InOutTruthSummary.Inferred);
	InOutTruthSummary.Limited = NormalizeUniqueStrings(InOutTruthSummary.Limited);
	InOutTruthSummary.NotVerified = NormalizeUniqueStrings(InOutTruthSummary.NotVerified);
}

void FOsvayderUEReportArtifacts::EnrichSummaryFromRunTrace(
	const FString& RunId,
	TArray<FString>& InOutToolNames,
	TArray<FString>& InOutToolFamilies,
	TArray<FString>& InOutEvidenceClasses,
	FString& InOutExecutionMode,
	TSharedPtr<FJsonObject>& OutObservedRunObject)
{
	if (RunId.IsEmpty())
	{
		InOutToolNames = NormalizeUniqueStrings(InOutToolNames);
		for (const FString& ToolName : InOutToolNames)
		{
			const FString ToolFamily = DeriveToolFamily(ToolName);
			if (!ToolFamily.IsEmpty())
			{
				InOutToolFamilies.Add(ToolFamily);
			}
		}
		InOutToolFamilies = NormalizeUniqueStrings(InOutToolFamilies);
		InOutEvidenceClasses.Append(DeriveEvidenceClasses(InOutToolNames, InOutToolFamilies));
		InOutEvidenceClasses = NormalizeUniqueStrings(InOutEvidenceClasses);
		if (NormalizeExecutionModeLabel(InOutExecutionMode) == TEXT("unknown"))
		{
			InOutExecutionMode = DetermineExecutionModeFromTools(InOutToolNames);
		}
		return;
	}

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.Count = 200;
	QueryOptions.bLatestOnly = false;
	QueryOptions.PreviewChars = 1200;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Events = FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);
	if (Events.Num() == 0)
	{
		InOutToolNames = NormalizeUniqueStrings(InOutToolNames);
		InOutToolFamilies = NormalizeUniqueStrings(InOutToolFamilies);
		InOutEvidenceClasses = NormalizeUniqueStrings(InOutEvidenceClasses);
		return;
	}

	AddTraceEvidenceClass(InOutEvidenceClasses, TEXT("observable_trace"));
	TArray<FString> EventTypes;
	FString ObservedBackend;
	FString PromptPreview;

	for (const TSharedPtr<FJsonObject>& Event : Events)
	{
		if (!Event.IsValid())
		{
			continue;
		}

		FString EventType;
		Event->TryGetStringField(TEXT("event_type"), EventType);
		if (!EventType.IsEmpty())
		{
			EventTypes.Add(EventType);
		}

		if (ObservedBackend.IsEmpty())
		{
			Event->TryGetStringField(TEXT("backend"), ObservedBackend);
		}

		const TSharedPtr<FJsonObject>* PayloadObject = nullptr;
		if (!Event->TryGetObjectField(TEXT("payload"), PayloadObject) || !PayloadObject || !(*PayloadObject).IsValid())
		{
			continue;
		}

		if (EventType == TEXT("tool_use"))
		{
			FString ToolName;
			if ((*PayloadObject)->TryGetStringField(TEXT("tool_name"), ToolName) && !ToolName.IsEmpty())
			{
				InOutToolNames.Add(ToolName);
				const FString ToolFamily = DeriveToolFamily(ToolName);
				if (!ToolFamily.IsEmpty())
				{
					InOutToolFamilies.Add(ToolFamily);
				}
			}
			AddTraceEvidenceClass(InOutEvidenceClasses, TEXT("tool_invocation"));
		}
		else if (EventType == TEXT("tool_result"))
		{
			AddTraceEvidenceClass(InOutEvidenceClasses, TEXT("tool_result"));
		}
		else if (EventType == TEXT("result") || EventType == TEXT("run_completed"))
		{
			AddTraceEvidenceClass(InOutEvidenceClasses, TEXT("provider_result"));
		}
		else if (EventType == TEXT("backend_snapshot"))
		{
			AddTraceEvidenceClass(InOutEvidenceClasses, TEXT("backend_snapshot"));
		}
		else if (EventType == TEXT("user_prompt_submitted") && PromptPreview.IsEmpty())
		{
			(*PayloadObject)->TryGetStringField(TEXT("prompt_text"), PromptPreview);
		}
	}

	InOutToolNames = NormalizeUniqueStrings(InOutToolNames);
	for (const FString& ToolName : InOutToolNames)
	{
		const FString ToolFamily = DeriveToolFamily(ToolName);
		if (!ToolFamily.IsEmpty())
		{
			InOutToolFamilies.Add(ToolFamily);
		}
	}
	InOutToolFamilies = NormalizeUniqueStrings(InOutToolFamilies);
	InOutEvidenceClasses.Append(DeriveEvidenceClasses(InOutToolNames, InOutToolFamilies));
	InOutEvidenceClasses = NormalizeUniqueStrings(InOutEvidenceClasses);
	InOutExecutionMode = NormalizeExecutionModeLabel(InOutExecutionMode) == TEXT("unknown")
		? DetermineExecutionModeFromTools(InOutToolNames)
		: NormalizeExecutionModeLabel(InOutExecutionMode);

	OutObservedRunObject = MakeShared<FJsonObject>();
	OutObservedRunObject->SetStringField(TEXT("resolved_run_id"), ResolvedRunId);
	OutObservedRunObject->SetStringField(TEXT("backend"), ObservedBackend);
	OutObservedRunObject->SetNumberField(TEXT("trace_events_loaded"), TotalLoaded);
	SetStringArrayField(OutObservedRunObject, TEXT("event_types"), NormalizeUniqueStrings(EventTypes));
	if (!PromptPreview.IsEmpty())
	{
		OutObservedRunObject->SetStringField(TEXT("prompt_preview"), TruncateForPreview(PromptPreview, 240));
	}
}

FString FOsvayderUEReportArtifacts::DeriveToolFamily(const FString& ToolName)
{
	const FString Normalized = ToolName.TrimStartAndEnd().ToLower();
	if (Normalized.IsEmpty())
	{
		return FString();
	}
	if (Normalized.StartsWith(TEXT("blueprint")) || Normalized == TEXT("anim_blueprint_modify"))
	{
		return TEXT("blueprint");
	}
	if (Normalized.StartsWith(TEXT("asset")))
	{
		return TEXT("asset");
	}
	if (Normalized == TEXT("plugin_settings") || Normalized == TEXT("project_memory_status"))
	{
		return TEXT("settings");
	}
	if (Normalized == TEXT("execution_log_status") || Normalized == TEXT("agent_trace_status"))
	{
		return TEXT("observability");
	}
	if (Normalized.StartsWith(TEXT("task_")))
	{
		return TEXT("task_queue");
	}
	if (Normalized == TEXT("get_output_log") || Normalized == TEXT("run_console_command"))
	{
		return TEXT("logs_console");
	}
	if (Normalized == TEXT("execute_script") || Normalized == TEXT("cleanup_scripts") || Normalized == TEXT("get_script_history"))
	{
		return TEXT("script_execution");
	}
	if (Normalized == TEXT("spawn_actor") || Normalized == TEXT("get_level_actors") || Normalized == TEXT("delete_actors") || Normalized == TEXT("move_actor") || Normalized == TEXT("set_property"))
	{
		return TEXT("level_actor");
	}
	if (Normalized == TEXT("open_level"))
	{
		return TEXT("level_management");
	}
	if (Normalized == TEXT("multiplayer"))
	{
		return TEXT("multiplayer");
	}
	if (Normalized == TEXT("gas"))
	{
		return TEXT("gas");
	}
	if (Normalized == TEXT("niagara"))
	{
		return TEXT("niagara");
	}
	if (Normalized == TEXT("ai"))
	{
		return TEXT("ai");
	}
	if (Normalized == TEXT("sequencer"))
	{
		return TEXT("sequencer");
	}
	if (Normalized == TEXT("character") || Normalized == TEXT("character_data"))
	{
		return TEXT("character");
	}
	if (Normalized == TEXT("material"))
	{
		return TEXT("material");
	}
	if (Normalized == TEXT("enhanced_input"))
	{
		return TEXT("enhanced_input");
	}
	if (Normalized == TEXT("capture_viewport") || Normalized.StartsWith(TEXT("osvayder_")))
	{
		return TEXT("screen_control");
	}
	return Normalized;
}

TArray<FString> FOsvayderUEReportArtifacts::DeriveEvidenceClasses(const TArray<FString>& ToolNames, const TArray<FString>& ToolFamilies)
{
	TArray<FString> Derived;

	auto AddDerived = [&Derived](const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Derived.Add(Value);
		}
	};

	for (const FString& ToolFamily : ToolFamilies)
	{
		const FString NormalizedFamily = ToolFamily.TrimStartAndEnd().ToLower();
		if (NormalizedFamily == TEXT("settings"))
		{
			AddDerived(TEXT("config_surface"));
		}
		else if (NormalizedFamily == TEXT("asset"))
		{
			AddDerived(TEXT("asset_metadata"));
		}
		else if (NormalizedFamily == TEXT("blueprint"))
		{
			AddDerived(TEXT("blueprint_metadata"));
		}
		else if (NormalizedFamily == TEXT("multiplayer"))
		{
			AddDerived(TEXT("replication_metadata"));
		}
		else if (NormalizedFamily == TEXT("logs_console"))
		{
			AddDerived(TEXT("log_observation"));
		}
		else if (NormalizedFamily == TEXT("level_actor") || NormalizedFamily == TEXT("level_management"))
		{
			AddDerived(TEXT("world_observation"));
		}
		else if (NormalizedFamily == TEXT("task_queue"))
		{
			AddDerived(TEXT("task_queue_state"));
		}
		else if (NormalizedFamily == TEXT("script_execution"))
		{
			AddDerived(TEXT("script_execution_artifact"));
		}
	}

	for (const FString& ToolName : ToolNames)
	{
		const FString NormalizedTool = ToolName.TrimStartAndEnd().ToLower();
		if (NormalizedTool == TEXT("project_memory_status"))
		{
			AddDerived(TEXT("memory_surface"));
		}
		else if (NormalizedTool == TEXT("agent_trace_status"))
		{
			AddDerived(TEXT("trace_readback"));
		}
		else if (NormalizedTool == TEXT("execution_log_status"))
		{
			AddDerived(TEXT("execution_receipt"));
		}
	}

	return NormalizeUniqueStrings(Derived);
}

FString FOsvayderUEReportArtifacts::NormalizeExecutionModeLabel(const FString& InExecutionMode)
{
	const FString Normalized = InExecutionMode.TrimStartAndEnd().ToLower();
	if (Normalized == TEXT("read_only") || Normalized == TEXT("readonly") || Normalized == TEXT("read-only"))
	{
		return TEXT("read_only");
	}
	if (Normalized == TEXT("mutation_capable") || Normalized == TEXT("mutation-capable") || Normalized == TEXT("mutating"))
	{
		return TEXT("mutation_capable");
	}
	if (Normalized == TEXT("mixed"))
	{
		return TEXT("mixed");
	}
	return TEXT("unknown");
}

TArray<FString> FOsvayderUEReportArtifacts::NormalizeUniqueStrings(const TArray<FString>& Values)
{
	TArray<FString> NormalizedValues;
	TSet<FString> Seen;
	for (const FString& Value : Values)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			continue;
		}
		if (!Seen.Contains(Trimmed))
		{
			Seen.Add(Trimmed);
			NormalizedValues.Add(Trimmed);
		}
	}
	return NormalizedValues;
}

void FOsvayderUEReportArtifacts::SortSummaryObjectsNewestFirst(TArray<TSharedPtr<FJsonObject>>& Summaries)
{
	Summaries.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		FDateTime TimestampA = FDateTime::MinValue();
		FDateTime TimestampB = FDateTime::MinValue();
		const bool bHasTimestampA = TryParseIsoTimestamp(A, TimestampA);
		const bool bHasTimestampB = TryParseIsoTimestamp(B, TimestampB);
		if (bHasTimestampA && bHasTimestampB)
		{
			return TimestampA > TimestampB;
		}
		if (bHasTimestampA != bHasTimestampB)
		{
			return bHasTimestampA;
		}

		FString ReportIdA;
		FString ReportIdB;
		if (A.IsValid())
		{
			A->TryGetStringField(TEXT("report_id"), ReportIdA);
		}
		if (B.IsValid())
		{
			B->TryGetStringField(TEXT("report_id"), ReportIdB);
		}
		return ReportIdA > ReportIdB;
	});
}
