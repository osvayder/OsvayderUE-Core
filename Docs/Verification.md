# Verification

This staging baseline has passed an export safety audit. Fresh public clone and Unreal build validation are tracked separately so the public release state stays explicit.

## Current Checks

- Forbidden local/generated artifact path scan: passed.
- Forbidden archive/session/build folder scan: passed.
- Selected plugin source/config/resource copy completed.
- Public skeleton docs created.
- Public repository metadata and support URLs reviewed.

## Validation Matrix

| Check | Status | Notes |
| --- | --- | --- |
| Export safety audit | Passed | See `Docs/SafetyAudit.md`. |
| Secret/path hygiene scan | Passed | No obvious token/session/build artifacts in the staging tree. |
| Fresh public clone install | Pending | Validate from a clean clone before first release tag. |
| Unreal editor target build | Pending | Validate against the primary target version first. |
| MCP bridge dependency install | Pending | Run from `Plugins/UnrealClaude/Resources/mcp-bridge/`. |
| MCP bridge tests | Pending | Run `npm test` from the bridge directory. |
| License and attribution review | Pending | Confirm bundled subcomponent notices before redistribution. |

## Before First Release Tag

1. Clone the public repository into a clean directory.
2. Copy `Plugins/UnrealClaude/` into a clean C++ Unreal host project.
3. Regenerate project files and build the editor target.
4. Launch Unreal Editor and confirm the plugin loads.
5. Run MCP bridge dependency install and tests.
6. Confirm docs, descriptor URLs, support links, and repository links point to `osvayder/OsvayderUE-Core`.
7. Confirm license compatibility for bundled components.
