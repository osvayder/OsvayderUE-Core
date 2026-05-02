#!/usr/bin/env node

/**
 * UE5 MCP Server
 *
 * Bridges MCP-compatible AI clients to Unreal Engine 5's editor via HTTP REST API.
 * The UnrealClaude plugin runs an HTTP server (default port 3000) with editor manipulation tools.
 *
 * Environment Variables:
 *   UNREAL_MCP_URL - Base URL for Unreal MCP server (default: http://localhost:3000)
 *   MCP_REQUEST_TIMEOUT_MS - HTTP request timeout in milliseconds (default: 30000)
 *   INJECT_CONTEXT - Enable automatic context injection on tool calls (default: false)
 *   MCP_TOOL_CACHE_TTL_MS - TTL for tool list cache in milliseconds (default: 30000)
 */

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

// Dynamic context loader for UE 5.7 API documentation
import {
  getContextForTool,
  getContextForQuery,
  listCategories,
  getCategoryInfo,
  loadContextForCategory,
} from "./context-loader.js";

// Extracted library functions
import {
  log,
  fetchUnrealTools as _fetchUnrealTools,
  executeUnrealTool as _executeUnrealTool,
  executeUnrealToolAsync as _executeUnrealToolAsync,
  checkUnrealConnection as _checkUnrealConnection,
  convertToMCPSchema,
  convertAnnotations,
  formatToolResponse as _formatToolResponse,
  formatBridgeError as _formatBridgeError,
  readMcpAuthTokenFile,
  resolveMcpAuthToken,
} from "./lib.js";

// Tool router for mega-tool collapsing
import {
  classifyTool,
  resolveUnrealTool,
  categorizeToolForStatus,
  ROUTER_TOOL_SCHEMA,
} from "./tool-router.js";

// OsvayderEye: screen control tools are registered in C++ plugin via HTTP sidecar.
// No eye-proxy.js needed — tools come through UE HTTP :3000 as osvayder_* tools.

// Configuration with defaults
const CONFIG = {
  unrealMcpUrl: process.env.UNREAL_MCP_URL || "http://localhost:3000",
  requestTimeoutMs: parseInt(process.env.MCP_REQUEST_TIMEOUT_MS, 10) || 30000,
  injectContext: process.env.INJECT_CONTEXT === "true",
  asyncEnabled: process.env.MCP_ASYNC_ENABLED !== "false",
  asyncTimeoutMs: parseInt(process.env.MCP_ASYNC_TIMEOUT_MS, 10) || 300000,
  pollIntervalMs: parseInt(process.env.MCP_POLL_INTERVAL_MS, 10) || 2000,
  toolCacheTtlMs: parseInt(process.env.MCP_TOOL_CACHE_TTL_MS, 10) || 30000,
  mcpAuthToken: resolveMcpAuthToken(process.env),
  mcpAuthTokenFile: process.env.UNREAL_MCP_TOKEN_FILE || "",
  allowedToolNames: new Set(
    (process.env.UNREAL_MCP_ALLOWED_TOOLS || "")
      .split(",")
      .map((name) => name.trim())
      .filter(Boolean)
  ),
};

const hasScopedToolFilter = CONFIG.allowedToolNames.size > 0;
let runtimeMcpAuthToken = CONFIG.mcpAuthToken;

function refreshRuntimeMcpAuthTokenFromStatus(status) {
  if (runtimeMcpAuthToken || !status?.auth?.token_file) {
    return runtimeMcpAuthToken;
  }
  runtimeMcpAuthToken = readMcpAuthTokenFile(status.auth.token_file);
  if (runtimeMcpAuthToken) {
    log.info("MCP auth token loaded from status-discovered local handoff", {
      tokenFile: status.auth.token_file,
      tokenValueRedacted: true,
    });
  }
  return runtimeMcpAuthToken;
}

async function ensureRuntimeMcpAuthToken() {
  if (runtimeMcpAuthToken) {
    return runtimeMcpAuthToken;
  }
  const status = await _checkUnrealConnection(CONFIG.unrealMcpUrl, CONFIG.requestTimeoutMs);
  refreshRuntimeMcpAuthTokenFromStatus(status);
  return runtimeMcpAuthToken;
}

function getExposedToolName(rawToolName) {
  if (hasScopedToolFilter) {
    return rawToolName;
  }
  if (rawToolName === "restart_survival" || rawToolName.startsWith("osvayder_")) {
    return rawToolName;
  }
  return `unreal_${rawToolName}`;
}

function isScopedToolAllowed(rawToolName) {
  return !hasScopedToolFilter || CONFIG.allowedToolNames.has(rawToolName);
}

