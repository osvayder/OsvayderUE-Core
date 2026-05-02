// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class FMCPTool_ReportExport : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("report_export");
		Info.Description = TEXT(
			"Export a plugin-owned Markdown report to Saved/UnrealClaude/Reports with a normalized JSON sidecar.\n"
			"Use this for durable UTF-8-safe audit/discovery/runtime-proof exports.\n"
			"The sidecar summarizes the run, tool families, evidence classes, truthful verification buckets,\n"
			"execution mode, and export encoding/round-trip status."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("report_name"), TEXT("string"), TEXT("Human-readable report name."), true),
			FMCPToolParameter(TEXT("markdown"), TEXT("string"), TEXT("Full Markdown body to persist."), true),
			FMCPToolParameter(TEXT("summary_text"), TEXT("string"), TEXT("Short bounded summary of what the report/run was."), false),
			FMCPToolParameter(TEXT("run_kind"), TEXT("string"), TEXT("Generic category such as discovery, audit, runtime_proof, dependency_health."), false, TEXT("generic_report")),
			FMCPToolParameter(TEXT("run_id"), TEXT("string"), TEXT("Optional agent trace run_id to enrich tool/evidence metadata from observed trace."), false),
			FMCPToolParameter(TEXT("report_slug"), TEXT("string"), TEXT("Optional custom slug for the saved artifact filenames."), false),
			FMCPToolParameter(TEXT("execution_mode"), TEXT("string"), TEXT("read_only, mutation_capable, mixed, or unknown."), false, TEXT("unknown")),
			FMCPToolParameter(TEXT("tool_names"), TEXT("array"), TEXT("Optional explicit MCP tool names used by the run."), false),
			FMCPToolParameter(TEXT("tool_families"), TEXT("array"), TEXT("Optional explicit tool families; trace-derived families are merged in when run_id is provided."), false),
			FMCPToolParameter(TEXT("evidence_classes"), TEXT("array"), TEXT("Observable evidence classes such as config_surface, log_surface, runtime_probe, observable_trace."), false),
			FMCPToolParameter(TEXT("truth_summary"), TEXT("object"), TEXT("Truth buckets: practically_verified, inspected, code_reviewed, inferred, limited, not_verified."), false),
			FMCPToolParameter(TEXT("extra_metadata"), TEXT("object"), TEXT("Optional extra machine-readable metadata; do not include hidden reasoning."), false),
		};
		// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
		Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
			TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
