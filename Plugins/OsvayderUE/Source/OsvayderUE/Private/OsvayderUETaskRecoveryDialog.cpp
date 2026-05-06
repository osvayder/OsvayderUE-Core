// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUETaskRecoveryDialog.h"

#include "OsvayderUEModule.h"
#include "OsvayderUETaskRecoverySummarizer.h"

#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Framework/Application/SlateApplication.h"

namespace
{
	constexpr float DialogWidth = 720.0f;
	constexpr float DialogHeight = 520.0f;

	TSharedRef<SWidget> BuildSummarySection(const FTaskRecoverySummary& S, const FString& DiagnosticNote)
	{
		return SNew(SScrollBox)
			+ SScrollBox::Slot()
			.Padding(0, 4)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Task: %s"), *S.ShortTitle)))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				.AutoWrapText(true)
			]
			+ SScrollBox::Slot()
			.Padding(0, 8)
			[
				SNew(STextBlock)
				.Text(FText::FromString(S.PhaseStatusLine))
				.AutoWrapText(true)
			]
			+ SScrollBox::Slot()
			.Padding(0, 8)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Last intent: %s"), *S.LastIntent)))
				.AutoWrapText(true)
			]
			+ SScrollBox::Slot()
			.Padding(0, 8)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Paused: %s"), *S.ElapsedSincePause)))
				.AutoWrapText(true)
			]
			+ SScrollBox::Slot()
			.Padding(0, DiagnosticNote.IsEmpty() ? 0 : 8)
			[
				SNew(STextBlock)
				.Text(FText::FromString(DiagnosticNote))
				.AutoWrapText(true)
				.Visibility(DiagnosticNote.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.ColorAndOpacity(FLinearColor(1.0f, 0.82f, 0.35f))
			]
			+ SScrollBox::Slot()
			.Padding(0, 8)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Progress:")))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SScrollBox::Slot()
			.Padding(0, 4)
			[
				SNew(STextBlock)
				.Text(FText::FromString(S.ProgressSummary))
				.AutoWrapText(true)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			]
			+ SScrollBox::Slot()
			.Padding(0, 8)
			[
				SNew(STextBlock)
				.Text(FText::FromString(S.StallIndicator.IsEmpty()
					? TEXT("")
					: FString::Printf(TEXT("⚠ %s"), *S.StallIndicator)))
				.ColorAndOpacity(FLinearColor(1.0f, 0.7f, 0.2f))
				.AutoWrapText(true)
			];
	}
}

FOsvayderUETaskRecoveryDialog::FDialogResult
FOsvayderUETaskRecoveryDialog::Show(const FTaskRecoverySummary& Summary, const FString& DiagnosticNote)
{
	FDialogResult FallbackResult;
	FallbackResult.Choice = EOsvayderUETaskRecoveryChoice::None;

	if (!IsInGameThread())
	{
		UE_LOG(LogOsvayderUE, Error,
			TEXT("TaskRecoveryDialog::Show must be called on game thread."));
		return FallbackResult;
	}

	// Shared state captured by button lambdas (cannot use stack locals).
	TSharedPtr<EOsvayderUETaskRecoveryChoice> ChoicePtr
		= MakeShared<EOsvayderUETaskRecoveryChoice>(EOsvayderUETaskRecoveryChoice::None);
	TSharedPtr<FString> ReasonPtr = MakeShared<FString>();
	TSharedPtr<FString> ReasonFieldValuePtr = MakeShared<FString>();

	TSharedPtr<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("Unfinished task detected")))
		.ClientSize(FVector2D(DialogWidth, DialogHeight))
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	// Reason field (only meaningful for "Close as Irrelevant"). Always rendered
	// below the button row so user can pre-fill before clicking; empty is OK.
	TSharedPtr<SEditableTextBox> ReasonBox = SNew(SEditableTextBox)
		.HintText(FText::FromString(TEXT("(optional) why is this task irrelevant?")))
		.OnTextChanged_Lambda([ReasonFieldValuePtr](const FText& NewText)
		{
			*ReasonFieldValuePtr = NewText.ToString();
		});

	TSharedRef<SWidget> Content = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(12, 12, 12, 6)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Your previous session has an autonomous task that never completed. What should I do?")))
			.AutoWrapText(true)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(12, 6)
		[
			BuildSummarySection(Summary, DiagnosticNote)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(12, 8, 12, 0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Optional reason (for 'Close as Irrelevant'):")))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(12, 4)
		[
			ReasonBox.ToSharedRef()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Right)
		.Padding(12, 10, 12, 12)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Continue")))
				.ToolTipText(FText::FromString(TEXT("Resume this task now with one bounded system-generated recovery turn. If auto-resume is blocked, the UI will explain why.")))
				.OnClicked_Lambda([ChoicePtr, Window]()
				{
					*ChoicePtr = EOsvayderUETaskRecoveryChoice::Continue;
					if (Window.IsValid())
					{
						Window->RequestDestroyWindow();
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Start Fresh")))
				.ToolTipText(FText::FromString(TEXT("Archive this plan as abandoned_for_fresh_session, reset active_plan.json to empty.")))
				.OnClicked_Lambda([ChoicePtr, Window]()
				{
					*ChoicePtr = EOsvayderUETaskRecoveryChoice::StartFresh;
					if (Window.IsValid())
					{
						Window->RequestDestroyWindow();
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Close as Irrelevant")))
				.ToolTipText(FText::FromString(TEXT("Mark this plan abandoned_by_user + record the optional reason above.")))
				.OnClicked_Lambda([ChoicePtr, ReasonPtr, ReasonFieldValuePtr, Window]()
				{
					*ChoicePtr = EOsvayderUETaskRecoveryChoice::CloseAsIrrelevant;
					*ReasonPtr = *ReasonFieldValuePtr;
					if (Window.IsValid())
					{
						Window->RequestDestroyWindow();
					}
					return FReply::Handled();
				})
			]
		];

	Window->SetContent(Content);

	FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(), nullptr);

	FDialogResult Out;
	Out.Choice = *ChoicePtr;
	Out.UserClosedReason = *ReasonPtr;
	return Out;
}

FString FOsvayderUETaskRecoveryContextBuilder::BuildContextBlockText(const FTaskRecoverySummary& Summary)
{
	TArray<FString> Lines;
	Lines.Add(TEXT("The previous autonomous session was interrupted before this task could complete. Resume it unless the user has redirected you."));
	Lines.Add(FString::Printf(TEXT("Original user task: %s"), *Summary.OriginalUserTask));
	Lines.Add(FString::Printf(TEXT("Phase status: %s"), *Summary.PhaseStatusLine));
	Lines.Add(FString::Printf(TEXT("Last intent: %s"), *Summary.LastIntent));
	Lines.Add(FString::Printf(TEXT("Paused: %s"), *Summary.ElapsedSincePause));
	if (!Summary.StallIndicator.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Possible stall: %s"), *Summary.StallIndicator));
	}
	Lines.Add(TEXT("Progress so far:"));
	Lines.Add(Summary.ProgressSummary);
	if (!Summary.EvidencePointer.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Evidence pointer: %s"), *Summary.EvidencePointer));
	}
	Lines.Add(TEXT("Resume the task by first verifying current state, then continuing from the last intent. Do not duplicate already-completed tool calls."));
	return FString::Join(Lines, TEXT("\n"));
}
