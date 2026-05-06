// Copyright Natali Caggiano. All Rights Reserved.

#include "SOsvayderInputArea.h"
#include "ClipboardImageUtils.h"
#include "OsvayderSlateStyle.h"
#include "OsvayderUEConstants.h"
#include "OsvayderUEModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Styling/AppStyle.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/FileHelper.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"

#define LOCTEXT_NAMESPACE "OsvayderUE"

void SOsvayderInputArea::Construct(const FArguments& InArgs)
{
	bIsWaiting = InArgs._bIsWaiting;
	OnSend = InArgs._OnSend;
	OnCancel = InArgs._OnCancel;
	OnToggleDictation = InArgs._OnToggleDictation;
	DictationState = InArgs._DictationState;
	DictationStatusText = InArgs._DictationStatusText;
	OnTextChangedDelegate = InArgs._OnTextChanged;
	OnImagesChangedDelegate = InArgs._OnImagesChanged;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(OsvayderSlateStyle::ComposerPanelBrush())
		.Padding(OsvayderSlateStyle::PanelPadding())
		[
			SNew(SVerticalBox)

			// Image preview strip (starts collapsed, horizontal scroll)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SAssignNew(ImagePreviewStrip, SHorizontalBox)
				.Visibility(EVisibility::Collapsed)
			]

			// Composer header/status row
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 5.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SBorder)
					.BorderImage(OsvayderSlateStyle::CyanChipBrush())
					.Padding(OsvayderSlateStyle::ChipPadding())
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ComposerChip", "Composer"))
						.TextStyle(FAppStyle::Get(), "SmallText")
						.ColorAndOpacity(OsvayderSlateStyle::AccentCyanText())
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return GetInputStatusText(); })
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(OsvayderSlateStyle::MutedText())
					.Clipping(EWidgetClipping::ClipToBounds)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				.Padding(12.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ComposerShortcut", "Enter send | Shift+Enter newline | Ctrl+V paste/image"))
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(OsvayderSlateStyle::MutedText())
					.Clipping(EWidgetClipping::ClipToBounds)
				]
			]

			// Input surface
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SBorder)
				.BorderImage(OsvayderSlateStyle::ComposerInputBrush())
				.Padding(3.0f)
				[
					SNew(SBox)
					.MinDesiredHeight(OsvayderSlateStyle::Metrics::ComposerMinHeight)
					.MaxDesiredHeight(OsvayderSlateStyle::Metrics::ComposerMaxHeight)
					[
						SAssignNew(InputTextBox, SMultiLineEditableTextBox)
						.HintText(LOCTEXT("InputHint", "Ask Osvayder UE about this editor task..."))
						.AutoWrapText(true)
						.AllowMultiLine(true)
						.OnTextChanged(this, &SOsvayderInputArea::HandleTextChanged)
						.OnTextCommitted(this, &SOsvayderInputArea::HandleTextCommitted)
						.OnKeyDownHandler(this, &SOsvayderInputArea::OnInputKeyDown)
						.IsEnabled_Lambda([this]() { return IsTextEntryEnabled(); })
					]
				]
			]

			// Composer footer
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return DictationStatusText.Get(); })
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity_Lambda([this]() { return GetDictationStatusColor(); })
					.Visibility_Lambda([this]()
					{
						return DictationStatusText.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(10.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						const int32 CharCount = CurrentInputText.Len();
						if (CharCount > 0)
						{
							return FText::Format(LOCTEXT("CharCount", "{0} chars"), FText::AsNumber(CharCount));
						}
						return FText::GetEmpty();
					})
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(OsvayderSlateStyle::MutedText())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(12.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text_Lambda([this]() { return GetDictationButtonText(); })
					.ContentPadding(OsvayderSlateStyle::CompactButtonPadding())
					.OnClicked(this, &SOsvayderInputArea::HandleToggleDictationClicked)
					.ToolTipText_Lambda([this]() { return GetDictationToolTipText(); })
					.IsEnabled_Lambda([this]()
					{
						return !bIsWaiting.Get() && DictationState.Get() != EOsvayderUEVoiceDictationState::PreparingRuntime
							&& DictationState.Get() != EOsvayderUEVoiceDictationState::Transcribing;
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text_Lambda([this]() { return bIsWaiting.Get() ? LOCTEXT("Cancel", "Cancel") : LOCTEXT("Send", "Send"); })
					.ContentPadding(FMargin(10.0f, 4.0f))
					.OnClicked(this, &SOsvayderInputArea::HandleSendCancelClicked)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.ForegroundColor_Lambda([this]() { return GetSendButtonTextColor(); })
					.IsEnabled_Lambda([this]() { return !IsDictationInteractionBusy() || bIsWaiting.Get(); })
				]
			]
		]
	];
}

