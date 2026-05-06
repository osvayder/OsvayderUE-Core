// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 631 Agent Self-Retrospective Policy unit tests.
 *
 * Covers:
 *   - Rule 1 plugin-side validation: DoesResponseClaimStatusFull classifier
 *     correctly detects `status: full` / `result: full` / markdown
 *     `**Result**` + `full` variants + rejects negative phrasings.
 *   - Rule 3 CanonLedger_PromotionSuggester.ps1 behavior: synthetic
 *     journal with 2+ matching pattern_keys -> suggestion emitted; 1
 *     matching pattern_key -> no suggestion.
 *
 * Rule 2 (mandatory retrospective on user-reported failure) is agent-
 * behavior-level, enforced via system-prompt text in AgentPromptContract.cpp
 * — no unit test for that; verified via prompt-text contains-check in
 * AgentPromptContract regression.
 *
 * Reference: dispatch `AgentBridge/CODEX_TO_OSVAYDER.md` 2026-04-20 01:15 (631).
 */

#include "CoreMinimal.h"
#include "OsvayderEditorWidget.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// ================================================================
// Test 1: DoesResponseClaimStatusFull classifier
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentSelfRetro_ResponseClaimStatusFull_ClassifierTruth,
	"OsvayderUE.AgentSelfRetro.ResponseClaimStatusFull_ClassifierTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAgentSelfRetro_ResponseClaimStatusFull_ClassifierTruth::RunTest(const FString& /*Parameters*/)
{
	// Positive cases: canonical `status: full` with various casings + spacing.
	TestTrue(TEXT("'Status: full' detected"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("Status: full")));
	TestTrue(TEXT("'status:full' (no space) detected"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("status:full")));
	TestTrue(TEXT("'STATUS : FULL' (uppercase + extra space) detected"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("STATUS : FULL")));
	TestTrue(TEXT("'status = full' (equals variant) detected"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("status = full")));
	TestTrue(TEXT("'result: full' detected"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("result: full")));
	TestTrue(TEXT("Markdown '**Result**' + '`full`' detected"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("**Result**\n\n`full`")));
	TestTrue(TEXT("Within a larger response body"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(
			TEXT("Implementation done. Status: full. Modified 3 files.")));

	// Negative cases: must NOT match partial / other-value / adversarial.
	TestFalse(TEXT("Empty string: no claim"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(FString()));
	TestFalse(TEXT("'status: partial' must NOT match"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("status: partial")));
	TestFalse(TEXT("'status: failed' must NOT match"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("status: failed")));
	TestFalse(TEXT("'status: unverified_manual' must NOT match"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("status: unverified_manual")));
	TestFalse(TEXT("'fully implemented' without status: prefix must NOT match"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("fully implemented the feature")));
	TestFalse(TEXT("'full implementation' without status: prefix must NOT match"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("full implementation of feature")));

	// Word-boundary guard: 'status: fullscreen' is a different property.
	TestFalse(TEXT("'status: fullscreen' must NOT match (word boundary guard)"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(TEXT("status: fullscreen mode enabled")));

	// Mixed-content stress: a partial claim followed by a full claim should match.
	TestTrue(TEXT("Second occurrence is full after first partial"),
		SOsvayderEditorWidget::DoesResponseClaimStatusFull(
			TEXT("first pass status: partial, after retry status: full.")));

	return true;
}

// ================================================================
// Helper: run the PromotionSuggester PowerShell script against a fixture
// ================================================================

namespace
{
	bool RunPromotionSuggesterAndReadOutput(
		const FString& FixturePath,
		FString& OutStdoutCombined,
		int32& OutExitCode)
	{
		IPluginManager& PM = IPluginManager::Get();
		TSharedPtr<IPlugin> Plugin = PM.FindPlugin(TEXT("OsvayderUE"));
		if (!Plugin.IsValid())
		{
			return false;
		}
		const FString ScriptPath = FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Script"),
			TEXT("CanonLedger_PromotionSuggester.ps1"));
		if (!IFileManager::Get().FileExists(*ScriptPath))
		{
			return false;
		}

		// Invoke PowerShell in a blocking child process.
		const FString Args = FString::Printf(
			TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -ObservedFailuresPath \"%s\" -Json"),
			*ScriptPath,
			*FixturePath);

		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
		{
			return false;
		}
		uint32 ProcessId = 0;
		FProcHandle Proc = FPlatformProcess::CreateProc(
			TEXT("powershell.exe"),
			*Args,
			/*bLaunchDetached=*/false,
			/*bLaunchHidden=*/true,
			/*bLaunchReallyHidden=*/true,
			&ProcessId,
			/*PriorityModifier=*/0,
			/*OptionalWorkingDirectory=*/nullptr,
			WritePipe,
			/*PipeReadChild=*/nullptr);
		if (!Proc.IsValid())
		{
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
			return false;
		}

		FString Accumulated;
		while (FPlatformProcess::IsProcRunning(Proc))
		{
			Accumulated += FPlatformProcess::ReadPipe(ReadPipe);
			FPlatformProcess::Sleep(0.05f);
		}
		// Drain remainder.
		Accumulated += FPlatformProcess::ReadPipe(ReadPipe);

		int32 ExitCode = INDEX_NONE;
		FPlatformProcess::GetProcReturnCode(Proc, &ExitCode);
		FPlatformProcess::CloseProc(Proc);
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

		OutStdoutCombined = Accumulated;
		OutExitCode = ExitCode;
		return true;
	}

	bool WriteSyntheticFixture(const FString& Path, const FString& Body)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(
			Body, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString SuggesterFixtureRoot()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("Automation"), TEXT("AgentSelfRetro"));
	}
}

