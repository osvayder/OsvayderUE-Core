// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_ProjectMemoryStatus.h"
#include "UnrealClaudeSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

static FString ExtractLatestHandoffMeta(const FString& FilePath)
{
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FilePath))
	{
		return TEXT("file not found");
	}

	// Find first ## MESSAGE block
	int32 MsgIdx = Content.Find(TEXT("## MESSAGE"));
	if (MsgIdx == INDEX_NONE)
	{
		return TEXT("no handoff");
	}

	// Extract first ~5 lines after ## MESSAGE
	FString Block = Content.Mid(MsgIdx, 300);
	// Find next ## MESSAGE to trim
	int32 NextMsg = Block.Find(TEXT("## MESSAGE"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 12);
	if (NextMsg != INDEX_NONE)
	{
		Block = Block.Left(NextMsg);
	}

	// Extract key fields
	FString Result;
	TArray<FString> Lines;
	Block.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("From:")) || Line.StartsWith(TEXT("Date:")) ||
			Line.StartsWith(TEXT("Status:")) || Line.StartsWith(TEXT("Task:")))
		{
			Result += Line.TrimStartAndEnd() + TEXT("\n");
		}
	}
	return Result.IsEmpty() ? TEXT("parsed but empty") : Result;
}

FMCPToolResult FMCPTool_ProjectMemoryStatus::Execute(const TSharedRef<FJsonObject>& Params)
{
	const UUnrealClaudeSettings* Settings = UUnrealClaudeSettings::Get();
	FString ProjectRoot = FPaths::ProjectDir();

	FString MemoryPath = (Settings && !Settings->ProjectMemoryPath.IsEmpty())
		? Settings->ProjectMemoryPath : TEXT("Docs/UnrealClaude");
	FString BridgePath = (Settings && !Settings->AgentBridgePath.IsEmpty())
		? Settings->AgentBridgePath : TEXT("AgentBridge");

	FString FullMemoryPath = FPaths::Combine(ProjectRoot, MemoryPath);
	FString FullBridgePath = FPaths::Combine(ProjectRoot, BridgePath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// Paths
	Data->SetStringField(TEXT("memory_path"), MemoryPath);
	Data->SetStringField(TEXT("bridge_path"), BridgePath);

	// Curated docs scan
	TArray<TSharedPtr<FJsonValue>> DocsFound;
	TArray<FString> FoundFiles;
	IFileManager::Get().FindFiles(FoundFiles, *(FullMemoryPath / TEXT("*.md")), true, false);
	for (const FString& File : FoundFiles)
	{
		DocsFound.Add(MakeShared<FJsonValueString>(File));
	}
	Data->SetArrayField(TEXT("curated_docs"), DocsFound);
	Data->SetNumberField(TEXT("curated_doc_count"), DocsFound.Num());

	// Key docs presence
	Data->SetBoolField(TEXT("has_execution_journal"),
		IFileManager::Get().FileExists(*(FullMemoryPath / TEXT("80_ExecutionJournal.md"))));
	Data->SetBoolField(TEXT("has_current_state"),
		IFileManager::Get().FileExists(*(FullMemoryPath / TEXT("90_CurrentState.md"))));
	Data->SetBoolField(TEXT("has_project_memory"),
		IFileManager::Get().FileExists(*(FullMemoryPath / TEXT("00_ProjectMemory.md"))));

	// Bridge handoff metadata (AgentBridgePath consumed here)
	TSharedPtr<FJsonObject> BridgeObj = MakeShared<FJsonObject>();
	FString ClaudeToCodex = FPaths::Combine(FullBridgePath, TEXT("CLAUDE_TO_CODEX.md"));
	FString CodexToClaude = FPaths::Combine(FullBridgePath, TEXT("CODEX_TO_CLAUDE.md"));

	BridgeObj->SetStringField(TEXT("claude_to_codex_latest"), ExtractLatestHandoffMeta(ClaudeToCodex));
	BridgeObj->SetStringField(TEXT("codex_to_claude_latest"), ExtractLatestHandoffMeta(CodexToClaude));
	BridgeObj->SetBoolField(TEXT("bridge_exists"), FPaths::DirectoryExists(FullBridgePath));
	Data->SetObjectField(TEXT("bridge"), BridgeObj);

	// Capability flags from settings
	if (Settings)
	{
		TSharedPtr<FJsonObject> Caps = MakeShared<FJsonObject>();
		Caps->SetBoolField(TEXT("osvayder_eye_enabled"), Settings->bEnableOsvayderEye);
		Caps->SetBoolField(TEXT("multiplayer_first"), Settings->bMultiplayerFirst);
		Caps->SetStringField(TEXT("scope_mode"),
			Settings->ScopeMode == EUnrealClaudeScopeMode::PluginOnly ? TEXT("PluginOnly") : TEXT("PluginAndProject"));
		Data->SetObjectField(TEXT("capabilities"), Caps);
	}

	return FMCPToolResult::Success(TEXT("Project memory and bridge status"), Data);
}
