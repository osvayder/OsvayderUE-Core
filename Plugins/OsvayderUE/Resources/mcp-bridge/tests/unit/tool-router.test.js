import { describe, it, expect } from "vitest";
import {
  classifyTool,
  resolveUnrealTool,
  categorizeToolForStatus,
  ROUTER_TOOL_SCHEMA,
  SIMPLE_TOOL_NAMES,
  HIDDEN_TOOL_NAMES,
  DOMAIN_TOOL_MAP,
} from "../../tool-router.js";

describe("classifyTool", () => {
  it("classifies all 17 simple tools", () => {
    const expected = [
      "spawn_actor", "move_actor", "delete_actors", "set_property",
      "get_level_actors", "open_level", "asset_search", "asset_dependencies",
      "asset_referencers", "capture_viewport", "get_output_log", "blueprint_query",
      "plugin_settings", "project_memory_status", "execution_log_status",
      "restart_survival", "mechanic_preflight",
    ];
    for (const name of expected) {
      expect(classifyTool(name)).toBe("simple");
    }
  });

  it("classifies all 9 hidden tools", () => {
    const expected = [
      "task_submit", "task_status", "task_result", "task_list", "task_cancel",
      "execute_script", "cleanup_scripts", "get_script_history", "run_console_command",
    ];
    for (const name of expected) {
      expect(classifyTool(name)).toBe("hidden");
    }
  });

  it("classifies mega tools (not simple, not hidden)", () => {
    const megas = [
      "blueprint_modify", "anim_blueprint_modify", "character",
      "character_data", "enhanced_input", "material", "asset",
      "gas", "niagara", "multiplayer", "ai", "sequencer",
    ];
    for (const name of megas) {
      expect(classifyTool(name)).toBe("mega");
    }
  });

  it("classifies unknown tools as mega (safe default)", () => {
    expect(classifyTool("future_tool")).toBe("mega");
    expect(classifyTool("")).toBe("mega");
  });
});

describe("resolveUnrealTool", () => {
  it("resolves all non-character domains", () => {
    expect(resolveUnrealTool("blueprint", "add_variable")).toBe("blueprint_modify");
    expect(resolveUnrealTool("anim", "add_state")).toBe("anim_blueprint_modify");
    expect(resolveUnrealTool("enhanced_input", "create_action")).toBe("enhanced_input");
    expect(resolveUnrealTool("material", "create_material_instance")).toBe("material");
    expect(resolveUnrealTool("asset", "duplicate")).toBe("asset");
  });

  it("routes character domain to 'character' for movement ops", () => {
    expect(resolveUnrealTool("character", "set_movement_param")).toBe("character");
    expect(resolveUnrealTool("character", "create_character_bp")).toBe("character");
    expect(resolveUnrealTool("character", "get_character_config")).toBe("character");
  });

  it("routes character domain to 'character_data' for data asset ops", () => {
    expect(resolveUnrealTool("character", "create_data_asset")).toBe("character_data");
    expect(resolveUnrealTool("character", "update_stats")).toBe("character_data");
    expect(resolveUnrealTool("character", "get_data_asset")).toBe("character_data");
    expect(resolveUnrealTool("character", "list_data_assets")).toBe("character_data");
    expect(resolveUnrealTool("character", "assign_data_asset")).toBe("character_data");
  });

  it("returns null for unknown domain", () => {
    expect(resolveUnrealTool("unknown", "op")).toBeNull();
  });

  it("returns null for null/undefined domain", () => {
    expect(resolveUnrealTool(null, "op")).toBeNull();
    expect(resolveUnrealTool(undefined, "op")).toBeNull();
  });
});

