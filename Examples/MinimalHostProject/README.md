# Minimal Host Project

This folder documents the intended public example location for a clean Unreal host setup. It does not currently contain generated Unreal project files.

## Verified

- The clean-host install pattern was verified on Windows with Unreal Engine 5.7.
- The verified path copied `Plugins/OsvayderUE/` into a fresh C++ host project's `Plugins/` directory.
- The host editor target built successfully.
- The editor commandlet reached the expected plugin-load and quit markers.
- The MCP bridge install and test flow passed from `Plugins/OsvayderUE/Resources/mcp-bridge/`.

## Known Caveats

- Compatibility outside Unreal Engine 5.7 is not currently claimed.
- Linux and macOS are not currently claimed for the public beta.
- The commandlet launch required forced termination after the quit marker because engine background activity kept the process alive.
- This folder is a placeholder until a tiny generated-state-free host project can be added.

## How to Reproduce

1. Create a fresh C++ Unreal Engine 5.7 project outside this repository.
2. Copy `Plugins/OsvayderUE/` into the host project's `Plugins/` directory.
3. Regenerate project files.
4. Build the host editor target.
5. Launch Unreal Editor and confirm the plugin appears.
6. From `Plugins/OsvayderUE/Resources/mcp-bridge/`, run:

```powershell
npm install
npm test
```

Use [Docs/Installation.md](../../Docs/Installation.md) and [Docs/Verification.md](../../Docs/Verification.md) as the source of truth for the current public setup and validation status.
