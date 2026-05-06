// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUESettingsDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "OsvayderUESettings.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "OsvayderUESettingsDetails"

namespace
{
	int32 GetCategorySortOrder(const FName CategoryName)
	{
		if (CategoryName == TEXT("General"))
		{
			return 0;
		}

		if (CategoryName == TEXT("Verification"))
		{
			return 2;
		}

		if (CategoryName == TEXT("Dictation"))
		{
			return 1;
		}

		if (CategoryName == TEXT("Architecture"))
		{
			return 3;
		}

		if (CategoryName == TEXT("Paths"))
		{
			return 4;
		}

		if (CategoryName == TEXT("MCP"))
		{
			return 5;
		}

		if (CategoryName == TEXT("OsvayderEye"))
		{
			return 6;
		}

		if (CategoryName == TEXT("Codex"))
		{
			return 7;
		}

		if (CategoryName == TEXT("Claude"))
		{
			return 8;
		}

		if (CategoryName == TEXT("Logging"))
		{
			return 9;
		}

		if (CategoryName == TEXT("Mutation Lifecycle"))
		{
			return 10;
		}

		return 500;
	}

	EOsvayderUECodexAuthMode GetAuthModeValue(const TSharedPtr<IPropertyHandle>& PropertyHandle)
	{
		if (!PropertyHandle.IsValid())
		{
			return EOsvayderUECodexAuthMode::Auto;
		}

		uint8 RawValue = 0;
		if (PropertyHandle->GetValue(RawValue) == FPropertyAccess::Success)
		{
			return static_cast<EOsvayderUECodexAuthMode>(RawValue);
		}

		return EOsvayderUECodexAuthMode::Auto;
	}

	EOsvayderUECodexWorkMode GetWorkModeValue(const TSharedPtr<IPropertyHandle>& PropertyHandle)
	{
		if (!PropertyHandle.IsValid())
		{
			return EOsvayderUECodexWorkMode::Balanced;
		}

		uint8 RawValue = 0;
		if (PropertyHandle->GetValue(RawValue) == FPropertyAccess::Success)
		{
			return static_cast<EOsvayderUECodexWorkMode>(RawValue);
		}

		return EOsvayderUECodexWorkMode::Balanced;
	}

	TAttribute<EVisibility> MakeAuthModeVisibility(
		const TSharedPtr<IPropertyHandle>& PropertyHandle,
		const EOsvayderUECodexAuthMode ExpectedMode,
		const bool bInvert = false)
	{
		return TAttribute<EVisibility>::CreateLambda([PropertyHandle, ExpectedMode, bInvert]()
		{
			const bool bMatches = GetAuthModeValue(PropertyHandle) == ExpectedMode;
			return (bInvert ? !bMatches : bMatches) ? EVisibility::Visible : EVisibility::Collapsed;
		});
	}

	TAttribute<EVisibility> MakeWorkModeNotCustomVisibility(const TSharedPtr<IPropertyHandle>& PropertyHandle)
	{
		return TAttribute<EVisibility>::CreateLambda([PropertyHandle]()
		{
			return GetWorkModeValue(PropertyHandle) == EOsvayderUECodexWorkMode::Custom
				? EVisibility::Collapsed
				: EVisibility::Visible;
		});
	}

	TAttribute<FText> MakeAuthModeDescription(const TSharedPtr<IPropertyHandle>& PropertyHandle)
	{
		return TAttribute<FText>::CreateLambda([PropertyHandle]()
		{
			switch (GetAuthModeValue(PropertyHandle))
			{
			case EOsvayderUECodexAuthMode::ApiKeyEnvVar:
				return LOCTEXT("AuthModeApiNote", "API mode reads one editor env var and forwards it to Codex as OPENAI_API_KEY.");

			case EOsvayderUECodexAuthMode::CliTerminal:
				return LOCTEXT("AuthModeCliNote", "CLI Terminal expects that you already completed codex login outside the editor.");

			case EOsvayderUECodexAuthMode::BrowserVerify:
				return LOCTEXT("AuthModeBrowserNote", "Browser Verify asks Codex app-server for a login URL and opens it from Unreal.");

			case EOsvayderUECodexAuthMode::Auto:
			default:
				return LOCTEXT("AuthModeAutoNote", "Auto keeps the accepted default path: inherited OPENAI_API_KEY first, then shared Codex login from the resolved home.");
			}
		});
	}
}

