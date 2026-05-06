// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Niagara / VFX authoring surfaces.
 *
 * Query operations (read-only):
 *   - list_systems: Discover Niagara System assets
 *   - get_system_info: Introspect system (emitters, parameters, warmup, etc.)
 *   - get_system_parameters: List user-exposed parameters with types and values
 *   - list_emitters: Discover standalone Niagara Emitter assets
 *   - get_emitter_info: Introspect emitter (sim target, properties)
 *
 * Modify operations:
 *   - set_system_parameter: Set a user parameter value on a Niagara System
 */
class FMCPTool_Niagara : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	// Query ops
	FMCPToolResult ExecuteListSystems(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetSystemInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetSystemParameters(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteListEmitters(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetEmitterInfo(const TSharedRef<FJsonObject>& Params);

	// Modify ops
	FMCPToolResult ExecuteSetSystemParameter(const TSharedRef<FJsonObject>& Params);

	// Helpers
	UObject* LoadAssetByPath(const FString& AssetPath, FString& OutError);
};
