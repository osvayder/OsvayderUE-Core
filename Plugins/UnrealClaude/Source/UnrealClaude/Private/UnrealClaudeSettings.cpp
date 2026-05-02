// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeSettings.h"

#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "UnrealClaudeConstants.h"

namespace
{
	struct FDictationLanguageSpec
	{
		EUnrealClaudeDictationLanguage Language = EUnrealClaudeDictationLanguage::EnglishUs;
		const TCHAR* ModeName = TEXT("en_us");
		const TCHAR* LanguageTag = TEXT("en-US");
		const TCHAR* DisplayName = TEXT("English (US)");
		const TCHAR* ModelName = UnrealClaudeConstants::VoiceDictation::OfflineEnglishModelName;
		const TCHAR* ModelUrl = UnrealClaudeConstants::VoiceDictation::OfflineEnglishModelUrl;
	};

	const FDictationLanguageSpec& GetDictationLanguageSpec(const EUnrealClaudeDictationLanguage Language)
	{
		static const FDictationLanguageSpec EnglishSpec {
			EUnrealClaudeDictationLanguage::EnglishUs,
			TEXT("en_us"),
			TEXT("en-US"),
			TEXT("English (US)"),
			UnrealClaudeConstants::VoiceDictation::OfflineEnglishModelName,
			UnrealClaudeConstants::VoiceDictation::OfflineEnglishModelUrl
		};

		static const FDictationLanguageSpec RussianSpec {
			EUnrealClaudeDictationLanguage::RussianRu,
			TEXT("ru_ru"),
			TEXT("ru-RU"),
			TEXT("Russian"),
			UnrealClaudeConstants::VoiceDictation::OfflineRussianModelName,
			UnrealClaudeConstants::VoiceDictation::OfflineRussianModelUrl
		};

		return Language == EUnrealClaudeDictationLanguage::RussianRu ? RussianSpec : EnglishSpec;
	}

	EUnrealClaudeDictationLanguage ResolveEffectiveDictationLanguage(const EUnrealClaudeDictationLanguage RequestedLanguage)
	{
		if (RequestedLanguage != EUnrealClaudeDictationLanguage::Auto)
		{
			return RequestedLanguage;
		}

		const FCultureRef Culture = FInternationalization::Get().GetCurrentCulture();
		const FString CultureName = Culture->GetName();
		return CultureName.StartsWith(TEXT("ru"), ESearchCase::IgnoreCase)
			? EUnrealClaudeDictationLanguage::RussianRu
			: EUnrealClaudeDictationLanguage::EnglishUs;
	}
}

UUnrealClaudeSettings::UUnrealClaudeSettings()
{
	// Set default paths based on common installation
	OsvayderEyeServerPath.FilePath = TEXT("D:/VibeCode/OsvayderEye/server.py");
	OsvayderEyePythonPath.FilePath = TEXT("C:/Python313/python.exe");
}

FString UUnrealClaudeSettings::GetConfiguredDictationLanguageModeName() const
{
	switch (GetConfiguredDictationLanguage())
	{
	case EUnrealClaudeDictationLanguage::EnglishUs:
		return TEXT("en_us");

	case EUnrealClaudeDictationLanguage::RussianRu:
		return TEXT("ru_ru");

	case EUnrealClaudeDictationLanguage::Auto:
	default:
		return TEXT("auto");
	}
}

FString UUnrealClaudeSettings::GetEffectiveDictationLanguageTag() const
{
	return GetDictationLanguageSpec(ResolveEffectiveDictationLanguage(GetConfiguredDictationLanguage())).LanguageTag;
}

FString UUnrealClaudeSettings::GetEffectiveDictationLanguageDisplayName() const
{
	return GetDictationLanguageSpec(ResolveEffectiveDictationLanguage(GetConfiguredDictationLanguage())).DisplayName;
}

FString UUnrealClaudeSettings::GetEffectiveDictationModelName() const
{
	return GetDictationLanguageSpec(ResolveEffectiveDictationLanguage(GetConfiguredDictationLanguage())).ModelName;
}

FString UUnrealClaudeSettings::GetEffectiveDictationModelUrl() const
{
	return GetDictationLanguageSpec(ResolveEffectiveDictationLanguage(GetConfiguredDictationLanguage())).ModelUrl;
}

FString UUnrealClaudeSettings::GetDictationRuntimeLabel() const
{
	return TEXT("offline_vosk");
}

bool UUnrealClaudeSettings::SupportsRussianOfflineDictation() const
{
	return true;
}

FString UUnrealClaudeSettings::GetDictationSupportClaimName() const
{
	return TEXT("best_effort_offline_small_model");
}

FString UUnrealClaudeSettings::GetDictationSupportDetail() const
{
	return TEXT("Dictation uses lightweight offline Vosk models for interactive editor use: Russian uses vosk-model-small-ru-0.22 and English uses vosk-model-small-en-us-0.15. Transcripts remain best-effort and should be reviewed before send.");
}
