// Copyright Natali Caggiano. All Rights Reserved.

#include "CompileIntentPolicyGate.h"

#include "Dom/JsonObject.h"
#include "OsvayderUECommandClassification.h"

const TCHAR* CompileIntentCommandClassToString(const ECompileIntentCommandClass Class)
{
	switch (Class)
	{
	case ECompileIntentCommandClass::Allow:            return TEXT("allow");
	case ECompileIntentCommandClass::DenyWithRedirect: return TEXT("deny_with_redirect");
	case ECompileIntentCommandClass::RequireRestart:   return TEXT("require_restart");
	case ECompileIntentCommandClass::Ambiguous:        return TEXT("ambiguous");
	default:                                           return TEXT("allow");
	}
}

namespace
{
	bool IsCommandExecutionLikeTool(const FString& ToolName)
	{
		if (ToolName.IsEmpty())
		{
			return false;
		}
		// Mirror detector family gate shape. Pre-P3 restart-survival uses the
		// same pattern. Accepts "command_execution" plus any tool name that
		// ends with or contains the family label.
		return ToolName.Equals(TEXT("command_execution"), ESearchCase::IgnoreCase)
			|| ToolName.EndsWith(TEXT("/command_execution"), ESearchCase::IgnoreCase)
			|| ToolName.Contains(TEXT("command_execution"), ESearchCase::IgnoreCase);
	}

	TSharedPtr<FJsonObject> MakeLiveCodingParamsHint()
	{
		TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
		Hint->SetBoolField(TEXT("wait_for_completion"), true);
		Hint->SetBoolField(TEXT("agent_diff_expected"), true);
		Hint->SetBoolField(TEXT("enable_for_session_if_needed"), true);
		// expected_file_path intentionally left unset -- agent should fill from its own edit context.
		return Hint;
	}

	TSharedPtr<FJsonObject> MakeRestartSurvivalParamsHint()
	{
		TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
		Hint->SetStringField(TEXT("operation"), TEXT("arm"));
		Hint->SetStringField(TEXT("reason"), TEXT("reflected_structural_change_requires_editor_restart"));
		return Hint;
	}

	TSharedPtr<FJsonObject> MakeCppReflectionPreviewHint()
	{
		TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
		Hint->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
		Hint->SetStringField(TEXT("intent"), TEXT("classify_pending_edit_before_compile_route_decision"));
		return Hint;
	}

	FString ClassificationLabelOrUnknown(const FString& Raw)
	{
		return Raw.IsEmpty() ? FString(TEXT("unknown")) : Raw;
	}
}

bool FCompileIntentPolicyGate::HasUserIntentRebuildOverride(const FString& UserIntentText)
{
	if (UserIntentText.IsEmpty())
	{
		return false;
	}
	const FString Lower = UserIntentText.ToLower();
	static const TArray<FString> OverridePhrases = {
		TEXT("rebuild everything"),
		TEXT("full build"),
		TEXT("full rebuild"),
		TEXT("shell build"),
		TEXT("manual compile"),
		TEXT("manual build"),
		TEXT("don't use live coding"),
		TEXT("dont use live coding"),
		TEXT("do not use live coding"),
		TEXT("skip live coding"),
		TEXT("bypass live coding"),
		TEXT("force rebuild"),
		TEXT("force full rebuild"),
		TEXT("clean build"),
		TEXT("clean rebuild")
	};
	for (const FString& Phrase : OverridePhrases)
	{
		if (Lower.Contains(Phrase))
		{
			return true;
		}
	}
	return false;
}

