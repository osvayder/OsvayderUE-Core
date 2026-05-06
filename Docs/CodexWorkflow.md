# Codex Workflow

Osvayder UE Core is designed for AI-assisted Unreal development workflows where Codex or another provider helps inspect, modify, and verify project code under explicit safety rules.

## Intended Use

- Generate implementation plans for bounded Unreal tasks.
- Route safe editor/tool actions through MCP.
- Preserve verification receipts for build, runtime, and manual gates.
- Recover across editor restarts when C++ reflection changes require it.

## Guardrails

- Do not grant broad filesystem or shell access unless a task explicitly requires it.
- Keep provider auth/session data outside the public repository.
- Treat local chat transcripts and session state as private.
- Require truthful closeout: `done`, `blocked`, `blocked_on_manual`, or `scope_pushback`.

## Grant Narrative

The project uses Codex-style workflows to harden real Unreal Engine plugin development: task decomposition, source edits, verification planning, regression evidence, and public OSS preparation.

