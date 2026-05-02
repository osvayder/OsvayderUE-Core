# Contributing

Thanks for helping improve Osvayder UE Core.

## Development Principles

- Keep changes bounded and reviewable.
- Prefer root-cause fixes over surface patches.
- Preserve Unreal Engine compatibility and editor safety.
- Do not commit local logs, tokens, sessions, generated files, or private project content.
- Add or update verification notes when changing workflow, MCP, build, or restart-survival behavior.

## Public-Repo Hygiene

Before opening a pull request:

1. Confirm no generated Unreal folders are included.
2. Confirm no private project paths, tokens, or session state are included.
3. Run the relevant build/test workflow for the changed area.
4. Update docs when user-visible behavior changes.

## Code Style

Match the existing Unreal C++ and PowerShell/JavaScript style in nearby files. Avoid broad renames unless a maintainer has explicitly approved a compatibility plan.

