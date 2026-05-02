// Copyright Natali Caggiano. All Rights Reserved.

#include "SClaudeToolbar.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "UnrealClaude"

const TArray<FClaudeToolbarActionDescriptor>& SClaudeToolbar::GetActionDescriptors()
{
	static const TArray<FClaudeToolbarActionDescriptor> Descriptors =
	{
		{ EClaudeToolbarActionId::UE57Context, TEXT("ue57_context"), LOCTEXT("UE57Context", "UE5.7 Context"), true, false, false },
		{ EClaudeToolbarActionId::ProjectContext, TEXT("project_context"), LOCTEXT("ProjectContext", "Project Context"), true, false, false },
		{ EClaudeToolbarActionId::RefreshContext, TEXT("refresh_context"), LOCTEXT("RefreshContext", "Refresh Context"), false, true, false },
		{ EClaudeToolbarActionId::RestoreSession, TEXT("restore_session"), LOCTEXT("RestoreSession", "Restore Session"), false, true, false },
		{ EClaudeToolbarActionId::RestartSurvival, TEXT("restart_survival"), LOCTEXT("RestartSurvival", "Restart Survival"), false, true, false },
		{ EClaudeToolbarActionId::DeferredTasks, TEXT("deferred_tasks"), LOCTEXT("DeferredTasks", "Отложенные задачи"), false, true, false },
		{ EClaudeToolbarActionId::BrowserVerify, TEXT("browser_verify"), LOCTEXT("BrowserVerify", "Browser Verify"), false, true, false },
		{ EClaudeToolbarActionId::NewSession, TEXT("new_session"), LOCTEXT("NewSession", "New Session"), false, true, true },
		{ EClaudeToolbarActionId::CopyLast, TEXT("copy_last"), LOCTEXT("Copy", "Copy Last"), false, true, false }
	};
	return Descriptors;
}

void SClaudeToolbar::Construct(const FArguments& InArgs)
{
	TitleText = InArgs._TitleText;
	ProviderSummaryText = InArgs._ProviderSummaryText;
	ProviderSummaryToolTip = InArgs._ProviderSummaryToolTip;
	bUE57ContextEnabled = InArgs._bUE57ContextEnabled;
	bProjectContextEnabled = InArgs._bProjectContextEnabled;
	bRestoreEnabled = InArgs._bRestoreEnabled;
	bRestartSurvivalEnabled = InArgs._bRestartSurvivalEnabled;
	bShowBrowserVerify = InArgs._bShowBrowserVerify;
	bBrowserVerifyEnabled = InArgs._bBrowserVerifyEnabled;
	DeferredTasksCount = InArgs._DeferredTasksCount;
	RestoreToolTip = InArgs._RestoreToolTip;
	RestartSurvivalToolTip = InArgs._RestartSurvivalToolTip;
	BrowserVerifyToolTip = InArgs._BrowserVerifyToolTip;
	OnUE57ContextChanged = InArgs._OnUE57ContextChanged;
	OnProjectContextChanged = InArgs._OnProjectContextChanged;
	OnRefreshContext = InArgs._OnRefreshContext;
	OnRestoreSession = InArgs._OnRestoreSession;
	OnRestartSurvival = InArgs._OnRestartSurvival;
	OnOpenDeferredTasks = InArgs._OnOpenDeferredTasks;
	OnBrowserVerify = InArgs._OnBrowserVerify;
	OnNewSession = InArgs._OnNewSession;
	OnCopyLast = InArgs._OnCopyLast;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.0f, 4.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildToolbarHeader()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				BuildPrimaryActionStrip()
			]
		]
	];
}

