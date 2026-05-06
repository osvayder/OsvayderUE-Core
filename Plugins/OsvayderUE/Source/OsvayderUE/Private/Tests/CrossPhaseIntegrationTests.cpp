// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 626 P5 Cross-Phase Integration Tests.
 *
 * Exercises interactions BETWEEN 626 P1/P2/P3/P4 components that the
 * isolated unit tests do NOT cover. All tests operate at the
 * helper/classifier/gate level — no live editor, no Slate construction
 * (automation-test boundary from P1 continues to hold).
 *
 * Reference:
 *   - Dispatch: `AgentBridge/CODEX_TO_OSVAYDER.md` 2026-04-19 18:35.
 *   - Audit:    `Docs/OsvayderUE/626_P5_AdversarialCoverageAudit.md`.
 *   - Spec:     `Docs/OsvayderUE/626_CompileRoutingAndErrorSafetyCorrection_DispatchReady_v3.md` §626-P5.
 *
 * Test grouping: `OsvayderUE.CrossPhaseIntegration.*`
 *
 * Layer C defense on this file's fixture needles: blocker phrases are
 * composed from string-concatenation helpers so grep output of this file
 * does not surface contiguous needles for the detector to re-consume.
 * Runtime strings remain byte-identical to the pre-P2 literals.
 */

#include "CoreMinimal.h"
#include "CompileIntentPolicyGate.h"
#include "Dom/JsonObject.h"
#include "MCP/Tools/MCPTool_LiveCodingCompile.h"
#include "Misc/AutomationTest.h"
#include "OsvayderUECommandClassification.h"
#include "OsvayderUERestartSurvival.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace OsvayderUECrossPhaseIntegrationTestsDetail
{
	// 626 P2 Layer C discipline: compose blocker needles from parts so this
	// file is itself safe to grep. Runtime string is the canonical needle.
	FString ComposeUhtRenameLockPhrase()
	{
		return FString(TEXT("Failed to "))
			+ TEXT("rename ")
			+ TEXT("exported ")
			+ TEXT("file ");
	}

	// Build a simulated `rg` result-body quoting the detector source's TEXT("...")
	// literals across multiple `path:line:content` lines. This is the canonical
	// Ouroboros input.
	FString BuildOuroborosRgOutputFixture()
	{
		const FString Needle = ComposeUhtRenameLockPhrase();
		return FString(TEXT("Plugins/OsvayderUE/Source/OsvayderUE/Private/OsvayderUERestartSurvival.cpp:453:\t\t\tif (Haystack.Contains(TEXT(\""))
			+ Needle
			+ TEXT("\"))\n")
			+ TEXT("Plugins/OsvayderUE/Source/OsvayderUE/Private/OsvayderUERestartSurvival.cpp:461:\t\t\t\tTEXT(\"")
			+ Needle + TEXT("\"));\n");
	}

	// Build a simulated REAL UBT rename-lock output body (what Build.bat would emit).
	FString BuildRealUbtRenameLockFixture()
	{
		return FString(TEXT("Running UnrealBuildTool: dotnet UnrealBuildTool.dll Poligon1Editor Win64 Development\n"))
			+ TEXT("Parsing headers for Poligon1Editor\n")
			+ TEXT("UnrealHeaderTool: Error: ") + ComposeUhtRenameLockPhrase()
			+ TEXT("Intermediate/Build/Win64/UnrealEditor/Inc/Poligon1/UHT/MyThing.generated.h.tmp because it is being used by another process.\n");
	}
}

using namespace OsvayderUECrossPhaseIntegrationTestsDetail;
using OsvayderUE::LiveCodingCompile::Testing::FLiveCodingTriangulationSignals;
using OsvayderUE::LiveCodingCompile::Testing::ClassifyRefreshStatusWithTriangulation;

