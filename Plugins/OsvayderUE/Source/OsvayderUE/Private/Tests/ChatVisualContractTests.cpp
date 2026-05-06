// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderEditorWidget.h"
#include "OsvayderUEVoiceDictationTypes.h"
#include "Widgets/OsvayderSlateStyle.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/SlateRenderer.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/SOsvayderInputArea.h"
#include "Widgets/SOsvayderToolCallRow.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace OsvayderUEChatVisualContractTests
{
	struct FVisualEvidenceMetrics
	{
		FString ScreenshotPath;
		int32 Width = 0;
		int32 Height = 0;
		int32 PixelCount = 0;
		int32 UniqueColorCount = 0;
		int32 NonBlackPixelCount = 0;
		int32 MinRequiredNonBlackPixels = 0;
		bool bRawCaptureSucceeded = false;
		bool bBitmapSaved = false;
		bool bUsableEvidence = false;
		FString FailureReason;
	};

	void CollectWidgetTree(
		const TSharedRef<SWidget>& Widget,
		TMap<FString, int32>& TypeCounts,
		TSet<FString>& TextLabels)
	{
		const FString TypeName = Widget->GetTypeAsString();
		++TypeCounts.FindOrAdd(TypeName);

		if (TypeName == TEXT("STextBlock"))
		{
			const TSharedRef<STextBlock> TextBlock = StaticCastSharedRef<STextBlock>(Widget);
			TextLabels.Add(TextBlock->GetText().ToString());
		}
		else if (TypeName == TEXT("SMultiLineEditableText"))
		{
			const TSharedRef<SMultiLineEditableText> TextBlock = StaticCastSharedRef<SMultiLineEditableText>(Widget);
			TextLabels.Add(TextBlock->GetText().ToString());
		}

		if (FChildren* Children = Widget->GetAllChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				CollectWidgetTree(Children->GetChildAt(Index), TypeCounts, TextLabels);
			}
		}
	}

	FVisualEvidenceMetrics AnalyzeScreenshotPixels(const TArray<FColor>& ColorData, const FIntVector& ImageSize)
	{
		FVisualEvidenceMetrics Metrics;
		Metrics.Width = ImageSize.X;
		Metrics.Height = ImageSize.Y;
		Metrics.PixelCount = ColorData.Num();
		Metrics.MinRequiredNonBlackPixels = FMath::Max(512, Metrics.PixelCount / 100);

		TSet<uint32> UniqueColors;
		for (const FColor& Pixel : ColorData)
		{
			UniqueColors.Add(Pixel.ToPackedARGB());
			if (Pixel.R > 8 || Pixel.G > 8 || Pixel.B > 8)
			{
				++Metrics.NonBlackPixelCount;
			}
		}

		Metrics.UniqueColorCount = UniqueColors.Num();

		if (Metrics.UniqueColorCount <= 1)
		{
			Metrics.FailureReason = TEXT("unique_color_count<=1");
		}
		else if (Metrics.NonBlackPixelCount < Metrics.MinRequiredNonBlackPixels)
		{
			Metrics.FailureReason = TEXT("non_black_pixel_count_below_threshold");
		}

		return Metrics;
	}

	FVisualEvidenceMetrics CaptureWidgetScreenshot(const TSharedRef<SWidget>& Widget)
	{
		FVisualEvidenceMetrics Metrics;
		if (!FSlateApplication::IsInitialized())
		{
			Metrics.FailureReason = TEXT("slate_not_initialized");
			return Metrics;
		}

		const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), TEXT("packet724d_chat_visual_contract"));
		IFileManager::Get().MakeDirectory(*OutputDir, true);

		TSharedRef<SWindow> PreviewWindow =
			SNew(SWindow)
			.Title(FText::FromString(TEXT("packet724d_chat_visual_contract_preview")))
			.ClientSize(FVector2D(760.0f, 520.0f))
			.SupportsMinimize(false)
			.SupportsMaximize(false);
		PreviewWindow->SetContent(Widget);
		FSlateApplication::Get().AddWindow(PreviewWindow, true);
		PreviewWindow->SlatePrepass(FSlateApplication::Get().GetApplicationScale());
		FSlateApplication::Get().Tick();
		if (FSlateApplication::Get().GetRenderer() != nullptr)
		{
			FSlateApplication::Get().GetRenderer()->FlushCommands();
		}
		FPlatformProcess::Sleep(0.05f);
		FSlateApplication::Get().Tick();

		TArray<FColor> ColorData;
		FIntVector ImageSize = FIntVector::ZeroValue;
		Metrics.bRawCaptureSucceeded = FSlateApplication::Get().TakeScreenshot(PreviewWindow, ColorData, ImageSize);

		FSlateApplication::Get().RequestDestroyWindow(PreviewWindow);
		FSlateApplication::Get().Tick();

		if (!Metrics.bRawCaptureSucceeded || ImageSize.X <= 0 || ImageSize.Y <= 0 || ColorData.Num() == 0)
		{
			Metrics.FailureReason = TEXT("take_screenshot_failed");
			return Metrics;
		}

		FString ActualFilename;
		const FString BitmapPattern = FPaths::Combine(OutputDir, TEXT("packet724d_chat_visual_contract_render.bmp"));
		FFileHelper::CreateBitmap(*BitmapPattern, ImageSize.X, ImageSize.Y, ColorData.GetData(), nullptr, &IFileManager::Get(), &ActualFilename, false);
		if (ActualFilename.IsEmpty())
		{
			ActualFilename = BitmapPattern;
		}

		Metrics = AnalyzeScreenshotPixels(ColorData, ImageSize);
		Metrics.bRawCaptureSucceeded = true;
		Metrics.ScreenshotPath = ActualFilename;
		FPaths::NormalizeFilename(Metrics.ScreenshotPath);
		Metrics.bBitmapSaved = IFileManager::Get().FileExists(*Metrics.ScreenshotPath);
		if (!Metrics.bBitmapSaved)
		{
			Metrics.FailureReason = TEXT("bitmap_not_saved");
			return Metrics;
		}

		Metrics.bUsableEvidence = Metrics.FailureReason.IsEmpty();
		return Metrics;
	}

	FString WriteReceipt(
		const TMap<FString, int32>& TypeCounts,
		const TSet<FString>& TextLabels,
		const FVisualEvidenceMetrics& ScreenshotMetrics)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("packet"), TEXT("packet724d_chat_visual_contract_v1"));
		Root->SetStringField(TEXT("presentation"), TEXT("document_messages_compact_tool_rows_composer"));
		Root->SetBoolField(TEXT("has_expandable_tool_rows"), TypeCounts.FindRef(TEXT("SExpandableArea")) >= 4);
		Root->SetBoolField(TEXT("has_user_chip"), TextLabels.Contains(TEXT("You")));
		Root->SetBoolField(TEXT("has_response_meta"), TextLabels.Contains(TEXT("Response")));
		Root->SetBoolField(TEXT("has_composer_chip"), TextLabels.Contains(TEXT("Composer")));
		Root->SetBoolField(TEXT("has_running_status"), TextLabels.Contains(TEXT("Running")));
		Root->SetBoolField(TEXT("has_completed_status"), TextLabels.Contains(TEXT("Completed")));
		Root->SetBoolField(TEXT("has_blocked_status"), TextLabels.Contains(TEXT("Blocked")));
		Root->SetBoolField(TEXT("has_failed_status"), TextLabels.Contains(TEXT("Failed")));
		Root->SetBoolField(TEXT("no_visible_paste_button"), !TextLabels.Contains(TEXT("Paste")));
		Root->SetBoolField(TEXT("has_selectable_output_text"), TypeCounts.FindRef(TEXT("SMultiLineEditableText")) >= 6);
		Root->SetBoolField(TEXT("has_editable_prompt_input"), TypeCounts.FindRef(TEXT("SMultiLineEditableTextBox")) == 1);
		Root->SetBoolField(TEXT("composer_min_height_increased"), OsvayderSlateStyle::Metrics::ComposerMinHeight >= 120.0f);
		Root->SetBoolField(TEXT("composer_max_height_allows_growth"), OsvayderSlateStyle::Metrics::ComposerMaxHeight >= 300.0f);
		Root->SetBoolField(TEXT("composer_has_no_nested_scrollbox"), !TypeCounts.Contains(TEXT("SScrollBox")));
		Root->SetNumberField(TEXT("expandable_area_count"), TypeCounts.FindRef(TEXT("SExpandableArea")));
		Root->SetNumberField(TEXT("selectable_text_count"), TypeCounts.FindRef(TEXT("SMultiLineEditableText")));
		Root->SetNumberField(TEXT("editable_prompt_count"), TypeCounts.FindRef(TEXT("SMultiLineEditableTextBox")));
		Root->SetNumberField(TEXT("text_label_count"), TextLabels.Num());
		Root->SetStringField(TEXT("rendered_chat_screenshot_path"), ScreenshotMetrics.ScreenshotPath);
		Root->SetBoolField(TEXT("rendered_chat_screenshot_captured"), ScreenshotMetrics.bRawCaptureSucceeded);
		Root->SetNumberField(TEXT("rendered_chat_screenshot_width_px"), ScreenshotMetrics.Width);
		Root->SetNumberField(TEXT("rendered_chat_screenshot_height_px"), ScreenshotMetrics.Height);
		Root->SetNumberField(TEXT("rendered_chat_screenshot_pixel_count"), ScreenshotMetrics.PixelCount);
		Root->SetNumberField(TEXT("rendered_chat_unique_color_count"), ScreenshotMetrics.UniqueColorCount);
		Root->SetNumberField(TEXT("rendered_chat_non_black_pixel_count"), ScreenshotMetrics.NonBlackPixelCount);
		Root->SetNumberField(TEXT("rendered_chat_min_required_non_black_pixels"), ScreenshotMetrics.MinRequiredNonBlackPixels);
		Root->SetBoolField(TEXT("rendered_chat_visual_evidence_usable"), ScreenshotMetrics.bUsableEvidence);
		Root->SetStringField(TEXT("rendered_chat_visual_evidence_failure_reason"), ScreenshotMetrics.FailureReason);

		const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), TEXT("packet724d_chat_visual_contract"));
		IFileManager::Get().MakeDirectory(*OutputDir, true);
		const FString OutputPath = FPaths::Combine(OutputDir, TEXT("packet724d_chat_visual_contract_v1.receipt.json"));

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Root, Writer);
		FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return OutputPath;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FChatVisualContract_DocumentMessagesToolRowsAndComposer,
	"OsvayderUE.Widget.VisualContract.ChatToolRowsComposer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChatVisualContract_DocumentMessagesToolRowsAndComposer::RunTest(const FString& /*Parameters*/)
{
	TSharedRef<SWidget> Fixture =
		SNew(SBox)
		.WidthOverride(520.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SChatMessage)
				.Message(TEXT("Please inspect the changed Slate widget and keep the response concise."))
				.IsUser(true)
				.AssistantLabel(TEXT("Osvayder UE"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SChatMessage)
				.Message(TEXT("The message body uses proportional editor text while paths like D:/Project/File.cpp remain readable."))
				.IsUser(false)
				.AssistantLabel(TEXT("Osvayder UE"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SOsvayderToolCallRow)
				.ToolName(FText::FromString(TEXT("command_execution")))
				.StatusText(FText::FromString(TEXT("Running")))
				.StatusColor(FSlateColor(FLinearColor(0.130f, 0.760f, 0.920f, 1.0f)))
				.ResultText(FText::FromString(TEXT("waiting for process output...")))
				.bResultVisible(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SOsvayderToolCallRow)
				.ToolName(FText::FromString(TEXT("read_file")))
				.StatusText(FText::FromString(TEXT("Completed")))
				.StatusColor(FSlateColor(FLinearColor(0.320f, 0.780f, 0.480f, 1.0f)))
				.ResultText(FText::FromString(TEXT("D:/Project/Source/File.cpp:42")))
				.bResultVisible(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SOsvayderToolCallRow)
				.ToolName(FText::FromString(TEXT("policy_gate")))
				.StatusText(FText::FromString(TEXT("Blocked")))
				.StatusColor(FSlateColor(FLinearColor(0.840f, 0.520f, 0.250f, 1.0f)))
				.ResultText(FText::FromString(TEXT("blocked: unsafe scope")))
				.bResultVisible(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SOsvayderToolCallRow)
				.ToolName(FText::FromString(TEXT("compile")))
				.StatusText(FText::FromString(TEXT("Failed")))
				.StatusColor(FSlateColor(FLinearColor(0.900f, 0.300f, 0.300f, 1.0f)))
				.ResultText(FText::FromString(TEXT("error C2143: syntax error")))
				.bResultVisible(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SOsvayderInputArea)
				.bIsWaiting(false)
				.DictationState(EOsvayderUEVoiceDictationState::Idle)
				.DictationStatusText(FText::GetEmpty())
			]
		];

	TMap<FString, int32> TypeCounts;
	TSet<FString> TextLabels;
	OsvayderUEChatVisualContractTests::CollectWidgetTree(Fixture, TypeCounts, TextLabels);

	TestTrue(TEXT("visual fixture should include compact expandable tool rows"), TypeCounts.FindRef(TEXT("SExpandableArea")) >= 4);
	TestTrue(TEXT("user message chip should be present"), TextLabels.Contains(TEXT("You")));
	TestTrue(TEXT("assistant response meta should be present"), TextLabels.Contains(TEXT("Response")));
	TestTrue(TEXT("composer chip should be present"), TextLabels.Contains(TEXT("Composer")));
	TestTrue(TEXT("running tool status should be present"), TextLabels.Contains(TEXT("Running")));
	TestTrue(TEXT("completed tool status should be present"), TextLabels.Contains(TEXT("Completed")));
	TestTrue(TEXT("blocked tool status should be present"), TextLabels.Contains(TEXT("Blocked")));
	TestTrue(TEXT("failed tool status should be present"), TextLabels.Contains(TEXT("Failed")));
	TestFalse(TEXT("composer should not expose a visible Paste button"), TextLabels.Contains(TEXT("Paste")));
	TestTrue(TEXT("chat and tool output should use selectable read-only text"), TypeCounts.FindRef(TEXT("SMultiLineEditableText")) >= 6);
	TestEqual(TEXT("composer should keep exactly one editable prompt input"), TypeCounts.FindRef(TEXT("SMultiLineEditableTextBox")), 1);
	TestTrue(TEXT("composer min height should make the editable area the main surface"), OsvayderSlateStyle::Metrics::ComposerMinHeight >= 120.0f);
	TestTrue(TEXT("composer max height should allow the prompt box to use available vertical space"), OsvayderSlateStyle::Metrics::ComposerMaxHeight >= 300.0f);
	TestFalse(TEXT("composer should not wrap the prompt input in a nested SScrollBox"), TypeCounts.Contains(TEXT("SScrollBox")));

	const OsvayderUEChatVisualContractTests::FVisualEvidenceMetrics ScreenshotMetrics =
		OsvayderUEChatVisualContractTests::CaptureWidgetScreenshot(Fixture);
	TestTrue(TEXT("chat visual contract screenshot should be captured"), ScreenshotMetrics.bRawCaptureSucceeded);
	TestTrue(TEXT("chat screenshot should contain more than one unique color"), ScreenshotMetrics.UniqueColorCount > 1);
	TestTrue(TEXT("chat screenshot should contain enough non-black pixels for usable visual proof"),
		ScreenshotMetrics.NonBlackPixelCount >= ScreenshotMetrics.MinRequiredNonBlackPixels);
	TestTrue(TEXT("chat screenshot should be usable visual evidence"), ScreenshotMetrics.bUsableEvidence);

	const FString ReceiptPath = OsvayderUEChatVisualContractTests::WriteReceipt(TypeCounts, TextLabels, ScreenshotMetrics);
	AddInfo(FString::Printf(TEXT("Chat visual contract receipt: %s"), *ReceiptPath));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
