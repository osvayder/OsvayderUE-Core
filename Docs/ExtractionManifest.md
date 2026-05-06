# OSS Extraction Manifest

Date: 2026-05-05

## Staging Root

`OpenSourcePrep/OsvayderUE-Core/`

## Current Release Shape

- Plugin path: `Plugins/OsvayderUE/`
- Descriptor: `Plugins/OsvayderUE/OsvayderUE.uplugin`
- Module/source path: `Plugins/OsvayderUE/Source/OsvayderUE/`
- Public remote: `https://github.com/osvayder/OsvayderUE-Core.git`

## Included From Current Plugin Core

| Source | Staged path | Reason |
| --- | --- | --- |
| `Plugins/OsvayderUE/OsvayderUE.uplugin` | `Plugins/OsvayderUE/OsvayderUE.uplugin` | Current Osvayder UE descriptor. |
| `Plugins/OsvayderUE/LICENSE` | `Plugins/OsvayderUE/LICENSE` | Existing plugin MIT license. |
| `Plugins/OsvayderUE/.gitignore` | `Plugins/OsvayderUE/.gitignore` | Plugin-local generated-artifact ignore rules. |
| `Plugins/OsvayderUE/Source/` | `Plugins/OsvayderUE/Source/` | Core Unreal plugin source and tests. |
| `Plugins/OsvayderUE/Config/` | `Plugins/OsvayderUE/Config/` | Plugin package filter configuration. |
| `Plugins/OsvayderUE/Resources/OSVAYDER.md.example` | `Plugins/OsvayderUE/Resources/OSVAYDER.md.example` | Public example context file, not private session state. |
| `Plugins/OsvayderUE/Resources/icon128.png` | `Plugins/OsvayderUE/Resources/icon128.png` | Plugin icon resource. |
| `Plugins/OsvayderUE/Resources/mcp-config.json` | `Plugins/OsvayderUE/Resources/mcp-config.json` | Template MCP config using placeholder plugin path. |
| `Plugins/OsvayderUE/Resources/mcp-bridge/` selected files | `Plugins/OsvayderUE/Resources/mcp-bridge/` | MCP bridge source, package metadata, tests, contexts, and license; generated dependency/output folders excluded. |

Included plugin-core count at public-beta close: `294` source files, `1` config file, `34` selected resource files.

## Excluded From Current Workspace

| Excluded path/pattern | Reason |
| --- | --- |
| `.claude/` | Private local agent state. |
| `AgentBridge/` | Internal reviewer/worker handoff memory, not public source. |
| `Backups/` | Local backup state. |
| `Binaries/` | Generated Unreal binaries. |
| `DerivedDataCache/` | Generated Unreal cache. |
| `Intermediate/` | Generated Unreal build state. |
| `ReviewBundles/` | Internal review artifacts. |
| `Saved/` | Logs, sessions, token files, generated state. |
| `Content/` | Private/project content not needed for core plugin extraction. |
| `Config/`, `Source/`, root `.uproject` | Host project files, not the standalone public plugin repo. |
| `PluginBuild_*/` | Local packaged build outputs. |
| `*.zip`, `*.rar`, `*.7z` | Archives/review bundles/opaque packaged state. |
| local scratch/review helper files in root | Local scratch/review artifacts outside public core. |

## Excluded From Plugin Local Tree

| Excluded path/pattern | Reason |
| --- | --- |
| `Plugins/OsvayderUE/Binaries/` | Generated plugin binaries. |
| `Plugins/OsvayderUE/Intermediate/` | Generated plugin build state. |
| `Plugins/OsvayderUE/Saved/` | Local plugin logs/session state. |
| `Plugins/OsvayderUE/.mcp.json` | Local MCP connector config; may include workstation-specific auth/session setup. |
| `Plugins/OsvayderUE/OSVAYDER.md*` | Local agent instructions/backups; public-safe replacement is `Resources/OSVAYDER.md.example`. |
| `Plugins/OsvayderUE/build_tree.bat` | Local utility not reviewed for public use. |
| `Plugins/OsvayderUE/structure.txt` | Generated tree dump. |
| `Plugins/OsvayderUE/Script/` | Excluded because path defaults, empirical scripts, and local verification helpers need a dedicated public sanitization pass. |
| `Plugins/OsvayderUE/Resources/voice/` | Excluded because helper scripts download external voice/runtime assets; provenance and license chain should be reviewed before publication. |
| `Plugins/OsvayderUE/Resources/mcp-bridge/.git` | Nested git pointer/state. |
| `Plugins/OsvayderUE/Resources/mcp-bridge/.env.example` | Local environment template with token-file guidance; not included in the public release tree. |
| `Plugins/OsvayderUE/Resources/mcp-bridge/index.js.backup_*` | Local backup copy. |
| `Plugins/OsvayderUE/Resources/mcp-bridge/node_modules/` | Generated dependency install tree. |
| `Plugins/OsvayderUE/Resources/mcp-bridge/coverage/` | Generated test coverage output. |

## Current Public Proof Baseline

- Unreal Engine 5.7 on Windows clean-host validation passed.
- Main plugin host build passed.
- Clean host build passed.
- MCP bridge dependency install and tests passed.
- Restart-survival and local workflow checks passed in the maintained private validation environment.
- Fresh-artifact forbidden old-literal scan had zero release-surface hits.
- Additional Unreal Engine versions, Linux, and macOS are not currently claimed.

## Known Follow-Up

- Reintroduce any public scripts only after path defaults, test fixtures, and local workflow references are sanitized.
- Keep `Resources/voice/` excluded until external runtime download/provenance and license handling are reviewed.
- Confirm bundled subcomponent notices before release tagging or redistribution packaging.
- Continue additional host validation before widening support claims.