FCompileIntentPolicyDecision FCompileIntentPolicyGate::EvaluateCompileIntent(
	const FCompileIntentPolicyInputs& In)
{
	using namespace OsvayderUE::CommandClassification;

	FCompileIntentPolicyDecision D;
	D.AgentToolChoice = In.ToolName;
	D.ChangeClassificationForTelemetry = ClassificationLabelOrUnknown(In.ChangeClassification);

	// ALLOW: tool is not in the command_execution family. Policy gate only
	// governs shell build routing; other tools pass through.
	if (!IsCommandExecutionLikeTool(In.ToolName))
	{
		D.CommandClass = ECompileIntentCommandClass::Allow;
		D.CompileIntent = TEXT("inspection"); // conservative label for non-shell tools
		D.ChosenCompilePath = TEXT("shell_build_allowed"); // i.e., not gated by P3
		D.DetectorContext = TEXT("non_command_execution_tool");
		return D;
	}

	// ALLOW: command is a recognized inspection command (Get-Content / rg / git status / etc.).
	if (IsKnownInspectionCommand(In.ToolCommandText))
	{
		D.CommandClass = ECompileIntentCommandClass::Allow;
		D.CompileIntent = TEXT("inspection");
		D.ChosenCompilePath = TEXT("shell_build_allowed");
		D.DetectorContext = TEXT("inspection_command");
		return D;
	}

	// ALLOW: explicit user-intent override phrase in the turn prompt.
	if (HasUserIntentRebuildOverride(In.UserIntentText))
	{
		D.CommandClass = ECompileIntentCommandClass::Allow;
		D.CompileIntent = TEXT("full_rebuild");
		D.ChosenCompilePath = TEXT("shell_build_allowed");
		D.DetectorContext = TEXT("user_intent_rebuild_override");
		return D;
	}

	// ALLOW: editor closed OR explicit-expert lane, and command looks build-like.
	// Both contexts allow shell build even for body-only edits because the
	// live-coding hot-patch lane is either unavailable (editor closed) or
	// explicitly opted-out (expert lane).
	const bool bIsBuildCommand = IsBuildContextCommand(In.ToolCommandText);
	if ((!In.bEditorOpen || In.LaneProfile == EAgentExecutionRunProfile::ExplicitExpertOptIn) && bIsBuildCommand)
	{
		D.CommandClass = ECompileIntentCommandClass::Allow;
		D.CompileIntent = TEXT("full_rebuild");
		D.ChosenCompilePath = TEXT("shell_build_allowed");
		D.DetectorContext = !In.bEditorOpen ? TEXT("editor_closed") : TEXT("expert_manual_lane");
		return D;
	}

	// ALLOW: command isn't a build invocation (and wasn't caught by inspection
	// list). Could be `dir`, `env`, arbitrary shell operation. Pass through.
	if (!bIsBuildCommand)
	{
		D.CommandClass = ECompileIntentCommandClass::Allow;
		D.CompileIntent = TEXT("unknown");
		D.ChosenCompilePath = TEXT("shell_build_allowed");
		D.DetectorContext = TEXT("non_build_shell_command");
		return D;
	}

	// Editor OPEN + default lane + build-like command: now the meaningful routing.

	// REQUIRE_RESTART: reflected structural change.
	if (In.ChangeClassification.Equals(TEXT("reflected_structural"), ESearchCase::IgnoreCase))
	{
		D.CommandClass = ECompileIntentCommandClass::RequireRestart;
		D.CompileIntent = TEXT("structural_add");
		D.ChosenCompilePath = TEXT("restart_survival");
		D.DenyReasonCode = TEXT("shell_build_denied_reflected_structural_requires_restart");
		D.DenyReasonText = TEXT(
			"Shell build attempted while editor is open AND the pending change is classified as reflected_structural "
			"(adds/removes UPROPERTY, UFUNCTION, UCLASS, USTRUCT layout). Live Coding cannot safely re-instance "
			"against a new reflected layout; a shell build alone also cannot proceed while the editor holds the "
			"DLL. Call restart_survival to arm the closed-editor rebuild lane instead.");
		D.RedirectTargetTool = TEXT("restart_survival");
		D.RedirectTargetOperation = TEXT("arm");
		D.RedirectTargetParamsHint = MakeRestartSurvivalParamsHint();
		D.InferredChangeClassification = TEXT("reflected_structural");
		D.AllowedContextsWhereThisWouldSucceed = {
			TEXT("explicit_expert_opt_in lane"),
			TEXT("editor_closed state")
		};
		D.RequiresRestartReason = TEXT("reflected_uproperty_structural_add_or_revert");
		D.DetectorContext = TEXT("editor_open_structural_context; feature_slice compile_gate requires restart_survival before runtime_proof");
		D.bRedirectedFromShell = true;
		D.bPolicyRedirectApplied = true;
		D.PolicyRedirectTarget = TEXT("restart_survival");
		return D;
	}

	// DENY_REDIRECT: body-only .cpp edit -- redirect to livecoding_compile.
	if (In.ChangeClassification.Equals(TEXT("body_only_cpp"), ESearchCase::IgnoreCase)
		|| In.ChangeClassification.Equals(TEXT("meta_only_uproperty"), ESearchCase::IgnoreCase))
	{
		const bool bMetaOnly = In.ChangeClassification.Equals(TEXT("meta_only_uproperty"), ESearchCase::IgnoreCase);
		D.CommandClass = ECompileIntentCommandClass::DenyWithRedirect;
		D.CompileIntent = bMetaOnly ? TEXT("meta_edit") : TEXT("body_edit");
		D.ChosenCompilePath = TEXT("denied_redirect");
		D.DenyReasonCode = TEXT("shell_build_denied_body_edit_while_editor_open");
		D.DenyReasonText = bMetaOnly
			? FString(TEXT(
				"Shell build attempted while editor is open AND the pending change is meta-only (UPROPERTY metadata "
				"flags). livecoding_compile hot-patches this class of change in 2-15 seconds without editor restart. "
				"Call livecoding_compile instead of a shell build. In feature_slice runs this satisfies the compile_gate "
				"phase; runtime_proof stays blocked until compile success is recorded."))
			: FString(TEXT(
				"Shell build attempted while editor is open AND the pending change is body-only .cpp (no reflected "
				"layout change). livecoding_compile hot-patches this class of change in 2-15 seconds; a shell build "
				"would be blocked by the editor holding the DLL, or at minimum would cost 5-15 minutes for a "
				"no-value full rebuild. Call livecoding_compile instead. In feature_slice runs this satisfies the "
				"compile_gate phase; runtime_proof stays blocked until compile success is recorded."));
		D.RedirectTargetTool = TEXT("livecoding_compile");
		D.RedirectTargetOperation = TEXT("compile");
		D.RedirectTargetParamsHint = MakeLiveCodingParamsHint();
		D.InferredChangeClassification = bMetaOnly ? TEXT("meta_only_uproperty") : TEXT("body_only_cpp_edit");
		D.AllowedContextsWhereThisWouldSucceed = {
			TEXT("explicit_expert_opt_in lane"),
			TEXT("editor_closed state"),
			TEXT("reflected_structural change classification (escalates to restart_survival)")
		};
		D.LiveCodingAllowedReason = bMetaOnly ? TEXT("meta_only_uproperty_change") : TEXT("body_only_edit");
		D.DetectorContext = TEXT("editor_open_body_edit_context");
		D.bRedirectedFromShell = true;
		D.bPolicyRedirectApplied = true;
		D.PolicyRedirectTarget = TEXT("livecoding_compile");
		return D;
	}

	// DENY_REDIRECT: non_cpp or other known-not-compile change -- still deny a
	// shell build because the editor being open means the DLL is held. Fall
	// back to livecoding_compile redirect (LC will correctly report NoChanges
	// if the edit isn't in a module).
	if (In.ChangeClassification.Equals(TEXT("non_cpp"), ESearchCase::IgnoreCase))
	{
		D.CommandClass = ECompileIntentCommandClass::DenyWithRedirect;
		D.CompileIntent = TEXT("unknown");
		D.ChosenCompilePath = TEXT("denied_redirect");
		D.DenyReasonCode = TEXT("shell_build_denied_non_cpp_change_no_rebuild_needed");
		D.DenyReasonText = TEXT(
			"Shell build attempted while editor is open, but the pending change is classified as non_cpp "
			"(docs, config, asset). A C++ rebuild is not required for a non-C++ change. If you believe a "
			"rebuild IS needed, call livecoding_compile with agent_diff_expected=true + expected_file_path; "
			"the NoChange diagnostic will clarify whether the edit belongs to a compiled module.");
		D.RedirectTargetTool = TEXT("livecoding_compile");
		D.RedirectTargetOperation = TEXT("compile");
		D.RedirectTargetParamsHint = MakeLiveCodingParamsHint();
		D.InferredChangeClassification = TEXT("non_cpp");
		D.AllowedContextsWhereThisWouldSucceed = {
			TEXT("explicit_expert_opt_in lane"),
			TEXT("editor_closed state")
		};
		D.DetectorContext = TEXT("editor_open_non_cpp_change_context");
		D.bRedirectedFromShell = true;
		D.bPolicyRedirectApplied = true;
		D.PolicyRedirectTarget = TEXT("livecoding_compile");
		return D;
	}

	// AMBIGUOUS: classification unknown (empty or any label we didn't match above).
	// v3 explicit rule: do NOT auto-escalate to restart_survival from shell output alone.
	// Instead, advise the agent to run cpp_reflection preview first to classify the pending edit.
	D.CommandClass = ECompileIntentCommandClass::Ambiguous;
	D.CompileIntent = TEXT("unknown");
	D.ChosenCompilePath = TEXT("ambiguous_no_route");
	D.DenyReasonCode = TEXT("shell_build_ambiguous_no_classification");
	D.DenyReasonText = TEXT(
		"Shell build attempted while editor is open, but the pending change has no classification (cpp_reflection "
		"classifier has not run against the recent edit). Run cpp_reflection preview against the edited file first "
		"so the compile-route decision has ground truth, then retry with either livecoding_compile (body/meta) or "
		"restart_survival (reflected_structural). This step is required because auto-escalating to restart_survival "
		"from shell output alone is explicitly disallowed (v3 rule).");
	D.RedirectTargetTool = TEXT("cpp_reflection");
	D.RedirectTargetOperation = TEXT("preview_reflected_property_declaration");
	D.RedirectTargetParamsHint = MakeCppReflectionPreviewHint();
	D.InferredChangeClassification = TEXT("unknown");
	D.AllowedContextsWhereThisWouldSucceed = {
		TEXT("any classification present"),
		TEXT("explicit_expert_opt_in lane"),
		TEXT("editor_closed state")
	};
	D.DetectorContext = TEXT("editor_open_unknown_change_context");
	D.bRedirectedFromShell = true;
	D.bPolicyRedirectApplied = true;
	D.PolicyRedirectTarget = TEXT("cpp_reflection");
	return D;
}

