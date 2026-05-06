/**
 * Tool Router for UE5 MCP Bridge
 *
 * Classifies tools into three layers:
 * - Simple: pass through from Unreal unchanged (17 tools)
 * - Hidden: callable but never listed (9 tools)
 * - Mega: collapsed into unreal_ue router (7 tools)
 *
 * Token budget: 28 tools / ~30K tokens -> 16 tools / ~12K tokens
 */

// Simple tools: appear in list_tools with full schema
export const SIMPLE_TOOL_NAMES = new Set([
  "restart_survival",
  "spawn_actor",
  "move_actor",
  "delete_actors",
  "set_property",
  "get_level_actors",
  "open_level",
  "asset_search",
  "asset_dependencies",
  "asset_referencers",
  "capture_viewport",
  "get_output_log",
  "blueprint_query",
  "local_animation_pack_intake",
  "animation_retarget_fixup",
  "plugin_settings",
  "project_memory_status",
  "execution_log_status",
  "mechanic_preflight",
]);

// Hidden tools: callable but never listed
export const HIDDEN_TOOL_NAMES = new Set([
  "task_submit",
  "task_status",
  "task_result",
  "task_list",
  "task_cancel",
  "execute_script",
  "cleanup_scripts",
  "get_script_history",
  "run_console_command",
]);

// Domain -> underlying Unreal tool name
export const DOMAIN_TOOL_MAP = {
  blueprint: "blueprint_modify",
  anim: "anim_blueprint_modify",
  character: "character",
  enhanced_input: "enhanced_input",
  material: "material",
  asset: "asset",
  gas: "gas",
  niagara: "niagara",
  multiplayer: "multiplayer",
  ai: "ai",
  sequencer: "sequencer",
};

// Blueprint READ operations that route to "blueprint_query" instead of "blueprint_modify"
const BLUEPRINT_READ_OPS = new Set([
  "list",
  "inspect",
  "get_graph",
  "list_graphs",
  "list_macros",
  "get_level_blueprint",
  "list_editor_utilities",
  "get_editor_utility_details",
  "get_class_defaults",
  "get_editable_properties",
  "get_component_editable_properties",
  "get_data_asset_properties",
  "get_data_table_schema",
  "get_data_table_rows",
  "get_widget_tree",
  "get_anim_blueprint_info",
  "animation_preflight",
  "get_graph_nodes",
  "find_nodes",
  "get_node_pins",
  "get_node",
  "get_node_connections",
  "can_connect_pins",
  "list_interfaces",
  "list_dispatchers",
  "get_function_signature",
  "list_components",
  "get_component_tree",
  "get_component_details",
]);

// Character operations that route to "character_data" instead of "character"
const CHARACTER_DATA_OPS = new Set([
  "create_data_asset",
  "update_stats",
  "get_data_asset",
  "list_data_assets",
  "assign_data_asset",
]);

/**
 * Resolve a router call to the underlying Unreal tool name.
 * @param {string} domain - e.g. "blueprint", "anim", "character"
 * @param {string} operation - e.g. "add_variable", "create_state_machine"
 * @returns {string|null} Underlying tool name, or null if domain unknown
 */
export function resolveUnrealTool(domain, operation) {
  if (!domain) return null;
  // Blueprint: read ops → blueprint_query, write ops → blueprint_modify
  if (domain === "blueprint" && BLUEPRINT_READ_OPS.has(operation)) {
    return "blueprint_query";
  }
  if (domain === "character" && CHARACTER_DATA_OPS.has(operation)) {
    return "character_data";
  }
  return DOMAIN_TOOL_MAP[domain] ?? null;
}

/**
 * Classify a tool for list_tools filtering.
 * @param {string} toolName - raw Unreal tool name (no "unreal_" prefix)
 * @returns {"simple"|"hidden"|"mega"}
 */
export function classifyTool(toolName) {
  if (SIMPLE_TOOL_NAMES.has(toolName)) return "simple";
  if (HIDDEN_TOOL_NAMES.has(toolName)) return "hidden";
  // Eye tools from OsvayderEye sidecar are always simple (visible)
  if (toolName.startsWith("osvayder_")) return "simple";
  return "mega";
}