// ================================================================
// Test 2: suggester emits recommendation when count >= 2
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentSelfRetro_Suggester_EmitsRecommendationForRepeatedPattern,
	"OsvayderUE.AgentSelfRetro.Suggester_EmitsRecommendationForRepeatedPattern",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAgentSelfRetro_Suggester_EmitsRecommendationForRepeatedPattern::RunTest(const FString& /*Parameters*/)
{
	const FString FixturePath = FPaths::Combine(SuggesterFixtureRoot(), TEXT("fixture_two_matching.md"));

	const FString Body = TEXT(
		"# Observed Agent Failures\n"
		"\n"
		"## Agent Retrospectives\n"
		"\n"
		"### 2026-04-20: Task A broken — category_x\n"
		"\n"
		"**Task**: first repro\n"
		"**Agent approach**: whatever\n"
		"**Confidence basis**: tier 3\n"
		"**Automated verification**: skipped\n"
		"**User observation**: \"does not work\"\n"
		"**Root cause hypothesis**: hypothesis one\n"
		"**Canon ledger candidate**: pattern_key: repeated_shared_pattern, short_title: the pattern\n"
		"**Retry plan**: retry A\n"
		"\n"
		"### 2026-04-20: Task B broken — category_y\n"
		"\n"
		"**Task**: second repro\n"
		"**Agent approach**: whatever else\n"
		"**Confidence basis**: tier 2\n"
		"**Automated verification**: skipped\n"
		"**User observation**: \"also broken\"\n"
		"**Root cause hypothesis**: hypothesis two\n"
		"**Canon ledger candidate**: pattern_key: repeated_shared_pattern, short_title: the pattern again\n"
		"**Retry plan**: retry B\n"
		"\n"
		"## Journal\n"
	);
	if (!TestTrue(TEXT("Fixture must be written successfully"),
		WriteSyntheticFixture(FixturePath, Body)))
	{
		return false;
	}

	FString Stdout;
	int32 ExitCode = INDEX_NONE;
	const bool bRan = RunPromotionSuggesterAndReadOutput(FixturePath, Stdout, ExitCode);
	TestTrue(TEXT("Suggester script must run to completion"), bRan);
	TestEqual(TEXT("Exit code must be 0"), ExitCode, 0);
	TestTrue(TEXT("Output must mention the shared pattern_key"),
		Stdout.Contains(TEXT("repeated_shared_pattern")));
	// JSON output should carry a suggestions array with 1 entry.
	TestTrue(TEXT("JSON must declare a suggestions array"),
		Stdout.Contains(TEXT("suggestions")));
	TestTrue(TEXT("JSON must record entry_count at or above 2"),
		Stdout.Contains(TEXT("entry_count"))
			&& (Stdout.Contains(TEXT("\"entry_count\": 2"))
				|| Stdout.Contains(TEXT("\"entry_count\":  2"))));
	return true;
}

// ================================================================
// Test 3: suggester emits NO recommendation when count < 2
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentSelfRetro_Suggester_NoRecommendationForSingleEntry,
	"OsvayderUE.AgentSelfRetro.Suggester_NoRecommendationForSingleEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAgentSelfRetro_Suggester_NoRecommendationForSingleEntry::RunTest(const FString& /*Parameters*/)
{
	const FString FixturePath = FPaths::Combine(SuggesterFixtureRoot(), TEXT("fixture_single_entry.md"));

	const FString Body = TEXT(
		"# Observed Agent Failures\n"
		"\n"
		"## Agent Retrospectives\n"
		"\n"
		"### 2026-04-20: Task A broken — category_x\n"
		"\n"
		"**Task**: only repro\n"
		"**Agent approach**: whatever\n"
		"**Confidence basis**: tier 3\n"
		"**Automated verification**: skipped\n"
		"**User observation**: \"does not work\"\n"
		"**Root cause hypothesis**: hypothesis\n"
		"**Canon ledger candidate**: pattern_key: singleton_pattern, short_title: only seen once\n"
		"**Retry plan**: retry A\n"
		"\n"
		"## Journal\n"
	);
	if (!TestTrue(TEXT("Fixture must be written successfully"),
		WriteSyntheticFixture(FixturePath, Body)))
	{
		return false;
	}

	FString Stdout;
	int32 ExitCode = INDEX_NONE;
	const bool bRan = RunPromotionSuggesterAndReadOutput(FixturePath, Stdout, ExitCode);
	TestTrue(TEXT("Suggester script must run to completion"), bRan);
	TestEqual(TEXT("Exit code must be 0"), ExitCode, 0);
	// Expect empty suggestions array in JSON output.
	TestTrue(TEXT("JSON output must include 'suggestions' key"),
		Stdout.Contains(TEXT("suggestions")));
	// Either [] or omitted — both acceptable; assert NO match on the specific key.
	TestFalse(
		TEXT("Must NOT emit a suggestion for a single-entry pattern"),
		Stdout.Contains(TEXT("singleton_pattern"))
			&& Stdout.Contains(TEXT("Recommendation")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
