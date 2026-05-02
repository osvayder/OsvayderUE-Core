# Installation

## Requirements

- Primary target: Unreal Engine 5.7.
- Planned validation: Unreal Engine 5.6 and broader clean-clone packaging checks.
- A C++ Unreal project.
- Node.js 18 or newer if using the bundled MCP bridge.
- A provider CLI or MCP-compatible client configured outside the repository when using AI-assisted workflows.

## Compatibility

| Unreal Engine version | Status |
| --- | --- |
| 5.7 | Primary target |
| 5.6 | Planned validation |
| 5.5 | Not yet validated |

## Plugin Install

1. Copy `Plugins/UnrealClaude/` into your Unreal project `Plugins/` directory.
2. Open or regenerate project files.
3. Build the editor target for your project.
4. Enable the plugin if Unreal does not enable it automatically.
5. Open the Osvayder UE editor UI from the plugin's registered menu/toolbar entry.

## MCP Bridge

The bridge entry point is staged at `Plugins/UnrealClaude/Resources/mcp-bridge/index.js`. Install dependencies from that directory with `npm install` before running MCP bridge tests or local MCP integration.

Do not commit local `.env`, auth token, or session files.
