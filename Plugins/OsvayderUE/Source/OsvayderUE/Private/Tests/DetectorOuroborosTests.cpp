// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 626 P2 detector Ouroboros/self-reference regression tests.
 *
 * Acceptance:
 *   A-P2-4 negative: five scenarios where the detector MUST NOT escalate
 *     because the input is repository inspection / agent reasoning / saved
 *     artifact read-back rather than a real build blocker.
 *   A-P2-5 positive: four scenarios where the detector MUST escalate
 *     because the input is a genuine UBT / Live Coding / compiler build
 *     blocker from a real Build.bat or equivalent build invocation.
 *
 * Reference:
 *   - Dispatch: `AgentBridge/CODEX_TO_OSVAYDER.md` 2026-04-19 16:15.
 *   - Audit:    `Docs/OsvayderUE/626_P2_DetectorSourceAudit.md`.
 *   - Spec:     `Docs/OsvayderUE/626_CompileRoutingAndErrorSafetyCorrection_DispatchReady_v3.md` §P2 Layers A/B/C.
 *
 * 626 P2 Layer C: fixtures that would otherwise carry verbatim blocker
 * needles are composed via string concatenation so grep output of this
 * file does not leak contiguous blocker phrases. The runtime strings
 * constructed at test time are bit-identical to the real-world inputs.
 */

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "OsvayderUERestartSurvival.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace OsvayderUEDetectorOuroborosTestsDetail
{
	// Compose the UHT rename-lock phrase without a single contiguous literal.
	FString ComposeUhtRenameLockPhrase()
	{
		return FString(TEXT("Failed to "))
			+ TEXT("rename ")
			+ TEXT("exported ")
			+ TEXT("file ");
	}

	// Compose "Unable to build while Live Coding is active" split.
	FString ComposeLiveCodingUnablePhrase()
	{
		return FString(TEXT("Unable to ")) + TEXT("build while ") + TEXT("Live Coding ") + TEXT("is active.");
	}

	// Compose "being used by another process" split.
	FString ComposeFileLockPhrase()
	{
		return FString(TEXT("being used ")) + TEXT("by another ") + TEXT("process");
	}
}

using namespace OsvayderUEDetectorOuroborosTestsDetail;

// ================================================================
// A-P2-4.1 GrepOwnDetectorSource
// Simulated `rg` output that quotes the detector's own source lines.
// Each result line is in `path:line:content` format with the plugin
// source-tree path. Layer B origin exclusion must strip these lines
// and leave nothing to match. Layer A command-gate (rg = inspection)
// rejects even earlier. Either defense alone kills the escalation.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetectorNegative_GrepOwnDetectorSource,
	"OsvayderUE.RestartSurvival.DetectorNegative.GrepOwnDetectorSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDetectorNegative_GrepOwnDetectorSource::RunTest(const FString& /*Parameters*/)
{
	// Simulated ripgrep output lines. Contains the literal needle as part
	// of a TEXT(...) string on a detector source line, with path:line:content
	// prefix. Pre-P2 code would escalate via substring match on the needle.
	const FString SimulatedRgOutput =
		  FString(TEXT("Plugins/OsvayderUE/Source/OsvayderUE/Private/OsvayderUERestartSurvival.cpp:453:\t\t\tif (Haystack.Contains(TEXT(\"")) + ComposeUhtRenameLockPhrase() + TEXT("\"))\n")
		+ TEXT("Plugins/OsvayderUE/Source/OsvayderUE/Private/OsvayderUERestartSurvival.cpp:461:\t\t\t\tTEXT(\"") + ComposeUhtRenameLockPhrase() + TEXT("\"));\n");

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetected = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		TEXT("rg \"Failed to rename\" Plugins/OsvayderUE/Source/"), // ToolInput: rg inspection
		SimulatedRgOutput,
		FString(),
		Blocker);

	TestFalse(TEXT("rg output over detector source must not escalate"), bDetected);
	TestFalse(TEXT("blocker must remain unset"), Blocker.bDetected);
	return true;
}

// ================================================================
// A-P2-4.2 ReadObservedFailures
// `Get-Content` of observed_failures.md, which historically cites
// past real blocker phrases verbatim. Layer A rejects Get-Content.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetectorNegative_ReadObservedFailures,
	"OsvayderUE.RestartSurvival.DetectorNegative.ReadObservedFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDetectorNegative_ReadObservedFailures::RunTest(const FString& /*Parameters*/)
{
	// Simulated tail of observed_failures.md citing a past blocker.
	const FString SimulatedMarkdownBody =
		  FString(TEXT("## Row 28 - UHT rename lock incident 2025-10-11\n"))
		+ TEXT("The supervisor observed `") + ComposeUhtRenameLockPhrase()
		+ TEXT("Intermediate/Build/Win64/UnrealEditor/Inc/Poligon1/UHT/Foo.generated.h.tmp` in the log.\n")
		+ TEXT("Escalation to restart_survival closed the editor; build completed after reopen.\n");

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetected = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		TEXT("Get-Content Docs/OsvayderUE/observed_failures.md"),
		SimulatedMarkdownBody,
		FString(),
		Blocker);

	TestFalse(TEXT("Get-Content of observed_failures.md must not escalate"), bDetected);
	TestFalse(TEXT("blocker must remain unset"), Blocker.bDetected);
	return true;
}

