// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "OsvayderEditorWidget.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "OsvayderUERecipeRegistry.h"
#include "OsvayderUERoleRegistry.h"
#include "OsvayderUEUserFacingStatus.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FOsvayderUEActivePlan MakeUserFacingStatusPlan()
	{
		FOsvayderUEActivePlan Plan;
		Plan.PlanId = TEXT("plan_658_user_status");
		Plan.Status = TEXT("active");
		Plan.ResultStatus = TEXT("incomplete");
		Plan.FeatureWorkflow.FeatureWorkflowId = TEXT("workflow_658_user_status");
		Plan.FeatureWorkflow.RecipeId = OsvayderUERecipeRegistry::InteractionAccessRecipeId();
		Plan.FeatureWorkflow.RoleId = OsvayderUERoleRegistry::WorkerRoleId();
		Plan.FeatureWorkflow.EvidenceSchemaVersion = 1;
		Plan.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
		Plan.FeatureWorkflow.bRuntimeProofRequired = true;
		Plan.CurrentMechanicId = Plan.FeatureWorkflow.CurrentPhase;
		return Plan;
	}

	FOsvayderUEActivePlanCloseoutDecision MakeCloseoutDecision(
		const FString& PlanStatus,
		const FString& ResultStatus,
		const FString& GateReasonCode = FString())
	{
		FOsvayderUEActivePlanCloseoutDecision Decision;
		Decision.PlanStatus = PlanStatus;
		Decision.ResultStatus = ResultStatus;
		Decision.GateReasonCode = GateReasonCode;
		Decision.SourceRunId = TEXT("run_658_user_status");
		Decision.SourcePlanId = TEXT("plan_658_user_status");
		Decision.SourceFeatureWorkflowId = TEXT("workflow_658_user_status");
		Decision.SourceRecipeId = OsvayderUERecipeRegistry::InteractionAccessRecipeId();
		Decision.SourceRoleId = OsvayderUERoleRegistry::VerifierRoleId();
		Decision.EvidenceSchemaVersion = 1;
		Decision.bFeatureWorkflowRequired = true;
		Decision.bRecipeContractResolved = true;
		Decision.bRoleContractResolved = true;
		return Decision;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_WorkRunning,
	"OsvayderUE.UserFacingStatus.WorkRunning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_WorkRunning::RunTest(const FString& Parameters)
{
	FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("pending");
	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan);

	TestEqual(TEXT("active pending work should map to work_running"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::WorkRunningStatusId());
	TestEqual(TEXT("work_running headline should be product-owned"),
		Status.Headline,
		FString(TEXT("Work is running")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_RuntimeProofPassedButCloseoutPending,
	"OsvayderUE.UserFacingStatus.RuntimeProofPassedButCloseoutPending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_RuntimeProofPassedButCloseoutPending::RunTest(const FString& Parameters)
{
	FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");
	Plan.FeatureWorkflow.CompletedPhaseIds.AddUnique(TEXT("runtime_proof"));
	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan);

	TestEqual(TEXT("runtime proof success alone should not be final closeout"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::RuntimeProofPassedStatusId());
	TestNotEqual(TEXT("runtime proof passed must be distinct from closeout_passed"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::CloseoutPassedStatusId());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_CloseoutPassedCarriesIdentifiers,
	"OsvayderUE.UserFacingStatus.CloseoutPassedCarriesIdentifiers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_CloseoutPassedCarriesIdentifiers::RunTest(const FString& Parameters)
{
	const FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(TEXT("done"), TEXT("achieved_fully"));
	Decision.bRuntimeProofPassed = true;

	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision);

	TestEqual(TEXT("full achieved closeout should map to closeout_passed"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::CloseoutPassedStatusId());
	TestEqual(TEXT("run id should be preserved"), Status.RunId, FString(TEXT("run_658_user_status")));
	TestEqual(TEXT("plan id should be preserved"), Status.PlanId, FString(TEXT("plan_658_user_status")));
	TestEqual(TEXT("recipe id should be preserved"), Status.RecipeId, OsvayderUERecipeRegistry::InteractionAccessRecipeId());
	TestEqual(TEXT("role id should be preserved"), Status.RoleId, OsvayderUERoleRegistry::VerifierRoleId());
	TestEqual(TEXT("schema version should be preserved"), Status.EvidenceSchemaVersion, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_CloseoutFailedIncludesGateReason,
	"OsvayderUE.UserFacingStatus.CloseoutFailedIncludesGateReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_CloseoutFailedIncludesGateReason::RunTest(const FString& Parameters)
{
	const FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(TEXT("failed"), TEXT("not_achieved"), TEXT("compile_failed_after_cpp_mutation"));
	Decision.BlockerFamily = TEXT("compile_proof");
	Decision.BlockerDetail = TEXT("UBT failed after the latest C++ mutation");

	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision);

	TestEqual(TEXT("deterministic closeout failure should map to closeout_failed"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::CloseoutFailedStatusId());
	TestTrue(TEXT("detail should include gate reason"),
		Status.Detail.Contains(TEXT("compile_failed_after_cpp_mutation")));
	TestTrue(TEXT("detail should include blocker detail"),
		Status.Detail.Contains(TEXT("UBT failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_ManualVerificationRequired,
	"OsvayderUE.UserFacingStatus.ManualVerificationRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_ManualVerificationRequired::RunTest(const FString& Parameters)
{
	const FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(TEXT("failed"), TEXT("not_achieved"), TEXT("manual_ui_boundary"));
	Decision.BlockerFamily = TEXT("manual_verification_required");
	Decision.BlockerDetail = TEXT("Open the editor UI and verify the final rendered status once.");

	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision);

	TestEqual(TEXT("manual UI boundary should map to manual_verification_required"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::ManualVerificationRequiredStatusId());
	TestTrue(TEXT("manual status should include exact manual action"),
		Status.Detail.Contains(TEXT("Open the editor UI")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_PartialManualRuntimeVisualProofRequired,
	"OsvayderUE.UserFacingStatus.PartialManualRuntimeVisualProofRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_PartialManualRuntimeVisualProofRequired::RunTest(const FString& Parameters)
{
	const FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(
			TEXT("done"),
			TEXT("achieved_partially"),
			TEXT("gameplay_runtime_visual_proof_manual_required"));
	Decision.BlockerFamily = TEXT("manual_verification_required");
	Decision.BlockerDetail = TEXT("PIE/runtime/visual gameplay verification remains manual.");

	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision);

	TestEqual(TEXT("partial manual proof gap should map to manual_verification_required"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::ManualVerificationRequiredStatusId());
	TestNotEqual(TEXT("partial manual proof gap must not map to closeout_failed"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::CloseoutFailedStatusId());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_ManualAssetDependencyBlocker,
	"OsvayderUE.UserFacingStatus.ManualAssetDependencyBlocker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_ManualAssetDependencyBlocker::RunTest(const FString& Parameters)
{
	const FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(TEXT("failed"), TEXT("not_achieved"), TEXT("manual_asset_dependency_blocker"));
	Decision.BlockerFamily = TEXT("manual_asset_dependency_blocker");
	Decision.BlockerDetail = TEXT("Requested Fab/Marketplace asset is not available locally.");

	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision);

	TestEqual(TEXT("external asset blocker should map to manual_verification_required"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::ManualVerificationRequiredStatusId());
	TestTrue(TEXT("manual asset blocker detail should be preserved"),
		Status.Detail.Contains(TEXT("Fab/Marketplace")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_BlockedByExactCauseStopLoss,
	"OsvayderUE.UserFacingStatus.BlockedByExactCauseStopLoss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_BlockedByExactCauseStopLoss::RunTest(const FString& Parameters)
{
	const FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(TEXT("failed"), TEXT("not_achieved"), TEXT("feature_workflow_stop_loss_triggered"));
	Decision.bStopLossTriggered = true;
	Decision.StopLossReason = TEXT("command_execution_without_phase_advance_gt_5");
	Decision.BlockerFamily = TEXT("proof_prerequisites_missing");
	Decision.BlockerDetail = TEXT("proof map and runtime actors were not observed");

	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision);

	TestEqual(TEXT("stop-loss/proof-prerequisite blocker should map to blocked_by_exact_cause"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::BlockedByExactCauseStatusId());
	TestTrue(TEXT("exact blocker should include stop-loss reason"),
		Status.Detail.Contains(TEXT("command_execution_without_phase_advance_gt_5")));
	TestTrue(TEXT("exact blocker should include blocker detail"),
		Status.Detail.Contains(TEXT("proof map")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_BlockedOnToolSurface,
	"OsvayderUE.UserFacingStatus.BlockedOnToolSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_MechanicInputConflictUnresolved,
	"OsvayderUE.UserFacingStatus.MechanicInputConflictUnresolved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_BlockedOnToolSurface::RunTest(const FString& Parameters)
{
	const FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(TEXT("failed"), TEXT("not_achieved"), TEXT("blocked_on_tool_surface"));
	Decision.BlockerFamily = TEXT("blocked_on_tool_surface");
	Decision.BlockerDetail = TEXT("missing tools: map_runtime_proof, capture_viewport");

	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision);

	TestEqual(TEXT("missing runtime proof tools should map to blocked_by_exact_cause"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::BlockedByExactCauseStatusId());
	TestTrue(TEXT("tool surface detail should list exact missing tools"),
		Status.Detail.Contains(TEXT("map_runtime_proof")) && Status.Detail.Contains(TEXT("capture_viewport")));
	return true;
}

bool FUserFacingStatus_MechanicInputConflictUnresolved::RunTest(const FString& Parameters)
{
	const FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(TEXT("failed"), TEXT("not_achieved"), TEXT("mechanic_input_conflict_unresolved"));
	Decision.BlockerFamily = TEXT("mechanic_input_conflict_unresolved");
	Decision.BlockerDetail = TEXT("Space+Shift conflicts with existing flight and sprint/wallrun input owners.");

	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan, Decision);

	TestEqual(TEXT("mechanic input conflict should map to blocked_by_exact_cause"),
		Status.StatusId,
		OsvayderUEUserFacingStatus::BlockedByExactCauseStatusId());
	TestTrue(TEXT("mechanic conflict detail should preserve conflicting controls"),
		Status.Detail.Contains(TEXT("Space+Shift")) && Status.Detail.Contains(TEXT("flight")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_HeadlineIsNotRawInternalPhase,
	"OsvayderUE.UserFacingStatus.HeadlineIsNotRawInternalPhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_HeadlineIsNotRawInternalPhase::RunTest(const FString& Parameters)
{
	FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	Plan.FeatureWorkflow.CurrentPhase = TEXT("memory_update");
	Plan.FeatureWorkflow.RuntimeProofState = TEXT("passed");

	const FOsvayderUEUserFacingStatus Status = OsvayderUEUserFacingStatus::BuildStatus(Plan);

	TestFalse(TEXT("headline must not be raw current phase"),
		OsvayderUEUserFacingStatus::IsRawInternalPhaseLabel(Status.Headline));
	TestFalse(TEXT("headline should not expose memory_update directly"),
		Status.Headline.Equals(TEXT("memory_update"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUserFacingStatus_DecisionJsonSerializesStatus,
	"OsvayderUE.UserFacingStatus.DecisionJsonSerializesStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUserFacingStatus_DecisionJsonSerializesStatus::RunTest(const FString& Parameters)
{
	const FOsvayderUEActivePlan Plan = MakeUserFacingStatusPlan();
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(TEXT("done"), TEXT("achieved_fully"));
	Decision.bRuntimeProofPassed = true;
	FOsvayderUERunCloseoutContext Context;
	Context.RunId = TEXT("run_658_user_status_context");
	Context.PlanId = Plan.PlanId;
	Context.FeatureWorkflowId = Plan.FeatureWorkflow.FeatureWorkflowId;
	Context.RecipeId = Plan.FeatureWorkflow.RecipeId;
	Context.RoleId = OsvayderUERoleRegistry::VerifierRoleId();
	Context.EvidenceSchemaVersion = 1;

	const TSharedPtr<FJsonObject> Json = SOsvayderEditorWidget::BuildCloseoutDecisionJson(Plan, Decision, Context);
	const TSharedPtr<FJsonObject>* UserStatusJson = nullptr;
	TestTrue(TEXT("decision json should carry user_facing_status"),
		Json->TryGetObjectField(TEXT("user_facing_status"), UserStatusJson) && UserStatusJson && UserStatusJson->IsValid());

	FString StatusId;
	(*UserStatusJson)->TryGetStringField(TEXT("status_id"), StatusId);
	TestEqual(TEXT("json status id should be closeout_passed"),
		StatusId,
		OsvayderUEUserFacingStatus::CloseoutPassedStatusId());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidget_StatusWarningHeadlineIsUserFacing,
	"OsvayderUE.Widget.Status.UserFacingWarningHeadline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidget_StatusWarningHeadlineIsUserFacing::RunTest(const FString& Parameters)
{
	FOsvayderUEActivePlanCloseoutDecision Decision =
		MakeCloseoutDecision(TEXT("failed"), TEXT("not_achieved"), TEXT("missing_compile_truth_after_cpp_mutation"));

	const FString Warning = SOsvayderEditorWidget::BuildActivePlanCloseoutWarningText(
		Decision,
		TEXT("Status: full"));

	TestTrue(TEXT("warning should start with simplified status headline"),
		Warning.StartsWith(TEXT("[WARN] Closeout failed")));
	TestTrue(TEXT("warning should still include exact gate reason"),
		Warning.Contains(TEXT("missing_compile_truth_after_cpp_mutation")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
