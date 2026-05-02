#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UnrealClaudeReleaseHygieneTests
{
	bool TryReadPluginJson(TSharedPtr<FJsonObject>& OutJson, FString& OutPluginBaseDir)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealClaude"));
		if (!Plugin.IsValid())
		{
			return false;
		}

		OutPluginBaseDir = Plugin->GetBaseDir();
		FString DescriptorText;
		if (!FFileHelper::LoadFileToString(DescriptorText, *Plugin->GetDescriptorFileName()))
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DescriptorText);
		return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
	}

	bool ReadFilterPluginText(const FString& PluginBaseDir, FString& OutText)
	{
		const FString FilterPath = FPaths::Combine(PluginBaseDir, TEXT("Config"), TEXT("FilterPlugin.ini"));
		return FFileHelper::LoadFileToString(OutText, *FilterPath);
	}

	bool ReadCommandHeaderText(const FString& PluginBaseDir, FString& OutText)
	{
		const FString CommandHeaderPath = FPaths::Combine(PluginBaseDir, TEXT("Source"), TEXT("UnrealClaude"), TEXT("Public"), TEXT("UnrealClaudeCommands.h"));
		return FFileHelper::LoadFileToString(OutText, *CommandHeaderPath);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealClaudeReleaseHygiene_UPluginMetadataTruth,
	"UnrealClaude.ReleaseHygiene.UPluginMetadataTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealClaudeReleaseHygiene_UPluginMetadataTruth::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Descriptor;
	FString PluginBaseDir;
	if (!TestTrue(TEXT("UnrealClaude plugin descriptor is readable"), UnrealClaudeReleaseHygieneTests::TryReadPluginJson(Descriptor, PluginBaseDir)))
	{
		return false;
	}

	TestEqual(TEXT("FriendlyName is release-facing product label"), Descriptor->GetStringField(TEXT("FriendlyName")), FString(TEXT("Osvayder UE")));
	TestEqual(TEXT("VersionName matches MCP bridge package version"), Descriptor->GetStringField(TEXT("VersionName")), FString(TEXT("1.4.1")));
	TestTrue(TEXT("IsBetaVersion truthfully marks controlled-dogfood maturity"), Descriptor->GetBoolField(TEXT("IsBetaVersion")));
	TestFalse(TEXT("IsExperimentalVersion remains false"), Descriptor->GetBoolField(TEXT("IsExperimentalVersion")));
	TestTrue(TEXT("Description is provider-neutral Osvayder copy"), Descriptor->GetStringField(TEXT("Description")).Contains(TEXT("Osvayder UE")));
	TestFalse(TEXT("Description does not expose old Claude Code-only positioning"), Descriptor->GetStringField(TEXT("Description")).Contains(TEXT("Claude Code CLI integration")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealClaudeReleaseHygiene_CommandContextLabelTruth,
	"UnrealClaude.ReleaseHygiene.CommandContextLabelTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealClaudeReleaseHygiene_CommandContextLabelTruth::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Descriptor;
	FString PluginBaseDir;
	if (!TestTrue(TEXT("UnrealClaude plugin descriptor is readable"), UnrealClaudeReleaseHygieneTests::TryReadPluginJson(Descriptor, PluginBaseDir)))
	{
		return false;
	}

	FString CommandHeaderText;
	if (!TestTrue(TEXT("UnrealClaudeCommands.h is readable"), UnrealClaudeReleaseHygieneTests::ReadCommandHeaderText(PluginBaseDir, CommandHeaderText)))
	{
		return false;
	}

	TestTrue(TEXT("Command context uses Osvayder UE"), CommandHeaderText.Contains(TEXT("NSLOCTEXT(\"Contexts\", \"UnrealClaude\", \"Osvayder UE\")")));
	TestFalse(TEXT("Command context does not expose old Unreal Claude label"), CommandHeaderText.Contains(TEXT("NSLOCTEXT(\"Contexts\", \"UnrealClaude\", \"Unreal Claude\")")));
	TestFalse(TEXT("Command header does not expose old Claude Assistant panel copy"), CommandHeaderText.Contains(TEXT("Open the Claude Assistant panel")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealClaudeReleaseHygiene_FilterPluginExcludesLocalArtifacts,
	"UnrealClaude.ReleaseHygiene.FilterPluginExcludesLocalArtifacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUnrealClaudeReleaseHygiene_FilterPluginExcludesLocalArtifacts::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Descriptor;
	FString PluginBaseDir;
	if (!TestTrue(TEXT("UnrealClaude plugin descriptor is readable"), UnrealClaudeReleaseHygieneTests::TryReadPluginJson(Descriptor, PluginBaseDir)))
	{
		return false;
	}

	FString FilterText;
	if (!TestTrue(TEXT("FilterPlugin.ini is readable"), UnrealClaudeReleaseHygieneTests::ReadFilterPluginText(PluginBaseDir, FilterText)))
	{
		return false;
	}

	const TArray<FString> RequiredExcludes = {
		TEXT("-/.mcp.json"),
		TEXT("-/Binaries/..."),
		TEXT("-/Intermediate/..."),
		TEXT("-/Saved/..."),
		TEXT("-/CLAUDE.md"),
		TEXT("-/CLAUDE.md.backup_*"),
		TEXT("-/CLAUDE.md.default"),
		TEXT("-/structure.txt"),
		TEXT("-/Resources/mcp-bridge/node_modules/..."),
		TEXT("-/Resources/mcp-bridge/coverage/..."),
		TEXT("-/Resources/mcp-bridge/.git/..."),
		TEXT("-/Resources/mcp-bridge/index.js.backup_*")
	};

	for (const FString& RequiredExclude : RequiredExcludes)
	{
		TestTrue(*FString::Printf(TEXT("FilterPlugin.ini contains %s"), *RequiredExclude), FilterText.Contains(RequiredExclude));
	}

	return true;
}

#endif
