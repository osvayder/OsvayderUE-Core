// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_AgentTraceStatus.h"

#include "UnrealClaudeAgentTrace.h"

FMCPToolResult FMCPTool_AgentTraceStatus::Execute(const TSharedRef<FJsonObject>& Params)
{
	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = ExtractOptionalString(Params, TEXT("run_id"));
	QueryOptions.EventType = ExtractOptionalString(Params, TEXT("event_type"));
	QueryOptions.Backend = ExtractOptionalString(Params, TEXT("backend"));
	QueryOptions.Count = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("count"), 60), 1, 200);
	QueryOptions.bLatestOnly = ExtractOptionalBool(Params, TEXT("latest_only"), true);
	QueryOptions.bIncludeRawJson = ExtractOptionalBool(Params, TEXT("include_raw_json"), false);
	QueryOptions.PreviewChars = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("preview_chars"), 800), 80, 12000);

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Events = FUnrealClaudeAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("trace_log_path"), FUnrealClaudeAgentTraceLog::Get().GetTraceLogPath());
	Data->SetStringField(TEXT("resolved_run_id"), ResolvedRunId);
	Data->SetStringField(TEXT("note"), TEXT("Observable execution trace only. Hidden/private model reasoning is not exposed here."));
	Data->SetNumberField(TEXT("schema_version"), 1);
	Data->SetNumberField(TEXT("total_events_loaded"), TotalLoaded);
	Data->SetNumberField(TEXT("returned_events"), Events.Num());
	Data->SetBoolField(TEXT("latest_only"), QueryOptions.bLatestOnly);
	Data->SetNumberField(TEXT("preview_chars"), QueryOptions.PreviewChars);
	Data->SetBoolField(TEXT("include_raw_json"), QueryOptions.bIncludeRawJson);

	TArray<TSharedPtr<FJsonValue>> EventTypes;
	for (const TCHAR* EventType : {
		TEXT("run_started"),
		TEXT("session_restored"),
		TEXT("context_refreshed"),
		TEXT("backend_snapshot"),
		TEXT("user_prompt_submitted"),
		TEXT("stream_text"),
		TEXT("tool_use"),
		TEXT("tool_result"),
		TEXT("result"),
		TEXT("backend_error"),
		TEXT("timeout"),
		TEXT("cancellation"),
		TEXT("backend_switch"),
		TEXT("new_session"),
		TEXT("restore_session"),
		TEXT("run_completed") })
	{
		EventTypes.Add(MakeShared<FJsonValueString>(EventType));
	}
	Data->SetArrayField(TEXT("supported_event_types"), EventTypes);

	TArray<TSharedPtr<FJsonValue>> EventArray;
	EventArray.Reserve(Events.Num());
	for (const TSharedPtr<FJsonObject>& Event : Events)
	{
		EventArray.Add(MakeShared<FJsonValueObject>(Event));
	}
	Data->SetArrayField(TEXT("events"), EventArray);

	const FString RunSuffix = ResolvedRunId.IsEmpty()
		? FString()
		: FString::Printf(TEXT(" for %s"), *ResolvedRunId);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Agent trace: %d event(s)%s"), Events.Num(), *RunSuffix),
		Data);
}