describe("ROUTER_TOOL_SCHEMA", () => {
  it("has name unreal_ue", () => {
    expect(ROUTER_TOOL_SCHEMA.name).toBe("unreal_ue");
  });

  it("requires domain and operation", () => {
    expect(ROUTER_TOOL_SCHEMA.inputSchema.required).toEqual(["domain", "operation"]);
  });

  it("has params as optional object", () => {
    const props = ROUTER_TOOL_SCHEMA.inputSchema.properties;
    expect(props.params.type).toBe("object");
    expect(ROUTER_TOOL_SCHEMA.inputSchema.required).not.toContain("params");
  });

  it("description mentions all eleven domains", () => {
    const desc = ROUTER_TOOL_SCHEMA.description;
    expect(desc).toContain('"blueprint"');
    expect(desc).toContain('"anim"');
    expect(desc).toContain('"character"');
    expect(desc).toContain('"enhanced_input"');
    expect(desc).toContain('"material"');
    expect(desc).toContain('"asset"');
    expect(desc).toContain('"gas"');
    expect(desc).toContain('"niagara"');
    expect(desc).toContain('"multiplayer"');
    expect(desc).toContain('"ai"');
    expect(desc).toContain('"sequencer"');
  });

  it("is not read-only (mega-tools mutate state)", () => {
    expect(ROUTER_TOOL_SCHEMA.annotations.readOnlyHint).toBe(false);
    expect(ROUTER_TOOL_SCHEMA.annotations.destructiveHint).toBe(true);
  });

  it("description includes key param names for discoverability", () => {
    const desc = ROUTER_TOOL_SCHEMA.description;
    expect(desc).toContain("blueprint_path");
    expect(desc).toContain("asset_path");
    expect(desc).toContain("material_path");
    expect(desc).toContain("action_name");
    expect(desc).toContain("character_name");
  });
});

describe("classification sets", () => {
  it("SIMPLE_TOOL_NAMES has 19 entries", () => {
    expect(SIMPLE_TOOL_NAMES.size).toBe(19);
  });

  it("HIDDEN_TOOL_NAMES has 9 entries", () => {
    expect(HIDDEN_TOOL_NAMES.size).toBe(9);
  });

  it("DOMAIN_TOOL_MAP has 11 domains with correct values", () => {
    expect(Object.keys(DOMAIN_TOOL_MAP)).toHaveLength(11);
    expect(DOMAIN_TOOL_MAP.blueprint).toBe("blueprint_modify");
    expect(DOMAIN_TOOL_MAP.anim).toBe("anim_blueprint_modify");
    expect(DOMAIN_TOOL_MAP.character).toBe("character");
    expect(DOMAIN_TOOL_MAP.enhanced_input).toBe("enhanced_input");
    expect(DOMAIN_TOOL_MAP.material).toBe("material");
    expect(DOMAIN_TOOL_MAP.asset).toBe("asset");
    expect(DOMAIN_TOOL_MAP.gas).toBe("gas");
    expect(DOMAIN_TOOL_MAP.niagara).toBe("niagara");
    expect(DOMAIN_TOOL_MAP.multiplayer).toBe("multiplayer");
    expect(DOMAIN_TOOL_MAP.ai).toBe("ai");
    expect(DOMAIN_TOOL_MAP.sequencer).toBe("sequencer");
  });

  it("no overlap between simple and hidden sets", () => {
    for (const name of SIMPLE_TOOL_NAMES) {
      expect(HIDDEN_TOOL_NAMES.has(name)).toBe(false);
    }
  });
});

describe("categorizeToolForStatus", () => {
  it("categorizes actor tools", () => {
    for (const name of ["spawn_actor", "move_actor", "delete_actors", "set_property", "get_level_actors"]) {
      expect(categorizeToolForStatus(name)).toBe("actor");
    }
  });

  it("categorizes level tools", () => {
    expect(categorizeToolForStatus("open_level")).toBe("level");
  });

  it("categorizes simple asset tools", () => {
    for (const name of ["asset_search", "asset_dependencies", "asset_referencers"]) {
      expect(categorizeToolForStatus(name)).toBe("asset");
    }
  });

  it("categorizes blueprint_query as blueprint", () => {
    expect(categorizeToolForStatus("blueprint_query")).toBe("blueprint");
  });

  it("categorizes utility tools", () => {
    for (const name of ["capture_viewport", "get_output_log"]) {
      expect(categorizeToolForStatus(name)).toBe("utility");
    }
  });

  it("categorizes mechanic_preflight distinctly", () => {
    expect(categorizeToolForStatus("mechanic_preflight")).toBe("mechanic_preflight");
  });

  it("categorizes mega tools by domain", () => {
    expect(categorizeToolForStatus("blueprint_modify")).toBe("blueprint");
    expect(categorizeToolForStatus("anim_blueprint_modify")).toBe("anim");
    expect(categorizeToolForStatus("character")).toBe("character");
    expect(categorizeToolForStatus("character_data")).toBe("character");
    expect(categorizeToolForStatus("enhanced_input")).toBe("enhanced_input");
    expect(categorizeToolForStatus("material")).toBe("material");
    expect(categorizeToolForStatus("asset")).toBe("asset");
  });

  it("categorizes task queue tools", () => {
    for (const name of ["task_submit", "task_status", "task_result", "task_list", "task_cancel"]) {
      expect(categorizeToolForStatus(name)).toBe("task_queue");
    }
  });

  it("categorizes scripting tools", () => {
    for (const name of ["execute_script", "cleanup_scripts", "get_script_history", "run_console_command"]) {
      expect(categorizeToolForStatus(name)).toBe("scripting");
    }
  });

  it("returns utility for unknown tools", () => {
    expect(categorizeToolForStatus("future_unknown_tool")).toBe("utility");
  });
});