// ================================================================
// Integration #1: InspectionCommandAgainstBlockerPhrase_DetectorRejectsAndPolicyAllows
// Classic Ouroboros: agent `rg`s blocker phrase against plugin source.
// Both P2 detector AND P3 policy gate must classify this as safe inspection.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrossPhaseIntegration_InspectionCommandAgainstBlockerPhrase_DetectorRejectsAndPolicyAllows,
	"OsvayderUE.CrossPhaseIntegration.InspectionCommandAgainstBlockerPhrase_DetectorRejectsAndPolicyAllows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrossPhaseIntegration_InspectionCommandAgainstBlockerPhrase_DetectorRejectsAndPolicyAllows::RunTest(const FString& /*Parameters*/)
{
	const FString ToolInput = FString(TEXT("rg \"")) + ComposeUhtRenameLockPhrase() + TEXT("\" Plugins/OsvayderUE/Source/");
	const FString ToolResultBody = BuildOuroborosRgOutputFixture();

	// === P2 detector path ===
	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetectorFired = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		ToolInput,
		ToolResultBody,
		FString(),
		Blocker);
	TestFalse(TEXT("P2 detector must reject inspection command (Layer A)"), bDetectorFired);
	TestFalse(TEXT("Blocker must remain undetected"), Blocker.bDetected);

	// === P3 policy gate path ===
	FCompileIntentPolicyInputs PolicyInputs;
	PolicyInputs.ToolName = TEXT("command_execution");
	PolicyInputs.ToolCommandText = ToolInput;
	PolicyInputs.ChangeClassification = FString();
	PolicyInputs.bEditorOpen = true;
	PolicyInputs.LaneProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	PolicyInputs.UserIntentText = FString();
	const FCompileIntentPolicyDecision Decision =
		FCompileIntentPolicyGate::EvaluateCompileIntent(PolicyInputs);
	TestEqual(TEXT("P3 policy gate must classify inspection command as Allow"),
		Decision.CommandClass, ECompileIntentCommandClass::Allow);
	TestEqual(TEXT("detector_context == inspection_command"),
		Decision.DetectorContext, FString(TEXT("inspection_command")));
	TestFalse(TEXT("bPolicyRedirectApplied == false on inspection"),
		Decision.bPolicyRedirectApplied);

	// === Cross-component consistency ===
	// Both components reach the same safe-passthrough conclusion.
	TestFalse(TEXT("Consistency: neither component escalates"),
		bDetectorFired || Decision.bPolicyRedirectApplied);
	return true;
}

// ================================================================
// Integration #2: RealBuildBlockerOutput_DetectorEscalatesPolicyEmitsAmbiguous
// Real Build.bat invocation with UBT rename-lock output. P2 detector fires;
// P3 policy gate (with unknown classification) routes to Ambiguous.
// Verifies detector escalation is not blocked by policy gate, and policy
// gate defensively defers to classifier preview when classification unknown.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrossPhaseIntegration_RealBuildBlockerOutput_DetectorEscalatesPolicyEmitsAmbiguous,
	"OsvayderUE.CrossPhaseIntegration.RealBuildBlockerOutput_DetectorEscalatesPolicyEmitsAmbiguous",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrossPhaseIntegration_RealBuildBlockerOutput_DetectorEscalatesPolicyEmitsAmbiguous::RunTest(const FString& /*Parameters*/)
{
	const FString ToolInput = TEXT("Build.bat Poligon1Editor Win64 Development");
	const FString ToolResultBody = BuildRealUbtRenameLockFixture();

	// === P2 detector path (must fire) ===
	FOsvayderUEClosedEditorBuildBlocker Blocker;
	const bool bDetectorFired = FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
		TEXT("command_execution"),
		ToolInput,
		ToolResultBody,
		FString(),
		Blocker);
	TestTrue(TEXT("P2 detector must fire on real UBT rename-lock"), bDetectorFired);
	TestEqual(TEXT("Detector family == UhtGeneratedRenameLock"),
		Blocker.Family,
		EOsvayderUEClosedEditorBuildBlockerFamily::UhtGeneratedRenameLock);

	// === P3 policy gate path (must route Ambiguous, not restart_survival) ===
	FCompileIntentPolicyInputs PolicyInputs;
	PolicyInputs.ToolName = TEXT("command_execution");
	PolicyInputs.ToolCommandText = ToolInput;
	PolicyInputs.ChangeClassification = FString(); // unknown
	PolicyInputs.bEditorOpen = true;
	PolicyInputs.LaneProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	PolicyInputs.UserIntentText = FString();
	const FCompileIntentPolicyDecision Decision =
		FCompileIntentPolicyGate::EvaluateCompileIntent(PolicyInputs);
	TestEqual(TEXT("P3 policy gate routes unknown+build to Ambiguous"),
		Decision.CommandClass, ECompileIntentCommandClass::Ambiguous);
	TestNotEqual(TEXT("Policy gate must NOT redirect to restart_survival on shell output alone"),
		Decision.RedirectTargetTool, FString(TEXT("restart_survival")));
	TestEqual(TEXT("Policy gate redirects to cpp_reflection for classification"),
		Decision.RedirectTargetTool, FString(TEXT("cpp_reflection")));

	// === Order of operations ===
	// Detector fires + arms restart_survival (higher-level widget integration);
	// policy gate annotates Ambiguous but does NOT block the detector's path.
	// Both signals go to the agent transcript / agent_trace; the widget
	// integration at HandleToolResultEvent calls detector FIRST, then policy.
	// This test proves both components can co-operate on the same input
	// without interference at the pure-evaluator layer.
	TestTrue(TEXT("Detector path independent of policy gate decision"),
		bDetectorFired);
	TestTrue(TEXT("Policy gate path independent of detector decision"),
		Decision.bPolicyRedirectApplied);
	return true;
}