FString FCompileIntentPolicyGate::BuildAgentAdvisoryText(const FCompileIntentPolicyDecision& Decision)
{
	if (Decision.CommandClass == ECompileIntentCommandClass::Allow)
	{
		return FString();
	}

	// Compose a short, agent-actionable advisory text. Concise by design so
	// it doesn't balloon the system prompt. Carries: what was denied, why,
	// redirect target, suggested params (abbreviated). Agent reads this in
	// next turn's [POLICY ROUTING ADVISORY] context block and retries.
	FString Advisory;
	Advisory += TEXT("Your previous command_execution call was OFF-POLICY per the OsvayderUE compile-intent gate.\n");
	Advisory += FString::Printf(TEXT("Classification: %s\n"),
		CompileIntentCommandClassToString(Decision.CommandClass));
	Advisory += FString::Printf(TEXT("Reason code: %s\n"), *Decision.DenyReasonCode);
	Advisory += FString::Printf(TEXT("Reason: %s\n"), *Decision.DenyReasonText);
	Advisory += FString::Printf(TEXT("Redirect target tool: %s\n"), *Decision.RedirectTargetTool);
	if (!Decision.RedirectTargetOperation.IsEmpty())
	{
		Advisory += FString::Printf(TEXT("Redirect target operation: %s\n"),
			*Decision.RedirectTargetOperation);
	}

	// Summarize suggested params (single-line). Full JSON hint is already in
	// agent_trace.jsonl at the policy_routing_decision event for forensics.
	if (Decision.RedirectTargetParamsHint.IsValid())
	{
		TArray<FString> Pairs;
		for (const auto& KV : Decision.RedirectTargetParamsHint->Values)
		{
			if (!KV.Value.IsValid())
			{
				continue;
			}
			FString ValueText;
			if (KV.Value->Type == EJson::Boolean)
			{
				ValueText = KV.Value->AsBool() ? TEXT("true") : TEXT("false");
			}
			else if (KV.Value->Type == EJson::String)
			{
				ValueText = TEXT("\"") + KV.Value->AsString() + TEXT("\"");
			}
			else
			{
				ValueText = TEXT("<object>");
			}
			Pairs.Add(FString::Printf(TEXT("%s=%s"), *KV.Key, *ValueText));
		}
		if (Pairs.Num() > 0)
		{
			Advisory += FString::Printf(TEXT("Suggested params: %s\n"),
				*FString::Join(Pairs, TEXT(", ")));
		}
	}

	if (!Decision.InferredChangeClassification.IsEmpty())
	{
		Advisory += FString::Printf(TEXT("Inferred change classification: %s\n"),
			*Decision.InferredChangeClassification);
	}

	Advisory += TEXT("ACTION: call the redirect target tool with the suggested params instead of retrying the shell build.");
	return Advisory;
}

