// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Run an Unreal console command
 */
class FMCPTool_RunConsoleCommand : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("run_console_command");
		Info.Description = TEXT(
			"Execute an Unreal Engine console command.\n\n"
			"Console commands provide access to engine features, debugging tools, and configuration.\n\n"
			"Useful commands:\n"
			"- 'stat fps' - Show FPS counter\n"
			"- 'stat unit' - Show frame timing\n"
			"- 'show collision' - Toggle collision visualization\n"
			"- 'show bounds' - Toggle bounding box display\n"
			"- 'r.SetRes 1920x1080' - Set resolution\n"
			"- 'slomo 0.5' - Slow motion (PIE only)\n"
			"- 'ce MyEvent' - Call custom event\n\n"
			"Note: Some commands only work in Play-In-Editor (PIE) mode.\n\n"
			"Returns: Command execution confirmation."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("command"), TEXT("string"), TEXT("The console command to execute (e.g., 'stat fps', 'show collision')"), true),
			FMCPToolParameter(TEXT("execution_profile"), TEXT("string"), TEXT("Optional governed runtime selection for representative policy checks: configured_default_runtime, read_only_diagnostic, bounded_plugin_mutation, or explicit_expert_opt_in"), false)
		};
		// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
		Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
			TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
