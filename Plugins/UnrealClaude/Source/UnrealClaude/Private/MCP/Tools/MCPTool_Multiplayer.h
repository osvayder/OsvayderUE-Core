// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Multiplayer authoring and introspection surfaces.
 *
 * Query operations (read-only):
 *   - get_replication_info: Inspect replicated properties, actor replication settings on a Blueprint
 *   - get_rpc_info: List RPC functions (Server/Client/Multicast, Reliable/Unreliable)
 *   - get_ownership_info: Inspect owner/authority semantics for a live actor in level
 *   - get_framework_roles: Inspect GameMode/GameState/PlayerState/PlayerController class hierarchy
 *   - get_multiplayer_config: Inspect world/session/travel-related settings
 *   - multiplayer_audit: Run diagnostic audit on a Blueprint for common replication issues (v2: component replication, RepNotify caveats, condition mismatches)
 *   - get_component_replication: Inspect SCS component replication status (name/class/is_replicated/is_scene_component)
 *   - get_replication_graph: Full replication summary (actor + vars + components + RPCs)
 *   - classify_multiplayer_actor: Semantic classification (authority/cosmetic/ownership guidance)
 *   - get_subobject_replication: Subobject replication inspection + C++ guidance (registration pattern, FastArray note)
 *   - audit_object_references: Scan BP vars for object refs, classify multiplayer safety (safe/requires_care/local_only)
 *   - get_travel_contracts: Per-class travel survival matrix + session contracts (errors on bad blueprint_path)
 *   - get_network_state: Live world net mode, PIE status, player topology (truthful editor/PIE context)
 *   - get_live_actor_network: Batch live actor network snapshot with filters (authority/role/owner)
 *   - audit_persistence_placement: Classify BP data placement vs framework role, warn on fragile patterns
 *   - get_session_state: Live session state introspection (OnlineSubsystem availability, session status)
 *   - get_travel_state: Current travel context (world type, net mode, seamless travel config, current level, runtime availability)
 *   - audit_live_replication: Compare configured replication intent vs live actor state (intent-vs-observed)
 *
 * Modify operations:
 *   - set_replication_config: Configure actor-level replication (bReplicates, NetUpdateFrequency, etc.)
 *   - set_property_replication: Set Replicated/RepNotify/ReplicationCondition on a BP variable
 *   - configure_multiplayer_actor: Composed bundle — actor config + variable replication + dry_run
 */
class FMCPTool_Multiplayer : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	// Query ops
	FMCPToolResult ExecuteGetReplicationInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetRpcInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetOwnershipInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetFrameworkRoles(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetMultiplayerConfig(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteMultiplayerAudit(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetComponentReplication(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetReplicationGraph(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteClassifyMultiplayerActor(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetSubobjectReplication(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAuditObjectReferences(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetTravelContracts(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetNetworkState(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetLiveActorNetwork(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAuditPersistencePlacement(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetSessionState(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetTravelState(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAuditLiveReplication(const TSharedRef<FJsonObject>& Params);

	// Modify ops
	FMCPToolResult ExecuteSetReplicationConfig(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetPropertyReplication(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteConfigureMultiplayerActor(const TSharedRef<FJsonObject>& Params);

	// Helpers
	UBlueprint* LoadBlueprintByPath(const FString& Path, FString& OutError);
	FString ReplicationConditionToString(uint8 Condition);
	FString RpcTypeToString(EFunctionFlags Flags);
	FString FrameworkRoleForClass(UClass* Class);
};
