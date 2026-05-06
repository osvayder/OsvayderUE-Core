// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Query Blueprint information (read-only operations)
 *
 * Operations:
 *   - list: List all Blueprints in project (with optional filters)
 *   - inspect: Get detailed Blueprint info (variables, functions, parent class)
 *   - get_graph: Get graph information (node count, events)
 */
class FMCPTool_BlueprintQuery : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("blueprint_query");
		Info.Description = TEXT(
			"Query Blueprint information (read-only).\n\n"
			"Operations:\n"
			"- 'list': Find Blueprints in project with optional filters\n"
			"- 'inspect': Get detailed Blueprint info (variables, functions, parent class)\n"
			"- 'get_graph': Get graph structure (node count, events, connections)\n"
			"- 'list_graphs': List all graphs in a Blueprint (event + function + macro graphs)\n"
			"- 'list_macros': List macro graphs distinctly with entry/exit info\n"
			"- 'get_graph_nodes': Get all nodes in a graph with stable IDs, titles, pins, and connections\n"
			"- 'find_nodes': Search for nodes by class or title filter\n"
			"- 'get_node_pins': Get detailed pin info for a specific node including connection targets\n"
			"- 'get_node': Get full node info by ID with all pins, connections, and position\n"
			"- 'get_node_connections': Get all inbound and outbound connections for a node\n"
			"- 'can_connect_pins': Check if two pins can be connected (dry-run, no mutation)\n"
			"- 'list_interfaces': List all implemented interfaces on a Blueprint\n"
			"- 'list_dispatchers': List event dispatchers with signature info\n"
			"- 'get_function_signature': Get function inputs/outputs with types\n"
			"- 'list_components': List all Blueprint components (SCS nodes) with hierarchy info\n"
			"- 'get_component_tree': Get component hierarchy as nested tree\n"
			"- 'get_component_details': Get detailed info for one component including properties\n"
			"- 'get_level_blueprint': Discover and inspect current level blueprint\n"
			"- 'list_editor_utilities': Find editor utility blueprints/widgets in project\n"
			"- 'get_editor_utility_details': Get editor utility type, parent, runnable status\n"
			"- 'get_class_defaults': Read Blueprint CDO properties with types, values, and source (local/inherited). Use expand_structs=true for struct children.\n"
			"- 'get_editable_properties': Schema of editable properties with type metadata, enum values, source, struct fields, and collection inner types\n"
			"- 'get_data_asset_properties': Inspect DataAsset properties with types and values\n"
			"- 'get_data_table_schema': Get DataTable row struct, columns, and row count\n"
			"- 'get_data_table_rows': Get DataTable rows with per-field values\n"
			"- 'get_widget_tree': Inspect WidgetBlueprint widget hierarchy\n"
			"- 'get_anim_blueprint_info': Get AnimBP parent class, skeleton, generated class, compile status\n\n"
			"- 'animation_preflight': Read-only gameplay animation readiness inventory across AnimBP state, skeleton compatibility, and required role assets\n\n"
			"Node IDs are stable: MCP-created nodes use MCP_ID, existing nodes use NodeGuid.\n"
			"Both ID types work in all read and write operations.\n"
			"Use is_macro_graph=true to target macro graphs. Use blueprint_path='level_blueprint' for level BP.\n\n"
			"Use 'list' to discover Blueprints, 'get_graph_nodes' to inspect graph contents,\n"
			"then 'find_nodes' to locate specific nodes by type or name."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("Operation: 'list', 'inspect', 'get_graph', 'list_graphs', 'list_macros', 'get_level_blueprint', 'list_editor_utilities', 'get_editor_utility_details', 'get_class_defaults', 'get_editable_properties', 'get_component_editable_properties', 'get_data_asset_properties', 'get_data_table_schema', 'get_data_table_rows', 'get_widget_tree', 'get_anim_blueprint_info', 'animation_preflight', 'get_graph_nodes', 'find_nodes', 'get_node_pins', 'get_node', 'get_node_connections', 'can_connect_pins', 'list_interfaces', 'list_dispatchers', 'get_function_signature', 'list_components', 'get_component_tree', 'get_component_details'"), true),
			FMCPToolParameter(TEXT("path_filter"), TEXT("string"),
				TEXT("Path prefix filter (e.g., '/Game/Blueprints/')"), false, TEXT("/Game/")),
			FMCPToolParameter(TEXT("type_filter"), TEXT("string"),
				TEXT("Blueprint type filter: 'Actor', 'Object', 'Widget', 'AnimBlueprint', etc."), false),
			FMCPToolParameter(TEXT("name_filter"), TEXT("string"),
				TEXT("Name substring filter"), false),
			FMCPToolParameter(TEXT("limit"), TEXT("number"),
				TEXT("Maximum results to return (1-1000, default: 25)"), false, TEXT("25")),
			FMCPToolParameter(TEXT("blueprint_path"), TEXT("string"),
				TEXT("Full Blueprint asset path (required for inspect/get_graph)"), false),
			FMCPToolParameter(TEXT("include_variables"), TEXT("boolean"),
				TEXT("Include variable list in inspect result (default: false)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("include_functions"), TEXT("boolean"),
				TEXT("Include function list in inspect result (default: false)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("include_graphs"), TEXT("boolean"),
				TEXT("Include graph info in inspect result"), false, TEXT("false")),
			FMCPToolParameter(TEXT("graph_name"), TEXT("string"),
				TEXT("Graph name (empty for default EventGraph). Used by get_graph_nodes, find_nodes, get_node_pins."), false),
			FMCPToolParameter(TEXT("is_function_graph"), TEXT("boolean"),
				TEXT("True to search function graphs instead of event graphs"), false, TEXT("false")),
			FMCPToolParameter(TEXT("is_macro_graph"), TEXT("boolean"),
				TEXT("True to search macro graphs (overrides is_function_graph)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("node_id"), TEXT("string"),
				TEXT("Node ID for get_node_pins (MCP_ID or guid_xxx)"), false),
			FMCPToolParameter(TEXT("class_filter"), TEXT("string"),
				TEXT("Filter nodes by class name substring (for find_nodes)"), false),
			FMCPToolParameter(TEXT("title_filter"), TEXT("string"),
				TEXT("Filter nodes by title substring (for find_nodes)"), false),
			FMCPToolParameter(TEXT("component_name"), TEXT("string"),
				TEXT("Component name (for get_component_details)"), false),
			FMCPToolParameter(TEXT("include_inherited"), TEXT("boolean"),
				TEXT("Include inherited properties (for get_editable_properties, get_class_defaults)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("expand_structs"), TEXT("boolean"),
				TEXT("Expand struct properties to show child fields with dot-path notation (for get_class_defaults)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("actor_blueprint_path"), TEXT("string"),
				TEXT("Optional actor blueprint that owns the target AnimBP; animation_preflight reports skeletal mesh assignment info when provided"), false),
			FMCPToolParameter(TEXT("required_animation_roles"), TEXT("array"),
				TEXT("Optional gameplay animation roles to inventory during animation_preflight, for example ['hover', 'fly', 'landing']"), false),
			FMCPToolParameter(TEXT("asset_root"), TEXT("string"),
				TEXT("Optional local /Game/... package root limiting animation_preflight inventory to already imported local assets only"), false),
			FMCPToolParameter(TEXT("imported_asset_root"), TEXT("string"),
				TEXT("Alias for asset_root during post-import animation-pack validation"), false),
			FMCPToolParameter(TEXT("pack_identifier"), TEXT("string"),
				TEXT("Optional label echoed in animation_preflight reports for the locally imported pack being validated"), false),
			FMCPToolParameter(TEXT("imported_pack_identifier"), TEXT("string"),
				TEXT("Alias for pack_identifier during post-import animation-pack validation"), false),
			FMCPToolParameter(TEXT("match_limit"), TEXT("number"),
				TEXT("Maximum local assets to return per role during animation_preflight (default: 5, max: 20)"), false, TEXT("5")),
			FMCPToolParameter(TEXT("source_node_id"), TEXT("string"),
				TEXT("Source node ID (for can_connect_pins)"), false),
			FMCPToolParameter(TEXT("source_pin_name"), TEXT("string"),
				TEXT("Source pin name (for can_connect_pins)"), false),
			FMCPToolParameter(TEXT("target_node_id"), TEXT("string"),
				TEXT("Target node ID (for can_connect_pins)"), false),
			FMCPToolParameter(TEXT("target_pin_name"), TEXT("string"),
				TEXT("Target pin name (for can_connect_pins)"), false)
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	/** List Blueprints matching filters */
	FMCPToolResult ExecuteList(const TSharedRef<FJsonObject>& Params);

	/** Get detailed Blueprint info */
	FMCPToolResult ExecuteInspect(const TSharedRef<FJsonObject>& Params);

	/** Get graph information */
	FMCPToolResult ExecuteGetGraph(const TSharedRef<FJsonObject>& Params);

	/** List all graphs in Blueprint */
	FMCPToolResult ExecuteListGraphs(const TSharedRef<FJsonObject>& Params);

	/** List macro graphs with entry/exit info */
	FMCPToolResult ExecuteListMacros(const TSharedRef<FJsonObject>& Params);

	/** Get current level blueprint info */
	FMCPToolResult ExecuteGetLevelBlueprint(const TSharedRef<FJsonObject>& Params);

	/** List editor utility assets */
	FMCPToolResult ExecuteListEditorUtilities(const TSharedRef<FJsonObject>& Params);

	/** Get editor utility details (type, parent, runnable) */
	FMCPToolResult ExecuteGetEditorUtilityDetails(const TSharedRef<FJsonObject>& Params);

	/** Get Blueprint class defaults (CDO properties) */
	FMCPToolResult ExecuteGetClassDefaults(const TSharedRef<FJsonObject>& Params);

	/** Get editable properties for a specific component template */
	FMCPToolResult ExecuteGetComponentEditableProperties(const TSharedRef<FJsonObject>& Params);

	// Data Surfaces
	FMCPToolResult ExecuteGetDataAssetProperties(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetDataTableSchema(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetDataTableRows(const TSharedRef<FJsonObject>& Params);

	// Widget Surfaces
	FMCPToolResult ExecuteGetWidgetTree(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetWidgetDetails(const TSharedRef<FJsonObject>& Params);

	// Animation Integration
	FMCPToolResult ExecuteGetAnimBlueprintInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAnimationPreflight(const TSharedRef<FJsonObject>& Params);

	/** Get editable properties schema for a Blueprint class */
	FMCPToolResult ExecuteGetEditableProperties(const TSharedRef<FJsonObject>& Params);

	/** Get all nodes in a graph with stable IDs */
	FMCPToolResult ExecuteGetGraphNodes(const TSharedRef<FJsonObject>& Params);

	/** Find nodes by class/title filter */
	FMCPToolResult ExecuteFindNodes(const TSharedRef<FJsonObject>& Params);

	/** Get detailed pin info for a specific node */
	FMCPToolResult ExecuteGetNodePins(const TSharedRef<FJsonObject>& Params);

	/** Get full node info by ID (alias for get_node_pins with richer context) */
	FMCPToolResult ExecuteGetNode(const TSharedRef<FJsonObject>& Params);

	/** Get all connections for a node (inbound + outbound) */
	FMCPToolResult ExecuteGetNodeConnections(const TSharedRef<FJsonObject>& Params);

	/** Check if two pins can be connected (dry-run) */
	FMCPToolResult ExecuteCanConnectPins(const TSharedRef<FJsonObject>& Params);

	/** List implemented interfaces */
	FMCPToolResult ExecuteListInterfaces(const TSharedRef<FJsonObject>& Params);

	/** List event dispatchers */
	FMCPToolResult ExecuteListDispatchers(const TSharedRef<FJsonObject>& Params);

	/** Get function signature (inputs/outputs) */
	FMCPToolResult ExecuteGetFunctionSignature(const TSharedRef<FJsonObject>& Params);

	/** List all Blueprint components */
	FMCPToolResult ExecuteListComponents(const TSharedRef<FJsonObject>& Params);

	/** Get component hierarchy tree */
	FMCPToolResult ExecuteGetComponentTree(const TSharedRef<FJsonObject>& Params);

	/** Get detailed info for one component */
	FMCPToolResult ExecuteGetComponentDetails(const TSharedRef<FJsonObject>& Params);
};
