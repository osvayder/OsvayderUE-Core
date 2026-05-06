// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Spawn an actor in the current level
 */
class FMCPTool_SpawnActor : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("spawn_actor");
		Info.Description = TEXT(
			"PREFERRED: Use this tool to add actors to the scene. Do not write Python scripts — call this tool directly.\n\n"
			"Spawn a new actor in the current level. "
			"Use this to add objects like lights, meshes, characters, or custom Blueprints to your scene.\n\n"
			"Common class paths:\n"
			"- 'PointLight' or '/Script/Engine.PointLight' - Point light source\n"
			"- 'SpotLight' - Spotlight source\n"
			"- 'DirectionalLight' - Directional/sun light\n"
			"- 'StaticMeshActor' - Static mesh placeholder\n"
			"- 'CameraActor' - Camera\n"
			"- 'PlayerStart' - Player spawn point\n"
			"- '/Game/Blueprints/BP_MyActor' - Custom Blueprint actors\n\n"
			"Returns: Actor name, class, label, and spawn location."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("class"), TEXT("string"), TEXT("The class path to spawn (e.g., '/Script/Engine.PointLight' or 'StaticMeshActor')"), true),
			FMCPToolParameter(TEXT("name"), TEXT("string"), TEXT("Optional name for the spawned actor"), false),
			FMCPToolParameter(TEXT("location"), TEXT("object"), TEXT("Spawn location {x, y, z}"), false, TEXT("{\"x\":0,\"y\":0,\"z\":0}")),
			FMCPToolParameter(TEXT("rotation"), TEXT("object"), TEXT("Spawn rotation {pitch, yaw, roll}"), false, TEXT("{\"pitch\":0,\"yaw\":0,\"roll\":0}")),
			FMCPToolParameter(TEXT("scale"), TEXT("object"), TEXT("Spawn scale {x, y, z}"), false, TEXT("{\"x\":1,\"y\":1,\"z\":1}")),
			FMCPToolParameter(TEXT("execution_profile"), TEXT("string"), TEXT("Optional governed runtime selection for representative policy checks: configured_default_runtime, read_only_diagnostic, bounded_plugin_mutation, or explicit_expert_opt_in"), false)
		};
		// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
		Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
			TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