function shouldExposeListedTool(rawToolName) {
  const toolClass = classifyTool(rawToolName);
  if (toolClass === "hidden") {
    return false;
  }
  if (toolClass === "simple") {
    return true;
  }
  return hasScopedToolFilter;
}

// Cache for tools with TTL (avoids re-fetching the full tool list on every list_tools call)
let toolCache = { tools: [], timestamp: 0 };

// Bind CONFIG values to library functions for convenience
const fetchUnrealTools = () => _fetchUnrealTools(CONFIG.unrealMcpUrl, CONFIG.requestTimeoutMs, {
  authToken: runtimeMcpAuthToken,
  // Invalidate the cached tool list whenever a fetch ultimately fails (after
  // retries) so the next list_tools call re-fetches instead of serving a stale
  // empty list. See SP1-S4 A1.
  onFailure: () => {
    toolCache = { tools: [], timestamp: 0 };
  },
});
const executeUnrealTool = async (toolName, args) => {
  await ensureRuntimeMcpAuthToken();
  return _executeUnrealTool(CONFIG.unrealMcpUrl, CONFIG.requestTimeoutMs, toolName, args, {
    authToken: runtimeMcpAuthToken,
  });
};
const checkUnrealConnection = () => _checkUnrealConnection(CONFIG.unrealMcpUrl, CONFIG.requestTimeoutMs);
const formatToolResponse = (toolName, result) =>
  _formatToolResponse(toolName, result, CONFIG.injectContext ? getContextForTool : null);

// Create the MCP server
const server = new Server(
  {
    name: "ue5-mcp-server",
    version: "1.4.1",
  },
  {
    capabilities: {
      tools: {},
    },
  }
);

// Handle list_tools request
server.setRequestHandler(ListToolsRequestSchema, async () => {
  const status = await checkUnrealConnection();

  if (!status.connected) {
    log.info("Unreal not connected", { reason: status.reason });
    return {
      tools: [
        {
          name: "unreal_status",
          description: "Check if Unreal Editor is running with the plugin. Currently: NOT CONNECTED. Please start Unreal Editor with the plugin enabled.",
          inputSchema: {
            type: "object",
            properties: {},
          },
        },
      ],
    };
  }
  refreshRuntimeMcpAuthTokenFromStatus(status);

  let unrealTools;
  const cacheAge = Date.now() - toolCache.timestamp;
  if (toolCache.tools.length > 0 && cacheAge < CONFIG.toolCacheTtlMs) {
    unrealTools = toolCache.tools;
    log.debug("Using cached tool list", { ageMs: cacheAge });
  } else {
    unrealTools = await fetchUnrealTools();
    // Only populate the cache on a real (non-empty) fetch. When fetchUnrealTools
    // returns `[]` it has either failed (onFailure already reset toolCache) or
    // the server legitimately has no tools; in either case, storing a fresh
    // timestamp would serve an empty list for a full TTL window. Keep the
    // reset state so the next call re-fetches. See SP1-S4 A1.
    if (unrealTools.length > 0) {
      toolCache = { tools: unrealTools, timestamp: Date.now() };
    }
  }

  const mcpTools = [];

  // 1. Status first
  mcpTools.push({
    name: "unreal_status",
    description: `Check Unreal Editor connection status. Currently: CONNECTED to ${status.projectName || "Unknown Project"} (${status.engineVersion || "Unknown"})`,
    inputSchema: { type: "object", properties: {} },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  });

  // 2. Tools — osvayder_* keep their prefix, others get "unreal_" prefix
  for (const tool of toolCache.tools) {
    if (!isScopedToolAllowed(tool.name)) {
      continue;
    }

    if (shouldExposeListedTool(tool.name)) {
      const toolName = getExposedToolName(tool.name);
      mcpTools.push({
        name: toolName,
        description: tool.description,
        inputSchema: convertToMCPSchema(tool.parameters, true),
        annotations: convertAnnotations(tool.annotations),
      });
    }
  }

  // 3. Router tool (static schema for all mega-tools)
  if (!hasScopedToolFilter) {
    mcpTools.push(ROUTER_TOOL_SCHEMA);
  }

  // 4. Context tool last
  if (!hasScopedToolFilter) {
    mcpTools.push({
      name: "unreal_get_ue_context",
      description: `Get UE 5.7 API context. Categories: ${listCategories().join(", ")}`,
      inputSchema: {
        type: "object",
        properties: {
          category: {
            type: "string",
            description: `Specific category to load: ${listCategories().join(", ")}`,
          },
          query: {
            type: "string",
            description: "Search query to find relevant context (e.g., 'state machine transitions', 'async loading')",
          },
        },
      },
      annotations: {
        readOnlyHint: true,
        destructiveHint: false,
        idempotentHint: true,
        openWorldHint: false,
      },
    });
  }

  log.info("Tools listed", {
    exposed: mcpTools.length,
    cached: toolCache.tools.length,
    connected: true,
    scopedToolFilter: hasScopedToolFilter ? Array.from(CONFIG.allowedToolNames) : [],
  });
  return { tools: mcpTools };
});

