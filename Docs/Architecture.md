# Architecture

Osvayder UE Core is staged as an Unreal Engine editor plugin plus a local MCP bridge.

## Main Areas

- `Plugins/UnrealClaude/Source/UnrealClaude/`: Unreal editor module, UI, orchestration, MCP tools, and tests.
- `Plugins/UnrealClaude/Config/`: plugin packaging filters.
- `Plugins/UnrealClaude/Resources/`: icon, MCP bridge, and context examples.
- `Plugins/UnrealClaude/Resources/mcp-bridge/`: Node-based MCP bridge source and tests.

## Workflow Model

The plugin favors evidence-based task execution:

- classify requested work before mutation;
- prefer bounded project writes;
- require build/runtime/manual-proof states where applicable;
- preserve restart-survival state when reflected C++ changes require editor restart;
- report truthful terminal status rather than forcing green closeout.

## Compatibility Note

The current module and plugin descriptor names remain `UnrealClaude`; user-facing docs and descriptor text use `Osvayder UE`. See `Docs/BrandingCompatibility.md`.
