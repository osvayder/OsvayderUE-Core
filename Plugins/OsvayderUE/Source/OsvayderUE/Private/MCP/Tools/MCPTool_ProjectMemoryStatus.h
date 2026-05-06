// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Introspect project memory and bridge status
 * Returns curated docs found, handoff metadata, capability flags.
 */
class FMCPTool_ProjectMemoryStatus : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("project_memory_status");
		Info.Description = TEXT(
			"Introspect project memory and agent bridge status.\n"
			"Returns: memory path, bridge path, curated docs found, latest handoff metadata,\n"
			"execution journal presence, current state presence, capability summary.\n"
			"Use this to understand what the plugin currently knows and trusts."
		);
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
