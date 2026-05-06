# Installation

## Requirements

- Unreal Engine 5.7 for the currently verified Windows path.
- A C++ Unreal project.
- Node.js 18 or newer for the bundled MCP bridge.
- A provider CLI or MCP-compatible client configured outside this repository for AI-assisted workflows.

## Compatibility

| Target | Status |
| --- | --- |
| Unreal Engine 5.7, Windows | Verified on a clean C++ host project |
| Other Unreal Engine versions | Not currently claimed |
| Linux / macOS | Not currently claimed |

## Verified

- Clean-host install on Windows with Unreal Engine 5.7.
- Host editor target build after copying `Plugins/OsvayderUE/`.
- Editor commandlet launch through plugin-load and quit markers.
- MCP bridge dependency install and test run from `Plugins/OsvayderUE/Resources/mcp-bridge/`.

## Known Caveats

- Compatibility outside Unreal Engine 5.7 is not currently claimed.
- Linux and macOS are not currently claimed for the public beta.
- The verified commandlet launch required forced termination after the quit marker because engine background activity kept the process alive.

## Plugin Install

Use this flow for a clean-host install:

1. Create a fresh C++ Unreal Engine 5.7 C++ project.
2. Copy `Plugins/OsvayderUE/` from this repository into the host project's `Plugins/` directory.
3. Regenerate project files for the host project.
4. Build the host editor target in `Development Editor` configuration.
5. Launch Unreal Editor.
6. Enable the Osvayder UE plugin if Unreal does not enable it automatically.
7. Open the Osvayder UE editor UI from the plugin menu or toolbar entry.

The public baseline was verified on Windows with Unreal Engine 5.7 using this clean-host flow. The commandlet launch reached the expected plugin-load and quit markers, then required forced termination because engine background activity kept the process alive.

## MCP Bridge

The bridge entry point is `Plugins/OsvayderUE/Resources/mcp-bridge/index.js`.

Run these commands before local MCP integration or release packaging:

```powershell
cd Plugins/OsvayderUE/Resources/mcp-bridge
npm install
npm test
```

Do not commit local `.env`, auth token, generated session, `Saved/`, `Intermediate/`, or `Binaries/` files.

## Minimal Host Example

[Examples/MinimalHostProject/README.md](../Examples/MinimalHostProject/README.md) documents the current minimal host-project pattern. It is intentionally a placeholder until a tiny generated-state-free Unreal example can be added.

## Verification

See [Docs/Verification.md](Verification.md) for the current verified baseline, caveats, and reproduction checklist.
