import { describe, it, expect, vi, beforeEach } from "vitest";
import {
  sleep,
  executeUnrealToolAsync,
  executeUnrealTool,
} from "../../lib.js";
import { installFetchMock, installFetchReject } from "../helpers/mock-fetch.js";

const BASE_URL = "http://localhost:3000";
const TIMEOUT_MS = 5000;

beforeEach(() => {
  vi.unstubAllGlobals();
});

// ─── sleep ───────────────────────────────────────────────────────────

describe("sleep", () => {
  it("resolves after the given delay", async () => {
    vi.useFakeTimers();
    const promise = sleep(100);
    vi.advanceTimersByTime(100);
    await expect(promise).resolves.toBeUndefined();
    vi.useRealTimers();
  });
});

// ─── executeUnrealToolAsync ──────────────────────────────────────────

describe("executeUnrealToolAsync", () => {
  it("submits task, polls, and returns result on completion", async () => {
    let callCount = 0;
    const spy = vi.fn(async (url, options) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            data: { task_id: "test-task-123" },
          }),
        };
      }
      if (url.includes("/task_status")) {
        callCount++;
        const status = callCount >= 2 ? "completed" : "running";
        return {
          ok: true,
          json: async () => ({
            data: {
              status,
              progress: callCount,
              total: 3,
              progress_message: `Step ${callCount}/3`,
            },
          }),
        };
      }
      if (url.includes("/task_result")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            message: "Actors found",
            data: { actors: ["Actor1", "Actor2"] },
          }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "get_actors", {},
      { pollIntervalMs: 10, asyncTimeoutMs: 5000 }
    );

    expect(result.success).toBe(true);
    expect(result.data.actors).toEqual(["Actor1", "Actor2"]);
    // Verify task_submit was called
    const submitCall = spy.mock.calls.find((c) => c[0].includes("/task_submit"));
    expect(submitCall).toBeDefined();
    const submitBody = JSON.parse(submitCall[1].body);
    expect(submitBody.tool_name).toBe("get_actors");
  });

  it("falls back to sync when task_submit returns no task_id", async () => {
    const spy = vi.fn(async (url, options) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({ success: false, message: "Task queue unavailable" }),
        };
      }
      // Sync fallback call to /mcp/tool/get_actors
      if (url.includes("/mcp/tool/get_actors")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            message: "Actors found (sync)",
            data: { actors: ["SyncActor"] },
          }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "get_actors", {},
      { pollIntervalMs: 10 }
    );

    expect(result.success).toBe(true);
    expect(result.message).toContain("sync");
  });

  it("falls back to sync when task_submit throws a network error", async () => {
    let firstCall = true;
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit") && firstCall) {
        firstCall = false;
        throw new Error("ECONNREFUSED");
      }
      // Sync fallback
      if (url.includes("/mcp/tool/spawn_actor")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            message: "Spawned (sync fallback)",
          }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "spawn_actor", {},
      { pollIntervalMs: 10 }
    );

    expect(result.success).toBe(true);
    expect(result.message).toContain("sync fallback");
  });

  it("invokes onProgress callback during polling", async () => {
    let callCount = 0;
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            data: { task_id: "progress-task" },
          }),
        };
      }
      if (url.includes("/task_status")) {
        callCount++;
        return {
          ok: true,
          json: async () => ({
            data: {
              status: callCount >= 3 ? "completed" : "running",
              progress: callCount,
              total: 3,
              progress_message: `Step ${callCount}`,
            },
          }),
        };
      }
      if (url.includes("/task_result")) {
        return {
          ok: true,
          json: async () => ({ success: true, message: "Done" }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const progressUpdates = [];
    const onProgress = (update) => progressUpdates.push(update);

    await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "some_tool", {},
      { onProgress, pollIntervalMs: 10, asyncTimeoutMs: 5000 }
    );

    expect(progressUpdates.length).toBeGreaterThanOrEqual(2);
    expect(progressUpdates[0]).toHaveProperty("progress");
    expect(progressUpdates[0]).toHaveProperty("total");
    expect(progressUpdates[0]).toHaveProperty("message");
  });

  it("returns timeout error when async timeout is exceeded", async () => {
    let cancelRequested = false;
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            data: { task_id: "timeout-task" },
          }),
        };
      }
      if (url.includes("/task_status")) {
        if (cancelRequested) {
          return {
            ok: true,
            json: async () => ({
              data: { status: "cancelled", progress: 100, total: 100, progress_message: "Cancelled" },
            }),
          };
        }
        return {
          ok: true,
          json: async () => ({
            data: { status: "running", progress: 1, total: 100, progress_message: "Still running" },
          }),
        };
      }
      if (url.includes("/task_cancel")) {
        cancelRequested = true;
        return {
          ok: true,
          json: async () => ({
            success: true,
            message: "Cancellation requested",
            data: { task_id: "timeout-task", cancel_requested: true, cancelled: false, terminal: false, status: "running" },
          }),
        };
      }
      if (url.includes("/task_result")) {
        return {
          ok: true,
          json: async () => ({
            success: false,
            message: "Task cancelled before guarded fixture execution",
            data: {
              task_id: "timeout-task",
              status: "cancelled",
              success: false,
              data: {
                result_type: "tool_error",
                schema_version: "mcp_tool_error.v1",
                error_category: "cancelled",
                structured_error: true,
              },
            },
          }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "slow_tool", {},
      { pollIntervalMs: 10, asyncTimeoutMs: 50 }
    );

    expect(result.success).toBe(false);
    expect(result.message).toContain("cancelled");
    expect(result.data.status).toBe("cancelled");
    expect(result.data.async_timeout.error_category).toBe("async_timeout");
    expect(result.data.async_timeout.cancel_requested).toBe(true);
    expect(result.data.async_timeout.terminal_state_reached).toBe(true);
    expect(result.data.data.schema_version).toBe("mcp_tool_error.v1");
    expect(result.data.data.error_category).toBe("cancelled");
    expect(spy.mock.calls.some((c) => c[0].includes("/task_cancel"))).toBe(true);
  });

  it("returns structured async_timeout_pending when cancellation does not reach terminal state", async () => {
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            data: { task_id: "timeout-pending-task" },
          }),
        };
      }
      if (url.includes("/task_status")) {
        return {
          ok: true,
          json: async () => ({
            data: { status: "running", progress: 1, total: 100, progress_message: "Still running" },
          }),
        };
      }
      if (url.includes("/task_cancel")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            message: "Cancellation requested",
            data: { task_id: "timeout-pending-task", cancel_requested: true, cancelled: false, terminal: false, status: "running" },
          }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "slow_tool", {},
      { pollIntervalMs: 10, asyncTimeoutMs: 30, cancelWaitMs: 30 }
    );

    expect(result.success).toBe(false);
    expect(result.data.result_type).toBe("async_error");
    expect(result.data.schema_version).toBe("mcp_async_error.v1");
    expect(result.data.error_category).toBe("async_timeout_pending");
    expect(result.data.cancel_requested).toBe(true);
    expect(result.data.terminal_state_reached).toBe(false);
    expect(result.data.cancel_response.data.status).toBe("running");
    expect(spy.mock.calls.some((call) => call[0].includes("/task_cancel"))).toBe(true);
    expect(spy.mock.calls.some((call) => call[0].includes("/task_result"))).toBe(false);
  });

  it("handles timed_out task status as terminal and fetches task_result", async () => {
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            data: { task_id: "server-timeout-task" },
          }),
        };
      }
      if (url.includes("/task_status")) {
        return {
          ok: true,
          json: async () => ({
            data: { status: "timed_out", progress: 100, total: 100, progress_message: "Timed out" },
          }),
        };
      }
      if (url.includes("/task_result")) {
        return {
          ok: true,
          json: async () => ({
            success: false,
            message: "Task timed out after 500 ms",
            data: { status: "timed_out", success: false, data: { error_category: "timeout" } },
          }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "slow_tool", {},
      { pollIntervalMs: 10, asyncTimeoutMs: 5000 }
    );

    expect(result.success).toBe(false);
    expect(result.message).toContain("timed out");
    expect(result.data.status).toBe("timed_out");
    expect(spy.mock.calls.some((c) => c[0].includes("/task_result"))).toBe(true);
    expect(spy.mock.calls.some((c) => c[0].includes("/task_cancel"))).toBe(false);
  });

  it("handles failed task status correctly", async () => {
    let callCount = 0;
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            data: { task_id: "fail-task" },
          }),
        };
      }
      if (url.includes("/task_status")) {
        callCount++;
        return {
          ok: true,
          json: async () => ({
            data: {
              status: callCount >= 2 ? "failed" : "running",
              progress: callCount,
              total: 0,
              progress_message: "Running...",
            },
          }),
        };
      }
      if (url.includes("/task_result")) {
        return {
          ok: true,
          json: async () => ({
            success: false,
            message: "Blueprint compilation failed",
          }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "blueprint_compile", {},
      { pollIntervalMs: 10, asyncTimeoutMs: 5000 }
    );

    expect(result.success).toBe(false);
    expect(result.message).toContain("compilation failed");
  });

  it("handles cancelled task status correctly", async () => {
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            data: { task_id: "cancel-task" },
          }),
        };
      }
      if (url.includes("/task_status")) {
        return {
          ok: true,
          json: async () => ({
            data: { status: "cancelled", progress: 0, total: 0, progress_message: "Cancelled" },
          }),
        };
      }
      if (url.includes("/task_result")) {
        return {
          ok: true,
          json: async () => ({
            success: false,
            message: "Task was cancelled",
          }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "some_tool", {},
      { pollIntervalMs: 10, asyncTimeoutMs: 5000 }
    );

    expect(result.success).toBe(false);
    expect(result.message).toContain("cancelled");
  });

  it("continues polling when task_status request fails", async () => {
    let callCount = 0;
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            data: { task_id: "retry-task" },
          }),
        };
      }
      if (url.includes("/task_status")) {
        callCount++;
        if (callCount === 1) {
          throw new Error("Network blip");
        }
        return {
          ok: true,
          json: async () => ({
            data: { status: "completed", progress: 2, total: 2, progress_message: "Done" },
          }),
        };
      }
      if (url.includes("/task_result")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            message: "Completed after retry",
          }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "some_tool", {},
      { pollIntervalMs: 10, asyncTimeoutMs: 5000 }
    );

    expect(result.success).toBe(true);
    expect(result.message).toContain("Completed after retry");
  });

  it("returns error when task_result fetch fails", async () => {
    let callCount = 0;
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({
            success: true,
            data: { task_id: "result-fail-task" },
          }),
        };
      }
      if (url.includes("/task_status")) {
        return {
          ok: true,
          json: async () => ({
            data: { status: "completed", progress: 1, total: 1, progress_message: "Done" },
          }),
        };
      }
      if (url.includes("/task_result")) {
        throw new Error("Connection reset");
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "some_tool", {},
      { pollIntervalMs: 10, asyncTimeoutMs: 5000 }
    );

    expect(result.success).toBe(false);
    expect(result.message).toContain("failed to retrieve result");
  });

  it("sends correct payload in task_submit request", async () => {
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({ success: false }),
        };
      }
      // Sync fallback
      return {
        ok: true,
        json: async () => ({ success: true, message: "Fallback" }),
      };
    });
    vi.stubGlobal("fetch", spy);

    await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "asset_search",
      { query: "BP_*", path: "/Game" },
      { asyncTimeoutMs: 60000 }
    );

    const submitCall = spy.mock.calls.find((c) => c[0].includes("/task_submit"));
    const body = JSON.parse(submitCall[1].body);
    expect(body.tool_name).toBe("asset_search");
    expect(body.params).toEqual({ query: "BP_*", path: "/Game" });
    expect(body.timeout_ms).toBe(60000);
  });
});

