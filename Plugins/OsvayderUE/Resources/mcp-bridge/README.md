# Osvayder UE MCP Bridge

This Node bridge exposes Osvayder UE's local Unreal Editor HTTP endpoint as a Model Context Protocol server. It is intended for MCP-compatible clients and Codex-style AI-assisted Unreal workflows.

## Requirements

- Node.js 18 or newer.
- Unreal Editor running with the Osvayder UE plugin loaded.
- Osvayder UE's local HTTP endpoint available at `http://localhost:3000` unless overridden with `UNREAL_MCP_URL`.

## Install And Test

From this directory:

```bash
npm install
npm test
```

The public repository CI runs the same test command with `npm ci`.

## MCP Client Configuration

Copy `Plugins/OsvayderUE/.mcp.example.json` to your MCP client's local configuration location, then point it at the bridge entry point in this plugin:

```json
{
  "mcpServers": {
    "osvayderue": {
      "command": "node",
      "args": ["/path/to/OsvayderUE/Resources/mcp-bridge/index.js"],
      "env": {
        "UNREAL_MCP_URL": "http://localhost:3000"
      }
    }
  }
}
```

Do not commit local `.env`, token, auth, or session files. If authentication is enabled, keep token transport in local machine configuration or the plugin-written runtime file under the host project's `Saved/OsvayderUE/` directory.

For local bridge environment defaults, copy `.env.example` to `.env` and adjust values on your machine. The public repo intentionally tracks only examples.

## Tool Surface

The bridge forwards MCP tool calls to the plugin's Unreal-aware endpoints. Available families include:

- editor status and context discovery;
- level actor queries and mutation;
- asset search, dependency, and referencer queries;
- Blueprint and Animation Blueprint inspection/mutation;
- character, material, Enhanced Input, GAS, Niagara, AI, Sequencer, and multiplayer helpers;
- async task queue and verification/reporting tools.

The Unreal plugin owns the actual editor-side validation and safety policy. This bridge is a transport layer, not a standalone game-generation system.

## Repository

Public repo: `https://github.com/osvayder/OsvayderUE-Core`

## Attribution

The bridge includes MIT-licensed upstream work by Natali Caggiano. See `LICENSE` in this directory for the retained upstream notice.
