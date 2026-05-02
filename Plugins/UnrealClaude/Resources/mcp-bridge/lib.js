/**
 * UE5 MCP Server - Extracted Library Functions
 *
 * Pure/testable functions extracted from index.js.
 * Functions that previously read from the module-scoped CONFIG closure
 * now accept those values as explicit parameters.
 */

import fs from "node:fs";

/**
 * Structured logging helper - writes to stderr to not interfere with MCP protocol
 */
export const log = {
  info: (msg, data) => console.error(`[INFO] ${msg}`, data ? JSON.stringify(data) : ""),
  error: (msg, data) => console.error(`[ERROR] ${msg}`, data ? JSON.stringify(data) : ""),
  debug: (msg, data) => process.env.DEBUG && console.error(`[DEBUG] ${msg}`, data ? JSON.stringify(data) : ""),
};

export const MCP_AUTH_HEADER = "X-UnrealClaude-MCP-Token";

function isUsableTokenFilePath(tokenFile) {
  return typeof tokenFile === "string" &&
    tokenFile.trim().length > 0 &&
    !tokenFile.includes("${") &&
    !tokenFile.includes("}");
}

/**
 * Read a runtime MCP auth token from the plugin-written local handoff file.
 * Supports both JSON `{ token: "..." }` and raw-token files. Never logs token values.
 */
export function readMcpAuthTokenFile(tokenFile, fsImpl = fs) {
  if (!isUsableTokenFilePath(tokenFile)) {
    return "";
  }

  try {
    const content = fsImpl.readFileSync(tokenFile, "utf8").trim();
    if (!content) {
      return "";
    }
    try {
      const parsed = JSON.parse(content);
      return typeof parsed.token === "string" ? parsed.token.trim() : "";
    } catch (_) {
      return content;
    }
  } catch (error) {
    log.debug("MCP auth token file unavailable", { tokenFile, error: error.message });
    return "";
  }
}

/**
 * Resolve token from environment or token file. Returns an empty string when unavailable.
 */
export function resolveMcpAuthToken(env = process.env, fsImpl = fs) {
  if (typeof env.UNREAL_MCP_TOKEN === "string" && env.UNREAL_MCP_TOKEN.trim().length > 0) {
    return env.UNREAL_MCP_TOKEN.trim();
  }

  return readMcpAuthTokenFile(env.UNREAL_MCP_TOKEN_FILE, fsImpl);
}

export function buildMcpAuthHeaders(authToken) {
  return authToken ? { [MCP_AUTH_HEADER]: authToken } : {};
}

/**
 * Fetch with timeout using AbortController
 * @param {string} url - URL to fetch
 * @param {object} options - fetch options
 * @param {number} timeoutMs - timeout in milliseconds
 */
export async function fetchWithTimeout(url, options = {}, timeoutMs = 30000) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetch(url, {
      ...options,
      signal: controller.signal,
    });
    return response;
  } finally {
    clearTimeout(timeout);
  }
}

/**
 * Retry policy: 3 retries on top of the initial attempt.
 * Delay sequence between attempts: 1s → 2s → 4s.
 * No jitter — single local bridge process issues requests serially from the
 * MCP client, so thundering-herd mitigation is not needed and deterministic
 * delays keep the tests simple.
 */
export const RETRY_DELAYS_MS = [1000, 2000, 4000];

/**
 * Classify whether an HTTP response status is retryable.
 * Retryable: 502 Bad Gateway, 503 Service Unavailable, 504 Gateway Timeout.
 * Non-retryable: all other statuses (including 4xx — a 4xx is a deterministic
 * client error and will not succeed on retry).
 */
export function isRetryableStatus(status) {
  return status === 502 || status === 503 || status === 504;
}

/**
 * Classify whether an error thrown by fetch is retryable.
 * Retryable: network-level errors (connection refused, DNS, non-abort timeouts).
 * Non-retryable: AbortError from a user-initiated AbortController signal.
 */
