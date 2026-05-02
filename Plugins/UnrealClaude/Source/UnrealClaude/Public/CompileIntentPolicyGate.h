// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"

class FJsonObject;

/**
 * 626 P3 Compile-Intent Policy Gate.
 *
 * ===== Purpose =====
 *
 * Classify a candidate tool dispatch -- specifically a `command_execution`
 * shell invocation -- against the active plugin state and produce a
 * structured routing decision that distinguishes:
 *
 *   - Allow          (inspection, safe reads, non-build shell, explicit expert lane, editor closed)
 *   - DenyWithRedirect (editor-open shell build of a body-only .cpp edit -> redirect to livecoding_compile)
 *   - RequireRestart  (reflected/structural change -> redirect to restart_survival)
 *   - Ambiguous       (change classification unknown + build-like command -> ask agent to run classifier first)
 *
 * The gate is PURE. No file I/O, no engine globals, no side effects.
 * Callers are responsible for gathering the 5 inputs, invoking the gate,
 * and either passing the decision through to the agent in the tool-result
 * annotation layer or emitting the structured deny response directly.
 *
 * ===== Why pure? =====
 *
 * Codex owns `command_execution` entirely; the plugin cannot pre-empt the
 * shell dispatch. The policy gate therefore runs at the TOOL-RESULT
 * ANNOTATION layer (widget-side, after Codex reports the command_execution
 * result but before the turn-level post-processing). The gate emits a
 * structured deny / advisory that the agent can observe in the transcript
 * + the next turn's context. This is the honest architectural shape per
 * v3 spec: "Prefer explicit deny + redirect message, not silent rewrite."
 *
 * ===== Reference =====
 *
 *   - Dispatch: `AgentBridge/CODEX_TO_CLAUDE.md` 2026-04-19 17:35.
 *   - Spec:     `Docs/UnrealClaude/626_CompileRoutingAndErrorSafetyCorrection_DispatchReady_v3.md` §626-P3.
 *   - Reuse:    `UnrealClaudeCommandClassification.h` (P2-derived classifiers).
 */

/** Top-level command classification the gate returns. */
enum class ECompileIntentCommandClass : uint8
{
	/** Pass-through: tool dispatch proceeds as normal. */
	Allow,
	/** Structured deny with redirect to a different MCP tool. */
	DenyWithRedirect,
	/** Structured deny with redirect to restart_survival. */
	RequireRestart,
	/**
	 * Change classification unknown + command looks build-like.
	 * Gate emits a "run classifier first" advisory; does NOT auto-escalate
	 * to restart_survival from shell output alone (v3 explicit rule).
	 */
	Ambiguous
};

UNREALCLAUDE_API const TCHAR* CompileIntentCommandClassToString(ECompileIntentCommandClass Class);

/** 5 inputs the gate inspects. Missing / unknown values default to reasonable fallbacks. */
struct UNREALCLAUDE_API FCompileIntentPolicyInputs
{
	/** Name of the tool the agent attempted to call (typically "command_execution"). */
	FString ToolName;

	/** For command_execution: the actual shell command string. Empty for non-shell tools. */
	FString ToolCommandText;

	/**
	 * Most recent relevant change classification from Fix #4 cpp_reflection
	 * classifier, OR a heuristic label if the agent surfaced one. Accepted
	 * values: "body_only_cpp", "meta_only_uproperty", "reflected_structural",
	 * "non_cpp", empty (unknown).
	 */
	FString ChangeClassification;

	/**
	 * Is the editor currently open + running? True in normal editor session;
	 * false in CI/headless/commandlet context. Affects whether the editor-
	 * open deny path is even applicable.
	 */
	bool bEditorOpen = true;

	/**
	 * Active execution-control profile for this request. Explicit-expert
	 * opt-in permits shell build even while editor is open.
	 */
	EAgentExecutionRunProfile LaneProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;

	/**
	 * Current turn's prompt text (user + system). Scanned for explicit
	 * user-intent keywords like "rebuild everything", "full build", "don't
	 * use live coding". Empty = no explicit override.
	 */
	FString UserIntentText;
};

/**
 * 10-field routing decision the gate produces. Always populated -- the
 * telemetry fields (compile_intent, change_classification, etc.) are
 * emitted on EVERY decision path, not just denials.
 */
struct UNREALCLAUDE_API FCompileIntentPolicyDecision
{
	// ----- Core classification ------------------------------------------
	ECompileIntentCommandClass CommandClass = ECompileIntentCommandClass::Allow;

	// ----- Structured deny fields (populated on DenyWithRedirect / RequireRestart / Ambiguous) -----
	FString DenyReasonCode;
	FString DenyReasonText;
	FString RedirectTargetTool;
	FString RedirectTargetOperation;
	TSharedPtr<FJsonObject> RedirectTargetParamsHint; // optional; caller may omit
	FString InferredChangeClassification;
	TArray<FString> AllowedContextsWhereThisWouldSucceed;

	// ----- 10 telemetry fields (always populated) -----------------------
	FString CompileIntent;              // "body_edit", "meta_edit", "structural_add", "structural_revert", "full_rebuild", "inspection", "unknown"
	FString ChangeClassificationForTelemetry; // "body_only_cpp", "meta_only_uproperty", "reflected_structural", "non_cpp", "unknown"
	FString ChosenCompilePath;          // "live_coding", "restart_survival", "shell_build_allowed", "denied_redirect", "ambiguous_no_route"
	bool bRedirectedFromShell = false;  // was this a shell build that got denied?
	FString RequiresRestartReason;      // filled when RequireRestart
	FString LiveCodingAllowedReason;    // filled when DenyWithRedirect -> livecoding_compile
	FString DetectorContext;            // "editor_open_body_edit_context", "editor_closed", "expert_manual_lane", "inspection_command", etc.
	FString AgentToolChoice;            // echo of input ToolName
	bool bPolicyRedirectApplied = false;// did the gate deny + redirect?
	FString PolicyRedirectTarget;       // if redirect, which tool? (livecoding_compile / restart_survival / cpp_reflection)
};

/**
 * Pure evaluator. Entry point for the policy gate. No I/O, thread-safe.
 */
class UNREALCLAUDE_API FCompileIntentPolicyGate
{
public:
	static FCompileIntentPolicyDecision EvaluateCompileIntent(const FCompileIntentPolicyInputs& Inputs);

	/**
	 * Build a structured JSON representing the policy-denied tool result.
	 * Used by the widget-side annotation layer to emit the deny body into
	 * result metadata. Always includes the 10 telemetry fields under
	 * `policy_routing`, plus the 6-field structured deny under
	 * `policy_denied=true` shape when CommandClass != Allow.
	 */
	static TSharedRef<FJsonObject> BuildPolicyRoutingJson(const FCompileIntentPolicyDecision& Decision);

	/**
	 * User-intent keyword scan. True if UserIntentText contains an explicit
	 * override phrase like "rebuild everything", "full build", "shell build",
	 * "don't use live coding", "manual compile". Case-insensitive.
	 */
	static bool HasUserIntentRebuildOverride(const FString& UserIntentText);

	/**
	 * 626 P6-prep: build a human-readable advisory text (to inject into the
	 * next turn's system prompt as a context block) from a non-Allow
	 * decision. Empty string for Allow decisions. The advisory briefly
	 * explains why the agent's last command_execution was off-policy and
	 * points at the correct redirect target tool + suggested params.
	 */
	static FString BuildAgentAdvisoryText(const FCompileIntentPolicyDecision& Decision);
};
