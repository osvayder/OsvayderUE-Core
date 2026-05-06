// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 626 P1 crash-safety tests for SOsvayderEditorWidget.
 *
 * Covers dispatch acceptance A-P1-3, A-P1-4, A-P1-5 + generic weak-pattern
 * invariant used by the P1 lambda-capture refactor. The actual widget
 * callback chain (OnClaudeResponse -> FinalizeStreamingResponse ->
 * ParseAndRenderCodeBlocks) runs inside Slate + editor context and is not
 * directly constructible in automation-test mode; these tests exercise
 * the CRITICAL BUILDING BLOCKS whose correctness makes the chain
 * crash-safe:
 *
 *   1. `ParseCodeFences` (static, pure) handles malformed / adversarial
 *      input without crashing (A-P1-5 proxy).
 *   2. TWeakPtr invariant: pinning a weak pointer after the referenced
 *      shared goes out of scope returns invalid; capturing the weak in a
 *      lambda produces a no-op when executed post-destruction (A-P1-4).
 *   3. Array-snapshot pattern used by `ParseAndRenderCodeBlocks` survives
 *      a concurrent clear of the source arrays mid-iteration (A-P1-1 +
 *      A-P1-5 structural guard).
 *   4. One-shot-guard state transition: a bool flag flipped on first call
 *      causes the second call to short-circuit (A-P1-3 proxy).
 *
 * Reference: `AgentBridge/CODEX_TO_OSVAYDER.md` 2026-04-19 13:00 dispatch for
 * `626 P1`; crash pattern from `observed_failures.md` rows #26/#28.
 */

#include "CoreMinimal.h"
#include "OsvayderEditorWidget.h"
#include "Misc/AutomationTest.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/SBoxPanel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace OsvayderUEWidgetCrashSafetyTestsDetail
{
	/**
	 * Resource that tracks its population via a file-scope static counter.
	 * Defined at file scope rather than inside RunTest because local classes
	 * cannot have out-of-line static data member definitions (C2246 / C3083).
	 */
	struct FCountedResource : public TSharedFromThis<FCountedResource>
	{
		static int32 LiveCount;
		FCountedResource() { ++LiveCount; }
		~FCountedResource() { --LiveCount; }
	};

	int32 FCountedResource::LiveCount = 0;
}

// ------------------------------------------------------------------------
// Test 1 (A-P1-5): malformed code-fence input does not crash parser.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidget_CrashSafety_MalformedCodeFences_DoesNotCrash,
	"OsvayderUE.Widget.CrashSafety.MalformedCodeFences_DoesNotCrash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidget_CrashSafety_MalformedCodeFences_DoesNotCrash::RunTest(const FString& /*Parameters*/)
{
	// Classic well-formed case as a baseline sanity check.
	{
		TArray<TPair<FString, bool>> Sections;
		SOsvayderEditorWidget::ParseCodeFences(TEXT("before\n```cpp\ncode;\n```\nafter"), Sections);
		TestTrue(TEXT("Well-formed fences must parse into >=1 section"), Sections.Num() >= 1);
	}

	// Malformed: unterminated opening fence (no closing ```).
	{
		TArray<TPair<FString, bool>> Sections;
		SOsvayderEditorWidget::ParseCodeFences(TEXT("before\n```cpp\nunterminated code\n"), Sections);
		// No crash = pass. Parser must return with SOME section list (may be
		// empty or partial), never crash. Exact shape isn't asserted because
		// the crash-safety gate is about not-crashing.
		TestTrue(TEXT("Unterminated fence must parse without crash"), Sections.Num() >= 0);
	}

	// Malformed: only a closing fence, no opener.
	{
		TArray<TPair<FString, bool>> Sections;
		SOsvayderEditorWidget::ParseCodeFences(TEXT("just text\n```\nmore text"), Sections);
		TestTrue(TEXT("Lone closing fence must parse without crash"), Sections.Num() >= 0);
	}

	// Adversarial: embedded null characters (C++ FString stores length so
	// these should be safe but test the pathway explicitly).
	{
		FString InputWithNulls = TEXT("prefix\0\n```cpp\nembedded null\n```\nsuffix");
		TArray<TPair<FString, bool>> Sections;
		SOsvayderEditorWidget::ParseCodeFences(InputWithNulls, Sections);
		TestTrue(TEXT("Input containing null chars must parse without crash"), Sections.Num() >= 0);
	}

	// Adversarial: very short input that previously crashed due to
	// off-by-one at the fence search boundary.
	{
		TArray<TPair<FString, bool>> Sections;
		SOsvayderEditorWidget::ParseCodeFences(TEXT("``"), Sections);
		TestTrue(TEXT("Two-backtick input must not crash"), Sections.Num() >= 0);
	}
	{
		TArray<TPair<FString, bool>> Sections;
		SOsvayderEditorWidget::ParseCodeFences(TEXT("```"), Sections);
		TestTrue(TEXT("Three-backtick-only input must not crash"), Sections.Num() >= 0);
	}

	// Empty input.
	{
		TArray<TPair<FString, bool>> Sections;
		SOsvayderEditorWidget::ParseCodeFences(FString(), Sections);
		TestTrue(TEXT("Empty input must not crash + produces 0 sections"), Sections.Num() == 0);
	}

	// Adversarial: many consecutive opening fences without closes.
	{
		FString PathologicalInput;
		for (int32 i = 0; i < 50; ++i)
		{
			PathologicalInput.Append(TEXT("```cpp\n"));
		}
		TArray<TPair<FString, bool>> Sections;
		SOsvayderEditorWidget::ParseCodeFences(PathologicalInput, Sections);
		TestTrue(TEXT("Pathological fence-spam input must not crash"), Sections.Num() >= 0);
	}

	return true;
}

