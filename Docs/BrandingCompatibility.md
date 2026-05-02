# Branding Compatibility

Public-facing name: `Osvayder UE`.

Compatibility names retained in this packet:

- plugin folder: `Plugins/UnrealClaude/`
- descriptor file: `UnrealClaude.uplugin`
- module name: `UnrealClaude`
- many C++ class/file identifiers containing `UnrealClaude` or `Claude`

These names are intentionally unchanged in packet700 because a full module/package rename would affect Unreal build files, descriptor references, generated artifacts, scripts, tests, and downstream project installs.

Before public release, choose one of two paths:

1. Keep compatibility names for the first OSS release and document that `UnrealClaude` is the historical module name behind the `Osvayder UE` product brand.
2. Run a dedicated rename packet that updates module, descriptor, folder, docs, scripts, tests, URLs, and migration instructions together.

