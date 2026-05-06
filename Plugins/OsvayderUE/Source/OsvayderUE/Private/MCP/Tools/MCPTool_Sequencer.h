// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Sequencer / Cinematics authoring surfaces.
 *
 * Query operations (read-only):
 *   - list_sequences: Discover LevelSequence assets
 *   - get_sequence_info: Introspect sequence (possessables, spawnables, tracks, sections, subsequences, camera cuts)
 *
 * Modify operations:
 *   - set_sequence_playback: Set playback defaults (play rate, start/end, loop)
 *   - configure_sequence_foundation: Composed — set playback + metadata in one call with dry_run
 */
class FMCPTool_Sequencer : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteListSequences(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetSequenceInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetSequencePlayback(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteConfigureSequenceFoundation(const TSharedRef<FJsonObject>& Params);

	UObject* LoadAssetByPath(const FString& AssetPath, FString& OutError);
};
