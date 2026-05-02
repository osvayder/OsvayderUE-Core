// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 626 P3 shared command-text classification utility.
 *
 * Originally introduced for 626 P2 detector hardening inside
 * `UnrealClaudeRestartSurvival.cpp` anonymous namespace. Promoted to a
 * plugin-public namespace here so 626 P3 `FCompileIntentPolicyGate`
 * (and future policy / detector surfaces) can reuse the classifiers
 * without duplicating the logic.
 *
 * All functions are pure (no I/O, no engine globals) and safe to call
 * from any thread. Behavior is byte-equivalent to the pre-P3 P2
 * inline helpers — moving the code did not change any matcher output.
 *
 * Canonical callers:
 *   - `FUnrealClaudeRestartSurvivalManager::TryDetectClosedEditorBuildBlocker`
 *     (626 P2 Layer A build-context gate)
 *   - `FCompileIntentPolicyGate::EvaluateCompileIntent`
 *     (626 P3 compile-intent policy gate)
 */
namespace UnrealClaude
{
namespace CommandClassification
{

/**
 * Peel common PowerShell / cmd / pwsh / bash wrappers + quote layers
 * off the head of a command string so the inner invocation leads.
 * Tolerant of repeated wrappers and of mixed single/double quoting.
 * Returns the inner command with whitespace trimmed. Original string
 * is not mutated.
 */
UNREALCLAUDE_API FString StripKnownShellWrapperPrefixes(const FString& CommandText);

/**
 * True if the first token of (wrapper-stripped) CommandText matches a
 * canonical read-only inspection command: Get-Content, rg, findstr,
 * grep, Get-ChildItem, head/tail, cat, git status/diff/log/show, etc.
 * Used as a hard reject gate before any build-context admission.
 */
UNREALCLAUDE_API bool IsKnownInspectionCommand(const FString& CommandText);

/**
 * True if CommandText contains a recognized build-tool invocation
 * substring (Build.bat, UnrealBuildTool, ubt.exe, RunUAT, cl.exe,
 * link.exe, msbuild, dotnet build, UnrealHeaderTool, uht.exe).
 * Case-insensitive. Used as a positive admission signal.
 */
UNREALCLAUDE_API bool IsBuildContextCommand(const FString& CommandText);

/**
 * True if LowerHaystack contains UBT/UHT/Live-Coding-characteristic
 * output markers (Running UnrealBuildTool, [LogLiveCoding],
 * Intermediate/Build/Win64, UnauthorizedAccessException, etc.).
 * Haystack must already be lowercased; function does no case folding.
 * Used as a secondary admission signal when ToolInput is absent.
 */
UNREALCLAUDE_API bool HasStructuredBuildOutputSignature(const FString& LowerHaystack);

enum class EUnrealClaudeExecutionTruthCategory : uint8
{
	ReadOnlyInspection,
	ApprovedBuildOrTestExecution,
	ApprovedProjectMutation,
	ManagedStateWrite,
	UnsafeOrUnknown,
	StaleOrOutOfRun
};

struct UNREALCLAUDE_API FUnrealClaudeExecutionTruthInputs
{
	FString RunId;
	FString ExpectedRunId;
	FString PlanId;
	FString FeatureWorkflowId;
	FString ProjectRoot;
	FString Cwd;
	FString ToolName;
	FString CommandInput;
	FString ToolFamily;
	FString ToolResult;
	FString RawJson;
	int32 ExitCode = INDEX_NONE;
	bool bIsError = false;
	bool bClassifiedMutatingTool = false;
	bool bPrimaryMutationAssigned = false;
};

struct UNREALCLAUDE_API FUnrealClaudeExecutionTruthDecision
{
	EUnrealClaudeExecutionTruthCategory Category = EUnrealClaudeExecutionTruthCategory::UnsafeOrUnknown;
	FString ReasonCode;
	FString RunId;
	FString ExpectedRunId;
	FString PlanId;
	FString FeatureWorkflowId;
	FString ProjectRoot;
	FString Cwd;
	FString ToolName;
	FString CommandInput;
	FString ToolFamily;
	int32 ExitCode = INDEX_NONE;
	bool bCurrentRun = true;
	bool bIsError = false;
	bool bManagedStateTouched = false;
	bool bManagedStateWrite = false;
	bool bTypedLogMarkersPresent = false;
	bool bExitCodePresent = false;
	bool bExitCodeSuccess = false;
	bool bTypedCurrentRunEvidence = false;
	TArray<FString> ArtifactPaths;

	FString ToSummaryString() const;
	TSharedPtr<FJsonObject> ToJsonObject() const;
};

UNREALCLAUDE_API FString ExecutionTruthCategoryToString(EUnrealClaudeExecutionTruthCategory Category);

UNREALCLAUDE_API bool IsCommandExecutionLikeToolName(const FString& ToolName);

UNREALCLAUDE_API bool IsManagedUnrealClaudeStatePathMentioned(const FString& Text);

UNREALCLAUDE_API FUnrealClaudeExecutionTruthDecision ClassifyExecutionTruth(
	const FUnrealClaudeExecutionTruthInputs& Inputs);

} // namespace CommandClassification
} // namespace UnrealClaude