// ================================================================
// A-P2-4.3 ReadSavedRelayArtifact
// Reading a stored relay_prompt.txt that contains archived blocker
// text from a prior real escalation. Layer B excludes Saved/OsvayderUE
// origin; Layer A rejects Get-Content.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetectorNegative_ReadSavedRelayArtifact,
	"OsvayderUE.RestartSurvival.DetectorNegative.ReadSavedRelayArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDetectorNegative_ReadSavedRelayArtifact::RunTest(const FString& /*Parameters*/)
{
	const FString SimulatedRelayPrompt =
		  FString(TEXT("Saved/OsvayderUE/relay_prompt.txt\n"))
		+ TEXT("Original blocker observed: ") + ComposeLiveCodingUnablePhrase() + TEXT("\n")
		+ TEXT("Use bounded build lane to close the detached lane and reopen editor.\n");

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetected = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		TEXT("Get-Content Saved/OsvayderUE/relay_prompt.txt"),
		SimulatedRelayPrompt,
		FString(),
		Blocker);

	TestFalse(TEXT("read-back of saved relay artifact must not escalate"), bDetected);
	return true;
}

// ================================================================
// A-P2-4.4 RgSearchBlockerPhrase
// ripgrep search output where the matched lines originate from
// Docs/OsvayderUE/*.md files. Path:line:content prefixes and
// excluded-origin match trip Layer B; Layer A rejects rg.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetectorNegative_RgSearchBlockerPhrase,
	"OsvayderUE.RestartSurvival.DetectorNegative.RgSearchBlockerPhrase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDetectorNegative_RgSearchBlockerPhrase::RunTest(const FString& /*Parameters*/)
{
	const FString SimulatedRgOutput =
		  FString(TEXT("Docs/OsvayderUE/626_CompileRoutingAndErrorSafetyCorrection_DispatchReady_v3.md:142:  - ")) + ComposeUhtRenameLockPhrase() + TEXT("...\n")
		+ TEXT("Docs/OsvayderUE/observed_failures.md:88:Past blocker string: ") + ComposeUhtRenameLockPhrase() + TEXT("\n");

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetected = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		FString(TEXT("rg \"")) + ComposeUhtRenameLockPhrase() + TEXT("\" Docs/OsvayderUE/"),
		SimulatedRgOutput,
		FString(),
		Blocker);

	TestFalse(TEXT("rg search over docs must not escalate"), bDetected);
	return true;
}

// ================================================================
// A-P2-4.5 AgentReasoningMentionsBlocker
// RawProviderEvent text contains agent's free-form reasoning mentioning
// the blocker phrase as hypothesis. ToolResultContent is a harmless
// file listing with no build signatures. Layer A fails to find a build
// context (ToolInput says Get-ChildItem, haystack has no build
// signature after Layer B strips reasoning paths). Detector bypasses.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetectorNegative_AgentReasoningMentionsBlocker,
	"OsvayderUE.RestartSurvival.DetectorNegative.AgentReasoningMentionsBlocker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDetectorNegative_AgentReasoningMentionsBlocker::RunTest(const FString& /*Parameters*/)
{
	const FString ReasoningPayload = FString::Printf(
		TEXT("{\"type\":\"reasoning_item\",\"text\":\"Let me check whether the build will %s or hit Live Coding issues before calling Build.bat. First I should inspect the current state with Get-ChildItem.\"}"),
		*ComposeUhtRenameLockPhrase());

	const FString HarmlessFileListing =
		  FString(TEXT("Directory: D:\\Project\\Plugins\\OsvayderUE\\Source\\OsvayderUE\\Private\n"))
		+ TEXT("Mode                 LastWriteTime         Length Name\n")
		+ TEXT("-a---           2026-04-19    15:36         123456 OsvayderEditorWidget.cpp\n");

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetected = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		TEXT("Get-ChildItem Plugins/OsvayderUE/Source/OsvayderUE/Private"),
		HarmlessFileListing,
		ReasoningPayload,
		Blocker);

	TestFalse(TEXT("agent reasoning mentioning blocker must not escalate"), bDetected);
	return true;
}

