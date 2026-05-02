# OSS Extraction Manifest

Date: 2026-05-03

## Staging Root

`OpenSourcePrep/OsvayderUE-Core/`

## Included From Current Plugin Core

| Source | Staged path | Reason |
| --- | --- | --- |
| `Plugins/UnrealClaude/UnrealClaude.uplugin` | `Plugins/UnrealClaude/UnrealClaude.uplugin` | Plugin descriptor retained unchanged for compatibility. |
| `Plugins/UnrealClaude/LICENSE` | `Plugins/UnrealClaude/LICENSE` | Existing plugin MIT license. |
| `Plugins/UnrealClaude/.gitignore` | `Plugins/UnrealClaude/.gitignore` | Plugin-local generated-artifact ignore rules. |
| `Plugins/UnrealClaude/Source/` | `Plugins/UnrealClaude/Source/` | Core Unreal plugin source and tests. |
| `Plugins/UnrealClaude/Config/` | `Plugins/UnrealClaude/Config/` | Plugin package filter configuration. |
| `Plugins/UnrealClaude/Resources/CLAUDE.md.example` | `Plugins/UnrealClaude/Resources/CLAUDE.md.example` | Public example context file, not private session state. |
| `Plugins/UnrealClaude/Resources/icon128.png` | `Plugins/UnrealClaude/Resources/icon128.png` | Plugin icon resource. |
| `Plugins/UnrealClaude/Resources/mcp-config.json` | `Plugins/UnrealClaude/Resources/mcp-config.json` | Template MCP config using placeholder plugin path. |
| `Plugins/UnrealClaude/Resources/mcp-bridge/` selected files | `Plugins/UnrealClaude/Resources/mcp-bridge/` | MCP bridge source, package metadata, tests, contexts, and license; generated dependency/output folders excluded. |

Included plugin-core count at packet close: `275` source files, `1` config file, `34` selected resource files.

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
| `GDR_Shooter_Y.png` | Project media asset not required for plugin core; provenance not revalidated in packet700. |
| `Hello*.txt`, launch/review helper files in root | Local scratch/review artifacts outside public core. |

## Excluded From Plugin Local Tree

| Excluded path/pattern | Reason |
| --- | --- |
| `Plugins/UnrealClaude/Binaries/` | Generated plugin binaries. |
| `Plugins/UnrealClaude/Intermediate/` | Generated plugin build state. |
| `Plugins/UnrealClaude/Saved/` | Local plugin logs/session state. |
| `Plugins/UnrealClaude/.mcp.json` | Local MCP connector config; may include workstation-specific auth/session setup. |
| `Plugins/UnrealClaude/CLAUDE.md*` | Local agent instructions/backups; public-safe replacement is `Resources/CLAUDE.md.example`. |
| `Plugins/UnrealClaude/build_tree.bat` | Local utility not reviewed for public use. |
| `Plugins/UnrealClaude/structure.txt` | Generated tree dump. |
| `Plugins/UnrealClaude/Script/` | Excluded for packet700 because path-clean audit found workstation defaults, empirical packet scripts, and local verification helpers. |
| `Plugins/UnrealClaude/Resources/voice/` | Excluded because helper scripts download external voice/runtime assets; provenance and license chain should be reviewed before publication. |
| `Plugins/UnrealClaude/Resources/mcp-bridge/.git` | Nested git pointer/state. |
| `Plugins/UnrealClaude/Resources/mcp-bridge/.env.example` | Contains token-file/token env guidance; needs sanitized public rewrite. |
| `Plugins/UnrealClaude/Resources/mcp-bridge/index.js.backup_*` | Local backup copy. |
| `Plugins/UnrealClaude/Resources/mcp-bridge/node_modules/` | Generated dependency install tree. |
| `Plugins/UnrealClaude/Resources/mcp-bridge/coverage/` | Generated test coverage output. |

## Known Pre-Public Follow-Up

- Decide whether to keep `Plugins/UnrealClaude/` for the first public release or run a dedicated `OsvayderUE` module/package rename.
- Rewrite descriptor `DocsURL` and `SupportURL` after the final GitHub repository exists.
- Reintroduce any public scripts only after path defaults, test fixtures, and local packet references are sanitized.
- Confirm license compatibility for `Resources/mcp-bridge/LICENSE` and align public README/license language accordingly.
- Run fresh clone/package/build verification before publishing.