void SOsvayderInputArea::SetText(const FString& NewText)
{
	CurrentInputText = NewText;
	if (InputTextBox.IsValid())
	{
		InputTextBox->SetText(FText::FromString(NewText));
	}
}

FString SOsvayderInputArea::GetText() const
{
	return CurrentInputText;
}

void SOsvayderInputArea::ClearText()
{
	CurrentInputText.Empty();
	if (InputTextBox.IsValid())
	{
		InputTextBox->SetText(FText::GetEmpty());
	}
	ClearAttachedImages();
}

void SOsvayderInputArea::FocusTextEntry()
{
	if (InputTextBox.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(InputTextBox, EFocusCause::SetDirectly);
	}
}

bool SOsvayderInputArea::HasAttachedImages() const
{
	return AttachedImagePaths.Num() > 0;
}

int32 SOsvayderInputArea::GetAttachedImageCount() const
{
	return AttachedImagePaths.Num();
}

TArray<FString> SOsvayderInputArea::GetAttachedImagePaths() const
{
	return AttachedImagePaths;
}

void SOsvayderInputArea::ClearAttachedImages()
{
	AttachedImagePaths.Empty();
	ThumbnailBrushes.Empty();
	RebuildImagePreviewStrip();
}

void SOsvayderInputArea::RemoveAttachedImage(int32 Index)
{
	if (AttachedImagePaths.IsValidIndex(Index))
	{
		AttachedImagePaths.RemoveAt(Index);
		ThumbnailBrushes.RemoveAt(Index);
		RebuildImagePreviewStrip();
		OnImagesChangedDelegate.ExecuteIfBound(AttachedImagePaths);
	}
}

FReply SOsvayderInputArea::OnInputKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (!IsTextEntryEnabled())
	{
		return FReply::Handled();
	}

	// Ctrl+V: try image paste first, fall back to text paste
	if (InKeyEvent.GetKey() == EKeys::V && InKeyEvent.IsControlDown())
	{
		if (TryPasteImageFromClipboard())
		{
			return FReply::Handled();
		}
		// Return unhandled to let default text paste proceed
		return FReply::Unhandled();
	}

	// Enter (without Shift) to send
	// Shift+Enter allows newline
	if (InKeyEvent.GetKey() == EKeys::Enter)
	{
		if (!InKeyEvent.IsShiftDown())
		{
			OnSend.ExecuteIfBound();
			return FReply::Handled();
		}
	}

	return FReply::Unhandled();
}

void SOsvayderInputArea::HandleTextChanged(const FText& NewText)
{
	CurrentInputText = NewText.ToString();
	OnTextChangedDelegate.ExecuteIfBound(CurrentInputText);
}

void SOsvayderInputArea::HandleTextCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	// Don't send on commit - use explicit Enter key handling
}

FReply SOsvayderInputArea::HandlePasteClicked()
{
	// Try image paste first
	if (TryPasteImageFromClipboard())
	{
		return FReply::Handled();
	}

	// Fall back to text paste
	FString ClipboardText;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardText);
	if (!ClipboardText.IsEmpty() && InputTextBox.IsValid())
	{
		// Append to existing text
		FString NewText = CurrentInputText + ClipboardText;
		SetText(NewText);
	}
	return FReply::Handled();
}

FReply SOsvayderInputArea::HandleSendCancelClicked()
{
	if (bIsWaiting.Get())
	{
		OnCancel.ExecuteIfBound();
	}
	else
	{
		OnSend.ExecuteIfBound();
	}
	return FReply::Handled();
}

FReply SOsvayderInputArea::HandleToggleDictationClicked()
{
	OnToggleDictation.ExecuteIfBound();
	return FReply::Handled();
}

