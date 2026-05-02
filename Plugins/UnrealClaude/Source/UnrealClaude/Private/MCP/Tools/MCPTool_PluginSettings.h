// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Query current UnrealClaude plugin settings
 * Returns effective settings as structured JSON for agent introspection.
 */
class FMCPTool_PluginSettings : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("plugin_settings");
		Info.Description = TEXT(
			"Query current UnrealClaude plugin settings.\n"
			"Returns effective scope, verification, architecture, paths, and integration settings.\n"
			"Use this to understand current plugin configuration before making decisions."
		);
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
