// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: AI / Behavior Tree / Blackboard / EQS authoring surfaces.
 *
 * Query operations (read-only):
 *   - list_behavior_trees: Discover BehaviorTree assets
 *   - get_behavior_tree_info: Introspect BT (root node, task/decorator/service counts, blackboard ref)
 *   - list_blackboards: Discover BlackboardData assets
 *   - get_blackboard_info: Introspect blackboard (keys with types)
 *   - list_eqs_queries: Discover EnvQuery assets
 *   - get_eqs_info: Introspect EQS query (generators, tests, options)
 *
 * Modify operations:
 *   - add_blackboard_key: Add a key to a BlackboardData asset
 *   - remove_blackboard_key: Remove a key from a BlackboardData asset
 *   - configure_ai_foundation: Composed — create/configure blackboard schema in one call
 */
class FMCPTool_AI : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	// Query ops
	FMCPToolResult ExecuteListBehaviorTrees(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetBehaviorTreeInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteListBlackboards(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetBlackboardInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteListEqsQueries(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetEqsInfo(const TSharedRef<FJsonObject>& Params);

	// Modify ops
	FMCPToolResult ExecuteAddBlackboardKey(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveBlackboardKey(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteConfigureAiFoundation(const TSharedRef<FJsonObject>& Params);

	// Helpers
	UObject* LoadAssetByPath(const FString& AssetPath, FString& OutError);
};
