// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "UnrealClaudeConstants.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUnrealClaude, Log, All);

class FUnrealClaudeMCPServer;
class SClaudeEditorWidget;

struct FUnrealClaudeBuildSyncStatus
{
	bool bBinaryPresent = false;
	bool bFresh = false;
	FString Detail;
	FString BinaryPath;
	FString LatestSourcePath;
	FDateTime BinaryTimestamp = FDateTime::MinValue();
	FDateTime LatestSourceTimestamp = FDateTime::MinValue();
};

class FUnrealClaudeModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Get the singleton instance */
	static FUnrealClaudeModule& Get();

	/** Check if module is available */
	static bool IsAvailable();

	/** Evaluate whether the loaded plugin binary is fresh relative to tracked source inputs. */
	static FUnrealClaudeBuildSyncStatus GetBuildSyncStatus();

	/** Convenience helper for UI and verification paths. */
	static bool HasFreshPluginBuild();

	/** Absolute path to the preferred preflight launcher. */
	static FString GetPreflightLauncherPath();

	/** Get the MCP server instance */
	TSharedPtr<FUnrealClaudeMCPServer> GetMCPServer() const { return MCPServer; }

	/** Get MCP server port - uses centralized constant */
	static constexpr uint32 GetMCPServerPort() { return UnrealClaudeConstants::MCPServer::DefaultPort; }

	TSharedPtr<SClaudeEditorWidget> OpenClaudePanelAndGetWidget();

private:
	void RegisterSettingsUiCustomization();
	void UnregisterSettingsUiCustomization();
	void RegisterMenus();
	void UnregisterMenus();
	void StartMCPServer();
	void StopMCPServer();

	TSharedPtr<class FUICommandList> PluginCommands;
	TSharedPtr<class SDockTab> ClaudeTab;
	TSharedPtr<FUnrealClaudeMCPServer> MCPServer;
	bool bSettingsUiCustomizationRegistered = false;
};
