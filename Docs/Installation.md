# Installation

## Requirements

- Unreal Engine 5.7.
- A C++ Unreal project.
- Node.js 18 or newer if using the bundled MCP bridge.
- A provider CLI or client configured outside the repository when using AI-assisted workflows.

## Plugin Install

1. Copy `Plugins/UnrealClaude/` into your Unreal project `Plugins/` directory.
2. Open or regenerate project files.
3. Build the editor target for your project.
4. Enable the plugin if Unreal does not enable it automatically.
5. Open the Osvayder UE editor UI from the plugin's registered menu/toolbar entry.

## MCP Bridge

The bridge entry point is staged at `Plugins/UnrealClaude/Resources/mcp-bridge/index.js`. Install dependencies from that directory with `npm install` before running MCP bridge tests or local MCP integration.

Do not commit local `.env`, auth token, or session files.

