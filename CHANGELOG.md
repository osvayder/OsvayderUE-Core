# Changelog

All notable public changes should be documented here.

## 1.1 - 2026-05-06

Versioning starts here for the independent Osvayder UE Core public line.

- Made Codex CLI the public-beta runtime default and documented Claude CLI as a legacy/experimental compatibility path.
- Tightened public compatibility claims to Unreal Engine 5.7 on Win64 until additional platforms or engine versions are validated.
- Created the initial OSS extraction staging baseline for `OsvayderUE-Core`.
- Updated the public plugin tree to the accepted `Osvayder UE` release shape under `Plugins/OsvayderUE/`.
- Clarified license provenance: upstream UnrealClaude `1.4.0` attribution is preserved, and Osvayder-authored work starts the independent public line at `1.1`.
- Reworked the README reader flow to define `Core`, source-beta status, Codex-first runtime direction, and UnrealClaude origin before comparison claims.
- Updated product-facing language from "assistant" to "development agent" where it describes the product category.
- Refreshed source with current storage migration, animation intake/retarget/preflight, mechanic preflight, and Slate UI components.
- Recorded the 2026-05-05 Packet FZ regression proof: main and Poligon builds, six script/restart-survival tests, visible/manual emulator receipt, and zero forbidden old-literal hits.
- Added public repository skeleton docs and governance files.
- Excluded local/generated/review/session/auth artifacts from the staging tree.