// ================================================================
// A-P2-5.1 UbtRenameLockOutput
// Real Build.bat invocation producing UHT rename-lock output. ToolInput
// is the literal Build.bat command; haystack contains a UBT signature.
// Must escalate as UhtGeneratedRenameLock.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetectorPositive_UbtRenameLockOutput,
	"OsvayderUE.RestartSurvival.DetectorPositive.UbtRenameLockOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDetectorPositive_UbtRenameLockOutput::RunTest(const FString& /*Parameters*/)
{
	const FString UbtOutput =
		  FString(TEXT("Running UnrealBuildTool: dotnet UnrealBuildTool.dll Poligon1Editor Win64 Development\n"))
		+ TEXT("Parsing headers for Poligon1Editor\n")
		+ TEXT("UnrealHeaderTool: Error: ") + ComposeUhtRenameLockPhrase()
		+ TEXT("Intermediate/Build/Win64/UnrealEditor/Inc/Poligon1/UHT/MyThing.generated.h.tmp because it is being used by another process.\n");

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetected = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		TEXT("Build.bat Poligon1Editor Win64 Development"),
		UbtOutput,
		FString(),
		Blocker);

	TestTrue(TEXT("real UBT rename-lock output must escalate"), bDetected);
	TestEqual(
		TEXT("family must be UhtGeneratedRenameLock"),
		Blocker.Family,
		EOsvayderUEClosedEditorBuildBlockerFamily::UhtGeneratedRenameLock);
	return true;
}

// ================================================================
// A-P2-5.2 LogBackupAccessDenied
// Real Build.bat producing UBT log-backup access-denied.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetectorPositive_LogBackupAccessDenied,
	"OsvayderUE.RestartSurvival.DetectorPositive.LogBackupAccessDenied",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDetectorPositive_LogBackupAccessDenied::RunTest(const FString& /*Parameters*/)
{
	const FString UbtLogBackupOutput =
		  FString(TEXT("Running UnrealBuildTool: dotnet UnrealBuildTool.dll Poligon1Editor Win64 Development\n"))
		+ TEXT("Unhandled exception. System.UnauthorizedAccessException: Access to the path is denied.\n")
		+ TEXT("   at EpicGames.Core.Log.BackupLogFile(...)\n")
		+ TEXT("   at EpicGames.Core.Log.AddFileWriter(...)\n");

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetected = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		TEXT("Build.bat Poligon1Editor Win64 Development"),
		UbtLogBackupOutput,
		FString(),
		Blocker);

	TestTrue(TEXT("real UBT log-backup access-denied must escalate"), bDetected);
	TestEqual(
		TEXT("family must be UbtLogBackupAccessDenied"),
		Blocker.Family,
		EOsvayderUEClosedEditorBuildBlockerFamily::UbtLogBackupAccessDenied);
	return true;
}

// ================================================================
// A-P2-5.3 LiveCodingUnableToBuild
// Real Build.bat hitting Live Coding guard.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetectorPositive_LiveCodingUnableToBuild,
	"OsvayderUE.RestartSurvival.DetectorPositive.LiveCodingUnableToBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDetectorPositive_LiveCodingUnableToBuild::RunTest(const FString& /*Parameters*/)
{
	const FString UbtLiveCodingOutput =
		  FString(TEXT("Running UnrealBuildTool: dotnet UnrealBuildTool.dll Poligon1Editor Win64 Development\n"))
		+ ComposeLiveCodingUnablePhrase() + TEXT("\n")
		+ TEXT("Close the editor before rebuilding.\n");

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetected = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		TEXT("Build.bat Poligon1Editor Win64 Development"),
		UbtLiveCodingOutput,
		FString(),
		Blocker);

	TestTrue(TEXT("real Live Coding unable-to-build must escalate"), bDetected);
	TestEqual(
		TEXT("family must be LiveCodingActive"),
		Blocker.Family,
		EOsvayderUEClosedEditorBuildBlockerFamily::LiveCodingActive);
	return true;
}

// ================================================================
// A-P2-5.4 IntermediateBuildFileLock
// Real cl.exe/link.exe hitting Intermediate/Build file lock.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetectorPositive_IntermediateBuildFileLock,
	"OsvayderUE.RestartSurvival.DetectorPositive.IntermediateBuildFileLock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDetectorPositive_IntermediateBuildFileLock::RunTest(const FString& /*Parameters*/)
{
	const FString CompilerOutput =
		  FString(TEXT("Running UnrealBuildTool: dotnet UnrealBuildTool.dll Poligon1Editor Win64 Development\n"))
		+ TEXT("LINK : fatal error LNK1168: cannot open D:/Project/Intermediate/Build/Win64/UnrealEditor/Poligon1.lib for writing. The file is ")
		+ ComposeFileLockPhrase() + TEXT(".\n");

	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetected = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		TEXT("Build.bat Poligon1Editor Win64 Development"),
		CompilerOutput,
		FString(),
		Blocker);

	TestTrue(TEXT("real Intermediate/Build file-lock must escalate"), bDetected);
	TestEqual(
		TEXT("family must be EditorOpenFileLock"),
		Blocker.Family,
		EOsvayderUEClosedEditorBuildBlockerFamily::EditorOpenFileLock);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
