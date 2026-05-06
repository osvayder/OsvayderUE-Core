// Copyright Natali Caggiano. All Rights Reserved.

#include "SOsvayderToolbar.h"
#include "OsvayderSlateStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "OsvayderUE"

const TArray<FClaudeToolbarActionDescriptor>& SOsvayderToolbar::GetActionDescriptors()
{
	static const TArray<FClaudeToolbarActionDescriptor> Descriptors =
	{
		{ EClaudeToolbarActionId::UE57Context, TEXT("ue57_context"), LOCTEXT("UE57Context", "UE5.7 Context"), true, false, false },
		{ EClaudeToolbarActionId::ProjectContext, TEXT("project_context"), LOCTEXT("ProjectContext", "Project Context"), true, false, false },
		{ EClaudeToolbarActionId::RefreshContext, TEXT("refresh_context"), LOCTEXT("RefreshContext", "Refresh Context"), false, false, false },
		{ EClaudeToolbarActionId::RestoreSession, TEXT("restore_session"), LOCTEXT("RestoreSession", "Restore Session"), false, false, false },
		{ EClaudeToolbarActionId::RestartSurvival, TEXT("restart_survival"), LOCTEXT("RestartSurvival", "Restart Survival"), false, false, false },
		{ EClaudeToolbarActionId::DeferredTasks, TEXT("deferred_tasks"), LOCTEXT("DeferredTasks", "Deferred Tasks"), false, true, false },
		{ EClaudeToolbarActionId::BrowserVerify, TEXT("browser_verify"), LOCTEXT("BrowserVerify", "Browser Verify"), false, false, false },
		{ EClaudeToolbarActionId::NewSession, TEXT("new_session"), LOCTEXT("NewSession", "New Session"), false, true, true },
		{ EClaudeToolbarActionId::CopyLast, TEXT("copy_last"), LOCTEXT("Copy", "Copy Last"), false, false, false }
	};
	return Descriptors;
}

FText SOsvayderToolbar::GetDeferredTasksMenuLabel()
{
	return LOCTEXT("DeferredTasksMenuLabel", "Deferred Tasks");
}

void SOsvayderToolbar::Construct(const FArguments& InArgs)
{
	TitleText = InArgs._TitleText;
	ProviderSummaryText = InArgs._ProviderSummaryText;
	ProviderSummaryToolTip = InArgs._ProviderSummaryToolTip;
	ModelText = InArgs._ModelText;
	ModelToolTip = InArgs._ModelToolTip;
	ProfileText = InArgs._ProfileText;
	ProfileToolTip = InArgs._ProfileToolTip;
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
		.BorderImage(OsvayderSlateStyle::ToolbarPanelBrush())
		.Padding(OsvayderSlateStyle::PanelPadding())
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildToolbarHeader()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, OsvayderSlateStyle::Metrics::ToolbarGap, 0.0f, 0.0f)
			[
				BuildPrimaryActionStrip()
			]
		]
	];
}

TArray<SOsvayderToolbar::FToolbarActionEntry> SOsvayderToolbar::BuildToolbarActions()
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
		LOCTEXT("DeferredTasksTip", "Open deferred tasks to continue or dismiss them"),
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

