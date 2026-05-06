// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_ExecutionLogStatus.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEScopePolicy.h"

FMCPToolResult FMCPTool_ExecutionLogStatus::Execute(const TSharedRef<FJsonObject>& Params)
{
	int32 Count = ExtractOptionalNumber<int32>(Params, TEXT("count"), 20);
	Count = FMath::Clamp(Count, 1, 100);
	FString ToolFilter = ExtractOptionalString(Params, TEXT("tool_filter"));
	bool bDeniedOnly = ExtractOptionalBool(Params, TEXT("denied_only"), false);
	FString ClassFilter = ExtractOptionalString(Params, TEXT("classification_filter"));

	const auto& Log = FOsvayderUEExecutionLog::Get();
	TArray<FExecutionReceipt> AllRecent = Log.GetRecent(100);

	// Apply filters
	TArray<FExecutionReceipt> Recent;
	for (const FExecutionReceipt& R : AllRecent)
	{
		if (!ToolFilter.IsEmpty() && !R.Tool.Contains(ToolFilter)) continue;
		if (bDeniedOnly && R.Classification != TEXT("denied") && R.Status != TEXT("denied")) continue;
		if (!ClassFilter.IsEmpty() && R.Classification != ClassFilter) continue;
		Recent.Add(R);
		if (Recent.Num() >= Count) break;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("total_entries"), Log.GetReceipts().Num());
	Data->SetNumberField(TEXT("returned_entries"), Recent.Num());

	// Scope policy info
	TSharedPtr<FJsonObject> ScopeObj = MakeShared<FJsonObject>();
	TArray<FString> AllowedRoots = FOsvayderUEScopePolicy::GetAllowedWriteRoots();
	TArray<TSharedPtr<FJsonValue>> RootsArr;
	for (const FString& Root : AllowedRoots)
	{
		RootsArr.Add(MakeShared<FJsonValueString>(Root.IsEmpty() ? TEXT("(all paths)") : Root));
	}
	ScopeObj->SetArrayField(TEXT("allowed_write_roots"), RootsArr);
	Data->SetObjectField(TEXT("scope_policy"), ScopeObj);
	Data->SetStringField(TEXT("session_log_path"), Log.GetSessionLogPath());

	// Recent entries
	TArray<TSharedPtr<FJsonValue>> EntriesArr;
	for (const FExecutionReceipt& R : Recent)
	{
		EntriesArr.Add(MakeShared<FJsonValueObject>(R.ToJson()));
	}
	Data->SetArrayField(TEXT("entries"), EntriesArr);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Execution log: %d recent entries"), Recent.Num()),
		Data
	);
}