export function isRetryableError(error) {
  if (!error) return false;
  // A timeout inside fetchWithTimeout surfaces as AbortError with our controller.
  // A user-initiated abort also surfaces as AbortError. We treat our-own-timeout
  // as retryable by inspecting the signal origin: callers must pass a
  // `userAbortSignal` to mark true user aborts as non-retryable.
  if (error.name === "AbortError" && error.__userAbort === true) {
    return false;
  }
  if (error.name === "AbortError") {
    // Treat our own timeout aborts as retryable (network-level timeout).
    return true;
  }
  return true;
}

/**
 * Perform a fetch with retries and exponential backoff.
 *
 * @param {string} url - URL to fetch
 * @param {object} options - fetch options
 * @param {number} timeoutMs - per-attempt timeout in milliseconds
 * @param {object} [retryOptions]
 * @param {number[]} [retryOptions.delays] - delay sequence between attempts
 * @param {AbortSignal} [retryOptions.userAbortSignal] - user-initiated abort; stops retry loop
 * @returns {Promise<Response>} the final Response (ok or non-retryable non-ok)
 * @throws propagates the last error if every attempt failed with a retryable error
 */
export async function fetchWithRetry(url, options, timeoutMs, retryOptions = {}) {
  const delays = retryOptions.delays || RETRY_DELAYS_MS;
  const userAbortSignal = retryOptions.userAbortSignal;
  let lastError;

  // Total attempts = 1 initial + delays.length retries.
  for (let attempt = 0; attempt <= delays.length; attempt++) {
    if (userAbortSignal?.aborted) {
      const err = new Error("User aborted");
      err.name = "AbortError";
      err.__userAbort = true;
      throw err;
    }

    try {
      const response = await fetchWithTimeout(url, options, timeoutMs);
      if (response.ok) {
        return response;
      }
      // Non-2xx response. Decide if we retry or bail.
      if (!isRetryableStatus(response.status) || attempt === delays.length) {
        return response;
      }
      log.debug("Retryable HTTP status, backing off", { url, status: response.status, attempt });
    } catch (error) {
      // Detect user abort before classifying.
      if (userAbortSignal?.aborted) {
        const err = new Error("User aborted");
        err.name = "AbortError";
        err.__userAbort = true;
        throw err;
      }
      lastError = error;
      if (!isRetryableError(error) || attempt === delays.length) {
        throw error;
      }
      log.debug("Retryable fetch error, backing off", { url, error: error.message, attempt });
    }

    await sleep(delays[attempt]);
  }

  // Unreachable in practice — the loop either returns a Response or throws.
  throw lastError || new Error("fetchWithRetry exhausted without result");
}

/**
 * Fetch tools from the Unreal HTTP server
 * @param {string} baseUrl - Unreal MCP server base URL
 * @param {number} timeoutMs - request timeout in milliseconds
 * @param {object} [options]
 * @param {function} [options.onFailure] - called with the error if the fetch
 *   ultimately fails (after all retries). Used by the caller to invalidate
 *   any cached tool list so the next list_tools call re-fetches.
 */
