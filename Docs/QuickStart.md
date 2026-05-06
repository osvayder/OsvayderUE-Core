# Quick Start

1. Install the plugin into a clean Unreal C++ host project.
2. Build the editor target.
3. Launch Unreal Editor.
4. Open the Osvayder UE development-agent UI.
5. Start with a bounded prompt, for example: "Audit this component and propose a minimal implementation plan before editing."
6. Run the requested verification step before accepting code changes.

## Recommended First Checks

- Confirm the editor plugin loads without generated-folder artifacts in the repo.
- Confirm the MCP bridge can reach the local plugin endpoint.
- Confirm build/verification receipts are written only to local project state.