TSharedRef<IDetailCustomization> FOsvayderUESettingsDetails::MakeInstance()
{
	return MakeShared<FOsvayderUESettingsDetails>();
}

void FOsvayderUESettingsDetails::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
{
	SortCategories(DetailLayout);

	const TSharedPtr<IPropertyHandle> PreferredBackend = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, PreferredBackend));
	const TSharedPtr<IPropertyHandle> ScopeMode = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, ScopeMode));
	const TSharedPtr<IPropertyHandle> AutoRestoreSession = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, bAutoRestoreSessionOnOpen));
	const TSharedPtr<IPropertyHandle> DictationLanguage = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultDictationLanguage));
	const TSharedPtr<IPropertyHandle> ClaudeModel = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultModel));
	const TSharedPtr<IPropertyHandle> ClaudeMaxTokens = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, MaxResponseTokens));
	const TSharedPtr<IPropertyHandle> ClaudePersistentSession = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, bClaudeUsePersistentSession));
	const TSharedPtr<IPropertyHandle> ClaudeForwardLanguage = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, bClaudeForwardLanguageToSystemPrompt));
	const TSharedPtr<IPropertyHandle> ClaudeEffortLevel = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultClaudeEffortLevel));
	const TSharedPtr<IPropertyHandle> CodexModel = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultCodexModel));
	const TSharedPtr<IPropertyHandle> CodexProfile = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultCodexProfile));
	const TSharedPtr<IPropertyHandle> CodexSpeedMode = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultCodexSpeedMode));
	const TSharedPtr<IPropertyHandle> CodexWorkMode = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultCodexWorkMode));
	const TSharedPtr<IPropertyHandle> CodexReasoning = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultCodexReasoningEffort));
	const TSharedPtr<IPropertyHandle> CodexVerbosity = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultCodexVerbosity));
	const TSharedPtr<IPropertyHandle> CodexAuthMode = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultCodexAuthMode));
	const TSharedPtr<IPropertyHandle> CodexApiEnvVar = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultCodexApiKeyEnvVar));
	const TSharedPtr<IPropertyHandle> CodexBrowserProxyClear = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, bCodexBrowserVerifyClearProxyEnv));
	const TSharedPtr<IPropertyHandle> CodexExecProxyClear = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, bCodexExecClearProxyEnv));
	const TSharedPtr<IPropertyHandle> CodexPersistentAppServer = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, bCodexUsePersistentAppServer));
	const TSharedPtr<IPropertyHandle> CodexHomeOverride = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, DefaultCodexHomeOverride));
	const TSharedPtr<IPropertyHandle> AutonomousMutationMode = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UOsvayderUESettings, bAutonomousMutationMode));

	DetailLayout.HideProperty(PreferredBackend);
	DetailLayout.HideProperty(ScopeMode);
	DetailLayout.HideProperty(AutoRestoreSession);
	DetailLayout.HideProperty(DictationLanguage);
	DetailLayout.HideProperty(ClaudeModel);
	DetailLayout.HideProperty(ClaudeMaxTokens);
	DetailLayout.HideProperty(ClaudePersistentSession);
	DetailLayout.HideProperty(ClaudeForwardLanguage);
	DetailLayout.HideProperty(ClaudeEffortLevel);
	DetailLayout.HideProperty(CodexModel);
	DetailLayout.HideProperty(CodexProfile);
	DetailLayout.HideProperty(CodexSpeedMode);
	DetailLayout.HideProperty(CodexWorkMode);
	DetailLayout.HideProperty(CodexReasoning);
	DetailLayout.HideProperty(CodexVerbosity);
	DetailLayout.HideProperty(CodexAuthMode);
	DetailLayout.HideProperty(CodexApiEnvVar);
	DetailLayout.HideProperty(CodexBrowserProxyClear);
	DetailLayout.HideProperty(CodexExecProxyClear);
	DetailLayout.HideProperty(CodexPersistentAppServer);
	DetailLayout.HideProperty(CodexHomeOverride);
	DetailLayout.HideProperty(AutonomousMutationMode);

	IDetailCategoryBuilder& GeneralCategory = DetailLayout.EditCategory(TEXT("General"));
	AddInlineNote(
		GeneralCategory,
		LOCTEXT("GeneralSearch", "Everyday defaults"),
		LOCTEXT("GeneralBody", "Start here for day-to-day defaults. Backend-specific settings stay grouped below."));
	GeneralCategory.AddProperty(PreferredBackend);
	GeneralCategory.AddProperty(AutoRestoreSession);
	GeneralCategory.AddProperty(ScopeMode);

	IDetailCategoryBuilder& DictationCategory = DetailLayout.EditCategory(TEXT("Dictation"));
	AddInlineNote(
		DictationCategory,
		LOCTEXT("DictationSearch", "Offline dictation language"),
		LOCTEXT("DictationBody", "Choose the spoken language for the bounded offline dictation flow. The active language and model are surfaced in the assistant debug snapshot and practical proof receipts, and the current Russian lane remains best-effort even though it now uses a larger desktop Vosk model."));
	AddGroupHeader(
		DictationCategory,
		LOCTEXT("DictationLanguageHeader", "Language and model"),
		LOCTEXT("DictationLanguageBody", "Russian uses the bounded lightweight Vosk model `vosk-model-small-ru-0.22`. English uses `vosk-model-small-en-us-0.15`. Both remain offline paths optimized for interaction, so transcripts should still be reviewed before send."));
	DictationCategory.AddProperty(DictationLanguage);

	IDetailCategoryBuilder& ClaudeCategory = DetailLayout.EditCategory(TEXT("Claude"));
	AddInlineNote(
		ClaudeCategory,
		LOCTEXT("ClaudeSearch", "Claude backend only"),
		LOCTEXT("ClaudeBody", "These options only affect the Claude backend and stay separate from Codex auth, proxy, and runtime controls."));
	ClaudeCategory.AddProperty(ClaudeModel);
	ClaudeCategory.AddProperty(ClaudeMaxTokens);
	AddGroupHeader(
		ClaudeCategory,
		LOCTEXT("ClaudeEffortHeader", "Effort"),
		LOCTEXT("ClaudeEffortBody", "Reasoning depth forwarded to Claude CLI via --effort. 'Default' leaves it to the CLI (current default on Opus 4.7). Mirrors the Codex Speed/Work-Mode dropdown pattern."));
	ClaudeCategory.AddProperty(ClaudeEffortLevel);
	AddGroupHeader(
		ClaudeCategory,
		LOCTEXT("ClaudeRuntimeHeader", "Runtime and session"),
		LOCTEXT("ClaudeRuntimeBody", "These options control how Osvayder UE keeps Claude CLI conversation memory alive across prompts and whether the system prompt carries a spoken-language directive."));
	ClaudeCategory.AddProperty(ClaudePersistentSession);
	ClaudeCategory.AddProperty(ClaudeForwardLanguage);

	IDetailCategoryBuilder& CodexCategory = DetailLayout.EditCategory(TEXT("Codex"));
	AddInlineNote(
		CodexCategory,
		LOCTEXT("CodexSearch", "Codex everyday defaults"),
		LOCTEXT("CodexBody", "Codex settings are grouped by what a normal user usually decides: model, work style, authentication, network, and runtime behavior."));

	AddGroupHeader(
		CodexCategory,
		LOCTEXT("CodexBasicsHeader", "Model and profile"),
		LOCTEXT("CodexBasicsBody", "Choose which Codex model and optional CLI profile Osvayder UE should use."));
	CodexCategory.AddProperty(CodexModel);
	CodexCategory.AddProperty(CodexProfile);

	AddGroupHeader(
		CodexCategory,
		LOCTEXT("CodexWorkHeader", "Work and speed"),
		LOCTEXT("CodexWorkBody", "Use Work Mode for the normal preset. Only switch to Custom when you need manual reasoning and response-detail overrides."));
	CodexCategory.AddProperty(CodexSpeedMode);
	CodexCategory.AddProperty(CodexWorkMode);
	CodexCategory.AddProperty(CodexReasoning);
	CodexCategory.AddProperty(CodexVerbosity);
	AddInlineNote(
		CodexCategory,
		LOCTEXT("CodexCustomOnlySearch", "Custom work mode"),
		LOCTEXT("CodexCustomOnlyBody", "Reasoning and Response Detail stay disabled until Work Mode is set to Custom."),
		MakeWorkModeNotCustomVisibility(CodexWorkMode));

	AddGroupHeader(
		CodexCategory,
		LOCTEXT("CodexAuthHeader", "Authentication"),
		LOCTEXT("CodexAuthBody", "Pick the login path you actually use on this machine. Mode-specific fields below stay visible but explain why they may be disabled."));
	CodexCategory.AddProperty(CodexAuthMode);
	CodexCategory.AddCustomRow(LOCTEXT("CodexAuthModeDescriptionSearch", "Authentication mode description"), false)
	.WholeRowContent()
	[
		SNew(STextBlock)
		.Text(MakeAuthModeDescription(CodexAuthMode))
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.AutoWrapText(true)
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
	];
	CodexCategory.AddProperty(CodexApiEnvVar);
	AddInlineNote(
		CodexCategory,
		LOCTEXT("CodexApiEnvVarSearch", "API mode only"),
		LOCTEXT("CodexApiEnvVarBody", "This field unlocks only when Authentication is set to API."),
		MakeAuthModeVisibility(CodexAuthMode, EOsvayderUECodexAuthMode::ApiKeyEnvVar, true));
	CodexCategory.AddProperty(CodexHomeOverride);
	AddInlineNote(
		CodexCategory,
		LOCTEXT("CodexHomeOverrideSearch", "Bring-up only"),
		LOCTEXT("CodexHomeOverrideBody", "Home Override is a controlled bring-up field. It stays read-only here because normal users should rely on the accepted resolution ladder."),
		EVisibility::Visible);

	AddGroupHeader(
		CodexCategory,
		LOCTEXT("CodexProxyHeader", "Network and proxy"),
		LOCTEXT("CodexProxyBody", "Normal Codex runs and Browser Verify use separate proxy-clearing controls so they do not look interchangeable."));
	CodexCategory.AddProperty(CodexBrowserProxyClear);
	AddInlineNote(
		CodexCategory,
		LOCTEXT("CodexBrowserProxySearch", "Browser Verify proxy note"),
		LOCTEXT("CodexBrowserProxyBody", "This toggle applies only when Authentication is Browser Verify."),
		MakeAuthModeVisibility(CodexAuthMode, EOsvayderUECodexAuthMode::BrowserVerify, true));
	CodexCategory.AddProperty(CodexExecProxyClear);
	AddInlineNote(
		CodexCategory,
		LOCTEXT("CodexExecProxySearch", "Normal runs proxy note"),
		LOCTEXT("CodexExecProxyBody", "This toggle affects ordinary Codex runs. Browser Verify keeps its own separate switch above."),
		EVisibility::Visible);

	AddGroupHeader(
		CodexCategory,
		LOCTEXT("CodexRuntimeHeader", "Runtime and session"),
		LOCTEXT("CodexRuntimeBody", "These options control how Osvayder UE keeps Codex conversation state alive during ordinary use."));
	CodexCategory.AddProperty(CodexPersistentAppServer);

	IDetailCategoryBuilder& MutationLifecycleCategory = DetailLayout.EditCategory(TEXT("Mutation Lifecycle"));
	AddInlineNote(
		MutationLifecycleCategory,
		LOCTEXT("MutationLifecycleSearch", "Autonomous mutation persistence"),
		LOCTEXT("MutationLifecycleBody", "Default is on: every successful MCP mutation tool saves its affected asset package to disk before returning. Disable globally to leave mutations dirty in memory for manual save, or pass auto_save=false on a single tool call for a per-call override."));
	MutationLifecycleCategory.AddProperty(AutonomousMutationMode);
}

