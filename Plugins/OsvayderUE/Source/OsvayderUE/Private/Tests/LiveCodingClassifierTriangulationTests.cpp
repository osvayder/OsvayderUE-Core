// Copyright Natali Caggiano. All Rights Reserved.

/**
 * 626 P4 Live Coding classifier triangulation tests.
 *
 * Covers:
 *   A-P4-2 HasPositiveReloadSignal (4 markers including observed_failures #31 canonical phrase)
 *   A-P4-3 HasPatchDllTimestampChange (mtime advance + new-DLL detection)
 *   A-P4-4 HasAbsenceOfCompileErrors (6 error patterns)
 *   A-P4-5 RunNoChangeDiagnostics (6 structured sub-fields)
 *   A-P4-6 Triangulated classifier decision table, 8 named cases
 *
 * The triangulation classifier is pure — no file I/O, no engine — so these
 * tests construct `FLiveCodingTriangulationSignals` directly and assert
 * the returned `refresh_status` string.
 *
 * Reference:
 *   - Dispatch: `AgentBridge/CODEX_TO_OSVAYDER.md` 2026-04-19 17:00.
 *   - Spec:     `Docs/OsvayderUE/626_CompileRoutingAndErrorSafetyCorrection_DispatchReady_v3.md` §626-P4.
 *   - Observed: `Docs/OsvayderUE/observed_failures.md` row #31 (UE 5.7 Cancelled-on-no-reinstance).
 */

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCP/Tools/MCPTool_LiveCodingCompile.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

using OsvayderUE::LiveCodingCompile::Testing::FLiveCodingTriangulationSignals;
using OsvayderUE::LiveCodingCompile::Testing::FLiveCodingNoChangeDiagnostics;
using OsvayderUE::LiveCodingCompile::Testing::ClassifyRefreshStatusWithTriangulation;
using OsvayderUE::LiveCodingCompile::Testing::HasPositiveReloadSignal;
using OsvayderUE::LiveCodingCompile::Testing::HasPatchDllTimestampChange;
using OsvayderUE::LiveCodingCompile::Testing::HasAbsenceOfCompileErrors;
using OsvayderUE::LiveCodingCompile::Testing::RunNoChangeDiagnostics;

namespace OsvayderUELiveCodingTriangulationDetail
{
	FLiveCodingTriangulationSignals MakeCancelledBaseline()
	{
		FLiveCodingTriangulationSignals S;
		S.bSuccess = false;
		S.bHasCancelledMarker = true;
		S.bSignal1_PositiveReload = false;
		S.bSignal2_DllMtimeChanged = false;
		S.bSignal3_NoCompileErrors = true; // no compile errors in baseline
		S.bAnyDllMtimeMeasured = true; // DLL was measured
		S.bAgentDiffExpected = false;
		S.bResultWasNoChanges = false;
		return S;
	}
}

using namespace OsvayderUELiveCodingTriangulationDetail;

// ================================================================
// A-P4-6.1 AllThreeSignalsPresent_ReturnsPatched
// All three positive signals = patched.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_AllThreeSignalsPresent_ReturnsPatched,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.AllThreeSignalsPresent_ReturnsPatched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_AllThreeSignalsPresent_ReturnsPatched::RunTest(const FString& /*Parameters*/)
{
	FLiveCodingTriangulationSignals S = MakeCancelledBaseline();
	S.bSignal1_PositiveReload = true;
	S.bSignal2_DllMtimeChanged = true;
	S.bSignal3_NoCompileErrors = true;

	const FString Status = ClassifyRefreshStatusWithTriangulation(S);
	TestEqual(TEXT("All 3 signals → live_coding_patched"), Status, FString(TEXT("live_coding_patched")));
	return true;
}

// ================================================================
// A-P4-6.2 Signal1AndSignal3Only_ReturnsPatchedNoReinstanceNeeded
// S1+S3, no S2 (DLL not rewritten because no layout change).
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_Signal1AndSignal3Only_ReturnsPatchedNoReinstanceNeeded,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.Signal1AndSignal3Only_ReturnsPatchedNoReinstanceNeeded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_Signal1AndSignal3Only_ReturnsPatchedNoReinstanceNeeded::RunTest(const FString& /*Parameters*/)
{
	FLiveCodingTriangulationSignals S = MakeCancelledBaseline();
	S.bSignal1_PositiveReload = true;
	S.bSignal2_DllMtimeChanged = false;
	S.bAnyDllMtimeMeasured = true; // measurement happened, DLL really didn't change
	S.bSignal3_NoCompileErrors = true;

	const FString Status = ClassifyRefreshStatusWithTriangulation(S);
	TestEqual(
		TEXT("S1 + S3 present, S2 absent, DLL measured → live_coding_patched_no_reinstance_needed"),
		Status,
		FString(TEXT("live_coding_patched_no_reinstance_needed")));
	return true;
}

