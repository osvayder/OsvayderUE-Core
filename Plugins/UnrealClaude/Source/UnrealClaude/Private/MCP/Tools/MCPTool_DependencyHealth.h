// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class FMCPTool_DependencyHealth : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("dependency_health");
		Info.Description = TEXT(
			"Classify Unreal dependency-health findings from one or more log files and enrich them with current project state.\n\n"
			"This U2 slice is classification-first and read-heavy. It does not auto-repair dependencies.\n"
			"It focuses on truthful blocker/noise separation, bounded recommendation classes,\n"
			"and machine-readable recommendation detail plus current-state proof framing.\n\n"
			"Supported evidence in this slice:\n"
			"- missing script packages from AssetLog / VerifyImport warnings\n"
			"- missing mounted package/plugin warnings\n"
			"- OnlineSubsystem Steam initialization failures\n"
			"- missing class follow-up lines tied to logged asset imports\n\n"
			"Current-state enrichment uses:\n"
			"- current project packaging config (MapsToCook, DirectoriesToAlwaysCook)\n"
			"- current plugin/module state\n"
			"- asset-registry referencer lookups for logged asset packages\n"
			"- current logged-package presence checks for logged asset/referencer packages\n\n"
			"Optional report export routes the result through the accepted report artifact contract."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("Operation to run. Supported: classify_dependency_evidence (default)."), false, TEXT("classify_dependency_evidence")),
			FMCPToolParameter(TEXT("log_path"), TEXT("string"),
				TEXT("Optional single log path to analyze. Relative paths are resolved against the project root."), false),
			FMCPToolParameter(TEXT("log_paths"), TEXT("array"),
				TEXT("Optional array of log paths to analyze together. Relative paths are resolved against the project root."), false),
			FMCPToolParameter(TEXT("max_findings"), TEXT("number"),
				TEXT("Maximum dependency findings to return (1-100, default: 20)."), false, TEXT("20")),
			FMCPToolParameter(TEXT("include_log_excerpt"), TEXT("boolean"),
				TEXT("If true, include short representative log excerpts for each finding (default: true)."), false, TEXT("true")),
			FMCPToolParameter(TEXT("export_report"), TEXT("boolean"),
				TEXT("If true, persist a Markdown report and normalized sidecar under Saved/UnrealClaude/Reports."), false, TEXT("false")),
			FMCPToolParameter(TEXT("report_name"), TEXT("string"),
				TEXT("Optional custom report name when export_report=true."), false),
			FMCPToolParameter(TEXT("report_slug"), TEXT("string"),
				TEXT("Optional custom slug for saved report filenames when export_report=true."), false)
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
