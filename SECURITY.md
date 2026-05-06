# Security Policy

## Supported Versions

This repository is in a public grant-readiness and early OSS-prep stage. Public support windows will be defined after the first tagged public release.

## Reporting a Vulnerability

Do not publish sensitive vulnerability details in a public issue.

If GitHub private vulnerability reporting or repository security advisories are enabled, use that channel. If private reporting is not available, open a public issue only for non-sensitive security hygiene reports and omit exploit details, secrets, private paths, provider transcripts, tokens, and host-project data.

Maintainers should add an official security contact here when one is configured. Until then, do not invent or rely on an unofficial email address.

## Sensitive Data Rules

- Do not commit API keys, MCP auth tokens, session files, chat logs, local bridge state, provider transcripts, or generated Unreal build state.
- Do not commit generated folders such as `Saved/`, `Intermediate/`, `Binaries/`, `node_modules/`, coverage output, or local review bundles.
- Treat all provider prompts, transcripts, agent session files, and host-project logs as private unless explicitly scrubbed.
- Sanitize screenshots and logs before attaching them to issues or pull requests.

## Security Scope

Security reports should focus on:

- Unsafe file access or mutation boundaries.
- MCP bridge input validation, command execution, and JSON output handling.
- Token, session, transcript, and credential handling.
- Unreal Editor workflow boundaries that could expose private project data.
- Dependency or packaging behavior that could execute unexpected code.

## Non-Sensitive Reports

Use the bug report template for non-sensitive security hygiene issues such as missing validation, unclear documentation, or dependency warnings that do not include exploitable details.
