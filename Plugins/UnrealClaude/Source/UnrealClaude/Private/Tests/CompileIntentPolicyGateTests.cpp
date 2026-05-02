// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 626 P3 compile-intent policy gate tests.
 *
 * Covers:
 *   A-P3-1  `FCompileIntentPolicyGate::EvaluateCompileIntent` exists + pure.
 *   A-P3-2  5 inputs (ToolName, ToolCommandText, ChangeClassification,
 *           bEditorOpen, LaneProfile, UserIntentText) each influence routing.
 *   A-P3-3  4 command classes (Allow / DenyWithRedirect / RequireRestart /
 *           Ambiguous) all exercised.
 *   A-P3-4  Deny response shape: 6-field structured JSON with redirect_target_tool,
 *           redirect_target_operation, redirect_target_params_hint,
 *           inferred_change_classification, allowed_contexts_where_this_would_succeed,
 *           deny_reason_code + deny_reason_text.
 *   A-P3-5  10 telemetry fields always emitted under `policy_routing.telemetry`.
 *   A-P3-7  15+ tests across the decision matrix + edge cases.
 *   A-P3-11 P2 helpers are reused (inherit from UnrealClaude::CommandClassification
 *           namespace; not duplicated inside the gate).
 *
 * Test grouping:
 *   - `UnrealClaude.CompileIntent.PolicyGate.*` -- direct gate decision tests.
 *   - `UnrealClaude.CompileIntent.Json.*` -- JSON shape verification.
 *   - `UnrealClaude.CompileIntent.Helpers.*` -- user-intent scan, enum-string.
 *
 * Reference:
 *   - Dispatch: `AgentBridge/CODEX_TO_CLAUDE.md` 2026-04-19 17:35.
 *   - Spec:     `Docs/UnrealClaude/626_CompileRoutingAndErrorSafetyCorrection_DispatchReady_v3.md` §626-P3.
 */

#include "CoreMinimal.h"
#include "CompileIntentPolicyGate.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UnrealClaudeCompileIntentTestsDetail
{
	FCompileIntentPolicyInputs MakeCommandExecutionBaseline()
	{
		FCompileIntentPolicyInputs In;
		In.ToolName = TEXT("command_execution");
		In.ToolCommandText = FString();
		In.ChangeClassification = FString();
		In.bEditorOpen = true;
		In.LaneProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
		In.UserIntentText = FString();
		return In;
	}
}

using namespace UnrealClaudeCompileIntentTestsDetail;

// ================================================================
// A-P3-3.1 BodyEditShellBuildEditorOpen_DeniesAndRedirectsToLiveCoding
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_BodyEditShellBuildEditorOpen_DeniesAndRedirectsToLiveCoding,
	"UnrealClaude.CompileIntent.PolicyGate.BodyEditShellBuildEditorOpen_DeniesAndRedirectsToLiveCoding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_BodyEditShellBuildEditorOpen_DeniesAndRedirectsToLiveCoding::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("body_only_cpp");
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Body edit + shell build + editor open -> DenyWithRedirect"),
		D.CommandClass, ECompileIntentCommandClass::DenyWithRedirect);
	TestEqual(TEXT("Redirect target tool == livecoding_compile"),
		D.RedirectTargetTool, FString(TEXT("livecoding_compile")));
	TestEqual(TEXT("ChosenCompilePath == denied_redirect"),
		D.ChosenCompilePath, FString(TEXT("denied_redirect")));
	TestTrue(TEXT("bRedirectedFromShell"), D.bRedirectedFromShell);
	TestTrue(TEXT("bPolicyRedirectApplied"), D.bPolicyRedirectApplied);
	TestEqual(TEXT("compile_intent == body_edit"),
		D.CompileIntent, FString(TEXT("body_edit")));
	TestEqual(TEXT("detector_context == editor_open_body_edit_context"),
		D.DetectorContext, FString(TEXT("editor_open_body_edit_context")));
	TestFalse(TEXT("DenyReasonCode not empty"), D.DenyReasonCode.IsEmpty());
	TestFalse(TEXT("DenyReasonText not empty"), D.DenyReasonText.IsEmpty());
	return true;
}

