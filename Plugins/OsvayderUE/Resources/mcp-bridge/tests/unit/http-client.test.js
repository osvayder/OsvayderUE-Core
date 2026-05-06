import { describe, it, expect, vi, beforeEach } from "vitest";
import {
  fetchWithTimeout,
  fetchUnrealTools,
  executeUnrealTool,
  executeUnrealToolAsync,
  checkUnrealConnection,
  formatToolResponse,
  RETRY_DELAYS_MS,
  isRetryableStatus,
  isRetryableError,
  fetchWithRetry,
  MCP_AUTH_HEADER,
  buildMcpAuthHeaders,
  readMcpAuthTokenFile,
  resolveMcpAuthToken,
} from "../../lib.js";
import {
  installFetchMock,
  installFetchReject,
  mockResponse,
} from "../helpers/mock-fetch.js";
import {
  UNREAL_TOOLS_RESPONSE,
  UNREAL_STATUS_RESPONSE,
  TOOL_EXECUTE_SUCCESS,
  TOOL_EXECUTE_FAILURE,
  TOOL_EXECUTE_POLICY_DENIED,
  TOOL_EXECUTE_AUTH_DENIED,
} from "../helpers/fixtures.js";

const BASE_URL = "http://localhost:3000";
const TIMEOUT_MS = 5000;

beforeEach(() => {
  vi.unstubAllGlobals();
});

describe("MCP auth token helpers", () => {
  it("builds token headers only when a token is present", () => {
    expect(buildMcpAuthHeaders("abc123")).toEqual({ [MCP_AUTH_HEADER]: "abc123" });
    expect(buildMcpAuthHeaders("")).toEqual({});
    expect(buildMcpAuthHeaders(null)).toEqual({});
  });

  it("resolves token from environment before token file", () => {
    const fsMock = {
      readFileSync: vi.fn(() => JSON.stringify({ token: "file-token" })),
    };

    const token = resolveMcpAuthToken(
      { UNREAL_MCP_TOKEN: " env-token ", UNREAL_MCP_TOKEN_FILE: "ignored.json" },
      fsMock
    );

    expect(token).toBe("env-token");
    expect(fsMock.readFileSync).not.toHaveBeenCalled();
  });

  it("reads token from JSON handoff file", () => {
    const fsMock = {
      readFileSync: vi.fn(() => JSON.stringify({ token: "file-token" })),
    };

    expect(readMcpAuthTokenFile("token.json", fsMock)).toBe("file-token");
  });
});

// ─── fetchWithTimeout ────────────────────────────────────────────────

describe("fetchWithTimeout", () => {
  it("returns the fetch response on success", async () => {
    installFetchMock([{ pattern: /example/, body: { ok: true } }]);
    const res = await fetchWithTimeout("http://example.com", {}, 5000);
    expect(res.ok).toBe(true);
    const json = await res.json();
    expect(json).toEqual({ ok: true });
  });

  it("aborts when the timeout elapses", async () => {
    vi.useFakeTimers();
    // fetch that never resolves until abort
    const spy = vi.fn((url, opts) => {
      return new Promise((resolve, reject) => {
        opts.signal.addEventListener("abort", () => {
          const err = new Error("The operation was aborted");
          err.name = "AbortError";
          reject(err);
        });
      });
    });
    vi.stubGlobal("fetch", spy);

    const promise = fetchWithTimeout("http://example.com", {}, 100);
    vi.advanceTimersByTime(101);

    await expect(promise).rejects.toThrow("aborted");
    vi.useRealTimers();
  });

  it("clears the timeout on success", async () => {
    const clearSpy = vi.spyOn(globalThis, "clearTimeout");
    installFetchMock([{ pattern: /example/, body: {} }]);
    await fetchWithTimeout("http://example.com", {}, 5000);
    expect(clearSpy).toHaveBeenCalled();
  });

  it("clears the timeout on fetch error", async () => {
    const clearSpy = vi.spyOn(globalThis, "clearTimeout");
    installFetchReject(new Error("network down"));
    await expect(fetchWithTimeout("http://example.com", {}, 5000)).rejects.toThrow("network down");
    expect(clearSpy).toHaveBeenCalled();
  });
});

// ─── fetchUnrealTools ────────────────────────────────────────────────