TArray<SClaudeToolbar::FToolbarActionEntry> SClaudeToolbar::BuildToolbarActions()
{
	TMap<EClaudeToolbarActionId, FClaudeToolbarActionDescriptor> DescriptorsById;
	for (const FClaudeToolbarActionDescriptor& Descriptor : GetActionDescriptors())
	{
		DescriptorsById.Add(Descriptor.Id, Descriptor);
	}

	const auto DescriptorFor = [&DescriptorsById](const EClaudeToolbarActionId Id)
	{
		return DescriptorsById.FindChecked(Id);
	};
	const auto AlwaysVisible = []()
	{
		return TAttribute<EVisibility>(EVisibility::Visible);
	};
	const auto Enabled = [](const bool bValue)
	{
		return TAttribute<bool>(bValue);
	};

	TArray<FToolbarActionEntry> Actions;
	Actions.Reserve(GetActionDescriptors().Num());

	Actions.Add({
		DescriptorFor(EClaudeToolbarActionId::RefreshContext),
		EToolbarActionWidgetKind::Button,
		LOCTEXT("RefreshContextTip", "Refresh cached project stats, classes, actors, assets, and project memory docs"),
		Enabled(true),
		AlwaysVisible(),
		[this]() { OnRefreshContext.ExecuteIfBound(); return FReply::Handled(); }
	});

	Actions.Add({
		DescriptorFor(EClaudeToolbarActionId::RestoreSession),
		EToolbarActionWidgetKind::Button,
		RestoreToolTip,
		TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateLambda([this]() { return bRestoreEnabled.Get(); })),
		AlwaysVisible(),
		[this]() { OnRestoreSession.ExecuteIfBound(); return FReply::Handled(); }
	});

	Actions.Add({
		DescriptorFor(EClaudeToolbarActionId::RestartSurvival),
		EToolbarActionWidgetKind::Button,
		RestartSurvivalToolTip,
		TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateLambda([this]() { return bRestartSurvivalEnabled.Get(); })),
		AlwaysVisible(),
		[this]() { OnRestartSurvival.ExecuteIfBound(); return FReply::Handled(); }
	});

	Actions.Add({
		DescriptorFor(EClaudeToolbarActionId::DeferredTasks),
		EToolbarActionWidgetKind::DeferredTasksButton,
		LOCTEXT("DeferredTasksTip", "Открыть отложенные задачи из восстановления и продолжить или удалить их"),
		Enabled(true),
		AlwaysVisible(),
		[this]() { OnOpenDeferredTasks.ExecuteIfBound(); return FReply::Handled(); }
	});

	Actions.Add({
		DescriptorFor(EClaudeToolbarActionId::BrowserVerify),
		EToolbarActionWidgetKind::Button,
		BrowserVerifyToolTip,
		TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateLambda([this]() { return bBrowserVerifyEnabled.Get(); })),
		TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda([this]()
		{
			return bShowBrowserVerify.Get() ? EVisibility::Visible : EVisibility::Collapsed;
		})),
		[this]() { OnBrowserVerify.ExecuteIfBound(); return FReply::Handled(); }
	});

	Actions.Add({
		DescriptorFor(EClaudeToolbarActionId::NewSession),
		EToolbarActionWidgetKind::Button,
		LOCTEXT("NewSessionTip", "Start a fresh session for the active provider. Clears visible chat, that provider's saved session history, and that provider's backend conversation state"),
		Enabled(true),
		AlwaysVisible(),
		[this]() { OnNewSession.ExecuteIfBound(); return FReply::Handled(); }
	});

	Actions.Add({
		DescriptorFor(EClaudeToolbarActionId::CopyLast),
		EToolbarActionWidgetKind::Button,
		LOCTEXT("CopyTip", "Copy last response to clipboard"),
		Enabled(true),
		AlwaysVisible(),
		[this]() { OnCopyLast.ExecuteIfBound(); return FReply::Handled(); }
	});

	return Actions;
}

TSharedRef<SWidget> SClaudeToolbar::BuildToolbarHeader()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(TitleText)
			.TextStyle(FAppStyle::Get(), "LargeText")
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.MaxDesiredWidth(360.0f)
			[
				SNew(STextBlock)
				.Text(ProviderSummaryText)
				.TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.65f)))
				.ToolTipText(ProviderSummaryToolTip)
				.Clipping(EWidgetClipping::ClipToBounds)
				.Visibility_Lambda([this]()
				{
					return ProviderSummaryText.Get().IsEmpty()
						? EVisibility::Collapsed
						: EVisibility::Visible;
				})
			]
		];
}

