// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "ClaudeEditorWidget.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FUnrealClaudeActivePlanCloseoutDecision MakePassingHeadlessCloseoutDecision(const bool bRuntimeProofRequired = true)
	{
		FUnrealClaudeActivePlanCloseoutDecision Decision;
		Decision.PlanStatus = TEXT("done");
		Decision.ResultStatus = TEXT("achieved_fully");
		Decision.SourceRunId = TEXT("run_packet665");
		Decision.SourcePlanId = TEXT("plan_packet665");
		Decision.SourceFeatureWorkflowId = TEXT("feature_packet665");
		Decision.SourceRecipeId = bRuntimeProofRequired
			? TEXT("feature.interaction_access_slice_v1")
			: TEXT("feature.documentation_trace_v1");
		Decision.SourceRoleId = TEXT("worker");
		Decision.SourceArchivePath = TEXT("D:/Saved/UnrealClaude/PlanArchives/packet665.active_plan.json");
		Decision.bRuntimeProofRequired = bRuntimeProofRequired;
		Decision.bRuntimeProofPassed = bRuntimeProofRequired;
		return Decision;
	}

	FUnrealClaudeHeadlessAcceptanceReceiptContext MakeReceiptContext(const bool bRuntimeProofRequired = true)
	{
		FUnrealClaudeHeadlessAcceptanceReceiptContext Context;
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
		Context.LatestCloseoutPath = TEXT("D:/Saved/UnrealClaude/closeout_decision.json");
		Context.ArchivePath = Context.CloseoutDecision.SourceArchivePath;
		Context.VisibleSessionPath = TEXT("D:/Saved/UnrealClaude/visible_session_codex_cli.json");
		Context.TracePath = TEXT("D:/Saved/UnrealClaude/agent_trace.jsonl");
		return Context;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_RequestValidation,
	"UnrealClaude.HeadlessAcceptance.RequestValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_RequestValidation::RunTest(const FString& /*Parameters*/)
{
	const FString TestDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Headless Acceptance"));
	IFileManager::Get().MakeDirectory(*TestDir, true);
	const FString PromptPath = FPaths::Combine(TestDir, TEXT("packet665 prompt.txt"));
	const FString MissingPromptPath = FPaths::Combine(TestDir, TEXT("missing_prompt.txt"));
	FFileHelper::SaveStringToFile(TEXT("headless acceptance prompt"), *PromptPath);

	FUnrealClaudeHeadlessAcceptanceRequest Request;
	Request.PromptFile = MissingPromptPath;
	Request.Prefix = TEXT("packet665_headless_new_session_v1");
	Request.TimeoutSec = 30;

	FString Error;
	TestFalse(
		TEXT("Headless bridge must require explicit local/dev opt-in"),
		SClaudeEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	TestEqual(TEXT("Missing opt-in error"), Error, FString(TEXT("local_dev_opt_in_required")));

	Request.bLocalDevOptIn = true;
	TestFalse(
		TEXT("Missing prompt file must fail cleanly"),
		SClaudeEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	TestTrue(TEXT("Missing prompt error should name the failure"), Error.Contains(TEXT("prompt_file_not_found")));

	Request.PromptFile = PromptPath;
	Request.TimeoutSec = 0;
	TestFalse(
		TEXT("Timeout must be explicit and positive"),
		SClaudeEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	TestEqual(TEXT("Timeout error"), Error, FString(TEXT("timeout_sec_must_be_positive")));

	Request.TimeoutSec = 30;
	TestTrue(
		TEXT("Valid local/dev request should pass validation"),
		SClaudeEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));

	const FString QuotedPromptPath = FString::Printf(TEXT("\"%s\""), *PromptPath);
	const FString SingleQuotedPromptPath = FString::Printf(TEXT("'%s'"), *PromptPath);
	const FString QuotedOutputDir = FString::Printf(TEXT("\"%s\""), *FPaths::Combine(TestDir, TEXT("Receipts With Spaces")));
	TestEqual(
		TEXT("Double-quoted prompt path normalizes to the real path"),
		SClaudeEditorWidget::NormalizeHeadlessAcceptancePathArgument(QuotedPromptPath),
		SClaudeEditorWidget::NormalizeHeadlessAcceptancePathArgument(PromptPath));
	TestEqual(
		TEXT("Single-quoted prompt path normalizes to the real path"),
		SClaudeEditorWidget::NormalizeHeadlessAcceptancePathArgument(SingleQuotedPromptPath),
		SClaudeEditorWidget::NormalizeHeadlessAcceptancePathArgument(PromptPath));

	Request.PromptFile = QuotedPromptPath;
	Request.OutputDir = QuotedOutputDir;
	Request.TimeoutSec = 30;
	TestTrue(
		TEXT("Quoted prompt file should not become prompt_file_not_found"),
		SClaudeEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	const FString ReceiptPath = SClaudeEditorWidget::ResolveHeadlessAcceptanceReceiptPath(Request);
	TestFalse(TEXT("Quoted output dir should not leak quote characters into receipt path"), ReceiptPath.Contains(TEXT("\"")));
	TestTrue(TEXT("Receipt path should preserve spaces in output dir"), ReceiptPath.Contains(TEXT("Receipts With Spaces")));

	Request.PromptFile = FString::Printf(TEXT("\"%s\""), *MissingPromptPath);
	TestFalse(
		TEXT("Quoted missing prompt must still fail truthfully"),
		SClaudeEditorWidget::ValidateHeadlessAcceptanceRequest(Request, Error));
	TestTrue(TEXT("Quoted missing prompt still reports prompt_file_not_found"), Error.Contains(TEXT("prompt_file_not_found")));
	TestFalse(TEXT("Missing prompt error should report normalized path"), Error.Contains(TEXT("\"")));

	IFileManager::Get().Delete(*PromptPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_ReceiptClassifiesRuntimeAndNonRuntimePrefixEvidence,
	"UnrealClaude.HeadlessAcceptance.ReceiptClassifiesRuntimeAndNonRuntimePrefixEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_ReceiptClassifiesRuntimeAndNonRuntimePrefixEvidence::RunTest(const FString& /*Parameters*/)
{
	FUnrealClaudeHeadlessAcceptanceReceiptContext Context = MakeReceiptContext();
	TSharedPtr<FJsonObject> ReceiptWithoutLogs = SClaudeEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	FString Status;
	FString FailureReason;
	ReceiptWithoutLogs->TryGetStringField(TEXT("status"), Status);
	ReceiptWithoutLogs->TryGetStringField(TEXT("failure_reason"), FailureReason);
	TestEqual(TEXT("No current-prefix artifacts must reject acceptance"), Status, FString(TEXT("closeout_failed")));
	TestEqual(TEXT("No current-prefix artifacts reason"), FailureReason, FString(TEXT("no_current_prefix_artifacts")));

	Context.LogPaths.Add(TEXT("D:/Saved/Logs/packet664_wrong_prefix_automation_20260428_120000.log"));
	TSharedPtr<FJsonObject> WrongPrefixReceipt = SClaudeEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	WrongPrefixReceipt->TryGetStringField(TEXT("status"), Status);
	TestEqual(TEXT("Wrong-prefix log artifacts must reject runtime acceptance"), Status, FString(TEXT("closeout_failed")));

	Context.LogPaths.Reset();
	Context.LogPaths.Add(TEXT("D:/Saved/Logs/packet665_headless_new_session_v1_automation_20260428_120000.log"));
	TSharedPtr<FJsonObject> PassingReceipt = SClaudeEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
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

	FUnrealClaudeHeadlessAcceptanceReceiptContext NonRuntimeContext = MakeReceiptContext(false);
	TSharedPtr<FJsonObject> NonRuntimeReceipt = SClaudeEditorWidget::BuildHeadlessAcceptanceReceiptJson(NonRuntimeContext);
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
	TSharedPtr<FJsonObject> NonRuntimeNoWorkflowReceipt = SClaudeEditorWidget::BuildHeadlessAcceptanceReceiptJson(NonRuntimeContext);
	NonRuntimeNoWorkflowReceipt->TryGetStringField(TEXT("status"), Status);
	TestEqual(TEXT("Non-runtime closeout can pass without feature workflow id"), Status, FString(TEXT("closeout_passed")));

	Context.CloseoutDecision.bManagedStateManualWriteDetected = true;
	TSharedPtr<FJsonObject> ManualWriteReceipt = SClaudeEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
	ManualWriteReceipt->TryGetStringField(TEXT("status"), Status);
	TestEqual(TEXT("Manual managed-state writes cannot be accepted"), Status, FString(TEXT("closeout_failed")));
	bool bManualWriteDetected = false;
	ManualWriteReceipt->TryGetBoolField(TEXT("managed_state_manual_write_detected"), bManualWriteDetected);
	TestTrue(TEXT("Manual managed-state write remains visible in receipt"), bManualWriteDetected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_PromptHashAndReceiptPathAreStable,
	"UnrealClaude.HeadlessAcceptance.PromptHashAndReceiptPathAreStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_PromptHashAndReceiptPathAreStable::RunTest(const FString& /*Parameters*/)
{
	const FString Prompt = TEXT("packet665 prompt\nUTF8 check");
	const FString HashA = SClaudeEditorWidget::ComputeHeadlessAcceptancePromptHash(Prompt);
	const FString HashB = SClaudeEditorWidget::ComputeHeadlessAcceptancePromptHash(Prompt);
	TestEqual(TEXT("Prompt hash must be stable"), HashA, HashB);
	TestTrue(TEXT("Prompt hash records algorithm"), HashA.StartsWith(TEXT("md5:")));

	FUnrealClaudeHeadlessAcceptanceRequest Request;
	Request.Prefix = TEXT("packet665 headless/new session");
	Request.OutputDir = TEXT("D:/Receipts");
	const FString ReceiptPath = SClaudeEditorWidget::ResolveHeadlessAcceptanceReceiptPath(Request);
	TestTrue(TEXT("Receipt path should be under requested output dir"), ReceiptPath.Contains(TEXT("D:/Receipts")));
	TestTrue(TEXT("Receipt path should be sanitized and typed"), ReceiptPath.EndsWith(TEXT(".headless_acceptance_receipt.json")));
	TestFalse(TEXT("Receipt filename should not contain spaces"), FPaths::GetCleanFilename(ReceiptPath).Contains(TEXT(" ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeadlessAcceptanceBridge_VisibleManualReceiptMetadata,
	"UnrealClaude.HeadlessAcceptance.VisibleManualReceiptMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_VisibleManualReceiptMetadata::RunTest(const FString& /*Parameters*/)
{
	FUnrealClaudeHeadlessAcceptanceReceiptContext Context = MakeReceiptContext(false);
	Context.Request.bVisibleManualEmulator = true;
	Context.Request.bRequireVisibleEditor = true;
	Context.Request.TriggerPath = TEXT("command_line_visible_manual_acceptance");
	Context.DispatchPath.Reset();
	Context.Status = TEXT("running");
	Context.bAssistantSuccess = false;
	Context.bHasCloseoutDecision = false;

	TSharedPtr<FJsonObject> Receipt = SClaudeEditorWidget::BuildHeadlessAcceptanceReceiptJson(Context);
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
	"UnrealClaude.HeadlessAcceptance.StartReceiptArtifactIsMachineReadable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHeadlessAcceptanceBridge_StartReceiptArtifactIsMachineReadable::RunTest(const FString& /*Parameters*/)
{
	const FString TestDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Headless Acceptance Start"));
	IFileManager::Get().MakeDirectory(*TestDir, true);
	const FString PromptPath = FPaths::Combine(TestDir, TEXT("packet681 prompt.txt"));
	FFileHelper::SaveStringToFile(TEXT("packet681 headless acceptance start"), *PromptPath);

	FUnrealClaudeHeadlessAcceptanceReceiptContext Context;
	Context.Request.PromptFile = SClaudeEditorWidget::NormalizeHeadlessAcceptancePathArgument(
		FString::Printf(TEXT("\"%s\""), *PromptPath));
	Context.Request.Prefix = TEXT("packet681_headless_acceptance_v1");
	Context.Request.TimeoutSec = 120;
	Context.Request.OutputDir = SClaudeEditorWidget::NormalizeHeadlessAcceptancePathArgument(
		FString::Printf(TEXT("\"%s\""), *FPaths::Combine(TestDir, TEXT("Receipts With Spaces"))));
	Context.ReceiptPath = SClaudeEditorWidget::ResolveHeadlessAcceptanceReceiptPath(Context.Request);
	Context.PromptHash = SClaudeEditorWidget::ComputeHeadlessAcceptancePromptHash(TEXT("packet681 headless acceptance start"));
	Context.StartedAtUtc = TEXT("2026-04-30T10:00:00Z");
	Context.DispatchPath = TEXT("console_command_headless_acceptance");
	Context.Status = TEXT("running");

	FString SaveError;
	TestTrue(
		TEXT("Start receipt artifact should be writable"),
		SClaudeEditorWidget::SaveHeadlessAcceptanceReceiptArtifact(Context, SaveError));
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

#endif // WITH_DEV_AUTOMATION_TESTS