// ================================================================
// Integration #3: ClassifierTriangulationConfirmsPatch_AfterPolicyRedirect
// End-to-end happy path: P3 denies Build.bat for body edit -> agent retries
// with livecoding_compile -> P4 triangulation classifier returns patched.
// Verifies policy-gate hint params align with what livecoding_compile needs.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrossPhaseIntegration_ClassifierTriangulationConfirmsPatch_AfterPolicyRedirect,
	"OsvayderUE.CrossPhaseIntegration.ClassifierTriangulationConfirmsPatch_AfterPolicyRedirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrossPhaseIntegration_ClassifierTriangulationConfirmsPatch_AfterPolicyRedirect::RunTest(const FString& /*Parameters*/)
{
	// === Step (a): P3 policy gate denies shell build for body_only_cpp edit ===
	FCompileIntentPolicyInputs PolicyInputs;
	PolicyInputs.ToolName = TEXT("command_execution");
	PolicyInputs.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	PolicyInputs.ChangeClassification = TEXT("body_only_cpp");
	PolicyInputs.bEditorOpen = true;
	PolicyInputs.LaneProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	PolicyInputs.UserIntentText = FString();

	const FCompileIntentPolicyDecision Decision =
		FCompileIntentPolicyGate::EvaluateCompileIntent(PolicyInputs);
	TestEqual(TEXT("Policy gate denies body edit shell build"),
		Decision.CommandClass, ECompileIntentCommandClass::DenyWithRedirect);
	TestEqual(TEXT("Redirect target tool == livecoding_compile"),
		Decision.RedirectTargetTool, FString(TEXT("livecoding_compile")));
	// Verify hint params contain the expected keys that livecoding_compile accepts.
	TestTrue(TEXT("Redirect params hint is valid JSON object"),
		Decision.RedirectTargetParamsHint.IsValid());
	if (Decision.RedirectTargetParamsHint.IsValid())
	{
		bool bWaitForCompletion = false;
		bool bAgentDiffExpected = false;
		bool bEnableForSession = false;
		TestTrue(TEXT("Hint includes wait_for_completion"),
			Decision.RedirectTargetParamsHint->TryGetBoolField(TEXT("wait_for_completion"), bWaitForCompletion));
		TestTrue(TEXT("Hint includes agent_diff_expected"),
			Decision.RedirectTargetParamsHint->TryGetBoolField(TEXT("agent_diff_expected"), bAgentDiffExpected));
		TestTrue(TEXT("Hint includes enable_for_session_if_needed"),
			Decision.RedirectTargetParamsHint->TryGetBoolField(TEXT("enable_for_session_if_needed"), bEnableForSession));
		TestTrue(TEXT("Hint wait_for_completion default true"), bWaitForCompletion);
		TestTrue(TEXT("Hint agent_diff_expected default true"), bAgentDiffExpected);
	}

	// === Step (b): Agent retries with livecoding_compile; P4 classifier confirms patch ===
	FLiveCodingTriangulationSignals Signals;
	Signals.bSuccess = false; // UE 5.7 LC quirk: returns Cancelled for no-reinstance
	Signals.bSignal1_PositiveReload = true;
	Signals.bSignal2_DllMtimeChanged = true;
	Signals.bSignal3_NoCompileErrors = true;
	Signals.bAnyDllMtimeMeasured = true;
	Signals.bHasCancelledMarker = true;
	Signals.bAgentDiffExpected = true; // per hint
	Signals.bResultWasNoChanges = false;

	const FString RefreshStatus = ClassifyRefreshStatusWithTriangulation(Signals);
	TestEqual(TEXT("P4 classifier confirms full triangulation patched"),
		RefreshStatus, FString(TEXT("live_coding_patched")));

	// === End-to-end assertion ===
	// Flow: Shell build denied with redirect -> LC called with hinted params -> patched confirmed.
	TestTrue(TEXT("End-to-end: policy redirect + triangulation together confirm patch path"),
		Decision.CommandClass == ECompileIntentCommandClass::DenyWithRedirect
		&& RefreshStatus.Equals(TEXT("live_coding_patched")));
	return true;
}

