// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 619 P2 description-guard tests for MCPTool_RestartSurvival.
 *
 * These tests lock the spec Fix #2 description text in place so that a
 * future maintainer cannot accidentally "simplify" the description back
 * to the pre-Fix #2 advisory form. They are intentionally cheap (no
 * fixtures, no editor subsystems) -- they invoke GetInfo() and assert
 * substring presence/absence on the Description field.
 *
 * Covers spec Test plan:
 *   - OsvayderUE.RestartSurvival.Description.ContainsLiveCodingPreconditionText
 *   - OsvayderUE.RestartSurvival.Description.MaxLengthSafe
 */

#include "CoreMinimal.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_RestartSurvival.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// ------------------------------------------------------------------------
// Test 1: Description must contain the Fix #2 critical markers + must NOT
// contain the old pre-Fix #2 advisory phrasing.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvivalDescription_ContainsLiveCodingPreconditionText,
	"OsvayderUE.RestartSurvival.Description.ContainsLiveCodingPreconditionText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRestartSurvivalDescription_ContainsLiveCodingPreconditionText::RunTest(const FString& /*Parameters*/)
{
	FMCPTool_RestartSurvival Tool;
	const FMCPToolInfo Info = Tool.GetInfo();

	// Fix #2 gating framing: the "ESCALATION PATH ONLY" banner is the
	// single strongest steering signal in the description. If a future
	// maintainer removes it, Codex/Claude tool-selection priors drift
	// back toward preemptive escalation.
	TestTrue(TEXT("Description must contain 'ESCALATION PATH ONLY' banner"),
		Info.Description.Contains(TEXT("ESCALATION PATH ONLY"), ESearchCase::CaseSensitive));

	// Cross-reference to livecoding_compile -- this is what makes the
	// description functional as tool-selection steering.
	TestTrue(TEXT("Description must reference 'livecoding_compile' tool by name"),
		Info.Description.Contains(TEXT("livecoding_compile"), ESearchCase::CaseSensitive));

	// The "do not call preemptively" phrase is the negative gating counterpart.
	TestTrue(TEXT("Description must contain 'Do not call preemptively' gating text"),
		Info.Description.Contains(TEXT("Do not call preemptively"), ESearchCase::CaseSensitive));

	// Defensive regression guard: the old pre-Fix #2 phrasing used
	// weaker advisory language. If either phrase reappears it means
	// someone partially reverted Fix #2.
	TestFalse(TEXT("Description must NOT contain the pre-Fix #2 advisory 'Use this only when' phrase"),
		Info.Description.Contains(TEXT("Use this only when"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("Description must NOT contain the old 'has truthfully hit a closed-editor blocker inside the bounded restart-survival family' phrase"),
		Info.Description.Contains(TEXT("has truthfully hit a closed-editor blocker inside the bounded restart-survival family"), ESearchCase::IgnoreCase));

	// The preconditions block must explicitly mention the reflection-layout
	// examples so the agent knows when restart_survival is actually correct.
	TestTrue(TEXT("Description must mention UPROPERTY as a reflection-layout example"),
		Info.Description.Contains(TEXT("UPROPERTY"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Description must mention UCLASS as a reflection-layout example"),
		Info.Description.Contains(TEXT("UCLASS"), ESearchCase::CaseSensitive));

	// Operation name must still appear so the agent knows what `operation`
	// value to pass -- Fix #2 preserves the exact operation semantics.
	TestTrue(TEXT("Description must still name 'prepare_task_continuation_handoff' operation"),
		Info.Description.Contains(TEXT("prepare_task_continuation_handoff"), ESearchCase::CaseSensitive));

	return true;
}

// ------------------------------------------------------------------------
// Test 2: Description length must stay under a conservative MCP limit.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestartSurvivalDescription_MaxLengthSafe,
	"OsvayderUE.RestartSurvival.Description.MaxLengthSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRestartSurvivalDescription_MaxLengthSafe::RunTest(const FString& /*Parameters*/)
{
	FMCPTool_RestartSurvival Tool;
	const FMCPToolInfo Info = Tool.GetInfo();

	// 619 P2 spec Test plan: "MCP protocol limits; make sure the new text fits."
	// MCP Tools spec 2025-06-18 does not publish a hard description length
	// limit, but Claude/Codex tool-overload studies (Lunar.dev + MS Research,
	// cited in 619 spec Sources footer) recommend <= ~4000 chars per tool
	// description to keep the prompt token budget reasonable across ~50 tools.
	// See: https://modelcontextprotocol.io/specification/2025-06-18/server/tools
	const int32 MaxSafeLength = 4000;
	TestTrue(
		FString::Printf(TEXT("Description length must be < %d chars (current: %d)"),
			MaxSafeLength, Info.Description.Len()),
		Info.Description.Len() < MaxSafeLength);

	// Sanity floor: the Fix #2 text is ~1100 chars. A description shorter
	// than 500 chars would mean someone stripped content, likely Fix #2
	// regression.
	const int32 MinExpectedLength = 500;
	TestTrue(
		FString::Printf(TEXT("Description length must be > %d chars (current: %d) -- guards against accidental strip"),
			MinExpectedLength, Info.Description.Len()),
		Info.Description.Len() > MinExpectedLength);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
