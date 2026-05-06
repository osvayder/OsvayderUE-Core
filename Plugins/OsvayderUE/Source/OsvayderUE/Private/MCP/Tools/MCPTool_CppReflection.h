// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCP/MCPToolBase.h"
#include "Templates/Function.h"

/**
 * MCP Tool: reflected C++ contract discovery plus one bounded mutation lane.
 *
 * This tool inspects loaded native reflection objects under /Script modules.
 * In CF1 slice 2 it also supports one narrow authoring path:
 * - preview/apply a bounded UPROPERTY metadata upsert on a plugin-owned header
 *
 * In Post-ULTRA P1.1 slice 1 it adds one preview-only declaration foundation:
 * - preview one plugin-owned reflected bool property declaration shape after
 *   an existing declared UPROPERTY anchor, without applying it
 *
 * In Post-ULTRA P1.1 slice 2 it extends that same exact declaration shape with:
 * - one bounded apply lane plus one receipt-backed revert lane
 * - both remain plugin-only, rebuild-gated, and narrower than arbitrary source editing
 *
 * It does not claim arbitrary C++ parsing, arbitrary source edits, or broad
 * mutation autonomy beyond that single plugin-scoped reflected lane.
 */
class FMCPTool_CppReflection : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteListReflectedContracts(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetReflectedContract(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecutePreviewReflectedPropertyDeclaration(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteApplyReflectedPropertyDeclaration(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRevertReflectedPropertyDeclaration(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteInspectReflectedPropertyDeclarationBuildFailure(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteBuildReflectedPropertyDeclarationEvidenceBundle(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecutePreviewPropertyMetadataMutation(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteApplyPropertyMetadataMutation(const TSharedRef<FJsonObject>& Params);
};

/**
 * 619 P4 Fix #4: classifier for meta-only vs structural C++ diffs.
 *
 * Given a "before" + "after" text snapshot of a plugin-owned header, decide
 * whether the diff is safe for a Live Coding hot-patch (meta-only: `meta=()`
 * contents changes on an existing reflected macro) or requires a full editor
 * restart (structural: adds/removes/rewrites a UPROPERTY/UFUNCTION/UCLASS/
 * USTRUCT/UENUM/UINTERFACE/UDELEGATE macro invocation).
 *
 * Conservative bias per spec D4 + Risks #3: when the classifier is uncertain
 * whether a diff is meta-only vs structural, it returns `AmbiguousBiasToRestart`
 * which the caller treats as structural. False positive costs ~2 minutes
 * (unnecessary restart). False negative corrupts loaded DLL with mismatched
 * reflected layout. Asymmetric cost -> bias to conservative.
 */
enum class ECppReflectionDiffClassification : uint8
{
	/** before == after (no semantic change detected); caller may skip both LC and restart. */
	Unchanged,

	/** Only the `meta=()` group contents changed; LC hot-patch is safe. */
	MetaOnly,

	/** A structural reflected macro was added/removed/rewritten; requires editor restart. */
	Structural,

	/** Classifier could not reliably disambiguate; conservative bias treats this as structural + emits a warning. */
	AmbiguousBiasToRestart
};

struct FCppReflectionDiffClassificationResult
{
	ECppReflectionDiffClassification Classification = ECppReflectionDiffClassification::Unchanged;

	/** Short machine-readable reason code, e.g. `"uproperty_macro_added"`, `"meta_contents_only"`, `"whitespace_churn_ambiguous"`. */
	FString ReasonCode;

	/** One-sentence human-readable explanation for logging / tool result transparency. */
	FString ReasonDetail;

	/** True when the classifier biased to restart due to ambiguity (agent should see a warning in logs). */
	bool bBiasedToRestart = false;
};

/**
 * Pure classifier. No I/O, no LC dependency. Safe to unit-test.
 * Input: full text of the header before + after the mutation.
 * Output: classification + reason strings.
 */
class FCppReflectionMutationClassifier
{
public:
	static FCppReflectionDiffClassificationResult ClassifyDiff(
		const FString& BeforeSource,
		const FString& AfterSource);
};

namespace OsvayderUE
{
namespace CppReflectionCompile
{
/**
 * 619 P4 test-only seams. Pair each setter with Clear*() or a ScopeExit
 * so the next test starts clean. Null TFunction = fallthrough to real
 * FScriptExecutionManager::TriggerLiveCodingCompile call.
 */
namespace Testing
{
	struct FMockTriggerLiveCodingCompileResult
	{
		bool bSuccess = false;
		FString ErrorLog;
		TSharedPtr<FJsonObject> Diagnostics;
	};

	/** Override for the meta-only path's call to TriggerLiveCodingCompile. */
	void SetTriggerLiveCodingCompileOverride(TFunction<FMockTriggerLiveCodingCompileResult()> Override);

	/** Clear all CppReflection test overrides. */
	void ClearAllOverrides();
}
} // namespace CppReflectionCompile
} // namespace OsvayderUE
