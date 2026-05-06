// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * Read-only mechanic preflight tool.
 *
 * Inspects current project source/config/assets for map, class, input, movement,
 * camera, replication, and animation conflicts before agents mutate gameplay.
 */
class FMCPTool_MechanicPreflight : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("mechanic_preflight");
		Info.Description = TEXT(
			"Read-only mechanic implementation preflight. Inspects current map/class/input/movement/camera/replication/animation ownership before gameplay mutation.\n\n"
			"Operation:\n"
			"- 'inspect': Produce a deterministic conflict artifact for a requested mechanic and input contract.\n\n"
			"Key contract:\n"
			"- read_only=true, mutates_assets=false\n"
			"- does not open runtime maps or mutate content by default\n"
			"- may export a JSON diagnostic report under Saved/OsvayderUE/mechanic_preflight when export_report=true\n"
			"- may compose local animation_preflight semantics when requested_animation_roles are supplied"
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("Operation: 'inspect'"), true),
			FMCPToolParameter(TEXT("target_map"), TEXT("string"),
				TEXT("Target map package path, for example /Game/Variant_Combat/Lvl_Combat"), false),
			FMCPToolParameter(TEXT("mechanic_name"), TEXT("string"),
				TEXT("Requested mechanic name, for example wallrun, telekinesis, possession, camera"), false),
			FMCPToolParameter(TEXT("requested_inputs"), TEXT("array"),
				TEXT("Requested input keys/buttons, for example ['Space', 'LeftShift']"), false),
			FMCPToolParameter(TEXT("requested_animation_roles"), TEXT("array"),
				TEXT("Requested gameplay animation roles, for example ['wallrun_left', 'wallrun_right', 'wall_jump']"), false),
			FMCPToolParameter(TEXT("target_character_class"), TEXT("string"),
				TEXT("Optional target character class or Blueprint path"), false),
			FMCPToolParameter(TEXT("target_controller_class"), TEXT("string"),
				TEXT("Optional target controller class or Blueprint path"), false),
			FMCPToolParameter(TEXT("include_animation_preflight"), TEXT("boolean"),
				TEXT("Compose an animation_preflight summary when roles are provided (default true)"), false, TEXT("true")),
			FMCPToolParameter(TEXT("include_runtime_map_probe"), TEXT("boolean"),
				TEXT("Reserved runtime probe opt-in; default false and this tool remains read-only"), false, TEXT("false")),
			FMCPToolParameter(TEXT("export_report"), TEXT("boolean"),
				TEXT("Write a JSON diagnostic report under Saved/OsvayderUE/mechanic_preflight (default false)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("report_slug"), TEXT("string"),
				TEXT("Optional filename slug used when export_report=true"), false)
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
