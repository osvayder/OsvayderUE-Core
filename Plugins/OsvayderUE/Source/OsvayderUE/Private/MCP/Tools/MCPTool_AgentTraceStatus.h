// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Query observable backend run traces.
 *
 * Returns persisted JSONL-backed event trails such as run start metadata,
 * tool use/result events, stream text, backend errors, cancellations, and
 * run completion state. This is not hidden chain-of-thought.
 */
class FMCPTool_AgentTraceStatus : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("agent_trace_status");
		Info.Description = TEXT(
			"Query persisted observable backend run traces.\n"
			"Returns run metadata, tool calls, tool results, stream/runtime events,\n"
			"session actions, and completion/error trails. This is an observable execution trace,\n"
			"not hidden private reasoning or chain-of-thought."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("run_id"), TEXT("string"),
				TEXT("Optional explicit run id. If omitted and latest_only=true, the newest traced run is returned."), false),
			FMCPToolParameter(TEXT("count"), TEXT("number"),
				TEXT("Maximum number of events to return (default: 60, max: 200)."), false, TEXT("60")),
			FMCPToolParameter(TEXT("event_type"), TEXT("string"),
				TEXT("Optional exact event type filter: run_started, tool_use, tool_result, result, backend_error, timeout, cancellation, run_completed, etc."), false),
			FMCPToolParameter(TEXT("backend"), TEXT("string"),
				TEXT("Optional backend substring filter, e.g. CodexCli or ClaudeCli."), false),
			FMCPToolParameter(TEXT("latest_only"), TEXT("boolean"),
				TEXT("When true and run_id is omitted, resolve to the latest traced run before filtering events."), false, TEXT("true")),
			FMCPToolParameter(TEXT("preview_chars"), TEXT("number"),
				TEXT("Truncate long string fields in readback (default: 800, max: 12000). Persisted disk trace remains full JSONL."), false, TEXT("800")),
			FMCPToolParameter(TEXT("include_raw_json"), TEXT("boolean"),
				TEXT("Include raw provider event JSON fields in readback. Off by default to keep the UI bounded."), false, TEXT("false")),
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