describe("fetchUnrealTools", () => {
  it("returns parsed tools array on success", async () => {
    installFetchMock([
      { pattern: "/mcp/tools", body: UNREAL_TOOLS_RESPONSE },
    ]);
    const tools = await fetchUnrealTools(BASE_URL, TIMEOUT_MS);
    expect(tools).toHaveLength(UNREAL_TOOLS_RESPONSE.tools.length);
    expect(tools[0].name).toBe("spawn_actor");
  });

  it("returns empty array on HTTP error", async () => {
    installFetchMock([
      { pattern: "/mcp/tools", body: {}, response: { status: 500, statusText: "Internal Server Error" } },
    ]);
    const tools = await fetchUnrealTools(BASE_URL, TIMEOUT_MS);
    expect(tools).toEqual([]);
  });

  it("returns empty array on network error", async () => {
    installFetchReject(new Error("ECONNREFUSED"));
    // Retry logic (1s→2s→4s backoff) triggers for network errors; use fake
    // timers to collapse real-time delays.
    vi.useFakeTimers();
    const promise = fetchUnrealTools(BASE_URL, TIMEOUT_MS);
    await vi.runAllTimersAsync();
    const tools = await promise;
    vi.useRealTimers();
    expect(tools).toEqual([]);
  });

  it("returns empty array on abort/timeout", async () => {
    const abortErr = new Error("The operation was aborted");
    abortErr.name = "AbortError";
    installFetchReject(abortErr);
    // Timeout aborts (without __userAbort) are treated as retryable; use fake
    // timers to collapse the backoff window.
    vi.useFakeTimers();
    const promise = fetchUnrealTools(BASE_URL, TIMEOUT_MS);
    await vi.runAllTimersAsync();
    const tools = await promise;
    vi.useRealTimers();
    expect(tools).toEqual([]);
  });

  it("calls the correct URL", async () => {
    const spy = installFetchMock([
      { pattern: "/mcp/tools", body: { tools: [] } },
    ]);
    await fetchUnrealTools(BASE_URL, TIMEOUT_MS);
    expect(spy).toHaveBeenCalled();
    const calledUrl = spy.mock.calls[0][0];
    expect(calledUrl).toBe(`${BASE_URL}/mcp/tools`);
  });

  it("sends MCP auth token header when configured", async () => {
    const spy = installFetchMock([
      { pattern: "/mcp/tools", body: { tools: [] } },
    ]);

    await fetchUnrealTools(BASE_URL, TIMEOUT_MS, { authToken: "token-for-test" });

    const [, opts] = spy.mock.calls[0];
    expect(opts.headers[MCP_AUTH_HEADER]).toBe("token-for-test");
  });
});

// ─── executeUnrealTool ───────────────────────────────────────────────