TSharedRef<FJsonObject> FCompileIntentPolicyGate::BuildPolicyRoutingJson(
	const FCompileIntentPolicyDecision& Decision)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	const bool bIsDeny = Decision.CommandClass != ECompileIntentCommandClass::Allow;

	// Top-level policy_denied flag + command_class label.
	Root->SetBoolField(TEXT("policy_denied"), bIsDeny);
	Root->SetStringField(TEXT("command_class"), CompileIntentCommandClassToString(Decision.CommandClass));

	// Structured deny body (6 fields per dispatch). Always present; empty/null when Allow.
	TSharedRef<FJsonObject> Deny = MakeShared<FJsonObject>();
	Deny->SetStringField(TEXT("deny_reason_code"), Decision.DenyReasonCode);
	Deny->SetStringField(TEXT("deny_reason_text"), Decision.DenyReasonText);
	Deny->SetStringField(TEXT("redirect_target_tool"), Decision.RedirectTargetTool);
	Deny->SetStringField(TEXT("redirect_target_operation"), Decision.RedirectTargetOperation);
	if (Decision.RedirectTargetParamsHint.IsValid())
	{
		Deny->SetObjectField(TEXT("redirect_target_params_hint"), Decision.RedirectTargetParamsHint);
	}
	else
	{
		Deny->SetObjectField(TEXT("redirect_target_params_hint"), MakeShared<FJsonObject>());
	}
	Deny->SetStringField(TEXT("inferred_change_classification"), Decision.InferredChangeClassification);
	TArray<TSharedPtr<FJsonValue>> AllowedCtxJson;
	for (const FString& Ctx : Decision.AllowedContextsWhereThisWouldSucceed)
	{
		AllowedCtxJson.Add(MakeShared<FJsonValueString>(Ctx));
	}
	Deny->SetArrayField(TEXT("allowed_contexts_where_this_would_succeed"), AllowedCtxJson);
	Root->SetObjectField(TEXT("deny"), Deny);

	// 10 telemetry fields under policy_routing.
	TSharedRef<FJsonObject> Telemetry = MakeShared<FJsonObject>();
	Telemetry->SetStringField(TEXT("compile_intent"), Decision.CompileIntent);
	Telemetry->SetStringField(TEXT("change_classification"), Decision.ChangeClassificationForTelemetry);
	Telemetry->SetStringField(TEXT("chosen_compile_path"), Decision.ChosenCompilePath);
	Telemetry->SetBoolField(TEXT("redirected_from_shell"), Decision.bRedirectedFromShell);
	Telemetry->SetStringField(TEXT("requires_restart_reason"), Decision.RequiresRestartReason);
	Telemetry->SetStringField(TEXT("livecoding_allowed_reason"), Decision.LiveCodingAllowedReason);
	Telemetry->SetStringField(TEXT("detector_context"), Decision.DetectorContext);
	Telemetry->SetStringField(TEXT("agent_tool_choice"), Decision.AgentToolChoice);
	Telemetry->SetBoolField(TEXT("policy_redirect_applied"), Decision.bPolicyRedirectApplied);
	Telemetry->SetStringField(TEXT("policy_redirect_target"), Decision.PolicyRedirectTarget);
	Root->SetObjectField(TEXT("telemetry"), Telemetry);

	return Root;
}
