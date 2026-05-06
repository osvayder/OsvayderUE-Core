// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FTaskRecoverySummary;

/**
 * 632 Task Recovery & Rehydration: user's choice on the recovery dialog.
 */
enum class EOsvayderUETaskRecoveryChoice : uint8
{
	/** No choice made yet (e.g., dialog dismissed by window close without clicking a button). */
	None,
	/** "Continue" — agent should receive [TASK RECOVERY] context block in next turn. */
	Continue,
	/** "Start Fresh" — archive current plan as abandoned_for_fresh_session, reset active_plan.json. */
	StartFresh,
	/** "Close as Irrelevant" — archive current plan as abandoned_by_user, record user reason. */
	CloseAsIrrelevant,
};

/**
 * 632 Task Recovery & Rehydration: modal Slate dialog that surfaces an
 * interrupted task to the user + captures their choice among 3 options.
 * Mirrors the FScriptPermissionDialog pattern (modal SWindow + button bar).
 *
 * Usage — call Show() from the game thread; returns the user's choice plus
 * the optional free-text reason when CloseAsIrrelevant is picked.
 */
class OSVAYDERUE_API FOsvayderUETaskRecoveryDialog
{
public:
	struct FDialogResult
	{
		EOsvayderUETaskRecoveryChoice Choice = EOsvayderUETaskRecoveryChoice::None;
		FString UserClosedReason; // Only populated when Choice == CloseAsIrrelevant
	};

	/**
	 * Show the modal dialog. MUST be called on the game thread.
	 * Summary is rendered into the dialog body; the user picks a button.
	 */
	static FDialogResult Show(const FTaskRecoverySummary& Summary, const FString& DiagnosticNote = FString());
};

/**
 * 632 Task Recovery & Rehydration: helper that builds the agent-facing
 * `[TASK RECOVERY]` context text from a summary struct. Pure function; same
 * format used for dialog body + prompt injection (keeps the two surfaces in
 * lock-step).
 */
class OSVAYDERUE_API FOsvayderUETaskRecoveryContextBuilder
{
public:
	/**
	 * Build the agent-facing context block:
	 * - Title line
	 * - Phase status line
	 * - Progress summary
	 * - Last intent
	 * - Stall indicator (if any)
	 * - Elapsed since pause
	 * - Evidence pointer
	 * - Verbatim original_user_task
	 */
	static FString BuildContextBlockText(const FTaskRecoverySummary& Summary);
};