// ================================================================
// Integration #4: PolicyDenyJsonStructure_CanBeReParsedAsStructuredTelemetry
// Constructs a DenyWithRedirect decision, serializes via BuildPolicyRoutingJson,
// then asserts every field the widget-side annotation layer would consume.
// Proves round-trip JSON stability for the structured-deny contract.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrossPhaseIntegration_PolicyDenyJsonStructure_CanBeReParsedAsStructuredTelemetry,
	"OsvayderUE.CrossPhaseIntegration.PolicyDenyJsonStructure_CanBeReParsedAsStructuredTelemetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrossPhaseIntegration_PolicyDenyJsonStructure_CanBeReParsedAsStructuredTelemetry::RunTest(const FString& /*Parameters*/)
{
	// Construct a RequireRestart decision (most field-dense shape).
	FCompileIntentPolicyInputs In;
	In.ToolName = TEXT("command_execution");
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("reflected_structural");
	In.bEditorOpen = true;
	In.LaneProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	In.UserIntentText = FString();

	const FCompileIntentPolicyDecision Decision = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	const TSharedRef<FJsonObject> Json = FCompileIntentPolicyGate::BuildPolicyRoutingJson(Decision);

	// === Top-level fields ===
	bool bPolicyDenied = false;
	FString CommandClassStr;
	TestTrue(TEXT("policy_denied field"),
		Json->TryGetBoolField(TEXT("policy_denied"), bPolicyDenied));
	TestTrue(TEXT("policy_denied == true on RequireRestart"), bPolicyDenied);
	TestTrue(TEXT("command_class field"),
		Json->TryGetStringField(TEXT("command_class"), CommandClassStr));
	TestEqual(TEXT("command_class == require_restart"),
		CommandClassStr, FString(TEXT("require_restart")));

	// === All 6 structured-deny fields present ===
	const TSharedPtr<FJsonObject>* DenyObj = nullptr;
	TestTrue(TEXT("deny object"), Json->TryGetObjectField(TEXT("deny"), DenyObj));
	if (!DenyObj || !(*DenyObj).IsValid())
	{
		return false;
	}
	static const TArray<FString> RequiredDenyFields = {
		TEXT("deny_reason_code"),
		TEXT("deny_reason_text"),
		TEXT("redirect_target_tool"),
		TEXT("redirect_target_operation"),
		TEXT("redirect_target_params_hint"),
		TEXT("inferred_change_classification"),
		TEXT("allowed_contexts_where_this_would_succeed")
	};
	for (const FString& Field : RequiredDenyFields)
	{
		TestTrue(
			FString::Printf(TEXT("deny.%s present"), *Field),
			(*DenyObj)->HasField(Field));
	}

	// === All 10 telemetry fields present ===
	const TSharedPtr<FJsonObject>* TelemetryObj = nullptr;
	TestTrue(TEXT("telemetry object"),
		Json->TryGetObjectField(TEXT("telemetry"), TelemetryObj));
	if (!TelemetryObj || !(*TelemetryObj).IsValid())
	{
		return false;
	}
	static const TArray<FString> RequiredTelemetryFields = {
		TEXT("compile_intent"),
		TEXT("change_classification"),
		TEXT("chosen_compile_path"),
		TEXT("redirected_from_shell"),
		TEXT("requires_restart_reason"),
		TEXT("livecoding_allowed_reason"),
		TEXT("detector_context"),
		TEXT("agent_tool_choice"),
		TEXT("policy_redirect_applied"),
		TEXT("policy_redirect_target")
	};
	for (const FString& Field : RequiredTelemetryFields)
	{
		TestTrue(
			FString::Printf(TEXT("telemetry.%s present"), *Field),
			(*TelemetryObj)->HasField(Field));
	}

	// === Specific values for RequireRestart path ===
	FString RedirectTool;
	(*DenyObj)->TryGetStringField(TEXT("redirect_target_tool"), RedirectTool);
	TestEqual(TEXT("redirect_target_tool == restart_survival on RequireRestart"),
		RedirectTool, FString(TEXT("restart_survival")));

	FString ChosenPath;
	(*TelemetryObj)->TryGetStringField(TEXT("chosen_compile_path"), ChosenPath);
	TestEqual(TEXT("chosen_compile_path == restart_survival"),
		ChosenPath, FString(TEXT("restart_survival")));

	return true;
}

