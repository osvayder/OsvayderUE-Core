# OpenAI API Credits Plan for Osvayder UE Core

## Project summary

Osvayder UE Core is an open-source Unreal Engine 5 development-agent plugin with
a local MCP bridge for bounded AI-assisted development inside Unreal Editor. The
project aims to make Unreal agent workflows more reviewable by keeping edits
scoped, tying work to editor context, and recording verification evidence before
changes are accepted.

## Why the project matters

Unreal teams often need help across C++, editor state, assets, logs, and build
tooling at the same time. General chat workflows can miss that context or make
unsafe broad changes. Osvayder UE Core gives reviewers and maintainers a public
tooling surface for safer agent work: explicit scopes, local receipts, restart
survival, clean-host checks, and documented security boundaries.

## Why OpenAI / Codex

Codex is a strong fit because this repository spans C++ plugin code,
JavaScript-based MCP bridge code, Unreal configuration, verification scripts,
and public documentation. API credits would let the maintainer use Codex for
repeatable OSS review loops: inspect the current repo, propose a bounded patch,
run targeted checks, and leave evidence that another reviewer can verify.

## How API credits will be used

- Review and harden plugin C++, Unreal editor integration, MCP bridge behavior,
  and public docs.
- Implement public OSS issues with small, scoped diffs and clear acceptance
  criteria.
- Expand regression coverage for clean-host installs, bridge tests,
  restart-survival state, auth-token handling, and unsafe mutation prevention.
- Produce and maintain reviewer-facing documentation, examples, issue triage,
  and verification notes.
- Run Codex-assisted security review focused on local auth files, filesystem
  boundaries, session storage, and tool-call safety.
- Validate workflows in synthetic or throwaway Unreal host projects instead of
  private user projects.

## What credits will not be used for

- Private game production, paid client work, or closed-source feature
  development.
- Marketplace-readiness claims, store submission work, advertising, or unrelated
  product marketing.
- Processing secrets, private chat transcripts, saved API keys, payment data, or
  unredacted user project files.
- Generating proprietary datasets or training data from user projects.
- Bypassing Unreal, Epic, provider, or third-party license terms.
- Long-running production hosting of user-facing AI services.

## 30 / 60 / 90 day plan

**30 days:** stabilize the public grant-review path. Tighten README and reviewer
docs, keep issue triage public, add small clean-host verification improvements,
and document any known beta limitations without overstating release maturity.

**60 days:** expand automated regression coverage. Add or improve checks for MCP
bridge startup, restart-survival receipts, auth/session hygiene, forbidden file
artifacts, and repeatable install validation in clean Unreal host projects.

**90 days:** improve maintainability and contributor readiness. Convert repeated
manual checks into documented scripts where practical, add example workflows,
review security boundaries again, and prepare a clearer public release checklist
for maintainers and outside contributors.

## Current verification evidence

The current public baseline documents a Windows / Unreal Engine 5.7 clean-host
build and launch check, MCP bridge dependency install and tests, forbidden
artifact scans, restart-survival checks, and safety-audit notes. Start with
[Docs/Verification.md](Docs/Verification.md), then review [README.md](README.md),
[SECURITY.md](SECURITY.md), and [Docs/Roadmap.md](Docs/Roadmap.md).

Known limitations remain documented: additional Unreal Engine version validation
and broader packaging checks are planned, and the project does not claim
Marketplace readiness.

## Maintainer role

Osvayder maintains the public core export, issue prioritization, safety posture,
documentation, and verification discipline. API credits would support maintainer
time on public OSS workflows only: review, implementation, tests, docs, security
hardening, and evidence production that can be checked by OpenAI/Codex reviewers
and outside contributors.