bool SOsvayderInputArea::TryPasteImageFromClipboard()
{
	using namespace OsvayderUEConstants::ClipboardImage;

	if (!FClipboardImageUtils::ClipboardHasImage())
	{
		return false;
	}

	// Reject if at max image count
	if (AttachedImagePaths.Num() >= MaxImagesPerMessage)
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("Image paste rejected: already at max (%d images)"), MaxImagesPerMessage);
		return false;
	}

	// Clean up old screenshots before saving a new one
	FString ScreenshotDir = FClipboardImageUtils::GetScreenshotDirectory();
	FClipboardImageUtils::CleanupOldScreenshots(
		ScreenshotDir,
		OsvayderUEConstants::ClipboardImage::MaxScreenshotAgeSeconds);

	FString SavedPath;
	if (!FClipboardImageUtils::SaveClipboardImageToFile(SavedPath, ScreenshotDir))
	{
		return false;
	}

	AttachedImagePaths.Add(SavedPath);
	ThumbnailBrushes.Add(CreateThumbnailBrush(SavedPath));
	RebuildImagePreviewStrip();

	OnImagesChangedDelegate.ExecuteIfBound(AttachedImagePaths);
	return true;
}

FReply SOsvayderInputArea::HandleRemoveImageClicked(int32 Index)
{
	RemoveAttachedImage(Index);
	return FReply::Handled();
}

void SOsvayderInputArea::RebuildImagePreviewStrip()
{
	using namespace OsvayderUEConstants::ClipboardImage;

	if (!ImagePreviewStrip.IsValid())
	{
		return;
	}

	ImagePreviewStrip->ClearChildren();

	if (AttachedImagePaths.Num() == 0)
	{
		ImagePreviewStrip->SetVisibility(EVisibility::Collapsed);
		return;
	}

	ImagePreviewStrip->SetVisibility(EVisibility::Visible);

	// Add a thumbnail slot for each attached image
	for (int32 i = 0; i < AttachedImagePaths.Num(); ++i)
	{
		const FString& ImagePath = AttachedImagePaths[i];
		FString FileName = FPaths::GetCleanFilename(ImagePath);

		ImagePreviewStrip->AddSlot()
		.AutoWidth()
		.Padding(i > 0 ? ThumbnailSpacing : 0.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.WidthOverride(ThumbnailSize)
			.HeightOverride(ThumbnailSize)
			.ToolTipText(FText::FromString(FileName))
			[
				SNew(SOverlay)

				// Layer 0: thumbnail image
				+ SOverlay::Slot()
				[
					SNew(SBorder)
					.BorderImage(OsvayderSlateStyle::ImagePreviewBrush())
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Fill)
					[
						SNew(SImage)
						.Image(ThumbnailBrushes.IsValidIndex(i) && ThumbnailBrushes[i].IsValid()
							? ThumbnailBrushes[i].Get() : nullptr)
					]
				]

				// Layer 1: remove button (top-right)
				+ SOverlay::Slot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Top)
				[
					SNew(SButton)
					.Text(LOCTEXT("RemoveImageX", "X"))
					.OnClicked_Lambda([this, Index = i]()
					{
						return HandleRemoveImageClicked(Index);
					})
					.ToolTipText(LOCTEXT("RemoveImageTip", "Remove this image"))
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				]
			]
		];
	}

	// Add count label after thumbnails
	ImagePreviewStrip->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(ThumbnailSpacing, 0.0f, 0.0f, 0.0f)
	[
		SNew(STextBlock)
		.Text(FText::Format(LOCTEXT("ImageCount", "{0}/{1}"),
			FText::AsNumber(AttachedImagePaths.Num()),
			FText::AsNumber(MaxImagesPerMessage)))
		.TextStyle(FAppStyle::Get(), "SmallText")
		.ColorAndOpacity(OsvayderSlateStyle::SubtleText())
	];
}

TSharedPtr<FSlateDynamicImageBrush> SOsvayderInputArea::CreateThumbnailBrush(const FString& FilePath) const
{
	// Load PNG file from disk
	TArray<uint8> PngData;
	if (!FFileHelper::LoadFileToArray(PngData, *FilePath))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to load image for thumbnail: %s"), *FilePath);
		return nullptr;
	}

	// Decompress PNG to raw pixels
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

	if (!ImageWrapper.IsValid())
	{
		return nullptr;
	}

	if (!ImageWrapper->SetCompressed(PngData.GetData(), PngData.Num()))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to decompress PNG for thumbnail"));
		return nullptr;
	}

	TArray<uint8> RawData;
	if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to get raw pixel data for thumbnail"));
		return nullptr;
	}

	const int32 Width = ImageWrapper->GetWidth();
	const int32 Height = ImageWrapper->GetHeight();

	if (Width <= 0 || Height <= 0)
	{
		return nullptr;
	}

	// Create a dynamic brush from the raw pixel data
	FName BrushName = FName(*FString::Printf(TEXT("ClipboardThumb_%s"), *FPaths::GetBaseFilename(FilePath)));
	TSharedPtr<FSlateDynamicImageBrush> Brush = FSlateDynamicImageBrush::CreateWithImageData(
		BrushName,
		FVector2D(Width, Height),
		TArray<uint8>(RawData));

	return Brush;
}

