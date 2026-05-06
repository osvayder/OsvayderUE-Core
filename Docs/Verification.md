# Verification

This page is the public reviewer path for validating the current Osvayder UE beta package.

## Verified

| Area | Status | Proof marker |
| --- | --- | --- |
| Repository hygiene | Passed | Release-surface scan found no generated build folders, sessions, auth tokens, or private machine paths intended for publication. |
| Documentation consistency | Passed | Public setup and reviewer docs were checked against the staged repository layout. |
| Unreal Engine 5.7 on Windows | Passed | Clean C++ host project copied `Plugins/OsvayderUE/`, regenerated project files, built the editor target, and launched the editor commandlet until plugin load and quit markers. |
| Osvayder UE plugin load | Passed with caveat | The plugin mounted, initialized, started the local MCP bridge flow, wrote the expected auth handoff under the host project's `Saved/` tree, and reached the quit command. |
| MCP bridge install and tests | Passed | `npm install` and `npm test` completed from `Plugins/OsvayderUE/Resources/mcp-bridge/`. |
| Public screenshots | Available | See `Docs/images/` and the demo checklist in `Docs/demo/README.md`. |

## Known Caveats

- The verified Unreal baseline is Windows with Unreal Engine 5.7.
- Compatibility outside Unreal Engine 5.7 is not currently claimed.
- Linux and macOS are not currently claimed for the public beta.
- The headless Unreal launch reached the quit marker, then required forced termination because engine background activity kept the commandlet process alive. Treat this as a launch lifecycle caveat, not as a plugin-load failure.
- License and attribution review for bundled third-party subcomponents should be completed before a first release tag.
- The repository includes a minimal host-project placeholder, not a fully checked-in generated Unreal project.

## How to Reproduce

1. Clone the public repository into a clean directory.
2. Create a fresh C++ Unreal Engine 5.7 project on Windows.
3. Copy `Plugins/OsvayderUE/` into the host project's `Plugins/` directory.
4. Regenerate project files.
5. Build the host editor target in `Development Editor` configuration.
6. Launch Unreal Editor and confirm the Osvayder UE plugin appears.
7. From `Plugins/OsvayderUE/Resources/mcp-bridge/`, run:

```powershell
npm install
npm test
```

8. Confirm no local `.env`, auth token, generated session, `Saved/`, `Intermediate/`, or `Binaries/` artifacts are committed.
9. Walk through the public demo checklist in `Docs/demo/osvayderue_public_beta_demo.md`.

## Related Docs

- Installation: [Docs/Installation.md](Installation.md)
- Demo checklist: [Docs/demo/README.md](demo/README.md)
- Public backlog: [Docs/PublicIssueBacklog.md](PublicIssueBacklog.md)
- Safety audit: [Docs/SafetyAudit.md](SafetyAudit.md)