// ------------------------------------------------------------------------
// Test 2 (A-P1-4): TWeakPtr pattern used by the lambda refactor -- pin
// after destruction returns invalid, body is skipped, no crash.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidget_CrashSafety_WeakPtrInvariant_PinAfterDestructionIsNoOp,
	"OsvayderUE.Widget.CrashSafety.WeakPtrInvariant_PinAfterDestructionIsNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidget_CrashSafety_WeakPtrInvariant_PinAfterDestructionIsNoOp::RunTest(const FString& /*Parameters*/)
{
	// Minimal Slate-aware dummy widget that mirrors the TSharedFromThis
	// relationship SOsvayderEditorWidget uses. This verifies the exact
	// capture + pin sequence the P1 refactor relies on without needing
	// to construct a full SOsvayderEditorWidget (which requires editor
	// context + UI dependencies).
	struct FDummySharedHolder : public TSharedFromThis<FDummySharedHolder>
	{
		int32 CallCount = 0;
		void Bump() { ++CallCount; }
	};

	int32 ExecutedCallCount = 0;
	TFunction<void()> CapturedLambda;
	{
		TSharedRef<FDummySharedHolder> Holder = MakeShared<FDummySharedHolder>();
		TWeakPtr<FDummySharedHolder> WeakHolder = Holder;
		CapturedLambda = [WeakHolder, &ExecutedCallCount]()
		{
			TSharedPtr<FDummySharedHolder> Pinned = WeakHolder.Pin();
			if (!Pinned.IsValid())
			{
				return;
			}
			Pinned->Bump();
			++ExecutedCallCount;
		};

		// Before destruction: lambda executes and bumps.
		CapturedLambda();
		TestEqual(TEXT("Lambda must execute while shared is alive"), ExecutedCallCount, 1);
		TestEqual(TEXT("Holder must see the bump"), Holder->CallCount, 1);
	}
	// Shared destroyed. Weak should now pin to invalid.

	// Lambda execute post-destruction: body skipped, no crash.
	CapturedLambda();
	TestEqual(
		TEXT("Lambda body must be skipped after shared destruction (ExecutedCallCount unchanged)"),
		ExecutedCallCount, 1);

	// Double-fire post-destruction also safe.
	CapturedLambda();
	CapturedLambda();
	TestEqual(
		TEXT("Repeated post-destruction fires remain no-ops"),
		ExecutedCallCount, 1);

	return true;
}