describe("blueprint router split", () => {
  it("routes blueprint read ops to blueprint_query", () => {
    const readOps = ["list", "inspect", "get_graph", "list_graphs", "list_macros", "get_level_blueprint", "list_editor_utilities", "get_editor_utility_details",
      "get_class_defaults", "get_editable_properties", "get_component_editable_properties",
      "get_data_asset_properties", "get_data_table_schema", "get_data_table_rows", "get_widget_tree", "get_anim_blueprint_info", "animation_preflight",
      "get_graph_nodes", "find_nodes", "get_node_pins",
      "get_node", "get_node_connections", "can_connect_pins", "list_interfaces", "list_dispatchers", "get_function_signature"];
    for (const op of readOps) {
      expect(resolveUnrealTool("blueprint", op)).toBe("blueprint_query");
    }
  });

  it("routes animation_preflight to blueprint_query", () => {
    expect(resolveUnrealTool("blueprint", "animation_preflight")).toBe("blueprint_query");
  });

  it("routes blueprint write ops to blueprint_modify", () => {
    const writeOps = ["create", "add_variable", "remove_variable", "add_function", "remove_function",
      "add_node", "add_nodes", "delete_node", "connect_pins", "disconnect_pins", "set_pin_value", "validate_blueprint",
      "replace_node", "set_function_signature", "add_interface", "remove_interface",
      "add_dispatcher", "remove_dispatcher", "apply_blueprint_spec",
      "create_macro", "remove_macro", "add_macro_instance",
      "run_editor_utility", "bind_dispatcher",
      "set_class_default", "set_class_defaults", "set_component_properties_batch",
      "modify_collection", "configure_actor_class",
      "set_data_asset_properties", "set_data_table_row", "remove_data_table_row",
      "add_widget", "remove_widget", "set_widget_property",
      "set_anim_blueprint",
      "add_component", "remove_component", "rename_component", "reparent_component", "set_component_property"];
    for (const op of writeOps) {
      expect(resolveUnrealTool("blueprint", op)).toBe("blueprint_modify");
    }
  });

  it("routes component read ops to blueprint_query", () => {
    const readOps = ["list_components", "get_component_tree", "get_component_details"];
    for (const op of readOps) {
      expect(resolveUnrealTool("blueprint", op)).toBe("blueprint_query");
    }
  });
});

describe("osvayder tool classification", () => {
  it("classifies osvayder_ tools as simple", () => {
    const eyeTools = [
      "osvayder_take_screenshot", "osvayder_mouse_click", "osvayder_keyboard_press",
      "osvayder_focus_window", "osvayder_get_cursor_position",
    ];
    for (const name of eyeTools) {
      expect(classifyTool(name)).toBe("simple");
    }
  });
});

describe("EP3: Property v3 + Composed Bundles v2", () => {
  it("routes modify_collection to blueprint_modify", () => {
    expect(resolveUnrealTool("blueprint", "modify_collection")).toBe("blueprint_modify");
  });

  it("routes configure_actor_class to blueprint_modify", () => {
    expect(resolveUnrealTool("blueprint", "configure_actor_class")).toBe("blueprint_modify");
  });

  it("ROUTER_TOOL_SCHEMA description mentions modify_collection", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain("modify_collection");
  });

  it("ROUTER_TOOL_SCHEMA description mentions configure_actor_class", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain("configure_actor_class");
  });

  it("modify_collection is not a read op (routes to blueprint_modify)", () => {
    expect(resolveUnrealTool("blueprint", "modify_collection")).toBe("blueprint_modify");
  });

  it("configure_actor_class is not a read op (routes to blueprint_modify)", () => {
    expect(resolveUnrealTool("blueprint", "configure_actor_class")).toBe("blueprint_modify");
  });
});

