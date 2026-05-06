# Osvayder UE Public Beta Demo Script

## Audience

Public reviewers, grant evaluators, and contributors who need to understand the beta workflow without reading internal development notes.

## Setup

- Use Unreal Engine 5.7 on Windows for the verified path.
- Start from a clean C++ host project.
- Install the plugin by copying `Plugins/OsvayderUE/` into the host project's `Plugins/` directory.
- Run the MCP bridge setup from `Plugins/OsvayderUE/Resources/mcp-bridge/`:

```powershell
npm install
npm test
```

## Demo Flow

1. Open the clean host project in Unreal Editor.
2. Confirm the Osvayder UE plugin is enabled.
3. Open the Osvayder UE panel from the plugin menu or toolbar.
4. Show the main panel and explain that the bridge is local-first.
5. Show the MCP bridge status or auth handoff state without exposing tokens.
6. Run one safe, read-only or low-risk workflow.
7. Restart or reload the editor if demonstrating continuity.
8. Confirm the UI returns to a usable state.
9. Close with the current caveats: compatibility outside Unreal Engine 5.7, Linux, and macOS is not currently claimed.

## Reviewer Markers

- Plugin appears in the editor.
- Main panel opens without requiring private project state.
- MCP bridge install and tests pass.
- No secrets or private paths are visible.
- Any lifecycle caveat is stated plainly instead of hidden in logs.

## Screenshot List

Use the existing public screenshots where they match the current UI:

- [Docs/images/01_project_settings.png](../images/01_project_settings.png)
- [Docs/images/02_main_widget.png](../images/02_main_widget.png)
- [Docs/images/05_restart_survival.png](../images/05_restart_survival.png)

If recording new material, capture only public-safe editor views. Do not include a video reference in public docs until a public-safe video file exists.
