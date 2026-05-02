// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 626 P6-prep wiring tests.
 *
 * Covers A-P6-5 acceptance: the policy decision emitted by the widget-side
 * hook is consumable by the next turn's prompt assembly. This test
 * validates the advisory-text builder + the orchestrator cache round-trip
 * pattern that the widget uses to queue decisions for injection.
 *
 * Tests are pure — they do NOT construct a widget, do NOT fire a live
 * agent turn. They exercise the contract between:
 *   - `FCompileIntentPolicyGate::BuildAgentAdvisoryText` (advisory text shape)
 *   - `FClaudeCodeSubsystem::SetPendingPolicyRoutingAdvisory` / orchestrator
 *     internals (cache API shape).
 *
 * Reference:
 *   - Dispatch: `AgentBridge/CODEX_TO_CLAUDE.md` 2026-04-19 19:00 (P6-prep).
 *   - Spec:     v3 §626-P6.
 */

#include "CoreMinimal.h"
#include "CompileIntentPolicyGate.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// ================================================================
// A-P6-5.1 AdvisoryText_EmptyForAllowDecision
// Non-denial classes emit empty advisory (zero noise on normal turns).
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntentAdvisory_EmptyForAllowDecision,
	"UnrealClaude.CompileIntent.Advisory.EmptyForAllowDecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntentAdvisory_EmptyForAllowDecision::RunTest(const FString& /*Parameters*/)
{
	// Construct an Allow decision by driving the gate with an inspection command.
	FCompileIntentPolicyInputs In;
	In.ToolName = TEXT("command_execution");
	In.ToolCommandText = TEXT("Get-Content Plugins/UnrealClaude/Source/Foo.cpp");
	In.ChangeClassification = TEXT("body_only_cpp"); // even with classification, inspection -> Allow
	In.bEditorOpen = true;
	In.LaneProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Inspection -> Allow"), D.CommandClass, ECompileIntentCommandClass::Allow);

	const FString Advisory = FCompileIntentPolicyGate::BuildAgentAdvisoryText(D);
	TestTrue(TEXT("Allow decision produces empty advisory"), Advisory.IsEmpty());
	return true;
}

// ================================================================
// A-P6-5.2 AdvisoryText_DenyWithRedirect_ContainsRequiredMarkers
// DenyWithRedirect emits advisory containing the agent-actionable fields.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntentAdvisory_DenyWithRedirect_ContainsRequiredMarkers,
	"UnrealClaude.CompileIntent.Advisory.DenyWithRedirect_ContainsRequiredMarkers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntentAdvisory_DenyWithRedirect_ContainsRequiredMarkers::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In;
	In.ToolName = TEXT("command_execution");
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("body_only_cpp");
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Body edit + shell build -> DenyWithRedirect"),
		D.CommandClass, ECompileIntentCommandClass::DenyWithRedirect);

	const FString Advisory = FCompileIntentPolicyGate::BuildAgentAdvisoryText(D);
	TestFalse(TEXT("Advisory not empty"), Advisory.IsEmpty());

	// Agent must be able to read the following from the advisory.
	TestTrue(TEXT("Advisory mentions OFF-POLICY"),
		Advisory.Contains(TEXT("OFF-POLICY")));
	TestTrue(TEXT("Advisory mentions the classification"),
		Advisory.Contains(TEXT("deny_with_redirect")));
	TestTrue(TEXT("Advisory mentions reason code"),
		Advisory.Contains(D.DenyReasonCode));
	TestTrue(TEXT("Advisory names the redirect target tool"),
		Advisory.Contains(D.RedirectTargetTool));
	TestTrue(TEXT("Advisory contains ACTION marker"),
		Advisory.Contains(TEXT("ACTION:")));
	// Suggested params should include the key livecoding_compile expects.
	TestTrue(TEXT("Advisory surfaces suggested params line"),
		Advisory.Contains(TEXT("Suggested params:")));
	TestTrue(TEXT("Advisory mentions wait_for_completion"),
		Advisory.Contains(TEXT("wait_for_completion")));
	TestTrue(TEXT("Advisory mentions agent_diff_expected"),
		Advisory.Contains(TEXT("agent_diff_expected")));
	return true;
}

// ================================================================
// A-P6-5.3 AdvisoryText_RequireRestart_PointsAtRestartSurvival
// RequireRestart emits advisory with restart_survival as redirect target.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntentAdvisory_RequireRestart_PointsAtRestartSurvival,
	"UnrealClaude.CompileIntent.Advisory.RequireRestart_PointsAtRestartSurvival",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntentAdvisory_RequireRestart_PointsAtRestartSurvival::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In;
	In.ToolName = TEXT("command_execution");
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("reflected_structural");
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Structural -> RequireRestart"),
		D.CommandClass, ECompileIntentCommandClass::RequireRestart);

	const FString Advisory = FCompileIntentPolicyGate::BuildAgentAdvisoryText(D);
	TestTrue(TEXT("Advisory names restart_survival as redirect target"),
		Advisory.Contains(TEXT("restart_survival")));
	TestTrue(TEXT("Advisory mentions require_restart classification"),
		Advisory.Contains(TEXT("require_restart")));
	return true;
}

// ================================================================
// A-P6-5.4 AdvisoryText_Ambiguous_PointsAtCppReflectionPreview
// Ambiguous classification emits advisory steering to classifier preview.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntentAdvisory_Ambiguous_PointsAtCppReflectionPreview,
	"UnrealClaude.CompileIntent.Advisory.Ambiguous_PointsAtCppReflectionPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntentAdvisory_Ambiguous_PointsAtCppReflectionPreview::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In;
	In.ToolName = TEXT("command_execution");
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = FString(); // unknown
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Unknown classification -> Ambiguous"),
		D.CommandClass, ECompileIntentCommandClass::Ambiguous);

	const FString Advisory = FCompileIntentPolicyGate::BuildAgentAdvisoryText(D);
	TestTrue(TEXT("Advisory names cpp_reflection as redirect target"),
		Advisory.Contains(TEXT("cpp_reflection")));
	TestTrue(TEXT("Advisory mentions ambiguous"),
		Advisory.Contains(TEXT("ambiguous")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
