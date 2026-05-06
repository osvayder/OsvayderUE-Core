// Copyright Natali Caggiano. All Rights Reserved.

#include "Widgets/SOsvayderToolCallRow.h"
#include "Widgets/OsvayderSlateStyle.h"

#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	const FTextBlockStyle& GetToolResultTextStyle()
	{
		static FTextBlockStyle Style;
		static bool bInitialized = false;
		if (!bInitialized)
		{
			Style = FAppStyle::GetWidgetStyle<FTextBlockStyle>("SmallText");
			Style.SetFont(FCoreStyle::GetDefaultFontStyle("Mono", 8));
			Style.SetColorAndOpacity(OsvayderSlateStyle::SubtleText());
			bInitialized = true;
		}
		return Style;
	}
}

void SOsvayderToolCallRow::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(OsvayderSlateStyle::ToolRowBrush())
		.Padding(FMargin(6.0f, 3.0f))
		[
			SAssignNew(ExpandableArea, SExpandableArea)
			.InitiallyCollapsed(!InArgs._bResultVisible)
			.HeaderPadding(FMargin(2.0f, 1.0f))
			.HeaderContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(7.0f)
					.HeightOverride(7.0f)
					[
						SNew(SBorder)
						.BorderImage(OsvayderSlateStyle::CyanChipBrush())
						.BorderBackgroundColor(InArgs._StatusColor.GetSpecifiedColor())
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(InArgs._ToolName)
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(OsvayderSlateStyle::SubtleText())
					.Clipping(EWidgetClipping::ClipToBounds)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SAssignNew(StatusLabel, STextBlock)
					.Text(InArgs._StatusText)
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(InArgs._StatusColor)
				]
			]
			.BodyContent()
			[
				SNew(SBorder)
				.BorderImage(OsvayderSlateStyle::ToolResultBrush())
				.Padding(FMargin(8.0f, 6.0f))
				[
					SAssignNew(ResultTextBlock, SMultiLineEditableText)
					.Text(InArgs._ResultText)
					.TextStyle(&GetToolResultTextStyle())
					.IsReadOnly(true)
					.AutoWrapText(true)
				]
			]
		]
	];
}
