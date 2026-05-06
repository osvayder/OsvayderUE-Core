// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Layout/Margin.h"
#include "Styling/SlateColor.h"

struct FSlateBrush;

namespace OsvayderSlateStyle
{
	namespace Metrics
	{
		constexpr float PanelRadius = 7.0f;
		constexpr float ChipRadius = 6.0f;
		constexpr float ComposerMinHeight = 144.0f;
		constexpr float ComposerMaxHeight = 320.0f;
		constexpr float ToolbarGap = 6.0f;
		constexpr float AccentLineHeight = 2.0f;
	}

	namespace Color
	{
		FLinearColor Panel();
		FLinearColor PanelRaised();
		FLinearColor Input();
		FLinearColor Outline();
		FLinearColor OutlineStrong();
		FLinearColor Cyan();
		FLinearColor Violet();
		FLinearColor CyanMuted();
		FLinearColor VioletMuted();
		FLinearColor TextSubtle();
		FLinearColor TextMuted();
		FLinearColor Warning();
		FLinearColor Error();
		FLinearColor Success();
		FLinearColor Blocked();
	}

	FMargin PanelPadding();
	FMargin ChipPadding();
	FMargin CompactButtonPadding();

	FSlateColor TextPrimary();
	FSlateColor SubtleText();
	FSlateColor MutedText();
	FSlateColor AccentCyanText();
	FSlateColor AccentVioletText();

	const FSlateBrush* ToolbarPanelBrush();
	const FSlateBrush* ComposerPanelBrush();
	const FSlateBrush* ComposerInputBrush();
	const FSlateBrush* ChatSurfaceBrush();
	const FSlateBrush* UserMessageBrush();
	const FSlateBrush* AssistantMessageBrush();
	const FSlateBrush* CodeBlockBrush();
	const FSlateBrush* ToolGroupBrush();
	const FSlateBrush* ToolRowBrush();
	const FSlateBrush* ToolResultBrush();
	const FSlateBrush* ChipBrush();
	const FSlateBrush* CyanChipBrush();
	const FSlateBrush* VioletChipBrush();
	const FSlateBrush* AccentLineBrush();
	const FSlateBrush* ImagePreviewBrush();
}
