// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_DependencyHealth.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString GetDependencyHealthLogPath(const FString& FileName)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), FileName);
	}

	FString GetDependencyHealthFixturePath(const FString& FileName)
	{
		return FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UnrealClaude"),
			TEXT("Source"),
			TEXT("UnrealClaude"),
			TEXT("Private"),
			TEXT("Tests"),
			TEXT("Fixtures"),
			FileName);
	}

	TSharedPtr<FJsonObject> FindFindingByKey(const TSharedPtr<FJsonObject>& ResultData, const FString& DependencyKey)
	{
		if (!ResultData.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
		if (!ResultData->TryGetArrayField(TEXT("findings"), Findings) || !Findings)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& FindingValue : *Findings)
		{
			const TSharedPtr<FJsonObject> FindingObject = FindingValue.IsValid() ? FindingValue->AsObject() : nullptr;
			if (!FindingObject.IsValid())
			{
				continue;
			}

			FString CurrentKey;
			if (FindingObject->TryGetStringField(TEXT("dependency_key"), CurrentKey) && CurrentKey == DependencyKey)
			{
				return FindingObject;
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> GetObjectFieldOrNull(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* NestedObject = nullptr;
		if (!Object->TryGetObjectField(FieldName, NestedObject) || !NestedObject || !(*NestedObject).IsValid())
		{
			return nullptr;
		}

		return *NestedObject;
	}

	bool JsonStringArrayContains(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& ExpectedValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString ActualValue;
			if (Value.IsValid() && Value->TryGetString(ActualValue) && ActualValue == ExpectedValue)
			{
				return true;
			}
		}

		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDependencyHealth_RegistryToolsRegistered,
	"UnrealClaude.DependencyHealth.Registry.ToolsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FDependencyHealth_RegistryToolsRegistered::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	IMCPTool* Tool = Registry.FindTool(TEXT("dependency_health"));
	TestNotNull(TEXT("dependency_health tool should be registered"), Tool);
	return Tool != nullptr;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDependencyHealth_ClassifyHistoricalMountIssue,
	"UnrealClaude.DependencyHealth.ClassifyHistoricalMountIssue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FDependencyHealth_ClassifyHistoricalMountIssue::RunTest(const FString& Parameters)
{
	const FString HistoricalLogPath = GetDependencyHealthFixturePath(TEXT("dependency_health_historical_mount_fixture.log"));
	TestTrue(TEXT("historical dependency fixture log should exist"), FPaths::FileExists(HistoricalLogPath));
	if (!FPaths::FileExists(HistoricalLogPath))
	{
		return false;
	}

	FMCPTool_DependencyHealth Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("log_path"), HistoricalLogPath);
	Params->SetNumberField(TEXT("max_findings"), 12);

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("dependency_health historical classification should succeed"), Result.bSuccess);
	TestTrue(TEXT("dependency_health historical classification should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Finding = FindFindingByKey(Result.Data, TEXT("/Engine/BasicShapes/Cube"));
	TestTrue(TEXT("should classify resolved historical engine package"), Finding.IsValid());
	if (!Finding.IsValid())
	{
		return false;
	}

	FString ContentImpactClass;
	FString CurrentState;
	FString RecommendationClass;
	FString RecommendationLane;
	Finding->TryGetStringField(TEXT("content_impact_class"), ContentImpactClass);
	Finding->TryGetStringField(TEXT("current_state"), CurrentState);
	Finding->TryGetStringField(TEXT("recommendation_class"), RecommendationClass);
	Finding->TryGetStringField(TEXT("recommendation_lane"), RecommendationLane);

	TestEqual(TEXT("historical engine package should classify as legacy or resolved"), ContentImpactClass, FString(TEXT("likely_legacy_or_resolved")));
	TestEqual(TEXT("historical engine package should be resolved in current state"), CurrentState, FString(TEXT("resolved_in_current_state")));
	TestEqual(TEXT("historical engine package should not demand repair now"), RecommendationClass, FString(TEXT("resolved_no_action")));
	TestEqual(TEXT("historical engine package should expose no_action lane"), RecommendationLane, FString(TEXT("no_action")));

	const TSharedPtr<FJsonObject> CurrentStateProof = GetObjectFieldOrNull(Finding, TEXT("current_state_proof"));
	TestTrue(TEXT("resolved historical finding should expose current_state_proof"), CurrentStateProof.IsValid());
	if (CurrentStateProof.IsValid())
	{
		FString ProofStrength;
		CurrentStateProof->TryGetStringField(TEXT("proof_strength"), ProofStrength);
		TestEqual(TEXT("resolved historical finding should expose strong proof"), ProofStrength, FString(TEXT("strong")));
		TestTrue(TEXT("resolved historical finding should expose package_path_resolution input"), JsonStringArrayContains(CurrentStateProof, TEXT("proof_inputs_used"), TEXT("package_path_resolution")));
	}

	const TSharedPtr<FJsonObject> RecommendationDetail = GetObjectFieldOrNull(Finding, TEXT("recommendation_detail"));
	TestTrue(TEXT("resolved historical finding should expose recommendation_detail"), RecommendationDetail.IsValid());
	if (RecommendationDetail.IsValid())
	{
		FString ActionFamily;
		RecommendationDetail->TryGetStringField(TEXT("action_family"), ActionFamily);
		TestEqual(TEXT("resolved historical finding should expose historical_resolution action family"), ActionFamily, FString(TEXT("historical_resolution")));

		const TSharedPtr<FJsonObject> NextStep = GetObjectFieldOrNull(RecommendationDetail, TEXT("next_step"));
		TestTrue(TEXT("resolved historical finding should expose bounded next_step"), NextStep.IsValid());
		if (NextStep.IsValid())
		{
			FString StepClass;
			NextStep->TryGetStringField(TEXT("step_class"), StepClass);
			TestEqual(TEXT("resolved historical finding should point at no_action recheck"), StepClass, FString(TEXT("no_action_recheck_on_regression")));
		}
	}

	const TSharedPtr<FJsonObject> AnimMontageFinding = FindFindingByKey(Result.Data, TEXT("Class:AnimMontage"));
	TestTrue(TEXT("should classify historical AnimMontage class import"), AnimMontageFinding.IsValid());
	if (AnimMontageFinding.IsValid())
	{
		FString MontageCurrentState;
		FString MontageRecommendationClass;
		AnimMontageFinding->TryGetStringField(TEXT("current_state"), MontageCurrentState);
		AnimMontageFinding->TryGetStringField(TEXT("recommendation_class"), MontageRecommendationClass);
		TestEqual(TEXT("AnimMontage historical class import should be resolved in current state"), MontageCurrentState, FString(TEXT("resolved_in_current_state")));
		TestEqual(TEXT("AnimMontage historical class import should not demand repair now"), MontageRecommendationClass, FString(TEXT("resolved_no_action")));
	}

	const TSharedPtr<FJsonObject> AnimSequenceFinding = FindFindingByKey(Result.Data, TEXT("Class:AnimSequence"));
	TestTrue(TEXT("should classify historical AnimSequence class import"), AnimSequenceFinding.IsValid());
	if (AnimSequenceFinding.IsValid())
	{
		FString SequenceCurrentState;
		FString SequenceRecommendationClass;
		AnimSequenceFinding->TryGetStringField(TEXT("current_state"), SequenceCurrentState);
		AnimSequenceFinding->TryGetStringField(TEXT("recommendation_class"), SequenceRecommendationClass);
		TestEqual(TEXT("AnimSequence historical class import should be resolved in current state"), SequenceCurrentState, FString(TEXT("resolved_in_current_state")));
		TestEqual(TEXT("AnimSequence historical class import should not demand repair now"), SequenceRecommendationClass, FString(TEXT("resolved_no_action")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDependencyHealth_ClassifyCurrentScriptPackages,
	"UnrealClaude.DependencyHealth.ClassifyCurrentScriptPackages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FDependencyHealth_ClassifyCurrentScriptPackages::RunTest(const FString& Parameters)
{
	const FString CurrentLogPath = GetDependencyHealthFixturePath(TEXT("dependency_health_current_script_packages_fixture.log"));
	TestTrue(TEXT("current dependency fixture log should exist"), FPaths::FileExists(CurrentLogPath));
	if (!FPaths::FileExists(CurrentLogPath))
	{
		return false;
	}

	FMCPTool_DependencyHealth Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("log_path"), CurrentLogPath);
	Params->SetNumberField(TEXT("max_findings"), 12);

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("dependency_health current classification should succeed"), Result.bSuccess);
	TestTrue(TEXT("dependency_health current classification should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> SpatializationFinding = FindFindingByKey(Result.Data, TEXT("/Script/Spatialization"));
	TestTrue(TEXT("should classify /Script/Spatialization"), SpatializationFinding.IsValid());
	if (!SpatializationFinding.IsValid())
	{
		return false;
	}

	FString ContentImpactClass;
	FString CurrentState;
	FString RecommendationClass;
	FString RecommendationLane;
	FString RecommendationActionFamily;
	SpatializationFinding->TryGetStringField(TEXT("content_impact_class"), ContentImpactClass);
	SpatializationFinding->TryGetStringField(TEXT("current_state"), CurrentState);
	SpatializationFinding->TryGetStringField(TEXT("recommendation_class"), RecommendationClass);
	SpatializationFinding->TryGetStringField(TEXT("recommendation_lane"), RecommendationLane);
	SpatializationFinding->TryGetStringField(TEXT("recommendation_action_family"), RecommendationActionFamily);

	TestEqual(TEXT("Spatialization finding should classify as candidate active without packaging override"), ContentImpactClass, FString(TEXT("candidate_active")));
	TestEqual(TEXT("Spatialization finding should remain unresolved"), CurrentState, FString(TEXT("unresolved")));
	TestEqual(TEXT("Spatialization finding should recommend runtime proof"), RecommendationClass, FString(TEXT("needs_runtime_proof")));
	TestEqual(TEXT("Spatialization finding should expose runtime_proof_next lane"), RecommendationLane, FString(TEXT("runtime_proof_next")));
	TestEqual(TEXT("Spatialization finding should expose runtime_validation action family"), RecommendationActionFamily, FString(TEXT("runtime_validation")));

	const TSharedPtr<FJsonObject> SpatializationProof = GetObjectFieldOrNull(SpatializationFinding, TEXT("current_state_proof"));
	TestTrue(TEXT("Spatialization finding should expose current_state_proof"), SpatializationProof.IsValid());
	if (SpatializationProof.IsValid())
	{
		FString ProofStrength;
		SpatializationProof->TryGetStringField(TEXT("proof_strength"), ProofStrength);
		TestEqual(TEXT("Spatialization finding should expose strong proof"), ProofStrength, FString(TEXT("strong")));
		TestTrue(TEXT("Spatialization proof should cite module_load_state"), JsonStringArrayContains(SpatializationProof, TEXT("proof_inputs_used"), TEXT("module_load_state")));
	}

	const TSharedPtr<FJsonObject> SpatializationRecommendation = GetObjectFieldOrNull(SpatializationFinding, TEXT("recommendation_detail"));
	TestTrue(TEXT("Spatialization finding should expose recommendation_detail"), SpatializationRecommendation.IsValid());
	if (SpatializationRecommendation.IsValid())
	{
		const TSharedPtr<FJsonObject> NextStep = GetObjectFieldOrNull(SpatializationRecommendation, TEXT("next_step"));
		TestTrue(TEXT("Spatialization recommendation should expose bounded next_step"), NextStep.IsValid());
		if (NextStep.IsValid())
		{
			FString StepClass;
			NextStep->TryGetStringField(TEXT("step_class"), StepClass);
			TestEqual(TEXT("Spatialization next_step should point at runtime_validation_probe"), StepClass, FString(TEXT("runtime_validation_probe")));
		}
	}

	const TSharedPtr<FJsonObject> LyraFinding = FindFindingByKey(Result.Data, TEXT("/Script/LyraGame"));
	TestTrue(TEXT("should classify /Script/LyraGame"), LyraFinding.IsValid());
	if (LyraFinding.IsValid())
	{
		FString LyraCurrentState;
		FString LyraRecommendationLane;
		FString LyraActionFamily;
		LyraFinding->TryGetStringField(TEXT("current_state"), LyraCurrentState);
		LyraFinding->TryGetStringField(TEXT("recommendation_lane"), LyraRecommendationLane);
		LyraFinding->TryGetStringField(TEXT("recommendation_action_family"), LyraActionFamily);
		TestEqual(TEXT("LyraGame finding should remain unresolved"), LyraCurrentState, FString(TEXT("unresolved")));
		TestEqual(TEXT("LyraGame finding should expose runtime_proof_next lane"), LyraRecommendationLane, FString(TEXT("runtime_proof_next")));
		TestEqual(TEXT("LyraGame finding should expose runtime_validation action family"), LyraActionFamily, FString(TEXT("runtime_validation")));

		const TSharedPtr<FJsonObject> LyraRecommendation = GetObjectFieldOrNull(LyraFinding, TEXT("recommendation_detail"));
		TestTrue(TEXT("LyraGame finding should expose recommendation_detail"), LyraRecommendation.IsValid());
		if (LyraRecommendation.IsValid())
		{
			const TSharedPtr<FJsonObject> NextStep = GetObjectFieldOrNull(LyraRecommendation, TEXT("next_step"));
			TestTrue(TEXT("LyraGame recommendation should expose next_step"), NextStep.IsValid());
			if (NextStep.IsValid())
			{
				bool bRequiresRuntimeEvidence = false;
				TestTrue(TEXT("LyraGame runtime proof path should require runtime evidence"), NextStep->TryGetBoolField(TEXT("requires_runtime_evidence"), bRequiresRuntimeEvidence));
				TestTrue(TEXT("LyraGame runtime proof path should require runtime evidence"), bRequiresRuntimeEvidence);
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDependencyHealth_ExportReportArtifact,
	"UnrealClaude.DependencyHealth.ExportReportArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDependencyHealth_ClassifyDemoteOrDeferFixture,
	"UnrealClaude.DependencyHealth.ClassifyDemoteOrDeferFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FDependencyHealth_ClassifyDemoteOrDeferFixture::RunTest(const FString& Parameters)
{
	const FString FixtureLogPath = GetDependencyHealthFixturePath(TEXT("dependency_health_demote_or_defer_fixture.log"));
	TestTrue(TEXT("demote_or_defer fixture log should exist"), FPaths::FileExists(FixtureLogPath));
	if (!FPaths::FileExists(FixtureLogPath))
	{
		return false;
	}

	FMCPTool_DependencyHealth Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("log_path"), FixtureLogPath);
	Params->SetNumberField(TEXT("max_findings"), 8);

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("dependency_health fixture classification should succeed"), Result.bSuccess);
	TestTrue(TEXT("dependency_health fixture classification should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Finding = FindFindingByKey(Result.Data, TEXT("/Script/PrototypeRuntime"));
	TestTrue(TEXT("fixture should classify /Script/PrototypeRuntime"), Finding.IsValid());
	if (!Finding.IsValid())
	{
		return false;
	}

	FString ContentImpactClass;
	FString CurrentState;
	FString RecommendationClass;
	FString RecommendationLane;
	FString RecommendationActionFamily;
	Finding->TryGetStringField(TEXT("content_impact_class"), ContentImpactClass);
	Finding->TryGetStringField(TEXT("current_state"), CurrentState);
	Finding->TryGetStringField(TEXT("recommendation_class"), RecommendationClass);
	Finding->TryGetStringField(TEXT("recommendation_lane"), RecommendationLane);
	Finding->TryGetStringField(TEXT("recommendation_action_family"), RecommendationActionFamily);

	TestEqual(TEXT("fixture finding should classify as likely legacy or resolved"), ContentImpactClass, FString(TEXT("likely_legacy_or_resolved")));
	TestEqual(TEXT("fixture finding should remain unresolved"), CurrentState, FString(TEXT("unresolved")));
	TestEqual(TEXT("fixture finding should recommend demote_or_defer"), RecommendationClass, FString(TEXT("demote_or_defer_candidate")));
	TestEqual(TEXT("fixture finding should expose demote_or_defer lane"), RecommendationLane, FString(TEXT("demote_or_defer")));
	TestEqual(TEXT("fixture finding should expose scope_review action family"), RecommendationActionFamily, FString(TEXT("scope_review")));

	const TSharedPtr<FJsonObject> CurrentStateProof = GetObjectFieldOrNull(Finding, TEXT("current_state_proof"));
	TestTrue(TEXT("fixture finding should expose current_state_proof"), CurrentStateProof.IsValid());
	if (CurrentStateProof.IsValid())
	{
		FString ProofStrength;
		CurrentStateProof->TryGetStringField(TEXT("proof_strength"), ProofStrength);
		TestEqual(TEXT("fixture finding should expose strong proof"), ProofStrength, FString(TEXT("strong")));
		TestTrue(TEXT("fixture proof should cite module_load_state"), JsonStringArrayContains(CurrentStateProof, TEXT("proof_inputs_used"), TEXT("module_load_state")));
		TestTrue(TEXT("fixture proof should cite logged_package_presence"), JsonStringArrayContains(CurrentStateProof, TEXT("proof_inputs_used"), TEXT("logged_package_presence")));
		TestTrue(TEXT("fixture proof should record missing logged package"), JsonStringArrayContains(CurrentStateProof, TEXT("current_state_basis"), TEXT("missing_logged_package=/Game/Legacy/Deprecated/BP_DeprecatedPrototype")));
	}

	const TSharedPtr<FJsonObject> RecommendationDetail = GetObjectFieldOrNull(Finding, TEXT("recommendation_detail"));
	TestTrue(TEXT("fixture finding should expose recommendation_detail"), RecommendationDetail.IsValid());
	if (RecommendationDetail.IsValid())
	{
		const TSharedPtr<FJsonObject> NextStep = GetObjectFieldOrNull(RecommendationDetail, TEXT("next_step"));
		TestTrue(TEXT("fixture recommendation should expose next_step"), NextStep.IsValid());
		if (NextStep.IsValid())
		{
			FString StepClass;
			bool bRequiresRuntimeEvidence = true;
			NextStep->TryGetStringField(TEXT("step_class"), StepClass);
			TestEqual(TEXT("fixture next_step should point at scope_review"), StepClass, FString(TEXT("scope_review")));
			TestTrue(TEXT("fixture demote/defer should not require runtime evidence"), NextStep->TryGetBoolField(TEXT("requires_runtime_evidence"), bRequiresRuntimeEvidence));
			TestFalse(TEXT("fixture demote/defer should not require runtime evidence"), bRequiresRuntimeEvidence);
		}
	}

	return true;
}

bool FDependencyHealth_ExportReportArtifact::RunTest(const FString& Parameters)
{
	const FString HistoricalLog = GetDependencyHealthFixturePath(TEXT("dependency_health_historical_mount_fixture.log"));
	const FString CurrentLog = GetDependencyHealthFixturePath(TEXT("dependency_health_current_script_packages_fixture.log"));
	const FString FixtureLog = GetDependencyHealthFixturePath(TEXT("dependency_health_demote_or_defer_fixture.log"));
	TestTrue(TEXT("historical log should exist"), FPaths::FileExists(HistoricalLog));
	TestTrue(TEXT("current log should exist"), FPaths::FileExists(CurrentLog));
	TestTrue(TEXT("fixture log should exist"), FPaths::FileExists(FixtureLog));
	if (!FPaths::FileExists(HistoricalLog) || !FPaths::FileExists(CurrentLog) || !FPaths::FileExists(FixtureLog))
	{
		return false;
	}

	FMCPTool_DependencyHealth Tool;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LogPaths;
	LogPaths.Add(MakeShared<FJsonValueString>(HistoricalLog));
	LogPaths.Add(MakeShared<FJsonValueString>(CurrentLog));
	LogPaths.Add(MakeShared<FJsonValueString>(FixtureLog));
	Params->SetArrayField(TEXT("log_paths"), LogPaths);
	Params->SetBoolField(TEXT("export_report"), true);
	Params->SetStringField(TEXT("report_name"), TEXT("Dependency Health Automation Probe"));
	Params->SetStringField(TEXT("report_slug"), TEXT("dependency_health_automation_probe"));

	const FMCPToolResult Result = Tool.Execute(Params);
	TestTrue(TEXT("dependency_health export classification should succeed"), Result.bSuccess);
	TestTrue(TEXT("dependency_health export classification should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ArtifactObjectPtr = nullptr;
	TestTrue(TEXT("report_artifact should exist"), Result.Data->TryGetObjectField(TEXT("report_artifact"), ArtifactObjectPtr) && ArtifactObjectPtr && (*ArtifactObjectPtr).IsValid());
	if (!ArtifactObjectPtr || !(*ArtifactObjectPtr).IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> SummaryObject = GetObjectFieldOrNull(Result.Data, TEXT("summary"));
	TestTrue(TEXT("summary should exist"), SummaryObject.IsValid());
	if (SummaryObject.IsValid())
	{
		double RepairNowCount = 0.0;
		double RuntimeProofNextCount = 0.0;
		double DemoteOrDeferCount = 0.0;
		double NoActionCount = 0.0;
		TestTrue(TEXT("summary should expose repair_now_count"), SummaryObject->TryGetNumberField(TEXT("repair_now_count"), RepairNowCount));
		TestTrue(TEXT("summary should expose runtime_proof_next_count"), SummaryObject->TryGetNumberField(TEXT("runtime_proof_next_count"), RuntimeProofNextCount));
		TestTrue(TEXT("summary should expose demote_or_defer_count"), SummaryObject->TryGetNumberField(TEXT("demote_or_defer_count"), DemoteOrDeferCount));
		TestTrue(TEXT("summary should expose no_action_count"), SummaryObject->TryGetNumberField(TEXT("no_action_count"), NoActionCount));
		TestTrue(TEXT("summary should classify at least one actionable dependency lane"), (RepairNowCount + RuntimeProofNextCount) >= 1.0);
		TestTrue(TEXT("summary should count at least one demote_or_defer finding"), DemoteOrDeferCount >= 1.0);
		TestTrue(TEXT("summary should count at least one no_action finding"), NoActionCount >= 1.0);
	}

	FString MarkdownPath;
	FString SummaryPath;
	FString StatusTool;
	(*ArtifactObjectPtr)->TryGetStringField(TEXT("markdown_path"), MarkdownPath);
	(*ArtifactObjectPtr)->TryGetStringField(TEXT("summary_path"), SummaryPath);
	(*ArtifactObjectPtr)->TryGetStringField(TEXT("status_tool"), StatusTool);

	TestTrue(TEXT("exported markdown should exist"), FPaths::FileExists(MarkdownPath));
	TestTrue(TEXT("exported summary should exist"), FPaths::FileExists(SummaryPath));
	TestEqual(TEXT("status tool should be report_artifact_status"), StatusTool, FString(TEXT("report_artifact_status")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
