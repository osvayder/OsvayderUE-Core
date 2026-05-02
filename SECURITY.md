# Security Policy

## Supported Versions

This staging tree tracks the current beta OSS-prep baseline. Public support windows will be defined after the first GitHub release.

## Reporting a Vulnerability

Do not disclose vulnerabilities publicly before maintainers have had time to investigate. For the first public release, configure GitHub private vulnerability reporting or publish a maintainer security contact.

## Sensitive Data Rules

- Do not commit API keys, MCP auth tokens, session files, chat logs, local bridge state, or generated Unreal build state.
- Do not commit `Saved/`, `Intermediate/`, `Binaries/`, `ReviewBundles/`, `.claude/`, `AgentBridge/`, `node_modules/`, or coverage output.
- Use local token files only from a private project `Saved/UnrealClaude/` directory.
- Treat all provider transcripts and agent session files as private unless explicitly scrubbed.

## Scope

Security reports should focus on plugin code, MCP bridge behavior, editor workflow boundaries, session/auth handling, and unsafe mutation or file-access behavior.

