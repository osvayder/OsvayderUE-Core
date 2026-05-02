// Copyright Natali Caggiano. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "UnrealClaudeCommandClassification.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using namespace UnrealClaude::CommandClassification;

	FUnrealClaudeExecutionTruthDecision ClassifyCommand(
		const FString& Command,
		const FString& Result = FString(),
		const int32 ExitCode = INDEX_NONE,
		const FString& RawJson = FString())
	{
		FUnrealClaudeExecutionTruthInputs Inputs;
		Inputs.RunId = TEXT("run_current");
		Inputs.ExpectedRunId = TEXT("run_current");
		Inputs.PlanId = TEXT("plan_current");
		Inputs.FeatureWorkflowId = TEXT("feature_current");
		Inputs.ProjectRoot = TEXT("D:/VibeCode/Unreal/Alternative/Alternative");
		Inputs.Cwd = TEXT("D:/VibeCode/Unreal/Alternative/Alternative");
		Inputs.ToolName = TEXT("command_execution");
		Inputs.CommandInput = Command;
		Inputs.ToolResult = Result;
		Inputs.RawJson = RawJson;
		Inputs.ExitCode = ExitCode;
		return ClassifyExecutionTruth(Inputs);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCommandClassification_ExecutionTruthCategorizesCommandExamples,
	"UnrealClaude.CommandClassification.ExecutionTruth.CategorizesCommandExamples",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCommandClassification_ExecutionTruthCategorizesCommandExamples::RunTest(const FString& Parameters)
{
	{
		const FUnrealClaudeExecutionTruthDecision Decision =
			ClassifyCommand(TEXT("Get-Content Saved/UnrealClaude/active_plan.json"));
		TestEqual(TEXT("managed active_plan read should be read-only"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("read_only_inspection")));
		TestFalse(TEXT("managed active_plan read should not be a write"),
			Decision.bManagedStateWrite);
	}

	const TArray<FString> ManagedWriteCommands = {
		TEXT("Set-Content Saved/UnrealClaude/active_plan.json '{}'"),
		TEXT("Set-Content -LiteralPath 'Saved/UnrealClaude/active_plan.json' -Value '{}'"),
		TEXT("Out-File Saved/UnrealClaude/closeout_decision.json"),
		TEXT("Copy-Item Saved/UnrealClaude/active_plan.json Saved/UnrealClaude/PlanArchives/current.active_plan.json"),
		TEXT("Move-Item Saved/UnrealClaude/visible_session_codex_cli.json Saved/UnrealClaude/visible_session_old.json"),
		TEXT("Remove-Item Saved/UnrealClaude/agent_trace.jsonl"),
		TEXT("Get-Content Saved/Logs/proof.log > Saved/UnrealClaude/active_plan.json")
	};
	for (const FString& Command : ManagedWriteCommands)
	{
		const FUnrealClaudeExecutionTruthDecision Decision = ClassifyCommand(Command);
		TestEqual(
			FString::Printf(TEXT("managed write command should be managed_state_write: %s"), *Command),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("managed_state_write")));
		TestTrue(
			FString::Printf(TEXT("managed write flag should be true: %s"), *Command),
			Decision.bManagedStateWrite);
	}

	{
		const FString ReadOnlyAuditCommand =
			TEXT("$run = 'run_current'\n")
			TEXT("$lines = Get-Content -LiteralPath 'Saved/UnrealClaude/agent_trace.jsonl'\n")
			TEXT("$currentLines = $lines | Where-Object { $_ -match $run }\n")
			TEXT("$manualManagedWritePattern = '(?i)(Set-Content|Out-File|Add-Content|New-Item|Copy-Item|Move-Item|Remove-Item|\\\\s[12]?>>?\\\\s*)'\n")
			TEXT("$manualManagedWrites = $currentLines | Where-Object { ($_ -match 'Saved[\\\\/]+UnrealClaude') -and ($_ -match $manualManagedWritePattern) }");
		const FUnrealClaudeExecutionTruthDecision Decision = ClassifyCommand(ReadOnlyAuditCommand);
		TestEqual(TEXT("read-only trace audit with quoted mutation regex should be read-only"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("read_only_inspection")));
		TestTrue(TEXT("read-only trace audit still mentions managed state"),
			Decision.bManagedStateTouched);
		TestFalse(TEXT("read-only trace audit should not be a managed-state write"),
			Decision.bManagedStateWrite);
	}

	{
		const FString RawJson =
			TEXT("{\"type\":\"commandExecution\",\"commandActions\":[{\"type\":\"unknown\",\"command\":\"$lines = Get-Content -LiteralPath 'Saved/UnrealClaude/agent_trace.jsonl'\\n$manualManagedWritePattern = '(?i)(Set-Content|Out-File|Add-Content|New-Item|Copy-Item|Move-Item|Remove-Item)'\\n$manualManagedWrites = $lines | Where-Object { $_ -match $manualManagedWritePattern }\"}]}");
		const FUnrealClaudeExecutionTruthDecision Decision =
			ClassifyCommand(FString(), FString(), 0, RawJson);
		TestEqual(TEXT("raw commandActions read-only audit should be read-only"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("read_only_inspection")));
		TestFalse(TEXT("raw commandActions read-only audit should not be a managed-state write"),
			Decision.bManagedStateWrite);
	}

	{
		const FString BuildResult =
			TEXT("Running UnrealBuildTool\n")
			TEXT("Intermediate/Build/Win64/GDR_Shooter_YEditor\n")
			TEXT("Result: Succeeded\n")
			TEXT("Exit Code: 0");
		const FUnrealClaudeExecutionTruthDecision Decision =
			ClassifyCommand(TEXT("& 'D:/Program Files/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat' GDR_Shooter_YEditor Win64 Development"), BuildResult, 0);
		TestEqual(TEXT("UBT command should be approved build/test execution"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("approved_build_or_test_execution")));
		TestTrue(TEXT("UBT command should carry typed current-run evidence"),
			Decision.bTypedCurrentRunEvidence);
	}

	{
		const FString AutomationResult =
			TEXT("LogAutomationCommandLine: Display: Found 7 automation tests based on 'Alternative.PrisonAccess'\n")
			TEXT("LogAutomationController: Display: Test Completed. Result={Success} Name={ProofFixtureSmoke} Path={Alternative.PrisonAccess.ProofFixtureSmoke}\n")
			TEXT("LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****");
		const FUnrealClaudeExecutionTruthDecision Decision =
			ClassifyCommand(TEXT("UnrealEditor-Cmd.exe Alternative.uproject -ExecCmds=\"Automation RunTests Alternative.PrisonAccess;Quit\""), AutomationResult, 0);
		TestEqual(TEXT("Unreal automation command should be approved build/test execution"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("approved_build_or_test_execution")));
		TestTrue(TEXT("Unreal automation command should carry typed current-run evidence"),
			Decision.bTypedCurrentRunEvidence);
	}

	{
		const FUnrealClaudeExecutionTruthDecision Decision = ClassifyCommand(TEXT("Write-Output unrelated"));
		TestEqual(TEXT("arbitrary PowerShell should be unsafe/unknown"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("unsafe_or_unknown")));
	}

	{
		FUnrealClaudeExecutionTruthInputs Inputs;
		Inputs.RunId = TEXT("run_old");
		Inputs.ExpectedRunId = TEXT("run_current");
		Inputs.ToolName = TEXT("command_execution");
		Inputs.CommandInput = TEXT("Get-Content Saved/Logs/old.log");
		const FUnrealClaudeExecutionTruthDecision Decision = ClassifyExecutionTruth(Inputs);
		TestEqual(TEXT("old run command should be stale/out-of-run"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("stale_or_out_of_run")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