// ================================================================
// Integration #5: AmbiguousClassificationWiringGapIsDeliberate_DocumentsFutureCacheIntegration
// Documents today's deliberate behavior: empty ChangeClassification -> Ambiguous.
// Future-proof marker: if cache wiring is added, this test updates to use populated
// classification, surfacing the wiring change explicitly.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrossPhaseIntegration_AmbiguousClassificationWiringGapIsDeliberate,
	"OsvayderUE.CrossPhaseIntegration.AmbiguousClassificationWiringGapIsDeliberate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCrossPhaseIntegration_AmbiguousClassificationWiringGapIsDeliberate::RunTest(const FString& /*Parameters*/)
{
	// Today's widget-side hook passes empty ChangeClassification. Gate must
	// route build commands to Ambiguous in that case, NOT auto-deny. This
	// is v3-compliant + per deliberate design per P3 dispatch.
	FCompileIntentPolicyInputs In;
	In.ToolName = TEXT("command_execution");
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = FString(); // wiring gap: empty in today's production
	In.bEditorOpen = true;
	In.LaneProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	In.UserIntentText = FString();

	const FCompileIntentPolicyDecision Decision = FCompileIntentPolicyGate::EvaluateCompileIntent(In);

	// Deliberate behavior assertions:
	TestEqual(TEXT("Empty ChangeClassification + build cmd -> Ambiguous (deliberate)"),
		Decision.CommandClass, ECompileIntentCommandClass::Ambiguous);
	TestEqual(TEXT("Ambiguous redirects to cpp_reflection (preview for classification)"),
		Decision.RedirectTargetTool, FString(TEXT("cpp_reflection")));
	TestEqual(TEXT("ChosenCompilePath == ambiguous_no_route"),
		Decision.ChosenCompilePath, FString(TEXT("ambiguous_no_route")));
	TestEqual(TEXT("compile_intent == unknown"),
		Decision.CompileIntent, FString(TEXT("unknown")));

	// v3 rule: must NOT auto-escalate to restart_survival from shell output alone.
	TestNotEqual(TEXT("Gate must NOT route to restart_survival on ambiguous shell output"),
		Decision.RedirectTargetTool, FString(TEXT("restart_survival")));

	// If/when the classification cache is wired in a future stage, swapping
	// the empty classification for "body_only_cpp" or "reflected_structural"
	// should change the outcome — this test will need updating, and that
	// change will surface the cache-wiring diff explicitly.
	FCompileIntentPolicyInputs PopulatedIn = In;
	PopulatedIn.ChangeClassification = TEXT("body_only_cpp");
	const FCompileIntentPolicyDecision PopulatedDecision = FCompileIntentPolicyGate::EvaluateCompileIntent(PopulatedIn);
	TestEqual(TEXT("When classification is populated (future cache wiring), gate routes to specific redirect"),
		PopulatedDecision.CommandClass, ECompileIntentCommandClass::DenyWithRedirect);
	TestEqual(TEXT("Populated body_only_cpp redirects to livecoding_compile"),
		PopulatedDecision.RedirectTargetTool, FString(TEXT("livecoding_compile")));
	// This assertion is the forward-marker: the difference between the two
	// routing outcomes (Ambiguous -> cpp_reflection vs DenyWithRedirect ->
	// livecoding_compile) on the same ToolCommandText proves the gate
	// behavior IS sensitive to classification, and the production wiring
	// gap is the reason today's default is Ambiguous.
	TestNotEqual(TEXT("Different classifications produce different routing (gate is classification-sensitive)"),
		Decision.RedirectTargetTool, PopulatedDecision.RedirectTargetTool);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
