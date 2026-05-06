# Public Issue Backlog

These are draft public issues for early repository hardening. They are kept here until the project can create and manage them directly in GitHub.

## 1. Public README Polish and Contributor Onboarding

### Goal

Make the repository entry point easy for new contributors and reviewers to understand quickly.

### Scope

- Tighten README wording where needed.
- Add a short contributor getting-started path.
- Clarify supported and unsupported workflows.
- Verify links between README and `Docs/` remain accurate.

### Acceptance

- A new contributor can understand what the plugin is, what it is not, and where to start within 5 minutes.
- README and docs links are internally consistent.
- No private local-machine details appear in public docs.

## 2. Clean Public Package Validation from Fresh Clone

### Goal

Prove that the public repository can be cloned and validated without hidden local-machine state.

### Scope

- Fresh clone on a clean path.
- Secret and private-path scan.
- Unreal Engine 5.7 Windows build and launch validation.
- MCP bridge dependency and test validation.
- Public docs checked against the actual setup steps.

### Acceptance

- Fresh clone validation succeeds without private artifacts.
- Any remaining blockers are documented precisely in `Docs/Verification.md`.
- Validation notes distinguish verified targets from unclaimed targets.

## 3. Additional Unreal Engine Version Compatibility Check

### Goal

Validate whether the current beta package works on additional Unreal Engine versions.

### Scope

- Repeat the clean-host install flow from `Docs/Installation.md`.
- Build the host editor target.
- Launch the editor and confirm plugin load.
- Record any API, build, or runtime differences.

### Acceptance

- `Docs/Verification.md` is updated with passed or failed compatibility status.
- Any required compatibility changes are tracked as separate issues.

## 4. Cross-Platform Validation

### Goal

Move Linux and macOS from not claimed to verified or documented unsupported status.

### Scope

- Run the clean-host install flow on Linux.
- Run the clean-host install flow on macOS.
- Validate MCP bridge install and tests on each platform.
- Document platform-specific caveats.

### Acceptance

- Platform status is public and reproducible.
- No compatibility claim is broader than the completed checks.

## 5. Visible Editor UI Proof and Screenshot Refresh

### Goal

Keep a public-safe visual walkthrough for reviewers and grant evaluators.

### Scope

- Capture clean plugin-panel screenshots.
- Capture the project settings or setup view.
- Capture a restart-survival or session-continuity view if available.
- Keep screenshots free of private paths, secrets, and local usernames.

### Acceptance

- `Docs/demo/README.md` lists the current screenshot set.
- Visuals are coherent enough to understand the workflow without internal logs.

## 6. MCP Auth and Security Hardening

### Goal

Continue hardening the local MCP/auth boundary for public collaboration.

### Scope

- Review token handling and exposure surfaces.
- Clarify local trust assumptions.
- Gate high-risk operations behind explicit user intent.
- Document security non-goals.

### Acceptance

- Current auth behavior is documented clearly.
- High-risk operations remain gated.
- Next security-hardening steps are concrete and testable.
