// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "OsvayderUEConstants.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOsvayderUE, Log, All);

class FOsvayderUEMCPServer;
class SOsvayderEditorWidget;

struct FOsvayderUEBuildSyncStatus
{
	bool bBinaryPresent = false;
	bool bFresh = false;
	FString Detail;
	FString BinaryPath;
	FString LatestSourcePath;
	FDateTime BinaryTimestamp = FDateTime::MinValue();
	FDateTime LatestSourceTimestamp = FDateTime::MinValue();
};

class FOsvayderUEModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Get the singleton instance */
	static FOsvayderUEModule& Get();

	/** Check if module is available */
	static bool IsAvailable();

	/** Evaluate whether the loaded plugin binary is fresh relative to tracked source inputs. */
	static FOsvayderUEBuildSyncStatus GetBuildSyncStatus();

	/** Convenience helper for UI and verification paths. */
	static bool HasFreshPluginBuild();

	/** Absolute path to the preferred preflight launcher. */
	static FString GetPreflightLauncherPath();

	/** Get the MCP server instance */
	TSharedPtr<FOsvayderUEMCPServer> GetMCPServer() const { return MCPServer; }

	/** Get MCP server port - uses centralized constant */
	static constexpr uint32 GetMCPServerPort() { return OsvayderUEConstants::MCPServer::DefaultPort; }

	TSharedPtr<SOsvayderEditorWidget> OpenClaudePanelAndGetWidget();

private:
	void RegisterSettingsUiCustomization();
	void UnregisterSettingsUiCustomization();
	void RegisterMenus();
	void UnregisterMenus();
	void StartMCPServer();
	void StopMCPServer();

	TSharedPtr<class FUICommandList> PluginCommands;
	TSharedPtr<class SDockTab> ClaudeTab;
	TSharedPtr<FOsvayderUEMCPServer> MCPServer;
	bool bSettingsUiCustomizationRegistered = false;
};