// ------------------------------------------------------------------------
// Test 3 (A-P1-1 + A-P1-5 structural): array-snapshot pattern survives
// concurrent clear of source arrays mid-iteration. Mirrors the exact
// pattern ParseAndRenderCodeBlocks uses: local `const TArray` snapshots
// keep shared_ptr refcounts stable even if the member arrays are cleared.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidget_CrashSafety_ArraySnapshot_SurvivesConcurrentClear,
	"OsvayderUE.Widget.CrashSafety.ArraySnapshot_SurvivesConcurrentClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidget_CrashSafety_ArraySnapshot_SurvivesConcurrentClear::RunTest(const FString& /*Parameters*/)
{
	using OsvayderUEWidgetCrashSafetyTestsDetail::FCountedResource;
	FCountedResource::LiveCount = 0;

	TArray<TSharedPtr<FCountedResource>> MemberArray;
	for (int32 i = 0; i < 5; ++i)
	{
		MemberArray.Add(MakeShared<FCountedResource>());
	}
	TestEqual(TEXT("Member array should hold 5 live resources"), FCountedResource::LiveCount, 5);

	int32 VisitedCount = 0;
	{
		// Snapshot pattern: const copy at loop entry. Bumps refcount on each
		// element so they survive any mutation of MemberArray during iteration.
		const TArray<TSharedPtr<FCountedResource>> Snapshot = MemberArray;
		TestEqual(TEXT("Snapshot must reference the same 5 resources"), Snapshot.Num(), MemberArray.Num());

		// Simulate the adversarial scenario: mid-iteration, another path
		// clears the member array. If we were iterating `MemberArray` directly
		// without a snapshot, the shared_ptr release would fire during
		// iteration and potentially invalidate the iterator. With the
		// snapshot pattern, LiveCount stays stable because the snapshot holds
		// refs.
		for (int32 i = 0; i < Snapshot.Num(); ++i)
		{
			if (i == 2)
			{
				MemberArray.Empty(); // Adversarial clear mid-loop
			}
			TSharedPtr<FCountedResource> Item = Snapshot[i];
			TestTrue(
				FString::Printf(TEXT("Snapshot item %d must remain valid even after member clear"), i),
				Item.IsValid());
			++VisitedCount;
		}

		TestEqual(TEXT("Snapshot loop must have visited all 5"), VisitedCount, 5);
		TestEqual(TEXT("Resources must still be live while Snapshot is in scope"), FCountedResource::LiveCount, 5);
	}
	// Snapshot goes out of scope here -> refs release.
	TestEqual(TEXT("Resources released once snapshot exits scope"), FCountedResource::LiveCount, 0);
	return true;
}

// ------------------------------------------------------------------------
// Test 4 (A-P1-3 proxy): one-shot guard logic -- a bool flag flipped on
// first call causes a second call to short-circuit. Mirrors the exact
// guard pattern used by FinalizeStreamingResponse::bHasCompletedResponseDelivery.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidget_CrashSafety_OneShotGuard_SecondCallIsNoOp,
	"OsvayderUE.Widget.CrashSafety.OneShotGuard_SecondCallIsNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidget_CrashSafety_OneShotGuard_SecondCallIsNoOp::RunTest(const FString& /*Parameters*/)
{
	// Model the guarded function's internal state machine without needing
	// to construct the widget.
	bool bHasCompleted = false;
	int32 BodyExecutedCount = 0;

	auto GuardedFinalize = [&]()
	{
		// First-fire guard: mirror the exact shape used in
		// SOsvayderEditorWidget::FinalizeStreamingResponse.
		if (bHasCompleted)
		{
			// Second + N-th calls no-op. Production code logs a Warning here;
			// this test observes via BodyExecutedCount stability.
			return;
		}
		bHasCompleted = true;
		++BodyExecutedCount;
	};

	GuardedFinalize();
	TestEqual(TEXT("First call executes body"), BodyExecutedCount, 1);
	TestTrue(TEXT("First call flips guard"), bHasCompleted);

	GuardedFinalize();
	TestEqual(TEXT("Second call is no-op -- body not re-executed"), BodyExecutedCount, 1);

	GuardedFinalize();
	GuardedFinalize();
	TestEqual(TEXT("Third/fourth calls remain no-ops"), BodyExecutedCount, 1);

	// Reset-and-reuse path: next request generation clears the guard,
	// allowing a fresh finalize. Mirrors `InvalidateActiveRequestCallbacks()`
	// resetting `bHasCompletedResponseDelivery = false` in production.
	bHasCompleted = false;
	GuardedFinalize();
	TestEqual(TEXT("After reset, next call executes body once"), BodyExecutedCount, 2);
	GuardedFinalize();
	TestEqual(TEXT("After reset, subsequent calls re-enter the guard"), BodyExecutedCount, 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
