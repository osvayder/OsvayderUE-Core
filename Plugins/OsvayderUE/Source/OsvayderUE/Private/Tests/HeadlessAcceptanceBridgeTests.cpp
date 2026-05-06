// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "OsvayderEditorWidget.h"
#include "OsvayderUEUserFacingStatus.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FOsvayderUEActivePlanCloseoutDecision MakePassingHeadlessCloseoutDecision(const bool bRuntimeProofRequired = true)
	{
		FOsvayderUEActivePlanCloseoutDecision Decision;
		Decision.PlanStatus = TEXT("done");
		Decision.ResultStatus = TEXT("achieved_fully");
		Decision.SourceRunId = TEXT("run_packet665");
		Decision.SourcePlanId = TEXT("plan_packet665");
		Decision.SourceFeatureWorkflowId = TEXT("feature_packet665");
		Decision.SourceRecipeId = bRuntimeProofRequired
			? TEXT("feature.interaction_access_slice_v1")
			: TEXT("feature.documentation_trace_v1");
		Decision.SourceRoleId = TEXT("worker");
		Decision.SourceArchivePath = TEXT("D:/Saved/OsvayderUE/PlanArchives/packet665.active_plan.json");
		Decision.bRuntimeProofRequired = bRuntimeProofRequired;
		Decision.bRuntimeProofPassed = bRuntimeProofRequired;
		return Decision;
	}

	FOsvayderUEHeadlessAcceptanceReceiptContext MakeReceiptContext(const bool bRuntimeProofRequired = true)
	{
		FOsvayderUEHeadlessAcceptanceReceiptContext Context;
		Context.Request.PromptFile = TEXT("D:/prompt.txt");
		Context.Request.Prefix = TEXT("packet665_headless_new_session_v1");
		Context.Request.TimeoutSec = 120;
		Context.ReceiptPath = TEXT("D:/receipt.json");
		Context.PromptHash = TEXT("md5:0123456789abcdef0123456789abcdef");
		Context.StartedAtUtc = TEXT("2026-04-28T12:00:00Z");
		Context.CompletedAtUtc = TEXT("2026-04-28T12:01:00Z");
		Context.DispatchPath = TEXT("widget_new_session_send_message");
		Context.Status = TEXT("completed");
		Context.bAssistantSuccess = true;
		Context.bHasCloseoutDecision = true;
		Context.CloseoutDecision = MakePassingHeadlessCloseoutDecision(bRuntimeProofRequired);
		Context.LatestCloseoutPath = TEXT("D:/Saved/OsvayderUE/closeout_decision.json");
		Context.ArchivePath = Context.CloseoutDecision.SourceArchivePath;
		Context.VisibleSessionPath = TEXT("D:/Saved/OsvayderUE/visible_session_codex_cli.json");
		Context.TracePath = TEXT("D:/Saved/OsvayderUE/agent_trace.jsonl");
		return Context;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_RequestValidation,
	"OsvayderUE.HeadlessAcceptance.RequestValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_RequestValidation::RunTest(const FString& /*Parameters*/)
{
	const FString TestDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Headless Acceptance"));
	IFileManager::Get().MakeDirectory(*TestDir, true);
	const FString PromptPath = FPaths::Combine(TestDir, TEXT("packet665 prompt.txt"));
	const FString MissingPromptPath = FPaths::Combine(TestDir, TEXT("missing_prompt.txt"));
	FFileHelper::SaveStringToFile(TEXT("headless acceptance prompt"), *PromptPath);

	FOsvayderUEHeadlessAcceptanceRequest Request;
	Request.PromptFile = MissingPromptPath;
	Request.Prefix = TEXT("packet665_headless_new_session_v1");
	Request.TimeoutSec = 30;

	FString Error;
	TestFalse(
		TEXT("Headless bridge must require explicit local/dev opt-in"),
		SOsvayderEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	TestEqual(TEXT("Missing opt-in error"), Error, FString(TEXT("local_dev_opt_in_required")));

	Request.bLocalDevOptIn = true;
	TestFalse(
		TEXT("Missing prompt file must fail cleanly"),
		SOsvayderEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	TestTrue(TEXT("Missing prompt error should name the failure"), Error.Contains(TEXT("prompt_file_not_found")));

	Request.PromptFile = PromptPath;
	Request.TimeoutSec = 0;
	TestFalse(
		TEXT("Timeout must be explicit and positive"),
		SOsvayderEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	TestEqual(TEXT("Timeout error"), Error, FString(TEXT("timeout_sec_must_be_positive")));

	Request.TimeoutSec = 30;
	TestTrue(
		TEXT("Valid local/dev request should pass validation"),
		SOsvayderEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));

	const FString QuotedPromptPath = FString::Printf(TEXT("\"%s\""), *PromptPath);
	const FString SingleQuotedPromptPath = FString::Printf(TEXT("'%s'"), *PromptPath);
	const FString QuotedOutputDir = FString::Printf(TEXT("\"%s\""), *FPaths::Combine(TestDir, TEXT("Receipts With Spaces")));
	TestEqual(
		TEXT("Double-quoted prompt path normalizes to the real path"),
		SOsvayderEditorWidget::NormalizeHeadlessAcceptancePathArgument(QuotedPromptPath),
		SOsvayderEditorWidget::NormalizeHeadlessAcceptancePathArgument(PromptPath));
	TestEqual(
		TEXT("Single-quoted prompt path normalizes to the real path"),
		SOsvayderEditorWidget::NormalizeHeadlessAcceptancePathArgument(SingleQuotedPromptPath),
		SOsvayderEditorWidget::NormalizeHeadlessAcceptancePathArgument(PromptPath));

	Request.PromptFile = QuotedPromptPath;
	Request.OutputDir = QuotedOutputDir;
	Request.TimeoutSec = 30;
	TestTrue(
		TEXT("Quoted prompt file should not become prompt_file_not_found"),
		SOsvayderEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	const FString ReceiptPath = SOsvayderEditorWidget::ResolveHeadlessAcceptanceReceiptPath(Request);
	TestFalse(TEXT("Quoted output dir should not leak quote characters into receipt path"), ReceiptPath.Contains(TEXT("\"")));
	TestTrue(TEXT("Receipt path should preserve spaces in output dir"), ReceiptPath.Contains(TEXT("Receipts With Spaces")));

	Request.PromptFile = FString::Printf(TEXT("\"%s\""), *MissingPromptPath);
	TestFalse(
		TEXT("Quoted missing prompt must still fail truthfully"),
		SOsvayderEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	TestTrue(TEXT("Quoted missing prompt still reports prompt_file_not_found"), Error.Contains(TEXT("prompt_file_not_found")));
	TestFalse(TEXT("Missing prompt error should report normalized path"), Error.Contains(TEXT("\"")));

	IFileManager::Get().Delete(*PromptPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_ReceiptClassifiesRuntimeAndNonRuntimePrefixEvidence,
	"OsvayderUE.HeadlessAcceptance.ReceiptClassifiesRuntimeAndNonRuntimePrefixEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_ReceiptClassifiesRuntimeAndNonRuntimePrefixEvidence::RunTest(const FString& /*Parameters*/)
{
	FOsvayderUEHeadlessAcceptanceReceiptContext Context = MakeReceiptContext();
	TSharedPtr<FJsonObject> ReceiptWithoutLogs = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	FString Status;
	FString FailureReason;
	ReceiptWithoutLogs->TryGetStringField(TEXT("status"), Status);
	ReceiptWithoutLogs->TryGetStringField(TEXT("failure_reason"), FailureReason);
	TestEqual(TEXT("No current-prefix artifacts must reject acceptance"), Status, FString(TEXT("closeout_failed")));
	TestEqual(TEXT("No current-prefix artifacts reason"), FailureReason, FString(TEXT("no_current_prefix_artifacts")));

	Context.LogPaths.Add(TEXT("D:/Saved/Logs/packet664_wrong_prefix_automation_20260428_120000.log"));
	TSharedPtr<FJsonObject> WrongPrefixReceipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	WrongPrefixReceipt->TryGetStringField(TEXT("status"), Status);
	TestEqual(TEXT("Wrong-prefix log artifacts must reject runtime acceptance"), Status, FString(TEXT("closeout_failed")));

	Context.LogPaths.Reset();
	Context.LogPaths.Add(TEXT("D:/Saved/Logs/packet665_headless_new_session_v1_automation_20260428_120000.log"));
	TSharedPtr<FJsonObject> PassingReceipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	PassingReceipt->TryGetStringField(TEXT("status"), Status);
	TestEqual(TEXT("Clean closeout plus fresh prefix artifacts should pass"), Status, FString(TEXT("closeout_passed")));

	FString RunId;
	FString PlanId;
	FString WorkflowId;
	FString DispatchPath;
	PassingReceipt->TryGetStringField(TEXT("run_id"), RunId);
	PassingReceipt->TryGetStringField(TEXT("plan_id"), PlanId);
	PassingReceipt->TryGetStringField(TEXT("workflow_id"), WorkflowId);
	PassingReceipt->TryGetStringField(TEXT("dispatch_path"), DispatchPath);
	TestEqual(TEXT("Receipt run id mirrors closeout"), RunId, FString(TEXT("run_packet665")));
	TestEqual(TEXT("Receipt plan id mirrors closeout"), PlanId, FString(TEXT("plan_packet665")));
	TestEqual(TEXT("Receipt workflow id mirrors closeout"), WorkflowId, FString(TEXT("feature_packet665")));
	TestEqual(TEXT("Receipt records widget dispatch path"), DispatchPath, FString(TEXT("widget_new_session_send_message")));

	FOsvayderUEHeadlessAcceptanceReceiptContext NonRuntimeContext = MakeReceiptContext(false);
	TSharedPtr<FJsonObject> NonRuntimeReceipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(NonRuntimeContext);
	bool bFreshArtifactsPresent = false;
	bool bCurrentPrefixLogsPresent = true;
	bool bCloseoutIdentityPresent = false;
	NonRuntimeReceipt->TryGetStringField(TEXT("status"), Status);
	NonRuntimeReceipt->TryGetBoolField(TEXT("fresh_prefix_artifacts_present"), bFreshArtifactsPresent);
	NonRuntimeReceipt->TryGetBoolField(TEXT("current_prefix_log_artifacts_present"), bCurrentPrefixLogsPresent);
	NonRuntimeReceipt->TryGetBoolField(TEXT("closeout_identity_artifacts_present"), bCloseoutIdentityPresent);
	TestEqual(TEXT("Non-runtime closeout can pass on aligned identity artifacts"), Status, FString(TEXT("closeout_passed")));
	TestTrue(TEXT("Non-runtime closeout should expose fresh artifact evidence"), bFreshArtifactsPresent);
	TestFalse(TEXT("Non-runtime closeout should not invent current-prefix logs"), bCurrentPrefixLogsPresent);
	TestTrue(TEXT("Non-runtime closeout should expose closeout identity evidence"), bCloseoutIdentityPresent);

	NonRuntimeContext.CloseoutDecision.SourceFeatureWorkflowId.Reset();
	TSharedPtr<FJsonObject> NonRuntimeNoWorkflowReceipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(NonRuntimeContext);
	NonRuntimeNoWorkflowReceipt->TryGetStringField(TEXT("status"), Status);
	TestEqual(TEXT("Non-runtime closeout can pass without feature workflow id"), Status, FString(TEXT("closeout_passed")));

	FOsvayderUEHeadlessAcceptanceReceiptContext ManualRequiredContext = MakeReceiptContext(false);
	ManualRequiredContext.CloseoutDecision.ResultStatus = TEXT("achieved_partially");
	ManualRequiredContext.CloseoutDecision.GateReasonCode = TEXT("gameplay_runtime_visual_proof_manual_required");
	ManualRequiredContext.CloseoutDecision.BlockerFamily = TEXT("manual_verification_required");
	TSharedPtr<FJsonObject> ManualRequiredReceipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(ManualRequiredContext);
	ManualRequiredReceipt->TryGetStringField(TEXT("status"), Status);
	ManualRequiredReceipt->TryGetStringField(TEXT("failure_reason"), FailureReason);
	TestEqual(
		TEXT("Manual verification required closeout should not be receipt-failed"),
		Status,
		OsvayderUEUserFacingStatus::ManualVerificationRequiredStatusId());
	TestNotEqual(
		TEXT("Manual verification required receipt must not be closeout_failed"),
		Status,
		FString(TEXT("closeout_failed")));
	TestEqual(
		TEXT("Manual verification required receipt should preserve exact gate reason"),
		FailureReason,
		FString(TEXT("gameplay_runtime_visual_proof_manual_required")));

	Context.CloseoutDecision.bManagedStateManualWriteDetected = true;
	TSharedPtr<FJsonObject> ManualWriteReceipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	ManualWriteReceipt->TryGetStringField(TEXT("status"), Status);
	TestEqual(TEXT("Manual managed-state writes cannot be accepted"), Status, FString(TEXT("closeout_failed")));
	bool bManualWriteDetected = false;
	ManualWriteReceipt->TryGetBoolField(TEXT("managed_state_manual_write_detected"), bManualWriteDetected);
	TestTrue(TEXT("Manual managed-state write remains visible in receipt"), bManualWriteDetected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_AnimationWorkflowGateReasonPreventsFalseGreen,
	"OsvayderUE.HeadlessAcceptance.AnimationWorkflowGateReasonPreventsFalseGreen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_AnimationWorkflowGateReasonPreventsFalseGreen::RunTest(const FString& /*Parameters*/)
{
	FOsvayderUEHeadlessAcceptanceReceiptContext Context = MakeReceiptContext(false);
	Context.CloseoutDecision.PlanStatus = TEXT("failed");
	Context.CloseoutDecision.ResultStatus = TEXT("not_achieved");
	Context.CloseoutDecision.GateReasonCode = TEXT("animation_retarget_required_missing");
	Context.CloseoutDecision.BlockerFamily = TEXT("animation_workflow_gate");
	Context.LogPaths = { TEXT("D:/Saved/Logs/packet665_headless_new_session_v1_automation.log") };

	const TSharedPtr<FJsonObject> Receipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	FString Status;
	FString FailureReason;
	Receipt->TryGetStringField(TEXT("status"), Status);
	Receipt->TryGetStringField(TEXT("failure_reason"), FailureReason);

	TestEqual(TEXT("animation workflow closeout gate must not pass headless receipt"), Status, FString(TEXT("closeout_failed")));
	TestEqual(
		TEXT("headless receipt should preserve precise animation gate reason"),
		FailureReason,
		FString(TEXT("animation_retarget_required_missing")));

	FOsvayderUEActivePlan ReplayPlan;
	ReplayPlan.PlanId = TEXT("plan_packet722_correction_false_green_replay");
	ReplayPlan.OriginalUserTask =
		TEXT("Создай wall climb. В папке X:\\PublicExample\\AnimationPacks\\7 -Parkour Animation\\ParkourAnimations\\Animations\\UE5_Skelet посмотри подходящие анимации и примени их.");
	const FOsvayderUECloseoutFactSnapshot ReplayFacts = SOsvayderEditorWidget::ExtractCloseoutFactsFromPlan(ReplayPlan);
	TestTrue(TEXT("replay prompt should require local animation pack intake"), ReplayFacts.bLocalAnimationPackIntakeRequired);
	const FOsvayderUEActivePlanCloseoutDecision ReplayDecision =
		SOsvayderEditorWidget::EvaluateActivePlanCloseoutWithFacts(ReplayPlan, true, ReplayFacts);
	TestEqual(
		TEXT("false-green replay should fail on local animation gate"),
		ReplayDecision.GateReasonCode,
		FString(TEXT("local_animation_pack_intake_required_missing")));

	FOsvayderUEHeadlessAcceptanceReceiptContext ReplayContext = MakeReceiptContext(false);
	ReplayContext.Request.Prefix = TEXT("packet722_correction_false_green_replay");
	ReplayContext.CloseoutDecision = ReplayDecision;
	ReplayContext.LogPaths = { TEXT("D:/Saved/Logs/packet722_correction_false_green_replay.log") };
	const TSharedPtr<FJsonObject> ReplayReceipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(ReplayContext);
	FString ReplayStatus;
	FString ReplayFailureReason;
	ReplayReceipt->TryGetStringField(TEXT("status"), ReplayStatus);
	ReplayReceipt->TryGetStringField(TEXT("failure_reason"), ReplayFailureReason);
	TestEqual(TEXT("false-green replay receipt must fail"), ReplayStatus, FString(TEXT("closeout_failed")));
	TestEqual(
		TEXT("false-green replay receipt must preserve local animation gate"),
		ReplayFailureReason,
		FString(TEXT("local_animation_pack_intake_required_missing")));

	const FString ReplayDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), TEXT("packet722_correction_replay"));
	IFileManager::Get().MakeDirectory(*ReplayDir, true);
	auto SaveJsonArtifact = [](const TSharedPtr<FJsonObject>& JsonObject, const FString& Path) -> bool
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		return FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer)
			&& FFileHelper::SaveStringToFile(JsonText, *Path);
	};
	TestTrue(
		TEXT("false-green replay closeout artifact should be written"),
		SaveJsonArtifact(
			SOsvayderEditorWidget::BuildCloseoutDecisionJson(ReplayPlan, ReplayDecision, ReplayFacts),
			FPaths::Combine(ReplayDir, TEXT("packet722_false_green_replay.closeout_decision.json"))));
	TestTrue(
		TEXT("false-green replay receipt artifact should be written"),
		SaveJsonArtifact(
			ReplayReceipt,
			FPaths::Combine(ReplayDir, TEXT("packet722_false_green_replay.headless_acceptance_receipt.json"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_PromptHashAndReceiptPathAreStable,
	"OsvayderUE.HeadlessAcceptance.PromptHashAndReceiptPathAreStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_PromptHashAndReceiptPathAreStable::RunTest(const FString& /*Parameters*/)
{
	const FString Prompt = TEXT("packet665 prompt\nUTF8 check");
	const FString HashA = SOsvayderEditorWidget::ComputeHeadlessAcceptancePromptHash(Prompt);
	const FString HashB = SOsvayderEditorWidget::ComputeHeadlessAcceptancePromptHash(Prompt);
	TestEqual(TEXT("Prompt hash must be stable"), HashA, HashB);
	TestTrue(TEXT("Prompt hash records algorithm"), HashA.StartsWith(TEXT("md5:")));

	FOsvayderUEHeadlessAcceptanceRequest Request;
	Request.Prefix = TEXT("packet665 headless/new session");
	Request.OutputDir = TEXT("D:/Receipts");
	const FString ReceiptPath = SOsvayderEditorWidget::ResolveHeadlessAcceptanceReceiptPath(Request);
	TestTrue(TEXT("Receipt path should be under requested output dir"), ReceiptPath.Contains(TEXT("D:/Receipts")));
	TestTrue(TEXT("Receipt path should be sanitized and typed"), ReceiptPath.EndsWith(TEXT(".headless_acceptance_receipt.json")));
	TestFalse(TEXT("Receipt filename should not contain spaces"), FPaths::GetCleanFilename(ReceiptPath).Contains(TEXT(" ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_VisibleManualReceiptMetadata,
	"OsvayderUE.HeadlessAcceptance.VisibleManualReceiptMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_VisibleManualReceiptMetadata::RunTest(const FString& /*Parameters*/)
{
	FOsvayderUEHeadlessAcceptanceReceiptContext Context = MakeReceiptContext(false);
	Context.Request.bVisibleManualEmulator = true;
	Context.Request.bRequireVisibleEditor = true;
	Context.Request.TriggerPath = TEXT("command_line_visible_manual_acceptance");
	Context.DispatchPath.Reset();
	Context.Status = TEXT("running");
	Context.bAssistantSuccess = false;
	Context.bHasCloseoutDecision = false;

	TSharedPtr<FJsonObject> Receipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	FString DispatchPath;
	FString UiPath;
	FString EmulatorMode;
	FString VisibilityMode;
	bool bVisibleManual = false;
	bool bVisibleRequired = false;
	Receipt->TryGetStringField(TEXT("dispatch_path"), DispatchPath);
	Receipt->TryGetStringField(TEXT("ui_path"), UiPath);
	Receipt->TryGetStringField(TEXT("emulator_mode"), EmulatorMode);
	Receipt->TryGetStringField(TEXT("process_visibility_mode"), VisibilityMode);
	Receipt->TryGetBoolField(TEXT("visible_manual_emulator"), bVisibleManual);
	Receipt->TryGetBoolField(TEXT("visible_unreal_required"), bVisibleRequired);

	TestEqual(TEXT("Visible manual receipt preserves startup trigger path"), DispatchPath, FString(TEXT("command_line_visible_manual_acceptance")));
	TestEqual(TEXT("Visible manual receipt records widget UI path"), UiPath, FString(TEXT("widget_new_session_send_message")));
	TestEqual(TEXT("Visible manual receipt records emulator mode"), EmulatorMode, FString(TEXT("visible_manual")));
	TestEqual(TEXT("Visible manual receipt records process visibility mode"), VisibilityMode, FString(TEXT("visible_editor_required")));
	TestTrue(TEXT("Visible manual receipt exposes visible_manual_emulator flag"), bVisibleManual);
	TestTrue(TEXT("Visible manual receipt exposes visible_unreal_required flag"), bVisibleRequired);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_StartReceiptArtifactIsMachineReadable,
	"OsvayderUE.HeadlessAcceptance.StartReceiptArtifactIsMachineReadable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_StartReceiptArtifactIsMachineReadable::RunTest(const FString& /*Parameters*/)
{
	const FString TestDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Headless Acceptance Start"));
	IFileManager::Get().MakeDirectory(*TestDir, true);
	const FString PromptPath = FPaths::Combine(TestDir, TEXT("packet681 prompt.txt"));
	FFileHelper::SaveStringToFile(TEXT("packet681 headless acceptance start"), *PromptPath);

	FOsvayderUEHeadlessAcceptanceReceiptContext Context;
	Context.Request.PromptFile = SOsvayderEditorWidget::NormalizeHeadlessAcceptancePathArgument(
		FString::Printf(TEXT("\"%s\""), *PromptPath));
	Context.Request.Prefix = TEXT("packet681_headless_acceptance_v1");
	Context.Request.TimeoutSec = 120;
	Context.Request.OutputDir = SOsvayderEditorWidget::NormalizeHeadlessAcceptancePathArgument(
		FString::Printf(TEXT("\"%s\""), *FPaths::Combine(TestDir, TEXT("Receipts With Spaces"))));
	Context.ReceiptPath = SOsvayderEditorWidget::ResolveHeadlessAcceptanceReceiptPath(Context.Request);
	Context.PromptHash = SOsvayderEditorWidget::ComputeHeadlessAcceptancePromptHash(TEXT("packet681 headless acceptance start"));
	Context.StartedAtUtc = TEXT("2026-04-30T10:00:00Z");
	Context.DispatchPath = TEXT("console_command_headless_acceptance");
	Context.Status = TEXT("running");

	FString SaveError;
	TestTrue(
		TEXT("Start receipt artifact should be writable"),
		SOsvayderEditorWidget::SaveHeadlessAcceptanceReceiptArtifact(Context, SaveError));
	TestTrue(TEXT("Start receipt path should exist"), FPaths::FileExists(Context.ReceiptPath));

	FString ReceiptText;
	TestTrue(TEXT("Start receipt should be readable"), FFileHelper::LoadFileToString(ReceiptText, *Context.ReceiptPath));
	TSharedPtr<FJsonObject> ReceiptJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReceiptText);
	TestTrue(TEXT("Start receipt should parse as JSON"), FJsonSerializer::Deserialize(Reader, ReceiptJson) && ReceiptJson.IsValid());
	if (!ReceiptJson.IsValid())
	{
		return false;
	}

	FString Status;
	FString PromptFile;
	FString OutputDir;
	FString DispatchPath;
	ReceiptJson->TryGetStringField(TEXT("status"), Status);
	ReceiptJson->TryGetStringField(TEXT("prompt_file"), PromptFile);
	ReceiptJson->TryGetStringField(TEXT("output_dir"), OutputDir);
	ReceiptJson->TryGetStringField(TEXT("dispatch_path"), DispatchPath);
	TestEqual(TEXT("Start receipt records running status"), Status, FString(TEXT("running")));
	TestEqual(TEXT("Start receipt records normalized prompt path"), PromptFile, Context.Request.PromptFile);
	TestEqual(TEXT("Start receipt records normalized output dir"), OutputDir, Context.Request.OutputDir);
	TestEqual(TEXT("Start receipt records non-modal dispatch path"), DispatchPath, FString(TEXT("console_command_headless_acceptance")));
	TestFalse(TEXT("Start receipt prompt path should not contain quote characters"), PromptFile.Contains(TEXT("\"")));
	TestFalse(TEXT("Start receipt output dir should not contain quote characters"), OutputDir.Contains(TEXT("\"")));

	IFileManager::Get().Delete(*Context.ReceiptPath);
	IFileManager::Get().Delete(*PromptPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_RecoverableRestartSurvivalDoesNotTerminalFail,
	"OsvayderUE.HeadlessAcceptance.RecoverableRestartSurvivalDoesNotTerminalFail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_RecoverableRestartSurvivalDoesNotTerminalFail::RunTest(const FString& /*Parameters*/)
{
	FOsvayderUEHeadlessAcceptanceReceiptContext Context = MakeReceiptContext(false);
	Context.Status = TEXT("restart_survival_recovering");
	Context.FailureReason = TEXT("restart_survival_recovery_pending");
	Context.bAssistantSuccess = false;
	Context.bHasCloseoutDecision = false;
	Context.CloseoutDecision = FOsvayderUEActivePlanCloseoutDecision{};
	Context.LogPaths.Reset();

	const TSharedPtr<FJsonObject> Receipt = SOsvayderEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	FString Status;
	FString FailureReason;
	bool bHasCloseoutDecision = true;
	Receipt->TryGetStringField(TEXT("status"), Status);
	Receipt->TryGetStringField(TEXT("failure_reason"), FailureReason);
	Receipt->TryGetBoolField(TEXT("has_closeout_decision"), bHasCloseoutDecision);

	TestEqual(
		TEXT("recoverable restart-survival must preserve non-terminal status"),
		Status,
		FString(TEXT("restart_survival_recovering")));
	TestNotEqual(
		TEXT("recoverable restart-survival must not be terminal closeout_failed"),
		Status,
		FString(TEXT("closeout_failed")));
	TestEqual(
		TEXT("recoverable restart-survival should name pending recovery"),
		FailureReason,
		FString(TEXT("restart_survival_recovery_pending")));
	TestFalse(
		TEXT("recoverable restart-survival receipt must not invent closeout proof"),
		bHasCloseoutDecision);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