// ================================================================
// A-P3-3.2 ReflectedHeaderShellBuildEditorOpen_DeniesAndRedirectsToRestartSurvival
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_ReflectedHeaderShellBuildEditorOpen_DeniesAndRedirectsToRestartSurvival,
	"UnrealClaude.CompileIntent.PolicyGate.ReflectedHeaderShellBuildEditorOpen_DeniesAndRedirectsToRestartSurvival",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_ReflectedHeaderShellBuildEditorOpen_DeniesAndRedirectsToRestartSurvival::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("reflected_structural");
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Reflected structural + shell build -> RequireRestart"),
		D.CommandClass, ECompileIntentCommandClass::RequireRestart);
	TestEqual(TEXT("Redirect target tool == restart_survival"),
		D.RedirectTargetTool, FString(TEXT("restart_survival")));
	TestEqual(TEXT("ChosenCompilePath == restart_survival"),
		D.ChosenCompilePath, FString(TEXT("restart_survival")));
	TestFalse(TEXT("RequiresRestartReason not empty"), D.RequiresRestartReason.IsEmpty());
	TestEqual(TEXT("compile_intent == structural_add"),
		D.CompileIntent, FString(TEXT("structural_add")));
	TestTrue(TEXT("detector_context should preserve editor-open structural context"),
		D.DetectorContext.StartsWith(TEXT("editor_open_structural_context")));
	TestTrue(TEXT("detector_context should explain restart-survival redirect"),
		D.DetectorContext.Contains(TEXT("restart_survival")));
	return true;
}

// ================================================================
// A-P3-3.3 BodyEditShellBuildEditorClosed_Allows (CI/headless legitimate)
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_BodyEditShellBuildEditorClosed_Allows,
	"UnrealClaude.CompileIntent.PolicyGate.BodyEditShellBuildEditorClosed_Allows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_BodyEditShellBuildEditorClosed_Allows::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("body_only_cpp");
	In.bEditorOpen = false; // editor closed (CI / headless)

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Editor closed -> Allow even for body edit shell build"),
		D.CommandClass, ECompileIntentCommandClass::Allow);
	TestEqual(TEXT("compile_intent == full_rebuild"),
		D.CompileIntent, FString(TEXT("full_rebuild")));
	TestEqual(TEXT("detector_context == editor_closed"),
		D.DetectorContext, FString(TEXT("editor_closed")));
	TestFalse(TEXT("bPolicyRedirectApplied == false on Allow"), D.bPolicyRedirectApplied);
	return true;
}

// ================================================================
// A-P3-3.4 BodyEditShellBuildExpertLane_Allows
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_BodyEditShellBuildExpertLane_Allows,
	"UnrealClaude.CompileIntent.PolicyGate.BodyEditShellBuildExpertLane_Allows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_BodyEditShellBuildExpertLane_Allows::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("body_only_cpp");
	In.bEditorOpen = true; // editor open but...
	In.LaneProfile = EAgentExecutionRunProfile::ExplicitExpertOptIn; // ...expert lane override

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Expert lane -> Allow"),
		D.CommandClass, ECompileIntentCommandClass::Allow);
	TestEqual(TEXT("detector_context == expert_manual_lane"),
		D.DetectorContext, FString(TEXT("expert_manual_lane")));
	return true;
}

// ================================================================
// A-P3-3.5 InspectionCommand_AlwaysAllows
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_InspectionCommand_AlwaysAllows,
	"UnrealClaude.CompileIntent.PolicyGate.InspectionCommand_AlwaysAllows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_InspectionCommand_AlwaysAllows::RunTest(const FString& /*Parameters*/)
{
	// Verify multiple inspection commands all pass through.
	const TArray<FString> InspectionCommands = {
		TEXT("Get-Content D:/Project/Plugins/UnrealClaude/Source/Foo.cpp"),
		TEXT("rg \"UPROPERTY\" Plugins/UnrealClaude/Source/"),
		TEXT("findstr /R \"class.*API\" *.h"),
		TEXT("git status"),
		TEXT("git diff HEAD~1"),
		TEXT("Get-ChildItem Plugins -Recurse"),
		TEXT("Test-Path D:/Project/GDR_Shooter_Y.uproject"),
		TEXT("head -n 20 README.md")
	};
	for (const FString& Cmd : InspectionCommands)
	{
		FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
		In.ToolCommandText = Cmd;
		In.ChangeClassification = TEXT("body_only_cpp"); // would matter for a build cmd
		In.bEditorOpen = true;

		const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
		TestEqual(
			FString::Printf(TEXT("Inspection command '%s' -> Allow"), *Cmd),
			D.CommandClass, ECompileIntentCommandClass::Allow);
		TestEqual(
			FString::Printf(TEXT("Inspection '%s' compile_intent == inspection"), *Cmd),
			D.CompileIntent, FString(TEXT("inspection")));
		TestEqual(
			FString::Printf(TEXT("Inspection '%s' detector_context == inspection_command"), *Cmd),
			D.DetectorContext, FString(TEXT("inspection_command")));
	}
	return true;
}