TSharedRef<SWidget> SOsvayderToolbar::BuildToolbarHeader()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SBorder)
					.BorderImage(OsvayderSlateStyle::VioletChipBrush())
					.Padding(FMargin(7.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("InfinityMark", "\u221e"))
						.TextStyle(FAppStyle::Get(), "LargeText")
						.ColorAndOpacity(OsvayderSlateStyle::AccentCyanText())
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(7.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(TitleText)
					.TextStyle(FAppStyle::Get(), "LargeText")
					.ColorAndOpacity(OsvayderSlateStyle::TextPrimary())
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SWrapBox)
			.UseAllottedSize(true)
			.InnerSlotPadding(FVector2D(6.0f, 6.0f))
			+ SWrapBox::Slot()
			[
				SNew(SBox)
				.MaxDesiredWidth(460.0f)
				[
					BuildToolbarChip(
						LOCTEXT("ProviderChipLabel", "Status"),
						ProviderSummaryText,
						OsvayderSlateStyle::ChipBrush(),
						OsvayderSlateStyle::SubtleText(),
						ProviderSummaryToolTip)
				]
			]
			+ SWrapBox::Slot()
			[
				SNew(SBox)
				.MaxDesiredWidth(260.0f)
				[
					BuildToolbarChip(
						LOCTEXT("ModelChipLabel", "Model"),
						ModelText,
						OsvayderSlateStyle::ChipBrush(),
						OsvayderSlateStyle::TextPrimary(),
						ModelToolTip)
				]
			]
			+ SWrapBox::Slot()
			[
				SNew(SBox)
				.MaxDesiredWidth(220.0f)
				[
					BuildToolbarChip(
						LOCTEXT("ProfileChipLabel", "Profile"),
						ProfileText,
						OsvayderSlateStyle::ChipBrush(),
						OsvayderSlateStyle::TextPrimary(),
						ProfileToolTip)
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.HeightOverride(OsvayderSlateStyle::Metrics::AccentLineHeight)
			[
				SNew(SBorder)
				.BorderImage(OsvayderSlateStyle::AccentLineBrush())
			]
		];
}

TSharedRef<SWidget> SOsvayderToolbar::BuildToolbarChip(const FText& Label, const TAttribute<FText>& Value, const FSlateBrush* Brush, const FSlateColor& ValueColor, const TAttribute<FText>& ToolTip) const
{
	return SNew(SBorder)
		.BorderImage(Brush)
		.Padding(OsvayderSlateStyle::ChipPadding())
		.ToolTipText(ToolTip)
		.Visibility_Lambda([Value]()
		{
			return Value.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
				.TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(OsvayderSlateStyle::MutedText())
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(5.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Value)
				.TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(ValueColor)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Clipping(EWidgetClipping::ClipToBounds)
			]
		];
}

TSharedRef<SWidget> SOsvayderToolbar::BuildPrimaryActionStrip()
{
	const TArray<FToolbarActionEntry> Actions = BuildToolbarActions();
	TSharedRef<SHorizontalBox> ActionRow = SNew(SHorizontalBox);
	const FToolbarActionEntry* SingleOverflowAction = nullptr;
	int32 OverflowActionCount = 0;

	for (const FToolbarActionEntry& Action : Actions)
	{
		if (!Action.Descriptor.bPrimaryToolbarAction && Action.Descriptor.bScrollableAction)
		{
			++OverflowActionCount;
			if (OverflowActionCount == 1)
			{
				SingleOverflowAction = &Action;
			}
		}

		if (!Action.Descriptor.bPrimaryToolbarAction)
		{
			continue;
		}

		ActionRow->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, OsvayderSlateStyle::Metrics::ToolbarGap, 0.0f))
		[
			BuildActionWidget(Action)
		];
	}

	ActionRow->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(FMargin(2.0f, 0.0f, 4.0f, 0.0f))
	[
		BuildToolbarChip(
			LOCTEXT("ContextChipLabel", "Scope"),
			TAttribute<FText>(LOCTEXT("MandatoryContextStatus", "Context: UE + Project")),
			OsvayderSlateStyle::CyanChipBrush(),
			OsvayderSlateStyle::AccentCyanText(),
			LOCTEXT("MandatoryContextStatusTip", "UE5.7 and project context are mandatory and always included in normal assistant runs."))
	];

	ActionRow->AddSlot()
	.FillWidth(1.0f)
	[
		SNew(SSpacer)
	];

	ActionRow->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
	[
		OverflowActionCount == 1 && SingleOverflowAction != nullptr
			? BuildActionWidget(*SingleOverflowAction)
			: BuildOverflowMenuButton(Actions)
	];

	return ActionRow;
}

TSharedRef<SWidget> SOsvayderToolbar::BuildOverflowMenuButton(const TArray<FToolbarActionEntry>& Actions)
{
	return SNew(SComboButton)
		.ToolTipText(LOCTEXT("MoreActionsTip", "Show lower-frequency and advanced assistant actions"))
		.ContentPadding(OsvayderSlateStyle::CompactButtonPadding())
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

TSharedRef<SWidget> SOsvayderToolbar::BuildOverflowMenuContent(const TArray<FToolbarActionEntry>& Actions)
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
		if (Action.Descriptor.bPrimaryToolbarAction || !Action.Descriptor.bScrollableAction)
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

TSharedRef<SWidget> SOsvayderToolbar::BuildActionWidget(const FToolbarActionEntry& Action)
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
		.ContentPadding(OsvayderSlateStyle::CompactButtonPadding())
		.OnClicked_Lambda([Action]()
		{
			return Action.Execute ? Action.Execute() : FReply::Handled();
		})
		[
			SNew(STextBlock)
			.Text(Action.Descriptor.Label)
		];
}

TSharedRef<SWidget> SOsvayderToolbar::BuildDeferredTasksWidget(const FToolbarActionEntry& Action)
{
	return SNew(SButton)
		.ToolTipText(Action.ToolTip)
		.IsEnabled(Action.bEnabled)
		.Visibility(Action.Visibility)
		.ContentPadding(OsvayderSlateStyle::CompactButtonPadding())
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
				.Text(GetDeferredTasksMenuLabel())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(OsvayderSlateStyle::ChipBrush())
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