describe("OSP1: GAS Foundations v1", () => {
  it("routes gas domain to gas tool", () => {
    expect(resolveUnrealTool("gas", "list_abilities")).toBe("gas");
    expect(resolveUnrealTool("gas", "get_ability_info")).toBe("gas");
    expect(resolveUnrealTool("gas", "list_effects")).toBe("gas");
    expect(resolveUnrealTool("gas", "get_effect_info")).toBe("gas");
    expect(resolveUnrealTool("gas", "list_attribute_sets")).toBe("gas");
    expect(resolveUnrealTool("gas", "get_attribute_set_info")).toBe("gas");
    expect(resolveUnrealTool("gas", "set_effect_properties")).toBe("gas");
    expect(resolveUnrealTool("gas", "configure_gas_ability")).toBe("gas");
  });

  it("classifies gas as mega", () => {
    expect(classifyTool("gas")).toBe("mega");
  });

  it("categorizes gas tool by domain", () => {
    expect(categorizeToolForStatus("gas")).toBe("gas");
  });

  it("ROUTER_TOOL_SCHEMA description mentions gas domain", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain('"gas"');
    expect(ROUTER_TOOL_SCHEMA.description).toContain("list_abilities");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("configure_gas_ability");
  });
});

describe("OSP1: Niagara/VFX Foundations v1", () => {
  it("routes niagara domain to niagara tool", () => {
    expect(resolveUnrealTool("niagara", "list_systems")).toBe("niagara");
    expect(resolveUnrealTool("niagara", "get_system_info")).toBe("niagara");
    expect(resolveUnrealTool("niagara", "get_system_parameters")).toBe("niagara");
    expect(resolveUnrealTool("niagara", "list_emitters")).toBe("niagara");
    expect(resolveUnrealTool("niagara", "get_emitter_info")).toBe("niagara");
    expect(resolveUnrealTool("niagara", "set_system_parameter")).toBe("niagara");
  });

  it("classifies niagara as mega", () => {
    expect(classifyTool("niagara")).toBe("mega");
  });

  it("categorizes niagara tool by domain", () => {
    expect(categorizeToolForStatus("niagara")).toBe("niagara");
  });

  it("ROUTER_TOOL_SCHEMA description mentions niagara domain", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain('"niagara"');
    expect(ROUTER_TOOL_SCHEMA.description).toContain("list_systems");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("set_system_parameter");
  });
});

describe("MMP1: Multiplayer Foundations v1", () => {
  it("routes multiplayer domain to multiplayer tool", () => {
    expect(resolveUnrealTool("multiplayer", "get_replication_info")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "get_rpc_info")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "get_ownership_info")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "get_framework_roles")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "get_multiplayer_config")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "multiplayer_audit")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "set_replication_config")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "set_property_replication")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "configure_multiplayer_actor")).toBe("multiplayer");
  });

  it("classifies multiplayer as mega", () => {
    expect(classifyTool("multiplayer")).toBe("mega");
  });

  it("categorizes multiplayer tool by domain", () => {
    expect(categorizeToolForStatus("multiplayer")).toBe("multiplayer");
  });

  it("ROUTER_TOOL_SCHEMA description mentions multiplayer domain", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain('"multiplayer"');
    expect(ROUTER_TOOL_SCHEMA.description).toContain("get_replication_info");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("configure_multiplayer_actor");
  });
});

describe("MMP2: Advanced Multiplayer + GAS Cosmetic Split", () => {
  it("routes new multiplayer advanced ops", () => {
    expect(resolveUnrealTool("multiplayer", "get_component_replication")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "get_replication_graph")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "classify_multiplayer_actor")).toBe("multiplayer");
  });

  it("routes GAS multiplayer bridge ops", () => {
    expect(resolveUnrealTool("gas", "classify_gas_multiplayer")).toBe("gas");
    expect(resolveUnrealTool("gas", "configure_gas_multiplayer_setup")).toBe("gas");
  });

  it("ROUTER_TOOL_SCHEMA mentions advanced multiplayer ops", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain("get_replication_graph");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("classify_multiplayer_actor");
  });

  it("ROUTER_TOOL_SCHEMA mentions GAS multiplayer bridge", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain("classify_gas_multiplayer");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("configure_gas_multiplayer_setup");
  });
});