// ================================================================
// A-P3-3.6 AmbiguousChange_NoAutoEscalation
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_AmbiguousChange_NoAutoEscalation,
	"UnrealClaude.CompileIntent.PolicyGate.AmbiguousChange_NoAutoEscalation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_AmbiguousChange_NoAutoEscalation::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = FString(); // unknown
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Unknown classification + build-like command -> Ambiguous"),
		D.CommandClass, ECompileIntentCommandClass::Ambiguous);
	TestNotEqual(TEXT("Must NOT auto-route to restart_survival"),
		D.RedirectTargetTool, FString(TEXT("restart_survival")));
	TestEqual(TEXT("Redirects to cpp_reflection for preview"),
		D.RedirectTargetTool, FString(TEXT("cpp_reflection")));
	TestEqual(TEXT("ChosenCompilePath == ambiguous_no_route"),
		D.ChosenCompilePath, FString(TEXT("ambiguous_no_route")));
	TestEqual(TEXT("compile_intent == unknown"),
		D.CompileIntent, FString(TEXT("unknown")));
	return true;
}

// ================================================================
// A-P3-3.7 UserIntentOverride_RebuildEverything_Allows
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_UserIntentOverride_RebuildEverything_Allows,
	"UnrealClaude.CompileIntent.PolicyGate.UserIntentOverride_RebuildEverything_Allows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_UserIntentOverride_RebuildEverything_Allows::RunTest(const FString& /*Parameters*/)
{
	// Verify multiple override phrases all admit through.
	const TArray<FString> OverridePrompts = {
		TEXT("Rebuild everything from scratch after my changes land."),
		TEXT("Do a full rebuild to check nothing broke."),
		TEXT("Please don't use live coding -- force rebuild."),
		TEXT("Manual compile requested. Skip live coding."),
		TEXT("Force full rebuild to verify the release pipeline.")
	};
	for (const FString& Prompt : OverridePrompts)
	{
		FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
		In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
		In.ChangeClassification = TEXT("body_only_cpp");
		In.bEditorOpen = true;
		In.UserIntentText = Prompt;

		const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
		TestEqual(
			FString::Printf(TEXT("User-intent override '%s' -> Allow"), *Prompt),
			D.CommandClass, ECompileIntentCommandClass::Allow);
		TestEqual(
			FString::Printf(TEXT("Override '%s' compile_intent == full_rebuild"), *Prompt),
			D.CompileIntent, FString(TEXT("full_rebuild")));
		TestEqual(
			FString::Printf(TEXT("Override '%s' detector_context == user_intent_rebuild_override"), *Prompt),
			D.DetectorContext, FString(TEXT("user_intent_rebuild_override")));
	}
	return true;
}

