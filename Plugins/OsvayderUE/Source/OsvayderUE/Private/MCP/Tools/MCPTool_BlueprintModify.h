// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Modify Blueprints (write operations)
 *
 * Level 2 Operations (Variables/Functions):
 *   - create: Create a new Blueprint
 *   - add_variable: Add a variable to a Blueprint
 *   - remove_variable: Remove a variable from a Blueprint
 *   - add_function: Add an empty function to a Blueprint
 *   - remove_function: Remove a function from a Blueprint
 *
 * Level 3 Operations (Nodes):
 *   - add_node: Add a single node to a graph
 *   - add_nodes: Batch add multiple nodes with connections
 *   - delete_node: Remove a node from a graph
 *
 * Level 4 Operations (Connections):
 *   - connect_pins: Connect two pins
 *   - disconnect_pins: Disconnect two pins
 *   - set_pin_value: Set default value for an input pin
 *
 * All modification operations auto-compile the Blueprint after changes.
 */
class FMCPTool_BlueprintModify : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("blueprint_modify");
		Info.Description = TEXT(
			"Create and modify Blueprints programmatically. Auto-compiles after changes.\n\n"
			"Complexity Levels:\n"
			"Level 2 (Structure): 'create', 'add_variable', 'remove_variable', 'add_function', 'remove_function'\n"
			"Level 3 (Nodes): 'add_node', 'add_nodes' (batch), 'delete_node'\n"
			"Level 4 (Wiring): 'connect_pins', 'disconnect_pins', 'set_pin_value'\n"
			"Repair: 'replace_node', 'validate_blueprint'\n"
			"Macros: 'create_macro', 'remove_macro', 'add_macro_instance'\n"
			"Composed: 'apply_blueprint_spec' (with dry_run preflight + idempotent re-apply)\n"
			"Defaults: 'set_class_default', 'set_class_defaults' (batch CDO with nested paths)\n"
			"Collections: 'modify_collection' (array append/insert/remove_at/set_at/clear, map put/remove/clear, set add/remove/clear)\n"
			"Bundles: 'configure_actor_class' (variables + components + defaults + anim in one call)\n"
			"Data: 'set_data_asset_properties', 'set_data_table_row', 'remove_data_table_row'\n"
			"Widgets: 'add_widget', 'remove_widget', 'set_widget_property'\n"
			"Animation: 'set_anim_blueprint' (assign AnimBP to SkeletalMeshComponent)\n\n"
			"Workflow: Use blueprint_query first to understand existing structure, then modify.\n"
			"Use is_macro_graph=true to target macro graphs for node operations.\n\n"
			"Node types: CallFunction, Branch, Event, VariableGet, VariableSet, Sequence, "
			"PrintString, Add, Subtract, Multiply, Divide, MacroInstance, "
			"CallDelegate, AddDelegate, RemoveDelegate, ClearDelegate\n\n"
			"Variable types: bool, int32, float, FString, FVector, FRotator, AActor*, UObject*, etc.\n\n"
			"Returns: Operation result with created node IDs (for subsequent connections)."
		);
		Info.Parameters = {
			// Operation selector
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("Operation to perform (see description for full list)"), true),

			// Common parameters
			FMCPToolParameter(TEXT("blueprint_path"), TEXT("string"),
				TEXT("Blueprint to modify"), false),

			// For 'create' operation
			FMCPToolParameter(TEXT("package_path"), TEXT("string"),
				TEXT("Package path for new Blueprint (e.g., '/Game/Blueprints')"), false),
			FMCPToolParameter(TEXT("blueprint_name"), TEXT("string"),
				TEXT("Name for new Blueprint"), false),
			FMCPToolParameter(TEXT("parent_class"), TEXT("string"),
				TEXT("Parent class (e.g., 'Actor', 'Pawn')"), false),
			FMCPToolParameter(TEXT("blueprint_type"), TEXT("string"),
				TEXT("Type: 'Normal', 'FunctionLibrary', 'Interface', 'MacroLibrary'"), false, TEXT("Normal")),

			// For variable operations
			FMCPToolParameter(TEXT("variable_name"), TEXT("string"),
				TEXT("Variable name"), false),
			FMCPToolParameter(TEXT("variable_type"), TEXT("string"),
				TEXT("Variable type: 'bool', 'int32', 'float', 'FString', 'FVector', 'AActor*', etc."), false),

			// For function operations
			FMCPToolParameter(TEXT("function_name"), TEXT("string"),
				TEXT("Function name"), false),

			// For node operations (Level 3)
			FMCPToolParameter(TEXT("graph_name"), TEXT("string"),
				TEXT("Graph name (empty for default EventGraph)"), false),
			FMCPToolParameter(TEXT("is_function_graph"), TEXT("boolean"),
				TEXT("True to target function graphs, false for event graphs"), false, TEXT("false")),
			FMCPToolParameter(TEXT("node_type"), TEXT("string"),
				TEXT("Node type: 'CallFunction', 'Branch', 'Event', 'CustomEvent', 'VariableGet', 'VariableSet', 'Sequence', 'PrintString', 'Add', 'Subtract', 'Multiply', 'Divide', 'MacroInstance', 'CallDelegate', 'AddDelegate', 'RemoveDelegate', 'ClearDelegate'"), false),
			FMCPToolParameter(TEXT("node_params"), TEXT("object"),
				TEXT("Node parameters: {function, target_class, event, variable, num_outputs}"), false),
			FMCPToolParameter(TEXT("pos_x"), TEXT("number"),
				TEXT("Node X position"), false, TEXT("0")),
			FMCPToolParameter(TEXT("pos_y"), TEXT("number"),
				TEXT("Node Y position"), false, TEXT("0")),
			FMCPToolParameter(TEXT("node_id"), TEXT("string"),
				TEXT("Node ID (for delete/connect operations)"), false),

			// For batch add_nodes operation
			FMCPToolParameter(TEXT("nodes"), TEXT("array"),
				TEXT("Array of node specs: [{type, params, pos_x, pos_y, pin_values}]"), false),
			FMCPToolParameter(TEXT("connections"), TEXT("array"),
				TEXT("Array of connections: [{from_node, from_pin, to_node, to_pin}] (use indices or node IDs)"), false),

			// For connection operations (Level 4)
			FMCPToolParameter(TEXT("source_node_id"), TEXT("string"),
				TEXT("Source node ID"), false),
			FMCPToolParameter(TEXT("source_pin"), TEXT("string"),
				TEXT("Source pin name (empty for auto exec)"), false),
			FMCPToolParameter(TEXT("target_node_id"), TEXT("string"),
				TEXT("Target node ID"), false),
			FMCPToolParameter(TEXT("target_pin"), TEXT("string"),
				TEXT("Target pin name (empty for auto exec)"), false),

			// For set_pin_value operation
			FMCPToolParameter(TEXT("pin_name"), TEXT("string"),
				TEXT("Pin name to set value"), false),
			FMCPToolParameter(TEXT("pin_value"), TEXT("string"),
				TEXT("Default value to set"), false),

			// For replace_node operation
			FMCPToolParameter(TEXT("new_node_type"), TEXT("string"),
				TEXT("New node type to replace with (for replace_node)"), false),
			FMCPToolParameter(TEXT("new_node_params"), TEXT("object"),
				TEXT("Parameters for the new replacement node (for replace_node)"), false),

			// For macro operations
			FMCPToolParameter(TEXT("macro_name"), TEXT("string"),
				TEXT("Macro graph name (for create_macro, remove_macro)"), false),
			FMCPToolParameter(TEXT("macro_source_blueprint"), TEXT("string"),
				TEXT("Source Blueprint path containing the macro (for add_macro_instance)"), false),
			FMCPToolParameter(TEXT("macro_source_name"), TEXT("string"),
				TEXT("Name of macro graph to instantiate (for add_macro_instance)"), false),
			FMCPToolParameter(TEXT("is_macro_graph"), TEXT("boolean"),
				TEXT("True to target macro graphs for node operations"), false, TEXT("false")),

			// For class defaults operations
			FMCPToolParameter(TEXT("property_name"), TEXT("string"),
				TEXT("Property name or nested path (for set_class_default, e.g. 'Health' or 'Offset.X')"), false),
			FMCPToolParameter(TEXT("property_value"), TEXT("string"),
				TEXT("Property value as string (for set_class_default)"), false),
			FMCPToolParameter(TEXT("properties"), TEXT("array"),
				TEXT("Array of {name, value} for set_class_defaults batch"), false),

			// For modify_collection operation
			FMCPToolParameter(TEXT("action"), TEXT("string"),
				TEXT("Collection action: TArray(append/insert/remove_at/set_at/clear), TMap(put/remove/clear), TSet(add/remove/clear)"), false),
			FMCPToolParameter(TEXT("index"), TEXT("number"),
				TEXT("Element index for array insert/remove_at/set_at"), false),
			FMCPToolParameter(TEXT("key"), TEXT("string"),
				TEXT("Map key for put/remove"), false),

			// For configure_actor_class bundle
			FMCPToolParameter(TEXT("components"), TEXT("array"),
				TEXT("Array of component specs: [{name, class, parent?, properties: [{name, value}]}]"), false),
			FMCPToolParameter(TEXT("defaults"), TEXT("array"),
				TEXT("Array of {name, value} for CDO defaults (nested paths supported)"), false),
			FMCPToolParameter(TEXT("anim_blueprint_path"), TEXT("string"),
				TEXT("AnimBP path to assign to SkeletalMeshComponent (for configure_actor_class/set_anim_blueprint)"), false),
			FMCPToolParameter(TEXT("anim_component_name"), TEXT("string"),
				TEXT("Target SkeletalMeshComponent name for AnimBP assignment"), false),

			// For data table operations
			FMCPToolParameter(TEXT("row_name"), TEXT("string"),
				TEXT("Row name (for set_data_table_row, remove_data_table_row)"), false),

			// For widget operations
			FMCPToolParameter(TEXT("widget_name"), TEXT("string"),
				TEXT("Widget name (for remove_widget, set_widget_property)"), false),
			FMCPToolParameter(TEXT("widget_class"), TEXT("string"),
				TEXT("Widget class (for add_widget, e.g. 'TextBlock', 'Button', 'Image')"), false),
			FMCPToolParameter(TEXT("parent_widget"), TEXT("string"),
				TEXT("Parent panel widget name (for add_widget)"), false),

			// For apply_blueprint_spec preflight
			FMCPToolParameter(TEXT("dry_run"), TEXT("boolean"),
				TEXT("If true, preview changes without mutating (preflight mode)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("macros"), TEXT("array"),
				TEXT("Array of macro specs: [{name}] for apply_blueprint_spec"), false)
		};
		// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
		Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
			TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	// Level 2 Operations
	FMCPToolResult ExecuteCreate(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddVariable(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveVariable(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddFunction(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveFunction(const TSharedRef<FJsonObject>& Params);

	// Level 3 Operations (Nodes)
	FMCPToolResult ExecuteAddNode(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddNodes(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteDeleteNode(const TSharedRef<FJsonObject>& Params);

	// Level 4 Operations (Connections)
	FMCPToolResult ExecuteConnectPins(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteDisconnectPins(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetPinValue(const TSharedRef<FJsonObject>& Params);

	// Validation
	FMCPToolResult ExecuteValidateBlueprint(const TSharedRef<FJsonObject>& Params);

	// Repair helpers
	FMCPToolResult ExecuteReplaceNode(const TSharedRef<FJsonObject>& Params);

	// Authoring helpers
	FMCPToolResult ExecuteSetFunctionSignature(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddInterface(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveInterface(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddDispatcher(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveDispatcher(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetDispatcherSignature(const TSharedRef<FJsonObject>& Params);

	// Class defaults / property operations
	FMCPToolResult ExecuteSetClassDefault(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetClassDefaults(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetComponentPropertiesBatch(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteModifyCollection(const TSharedRef<FJsonObject>& Params);

	// Composed bundles
	FMCPToolResult ExecuteConfigureActorClass(const TSharedRef<FJsonObject>& Params);

	// Animation integration
	FMCPToolResult ExecuteSetAnimBlueprint(const TSharedRef<FJsonObject>& Params);

	// Data surface operations
	FMCPToolResult ExecuteSetDataAssetProperties(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetDataTableRow(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveDataTableRow(const TSharedRef<FJsonObject>& Params);

	// Widget surface operations
	FMCPToolResult ExecuteAddWidget(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveWidget(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetWidgetProperty(const TSharedRef<FJsonObject>& Params);

	// Editor utility operations
	FMCPToolResult ExecuteRunEditorUtility(const TSharedRef<FJsonObject>& Params);

	// Delegate binding workflow
	FMCPToolResult ExecuteBindDispatcher(const TSharedRef<FJsonObject>& Params);

	// Macro graph operations
	FMCPToolResult ExecuteCreateMacro(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveMacro(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddMacroInstance(const TSharedRef<FJsonObject>& Params);

	// Composed authoring
	FMCPToolResult ExecuteApplyBlueprintSpec(const TSharedRef<FJsonObject>& Params);

	// Component Operations
	FMCPToolResult ExecuteAddComponent(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveComponent(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRenameComponent(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteReparentComponent(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetComponentProperty(const TSharedRef<FJsonObject>& Params);

	// Helpers
	EBlueprintType ParseBlueprintType(const FString& TypeString);

	// ExecuteAddNodes helper functions (reduces function complexity)
	bool CreateNodesFromSpec(
		UEdGraph* Graph,
		const TArray<TSharedPtr<FJsonValue>>& NodesArray,
		TArray<FString>& OutCreatedNodeIds,
		TArray<TSharedPtr<FJsonValue>>& OutCreatedNodes,
		FString& OutError,
		TArray<TSharedPtr<FJsonValue>>& OutNodeFailures,
		TArray<TSharedPtr<FJsonValue>>& OutPinValueFailures
	);

	void ProcessNodeConnections(
		UEdGraph* Graph,
		const TArray<TSharedPtr<FJsonValue>>& ConnectionsArray,
		const TArray<FString>& CreatedNodeIds,
		TArray<TSharedPtr<FJsonValue>>& OutConnectionFailures
	);
};