// Reverse map: tool name → domain (built from DOMAIN_TOOL_MAP)
const TOOL_TO_DOMAIN = Object.fromEntries(
  Object.entries(DOMAIN_TOOL_MAP).map(([domain, tool]) => [tool, domain])
);
TOOL_TO_DOMAIN["character_data"] = "character"; // sub-route

/**
 * Categorize a tool for the unreal_status health check.
 * Uses the router classification + domain map for accurate grouping.
 * @param {string} toolName - raw Unreal tool name (no "unreal_" prefix)
 * @returns {string} Category name for status display
 */
export function categorizeToolForStatus(toolName) {
  const cls = classifyTool(toolName);
  if (cls === "mega") return TOOL_TO_DOMAIN[toolName] || "utility";
  if (cls === "hidden") return toolName.startsWith("task_") ? "task_queue" : "scripting";
  // Simple tools
  if (toolName.startsWith("asset_")) return "asset";
  if (toolName === "blueprint_query") return "blueprint";
  if (toolName === "animation_retarget_fixup") return "animation";
  if (toolName === "open_level") return "level";
  if (toolName.includes("actor") || toolName === "spawn_actor" ||
      toolName === "move_actor" || toolName === "delete_actors" ||
      toolName === "set_property") return "actor";
  if (toolName === "mechanic_preflight") return "mechanic_preflight";
  return "utility"; // capture_viewport, get_output_log
}

/**
 * Static MCP schema for the unreal_ue router tool.
 */
