// Copyright Natali Caggiano. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "OsvayderEditorWidget.h"
#include "CodexCliRunner.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUEExecutionLog.h"

namespace BackendTransportRecoveryTests
{
	FAgentBackendStatus MakeReadyCodexStatus()
	{
		FAgentBackendStatus Status;
		Status.Backend = EOsvayderUEProviderBackend::CodexCli;
		Status.DisplayName = TEXT("Codex CLI");
		Status.bAvailable = true;
		Status.bReady = true;
		Status.Readiness = EAgentBackendReadiness::Ready;
		Status.AuthState = EAgentBackendAuthState::Authenticated;
		Status.Detail = TEXT("ready");
		Status.AuthDetail = TEXT("authenticated");
		return Status;
	}

	FAgentRequestConfig MakePersistentTransportConfig()
	{
		FAgentRequestConfig Config;
		Config.ExecutionTransportLabel = TEXT("persistent_app_server");
		Config.ExecutionControlProfileId = TEXT("workspace_write_default_runtime_v1");
		Config.Prompt = TEXT("transport recovery verification");
		return Config;
	}

	TSharedPtr<FJsonObject> FindRecordByEventType(
		const TArray<TSharedPtr<FJsonObject>>& Records,
		const FString& EventType)
	{
		for (const TSharedPtr<FJsonObject>& Record : Records)
		{
			if (!Record.IsValid())
			{
				continue;
			}

			FString RecordEventType;
			if (Record->TryGetStringField(TEXT("event_type"), RecordEventType)
				&& RecordEventType.Equals(EventType, ESearchCase::CaseSensitive))
			{
				return Record;
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> GetPayloadObject(const TSharedPtr<FJsonObject>& Record)
	{
		if (!Record.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (Record->TryGetObjectField(TEXT("payload"), Payload) && Payload != nullptr)
		{
			return *Payload;
		}

		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBackendTransportRecovery_CodexClassifierTruth,
	"OsvayderUE.CodexCliRunner.PersistentTransport.ClassifierTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackendTransportRecovery_CodexClassifierTruth::RunTest(const FString& Parameters)
{
	const FString TimeoutMessage =
		TEXT("Timed out waiting for Codex persistent app-server response. Persistent transport was reset; the next request will start a fresh app-server session.");
	const FString WebsocketMessage =
		TEXT("Codex persistent app-server error: websocket connect failed against wss://chatgpt.com/backend-api/codex/responses (os error 10061). Persistent transport was reset; the next request will start a fresh app-server session.");
	const FString ExitMessage = TEXT("Codex persistent app-server exited unexpectedly.");

	TestEqual(
		TEXT("timeout messages should classify as transport timeout"),
		FCodexCliRunner::ClassifyPersistentTransportFailureMessage(TimeoutMessage),
		FString(TEXT("persistent_transport_timeout")));
	TestEqual(
		TEXT("websocket reset messages should classify as websocket transport reset"),
		FCodexCliRunner::ClassifyPersistentTransportFailureMessage(WebsocketMessage),
		FString(TEXT("persistent_transport_websocket_reset")));
	TestEqual(
		TEXT("unexpected app-server exit should classify as process exit"),
		FCodexCliRunner::ClassifyPersistentTransportFailureMessage(ExitMessage),
		FString(TEXT("persistent_transport_process_exit")));
	TestTrue(
		TEXT("transport timeout should be recognized as transport failure"),
		FCodexCliRunner::IsPersistentTransportFailureMessage(TimeoutMessage));
	TestFalse(
		TEXT("ordinary tool error must not classify as transport failure"),
		FCodexCliRunner::IsPersistentTransportFailureMessage(TEXT("Tool failed: compile proof mismatch")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBackendTransportRecovery_TraceSafeRetryTruth,
	"OsvayderUE.AgentTrace.TransportRecovery.SafeRetryTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackendTransportRecovery_TraceSafeRetryTruth::RunTest(const FString& Parameters)
{
	using namespace BackendTransportRecoveryTests;

	FOsvayderUEExecutionLog::Get().Clear();

	const FAgentBackendStatus Status = MakeReadyCodexStatus();
	const FAgentRequestConfig Config = MakePersistentTransportConfig();
	const FString TimeoutMessage =
		TEXT("Timed out waiting for Codex persistent app-server response. Persistent transport was reset; the next request will start a fresh app-server session.");

	const FString RunId = FOsvayderUEAgentTraceLog::Get().BeginRun(Status, Config, Config.Prompt, true, true);
	FOsvayderUEAgentTraceLog::Get().LogBackendFailure(RunId, Status.Backend, TimeoutMessage, TEXT("backend_completion"));
	FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, TimeoutMessage, false);

	FAgentTransportFailureSummary Summary;
	TestTrue(
		TEXT("latest transport failure summary should be available for Codex"),
		FOsvayderUEAgentTraceLog::Get().TryGetLatestTransportFailureSummary(Status.Backend, Summary));
	TestEqual(TEXT("transport summary should resolve the same run id"), Summary.RunId, RunId);
	TestTrue(TEXT("safe transport failure should be retry-safe when no tools ran"), Summary.bRetrySafe);

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 20;
	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records =
		FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);

	const TSharedPtr<FJsonObject> TransportDetectedRecord =
		FindRecordByEventType(Records, TEXT("transport_reset_detected"));
	TestTrue(TEXT("transport reset event should be emitted"), TransportDetectedRecord.IsValid());

	const TSharedPtr<FJsonObject> RetryEligibilityRecord =
		FindRecordByEventType(Records, TEXT("transport_retry_eligibility"));
	TestTrue(TEXT("retry eligibility event should be emitted"), RetryEligibilityRecord.IsValid());
	if (const TSharedPtr<FJsonObject> Payload = GetPayloadObject(RetryEligibilityRecord))
	{
		bool bRetrySafe = false;
		FString CurrentLane;
		FString RecoveryKind;
		bool bDetachedEscalation = true;
		TestTrue(TEXT("retry eligibility payload should expose retry_safe"), Payload->TryGetBoolField(TEXT("retry_safe"), bRetrySafe));
		TestTrue(TEXT("retry eligibility should be safe when no tools ran"), bRetrySafe);
		TestTrue(TEXT("retry eligibility should expose current task lane"), Payload->TryGetStringField(TEXT("current_task_lane"), CurrentLane));
		TestEqual(TEXT("transport retry should stay in the live editor lane"), CurrentLane, FString(TEXT("live_editor")));
		TestTrue(TEXT("retry eligibility should expose recovery kind"), Payload->TryGetStringField(TEXT("recovery_kind"), RecoveryKind));
		TestEqual(TEXT("transport retry should be tagged as same-lane recovery"), RecoveryKind, FString(TEXT("same_lane_recovery")));
		TestTrue(TEXT("retry eligibility should expose detached escalation guard"), Payload->TryGetBoolField(TEXT("detached_lane_escalation"), bDetachedEscalation));
		TestFalse(TEXT("transport retry must not masquerade as detached escalation"), bDetachedEscalation);
	}

	const TSharedPtr<FJsonObject> RunCompletedRecord =
		FindRecordByEventType(Records, TEXT("run_completed"));
	TestTrue(TEXT("run completed event should be present"), RunCompletedRecord.IsValid());
	if (const TSharedPtr<FJsonObject> Payload = GetPayloadObject(RunCompletedRecord))
	{
		FString FailureFamily;
		FString CurrentLane;
		FString RecoveryKind;
		bool bRetrySafe = false;
		bool bDetachedEscalation = true;
		TestTrue(TEXT("run_completed payload should include transport failure family"), Payload->TryGetStringField(TEXT("transport_failure_family"), FailureFamily));
		TestEqual(TEXT("run_completed transport family should match timeout classification"), FailureFamily, FString(TEXT("persistent_transport_timeout")));
		TestTrue(TEXT("run_completed payload should include retry-safe truth"), Payload->TryGetBoolField(TEXT("transport_retry_safe"), bRetrySafe));
		TestTrue(TEXT("run_completed should report retry-safe for transport-only failure"), bRetrySafe);
		TestTrue(TEXT("run_completed should expose current task lane"), Payload->TryGetStringField(TEXT("current_task_lane"), CurrentLane));
		TestEqual(TEXT("run_completed should stay in live_editor lane"), CurrentLane, FString(TEXT("live_editor")));
		TestTrue(TEXT("run_completed should expose same-lane recovery kind"), Payload->TryGetStringField(TEXT("recovery_kind"), RecoveryKind));
		TestEqual(TEXT("run_completed recovery kind should be same-lane"), RecoveryKind, FString(TEXT("same_lane_recovery")));
		TestTrue(TEXT("run_completed should expose detached escalation guard"), Payload->TryGetBoolField(TEXT("detached_lane_escalation"), bDetachedEscalation));
		TestFalse(TEXT("run_completed transport recovery must not mark detached escalation"), bDetachedEscalation);
	}

	const TArray<FExecutionReceipt> Receipts = FOsvayderUEExecutionLog::Get().GetRecent(4);
	TestTrue(TEXT("execution log should capture the backend run receipt"), Receipts.Num() > 0);
	if (Receipts.Num() > 0)
	{
		const FExecutionReceipt& Receipt = Receipts.Last();
		TestEqual(TEXT("receipt should classify the transport failure family"), Receipt.TransportFailureFamily, FString(TEXT("persistent_transport_timeout")));
		TestTrue(TEXT("receipt should mark the transport failure retry-safe"), Receipt.bTransportRetrySafe);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBackendTransportRecovery_TraceUnsafeRetryTruth,
	"OsvayderUE.AgentTrace.TransportRecovery.UnsafeRetryTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackendTransportRecovery_TraceUnsafeRetryTruth::RunTest(const FString& Parameters)
{
	using namespace BackendTransportRecoveryTests;

	const FAgentBackendStatus Status = MakeReadyCodexStatus();
	const FAgentRequestConfig Config = MakePersistentTransportConfig();
	const FString TransportMessage =
		TEXT("Codex persistent app-server error: websocket connect failed against wss://chatgpt.com/backend-api/codex/responses (os error 10061). Persistent transport was reset; the next request will start a fresh app-server session.");

	const FString RunId = FOsvayderUEAgentTraceLog::Get().BeginRun(Status, Config, Config.Prompt, true, true);

	FAgentRunEvent ToolUse;
	ToolUse.Type = EAgentRunEventType::ToolUse;
	ToolUse.Backend = Status.Backend;
	ToolUse.ToolName = TEXT("Read");
	ToolUse.ToolInput = TEXT("{\"path\":\"Plugins/OsvayderUE/Source/OsvayderUE/Private/CodexCliRunner.cpp\"}");
	ToolUse.ToolCallId = TEXT("tool_001");
	FOsvayderUEAgentTraceLog::Get().AppendObservedEvent(RunId, ToolUse);

	FOsvayderUEAgentTraceLog::Get().LogBackendFailure(RunId, Status.Backend, TransportMessage, TEXT("backend_completion"));
	FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, TransportMessage, false);

	FAgentTransportFailureSummary Summary;
	TestTrue(
		TEXT("unsafe transport failure summary should still be queryable"),
		FOsvayderUEAgentTraceLog::Get().TryGetLatestTransportFailureSummary(Status.Backend, Summary));
	TestFalse(TEXT("retry should be unsafe after tool activity"), Summary.bRetrySafe);
	TestEqual(TEXT("unsafe retry should cite tool activity"), Summary.RetryBlockReason, FString(TEXT("tool_activity_observed")));

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 20;
	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records =
		FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);

	const TSharedPtr<FJsonObject> RetryBlockedRecord =
		FindRecordByEventType(Records, TEXT("transport_retry_blocked"));
	TestTrue(TEXT("unsafe transport failure should emit retry-blocked event"), RetryBlockedRecord.IsValid());
	if (const TSharedPtr<FJsonObject> Payload = GetPayloadObject(RetryBlockedRecord))
	{
		FString BlockReason;
		FString CurrentLane;
		FString RecoveryKind;
		bool bDetachedEscalation = true;
		TestTrue(TEXT("retry blocked payload should expose block_reason"), Payload->TryGetStringField(TEXT("block_reason"), BlockReason));
		TestEqual(TEXT("retry blocked event should keep truthful reason"), BlockReason, FString(TEXT("tool_activity_observed")));
		TestTrue(TEXT("retry blocked should expose current task lane"), Payload->TryGetStringField(TEXT("current_task_lane"), CurrentLane));
		TestEqual(TEXT("retry blocked should stay in live_editor lane"), CurrentLane, FString(TEXT("live_editor")));
		TestTrue(TEXT("retry blocked should expose same-lane recovery kind"), Payload->TryGetStringField(TEXT("recovery_kind"), RecoveryKind));
		TestEqual(TEXT("retry blocked recovery kind should be same-lane"), RecoveryKind, FString(TEXT("same_lane_recovery")));
		TestTrue(TEXT("retry blocked should expose detached escalation guard"), Payload->TryGetBoolField(TEXT("detached_lane_escalation"), bDetachedEscalation));
		TestFalse(TEXT("retry blocked transport recovery must not mark detached escalation"), bDetachedEscalation);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBackendTransportRecovery_RetryAttemptAndReplayMetadata,
	"OsvayderUE.AgentTrace.TransportRecovery.RetryAttemptAndReplayMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackendTransportRecovery_RetryAttemptAndReplayMetadata::RunTest(const FString& Parameters)
{
	using namespace BackendTransportRecoveryTests;

	FOsvayderUEExecutionLog::Get().Clear();

	const FAgentBackendStatus Status = MakeReadyCodexStatus();
	const FAgentRequestConfig Config = MakePersistentTransportConfig();
	const FString TimeoutMessage =
		TEXT("Timed out waiting for Codex persistent app-server response. Persistent transport was reset; the next request will start a fresh app-server session.");

	const FString FailedRunId = FOsvayderUEAgentTraceLog::Get().BeginRun(Status, Config, Config.Prompt, true, true);
	FOsvayderUEAgentTraceLog::Get().LogBackendFailure(FailedRunId, Status.Backend, TimeoutMessage, TEXT("backend_completion"));
	FOsvayderUEAgentTraceLog::Get().CompleteRun(FailedRunId, Status.Backend, TimeoutMessage, false);
	TestTrue(
		TEXT("retry attempt should be recorded for the latest transport failure"),
		FOsvayderUEAgentTraceLog::Get().MarkTransportRetryAttempt(Status.Backend, TEXT("widget_retry_last")));

	FAgentTransportFailureSummary Summary;
	TestTrue(
		TEXT("latest transport failure summary should still be readable after marking retry"),
		FOsvayderUEAgentTraceLog::Get().TryGetLatestTransportFailureSummary(Status.Backend, Summary));
	TestTrue(TEXT("summary should record that retry was attempted"), Summary.bRetryAttempted);

	FAgentRequestConfig ReplayConfig = MakePersistentTransportConfig();
	ReplayConfig.bIsTransportRetryReplay = true;
	ReplayConfig.TransportRetrySourceRunId = FailedRunId;
	ReplayConfig.TransportRetryFailureFamily = TEXT("persistent_transport_timeout");
	const FString ReplayRunId = FOsvayderUEAgentTraceLog::Get().BeginRun(Status, ReplayConfig, ReplayConfig.Prompt, true, true);
	FOsvayderUEAgentTraceLog::Get().CompleteRun(ReplayRunId, Status.Backend, TEXT("retry succeeded"), true);

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = FailedRunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 20;
	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> FailedRunRecords =
		FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);
	TestTrue(
		TEXT("transport retry attempted event should be emitted"),
		FindRecordByEventType(FailedRunRecords, TEXT("transport_retry_attempted")).IsValid());

	QueryOptions.RunId = ReplayRunId;
	const TArray<TSharedPtr<FJsonObject>> ReplayRecords =
		FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);
	const TSharedPtr<FJsonObject> RunStartedRecord = FindRecordByEventType(ReplayRecords, TEXT("run_started"));
	TestTrue(TEXT("replay run should emit run_started"), RunStartedRecord.IsValid());
	if (const TSharedPtr<FJsonObject> Payload = GetPayloadObject(RunStartedRecord))
	{
		const TSharedPtr<FJsonObject>* RetryObject = nullptr;
		TestTrue(TEXT("run_started payload should include transport retry replay object"), Payload->TryGetObjectField(TEXT("transport_retry_replay"), RetryObject));
		if (RetryObject != nullptr && RetryObject->IsValid())
		{
			FString SourceRunId;
			TestTrue(TEXT("replay metadata should expose source run id"), (*RetryObject)->TryGetStringField(TEXT("source_run_id"), SourceRunId));
			TestEqual(TEXT("replay metadata should preserve the failed run id"), SourceRunId, FailedRunId);
		}
	}

	bool bFoundRetryAttemptReceipt = false;
	for (const FExecutionReceipt& Receipt : FOsvayderUEExecutionLog::Get().GetRecent(8))
	{
		if (Receipt.TransportRetrySourceRunId.Equals(FailedRunId, ESearchCase::CaseSensitive)
			&& Receipt.bTransportRetryAttempted)
		{
			bFoundRetryAttemptReceipt = true;
			break;
		}
	}
	TestTrue(TEXT("execution log should record a retry-attempt receipt"), bFoundRetryAttemptReceipt);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBackendTransportRecovery_WidgetGuidanceTruth,
	"OsvayderUE.Widget.TransportRecovery.GuidanceTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackendTransportRecovery_WidgetGuidanceTruth::RunTest(const FString& Parameters)
{
	const FString SafeStatus = SOsvayderEditorWidget::BuildTransportRetryStatusLabel(true);
	const FString UnsafeStatus = SOsvayderEditorWidget::BuildTransportRetryStatusLabel(false);
	const FString UnsafeNotice =
		SOsvayderEditorWidget::BuildTransportRetryNotice(
			TEXT("Codex CLI"),
			false,
			TEXT("tool_activity_observed"));

	TestTrue(TEXT("safe status should mention fresh session"), SafeStatus.Contains(TEXT("fresh session"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("unsafe status should mention tool activity"), UnsafeStatus.Contains(TEXT("tool activity"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("safe status should stay lane-centric"), SafeStatus.Contains(TEXT("working in editor"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("unsafe notice should stay lane-centric"), UnsafeNotice.Contains(TEXT("working in editor"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("unsafe notice should explain that retry is blocked"), UnsafeNotice.Contains(TEXT("blocked"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("block reason helper should expand tool activity"),
		SOsvayderEditorWidget::DescribeTransportRetryBlockReason(TEXT("tool_activity_observed")).Contains(TEXT("tool activity"), ESearchCase::IgnoreCase));

	return true;
}
