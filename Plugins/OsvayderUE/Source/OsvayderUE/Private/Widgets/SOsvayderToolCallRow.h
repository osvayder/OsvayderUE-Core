// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SExpandableArea;
class SMultiLineEditableText;
class STextBlock;

/**
 * Compact native Slate row for one assistant tool/action call.
 * Owns only presentation widgets; caller still controls backend state.
 */
class SOsvayderToolCallRow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOsvayderToolCallRow)
		: _ToolName(FText::FromString(TEXT("Tool")))
		, _StatusText(FText::FromString(TEXT("Running")))
		, _StatusColor(FSlateColor(FLinearColor::White))
		, _ResultText(FText::GetEmpty())
		, _bResultVisible(false)
	{}
		SLATE_ARGUMENT(FText, ToolName)
		SLATE_ARGUMENT(FText, StatusText)
		SLATE_ARGUMENT(FSlateColor, StatusColor)
		SLATE_ARGUMENT(FText, ResultText)
		SLATE_ARGUMENT(bool, bResultVisible)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	TSharedPtr<STextBlock> GetStatusLabel() const { return StatusLabel; }
	TSharedPtr<SMultiLineEditableText> GetResultTextBlock() const { return ResultTextBlock; }
	TSharedPtr<SExpandableArea> GetExpandableArea() const { return ExpandableArea; }

private:
	TSharedPtr<STextBlock> StatusLabel;
	TSharedPtr<SMultiLineEditableText> ResultTextBlock;
	TSharedPtr<SExpandableArea> ExpandableArea;
};
