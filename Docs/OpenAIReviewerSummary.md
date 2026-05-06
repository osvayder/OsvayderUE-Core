# OpenAI Reviewer Summary

Short answers for grant/application review. Each answer is intentionally compact
for copy-paste use.

## Copy-Ready Answers

**What is Osvayder UE Core?**  
An open-source Unreal Engine 5 development-agent plugin with a local MCP bridge
for bounded AI-assisted development, editor-aware context, restart-survival
receipts, and verification-first workflows.

**Why does it matter?**  
Unreal AI workflows often become opaque chat sessions. This project keeps agent
work tied to scoped editor actions, local proof, safety boundaries, and public
reviewable diffs.

**Why OpenAI/Codex?**  
Codex fits the project because it can review C++/Unreal, bridge JavaScript, docs,
and tests together, then make bounded patches only when verification criteria are
explicit.

**How would API credits be used?**  
Credits would support OSS maintenance: code review, bounded fixes, regression
automation, security hardening, documentation, issue triage, and clean-host
verification.

**How would credits not be used?**  
Credits would not fund private game production, paid client work, Marketplace
submission claims, credential handling, unrelated marketing, or closed-source
feature development.

**Current status?**  
Beta / controlled dogfood ready. UE 5.7 is the primary target. The public core
export is live, with broader packaging, security, and cross-host validation still
on the roadmap.

**Security posture?**  
The repo excludes API keys, local auth tokens, session files, build outputs, and
private project state. Security work focuses on auth handling, filesystem
boundaries, and safe mutation.

**Main proof links?**  
Start with [README.md](../README.md), [GRANT.md](../GRANT.md),
[Docs/Verification.md](Verification.md), [SECURITY.md](../SECURITY.md), and
[Docs/Roadmap.md](Roadmap.md).