export const ROUTER_TOOL_SCHEMA = {
  name: "unreal_ue",
  description: [
    "Route a command to a domain-specific Unreal Editor tool.",
    "",
    'domain:"blueprint" (requires params.blueprint_path)',
    "  ops: create, add_variable, remove_variable, add_function, remove_function,",
    "  add_node, add_nodes, delete_node, connect_pins, disconnect_pins, set_pin_value, validate_blueprint, replace_node,",
    "  create_macro, remove_macro, add_macro_instance, apply_blueprint_spec (with dry_run preflight),",
    "  CallDelegate/AddDelegate/RemoveDelegate/ClearDelegate node types via add_node,",
    "  set_class_default, set_class_defaults (CDO property write + batch, nested paths supported),",
    "  modify_collection (TArray append/insert/remove_at/set_at/clear, TMap put/remove/clear, TSet add/remove/clear),",
    "  configure_actor_class (composed: variables + components + defaults + anim in one call),",
    "  set_data_asset_properties, set_data_table_row, remove_data_table_row,",
    "  add_widget, remove_widget, set_widget_property, set_anim_blueprint",
    "  Per-op: package_path, blueprint_name, parent_class, variable_name, variable_type,",
    "  function_name, graph_name, node_type, node_params, source_node_id, target_node_id, etc.",
    "  Use blueprint_path='level_blueprint' to target the current level's blueprint.",
    "  Read ops (list, inspect, get_graph, list_graphs, list_macros, get_level_blueprint, list_editor_utilities, get_editor_utility_details, get_class_defaults, get_editable_properties, get_component_editable_properties, get_data_asset_properties, get_data_table_schema, get_data_table_rows, get_widget_tree, get_anim_blueprint_info, animation_preflight, get_graph_nodes, find_nodes, get_node_pins, get_node, get_node_connections, can_connect_pins, list_components, get_component_tree, get_component_details) route to blueprint_query.",
    "  Repair ops: replace_node (best-effort swap, not atomic), validate_blueprint (diagnostics with node_id + graph_name)",
    "  Component write ops: add_component, remove_component, rename_component, reparent_component, set_component_property",
    "",
    'domain:"anim" (requires params.blueprint_path)',
    "  ops: get_info, get_state_machine, create_state_machine, add_state, remove_state,",
    "  set_entry_state, add_transition, remove_transition, set_transition_duration,",
    "  set_transition_priority, add_condition_node, delete_condition_node,",
    "  connect_condition_nodes, connect_to_result, connect_state_machine_to_output,",
    "  set_state_animation, find_animations, batch, get_transition_nodes,",
    "  inspect_node_pins, set_pin_default_value, add_comparison_chain,",
    "  validate_blueprint, get_state_machine_diagram, setup_transition_conditions",
    "",
    'domain:"character" (key params: blueprint_path, character_name, or asset_name)',
    "  ops: create_character_bp, setup_enhanced_input,",
    "  get_character_config, assign_anim_bp, set_movement_param, get_movement_params,",
    "  create_data_asset, update_stats, get_data_asset, list_data_assets, assign_data_asset",
    "",
    'domain:"enhanced_input" (key params: action_name, context_name, package_path)',
    "  ops: create_action, create_context, add_mapping,",
    "  set_trigger, set_modifier, assign_to_character, list_actions, list_contexts,",
    "  get_action_info, remove_mapping",
    "",
    'domain:"material" (key params: material_path or actor_name)',
    "  ops: create_material_instance, set_material_parameters,",
    "  set_skeletal_mesh_material, set_actor_material, get_material_info",
    "",
    'domain:"asset" (key params: asset_path, or asset_name+package_path for create)',
    "  ops: create_blueprint, duplicate, rename, delete, move,",
    "  list_assets, get_asset_info, reimport",
    "",
    'domain:"gas" (key params: asset_path, class_name, ability_path)',
    "  query ops: list_abilities, get_ability_info, list_effects, get_effect_info, list_attribute_sets, get_attribute_set_info",
    "  modify ops: set_effect_properties (reflection-based), configure_gas_ability (composed: tags + cost/cooldown GE + dry_run)",
    "  multiplayer: classify_gas_multiplayer (authority/predicted/cosmetic classification), configure_gas_multiplayer_setup (composed: net policy + authoritative GE + cosmetic VFX)",
    "  NOTE: asset authoring only — no runtime ability granting",
    "",
    'domain:"niagara" (key params: asset_path, parameter_name)',
    "  query ops: list_systems, get_system_info, get_system_parameters, list_emitters, get_emitter_info",
    "  modify ops: set_system_parameter (float/int/bool/vector/color user params)",
    "  NOTE: parameter/system authoring only — no graph/module editing",
    "",
    'domain:"multiplayer" (key params: blueprint_path, actor_name, variable_name)',
    "  query ops: get_replication_info, get_rpc_info, get_ownership_info, get_framework_roles, get_multiplayer_config, multiplayer_audit",
    "  modify ops: set_replication_config, set_property_replication, configure_multiplayer_actor (composed + dry_run)",
    "  advanced: get_component_replication, get_replication_graph, classify_multiplayer_actor, get_subobject_replication, audit_object_references, get_travel_contracts, get_network_state (live runtime), get_live_actor_network (live actor snapshot), audit_persistence_placement, get_session_state (live session/OSS), get_travel_state (live travel context), audit_live_replication (intent-vs-observed)",
    "  NOTE: metadata authoring only — no runtime replication verification",
    "",
    'domain:"ai" (key params: asset_path, key_name, key_type)',
    "  query ops: list_behavior_trees, get_behavior_tree_info, list_blackboards, get_blackboard_info, list_eqs_queries, get_eqs_info",
    "  modify ops: add_blackboard_key, remove_blackboard_key, configure_ai_foundation (composed BB schema + dry_run)",
    "  NOTE: BT introspection only (no graph mutation). EQS introspection only. Blackboard key authoring supported.",
    "",
    'domain:"sequencer" (key params: asset_path)',
    "  query ops: list_sequences, get_sequence_info (possessables/spawnables/tracks/sections/camera cuts/subsequences)",
    "  modify ops: set_sequence_playback (playback range), configure_sequence_foundation (composed + dry_run)",
    "  NOTE: cinematic introspection + playback config only — no full Sequencer graph editing or keyframing",
    "",
    "Pass all domain-specific params inside the params object.",
  ].join("\n"),
  inputSchema: {
    type: "object",
    required: ["domain", "operation"],
    properties: {
      domain: {
        type: "string",
        description: "blueprint | anim | character | enhanced_input | material | asset | gas | niagara | multiplayer | ai | sequencer",
      },
      operation: {
        type: "string",
        description: "The specific operation to perform within the domain",
      },
      params: {
        type: "object",
        description: "All domain-specific parameters as key-value pairs",
      },
    },
  },
  annotations: {
    readOnlyHint: false,
    destructiveHint: true,
    idempotentHint: false,
    openWorldHint: false,
  },
};