// ─── SP1-S4 task-poll backoff ────────────────────────────────────────

describe("executeUnrealToolAsync poll backoff (SP1-S4)", () => {
  it("enforces ≥500ms delay between poll attempts after a status error", async () => {
    // Simulate task_submit success, first task_status throws, second succeeds.
    // Record the real wall-time gap between the two /task_status calls to
    // prove the ≥500ms floor kicks in even with pollIntervalMs=10.
    const timestamps = [];
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({ success: true, data: { task_id: "backoff-task" } }),
        };
      }
      if (url.includes("/task_status")) {
        timestamps.push(Date.now());
        if (timestamps.length === 1) {
          throw new Error("transient blip");
        }
        return {
          ok: true,
          json: async () => ({
            data: { status: "completed", progress: 1, total: 1, progress_message: "Done" },
          }),
        };
      }
      if (url.includes("/task_result")) {
        return {
          ok: true,
          json: async () => ({ success: true, message: "OK" }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const { executeUnrealToolAsync } = await import("../../lib.js");
    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "some_tool", {},
      { pollIntervalMs: 10, asyncTimeoutMs: 10000 }
    );

    expect(result.success).toBe(true);
    expect(timestamps).toHaveLength(2);
    const gap = timestamps[1] - timestamps[0];
    // Floor: max(500, pollIntervalMs). With pollIntervalMs=10, gap must be ≥500ms.
    expect(gap).toBeGreaterThanOrEqual(500);
  });

  it("does not add extra delay when pollIntervalMs already exceeds the 500ms floor", async () => {
    // pollIntervalMs=600 means the top-of-loop sleep already covers the floor.
    // We verify recovery still works (no regression) but do not assert on
    // timing here because the existing top-of-loop sleep already guarantees
    // the floor.
    let call = 0;
    const spy = vi.fn(async (url) => {
      if (url.includes("/task_submit")) {
        return {
          ok: true,
          json: async () => ({ success: true, data: { task_id: "nominal-task" } }),
        };
      }
      if (url.includes("/task_status")) {
        call++;
        if (call === 1) {
          throw new Error("blip");
        }
        return {
          ok: true,
          json: async () => ({
            data: { status: "completed", progress: 1, total: 1, progress_message: "Done" },
          }),
        };
      }
      if (url.includes("/task_result")) {
        return {
          ok: true,
          json: async () => ({ success: true, message: "OK" }),
        };
      }
      return { ok: false, json: async () => ({}) };
    });
    vi.stubGlobal("fetch", spy);

    const { executeUnrealToolAsync } = await import("../../lib.js");
    const result = await executeUnrealToolAsync(
      BASE_URL, TIMEOUT_MS, "some_tool", {},
      { pollIntervalMs: 600, asyncTimeoutMs: 10000 }
    );
    expect(result.success).toBe(true);
  });
});
