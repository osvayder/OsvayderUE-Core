// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class FMCPTool_MapRuntimeProof : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("map_runtime_proof");
		Info.Description = TEXT(
			"Produce a bounded runtime-proof artifact for Unreal map viability.\n\n"
			"This U4 slice is read-only and explicitly separates:\n"
			"- current package presence\n"
			"- headless editor-load runtime proof\n"
			"- not-yet-proven gameplay/listen/session behavior\n\n"
			"Current runtime probe mode in this slice:\n"
			"- headless_editor_load: launch a separate UnrealEditor(-Cmd) process,\n"
			"  load the target map through Python editor scripting, and record bounded world facts.\n\n"
			"Truth boundary:\n"
			"- current package presence is not runtime proof\n"
			"- headless editor load is not gameplay/listen/session proof\n"
			"- world-partition ambiguity is surfaced, not hidden"
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("Operation to run. Supported: probe_map_runtime_viability (default)."), false, TEXT("probe_map_runtime_viability")),
			FMCPToolParameter(TEXT("map_path"), TEXT("string"),
				TEXT("Target map package or object path, for example /Game/Maps/MP_Map_03 or /Game/Maps/MP_Map_03.MP_Map_03."), true),
			FMCPToolParameter(TEXT("probe_mode"), TEXT("string"),
				TEXT("Runtime probe mode. Supported: headless_editor_load (default)."), false, TEXT("headless_editor_load")),
			FMCPToolParameter(TEXT("timeout_seconds"), TEXT("number"),
				TEXT("Timeout for the child headless probe process in seconds (10-600, default: 120)."), false, TEXT("120")),
			FMCPToolParameter(TEXT("export_report"), TEXT("boolean"),
				TEXT("If true, persist a Markdown runtime-proof report and normalized sidecar under Saved/OsvayderUE/Reports."), false, TEXT("false")),
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
