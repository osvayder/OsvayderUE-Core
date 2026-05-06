// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MCP/Tools/MCPTool_PluginSettings.h"
#include "UObject/Class.h"
#include "OsvayderUESettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SettingsBrandingTests
{
	bool ReadSettingsHeaderText(FString& OutText)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("OsvayderUE"));
		if (!Plugin.IsValid())
		{
			return false;
		}

		const FString HeaderPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source"), TEXT("OsvayderUE"), TEXT("Public"), TEXT("OsvayderUESettings.h"));
		return FFileHelper::LoadFileToString(OutText, *HeaderPath);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSettingsBranding_ProjectSettingsDisplayNameUsesOsvayder,
	"OsvayderUE.Settings.Branding.ProjectSettingsDisplayNameUsesOsvayder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsBranding_ProjectSettingsDisplayNameUsesOsvayder::RunTest(const FString& Parameters)
{
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	TestNotNull(TEXT("settings should exist"), Settings);
	if (!Settings)
	{
		return false;
	}

	const UClass* SettingsClass = UOsvayderUESettings::StaticClass();
	TestEqual(
		TEXT("visible UCLASS DisplayName should use product branding"),
		SettingsClass->GetMetaData(TEXT("DisplayName")),
		FString(TEXT("Osvayder UE")));
	TestEqual(
		TEXT("Project Settings section label should use product branding"),
		Settings->GetSectionText().ToString(),
		FString(TEXT("Osvayder UE")));
	TestTrue(
		TEXT("Project Settings description should use product branding"),
		Settings->GetSectionDescription().ToString().Contains(TEXT("Osvayder UE")));

	TestEqual(
		TEXT("settings class path should remain legacy until Packet F"),
		SettingsClass->GetPathName(),
		FString(TEXT("/Script/OsvayderUE.OsvayderUESettings")));
	TestEqual(
		TEXT("settings section key should remain legacy until Packet F"),
		Settings->GetSectionName(),
		FName(TEXT("OsvayderUE")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSettingsBranding_ProjectSettingsTooltipsUseOsvayder,
	"OsvayderUE.Settings.Branding.ProjectSettingsTooltipsUseOsvayder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsBranding_ProjectSettingsTooltipsUseOsvayder::RunTest(const FString& Parameters)
{
	const UClass* SettingsClass = UOsvayderUESettings::StaticClass();

	const FProperty* ScopeProperty = SettingsClass->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, ScopeMode));
	TestNotNull(TEXT("ScopeMode property should exist"), ScopeProperty);
	if (!ScopeProperty)
	{
		return false;
	}

	const FString ScopeTooltip = ScopeProperty->GetMetaData(TEXT("ToolTip"));
	TestTrue(TEXT("Scope tooltip should use product branding"), ScopeTooltip.Contains(TEXT("Osvayder UE")));
	TestFalse(TEXT("Scope tooltip should not use stale product branding"), ScopeTooltip.Contains(TEXT("OsvayderUE")));

	const FProperty* McpPortProperty = SettingsClass->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, MCPServerPort));
	TestNotNull(TEXT("MCPServerPort property should exist"), McpPortProperty);
	if (!McpPortProperty)
	{
		return false;
	}

	const FString McpPortTooltip = McpPortProperty->GetMetaData(TEXT("ToolTip"));
	TestTrue(TEXT("MCP tooltip should use product branding"), McpPortTooltip.Contains(TEXT("Osvayder UE")));
	TestFalse(TEXT("MCP tooltip should not use stale product branding"), McpPortTooltip.Contains(TEXT("OsvayderUE")));

	const FProperty* DefaultModelProperty = SettingsClass->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultModel));
	TestNotNull(TEXT("DefaultModel property should exist"), DefaultModelProperty);
	if (!DefaultModelProperty)
	{
		return false;
	}

	TestEqual(TEXT("DefaultModel display name should preserve provider truth"), DefaultModelProperty->GetMetaData(TEXT("DisplayName")), FString(TEXT("Claude Model")));

	const FString DefaultModelTooltip = DefaultModelProperty->GetMetaData(TEXT("ToolTip"));
	TestTrue(TEXT("DefaultModel tooltip should describe the Claude backend"), DefaultModelTooltip.Contains(TEXT("preferred backend is Claude")));
	TestFalse(TEXT("DefaultModel tooltip should not use stale product branding"), DefaultModelTooltip.Contains(TEXT("Claude Assistant")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSettingsBranding_SettingsHeaderAvoidsStaleProductLabels,
	"OsvayderUE.Settings.Branding.SettingsHeaderAvoidsStaleProductLabels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsBranding_SettingsHeaderAvoidsStaleProductLabels::RunTest(const FString& Parameters)
{
	FString HeaderText;
	if (!TestTrue(TEXT("settings header should be readable"), SettingsBrandingTests::ReadSettingsHeaderText(HeaderText)))
	{
		return false;
	}

	TestFalse(TEXT("settings header should not expose stale Claude Assistant product wording"), HeaderText.Contains(TEXT("Claude Assistant")));
	TestFalse(TEXT("settings header should not expose stale Unreal Claude product wording"), HeaderText.Contains(TEXT("Unreal Claude")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSettingsBranding_PluginSettingsToolMessageUsesOsvayder,
	"OsvayderUE.Settings.Branding.PluginSettingsToolMessageUsesOsvayder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsBranding_PluginSettingsToolMessageUsesOsvayder::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolInfo Info = Tool.GetInfo();
	TestTrue(TEXT("plugin settings tool description should use product branding"), Info.Description.Contains(TEXT("Osvayder UE")));
	TestFalse(TEXT("plugin settings tool description should not use stale product branding"), Info.Description.Contains(TEXT("OsvayderUE")));

	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());

	TestTrue(TEXT("plugin settings tool should succeed"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		return false;
	}

	TestTrue(TEXT("plugin settings tool message should use product branding"), Result.Message.Contains(TEXT("Osvayder UE")));
	TestFalse(TEXT("plugin settings tool message should not use stale product branding"), Result.Message.Contains(TEXT("OsvayderUE")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