bool SOsvayderInputArea::IsDictationInteractionBusy() const
{
	const EOsvayderUEVoiceDictationState State = DictationState.Get();
	return State == EOsvayderUEVoiceDictationState::Recording
		|| State == EOsvayderUEVoiceDictationState::PreparingRuntime
		|| State == EOsvayderUEVoiceDictationState::Transcribing;
}

bool SOsvayderInputArea::IsTextEntryEnabled() const
{
	return !bIsWaiting.Get() && !IsDictationInteractionBusy();
}

FText SOsvayderInputArea::GetDictationButtonText() const
{
	switch (DictationState.Get())
	{
	case EOsvayderUEVoiceDictationState::Recording:
		return LOCTEXT("StopDictation", "Stop");

	case EOsvayderUEVoiceDictationState::PreparingRuntime:
		return LOCTEXT("PreparingDictation", "Preparing...");

	case EOsvayderUEVoiceDictationState::Transcribing:
		return LOCTEXT("TranscribingDictation", "Transcribing...");

	case EOsvayderUEVoiceDictationState::Failed:
	case EOsvayderUEVoiceDictationState::Unavailable:
	case EOsvayderUEVoiceDictationState::Idle:
	default:
		return LOCTEXT("StartDictation", "Dictate");
	}
}

FText SOsvayderInputArea::GetDictationToolTipText() const
{
	switch (DictationState.Get())
	{
	case EOsvayderUEVoiceDictationState::Recording:
		return LOCTEXT("StopDictationTip", "Stop recording and transcribe the captured audio");

	case EOsvayderUEVoiceDictationState::PreparingRuntime:
		return LOCTEXT("PreparingDictationTip", "Preparing the bounded offline dictation runtime");

	case EOsvayderUEVoiceDictationState::Transcribing:
		return LOCTEXT("TranscribingDictationTip", "Transcribing captured audio into the prompt input");

	case EOsvayderUEVoiceDictationState::Failed:
	case EOsvayderUEVoiceDictationState::Unavailable:
	case EOsvayderUEVoiceDictationState::Idle:
	default:
		return LOCTEXT("StartDictationTip", "Record a short dictation clip and insert the transcript into the prompt box");
	}
}

FSlateColor SOsvayderInputArea::GetDictationStatusColor() const
{
	switch (DictationState.Get())
	{
	case EOsvayderUEVoiceDictationState::Recording:
		return FSlateColor(OsvayderSlateStyle::Color::Warning());

	case EOsvayderUEVoiceDictationState::PreparingRuntime:
	case EOsvayderUEVoiceDictationState::Transcribing:
		return FSlateColor(OsvayderSlateStyle::Color::Warning());

	case EOsvayderUEVoiceDictationState::Failed:
	case EOsvayderUEVoiceDictationState::Unavailable:
		return FSlateColor(OsvayderSlateStyle::Color::Error());

	case EOsvayderUEVoiceDictationState::Idle:
	default:
		return OsvayderSlateStyle::MutedText();
	}
}

FText SOsvayderInputArea::GetInputStatusText() const
{
	if (bIsWaiting.Get())
	{
		return LOCTEXT("ComposerWaitingStatus", "Assistant running - Cancel remains available");
	}

	if (IsDictationInteractionBusy())
	{
		return LOCTEXT("ComposerDictationBusyStatus", "Voice dictation is preparing input");
	}

	if (AttachedImagePaths.Num() > 0)
	{
		return FText::Format(LOCTEXT("ComposerImageStatus", "{0} image(s) attached"),
			FText::AsNumber(AttachedImagePaths.Num()));
	}

	return LOCTEXT("ComposerReadyStatus", "Ready for prompt, pasted images, or dictation");
}

FSlateColor SOsvayderInputArea::GetSendButtonTextColor() const
{
	return bIsWaiting.Get() ? OsvayderSlateStyle::SubtleText() : FSlateColor::UseForeground();
}

#undef LOCTEXT_NAMESPACE
