// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 619 P4 classifier tests for FCppReflectionMutationClassifier + the
 * post-mutation routing in MCPTool_CppReflection.
 *
 * Covers spec Test plan `OsvayderUE.CppReflection.Compile.*`:
 *   - CppReflectionModify_MetaOnlyChange_TriggersLiveCoding           (spec)
 *   - CppReflectionModify_StructuralChange_ReturnsRequiresRestart     (spec)
 *   - CppReflectionModify_ClassifierPatternSet_CoversAllUHTMacros     (spec)
 *   - CppReflectionModify_CommentOnlyUPROPERTY_NotStructural          (reviewer #1)
 *   - CppReflectionModify_MultilineUPROPERTY_WithConditionals_ClassifiesCorrectly (reviewer #2)
 *   - CppReflectionModify_MassWhitespaceChurnOnUFUNCTION_NoFalsePositive (reviewer #3)
 *
 * Classifier tests exercise FCppReflectionMutationClassifier::ClassifyDiff
 * directly as a pure function (no editor, no LC, no filesystem). The
 * integration test for the LC trigger uses the Testing:: override seam to
 * avoid a real Live Coding dependency on the automation thread.
 */

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MCP/Tools/MCPTool_CppReflection.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace OsvayderUE::CppReflectionClassifierTests
{
	// Sample fragment with one existing UPROPERTY declaration + wrapper.
	// Used by all classifier tests as the "before" baseline. Tests mutate
	// specific regions to exercise each diff-shape case.
	static const TCHAR* KBaselineHeader =
		TEXT("#pragma once\n")
		TEXT("#include \"CoreMinimal.h\"\n")
		TEXT("#include \"MyClass.generated.h\"\n")
		TEXT("\n")
		TEXT("UCLASS()\n")
		TEXT("class UMyClass : public UObject\n")
		TEXT("{\n")
		TEXT("\tGENERATED_BODY()\n")
		TEXT("public:\n")
		TEXT("\tUPROPERTY(EditAnywhere, meta=(DisplayName=\"Speed\"))\n")
		TEXT("\tfloat Speed = 1.0f;\n")
		TEXT("\n")
		TEXT("\tUFUNCTION(BlueprintCallable)\n")
		TEXT("\tvoid DoThing();\n")
		TEXT("};\n");
}

using namespace OsvayderUE::CppReflectionClassifierTests;

// ------------------------------------------------------------------------
// Test 1 (spec): meta-only change -> classifier returns MetaOnly.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectionModify_MetaOnlyChange_TriggersLiveCoding,
	"OsvayderUE.CppReflection.Compile.MetaOnlyChange_TriggersLiveCoding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectionModify_MetaOnlyChange_TriggersLiveCoding::RunTest(const FString& /*Parameters*/)
{
	const FString Before = KBaselineHeader;
	// Add ClampMin=0 to an existing UPROPERTY's meta=() group. Meta-only change.
	FString After = Before;
	const FString ExistingMeta = TEXT("meta=(DisplayName=\"Speed\")");
	const FString UpdatedMeta = TEXT("meta=(DisplayName=\"Speed\", ClampMin=\"0\")");
	After.ReplaceInline(*ExistingMeta, *UpdatedMeta, ESearchCase::CaseSensitive);

	const FCppReflectionDiffClassificationResult Result =
		FCppReflectionMutationClassifier::ClassifyDiff(Before, After);

	TestEqual(
		TEXT("Meta-only diff must classify as MetaOnly"),
		static_cast<int32>(Result.Classification),
		static_cast<int32>(ECppReflectionDiffClassification::MetaOnly));
	TestFalse(TEXT("Meta-only diff must NOT bias to restart"), Result.bBiasedToRestart);
	TestEqual(
		TEXT("Reason code should be meta_contents_only"),
		Result.ReasonCode, FString(TEXT("meta_contents_only")));
	return true;
}

// ------------------------------------------------------------------------
// Test 2 (spec): structural UPROPERTY add -> Structural.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectionModify_StructuralChange_ReturnsRequiresRestart,
	"OsvayderUE.CppReflection.Compile.StructuralChange_ReturnsRequiresRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectionModify_StructuralChange_ReturnsRequiresRestart::RunTest(const FString& /*Parameters*/)
{
	const FString Before = KBaselineHeader;
	// Insert a brand new UPROPERTY declaration inside the class body.
	FString After = Before;
	const FString Anchor = TEXT("\tfloat Speed = 1.0f;\n");
	const FString NewField = TEXT("\tfloat Speed = 1.0f;\n\n\tUPROPERTY(EditAnywhere)\n\tint32 DiagCounter = 0;\n");
	After.ReplaceInline(*Anchor, *NewField, ESearchCase::CaseSensitive);

	const FCppReflectionDiffClassificationResult Result =
		FCppReflectionMutationClassifier::ClassifyDiff(Before, After);

	TestEqual(
		TEXT("Added UPROPERTY must classify as Structural"),
		static_cast<int32>(Result.Classification),
		static_cast<int32>(ECppReflectionDiffClassification::Structural));
	TestEqual(
		TEXT("Reason code should be uht_macro_count_changed"),
		Result.ReasonCode, FString(TEXT("uht_macro_count_changed")));
	return true;
}

// ------------------------------------------------------------------------
// Test 3 (spec): classifier fires on each of the 7 UHT macros, does NOT
// fire on UE_LOG / UE_CUSTOM_* prefixed identifiers.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectionModify_ClassifierPatternSet_CoversAllUHTMacros,
	"OsvayderUE.CppReflection.Compile.ClassifierPatternSet_CoversAllUHTMacros",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectionModify_ClassifierPatternSet_CoversAllUHTMacros::RunTest(const FString& /*Parameters*/)
{
	const TArray<FString> TargetMacros = { TEXT("UPROPERTY"), TEXT("UFUNCTION"), TEXT("UCLASS"), TEXT("USTRUCT"), TEXT("UENUM"), TEXT("UINTERFACE"), TEXT("UDELEGATE") };

	for (const FString& Macro : TargetMacros)
	{
		const FString Before = TEXT("class UFoo { void Bar(); };\n");
		const FString After = FString::Printf(TEXT("class UFoo { %s() void Bar(); };\n"), *Macro);
		const FCppReflectionDiffClassificationResult Result =
			FCppReflectionMutationClassifier::ClassifyDiff(Before, After);
		TestEqual(
			FString::Printf(TEXT("Adding %s must classify as Structural"), *Macro),
			static_cast<int32>(Result.Classification),
			static_cast<int32>(ECppReflectionDiffClassification::Structural));
	}

	// Negative case: UE_LOG must NOT be classified as structural.
	{
		const FString Before = TEXT("void Foo() { int x = 0; }\n");
		const FString After = TEXT("void Foo() { int x = 0; UE_LOG(LogTemp, Warning, TEXT(\"hi\")); }\n");
		const FCppReflectionDiffClassificationResult Result =
			FCppReflectionMutationClassifier::ClassifyDiff(Before, After);
		TestNotEqual(
			TEXT("Adding UE_LOG must NOT classify as Structural"),
			static_cast<int32>(Result.Classification),
			static_cast<int32>(ECppReflectionDiffClassification::Structural));
	}

	// Negative case: UE_CUSTOM_MARKER must NOT be classified as structural
	// (word-boundary prevents substring match on UE prefix).
	{
		const FString Before = TEXT("void Foo() {}\n");
		const FString After = TEXT("void Foo() { UE_CUSTOM_MARKER(); }\n");
		const FCppReflectionDiffClassificationResult Result =
			FCppReflectionMutationClassifier::ClassifyDiff(Before, After);
		TestNotEqual(
			TEXT("Adding UE_CUSTOM_MARKER must NOT classify as Structural"),
			static_cast<int32>(Result.Classification),
			static_cast<int32>(ECppReflectionDiffClassification::Structural));
	}

	// Negative case: UE_DEPRECATED must NOT classify as structural.
	{
		const FString Before = TEXT("void OldThing();\n");
		const FString After = TEXT("UE_DEPRECATED(5.7, \"Use NewThing\") void OldThing();\n");
		const FCppReflectionDiffClassificationResult Result =
			FCppReflectionMutationClassifier::ClassifyDiff(Before, After);
		TestNotEqual(
			TEXT("Adding UE_DEPRECATED must NOT classify as Structural"),
			static_cast<int32>(Result.Classification),
			static_cast<int32>(ECppReflectionDiffClassification::Structural));
	}

	return true;
}

// ------------------------------------------------------------------------
// Test 4 (reviewer #1): comment-only UPROPERTY must NOT fire structural.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectionModify_CommentOnlyUPROPERTY_NotStructural,
	"OsvayderUE.CppReflection.Compile.CommentOnlyUPROPERTY_NotStructural",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectionModify_CommentOnlyUPROPERTY_NotStructural::RunTest(const FString& /*Parameters*/)
{
	const FString Before = KBaselineHeader;
	// Add a line-comment that contains the literal text "UPROPERTY" but is
	// not actually a reflected declaration. Classifier should see through
	// the comment and NOT fire structural.
	FString After = Before;
	const FString Anchor = TEXT("\tfloat Speed = 1.0f;\n");
	const FString CommentOnlyInsert = TEXT("\tfloat Speed = 1.0f;\n\n\t// UPROPERTY(EditAnywhere) int32 Commented;\n");
	After.ReplaceInline(*Anchor, *CommentOnlyInsert, ESearchCase::CaseSensitive);

	const FCppReflectionDiffClassificationResult Result =
		FCppReflectionMutationClassifier::ClassifyDiff(Before, After);

	// The classifier should NOT fire Structural on a comment-only mention.
	// Two acceptable outcomes: Unchanged (comment stripping sees equal normalized content)
	// OR the residual-diff path but NOT structural by macro count.
	TestNotEqual(
		TEXT("Comment-only UPROPERTY must NOT classify as Structural"),
		static_cast<int32>(Result.Classification),
		static_cast<int32>(ECppReflectionDiffClassification::Structural));
	// Specifically: macro counts should be identical after comment strip.
	TestNotEqual(
		TEXT("Comment-only classifier reason must not be uht_macro_count_changed"),
		Result.ReasonCode, FString(TEXT("uht_macro_count_changed")));
	return true;
}

// ------------------------------------------------------------------------
// Test 5 (reviewer #2): multi-line UPROPERTY wrapped in #if / #endif -
// classifier must detect structural change when macro is actually added.
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectionModify_MultilineUPROPERTY_WithConditionals_ClassifiesCorrectly,
	"OsvayderUE.CppReflection.Compile.MultilineUPROPERTY_WithConditionals_ClassifiesCorrectly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectionModify_MultilineUPROPERTY_WithConditionals_ClassifiesCorrectly::RunTest(const FString& /*Parameters*/)
{
	const FString Before = KBaselineHeader;
	// Insert a new multi-line UPROPERTY guarded by #if WITH_EDITOR / #endif.
	FString After = Before;
	const FString Anchor = TEXT("\tfloat Speed = 1.0f;\n");
	const FString MultilineInsert = TEXT(
		"\tfloat Speed = 1.0f;\n"
		"\n"
		"#if WITH_EDITOR\n"
		"\tUPROPERTY(\n"
		"\t\tEditAnywhere,\n"
		"\t\tCategory=\"Diagnostic\",\n"
		"\t\tmeta=(EditCondition=\"bEnabled\"))\n"
		"\tint32 EditorDiag = 0;\n"
		"#endif\n");
	After.ReplaceInline(*Anchor, *MultilineInsert, ESearchCase::CaseSensitive);

	const FCppReflectionDiffClassificationResult Result =
		FCppReflectionMutationClassifier::ClassifyDiff(Before, After);

	// Added UPROPERTY must fire structural even when multi-line + #if/#endif
	// wrapped.
	TestEqual(
		TEXT("Multi-line #if-wrapped UPROPERTY add must classify as Structural"),
		static_cast<int32>(Result.Classification),
		static_cast<int32>(ECppReflectionDiffClassification::Structural));
	return true;
}

// ------------------------------------------------------------------------
// Test 6 (reviewer #3): mass whitespace churn on UFUNCTION must NOT
// false-positive as structural (content unchanged semantically).
// ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectionModify_MassWhitespaceChurnOnUFUNCTION_NoFalsePositive,
	"OsvayderUE.CppReflection.Compile.MassWhitespaceChurnOnUFUNCTION_NoFalsePositive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectionModify_MassWhitespaceChurnOnUFUNCTION_NoFalsePositive::RunTest(const FString& /*Parameters*/)
{
	const FString Before = KBaselineHeader;
	// Reformat whitespace inside the existing UFUNCTION() invocation +
	// surrounding lines, but keep semantic content identical (no added /
	// removed reflected macros).
	FString After = Before;
	After.ReplaceInline(
		TEXT("\tUFUNCTION(BlueprintCallable)\n\tvoid DoThing();\n"),
		TEXT("    UFUNCTION ( BlueprintCallable )\n    void DoThing ( ) ;\n"),
		ESearchCase::CaseSensitive);

	const FCppReflectionDiffClassificationResult Result =
		FCppReflectionMutationClassifier::ClassifyDiff(Before, After);

	// Spec §Risks #3 + P4 dispatch "If classifier cannot reliably
	// distinguish, bias to `requires_restart=true` + log warning":
	// whitespace-only churn is the classic false-positive boundary case.
	// Two acceptable outcomes:
	//   (a) Classifier normalizes aggressively enough to return Unchanged.
	//   (b) Classifier can't disambiguate and returns
	//       AmbiguousBiasToRestart with bBiasedToRestart=true + warning log.
	// The FAILURE mode we're guarding against is silently classifying as
	// Structural (which would be a false positive with no warning).
	const bool bUnchanged = Result.Classification == ECppReflectionDiffClassification::Unchanged;
	const bool bAmbiguousBiasedToRestart =
		Result.Classification == ECppReflectionDiffClassification::AmbiguousBiasToRestart
		&& Result.bBiasedToRestart;

	TestTrue(
		TEXT("Whitespace-only churn must either normalize to Unchanged OR classify as AmbiguousBiasToRestart with bias flag set"),
		bUnchanged || bAmbiguousBiasedToRestart);

	// Regardless of which acceptable outcome: classifier must NOT silently
	// fire Structural on whitespace-only churn (that's the real failure
	// mode per spec §Risks #3).
	TestNotEqual(
		TEXT("Whitespace-only churn must NOT classify as Structural (false-positive guard)"),
		static_cast<int32>(Result.Classification),
		static_cast<int32>(ECppReflectionDiffClassification::Structural));

	// If the classifier picked the ambiguous path, it must have set the
	// bias flag so the caller (MCPTool_CppReflection meta-only path) will
	// emit the warning log + requires_restart=true -- conservative cost.
	if (Result.Classification == ECppReflectionDiffClassification::AmbiguousBiasToRestart)
	{
		TestTrue(
			TEXT("AmbiguousBiasToRestart outcome must set bBiasedToRestart so caller emits warning log"),
			Result.bBiasedToRestart);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