void FOsvayderUESettingsDetails::AddGroupHeader(
	IDetailCategoryBuilder& Category,
	const FText& Title,
	const FText& Body)
{
	Category.AddCustomRow(Title, false)
	.WholeRowContent()
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(Title)
			.Font(IDetailLayoutBuilder::GetDetailFontBold())
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 2.0f, 0.0f, 6.0f)
		[
			SNew(STextBlock)
			.Text(Body)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
	];
}

void FOsvayderUESettingsDetails::AddInlineNote(
	IDetailCategoryBuilder& Category,
	const FText& SearchText,
	const FText& Body,
	const TAttribute<EVisibility>& Visibility)
{
	Category.AddCustomRow(SearchText, false)
	.Visibility(Visibility)
	.WholeRowContent()
	[
		SNew(STextBlock)
		.Text(Body)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.AutoWrapText(true)
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
	];
}

void FOsvayderUESettingsDetails::SortCategories(IDetailLayoutBuilder& DetailLayout)
{
	DetailLayout.SortCategories([](const TMap<FName, IDetailCategoryBuilder*>& CategoryMap)
	{
		for (const TPair<FName, IDetailCategoryBuilder*>& Pair : CategoryMap)
		{
			if (Pair.Value)
			{
				Pair.Value->SetSortOrder(GetCategorySortOrder(Pair.Key));
			}
		}
	});
}

#undef LOCTEXT_NAMESPACE
