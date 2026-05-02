// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class FMCPTool_ReportArtifactStatus : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("report_artifact_status");
		Info.Description = TEXT(
			"Read bounded report artifact summaries from Saved/UnrealClaude/Reports without reopening the full Markdown.\n"
			"Returns report identity, run kind, execution mode, tool families, evidence classes, truth buckets,\n"
			"and export encoding/round-trip status."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("report_id"), TEXT("string"), TEXT("Optional exact report_id to load."), false),
			FMCPToolParameter(TEXT("run_kind"), TEXT("string"), TEXT("Optional run_kind filter."), false),
			FMCPToolParameter(TEXT("name_filter"), TEXT("string"), TEXT("Optional substring filter for report_name."), false),
			FMCPToolParameter(TEXT("count"), TEXT("number"), TEXT("Number of reports to return when latest_only=false (default: 10, max: 50)."), false, TEXT("10")),
			FMCPToolParameter(TEXT("latest_only"), TEXT("boolean"), TEXT("If true, return only the newest matching report."), false, TEXT("true")),
			FMCPToolParameter(TEXT("include_markdown_preview"), TEXT("boolean"), TEXT("If true, include a bounded preview of the Markdown body."), false, TEXT("false")),
			FMCPToolParameter(TEXT("preview_chars"), TEXT("number"), TEXT("Preview size when include_markdown_preview=true (default: 600, max: 4000)."), false, TEXT("600")),
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