// ================================================================
// A-P4-6.3 Signal1AbsentWithCancelled_ReturnsCancelledUnverified
// Cancelled marker but no engine reload confirmation.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_Signal1AbsentWithCancelled_ReturnsCancelledUnverified,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.Signal1AbsentWithCancelled_ReturnsCancelledUnverified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_Signal1AbsentWithCancelled_ReturnsCancelledUnverified::RunTest(const FString& /*Parameters*/)
{
	FLiveCodingTriangulationSignals S = MakeCancelledBaseline();
	S.bSignal1_PositiveReload = false;
	S.bSignal2_DllMtimeChanged = true; // ambiguous: DLL touched but no reload log
	S.bSignal3_NoCompileErrors = true;

	const FString Status = ClassifyRefreshStatusWithTriangulation(S);
	TestEqual(
		TEXT("Cancelled without S1 → live_coding_cancelled_unverified"),
		Status,
		FString(TEXT("live_coding_cancelled_unverified")));
	return true;
}

// ================================================================
// A-P4-6.4 CompileErrorPresent_ReturnsFailed
// Even with positive reload + DLL mtime change, compile error short-circuits.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_CompileErrorPresent_ReturnsFailed,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.CompileErrorPresent_ReturnsFailed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_CompileErrorPresent_ReturnsFailed::RunTest(const FString& /*Parameters*/)
{
	FLiveCodingTriangulationSignals S = MakeCancelledBaseline();
	S.bSignal1_PositiveReload = true; // even this doesn't save us
	S.bSignal2_DllMtimeChanged = true;
	S.bSignal3_NoCompileErrors = false; // compile error present

	const FString Status = ClassifyRefreshStatusWithTriangulation(S);
	TestEqual(
		TEXT("Compile error short-circuits to failed regardless of reload signal"),
		Status,
		FString(TEXT("live_coding_failed")));

	// Verify HasAbsenceOfCompileErrors also returns false for MSVC error patterns.
	TestFalse(TEXT("error C2059 is detected as compile error"),
		HasAbsenceOfCompileErrors(TEXT("Source/Foo.cpp(12): error C2059: syntax error: '{'")));
	TestFalse(TEXT("fatal error C1189 is detected"),
		HasAbsenceOfCompileErrors(TEXT("fatal error C1189: #error:  compilation terminated")));
	TestFalse(TEXT("UBT ERROR prefix is detected"),
		HasAbsenceOfCompileErrors(TEXT("UnrealBuildTool: ERROR: build failed")));
	return true;
}

// ================================================================
// A-P4-6.5 LinkErrorPresent_ReturnsFailed
// Linker error (LNK) also short-circuits to failed.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_LinkErrorPresent_ReturnsFailed,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.LinkErrorPresent_ReturnsFailed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_LinkErrorPresent_ReturnsFailed::RunTest(const FString& /*Parameters*/)
{
	FLiveCodingTriangulationSignals S = MakeCancelledBaseline();
	S.bSignal1_PositiveReload = true;
	S.bSignal2_DllMtimeChanged = true;
	S.bSignal3_NoCompileErrors = false;

	const FString Status = ClassifyRefreshStatusWithTriangulation(S);
	TestEqual(
		TEXT("Link error path also → live_coding_failed"),
		Status,
		FString(TEXT("live_coding_failed")));

	TestFalse(TEXT("error LNK2019 is detected"),
		HasAbsenceOfCompileErrors(TEXT("FooModule.obj : error LNK2019: unresolved external symbol")));
	TestFalse(TEXT("fatal error LNK1104 is detected"),
		HasAbsenceOfCompileErrors(TEXT("LINK : fatal error LNK1104: cannot open file 'foo.lib'")));
	TestFalse(TEXT("GCC undefined reference is detected"),
		HasAbsenceOfCompileErrors(TEXT("/obj/Foo.o: undefined reference to `Bar::baz()'")));
	return true;
}

