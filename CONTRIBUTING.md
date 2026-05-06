# Contributing

Thanks for helping improve Osvayder UE Core. This repository is being prepared for public review, so contributor changes should be small, reproducible, and free of private project data.

## Ground Rules

- Keep changes bounded to one topic and explain user-visible behavior changes.
- Avoid runtime behavior changes in documentation-only or repository-hygiene pull requests.
- Do not commit generated Unreal folders, local logs, tokens, chat/session state, or private host-project content.
- Do not include absolute local paths, machine names, secrets, API keys, or private URLs in code, docs, screenshots, or logs.
- Prefer reproducible verification evidence over claims such as "works on my machine".

## Development Setup

Use a local Unreal Engine host project appropriate for the change. When reporting validation, include:

- Unreal Engine version.
- Operating system and version.
- Plugin version, release tag, or commit SHA.
- Host project type, for example C++ project, Blueprint-only project, or sample/minimal host.
- Whether the MCP bridge package was installed from the repository workspace or another local checkout.

## Validation Expectations

Run the checks relevant to your change and paste sanitized results in the pull request:

- Unreal Editor loads the plugin without startup errors.
- Relevant automation tests or manual editor flows pass.
- MCP bridge package installs and its npm checks pass when MCP behavior is touched.
- MCP bridge JSON output parses cleanly when command/tool output is changed.
- Documentation, examples, or templates are updated when contributor-facing behavior changes.

Logs and screenshots are welcome, but remove tokens, usernames, machine paths, project names that should remain private, provider transcripts, and local session identifiers.

## Pull Requests

Before opening a pull request:

- Rebase or merge the latest target branch and resolve conflicts locally.
- Fill in the pull request template honestly; mark non-applicable checks as `N/A` with a short reason.
- Link related issues when possible.
- Keep formatting-only changes separate from behavior changes.
- Do not make Marketplace-ready, production-ready, or official-support claims unless maintainers have added that wording.

## Issues

Use the issue templates when available. For bugs, include minimal reproduction steps and sanitized logs. For feature requests, describe the workflow and why existing behavior is insufficient. For verification reports, include exact versions and evidence so maintainers can compare environments.

## Code Style

Match the style of nearby Unreal C++, JavaScript/TypeScript, PowerShell, Markdown, and YAML files. Avoid broad renames or compatibility-breaking changes unless a maintainer has approved the plan in an issue or discussion.
