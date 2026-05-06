// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUESettings.h"

#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "OsvayderUEConstants.h"

namespace
{
	struct FDictationLanguageSpec
	{
		EOsvayderUEDictationLanguage Language = EOsvayderUEDictationLanguage::EnglishUs;
		const TCHAR* ModeName = TEXT("en_us");
		const TCHAR* LanguageTag = TEXT("en-US");
		const TCHAR* DisplayName = TEXT("English (US)");
		const TCHAR* ModelName = OsvayderUEConstants::VoiceDictation::OfflineEnglishModelName;
		const TCHAR* ModelUrl = OsvayderUEConstants::VoiceDictation::OfflineEnglishModelUrl;
	};

	const FDictationLanguageSpec& GetDictationLanguageSpec(const EOsvayderUEDictationLanguage Language)
	{
		static const FDictationLanguageSpec EnglishSpec {
			EOsvayderUEDictationLanguage::EnglishUs,
			TEXT("en_us"),
			TEXT("en-US"),
			TEXT("English (US)"),
			OsvayderUEConstants::VoiceDictation::OfflineEnglishModelName,
			OsvayderUEConstants::VoiceDictation::OfflineEnglishModelUrl
		};

		static const FDictationLanguageSpec RussianSpec {
			EOsvayderUEDictationLanguage::RussianRu,
			TEXT("ru_ru"),
			TEXT("ru-RU"),
			TEXT("Russian"),
			OsvayderUEConstants::VoiceDictation::OfflineRussianModelName,
			OsvayderUEConstants::VoiceDictation::OfflineRussianModelUrl
		};

		return Language == EOsvayderUEDictationLanguage::RussianRu ? RussianSpec : EnglishSpec;
	}

	EOsvayderUEDictationLanguage ResolveEffectiveDictationLanguage(const EOsvayderUEDictationLanguage RequestedLanguage)
	{
		if (RequestedLanguage != EOsvayderUEDictationLanguage::Auto)
		{
			return RequestedLanguage;
		}

		const FCultureRef Culture = FInternationalization::Get().GetCurrentCulture();
		const FString CultureName = Culture->GetName();
		return CultureName.StartsWith(TEXT("ru"), ESearchCase::IgnoreCase)
			? EOsvayderUEDictationLanguage::RussianRu
			: EOsvayderUEDictationLanguage::EnglishUs;
	}
}

UOsvayderUESettings::UOsvayderUESettings()
{
	OsvayderEyeServerPath.FilePath = FString();
	OsvayderEyePythonPath.FilePath = TEXT("python");
}

FString UOsvayderUESettings::GetConfiguredDictationLanguageModeName() const
{
	switch (GetConfiguredDictationLanguage())
	{
	case EOsvayderUEDictationLanguage::EnglishUs:
		return TEXT("en_us");

	case EOsvayderUEDictationLanguage::RussianRu:
		return TEXT("ru_ru");

	case EOsvayderUEDictationLanguage::Auto:
	default:
		return TEXT("auto");
	}
}

FString UOsvayderUESettings::GetEffectiveDictationLanguageTag() const
{
	return GetDictationLanguageSpec(ResolveEffectiveDictationLanguage(GetConfiguredDictationLanguage())).LanguageTag;
}

FString UOsvayderUESettings::GetEffectiveDictationLanguageDisplayName() const
{
	return GetDictationLanguageSpec(ResolveEffectiveDictationLanguage(GetConfiguredDictationLanguage())).DisplayName;
}

FString UOsvayderUESettings::GetEffectiveDictationModelName() const
{
	return GetDictationLanguageSpec(ResolveEffectiveDictationLanguage(GetConfiguredDictationLanguage())).ModelName;
}

FString UOsvayderUESettings::GetEffectiveDictationModelUrl() const
{
	return GetDictationLanguageSpec(ResolveEffectiveDictationLanguage(GetConfiguredDictationLanguage())).ModelUrl;
}

FString UOsvayderUESettings::GetDictationRuntimeLabel() const
{
	return TEXT("offline_vosk");
}

bool UOsvayderUESettings::SupportsRussianOfflineDictation() const
{
	return true;
}

FString UOsvayderUESettings::GetDictationSupportClaimName() const
{
	return TEXT("best_effort_offline_small_model");
}

FString UOsvayderUESettings::GetDictationSupportDetail() const
{
	return TEXT("Dictation uses lightweight offline Vosk models for interactive editor use: Russian uses vosk-model-small-ru-0.22 and English uses vosk-model-small-en-us-0.15. Transcripts remain best-effort and should be reviewed before send.");
}
