// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ScriptTypes.h"

/**
 * Manages script execution pipeline:
 * - Permission dialog flow
 * - Script compilation (C++ via Live Coding)
 * - Script execution (Python, Console, Editor Utility)
 * - Persistent history log
 * - Auto-retry on build failure
 */
class OSVAYDERUE_API FScriptExecutionManager
{
public:
	/** Get singleton instance */
	static FScriptExecutionManager& Get();

	FScriptExecutionManager();
	~FScriptExecutionManager();

	/**
	 * Execute a script with permission flow
	 * @param Type - Script type (Cpp, Python, Console, EditorUtility)
	 * @param ScriptContent - The script code (MUST include header comment with @Description)
	 * @param Description - Brief description (used if not in header)
	 * @return Execution result
	 */
	FScriptExecutionResult ExecuteScript(
		EScriptType Type,
		const FString& ScriptContent,
		const FString& Description = TEXT("")
	);

	/**
	 * Get recent script history for Claude context
	 * @param Count - Number of recent scripts to return
	 * @return Array of history entries
	 */
	TArray<FScriptHistoryEntry> GetRecentScripts(int32 Count = 10) const;

	/**
	 * Format recent scripts for Claude context prompt
	 * @param Count - Number of recent scripts to include
	 * @return Formatted string for system prompt
	 */
	FString FormatHistoryForContext(int32 Count = 10) const;

	/** Clear all script history */
	void ClearHistory();

	/** Save history to disk */
	bool SaveHistory();

	/** Load history from disk */
	bool LoadHistory();

	/**
	 * Cleanup all generated scripts and history
	 * @return Result message
	 */
	FString CleanupAll();

	/** Get path to script history JSON file */
	FString GetHistoryFilePath() const;

	/** Get directory for generated C++ scripts */
	FString GetCppScriptDirectory() const;

	/** Get directory for Python/other scripts */
	FString GetContentScriptDirectory() const;

	/**
	 * 619 P1 (D2: single source of truth): publicly exposed so the new
	 * MCPTool_LiveCodingCompile can delegate here without reimplementing
	 * the ILiveCodingModule dispatch + polling + UBT log fallback logic.
	 * ExecuteCpp (private) is the historical sole call site; this helper
	 * now has a second call site (MCPTool_LiveCodingCompile::Execute).
	 * @param OutErrorLog - Compilation error output
	 * @param OutDiagnostics - Structured diagnostics JSON (file/line/severity/code)
	 * @return true if compilation succeeded
	 */
	bool TriggerLiveCodingCompile(FString& OutErrorLog, TSharedPtr<FJsonObject>& OutDiagnostics);

private:
	/** Execute C++ script via Live Coding */
	FScriptExecutionResult ExecuteCpp(const FString& ScriptContent, const FString& Description);

	/** Execute Python script */
	FScriptExecutionResult ExecutePython(const FString& ScriptContent, const FString& Description);

	/** Execute console command(s) */
	FScriptExecutionResult ExecuteConsole(const FString& ScriptContent, const FString& Description);

	/** Execute Editor Utility Blueprint */
	FScriptExecutionResult ExecuteEditorUtility(const FString& ScriptContent, const FString& Description);

	/**
	 * Show permission dialog to user
	 * @param ScriptPreview - Code to display in dialog
	 * @param Type - Script type
	 * @param Description - Script description
	 * @return true if user approved, false if denied
	 */
	bool ShowPermissionDialog(const FString& ScriptPreview, EScriptType Type, const FString& Description);

	// TriggerLiveCodingCompile moved to public section above per 619 P1 D2.

	/**
	 * Write script file to disk
	 * @param Content - Script content
	 * @param Type - Script type (determines directory)
	 * @param ScriptName - Name for the script file
	 * @return Full path to written file, or empty on failure
	 */
	FString WriteScriptFile(const FString& Content, EScriptType Type, const FString& ScriptName);

	/**
	 * Generate unique script name
	 * @param Type - Script type
	 * @param Description - Description to base name on
	 * @return Sanitized unique script name
	 */
	FString GenerateScriptName(EScriptType Type, const FString& Description);

	/**
	 * Add entry to history
	 */
	void AddToHistory(const FScriptHistoryEntry& Entry);

	/** Script execution history */
	TArray<FScriptHistoryEntry> History;

	/** Maximum history entries to keep */
	int32 MaxHistorySize;

	/** Script counter for unique naming */
	int32 ScriptCounter;
};
