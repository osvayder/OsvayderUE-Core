# Public Issue Backlog

These issue drafts are the intended first public roadmap items for the repository. They are stored here because the current GitHub integration does not have permission to create issues in `osvayder/OsvayderUE-Core`.

## 1. Public README polish and contributor onboarding

### Goal
Polish the public-facing repository entrypoint so new contributors can understand the project quickly.

### Scope
- tighten README wording where needed
- add a short contributor getting-started path
- clarify supported/unsupported workflows
- verify links between README and Docs remain accurate

### Acceptance
- new contributor can understand what the plugin is, what it is not, and where to start within 5 minutes
- README and docs links are internally consistent
- no private/local-machine details leak into public docs

## 2. Descriptor URL and public branding cleanup

### Goal
Reduce public-facing legacy naming and stale repository metadata while preserving module compatibility for the first OSS phase.

### Scope
- update descriptor docs/support URLs
- audit public-facing strings for unnecessary legacy branding
- document which compatibility names must remain for now
- avoid breaking the existing module/package path unless explicitly planned

### Acceptance
- public metadata points to the current repository where possible
- compatibility naming is documented instead of hidden
- no accidental module/descriptor breakage

## 3. Visible editor UI proof and screenshot refresh

### Goal
Add cleaner visible-editor evidence for the public repo and future grant/review bundles.

### Scope
- capture cleaner plugin-panel and workflow screenshots
- add at least one visible-editor UX proof beyond synthetic render output
- reduce internal diagnostic noise in public-facing screenshots
- optionally add a short GIF for the repo landing page

### Acceptance
- screenshot pack is coherent and public-safe
- visible editor UI is represented clearly
- grant/public reviewers can understand the workflow from visuals alone

## 4. Clean public package validation from fresh clone

### Goal
Prove that the public repository can be cloned cleanly and validated without hidden local-machine state.

### Scope
- fresh clone on a clean path
- secret/path scan
- package/build validation
- MCP bridge dependency/bootstrap validation
- verify public docs match the actual setup steps

### Acceptance
- fresh clone path succeeds without relying on hidden private artifacts
- any remaining blockers are documented precisely
- resulting validation notes are added to public docs

## 5. Expert-mode auth and security hardening

### Goal
Continue hardening the local MCP/auth boundary for public collaboration and controlled dogfood usage.

### Scope
- review token handling and exposure surfaces
- tighten unauthenticated status/reporting where appropriate
- define an explicit armed expert-mode UX and audit path
- document current trust assumptions and non-goals

### Acceptance
- current auth behavior is documented clearly
- high-risk operations remain gated
- next security-hardening steps are concrete and testable

## 6. Animation asset dependency pipeline for mechanic workflows

### Goal
Add a first-class workflow for animation-dependent mechanic implementation instead of treating missing animation assets as ad-hoc blockers.

### Scope
- define animation dependency status in mechanic closeout
- support candidate pack discovery / free-first filtering
- define manual acquisition gate for licensed/external assets
- document retarget/apply/verify steps

### Acceptance
- mechanics can report animation readiness explicitly
- missing animation assets no longer appear as ambiguous implementation failures
- the workflow is documented for contributors and reviewers
