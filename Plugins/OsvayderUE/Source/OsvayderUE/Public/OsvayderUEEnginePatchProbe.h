// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 628-v3 engine-patch-presence probe.
 *
 * Reads Epic UE 5.7 engine source file `Engine/Source/Developer/Windows/
 * LiveCoding/Private/LiveCodingModule.cpp` at editor startup and verifies
 * the 628-v3 `-LiveCodingLimit=1000` patch is present. If absent — which
 * happens whenever the user reinstalls the engine, upgrades UE, or has
 * not yet run `Apply_628v3_LiveCodingLimitPatch.ps1` — emits a warning
 * via `LogOsvayderUE` and surfaces the state as a
 * `engine_patch_status` agent_trace event so forensic analysis can
 * correlate "S1 hit LC limit" reports with patch state.
 *
 * Detection signatures (OR logic — either confirms patch presence):
 *   - `OsvayderUE 628-v3 patch` (comment breadcrumb)
 *   - `Arguments += TEXT(" -LiveCodingLimit=1000");` (literal patch line)
 *
 * Pure check — NEVER auto-applies or auto-modifies engine source. User
 * must run the elevation helper manually (see audit doc 628 Option A
 * Landing section).
 *
 * Reference:
 *   - Dispatch: `AgentBridge/CODEX_TO_OSVAYDER.md` 2026-04-20 02:15 (628-v3).
 *   - Engine source: `UE_5.7/Engine/Source/Developer/Windows/LiveCoding/
 *     Private/LiveCodingModule.cpp:1294-1296`.
 *   - Audit:    `Docs/OsvayderUE/628_LiveCodingLimitOriginAudit.md`.
 */
class OSVAYDERUE_API FOsvayderUEEnginePatchProbe
{
public:
	/** Run the probe at editor startup (from FOsvayderUEModule::StartupModule tail). */
	static void RunStartupProbe();

	/**
	 * Pure probe: load the given engine source file content (or caller-provided
	 * override for tests), check for either detection signature, return true
	 * if either signature is present. Does NOT log or emit agent_trace.
	 * Unit-testable without an actual engine installation.
	 */
	static bool DetectPatchPresence(const FString& EngineSourceContent);

	/**
	 * Canonical engine source file path for the 628-v3 patch target. Uses
	 * `FPaths::ConvertRelativePathToFull(FPaths::EngineDir())` + the fixed
	 * relative suffix `Source/Developer/Windows/LiveCoding/Private/
	 * LiveCodingModule.cpp`. Public for test transparency.
	 */
	static FString GetEngineSourceAbsolutePath();
};