// ================================================================
// A-P3-4 RedirectResponseShape_MachineParseable
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_RedirectResponseShape_MachineParseable,
	"UnrealClaude.CompileIntent.Json.RedirectResponseShape_MachineParseable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_RedirectResponseShape_MachineParseable::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("body_only_cpp");

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	const TSharedRef<FJsonObject> Json = FCompileIntentPolicyGate::BuildPolicyRoutingJson(D);

	// Top-level fields.
	bool bPolicyDenied = false;
	TestTrue(TEXT("policy_denied field present"),
		Json->TryGetBoolField(TEXT("policy_denied"), bPolicyDenied));
	TestTrue(TEXT("policy_denied == true on DenyWithRedirect"), bPolicyDenied);

	FString CommandClassStr;
	TestTrue(TEXT("command_class field present"),
		Json->TryGetStringField(TEXT("command_class"), CommandClassStr));
	TestEqual(TEXT("command_class == deny_with_redirect"),
		CommandClassStr, FString(TEXT("deny_with_redirect")));

	// Structured deny body: 6 fields.
	const TSharedPtr<FJsonObject>* DenyObj = nullptr;
	TestTrue(TEXT("deny object present"),
		Json->TryGetObjectField(TEXT("deny"), DenyObj));
	if (!DenyObj || !DenyObj->IsValid())
	{
		return false;
	}
	FString ReasonCode, ReasonText, TargetTool, TargetOp, InferredCls;
	TestTrue(TEXT("deny.deny_reason_code present"),
		(*DenyObj)->TryGetStringField(TEXT("deny_reason_code"), ReasonCode));
	TestTrue(TEXT("deny.deny_reason_text present"),
		(*DenyObj)->TryGetStringField(TEXT("deny_reason_text"), ReasonText));
	TestTrue(TEXT("deny.redirect_target_tool present"),
		(*DenyObj)->TryGetStringField(TEXT("redirect_target_tool"), TargetTool));
	TestEqual(TEXT("redirect_target_tool == livecoding_compile"),
		TargetTool, FString(TEXT("livecoding_compile")));
	TestTrue(TEXT("deny.redirect_target_operation present"),
		(*DenyObj)->TryGetStringField(TEXT("redirect_target_operation"), TargetOp));
	const TSharedPtr<FJsonObject>* HintObj = nullptr;
	TestTrue(TEXT("deny.redirect_target_params_hint present"),
		(*DenyObj)->TryGetObjectField(TEXT("redirect_target_params_hint"), HintObj));
	TestTrue(TEXT("deny.inferred_change_classification present"),
		(*DenyObj)->TryGetStringField(TEXT("inferred_change_classification"), InferredCls));
	const TArray<TSharedPtr<FJsonValue>>* AllowedCtxArr = nullptr;
	TestTrue(TEXT("deny.allowed_contexts_where_this_would_succeed present"),
		(*DenyObj)->TryGetArrayField(TEXT("allowed_contexts_where_this_would_succeed"), AllowedCtxArr));
	if (AllowedCtxArr)
	{
		TestTrue(TEXT("allowed_contexts has at least one entry"),
			AllowedCtxArr->Num() >= 1);
	}
	return true;
}

// ================================================================
// A-P3-5 TelemetryFields_AllTenEmitted
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_TelemetryFields_AllTenEmitted,
	"UnrealClaude.CompileIntent.Json.TelemetryFields_AllTenEmitted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_TelemetryFields_AllTenEmitted::RunTest(const FString& /*Parameters*/)
{
	// Exercise all 4 command classes and verify every decision path emits
	// the full 10-field telemetry block.
	struct FCase
	{
		FString Name;
		FCompileIntentPolicyInputs In;
	};

	TArray<FCase> Cases;
	{
		FCase C;
		C.Name = TEXT("Allow-inspection");
		C.In = MakeCommandExecutionBaseline();
		C.In.ToolCommandText = TEXT("Get-Content foo.cpp");
		Cases.Add(C);
	}
	{
		FCase C;
		C.Name = TEXT("DenyWithRedirect-body");
		C.In = MakeCommandExecutionBaseline();
		C.In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
		C.In.ChangeClassification = TEXT("body_only_cpp");
		Cases.Add(C);
	}
	{
		FCase C;
		C.Name = TEXT("RequireRestart-structural");
		C.In = MakeCommandExecutionBaseline();
		C.In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
		C.In.ChangeClassification = TEXT("reflected_structural");
		Cases.Add(C);
	}
	{
		FCase C;
		C.Name = TEXT("Ambiguous-no-classification");
		C.In = MakeCommandExecutionBaseline();
		C.In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
		// ChangeClassification left empty
		Cases.Add(C);
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

	for (const FCase& Case : Cases)
	{
		const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(Case.In);
		const TSharedRef<FJsonObject> Json = FCompileIntentPolicyGate::BuildPolicyRoutingJson(D);

		const TSharedPtr<FJsonObject>* TelemetryObj = nullptr;
		TestTrue(
			FString::Printf(TEXT("Case '%s': telemetry block present"), *Case.Name),
			Json->TryGetObjectField(TEXT("telemetry"), TelemetryObj));
		if (!TelemetryObj || !TelemetryObj->IsValid())
		{
			continue;
		}
		for (const FString& FieldName : RequiredTelemetryFields)
		{
			TestTrue(
				FString::Printf(TEXT("Case '%s': telemetry.%s present"), *Case.Name, *FieldName),
				(*TelemetryObj)->HasField(FieldName));
		}
	}
	return true;
}

// ================================================================
// A-P3-4 StructuredDenyPreservesAgentReasoningChain
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_StructuredDenyPreservesAgentReasoningChain,
	"UnrealClaude.CompileIntent.PolicyGate.StructuredDenyPreservesAgentReasoningChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_StructuredDenyPreservesAgentReasoningChain::RunTest(const FString& /*Parameters*/)
{
	// Non-silent rewrite: gate produces a deny DECISION; it does NOT mutate
	// the input ToolName or ToolCommandText. Agent observes the deny +
	// chooses retry path explicitly.
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("body_only_cpp");
	const FString OriginalTool = In.ToolName;
	const FString OriginalCmd = In.ToolCommandText;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);

	// Input struct is unchanged (pure evaluator).
	TestEqual(TEXT("ToolName input unchanged"), In.ToolName, OriginalTool);
	TestEqual(TEXT("ToolCommandText input unchanged"), In.ToolCommandText, OriginalCmd);

	// AgentToolChoice telemetry echoes the original tool the agent tried.
	TestEqual(TEXT("Telemetry AgentToolChoice echoes original"),
		D.AgentToolChoice, OriginalTool);

	// RedirectTargetTool is NOT the same as the original tool.
	TestNotEqual(TEXT("Redirect target differs from original"),
		D.RedirectTargetTool, OriginalTool);
	return true;
}

