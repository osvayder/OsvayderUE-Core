// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Query recent execution receipts for the current session.
 */
class FMCPTool_ExecutionLogStatus : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("execution_log_status");
		Info.Description = TEXT(
			"Query recent mutation receipts for the current session.\n"
			"Returns: recent tool executions with success/failure, targets, created/modified/deleted items,\n"
			"warnings, and validation summaries.\n"
			"Use to review what was changed without guessing from memory."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("count"), TEXT("number"),
				TEXT("Number of recent entries to return (default: 20, max: 100)"), false, TEXT("20")),
			FMCPToolParameter(TEXT("tool_filter"), TEXT("string"),
				TEXT("Filter by tool name substring"), false),
			FMCPToolParameter(TEXT("denied_only"), TEXT("boolean"),
				TEXT("Show only denied write receipts"), false, TEXT("false")),
			FMCPToolParameter(TEXT("classification_filter"), TEXT("string"),
				TEXT("Filter by classification: user_mutation, internal_state, denied"), false),
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