export async function fetchUnrealTools(baseUrl, timeoutMs, options = {}) {
  const { onFailure, authToken = "" } = options;
  try {
    const response = await fetchWithRetry(`${baseUrl}/mcp/tools`, {
      headers: buildMcpAuthHeaders(authToken),
    }, timeoutMs);
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${response.statusText}`);
    }
    const data = await response.json();
    return data.tools || [];
  } catch (error) {
    if (error.name === "AbortError") {
      log.error("Request timeout fetching tools", { url: `${baseUrl}/mcp/tools` });
    } else {
      log.error("Failed to fetch tools from Unreal", { error: error.message });
    }
    if (onFailure) {
      try {
        onFailure(error);
      } catch (_) {
        // onFailure must not break the fetch contract.
      }
    }
    return [];
  }
}

/**
 * Execute a tool via the Unreal HTTP server
 * @param {string} baseUrl - Unreal MCP server base URL
 * @param {number} timeoutMs - request timeout in milliseconds
 * @param {string} toolName - name of the tool to execute
 * @param {object} args - tool arguments
 */
export async function executeUnrealTool(baseUrl, timeoutMs, toolName, args, options = {}) {
  const url = `${baseUrl}/mcp/tool/${toolName}`;
  const { authToken = "" } = options;
  try {
    const response = await fetchWithRetry(url, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        ...buildMcpAuthHeaders(authToken),
      },
      body: JSON.stringify(args || {}),
    }, timeoutMs);

    const data = await response.json();
    log.debug("Tool executed", { tool: toolName, success: data.success });
    return data;
  } catch (error) {
    const errorMessage = error.name === "AbortError"
      ? `Request timeout after ${timeoutMs}ms`
      : error.message;
    log.error("Tool execution failed", { tool: toolName, error: errorMessage });
    return {
      success: false,
      message: `Failed to execute tool: ${errorMessage}`,
    };
  }
}

/**
 * Check if Unreal Editor is running with the plugin
 * @param {string} baseUrl - Unreal MCP server base URL
 * @param {number} timeoutMs - request timeout in milliseconds
 */
export async function checkUnrealConnection(baseUrl, timeoutMs) {
  try {
    const response = await fetchWithTimeout(`${baseUrl}/mcp/status`, {}, timeoutMs);
    if (response.ok) {
      const data = await response.json();
      return { connected: true, ...data };
    }
    return { connected: false, reason: `HTTP ${response.status}` };
  } catch (error) {
    const reason = error.name === "AbortError" ? "timeout" : error.message;
    return { connected: false, reason };
  }
}

/**
 * Convert Unreal tool parameter schema to MCP tool input schema
 * @param {Array} unrealParams - array of parameter descriptors from Unreal
 * @param {boolean} compact - if true, omit defaults and trim long descriptions to reduce token count
 */
export function convertToMCPSchema(unrealParams, compact = false) {
  const properties = {};
  const required = [];

  for (const param of unrealParams || []) {
    const prop = {};
    if (param.type !== "any") {
      prop.type = param.type === "number" ? "number" :
                  param.type === "boolean" ? "boolean" :
                  param.type === "array" ? "array" :
                  param.type === "object" ? "object" : "string";
    }

    if (param.description) {
      prop.description = compact && param.description.length > 80
        ? param.description.slice(0, 77) + "..."
        : param.description;
    }

    if (!compact && param.default !== undefined) {
      prop.default = param.default;
    }

    properties[param.name] = prop;

    if (param.required) {
      required.push(param.name);
    }
  }

  return {
    type: "object",
    properties,
    required: required.length > 0 ? required : undefined,
  };
}

/**
 * Sleep for a given number of milliseconds.
 * @param {number} ms - milliseconds to sleep
 */
export function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/**
 * Execute a tool via Unreal's async task queue (task_submit → poll task_status → task_result).
 * Falls back to synchronous executeUnrealTool() if task_submit fails.
 *
 * @param {string} baseUrl - Unreal MCP server base URL
 * @param {number} timeoutMs - per-request HTTP timeout in milliseconds
 * @param {string} toolName - name of the tool to execute
 * @param {object} args - tool arguments
 * @param {object} [options]
 * @param {function} [options.onProgress] - callback({progress, total, message})
 * @param {number}   [options.pollIntervalMs=2000] - poll interval
 * @param {number}   [options.asyncTimeoutMs=300000] - overall async timeout (5 min)
 * @param {number}   [options.taskTimeoutMs=asyncTimeoutMs] - server-side task timeout submitted to Unreal
 * @param {number}   [options.cancelWaitMs=max(timeoutMs,5000)] - how long to wait for terminal state after timeout cancellation
 */
export async function executeUnrealToolAsync(baseUrl, timeoutMs, toolName, args, options = {}) {
  const {
    onProgress,
    pollIntervalMs = 2000,
    asyncTimeoutMs = 300000,
    taskTimeoutMs = asyncTimeoutMs,
    cancelWaitMs = Math.max(timeoutMs, 5000),
    authToken = "",
  } = options;
  const authHeaders = buildMcpAuthHeaders(authToken);

  // Step 1: Submit task
  let taskId;
  try {
    const submitResponse = await fetchWithTimeout(`${baseUrl}/mcp/tool/task_submit`, {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeaders },
      body: JSON.stringify({
        tool_name: toolName,
        params: args || {},
        timeout_ms: taskTimeoutMs,
      }),
    }, timeoutMs);

    const submitData = await submitResponse.json();
    if (!submitData.success || !submitData.data?.task_id) {
      log.debug("task_submit failed or no task_id, falling back to sync", { tool: toolName });
      return executeUnrealTool(baseUrl, timeoutMs, toolName, args, { authToken });
    }
    taskId = submitData.data.task_id;
    log.debug("Task submitted", { tool: toolName, taskId });
  } catch (error) {
    log.debug("task_submit error, falling back to sync", { tool: toolName, error: error.message });
    return executeUnrealTool(baseUrl, timeoutMs, toolName, args, { authToken });
  }

  // Step 2: Poll for completion
  const deadline = Date.now() + asyncTimeoutMs;
  let pollCount = 0;

  const terminalStatuses = new Set(["completed", "failed", "cancelled", "timed_out"]);

  const fetchTaskResult = async (taskStatus, asyncTimeoutData = null) => {
    try {
      const resultResponse = await fetchWithTimeout(`${baseUrl}/mcp/tool/task_result`, {
        method: "POST",
        headers: { "Content-Type": "application/json", ...authHeaders },
        body: JSON.stringify({ task_id: taskId }),
      }, timeoutMs);
      const resultData = await resultResponse.json();
      if (asyncTimeoutData) {
        if (!resultData.data || typeof resultData.data !== "object") {
          resultData.data = {};
        }
        resultData.data.async_timeout = {
          ...asyncTimeoutData,
          terminal_state_reached: true,
          terminal_status: taskStatus,
        };
      }
      log.debug("Task completed", { tool: toolName, taskId, status: taskStatus });
      return resultData;
    } catch (error) {
      log.error("task_result fetch failed", { taskId, error: error.message });
      return {
        success: false,
        message: `Task ${taskStatus} but failed to retrieve result: ${error.message}`,
        data: asyncTimeoutData ? {
          ...asyncTimeoutData,
          terminal_state_reached: true,
          terminal_status: taskStatus,
          result_fetch_error: error.message,
        } : undefined,
      };
    }
  };

  while (Date.now() < deadline) {
    await sleep(pollIntervalMs);
    pollCount++;

    let statusData;
    try {
      const statusResponse = await fetchWithTimeout(`${baseUrl}/mcp/tool/task_status`, {
        method: "POST",
        headers: { "Content-Type": "application/json", ...authHeaders },
        body: JSON.stringify({ task_id: taskId }),
      }, timeoutMs);
      statusData = await statusResponse.json();
    } catch (error) {
      log.error("task_status poll failed", { taskId, error: error.message });
      // Apply a ≥500ms backoff before the next poll attempt to avoid a tight
      // retry loop when pollIntervalMs is configured small. The top of the
      // loop will additionally sleep pollIntervalMs, so the actual floor is
      // max(500, pollIntervalMs). The extra delay is skipped when
      // pollIntervalMs already meets the floor.
      const extraDelayMs = Math.max(0, 500 - pollIntervalMs);
      if (extraDelayMs > 0) {
        await sleep(extraDelayMs);
      }
      continue;
    }

    const taskStatus = statusData.data?.status || statusData.status;
    const progress = statusData.data?.progress ?? pollCount;
    const total = statusData.data?.total ?? 0;
    const progressMessage = statusData.data?.progress_message || `Polling... (${pollCount})`;

    // Send progress notification
    if (onProgress) {
      onProgress({ progress, total, message: progressMessage });
    }

    // Check for terminal states
    if (terminalStatuses.has(taskStatus)) {
      // Step 3: Retrieve result
      return fetchTaskResult(taskStatus);
    }
  }

  // Async timeout exceeded. Request cancellation, then wait for terminal state
  // before returning so callers do not receive a terminal failure while the
  // queued task can still mutate afterward.
  let cancelResponseData = null;
  let cancelError = null;
  try {
    const cancelResponse = await fetchWithTimeout(`${baseUrl}/mcp/tool/task_cancel`, {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeaders },
      body: JSON.stringify({ task_id: taskId }),
    }, timeoutMs);
    cancelResponseData = await cancelResponse.json();
  } catch (error) {
    cancelError = error.message;
    log.error("task_cancel after async timeout failed", { taskId, error: error.message });
  }

  const asyncTimeoutData = {
    result_type: "async_error",
    schema_version: "mcp_async_error.v1",
    error_category: "async_timeout",
    task_id: taskId,
    timeout_ms: asyncTimeoutMs,
    task_timeout_ms: taskTimeoutMs,
    cancel_requested: cancelResponseData?.success === true,
    cancel_response: cancelResponseData,
    cancel_error: cancelError,
    terminal_state_reached: false,
  };

  const cancelDeadline = Date.now() + cancelWaitMs;
  while (Date.now() < cancelDeadline) {
    await sleep(pollIntervalMs);

    let statusData;
    try {
      const statusResponse = await fetchWithTimeout(`${baseUrl}/mcp/tool/task_status`, {
        method: "POST",
        headers: { "Content-Type": "application/json", ...authHeaders },
        body: JSON.stringify({ task_id: taskId }),
      }, timeoutMs);
      statusData = await statusResponse.json();
    } catch (error) {
      log.error("task_status after timeout cancellation failed", { taskId, error: error.message });
      continue;
    }

    const taskStatus = statusData.data?.status || statusData.status;
    if (terminalStatuses.has(taskStatus)) {
      return fetchTaskResult(taskStatus, asyncTimeoutData);
    }
  }

  return {
    success: false,
    message: `Task timed out after ${asyncTimeoutMs}ms (task_id: ${taskId}); cancellation requested but terminal state was not reached within ${cancelWaitMs}ms`,
    data: {
      ...asyncTimeoutData,
      error_category: "async_timeout_pending",
      cancel_wait_ms: cancelWaitMs,
    },
  };
}

/**
 * Format a tool result into MCP response content blocks.
 * Detects image_base64 in capture_viewport results and returns native ImageContent.
 *
 * @param {string} toolName - raw Unreal tool name (no "unreal_" prefix)
 * @param {object} result - {success, message, data} from Unreal
 * @param {function|null} getContext - optional (toolName) => string|null for context injection
 * @returns {{ content: Array, isError: boolean }}
 */
export function formatToolResponse(toolName, result, getContext) {
  if (!result.success) {
    let text = `Error: ${result.message || "Tool execution failed"}`;
    if (Object.prototype.hasOwnProperty.call(result, "data") && result.data !== undefined && result.data !== null) {
      text += `\n\n${JSON.stringify(result.data)}`;
    }
    return {
      content: [{ type: "text", text }],
      isError: true,
    };
  }

  const content = [];

  if (toolName === "capture_viewport" && result.data?.image_base64) {
    content.push({
      type: "image",
      data: result.data.image_base64,
      mimeType: `image/${result.data.format || "jpeg"}`,
    });
    // Include metadata without the huge base64 string
    const meta = { ...result.data };
    delete meta.image_base64;
    content.push({ type: "text", text: result.message + "\n\n" + JSON.stringify(meta) });
  } else {
    let text = result.message + (result.data ? "\n\n" + JSON.stringify(result.data) : "");
    if (getContext) {
      const ctx = getContext(toolName);
      if (ctx) {
        text += `\n\n---\n\n## Relevant UE 5.7 API Context\n\n${ctx}`;
      }
    }
    content.push({ type: "text", text });
  }

  return { content, isError: false };
}

export function formatBridgeError(message, errorCategory, extra = {}) {
  const data = {
    result_type: "bridge_error",
    schema_version: "mcp_bridge_error.v1",
    error_category: errorCategory,
    error_message: message,
    ...extra,
  };

  return {
    content: [{ type: "text", text: `Error: ${message}\n\n${JSON.stringify(data)}` }],
    isError: true,
  };
}

/**
 * Convert Unreal tool annotations to MCP annotations format
 * @param {object} unrealAnnotations - annotation object from Unreal
 */
export function convertAnnotations(unrealAnnotations) {
  if (!unrealAnnotations) {
    return {
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    };
  }
  return {
    readOnlyHint: unrealAnnotations.readOnlyHint ?? false,
    destructiveHint: unrealAnnotations.destructiveHint ?? true,
    idempotentHint: unrealAnnotations.idempotentHint ?? false,
    openWorldHint: unrealAnnotations.openWorldHint ?? false,
  };
}