describe("executeUnrealTool", () => {
  it("sends a POST with JSON body", async () => {
    const spy = installFetchMock([
      { pattern: "/mcp/tool/spawn_actor", body: TOOL_EXECUTE_SUCCESS },
    ]);
    await executeUnrealTool(BASE_URL, TIMEOUT_MS, "spawn_actor", { class_name: "BP_Enemy" });
    const [, opts] = spy.mock.calls[0];
    expect(opts.method).toBe("POST");
    expect(opts.headers["Content-Type"]).toBe("application/json");
    expect(JSON.parse(opts.body)).toEqual({ class_name: "BP_Enemy" });
  });

  it("sends MCP auth token header with tool execution", async () => {
    const spy = installFetchMock([
      { pattern: "/mcp/tool/spawn_actor", body: TOOL_EXECUTE_SUCCESS },
    ]);

    await executeUnrealTool(BASE_URL, TIMEOUT_MS, "spawn_actor", {}, { authToken: "token-for-test" });

    const [, opts] = spy.mock.calls[0];
    expect(opts.headers[MCP_AUTH_HEADER]).toBe("token-for-test");
  });

  it("calls the correct URL with tool name", async () => {
    const spy = installFetchMock([
      { pattern: "/mcp/tool/get_actors", body: TOOL_EXECUTE_SUCCESS },
    ]);
    await executeUnrealTool(BASE_URL, TIMEOUT_MS, "get_actors", {});
    expect(spy.mock.calls[0][0]).toBe(`${BASE_URL}/mcp/tool/get_actors`);
  });

  it("returns the parsed JSON response on success", async () => {
    installFetchMock([
      { pattern: "/mcp/tool/spawn_actor", body: TOOL_EXECUTE_SUCCESS },
    ]);
    const result = await executeUnrealTool(BASE_URL, TIMEOUT_MS, "spawn_actor", {});
    expect(result.success).toBe(true);
    expect(result.data.actorName).toBe("BP_Enemy_42");
  });

  it("returns structured Unreal failure data without collapsing it", async () => {
    installFetchMock([
      {
        pattern: "/mcp/tool/spawn_actor",
        body: TOOL_EXECUTE_POLICY_DENIED,
        response: { status: 400, statusText: "Bad Request" },
      },
    ]);

    const result = await executeUnrealTool(BASE_URL, TIMEOUT_MS, "spawn_actor", {});
    expect(result.success).toBe(false);
    expect(result.data.result_type).toBe("policy_denied");
    expect(result.data.policy_denied_contract.policy_rule_id).toBe(
      "workspace_write_project.broad_authoring_mutation_surface_denied"
    );
  });

  it("preserves structured MCP auth denial data", async () => {
    installFetchMock([
      {
        pattern: "/mcp/tool/spawn_actor",
        body: TOOL_EXECUTE_AUTH_DENIED,
        response: { status: 401, statusText: "Unauthorized" },
      },
    ]);

    const result = await executeUnrealTool(
      BASE_URL,
      TIMEOUT_MS,
      "spawn_actor",
      {},
      { authToken: "bad-token" }
    );

    expect(result.success).toBe(false);
    expect(result.data.result_type).toBe("auth_denied");
    expect(result.data.denial_reason).toBe("invalid_token");
    expect(result.data.token_value_redacted).toBe(true);
  });

  it("returns error object on network failure", async () => {
    installFetchReject(new Error("ECONNREFUSED"));
    // Retry logic triggers for network errors; fast-forward through backoff.
    vi.useFakeTimers();
    const promise = executeUnrealTool(BASE_URL, TIMEOUT_MS, "spawn_actor", {});
    await vi.runAllTimersAsync();
    const result = await promise;
    vi.useRealTimers();
    expect(result.success).toBe(false);
    expect(result.message).toContain("ECONNREFUSED");
  });

  it("includes timeout ms in abort error message", async () => {
    const abortErr = new Error("The operation was aborted");
    abortErr.name = "AbortError";
    installFetchReject(abortErr);
    // Timeout aborts are retryable; fast-forward through backoff.
    vi.useFakeTimers();
    const promise = executeUnrealTool(BASE_URL, TIMEOUT_MS, "spawn_actor", {});
    await vi.runAllTimersAsync();
    const result = await promise;
    vi.useRealTimers();
    expect(result.success).toBe(false);
    expect(result.message).toContain(`${TIMEOUT_MS}ms`);
  });

  it("sends empty object when args is null", async () => {
    const spy = installFetchMock([
      { pattern: "/mcp/tool/test", body: TOOL_EXECUTE_SUCCESS },
    ]);
    await executeUnrealTool(BASE_URL, TIMEOUT_MS, "test", null);
    const body = JSON.parse(spy.mock.calls[0][1].body);
    expect(body).toEqual({});
  });
});

// ─── checkUnrealConnection ───────────────────────────────────────────

