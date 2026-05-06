// Copyright Natali Caggiano. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "OsvayderUECommandClassification.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using namespace OsvayderUE::CommandClassification;

	FOsvayderUEExecutionTruthDecision ClassifyCommand(
		const FString& Command,
		const FString& Result = FString(),
		const int32 ExitCode = INDEX_NONE,
		const FString& RawJson = FString())
	{
		FOsvayderUEExecutionTruthInputs Inputs;
		Inputs.RunId = TEXT("run_current");
		Inputs.ExpectedRunId = TEXT("run_current");
		Inputs.PlanId = TEXT("plan_current");
		Inputs.FeatureWorkflowId = TEXT("feature_current");
		Inputs.ProjectRoot = TEXT("X:/PublicExample/Unreal/SampleProject");
		Inputs.Cwd = TEXT("X:/PublicExample/Unreal/SampleProject");
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
	"OsvayderUE.CommandClassification.ExecutionTruth.CategorizesCommandExamples",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCommandClassification_OverbroadInspectionRootsAreUnsafe,
	"OsvayderUE.CommandClassification.ExecutionTruth.OverbroadInspectionRootsAreUnsafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCommandClassification_ExecutionTruthCategorizesCommandExamples::RunTest(const FString& Parameters)
{
	{
		const FOsvayderUEExecutionTruthDecision Decision =
			ClassifyCommand(TEXT("Get-Content Saved/OsvayderUE/active_plan.json"));
		TestEqual(TEXT("managed active_plan read should be read-only"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("read_only_inspection")));
		TestFalse(TEXT("managed active_plan read should not be a write"),
			Decision.bManagedStateWrite);
	}

	const TArray<FString> ManagedWriteCommands = {
		TEXT("Set-Content Saved/OsvayderUE/active_plan.json '{}'"),
		TEXT("Set-Content -LiteralPath 'Saved/OsvayderUE/active_plan.json' -Value '{}'"),
		TEXT("Out-File Saved/OsvayderUE/closeout_decision.json"),
		TEXT("Copy-Item Saved/OsvayderUE/active_plan.json Saved/OsvayderUE/PlanArchives/current.active_plan.json"),
		TEXT("Move-Item Saved/OsvayderUE/visible_session_codex_cli.json Saved/OsvayderUE/visible_session_old.json"),
		TEXT("Remove-Item Saved/OsvayderUE/agent_trace.jsonl"),
		TEXT("Get-Content Saved/Logs/proof.log > Saved/OsvayderUE/active_plan.json")
	};
	for (const FString& Command : ManagedWriteCommands)
	{
		const FOsvayderUEExecutionTruthDecision Decision = ClassifyCommand(Command);
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
			TEXT("$lines = Get-Content -LiteralPath 'Saved/OsvayderUE/agent_trace.jsonl'\n")
			TEXT("$currentLines = $lines | Where-Object { $_ -match $run }\n")
			TEXT("$manualManagedWritePattern = '(?i)(Set-Content|Out-File|Add-Content|New-Item|Copy-Item|Move-Item|Remove-Item|\\\\s[12]?>>?\\\\s*)'\n")
			TEXT("$manualManagedWrites = $currentLines | Where-Object { ($_ -match 'Saved[\\\\/]+OsvayderUE') -and ($_ -match $manualManagedWritePattern) }");
		const FOsvayderUEExecutionTruthDecision Decision = ClassifyCommand(ReadOnlyAuditCommand);
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
			TEXT("{\"type\":\"commandExecution\",\"commandActions\":[{\"type\":\"unknown\",\"command\":\"$lines = Get-Content -LiteralPath 'Saved/OsvayderUE/agent_trace.jsonl'\\n$manualManagedWritePattern = '(?i)(Set-Content|Out-File|Add-Content|New-Item|Copy-Item|Move-Item|Remove-Item)'\\n$manualManagedWrites = $lines | Where-Object { $_ -match $manualManagedWritePattern }\"}]}");
		const FOsvayderUEExecutionTruthDecision Decision =
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
		const FOsvayderUEExecutionTruthDecision Decision =
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
		const FOsvayderUEExecutionTruthDecision Decision =
			ClassifyCommand(TEXT("UnrealEditor-Cmd.exe Alternative.uproject -ExecCmds=\"Automation RunTests Alternative.PrisonAccess;Quit\""), AutomationResult, 0);
		TestEqual(TEXT("Unreal automation command should be approved build/test execution"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("approved_build_or_test_execution")));
		TestTrue(TEXT("Unreal automation command should carry typed current-run evidence"),
			Decision.bTypedCurrentRunEvidence);
	}

	{
		const FOsvayderUEExecutionTruthDecision Decision = ClassifyCommand(TEXT("Write-Output unrelated"));
		TestEqual(TEXT("arbitrary PowerShell should be unsafe/unknown"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("unsafe_or_unknown")));
	}

	{
		FOsvayderUEExecutionTruthInputs Inputs;
		Inputs.RunId = TEXT("run_old");
		Inputs.ExpectedRunId = TEXT("run_current");
		Inputs.ToolName = TEXT("command_execution");
		Inputs.CommandInput = TEXT("Get-Content Saved/Logs/old.log");
		const FOsvayderUEExecutionTruthDecision Decision = ClassifyExecutionTruth(Inputs);
		TestEqual(TEXT("old run command should be stale/out-of-run"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("stale_or_out_of_run")));
	}

	return true;
}

bool FCommandClassification_OverbroadInspectionRootsAreUnsafe::RunTest(const FString& Parameters)
{
	{
		const FOsvayderUEExecutionTruthDecision Decision = ClassifyCommand(
			TEXT("rg -n PlaySlotAnimationAsDynamicMontage \"C:\\Program Files\\Epic Games\" D:\\"));
		TestEqual(TEXT("whole-drive and engine-root rg search should be unsafe"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("unsafe_or_unknown")));
		TestEqual(TEXT("whole-drive and engine-root rg search should expose a specific reason"),
			Decision.ReasonCode,
			FString(TEXT("overbroad_root_inspection_command")));
	}

	{
		const FOsvayderUEExecutionTruthDecision Decision = ClassifyCommand(
			TEXT("rg -n PlaySlotAnimationAsDynamicMontage \"D:\\Program Files\\Epic Games\\UE_5.7\\Engine\\Source\\Runtime\\Engine\\Public\\Animation\\AnimMontage.h\""));
		TestEqual(TEXT("exact engine source/header lookup should remain read-only"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("read_only_inspection")));
	}

	{
		const FOsvayderUEExecutionTruthDecision Decision = ClassifyCommand(
			TEXT("rg -n wallrun \"X:/PublicExample/AnimationPacks/7 -Parkour Animation\""));
		TestEqual(TEXT("bounded local pack lookup should remain read-only"),
			ExecutionTruthCategoryToString(Decision.Category),
			FString(TEXT("read_only_inspection")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