// ================================================================
// A-P4-6.6 NoChangesWithExpectedDiff_EmitsAllSixDiagnosticFields
// bSuccess=true + NoChanges + agent_diff_expected → no_changes_despite_expected_diff.
// Companion assertion verifies all 6 sub-fields are addressable.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_NoChangesWithExpectedDiff_EmitsAllSixDiagnosticFields,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.NoChangesWithExpectedDiff_EmitsAllSixDiagnosticFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_NoChangesWithExpectedDiff_EmitsAllSixDiagnosticFields::RunTest(const FString& /*Parameters*/)
{
	FLiveCodingTriangulationSignals S;
	S.bSuccess = true; // underlying helper returned true
	S.bResultWasNoChanges = true;
	S.bAgentDiffExpected = true;
	S.bSignal3_NoCompileErrors = true;

	const FString Status = ClassifyRefreshStatusWithTriangulation(S);
	TestEqual(
		TEXT("bSuccess=true + NoChanges + diff expected → no_changes_despite_expected_diff"),
		Status,
		FString(TEXT("live_coding_no_changes_despite_expected_diff")));

	// Verify all 6 diagnostic sub-fields are addressable via the helper.
	// Using empty inputs gives all-false (best-effort degraded); the
	// shape of the struct is what this test pins down.
	const FLiveCodingNoChangeDiagnostics Diag = RunNoChangeDiagnostics(FString(), FString(), FString());
	TestFalse(TEXT("expected_file_edited_on_disk field addressable"), Diag.bExpectedFileEditedOnDisk);
	TestFalse(TEXT("expected_file_saved_to_disk field addressable"), Diag.bExpectedFileSavedToDisk);
	TestFalse(TEXT("file_in_compiled_module field addressable"), Diag.bFileInCompiledModule);
	TestFalse(TEXT("module_target_sees_file field addressable"), Diag.bModuleTargetSeesFile);
	TestFalse(TEXT("diff_already_applied_before_call field addressable"), Diag.bDiffAlreadyAppliedBeforeCall);
	TestFalse(TEXT("wrong_target_or_project field addressable"), Diag.bWrongTargetOrProject);
	return true;
}

// ================================================================
// A-P4-6.7 NoChangesWithoutExpectedDiff_ReturnsRegularNoChanges
// bSuccess=true + NoChanges + !agent_diff_expected → regular patched
// (classifier does not escalate when agent didn't claim a diff).
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_NoChangesWithoutExpectedDiff_ReturnsRegularNoChanges,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.NoChangesWithoutExpectedDiff_ReturnsRegularNoChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_NoChangesWithoutExpectedDiff_ReturnsRegularNoChanges::RunTest(const FString& /*Parameters*/)
{
	FLiveCodingTriangulationSignals S;
	S.bSuccess = true;
	S.bResultWasNoChanges = true;
	S.bAgentDiffExpected = false;
	S.bSignal3_NoCompileErrors = true;

	const FString Status = ClassifyRefreshStatusWithTriangulation(S);
	TestEqual(
		TEXT("NoChanges without expected diff → live_coding_patched (benign no-op)"),
		Status,
		FString(TEXT("live_coding_patched")));
	return true;
}

// ================================================================
// A-P4-6.8 DllMtimeChangedButEngineLogSilent_ReturnsCancelledUnverified
// Defensive case: DLL touched but engine log silent → ambiguous; don't claim patched.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_DllMtimeChangedButEngineLogSilent_ReturnsCancelledUnverified,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.DllMtimeChangedButEngineLogSilent_ReturnsCancelledUnverified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_DllMtimeChangedButEngineLogSilent_ReturnsCancelledUnverified::RunTest(const FString& /*Parameters*/)
{
	FLiveCodingTriangulationSignals S = MakeCancelledBaseline();
	S.bSignal1_PositiveReload = false; // engine log silent
	S.bSignal2_DllMtimeChanged = true; // DLL touched (ambiguous — could be background UBT, unrelated)
	S.bSignal3_NoCompileErrors = true;

	const FString Status = ClassifyRefreshStatusWithTriangulation(S);
	TestEqual(
		TEXT("DLL changed but engine log silent → cancelled_unverified (don't claim patched)"),
		Status,
		FString(TEXT("live_coding_cancelled_unverified")));
	return true;
}

