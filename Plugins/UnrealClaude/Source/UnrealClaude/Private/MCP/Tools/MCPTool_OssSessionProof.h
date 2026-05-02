// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class FMCPTool_OssSessionProof : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("oss_session_proof");
		Info.Description = TEXT(
			"Produce a bounded OSS/session baseline proof artifact for the current Unreal context.\n\n"
			"This U4 slice is read-only and explicitly separates:\n"
			"- configured/default OSS baseline\n"
			"- observed current runtime/editor context\n"
			"- active-session presence or absence\n"
			"- not-yet-proven host/join/gameplay behavior\n\n"
			"Current probe mode in this slice:\n"
			"- current_context_snapshot: inspect current world/network/session/travel state and pair it with configured baseline facts.\n\n"
			"Truth boundary:\n"
			"- configured/default OSS choice is not active-session proof\n"
			"- session interface availability is not active-session proof\n"
			"- NULL/offline baseline is not treated as a bug unless stronger evidence exists\n"
			"- if runtime is not active, the tool says so explicitly"
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("Operation to run. Supported: probe_oss_session_baseline (default)."), false, TEXT("probe_oss_session_baseline")),
			FMCPToolParameter(TEXT("probe_mode"), TEXT("string"),
				TEXT("Proof mode. Supported: current_context_snapshot (default)."), false, TEXT("current_context_snapshot")),
			FMCPToolParameter(TEXT("export_report"), TEXT("boolean"),
				TEXT("If true, persist a Markdown OSS/session baseline proof report and normalized sidecar under Saved/UnrealClaude/Reports."), false, TEXT("false")),
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