describe("executeUnrealToolAsync auth", () => {
  it("sends MCP auth token header to async task endpoints", async () => {
    const spy = vi.fn(async (url) => {
      if (url.includes("/mcp/tool/task_submit")) {
        return mockResponse({ success: true, data: { task_id: "task-1" } });
      }
      if (url.includes("/mcp/tool/task_status")) {
        return mockResponse({ success: true, data: { status: "completed", progress: 1, total: 1 } });
      }
      if (url.includes("/mcp/tool/task_result")) {
        return mockResponse(TOOL_EXECUTE_SUCCESS);
      }
      return mockResponse({ success: false, message: "not found" }, { status: 404, statusText: "Not Found" });
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(BASE_URL, TIMEOUT_MS, "spawn_actor", {}, {
      authToken: "token-for-test",
      pollIntervalMs: 0,
      asyncTimeoutMs: 1000,
    });

    expect(result.success).toBe(true);
    expect(spy.mock.calls.map(([url]) => url)).toEqual([
      `${BASE_URL}/mcp/tool/task_submit`,
      `${BASE_URL}/mcp/tool/task_status`,
      `${BASE_URL}/mcp/tool/task_result`,
    ]);
    for (const [, opts] of spy.mock.calls) {
      expect(opts.headers[MCP_AUTH_HEADER]).toBe("token-for-test");
    }
  });
});

describe("formatToolResponse failure data", () => {
  it("includes structured failure data in the MCP error text", () => {
    const result = formatToolResponse("spawn_actor", TOOL_EXECUTE_POLICY_DENIED, null);
    const text = result.content[0].text;

    expect(result.isError).toBe(true);
    expect(text).toContain("Error: Policy denied");
    expect(text).toContain("\"result_type\":\"policy_denied\"");
    expect(text).toContain("\"policy_rule_id\":\"workspace_write_project.broad_authoring_mutation_surface_denied\"");
    expect(text).toContain("\"skipped_reason\":\"policy_denied\"");
  });

  it("includes structured auth denial data without token values", () => {
    const result = formatToolResponse("spawn_actor", TOOL_EXECUTE_AUTH_DENIED, null);
    const text = result.content[0].text;

    expect(result.isError).toBe(true);
    expect(text).toContain("Error: MCP authentication failed");
    expect(text).toContain("\"result_type\":\"auth_denied\"");
    expect(text).toContain("\"denial_reason\":\"invalid_token\"");
    expect(text).toContain("\"token_value_redacted\":true");
    expect(text).not.toContain("bad-token");
  });
});

describe("checkUnrealConnection", () => {
  it("returns connected:true with spread data on success", async () => {
    installFetchMock([
      { pattern: "/mcp/status", body: UNREAL_STATUS_RESPONSE },
    ]);
    const status = await checkUnrealConnection(BASE_URL, TIMEOUT_MS);
    expect(status.connected).toBe(true);
    expect(status.projectName).toBe("MyGame");
    expect(status.engineVersion).toBe("5.7.1");
  });

  it("returns connected:false with HTTP status reason on non-ok", async () => {
    installFetchMock([
      { pattern: "/mcp/status", body: {}, response: { status: 503, statusText: "Service Unavailable" } },
    ]);
    const status = await checkUnrealConnection(BASE_URL, TIMEOUT_MS);
    expect(status.connected).toBe(false);
    expect(status.reason).toBe("HTTP 503");
  });

  it("returns connected:false with timeout reason on abort", async () => {
    const abortErr = new Error("The operation was aborted");
    abortErr.name = "AbortError";
    installFetchReject(abortErr);
    const status = await checkUnrealConnection(BASE_URL, TIMEOUT_MS);
    expect(status.connected).toBe(false);
    expect(status.reason).toBe("timeout");
  });

  it("returns connected:false with error message on network error", async () => {
    installFetchReject(new Error("ECONNREFUSED"));
    const status = await checkUnrealConnection(BASE_URL, TIMEOUT_MS);
    expect(status.connected).toBe(false);
    expect(status.reason).toBe("ECONNREFUSED");
  });
});

// ─── retry policy helpers (SP1-S3) ───────────────────────────────────

describe("retry policy helpers", () => {
  it("RETRY_DELAYS_MS is the 1s/2s/4s sequence", () => {
    expect(RETRY_DELAYS_MS).toEqual([1000, 2000, 4000]);
  });

  it("isRetryableStatus accepts 502/503/504 and rejects everything else", () => {
    expect(isRetryableStatus(502)).toBe(true);
    expect(isRetryableStatus(503)).toBe(true);
    expect(isRetryableStatus(504)).toBe(true);
    // 4xx → non-retryable
    expect(isRetryableStatus(400)).toBe(false);
    expect(isRetryableStatus(404)).toBe(false);
    expect(isRetryableStatus(429)).toBe(false);
    // Other 5xx → non-retryable (conservative scope)
    expect(isRetryableStatus(500)).toBe(false);
    expect(isRetryableStatus(501)).toBe(false);
  });

  it("isRetryableError treats network errors and timeout aborts as retryable, user aborts as non-retryable", () => {
    expect(isRetryableError(new Error("ECONNREFUSED"))).toBe(true);
    expect(isRetryableError(new Error("DNS lookup failed"))).toBe(true);
    const timeoutAbort = new Error("The operation was aborted");
    timeoutAbort.name = "AbortError";
    expect(isRetryableError(timeoutAbort)).toBe(true);
    const userAbort = new Error("User aborted");
    userAbort.name = "AbortError";
    userAbort.__userAbort = true;
    expect(isRetryableError(userAbort)).toBe(false);
    expect(isRetryableError(null)).toBe(false);
  });
});

// ─── SP1-S3 retry + backoff behaviour ────────────────────────────────

describe("fetchUnrealTools retry (SP1-S3)", () => {
  it("5xx-then-success: retries 503 then returns parsed tools", async () => {
    let call = 0;
    const spy = vi.fn(async (url) => {
      call++;
      if (call === 1) {
        return mockResponse({}, { status: 503, statusText: "Service Unavailable" });
      }
      return mockResponse(UNREAL_TOOLS_RESPONSE);
    });
    vi.stubGlobal("fetch", spy);

    vi.useFakeTimers();
    const promise = fetchUnrealTools(BASE_URL, TIMEOUT_MS);
    await vi.runAllTimersAsync();
    const tools = await promise;
    vi.useRealTimers();

    expect(tools).toHaveLength(UNREAL_TOOLS_RESPONSE.tools.length);
    expect(spy).toHaveBeenCalledTimes(2);
  });

  it("5xx-persistent-final-error: retries exactly 3 times then gives up", async () => {
    const spy = vi.fn(async () =>
      mockResponse({}, { status: 502, statusText: "Bad Gateway" })
    );
    vi.stubGlobal("fetch", spy);

    vi.useFakeTimers();
    const failureSpy = vi.fn();
    const promise = fetchUnrealTools(BASE_URL, TIMEOUT_MS, { onFailure: failureSpy });
    await vi.runAllTimersAsync();
    const tools = await promise;
    vi.useRealTimers();

    expect(tools).toEqual([]);
    // 1 initial + 3 retries = 4 total attempts.
    expect(spy).toHaveBeenCalledTimes(4);
    expect(failureSpy).toHaveBeenCalledTimes(1);
  });

  it("4xx-immediate-fail: does not retry on 4xx", async () => {
    const spy = vi.fn(async () =>
      mockResponse({}, { status: 404, statusText: "Not Found" })
    );
    vi.stubGlobal("fetch", spy);

    const tools = await fetchUnrealTools(BASE_URL, TIMEOUT_MS);
    expect(tools).toEqual([]);
    expect(spy).toHaveBeenCalledTimes(1);
  });

  it("network-error-retried: retries network-level errors and recovers", async () => {
    let call = 0;
    const spy = vi.fn(async () => {
      call++;
      if (call <= 2) {
        throw new Error("ECONNREFUSED");
      }
      return mockResponse(UNREAL_TOOLS_RESPONSE);
    });
    vi.stubGlobal("fetch", spy);

    vi.useFakeTimers();
    const promise = fetchUnrealTools(BASE_URL, TIMEOUT_MS);
    await vi.runAllTimersAsync();
    const tools = await promise;
    vi.useRealTimers();

    expect(tools).toHaveLength(UNREAL_TOOLS_RESPONSE.tools.length);
    expect(spy).toHaveBeenCalledTimes(3);
  });
});

describe("executeUnrealTool retry (SP1-S3)", () => {
  it("retries 503 then returns the parsed success payload", async () => {
    let call = 0;
    const spy = vi.fn(async () => {
      call++;
      if (call === 1) {
        return mockResponse({}, { status: 503, statusText: "Service Unavailable" });
      }
      return mockResponse(TOOL_EXECUTE_SUCCESS);
    });
    vi.stubGlobal("fetch", spy);

    vi.useFakeTimers();
    const promise = executeUnrealTool(BASE_URL, TIMEOUT_MS, "spawn_actor", {});
    await vi.runAllTimersAsync();
    const result = await promise;
    vi.useRealTimers();

    expect(result.success).toBe(true);
    expect(spy).toHaveBeenCalledTimes(2);
  });

  it("does not retry 4xx responses (returns parsed body)", async () => {
    // A 4xx response still has a body the caller will parse (not retried).
    const spy = vi.fn(async () =>
      mockResponse({ success: false, message: "Bad request" }, { status: 400, statusText: "Bad Request" })
    );
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealTool(BASE_URL, TIMEOUT_MS, "spawn_actor", {});
    // The response body is returned verbatim.
    expect(result.success).toBe(false);
    expect(spy).toHaveBeenCalledTimes(1);
  });
});

// ─── SP1-S3 fetchWithRetry user-abort handling ───────────────────────

describe("fetchWithRetry user-abort", () => {
  it("stops retrying when userAbortSignal is aborted before the next attempt", async () => {
    const spy = vi.fn(async () => {
      throw new Error("ECONNREFUSED");
    });
    vi.stubGlobal("fetch", spy);

    const controller = new AbortController();
    controller.abort();

    await expect(
      fetchWithRetry("http://example.com/x", {}, 1000, { userAbortSignal: controller.signal })
    ).rejects.toMatchObject({ name: "AbortError" });
    // Should not attempt fetch at all — abort checked before the first try.
    expect(spy).toHaveBeenCalledTimes(0);
  });
});

// ─── SP1-S4 cache invalidation onFailure ─────────────────────────────

describe("fetchUnrealTools cache invalidation (SP1-S4)", () => {
  it("invokes onFailure with the underlying error after retries exhaust", async () => {
    const spy = vi.fn(async () =>
      mockResponse({}, { status: 504, statusText: "Gateway Timeout" })
    );
    vi.stubGlobal("fetch", spy);

    vi.useFakeTimers();
    const errors = [];
    const onFailure = vi.fn((err) => errors.push(err));
    const promise = fetchUnrealTools(BASE_URL, TIMEOUT_MS, { onFailure });
    await vi.runAllTimersAsync();
    await promise;
    vi.useRealTimers();

    expect(onFailure).toHaveBeenCalledTimes(1);
    expect(errors[0]).toBeInstanceOf(Error);
    expect(errors[0].message).toContain("504");
  });

  it("does not invoke onFailure on success", async () => {
    installFetchMock([{ pattern: "/mcp/tools", body: UNREAL_TOOLS_RESPONSE }]);
    const onFailure = vi.fn();
    const tools = await fetchUnrealTools(BASE_URL, TIMEOUT_MS, { onFailure });
    expect(tools.length).toBeGreaterThan(0);
    expect(onFailure).not.toHaveBeenCalled();
  });

  it("swallows onFailure callback exceptions so the fetch contract is preserved", async () => {
    installFetchReject(new Error("ECONNREFUSED"));
    vi.useFakeTimers();
    const onFailure = vi.fn(() => {
      throw new Error("callback blew up");
    });
    const promise = fetchUnrealTools(BASE_URL, TIMEOUT_MS, { onFailure });
    await vi.runAllTimersAsync();
    const tools = await promise;
    vi.useRealTimers();
    expect(tools).toEqual([]);
    expect(onFailure).toHaveBeenCalled();
  });

  // Explicit acceptance test: cache reset semantics driven by the onFailure
  // callback. Mirrors how index.js wires toolCache = { tools: [], timestamp: 0 }
  // on every ultimate fetch failure. See SP1-S4 [A1].
  it("tool-cache-invalidated-on-fetch-fail", async () => {
    // Simulate the index.js cache structure used by the ListTools handler.
    let toolCache = { tools: [{ name: "stale_tool" }], timestamp: Date.now() };

    // All attempts fail (503) → retries exhaust → onFailure fires → cache reset.
    const spy = vi.fn(async () =>
      mockResponse({}, { status: 503, statusText: "Service Unavailable" })
    );
    vi.stubGlobal("fetch", spy);

    vi.useFakeTimers();
    const onFailure = vi.fn(() => {
      toolCache = { tools: [], timestamp: 0 };
    });
    const promise = fetchUnrealTools(BASE_URL, TIMEOUT_MS, { onFailure });
    await vi.runAllTimersAsync();
    await promise;
    vi.useRealTimers();

    expect(onFailure).toHaveBeenCalledTimes(1);
    // Cache is reset — next list_tools call will re-fetch instead of returning
    // the stale entry.
    expect(toolCache.tools).toEqual([]);
    expect(toolCache.timestamp).toBe(0);
  });
});
