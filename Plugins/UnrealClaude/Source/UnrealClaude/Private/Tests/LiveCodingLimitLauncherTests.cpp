// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 628-v2 Phase 2 launcher safety-net verification.
 *
 * Per 628 pushback EFP audit: UE 5.7 `TargetDescriptor.LiveCodingLimit`
 * defaults to 0 (no limit). Override is ONLY via `-LiveCodingLimit=N` UBT
 * CLI flag. Editor's `FLiveCodingModule::SetBuildArguments` does NOT inject
 * this flag. The preflight launcher `Launch-OsvayderPlugin-WithPreflight.ps1`
 * is the ONE plugin-controlled layer that invokes `Build.bat` directly —
 * adding `-LiveCodingLimit=1000` there is the launcher-level safety net
 * per 628-v2 dispatch Phase 2.
 *
 * This test enforces the invariant: the launcher script MUST carry
 * `-LiveCodingLimit=1000` on its Build.bat invocation line. If a future
 * refactor drops the flag, this test fires; reviewer sees the drop in
 * the diff and either keeps it (correct default for autonomous AI sessions)
 * or documents why it was removed.
 *
 * Reference:
 *   - Dispatch: `AgentBridge/CODEX_TO_CLAUDE.md` 2026-04-20 02:00 (628-v2).
 *   - Engine source: `UE_5.7/Engine/Source/Programs/UnrealBuildTool/
 *     Configuration/TargetDescriptor.cs:121-122`.
 */

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClaudeEnginePatchProbe.h" // 628-v3 patch probe tests

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingLimitLauncher_PreflightScriptEmitsLimit,
	"UnrealClaude.LiveCoding.LimitLauncher.PreflightScriptEmitsLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingLimitLauncher_PreflightScriptEmitsLimit::RunTest(const FString& /*Parameters*/)
{
	// Resolve the launcher script path via the plugin dir (stable across hosts).
	IPluginManager& PM = IPluginManager::Get();
	TSharedPtr<IPlugin> Plugin = PM.FindPlugin(TEXT("UnrealClaude"));
	if (!TestTrue(TEXT("UnrealClaude plugin must be discoverable"), Plugin.IsValid()))
	{
		return false;
	}

	const FString ScriptPath = FPaths::Combine(
		Plugin->GetBaseDir(),
		TEXT("Script"),
		TEXT("Launch-OsvayderPlugin-WithPreflight.ps1"));

	if (!TestTrue(
		FString::Printf(TEXT("Launcher script must exist at %s"), *ScriptPath),
		IFileManager::Get().FileExists(*ScriptPath)))
	{
		return false;
	}

	FString ScriptContent;
	if (!TestTrue(
		TEXT("Launcher script must be loadable"),
		FFileHelper::LoadFileToString(ScriptContent, *ScriptPath)))
	{
		return false;
	}

	// Core invariant 1: the script must carry -LiveCodingLimit=1000 on a
	// Build.bat invocation line. Use case-insensitive Contains because PS
	// arg parsing accepts mixed casing; spec value is 1000.
	TestTrue(
		TEXT("Launcher script MUST carry -LiveCodingLimit=1000"),
		ScriptContent.Contains(TEXT("-LiveCodingLimit=1000"), ESearchCase::IgnoreCase));

	// Core invariant 2: the flag must appear on a line that also carries
	// Build.bat or $buildBat (the UBT invocation line), not just in a
	// comment. Verify by line-scan.
	TArray<FString> Lines;
	ScriptContent.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
	bool bFoundOnInvocationLine = false;
	for (const FString& Line : Lines)
	{
		if (!Line.Contains(TEXT("-LiveCodingLimit=1000"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		// Skip comment-only lines (# or <# at start, tolerant of whitespace).
		const FString Trimmed = Line.TrimStart();
		if (Trimmed.StartsWith(TEXT("#")) || Trimmed.StartsWith(TEXT("<#")))
		{
			continue;
		}
		// Must be co-located with Build.bat or $buildBat invocation.
		if (Line.Contains(TEXT("Build.bat"), ESearchCase::IgnoreCase)
			|| Line.Contains(TEXT("$buildBat"), ESearchCase::CaseSensitive))
		{
			bFoundOnInvocationLine = true;
			break;
		}
	}
	TestTrue(
		TEXT("-LiveCodingLimit=1000 must be on a Build.bat / $buildBat invocation line (not only in a comment)"),
		bFoundOnInvocationLine);

	return true;
}

// ================================================================
// 628-v3 engine-patch-presence probe tests
//
// These tests exercise the PURE detection helper `DetectPatchPresence`
// over SYNTHETIC engine source fixtures. They do NOT read the real
// engine file on disk, so they're stable across dev machines regardless
// of whether the 628-v3 patch has been applied.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingLimitLauncher_ProbeDetectsPatchWhenPresent,
	"UnrealClaude.LiveCoding.EnginePatch.ProbeDetectsPatchWhenPresent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingLimitLauncher_ProbeDetectsPatchWhenPresent::RunTest(const FString& /*Parameters*/)
{
	// Synthesize a fake engine source file content carrying BOTH signatures:
	// the comment breadcrumb and the literal patch line.
	const FString SynthesizedWithPatch = TEXT(
		"void FLiveCodingModule::SetBuildArguments()\n"
		"{\n"
		"    FString Arguments = TEXT(\"SomeTarget Win64 Development\");\n"
		"    // UnrealClaude 628-v3 patch: raise LiveCoding action limit from environmental\n"
		"    // default (observed 100 during autonomous AI workflows) to 1000 for long sessions.\n"
		"    Arguments += TEXT(\" -LiveCodingLimit=1000\");\n"
		"    LppSetBuildArguments(*Arguments);\n"
		"}\n"
	);
	TestTrue(
		TEXT("Fixture with both signatures must be detected as patch-present"),
		FUnrealClaudeEnginePatchProbe::DetectPatchPresence(SynthesizedWithPatch));

	// Comment-only signature — OR logic, should still detect.
	const FString CommentOnly = TEXT(
		"// UnrealClaude 628-v3 patch: raise LiveCoding action limit...\n"
		"// (but patch line is absent for some reason)\n"
	);
	TestTrue(
		TEXT("Fixture with only comment signature must still be detected"),
		FUnrealClaudeEnginePatchProbe::DetectPatchPresence(CommentOnly));

	// Line-only signature — OR logic, should still detect.
	const FString LineOnly = TEXT(
		"// (comment breadcrumb missing)\n"
		"Arguments += TEXT(\" -LiveCodingLimit=1000\");\n"
	);
	TestTrue(
		TEXT("Fixture with only line signature must still be detected"),
		FUnrealClaudeEnginePatchProbe::DetectPatchPresence(LineOnly));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingLimitLauncher_ProbeFlagsMissingPatch,
	"UnrealClaude.LiveCoding.EnginePatch.ProbeFlagsMissingPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingLimitLauncher_ProbeFlagsMissingPatch::RunTest(const FString& /*Parameters*/)
{
	// Synthesize stock UE 5.7 engine source content (unpatched baseline
	// — what an engine reinstall produces). Neither signature present.
	const FString UnpatchedBaseline = TEXT(
		"void FLiveCodingModule::SetBuildArguments()\n"
		"{\n"
		"    FString Arguments = FString::Printf(TEXT(\"%s %s %s\"),\n"
		"        *KnownTargetName,\n"
		"        FPlatformMisc::GetUBTPlatform(),\n"
		"        LexToString(FApp::GetBuildConfiguration()));\n"
		"    FString SourceProject = SourceProjectVariable->GetString();\n"
		"    if (SourceProject.Len() > 0)\n"
		"    {\n"
		"        Arguments += FString::Printf(TEXT(\" -Project=\\\"%s\\\"\"), *FPaths::ConvertRelativePathToFull(SourceProject));\n"
		"    }\n"
		"    LppSetBuildArguments(*Arguments);\n"
		"}\n"
	);
	TestFalse(
		TEXT("Unpatched engine baseline must NOT be detected as patch-present"),
		FUnrealClaudeEnginePatchProbe::DetectPatchPresence(UnpatchedBaseline));

	// Empty content — must not false-positive.
	TestFalse(
		TEXT("Empty content must not be detected as patch-present"),
		FUnrealClaudeEnginePatchProbe::DetectPatchPresence(FString()));

	// Adversarial: content containing a DIFFERENT LiveCodingLimit value
	// (e.g., pre-existing =500 patch from a different fix) should NOT
	// false-positive as our 628-v3 patch. Exact string match on =1000.
	const FString DifferentLimit = TEXT(
		"// Some other developer's earlier patch\n"
		"Arguments += TEXT(\" -LiveCodingLimit=500\");\n"
	);
	TestFalse(
		TEXT("Different LiveCodingLimit value (=500) must NOT match our =1000 signature"),
		FUnrealClaudeEnginePatchProbe::DetectPatchPresence(DifferentLimit));

	// Adversarial: arbitrary reference to "628-v3 patch" without comment
	// context (e.g., a log message) should NOT false-positive because
	// the signature is the FULL phrase "UnrealClaude 628-v3 patch".
	const FString PartialPhrase = TEXT("The user applied 628-v3 patch manually yesterday.\n");
	TestFalse(
		TEXT("Partial phrase without canonical prefix must NOT match"),
		FUnrealClaudeEnginePatchProbe::DetectPatchPresence(PartialPhrase));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingLimitLauncher_ProbeResolvesEngineSourcePath,
	"UnrealClaude.LiveCoding.EnginePatch.ProbeResolvesEngineSourcePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingLimitLauncher_ProbeResolvesEngineSourcePath::RunTest(const FString& /*Parameters*/)
{
	// Smoke test: GetEngineSourceAbsolutePath returns something reasonable.
	// Must be non-empty and absolute.
	const FString Path = FUnrealClaudeEnginePatchProbe::GetEngineSourceAbsolutePath();
	TestFalse(TEXT("Engine source path must not be empty"), Path.IsEmpty());
	TestTrue(
		TEXT("Engine source path must end with the canonical LiveCodingModule.cpp suffix"),
		Path.EndsWith(TEXT("LiveCodingModule.cpp"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("Engine source path must be absolute (contains drive letter or starts with /)"),
		Path.Contains(TEXT(":")) || Path.StartsWith(TEXT("/")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
