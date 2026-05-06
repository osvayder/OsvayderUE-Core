// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderSlateStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/StyleColors.h"

namespace OsvayderSlateStyle
{
	namespace Color
	{
		FLinearColor Panel()
		{
			return FLinearColor(0.020f, 0.023f, 0.030f, 1.0f);
		}

		FLinearColor PanelRaised()
		{
			return FLinearColor(0.035f, 0.040f, 0.052f, 1.0f);
		}

		FLinearColor Input()
		{
			return FLinearColor(0.012f, 0.015f, 0.021f, 1.0f);
		}

		FLinearColor Outline()
		{
			return FLinearColor(0.125f, 0.145f, 0.175f, 1.0f);
		}

		FLinearColor OutlineStrong()
		{
			return FLinearColor(0.215f, 0.250f, 0.310f, 1.0f);
		}

		FLinearColor Cyan()
		{
			return FLinearColor(0.130f, 0.760f, 0.920f, 1.0f);
		}

		FLinearColor Violet()
		{
			return FLinearColor(0.560f, 0.390f, 0.950f, 1.0f);
		}

		FLinearColor CyanMuted()
		{
			return FLinearColor(0.055f, 0.260f, 0.330f, 1.0f);
		}

		FLinearColor VioletMuted()
		{
			return FLinearColor(0.190f, 0.135f, 0.310f, 1.0f);
		}

		FLinearColor TextSubtle()
		{
			return FLinearColor(0.720f, 0.760f, 0.820f, 1.0f);
		}

		FLinearColor TextMuted()
		{
			return FLinearColor(0.470f, 0.500f, 0.560f, 1.0f);
		}

		FLinearColor Warning()
		{
			return FLinearColor(0.870f, 0.690f, 0.220f, 1.0f);
		}

		FLinearColor Error()
		{
			return FLinearColor(0.900f, 0.300f, 0.300f, 1.0f);
		}

		FLinearColor Success()
		{
			return FLinearColor(0.320f, 0.780f, 0.480f, 1.0f);
		}

		FLinearColor Blocked()
		{
			return FLinearColor(0.840f, 0.520f, 0.250f, 1.0f);
		}
	}

	FMargin PanelPadding()
	{
		return FMargin(10.0f, 7.0f);
	}

	FMargin ChipPadding()
	{
		return FMargin(7.0f, 2.0f);
	}

	FMargin CompactButtonPadding()
	{
		return FMargin(8.0f, 3.0f);
	}

	FSlateColor TextPrimary()
	{
		return FStyleColors::Foreground;
	}

	FSlateColor SubtleText()
	{
		return FSlateColor(Color::TextSubtle());
	}

	FSlateColor MutedText()
	{
		return FSlateColor(Color::TextMuted());
	}

	FSlateColor AccentCyanText()
	{
		return FSlateColor(Color::Cyan());
	}

	FSlateColor AccentVioletText()
	{
		return FSlateColor(Color::Violet());
	}

	const FSlateBrush* ToolbarPanelBrush()
	{
		static const FSlateRoundedBoxBrush Brush(Color::Panel(), Metrics::PanelRadius, Color::Outline(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* ComposerPanelBrush()
	{
		static const FSlateRoundedBoxBrush Brush(Color::PanelRaised(), Metrics::PanelRadius, Color::Outline(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* ComposerInputBrush()
	{
		static const FSlateRoundedBoxBrush Brush(Color::Input(), 6.0f, Color::OutlineStrong(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* ChatSurfaceBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.016f, 0.019f, 0.026f, 1.0f), 6.0f, Color::Outline(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* UserMessageBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.036f, 0.050f, 0.070f, 1.0f), 7.0f, FLinearColor(0.115f, 0.185f, 0.255f, 1.0f), 1.0f);
		return &Brush;
	}

	const FSlateBrush* AssistantMessageBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.030f, 0.034f, 0.044f, 1.0f), 7.0f, Color::Outline(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* CodeBlockBrush()
	{
		static const FSlateRoundedBoxBrush Brush(Color::Input(), 5.0f, Color::OutlineStrong(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* ToolGroupBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.025f, 0.031f, 0.041f, 1.0f), 6.0f, Color::Outline(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* ToolRowBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.020f, 0.024f, 0.032f, 1.0f), 5.0f, Color::Outline(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* ToolResultBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.012f, 0.015f, 0.020f, 1.0f), 4.0f, Color::OutlineStrong(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* ChipBrush()
	{
		static const FSlateRoundedBoxBrush Brush(Color::PanelRaised(), Metrics::ChipRadius, Color::Outline(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* CyanChipBrush()
	{
		static const FSlateRoundedBoxBrush Brush(Color::CyanMuted(), Metrics::ChipRadius, Color::Cyan(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* VioletChipBrush()
	{
		static const FSlateRoundedBoxBrush Brush(Color::VioletMuted(), Metrics::ChipRadius, Color::Violet(), 1.0f);
		return &Brush;
	}

	const FSlateBrush* AccentLineBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.110f, 0.470f, 0.640f, 1.0f), 1.0f);
		return &Brush;
	}

	const FSlateBrush* ImagePreviewBrush()
	{
		static const FSlateRoundedBoxBrush Brush(Color::Input(), 4.0f, Color::OutlineStrong(), 1.0f);
		return &Brush;
	}
}
