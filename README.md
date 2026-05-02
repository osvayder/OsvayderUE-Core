# Osvayder UE Core

Osvayder UE Core is an Unreal Engine editor-assistant plugin focused on safe AI-assisted development workflows: MCP tooling, editor-aware task execution, restart-survival, verification receipts, and bounded automation for UE projects.

This directory is a local public-repository staging tree. It is not yet a published GitHub repository and should be reviewed before release.

## Current Package Shape

The staged plugin remains under `Plugins/UnrealClaude/` for compatibility with the existing Unreal module and descriptor names. User-facing branding is `Osvayder UE`; a full module/package rename is intentionally deferred.

```text
OsvayderUE-Core/
  Plugins/
    UnrealClaude/
      Source/
      Config/
      Resources/
      UnrealClaude.uplugin
      LICENSE
  Docs/
  Examples/
```

## Highlights

- Unreal Engine 5.7 editor plugin with HTTP/MCP tool surface.
- Codex/Claude-oriented workflow orchestration for bounded code-assist tasks.
- Restart-survival flows for reflected C++ and editor restart cases.
- Verification-first closeout discipline for build, runtime, and manual-proof states.
- Safety-oriented handling of session, auth, logs, and local artifacts.

## Status

- Version staged from plugin descriptor: `1.4.1`.
- Current maturity: beta / OSS extraction baseline.
- Tested public export status: safety-audited staging tree only; clean GitHub release and fresh package build are still pending.

## Quick Start

See `Docs/Installation.md` and `Docs/QuickStart.md`.

## License

The core plugin is staged under MIT. Some bundled subcomponents retain their own license files; review `Plugins/UnrealClaude/Resources/mcp-bridge/LICENSE` before public release.

