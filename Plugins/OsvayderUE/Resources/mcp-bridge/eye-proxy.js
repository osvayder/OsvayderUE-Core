/**
 * OsvayderEye Proxy — MCP client that connects to OsvayderEye Python server.
 * Spawns Python as child process and communicates via MCP stdio protocol.
 *
 * Environment Variables:
 *   EYE_SERVER_PATH - Path to OsvayderEye server.py
 *   EYE_PYTHON_PATH - Path to Python executable (default: python)
 */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { log } from "./lib.js";

let eyeClient = null;
let eyeTransport = null;
let eyeTools = [];
let eyeConnected = false;
let eyeError = null;
let initAttempted = false;

/**
 * Initialize eye proxy — spawn Python process and connect MCP client.
 * Non-fatal: if Python or server.py not found, sets eyeConnected=false.
 */
export async function initEyeProxy(pythonPath, serverScript) {
  if (!serverScript) {
    eyeError = "EYE_SERVER_PATH not configured";
    log.info("Eye proxy skipped", { reason: eyeError });
    return;
  }

  initAttempted = true;

  try {
    eyeTransport = new StdioClientTransport({
      command: pythonPath || "python",
      args: [serverScript],
      env: { ...process.env, PYTHONUNBUFFERED: "1" },
    });

    eyeClient = new Client(
      { name: "eye-proxy", version: "1.0.0" },
      { capabilities: {} }
    );

    await eyeClient.connect(eyeTransport);

    const result = await eyeClient.listTools();
    eyeTools = result.tools || [];
    eyeConnected = true;
    eyeError = null;

    log.info("Eye proxy connected", { toolCount: eyeTools.length });
  } catch (err) {
    eyeConnected = false;
    eyeError = err.message;
    eyeClient = null;
    eyeTransport = null;
    eyeTools = [];
    log.info("Eye proxy not available", { reason: err.message });
  }
}

/**
 * Get cached eye tools list. Returns empty array if not connected.
 */
export function listEyeTools() {
  return eyeTools;
}

/**
 * Call an eye tool by name. Attempts one reconnect on failure.
 */
export async function callEyeTool(name, args) {
  if (!eyeConnected || !eyeClient) {
    // Attempt lazy reconnect once
    if (initAttempted && !eyeConnected) {
      const serverScript = process.env.EYE_SERVER_PATH;
      const pythonPath = process.env.EYE_PYTHON_PATH || "python";
      if (serverScript) {
        log.info("Eye proxy reconnecting...");
        await initEyeProxy(pythonPath, serverScript);
      }
    }
    if (!eyeConnected) {
      throw new Error("OsvayderEye not connected");
    }
  }

  try {
    const result = await eyeClient.callTool({ name, arguments: args });
    return result;
  } catch (err) {
    // Mark as disconnected for potential reconnect on next call
    eyeConnected = false;
    eyeError = err.message;
    throw err;
  }
}

/**
 * Get eye proxy status for unified status tool.
 */
export function getEyeStatus() {
  return {
    connected: eyeConnected,
    toolCount: eyeTools.length,
    error: eyeError,
  };
}

/**
 * Close eye proxy — kill child process cleanly.
 */
export async function closeEyeProxy() {
  if (eyeClient) {
    try {
      await eyeClient.close();
    } catch (err) {
      // Ignore close errors
    }
    eyeClient = null;
  }
  if (eyeTransport) {
    try {
      await eyeTransport.close();
    } catch (err) {
      // Ignore close errors
    }
    eyeTransport = null;
  }
  eyeConnected = false;
  eyeTools = [];
  log.info("Eye proxy closed");
}