// ================================================================
// A-P4-2 HasPositiveReloadSignal: verify all 4 canonical markers.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_HasPositiveReloadSignal_DetectsAllFourMarkers,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.HasPositiveReloadSignal_DetectsAllFourMarkers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_HasPositiveReloadSignal_DetectsAllFourMarkers::RunTest(const FString& /*Parameters*/)
{
	// Marker 1: canonical UE 5.7 phrase (most common)
	TestTrue(TEXT("Reload/Re-instancing Complete marker detected"),
		HasPositiveReloadSignal(
			TEXT("[LogLiveCoding] Display: Reload/Re-instancing Complete: 0 modules changed"),
			FString()));

	// Marker 2: observed_failures #31 canonical no-reinstance variant (from engine log)
	TestTrue(TEXT("Re-instancing complete: No object changes detected marker detected"),
		HasPositiveReloadSignal(
			FString(),
			TEXT("[LogLiveCoding] Display: Re-instancing complete: No object changes detected")));

	// Marker 3: explicit patch-loaded
	TestTrue(TEXT("Patch DLL loaded marker detected"),
		HasPositiveReloadSignal(TEXT("Patch DLL loaded successfully"), FString()));

	// Marker 4: shorter variant
	TestTrue(TEXT("Reload Complete marker detected"),
		HasPositiveReloadSignal(TEXT("Reload Complete (0 ms)"), FString()));

	// Negative: irrelevant log does not match
	TestFalse(TEXT("Unrelated log does not match"),
		HasPositiveReloadSignal(TEXT("Live coding canceled"), TEXT("Some unrelated editor log line")));

	// Negative: empty inputs
	TestFalse(TEXT("Both inputs empty returns false"),
		HasPositiveReloadSignal(FString(), FString()));

	return true;
}

// ================================================================
// A-P4-3 HasPatchDllTimestampChange: verify mtime advance + new-DLL detection.
// ================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLiveCodingTriangulation_HasPatchDllTimestampChange_DetectsAdvanceAndNewDll,
	"OsvayderUE.LiveCoding.ClassifierTriangulation.HasPatchDllTimestampChange_DetectsAdvanceAndNewDll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLiveCodingTriangulation_HasPatchDllTimestampChange_DetectsAdvanceAndNewDll::RunTest(const FString& /*Parameters*/)
{
	// Case 1: mtime advanced on existing DLL.
	{
		TMap<FString, FDateTime> Pre;
		TMap<FString, FDateTime> Post;
		const FDateTime T0 = FDateTime(2026, 4, 19, 10, 0, 0);
		const FDateTime T1 = FDateTime(2026, 4, 19, 10, 5, 0);
		Pre.Add(TEXT("UnrealEditor-OsvayderUE.dll"), T0);
		Post.Add(TEXT("UnrealEditor-OsvayderUE.dll"), T1);
		TestTrue(TEXT("Existing DLL with advanced mtime detected"),
			HasPatchDllTimestampChange(Pre, Post));
	}

	// Case 2: new Patch DLL appeared only post-call.
	{
		TMap<FString, FDateTime> Pre;
		TMap<FString, FDateTime> Post;
		Pre.Add(TEXT("UnrealEditor-OsvayderUE.dll"), FDateTime(2026, 4, 19, 10, 0, 0));
		Post.Add(TEXT("UnrealEditor-OsvayderUE.dll"), FDateTime(2026, 4, 19, 10, 0, 0));
		Post.Add(TEXT("UnrealEditor-OsvayderUE.Patch_1.dll"), FDateTime(2026, 4, 19, 10, 5, 0));
		TestTrue(TEXT("New Patch DLL appearing post-call detected"),
			HasPatchDllTimestampChange(Pre, Post));
	}

	// Case 3: no changes — mtime identical.
	{
		TMap<FString, FDateTime> Pre;
		TMap<FString, FDateTime> Post;
		const FDateTime T = FDateTime(2026, 4, 19, 10, 0, 0);
		Pre.Add(TEXT("UnrealEditor-OsvayderUE.dll"), T);
		Post.Add(TEXT("UnrealEditor-OsvayderUE.dll"), T);
		TestFalse(TEXT("Identical mtimes return false"),
			HasPatchDllTimestampChange(Pre, Post));
	}

	// Case 4: both empty — false.
	{
		TMap<FString, FDateTime> Pre;
		TMap<FString, FDateTime> Post;
		TestFalse(TEXT("Empty maps return false"),
			HasPatchDllTimestampChange(Pre, Post));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