// Handle call_tool request
server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  // Handle UE context request
  if (name === "unreal_get_ue_context") {
    if (hasScopedToolFilter) {
      return {
        content: [{ type: "text", text: "UE context tool is not exposed in the current narrow MCP scope." }],
        isError: true,
      };
    }

    const { category, query } = args || {};

    let result = null;
    let matchedCategories = [];

    if (category) {
      const content = loadContextForCategory(category);
      if (content) {
        result = content;
        matchedCategories = [category];
      } else {
        return {
          content: [
            {
              type: "text",
              text: `Unknown category: ${category}. Available categories: ${listCategories().join(", ")}`,
            },
          ],
          isError: true,
        };
      }
    }
    else if (query) {
      const queryResult = getContextForQuery(query);
      if (queryResult) {
        result = queryResult.content;
        matchedCategories = queryResult.categories;
      } else {
        return {
          content: [
            {
              type: "text",
              text: `No context found for query: "${query}". Try categories: ${listCategories().join(", ")}`,
            },
          ],
          isError: false,
        };
      }
    }
    else {
      const categoryList = listCategories().map((cat) => {
        const info = getCategoryInfo(cat);
        return `- **${cat}**: Keywords: ${info.keywords.slice(0, 5).join(", ")}...`;
      });

      return {
        content: [
          {
            type: "text",
            text: `# Available UE 5.7 Context Categories\n\n${categoryList.join("\n")}\n\nUse \`category\` param for specific context or \`query\` to search by keywords.`,
          },
        ],
      };
    }

    log.info("UE context loaded", { categories: matchedCategories });

    return {
      content: [
        {
          type: "text",
          text: `# UE 5.7 Context: ${matchedCategories.join(", ")}\n\n${result}`,
        },
      ],
    };
  }

  // Handle eye_* tools — route to UE HTTP (eye tools are registered in C++ registry)
  if (name.startsWith("osvayder_")) {
    try {
      const result = await executeUnrealTool(name, args);
      return formatToolResponse(name, result);
    } catch (error) {
      return {
        content: [{ type: "text", text: `Eye tool error: ${error.message}` }],
        isError: true,
      };
    }
  }

  // Handle status check
  if (name === "unreal_status") {
    const status = await checkUnrealConnection();
    if (status.connected) {
      const unrealTools = toolCache.tools.filter((tool) => isScopedToolAllowed(tool.name));
      const categories = {};

      for (const tool of unrealTools) {
        const category = categorizeToolForStatus(tool.name);
        categories[category] = (categories[category] || 0) + 1;
      }

      const contextCategories = hasScopedToolFilter ? [] : listCategories();
      const listedToolCount = unrealTools.filter(t => shouldExposeListedTool(t.name)).length;

      const response = {
        connected: true,
        project: status.projectName,
        engine: status.engineVersion,
        context_categories: contextCategories.length,
        tool_summary: categories,
        total_tools: unrealTools.length,
        exposed_tools: listedToolCount + (hasScopedToolFilter ? 1 : 3),
        message: "Unreal Editor connected. All tools operational.",
      };

      return {
        content: [{ type: "text", text: JSON.stringify(response, null, 2) }],
      };
    } else {
      return {
        content: [{
          type: "text",
          text: JSON.stringify({
            connected: false,
            reason: status.reason,
            message: "Unreal Editor is not running or the plugin is not enabled.",
          }, null, 2),
        }],
        isError: true,
      };
    }
  }

  // Router tool — dispatches to underlying mega-tool
  if (name === "unreal_ue") {
    if (hasScopedToolFilter) {
      return {
        content: [{ type: "text", text: "Router tool is not exposed in the current narrow MCP scope." }],
        isError: true,
      };
    }

    const { domain, operation, params: routerParams } = args || {};

    if (!domain || !operation) {
      return _formatBridgeError(
        "unreal_ue requires 'domain' and 'operation' parameters.",
        "missing_router_parameter",
        { tool_name: "unreal_ue", domain: domain ?? null, operation: operation ?? null }
      );
    }

    const targetTool = resolveUnrealTool(domain, operation);
    if (!targetTool) {
      return _formatBridgeError(
        `Unknown domain "${domain}". Valid domains: blueprint, anim, character, enhanced_input, material, asset`,
        "unknown_router_domain",
        { tool_name: "unreal_ue", domain, operation }
      );
    }

    const unrealArgs = { operation, ...(routerParams || {}) };

    log.info("Router dispatch", { domain, operation, targetTool });

    const progressToken = request.params._meta?.progressToken;
    const onProgress = progressToken
      ? ({ progress, total, message }) => {
          server.notification({
            method: "notifications/progress",
            params: { progressToken, progress, total: total || 0, message },
          });
        }
      : undefined;

    await ensureRuntimeMcpAuthToken();
    let result;
    if (CONFIG.asyncEnabled) {
      result = await _executeUnrealToolAsync(
        CONFIG.unrealMcpUrl,
        CONFIG.requestTimeoutMs,
        targetTool,
        unrealArgs,
        { onProgress, pollIntervalMs: CONFIG.pollIntervalMs, asyncTimeoutMs: CONFIG.asyncTimeoutMs, authToken: runtimeMcpAuthToken }
      );
    } else {
      result = await executeUnrealTool(targetTool, unrealArgs);
    }

    return formatToolResponse(targetTool, result);
  }

  // Strip "unreal_" prefix to get actual tool name
  const isDirectRawToolCall =
    name === "restart_survival" ||
    (hasScopedToolFilter &&
      name !== "unreal_status" &&
      name !== "unreal_ue" &&
      name !== "unreal_get_ue_context" &&
      !name.startsWith("unreal_"));

  if (!isDirectRawToolCall && !name.startsWith("unreal_")) {
    return {
      content: [
        {
          type: "text",
          text: `Unknown tool: ${name}`,
        },
      ],
      isError: true,
    };
  }

  const toolName = isDirectRawToolCall ? name : name.substring(7);
  if (!isScopedToolAllowed(toolName)) {
    return {
      content: [
        {
          type: "text",
          text: `Tool ${name} is not exposed in the current narrow MCP scope.`,
        },
      ],
      isError: true,
    };
  }

  // Tools excluded from auto-async: task_* tools and read-only tools
  const isTaskTool = toolName.startsWith("task_");
  const cachedTool = toolCache.tools.find(t => t.name === toolName);
  const isReadOnly = cachedTool?.annotations?.readOnlyHint === true;

  await ensureRuntimeMcpAuthToken();
  let result;
  if (CONFIG.asyncEnabled && !isTaskTool && !isReadOnly) {
    const progressToken = request.params._meta?.progressToken;
    const onProgress = progressToken
      ? ({ progress, total, message }) => {
          server.notification({
            method: "notifications/progress",
            params: { progressToken, progress, total: total || 0, message },
          });
        }
      : undefined;

    result = await _executeUnrealToolAsync(
      CONFIG.unrealMcpUrl,
      CONFIG.requestTimeoutMs,
      toolName,
      args,
      {
        onProgress,
        pollIntervalMs: CONFIG.pollIntervalMs,
        asyncTimeoutMs: CONFIG.asyncTimeoutMs,
        authToken: runtimeMcpAuthToken,
      }
    );
  } else {
    result = await executeUnrealTool(toolName, args);
  }

  const response = formatToolResponse(toolName, result);
  if (CONFIG.injectContext && result.success) {
    log.debug("Injected context for tool", { tool: toolName });
  }
  return response;
});

// Start the server
async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);

  const categories = listCategories();
  const testContext = loadContextForCategory("animation");
  const contextStatus = testContext ? `OK (${categories.length} categories loaded)` : "FAILED";

  log.info("UE5 MCP Server started", {
    version: "1.4.1",
    unrealUrl: CONFIG.unrealMcpUrl,
    timeoutMs: CONFIG.requestTimeoutMs,
    asyncEnabled: CONFIG.asyncEnabled,
    asyncTimeoutMs: CONFIG.asyncTimeoutMs,
    pollIntervalMs: CONFIG.pollIntervalMs,
    contextInjection: CONFIG.injectContext,
    authTokenConfigured: Boolean(runtimeMcpAuthToken),
    authTokenFileConfigured: Boolean(CONFIG.mcpAuthTokenFile),
    authTokenValueRedacted: true,
    contextSystem: contextStatus,
    contextCategories: categories,
  });
}

main().catch((error) => {
  log.error("Fatal error", { error: error.message, stack: error.stack });
  process.exit(1);
});
