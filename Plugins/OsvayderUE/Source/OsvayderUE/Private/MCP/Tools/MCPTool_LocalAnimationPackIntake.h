// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * Bounded intake tool for explicit local animation content packs.
 *
 * The tool only enumerates the provided source root, preserves the native
 * package layout under /Game/<PackName>, copies missing .uasset/.umap files,
 * and reports the exact animation_preflight call required next.
 */
class FMCPTool_LocalAnimationPackIntake : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("local_animation_pack_intake");
		Info.Description = TEXT(
			"Bounded semantic intake for an explicit local Unreal animation pack path. "
			"Never searches above source_root and rejects drive roots, system roots, user-profile roots, "
			"and project roots. Dry-run reports the selected pack root, /Game mount, copy plan, conflicts, "
			"candidate animation paths, and the exact animation_preflight call. Execute copies missing "
			".uasset/.umap files only, preserves nested layout, never overwrites mismatches, and never deletes."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("source_root"), TEXT("string"),
				TEXT("Explicit local folder supplied by the user. The tool enumerates only this folder or a detected pack child inside it."), true),
			FMCPToolParameter(TEXT("destination_game_root"), TEXT("string"),
				TEXT("Optional /Game/<PackName> package root. Defaults to /Game/<detected pack folder name>."), false),
			FMCPToolParameter(TEXT("mode"), TEXT("string"),
				TEXT("'dry_run' (default) or 'execute'. Dry-run never copies files."), false, TEXT("dry_run")),
			FMCPToolParameter(TEXT("max_files"), TEXT("number"),
				TEXT("Bounded enumeration guard for all files under the selected pack root (default 5000, max 20000)."), false, TEXT("5000")),
			FMCPToolParameter(TEXT("max_total_size_mb"), TEXT("number"),
				TEXT("Bounded aggregate size guard for .uasset/.umap candidates (default 8192 MB, max 65536 MB)."), false, TEXT("8192"))
		};
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
