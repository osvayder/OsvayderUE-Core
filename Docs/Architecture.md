# Architecture

Osvayder UE Core is staged as an Unreal Engine editor plugin plus a local MCP bridge.

## Main Areas

- `Plugins/OsvayderUE/Source/OsvayderUE/`: Unreal editor module, UI, orchestration, MCP tools, and tests.
- `Plugins/OsvayderUE/Config/`: plugin packaging filters.
- `Plugins/OsvayderUE/Resources/`: icon, MCP bridge, and context examples.
- `Plugins/OsvayderUE/Resources/mcp-bridge/`: Node-based MCP bridge source and tests.

## Workflow Model

The plugin favors evidence-based task execution:

- classify requested work before mutation;
- prefer bounded project writes;
- require build/runtime/manual-proof states where applicable;
- preserve restart-survival state when reflected C++ changes require editor restart;
- report truthful terminal status rather than forcing green closeout.

## Current Proof Baseline

The accepted 2026-05-05 Packet FZ proof covers the current Osvayder UE product shape: main and Poligon editor builds, six script/restart-survival tests, visible/manual emulator receipt, and zero forbidden old-literal hits in fresh artifacts. See `Docs/Verification.md`.

## Compatibility Note

The current module and plugin descriptor names are `OsvayderUE`; user-facing docs and descriptor text use `Osvayder UE`. See `Docs/BrandingCompatibility.md`.