// ================================================================
// Additional edge-case tests (A-P3-7 "5+ additional"):
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_MalformedCommand_EmptyString,
	"UnrealClaude.CompileIntent.PolicyGate.Edge.MalformedCommand_EmptyString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_MalformedCommand_EmptyString::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = FString();
	In.ChangeClassification = TEXT("body_only_cpp");
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	// Empty command is not inspection, not build -> Allow (non-build shell command path).
	TestEqual(TEXT("Empty command text -> Allow"),
		D.CommandClass, ECompileIntentCommandClass::Allow);
	TestEqual(TEXT("detector_context == non_build_shell_command"),
		D.DetectorContext, FString(TEXT("non_build_shell_command")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_NonCommandExecutionTool_AlwaysAllow,
	"UnrealClaude.CompileIntent.PolicyGate.Edge.NonCommandExecutionTool_AlwaysAllow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_NonCommandExecutionTool_AlwaysAllow::RunTest(const FString& /*Parameters*/)
{
	// A tool that isn't command_execution (e.g. blueprint_modify) should
	// pass through unconditionally -- the policy gate only governs shell builds.
	const TArray<FString> OtherTools = {
		TEXT("blueprint_modify"),
		TEXT("livecoding_compile"),
		TEXT("restart_survival"),
		TEXT("cpp_reflection"),
		TEXT("spawn_actor")
	};
	for (const FString& Tool : OtherTools)
	{
		FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
		In.ToolName = Tool;
		In.ToolCommandText = TEXT("Build.bat something");
		In.ChangeClassification = TEXT("reflected_structural");

		const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
		TestEqual(
			FString::Printf(TEXT("Tool '%s' passes through regardless of other inputs"), *Tool),
			D.CommandClass, ECompileIntentCommandClass::Allow);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_MetaOnlyUpropertyEdit_RedirectsToLiveCoding,
	"UnrealClaude.CompileIntent.PolicyGate.Edge.MetaOnlyUpropertyEdit_RedirectsToLiveCoding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_MetaOnlyUpropertyEdit_RedirectsToLiveCoding::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("meta_only_uproperty");
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("Meta-only change -> DenyWithRedirect"),
		D.CommandClass, ECompileIntentCommandClass::DenyWithRedirect);
	TestEqual(TEXT("Redirect target == livecoding_compile"),
		D.RedirectTargetTool, FString(TEXT("livecoding_compile")));
	TestEqual(TEXT("compile_intent == meta_edit"),
		D.CompileIntent, FString(TEXT("meta_edit")));
	TestEqual(TEXT("LiveCodingAllowedReason == meta_only_uproperty_change"),
		D.LiveCodingAllowedReason, FString(TEXT("meta_only_uproperty_change")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_NonCppChange_RedirectsToLiveCodingDiagnostic,
	"UnrealClaude.CompileIntent.PolicyGate.Edge.NonCppChange_RedirectsToLiveCodingDiagnostic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_NonCppChange_RedirectsToLiveCodingDiagnostic::RunTest(const FString& /*Parameters*/)
{
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("Build.bat Poligon1Editor Win64 Development");
	In.ChangeClassification = TEXT("non_cpp");
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("non_cpp change -> DenyWithRedirect"),
		D.CommandClass, ECompileIntentCommandClass::DenyWithRedirect);
	TestEqual(TEXT("Redirect target == livecoding_compile (for NoChange diagnostic)"),
		D.RedirectTargetTool, FString(TEXT("livecoding_compile")));
	TestEqual(TEXT("detector_context == editor_open_non_cpp_change_context"),
		D.DetectorContext, FString(TEXT("editor_open_non_cpp_change_context")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_PowerShellWrappedBuild_EditorOpen_Denies,
	"UnrealClaude.CompileIntent.PolicyGate.Edge.PowerShellWrappedBuild_EditorOpen_Denies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_PowerShellWrappedBuild_EditorOpen_Denies::RunTest(const FString& /*Parameters*/)
{
	// PowerShell wrapper strip should find the inner Build.bat token and
	// classify as build context even though the raw command starts with
	// `powershell.exe -Command "..."`.
	FCompileIntentPolicyInputs In = MakeCommandExecutionBaseline();
	In.ToolCommandText = TEXT("powershell.exe -NoProfile -Command \"Build.bat Poligon1Editor Win64 Development\"");
	In.ChangeClassification = TEXT("body_only_cpp");
	In.bEditorOpen = true;

	const FCompileIntentPolicyDecision D = FCompileIntentPolicyGate::EvaluateCompileIntent(In);
	TestEqual(TEXT("PS-wrapped Build.bat -> DenyWithRedirect"),
		D.CommandClass, ECompileIntentCommandClass::DenyWithRedirect);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_HasUserIntentRebuildOverride_Helper,
	"UnrealClaude.CompileIntent.Helpers.HasUserIntentRebuildOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_HasUserIntentRebuildOverride_Helper::RunTest(const FString& /*Parameters*/)
{
	// Positive cases.
	TestTrue(TEXT("'rebuild everything'"),
		FCompileIntentPolicyGate::HasUserIntentRebuildOverride(TEXT("Rebuild everything please.")));
	TestTrue(TEXT("'full build'"),
		FCompileIntentPolicyGate::HasUserIntentRebuildOverride(TEXT("Run a full build.")));
	TestTrue(TEXT("'force rebuild'"),
		FCompileIntentPolicyGate::HasUserIntentRebuildOverride(TEXT("Force rebuild from scratch.")));
	TestTrue(TEXT("'don't use live coding'"),
		FCompileIntentPolicyGate::HasUserIntentRebuildOverride(TEXT("Don't use live coding this time.")));
	TestTrue(TEXT("'skip live coding'"),
		FCompileIntentPolicyGate::HasUserIntentRebuildOverride(TEXT("Skip live coding, rebuild manually.")));
	TestTrue(TEXT("'clean build'"),
		FCompileIntentPolicyGate::HasUserIntentRebuildOverride(TEXT("Need a clean build for release.")));
	// Negative cases.
	TestFalse(TEXT("Empty -> false"),
		FCompileIntentPolicyGate::HasUserIntentRebuildOverride(FString()));
	TestFalse(TEXT("Normal task -> false"),
		FCompileIntentPolicyGate::HasUserIntentRebuildOverride(TEXT("Add a UE_LOG line to the constructor.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCompileIntent_CommandClassToString_AllValuesMapped,
	"UnrealClaude.CompileIntent.Helpers.CommandClassToString_AllValuesMapped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileIntent_CommandClassToString_AllValuesMapped::RunTest(const FString& /*Parameters*/)
{
	TestEqual(TEXT("Allow"),
		FString(CompileIntentCommandClassToString(ECompileIntentCommandClass::Allow)),
		FString(TEXT("allow")));
	TestEqual(TEXT("DenyWithRedirect"),
		FString(CompileIntentCommandClassToString(ECompileIntentCommandClass::DenyWithRedirect)),
		FString(TEXT("deny_with_redirect")));
	TestEqual(TEXT("RequireRestart"),
		FString(CompileIntentCommandClassToString(ECompileIntentCommandClass::RequireRestart)),
		FString(TEXT("require_restart")));
	TestEqual(TEXT("Ambiguous"),
		FString(CompileIntentCommandClassToString(ECompileIntentCommandClass::Ambiguous)),
		FString(TEXT("ambiguous")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
