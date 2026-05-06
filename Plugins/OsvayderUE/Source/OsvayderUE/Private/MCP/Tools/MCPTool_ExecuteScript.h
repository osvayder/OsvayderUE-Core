// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool for executing scripts with user permission
 *
 * Supports: C++, Python, Console commands, Editor Utility
 *
 * IMPORTANT: Scripts MUST include a header comment with @Description
 * This description is stored in history for context restoration.
 *
 * C++ Header Format:
 * @code
 *   // @OsvayderUE Script
 *   // @Name: MyScript
 *   // @Description: Brief description of what this script does
 *   // @Created: 2026-01-03T10:30:00Z
 * @endcode
 *
 * Python Header Format:
 * @code
 *   # @OsvayderUE Script
 *   # @Name: MyScript
 *   # @Description: Brief description of what this script does
 *   # @Created: 2026-01-03T10:30:00Z
 * @endcode
 */
class FMCPTaskQueue;

class FMCPTool_ExecuteScript : public FMCPToolBase
{
public:
	/** Set the task queue for async execution */
	void SetTaskQueue(TSharedPtr<FMCPTaskQueue> InTaskQueue) { TaskQueue = InTaskQueue; }

	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("execute_script");
		Info.Description = TEXT(
			"Execute custom scripts in Unreal Engine with user permission.\n\n"
			"IMPORTANT: Only use this tool when no dedicated MCP tool can accomplish the task. "
			"Prefer specific tools first: spawn_actor, move_actor, set_property, blueprint_modify, "
			"open_level, asset_search, etc. Scripts require user approval and are slower than dedicated tools.\n\n"
			"Script types:\n"
			"- 'cpp': C++ via Live Coding (auto-retries on compile failure)\n"
			"- 'python': Python script (requires Python plugin)\n"
			"- 'console': Console command batch\n"
			"- 'editor_utility': Editor Utility Widget/Blueprint\n\n"
			"IMPORTANT: Include @Description in script header for history tracking:\n"
			"/** @OsvayderUE Script\\n * @Description: What this script does */\n\n"
			"Returns: Script execution result, output, and any errors."
		);
		Info.Parameters = {
			FMCPToolParameter(
				TEXT("script_type"),
				TEXT("string"),
				TEXT("Type: 'cpp', 'python', 'console', or 'editor_utility'"),
				true
			),
			FMCPToolParameter(
				TEXT("script_content"),
				TEXT("string"),
				TEXT("The script code. MUST include @Description in header comment."),
				true
			),
			FMCPToolParameter(
				TEXT("description"),
				TEXT("string"),
				TEXT("Brief description (optional if @Description in header)"),
				false
			),
			FMCPToolParameter(
				TEXT("execution_profile"),
				TEXT("string"),
				TEXT("Optional governed runtime selection for representative policy checks: configured_default_runtime, read_only_diagnostic, bounded_plugin_mutation, or explicit_expert_opt_in"),
				false
			)
		};
		// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
		Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
			TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));
		Info.Annotations = FMCPToolAnnotations::Modifying();
		Info.Annotations.bDestructiveHint = true; // Scripts can do anything
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	/** Task queue for async execution */
	TSharedPtr<FMCPTaskQueue> TaskQueue;

	/** Execute script synchronously (called by task queue) */
	FMCPToolResult ExecuteSync(const TSharedRef<FJsonObject>& Params);
};
