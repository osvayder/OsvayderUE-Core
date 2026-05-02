// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FUnrealClaudeActivePlan;

/**
 * 632 Task Recovery & Rehydration: deterministic summary of an interrupted
 * active plan. Pure data struct — all fields are plain-text lines suitable
 * for Slate dialog body + agent prompt injection.
 */
struct UNREALCLAUDE_API FTaskRecoverySummary
{
	FString ShortTitle;          // first 80 chars of original_user_task, word-boundary truncated
	FString PhaseStatusLine;     // "Phase 2 of 3 (perform_bounded_work) — in progress. Inspect completed, Verify pending."
	FString ProgressSummary;     // deterministic rollup of tool_calls
	FString LastIntent;          // verbatim CurrentActionRu if present, else CurrentAction
	FString StallIndicator;      // "Likely stuck on <error>" if last 3 calls share an error, else empty
	FString ElapsedSincePause;   // "12 hours 45 minutes ago" / "2 days 5 hours ago" / "just now"
	FString EvidencePointer;     // relative path to archived plan for forensic audit (or active path)
	FString OriginalUserTask;    // verbatim, so consumers can re-inject full user intent
};

/**
 * 632 Task Recovery & Rehydration: pure, testable, deterministic summarizer.
 * NO LLM call, NO filesystem read, NO external process. Takes a plan + a
 * "now UTC" reference time and returns a fully-populated summary struct.
 */
class UNREALCLAUDE_API FUnrealClaudeTaskRecoverySummarizer
{
public:
	/**
	 * Build the deterministic summary.
	 *
	 * @param Plan        The active plan loaded from active_plan.json.
	 * @param NowUtc      Reference "now" — lets callers inject a fixed time for tests.
	 *                    In production, pass FDateTime::UtcNow().
	 * @param EvidencePath Relative or absolute path to the plan's archive / live file;
	 *                    stored in FTaskRecoverySummary::EvidencePointer verbatim.
	 */
	static FTaskRecoverySummary BuildRecoverySummary(
		const FUnrealClaudeActivePlan& Plan,
		const FDateTime& NowUtc,
		const FString& EvidencePath);

	/**
	 * Exposed for tests: format an elapsed duration into human-readable form
	 * per the rules in 632 Part 2. Pure function.
	 *   <30s              => "just now"
	 *   <60min            => "N minutes ago"
	 *   <24h              => "N hours M minutes ago"
	 *   <7 days           => "N days M hours ago"
	 *   >=7 days          => "N weeks ago"  (coarse)
	 */
	static FString FormatElapsed(int64 TotalSeconds);
};