TSharedRef<SWidget> SClaudeToolbar::BuildPrimaryActionStrip()
{
	const TArray<FToolbarActionEntry> Actions = BuildToolbarActions();
	TSharedRef<SHorizontalBox> ActionRow = SNew(SHorizontalBox);

	for (const FToolbarActionEntry& Action : Actions)
	{
		if (!Action.Descriptor.bPrimaryToolbarAction)
		{
			continue;
		}

		ActionRow->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(4.0f, 0.0f))
		[
			BuildActionWidget(Action)
		];
	}

	ActionRow->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(FMargin(8.0f, 0.0f, 4.0f, 0.0f))
	[
		SNew(STextBlock)
		.Text(LOCTEXT("MandatoryContextStatus", "Context: UE + Project"))
		.TextStyle(FAppStyle::Get(), "SmallText")
		.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.75f, 0.95f)))
		.ToolTipText(LOCTEXT("MandatoryContextStatusTip", "UE5.7 and project context are mandatory and always included in normal assistant runs."))
	];

	ActionRow->AddSlot()
	.FillWidth(1.0f)
	[
		SNew(SSpacer)
	];

	ActionRow->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(FMargin(4.0f, 0.0f))
	[
		BuildOverflowMenuButton(Actions)
	];

	return ActionRow;
}

TSharedRef<SWidget> SClaudeToolbar::BuildOverflowMenuButton(const TArray<FToolbarActionEntry>& Actions)
{
	return SNew(SComboButton)
		.ToolTipText(LOCTEXT("MoreActionsTip", "Show lower-frequency and advanced assistant actions"))
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("MoreActions", "More"))
		]
		.MenuContent()
		[
			BuildOverflowMenuContent(Actions)
		];
}

TSharedRef<SWidget> SClaudeToolbar::BuildOverflowMenuContent(const TArray<FToolbarActionEntry>& Actions)
{
	TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);

	Menu->AddSlot()
	.AutoHeight()
	.Padding(FMargin(8.0f, 6.0f, 8.0f, 2.0f))
	[
		SNew(STextBlock)
		.Text(LOCTEXT("AdvancedActionsHeader", "Advanced"))
		.TextStyle(FAppStyle::Get(), "SmallText")
		.ColorAndOpacity(FSlateColor(FLinearColor(0.65f, 0.65f, 0.7f)))
	];

	for (const FToolbarActionEntry& Action : Actions)
	{
		if (Action.Descriptor.bPrimaryToolbarAction)
		{
			continue;
		}

		Menu->AddSlot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 2.0f))
		[
			SNew(SBox)
			.MinDesiredWidth(220.0f)
			[
				BuildActionWidget(Action)
			]
		];
	}

	return Menu;
}

TSharedRef<SWidget> SClaudeToolbar::BuildActionWidget(const FToolbarActionEntry& Action)
{
	if (Action.WidgetKind == EToolbarActionWidgetKind::DeferredTasksButton)
	{
		return BuildDeferredTasksWidget(Action);
	}

	if (Action.WidgetKind == EToolbarActionWidgetKind::CheckBox)
	{
		return SNew(SCheckBox)
			.IsChecked_Lambda([Action]()
			{
				return Action.GetCheckState ? Action.GetCheckState() : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([Action](ECheckBoxState NewState)
			{
				if (Action.SetCheckState)
				{
					Action.SetCheckState(NewState);
				}
			})
			.ToolTipText(Action.ToolTip)
			.IsEnabled(Action.bEnabled)
			.Visibility(Action.Visibility)
			[
				SNew(STextBlock)
				.Text(Action.Descriptor.Label)
			];
	}

	return SNew(SButton)
		.ToolTipText(Action.ToolTip)
		.IsEnabled(Action.bEnabled)
		.Visibility(Action.Visibility)
		.OnClicked_Lambda([Action]()
		{
			return Action.Execute ? Action.Execute() : FReply::Handled();
		})
		[
			SNew(STextBlock)
			.Text(Action.Descriptor.Label)
		];
}

TSharedRef<SWidget> SClaudeToolbar::BuildDeferredTasksWidget(const FToolbarActionEntry& Action)
{
	return SNew(SButton)
		.ToolTipText(Action.ToolTip)
		.IsEnabled(Action.bEnabled)
		.Visibility(Action.Visibility)
		.OnClicked_Lambda([Action]()
		{
			return Action.Execute ? Action.Execute() : FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Recent"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Action.Descriptor.Label)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(FMargin(6.0f, 1.0f))
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return FText::AsNumber(FMath::Max(0, DeferredTasksCount.Get()));
					})
					.TextStyle(FAppStyle::Get(), "SmallText")
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
