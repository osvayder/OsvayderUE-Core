// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Animation skeleton compatibility and retarget/fixup preflight.
 *
 * This tool is intentionally narrow: it diagnoses target AnimBP skeleton vs
 * candidate AnimSequence skeletons after local pack intake, builds a missing-only
 * destination plan, and blocks instead of pretending a retarget succeeded when
 * the required IK Retargeter/tooling assets are not available.
 */
class FMCPTool_AnimationRetargetFixup : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("animation_retarget_fixup");
		Info.Description = TEXT(
			"Diagnose AnimSequence skeleton compatibility for a target AnimBP/skeleton and plan safe retargeted copies.\n\n"
			"Inputs:\n"
			"- target_anim_blueprint_path or target_skeleton_path\n"
			"- candidate_animation_paths and/or asset_root\n"
			"- destination_game_root for planned target-skeleton-compatible outputs\n"
			"- mode: dry_run or execute\n\n"
			"Behavior:\n"
			"- Never mutates source pack assets.\n"
			"- Never overwrites existing destination assets.\n"
			"- Compatible candidates are returned as already usable.\n"
			"- Skeleton mismatches return exact source skeleton paths and a destination plan.\n"
			"- Dry-run reports discovered IK Rig/IK Retargeter routes and whether they are executable.\n"
			"- Execute creates missing-only retargeted AnimSequence assets only when a verified safe route exists."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("target_anim_blueprint_path"), TEXT("string"),
				TEXT("Target Animation Blueprint path, e.g. /Game/Variant_Platforming/Anims/ABP_Manny_Platforming"), false),
			FMCPToolParameter(TEXT("target_skeleton_path"), TEXT("string"),
				TEXT("Target Skeleton asset path when no AnimBP is available"), false),
			FMCPToolParameter(TEXT("candidate_animation_paths"), TEXT("array"),
				TEXT("Optional explicit /Game AnimSequence package/object paths to classify"), false),
			FMCPToolParameter(TEXT("asset_root"), TEXT("string"),
				TEXT("Optional bounded /Game package root to discover AnimSequence candidates recursively"), false),
			FMCPToolParameter(TEXT("destination_game_root"), TEXT("string"),
				TEXT("Destination /Game package root for retargeted copies, missing-only/no-overwrite"), true),
			FMCPToolParameter(TEXT("retargeter_asset_path"), TEXT("string"),
				TEXT("Optional explicit IK Retargeter asset path to use before bounded route discovery"), false),
			FMCPToolParameter(TEXT("source_mesh_path"), TEXT("string"),
				TEXT("Optional explicit source SkeletalMesh for IK retarget execution"), false),
			FMCPToolParameter(TEXT("target_mesh_path"), TEXT("string"),
				TEXT("Optional explicit target SkeletalMesh for IK retarget execution"), false),
			FMCPToolParameter(TEXT("mode"), TEXT("string"),
				TEXT("dry_run or execute"), false, TEXT("dry_run")),
			FMCPToolParameter(TEXT("match_limit"), TEXT("number"),
				TEXT("Maximum candidates discovered from asset_root (default 200, max 1000)"), false, TEXT("200"))
		};
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