describe("MMP3: Replicated Subobjects + Travel Contracts", () => {
  it("routes MMP3 multiplayer ops", () => {
    expect(resolveUnrealTool("multiplayer", "get_subobject_replication")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "audit_object_references")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "get_travel_contracts")).toBe("multiplayer");
  });

  it("ROUTER_TOOL_SCHEMA mentions MMP3 ops", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain("get_subobject_replication");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("audit_object_references");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("get_travel_contracts");
  });
});

describe("MMP4: Live Runtime + Persistence Audit", () => {
  it("routes MMP4 multiplayer ops", () => {
    expect(resolveUnrealTool("multiplayer", "get_network_state")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "get_live_actor_network")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "audit_persistence_placement")).toBe("multiplayer");
  });

  it("ROUTER_TOOL_SCHEMA mentions MMP4 ops", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain("get_network_state");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("get_live_actor_network");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("audit_persistence_placement");
  });
});

describe("MMP5: Runtime Session/Travel + Live Audit", () => {
  it("routes MMP5 multiplayer ops", () => {
    expect(resolveUnrealTool("multiplayer", "get_session_state")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "get_travel_state")).toBe("multiplayer");
    expect(resolveUnrealTool("multiplayer", "audit_live_replication")).toBe("multiplayer");
  });

  it("ROUTER_TOOL_SCHEMA mentions MMP5 ops", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain("get_session_state");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("get_travel_state");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("audit_live_replication");
  });
});

describe("OSP2: AI / Behavior Trees / EQS", () => {
  it("routes AI domain to ai tool", () => {
    expect(resolveUnrealTool("ai", "list_behavior_trees")).toBe("ai");
    expect(resolveUnrealTool("ai", "get_behavior_tree_info")).toBe("ai");
    expect(resolveUnrealTool("ai", "list_blackboards")).toBe("ai");
    expect(resolveUnrealTool("ai", "get_blackboard_info")).toBe("ai");
    expect(resolveUnrealTool("ai", "list_eqs_queries")).toBe("ai");
    expect(resolveUnrealTool("ai", "get_eqs_info")).toBe("ai");
    expect(resolveUnrealTool("ai", "add_blackboard_key")).toBe("ai");
    expect(resolveUnrealTool("ai", "remove_blackboard_key")).toBe("ai");
    expect(resolveUnrealTool("ai", "configure_ai_foundation")).toBe("ai");
  });

  it("classifies ai as mega", () => {
    expect(classifyTool("ai")).toBe("mega");
  });

  it("categorizes ai tool by domain", () => {
    expect(categorizeToolForStatus("ai")).toBe("ai");
  });

  it("ROUTER_TOOL_SCHEMA mentions ai domain", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain('"ai"');
    expect(ROUTER_TOOL_SCHEMA.description).toContain("list_behavior_trees");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("configure_ai_foundation");
  });
});

describe("OSP3: Sequencer / Cinematics", () => {
  it("routes sequencer domain to sequencer tool", () => {
    expect(resolveUnrealTool("sequencer", "list_sequences")).toBe("sequencer");
    expect(resolveUnrealTool("sequencer", "get_sequence_info")).toBe("sequencer");
    expect(resolveUnrealTool("sequencer", "set_sequence_playback")).toBe("sequencer");
    expect(resolveUnrealTool("sequencer", "configure_sequence_foundation")).toBe("sequencer");
  });

  it("classifies sequencer as mega", () => {
    expect(classifyTool("sequencer")).toBe("mega");
  });

  it("categorizes sequencer tool by domain", () => {
    expect(categorizeToolForStatus("sequencer")).toBe("sequencer");
  });

  it("ROUTER_TOOL_SCHEMA mentions sequencer domain", () => {
    expect(ROUTER_TOOL_SCHEMA.description).toContain('"sequencer"');
    expect(ROUTER_TOOL_SCHEMA.description).toContain("list_sequences");
    expect(ROUTER_TOOL_SCHEMA.description).toContain("configure_sequence_foundation");
  });
});
