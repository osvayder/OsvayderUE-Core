// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_PluginSettings.h"
#include "UnrealClaudeSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FScopedDictationLanguageOverride
	{
		UUnrealClaudeSettings* Settings = nullptr;
		EUnrealClaudeDictationLanguage OriginalLanguage = EUnrealClaudeDictationLanguage::Auto;

		explicit FScopedDictationLanguageOverride(const EUnrealClaudeDictationLanguage NewLanguage)
		{
			Settings = UUnrealClaudeSettings::GetMutable();
			if (Settings)
			{
				OriginalLanguage = Settings->DefaultDictationLanguage;
				Settings->DefaultDictationLanguage = NewLanguage;
			}
		}

		~FScopedDictationLanguageOverride()
		{
			if (Settings)
			{
				Settings->DefaultDictationLanguage = OriginalLanguage;
			}
		}
	};

	TSharedPtr<FJsonObject> GetObjectFieldOrNull(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* NestedObject = nullptr;
		if (!Object->TryGetObjectField(FieldName, NestedObject) || !NestedObject || !(*NestedObject).IsValid())
		{
			return nullptr;
		}

		return *NestedObject;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceDictation_ProjectDefaultIsRussian,
	"UnrealClaude.VoiceDictation.LanguageSupport.ProjectDefaultRussian",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoiceDictation_ProjectDefaultIsRussian::RunTest(const FString& Parameters)
{
	const UUnrealClaudeSettings* Settings = UUnrealClaudeSettings::Get();
	TestNotNull(TEXT("settings should exist"), Settings);
	if (!Settings)
	{
		return false;
	}

	TestEqual(
		TEXT("project default should request Russian dictation"),
		Settings->GetConfiguredDictationLanguageModeName(),
		FString(TEXT("ru_ru")));
	TestEqual(
		TEXT("effective language tag should be Russian for the project default"),
		Settings->GetEffectiveDictationLanguageTag(),
		FString(TEXT("ru-RU")));
	TestEqual(
		TEXT("effective model should be the bounded lightweight Russian Vosk model"),
		Settings->GetEffectiveDictationModelName(),
		FString(TEXT("vosk-model-small-ru-0.22")));
	TestEqual(
		TEXT("support claim should surface the interactive lightweight offline lane"),
		Settings->GetDictationSupportClaimName(),
		FString(TEXT("best_effort_offline_small_model")));
	AddInfo(FString::Printf(
		TEXT("dictation default mapping: effective_language=%s model=%s support_claim=%s"),
		*Settings->GetEffectiveDictationLanguageTag(),
		*Settings->GetEffectiveDictationModelName(),
		*Settings->GetDictationSupportClaimName()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceDictation_EnglishOverrideMapsToEnglishModel,
	"UnrealClaude.VoiceDictation.LanguageSupport.EnglishOverrideMapsToEnglishModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoiceDictation_EnglishOverrideMapsToEnglishModel::RunTest(const FString& Parameters)
{
	FScopedDictationLanguageOverride DictationLanguageOverride(EUnrealClaudeDictationLanguage::EnglishUs);

	const UUnrealClaudeSettings* Settings = UUnrealClaudeSettings::Get();
	TestNotNull(TEXT("settings should exist"), Settings);
	if (!Settings)
	{
		return false;
	}

	TestEqual(
		TEXT("requested mode should surface the explicit English override"),
		Settings->GetConfiguredDictationLanguageModeName(),
		FString(TEXT("en_us")));
	TestEqual(
		TEXT("effective language tag should be English"),
		Settings->GetEffectiveDictationLanguageTag(),
		FString(TEXT("en-US")));
	TestEqual(
		TEXT("effective model should be the bounded lightweight English Vosk model"),
		Settings->GetEffectiveDictationModelName(),
		FString(TEXT("vosk-model-small-en-us-0.15")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoiceDictation_PluginSettingsExposeLanguageAndModelReadback,
	"UnrealClaude.VoiceDictation.LanguageSupport.PluginSettingsReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoiceDictation_PluginSettingsExposeLanguageAndModelReadback::RunTest(const FString& Parameters)
{
	FScopedDictationLanguageOverride DictationLanguageOverride(EUnrealClaudeDictationLanguage::RussianRu);

	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());

	TestTrue(TEXT("plugin settings tool should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin settings tool should return structured data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> DictationObject = GetObjectFieldOrNull(Result.Data, TEXT("dictation"));
	TestTrue(TEXT("dictation readback object should be present"), DictationObject.IsValid());
	if (!DictationObject.IsValid())
	{
		return false;
	}

	FString Runtime;
	DictationObject->TryGetStringField(TEXT("runtime"), Runtime);
	TestEqual(TEXT("dictation runtime should be surfaced"), Runtime, FString(TEXT("offline_vosk")));

	FString LanguageRequested;
	DictationObject->TryGetStringField(TEXT("language_requested"), LanguageRequested);
	TestEqual(TEXT("dictation requested language should be machine-readable"), LanguageRequested, FString(TEXT("ru_ru")));

	FString LanguageEffective;
	DictationObject->TryGetStringField(TEXT("language_effective"), LanguageEffective);
	TestEqual(TEXT("dictation effective language should be machine-readable"), LanguageEffective, FString(TEXT("ru-RU")));

	FString ModelName;
	DictationObject->TryGetStringField(TEXT("model_name"), ModelName);
	TestEqual(TEXT("dictation model should be machine-readable"), ModelName, FString(TEXT("vosk-model-small-ru-0.22")));

	FString SupportClaim;
	DictationObject->TryGetStringField(TEXT("support_claim"), SupportClaim);
	TestEqual(TEXT("dictation support claim should stay truthful"), SupportClaim, FString(TEXT("best_effort_offline_small_model")));
	AddInfo(FString::Printf(
		TEXT("plugin_settings dictation readback: language_effective=%s model_name=%s support_claim=%s"),
		*LanguageEffective,
		*ModelName,
		*SupportClaim));

	bool bSupportsRussianOffline = false;
	DictationObject->TryGetBoolField(TEXT("supports_russian_offline"), bSupportsRussianOffline);
	TestTrue(TEXT("readback should surface Russian offline support"), bSupportsRussianOffline);
	return true;
}

#endif
