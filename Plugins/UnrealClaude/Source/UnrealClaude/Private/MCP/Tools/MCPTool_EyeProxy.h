// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolRegistry.h"

/**
 * Dynamic proxy tool for OsvayderEye HTTP sidecar.
 * One instance per Eye tool — created during discovery.
 * Execute() sends HTTP POST to Eye sidecar and returns result.
 */
class FMCPTool_EyeProxy : public IMCPTool
{
public:
	FMCPTool_EyeProxy(const FMCPToolInfo& InToolInfo, const FString& InBaseUrl);

	virtual FMCPToolInfo GetInfo() const override { return ToolInfo; }
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolInfo ToolInfo;
	FString BaseUrl;
};

/**
 * Discovers and registers OsvayderEye tools from HTTP sidecar.
 * Call once at startup after FMCPToolRegistry is ready.
 */
namespace EyeToolDiscovery
{
	/**
	 * Discover Eye tools from sidecar and register them in the registry.
	 * @param Registry - Tool registry to register into
	 * @param EyeServerUrl - Base URL of Eye sidecar (e.g., http://localhost:3001)
	 * @return Number of tools registered, or -1 on connection failure
	 */
	int32 DiscoverAndRegister(FMCPToolRegistry& Registry, const FString& EyeServerUrl);
}
