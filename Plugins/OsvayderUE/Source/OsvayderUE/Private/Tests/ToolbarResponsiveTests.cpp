// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "OsvayderEditorWidget.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/SlateRenderer.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOsvayderToolbar.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace OsvayderUEToolbarResponsiveTests
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

	FString WriteToolbarReceipt(
		const TMap<FString, int32>& TypeCounts,
		const TSet<FString>& TextLabels,
		const TArray<FString>& MissingPrimaryLabels,
		const TArray<FString>& PrimaryLabels,
		const TArray<FString>& OverflowLabels,
		const TArray<FString>& HiddenMandatoryLabels,
		const FVisualEvidenceMetrics& ScreenshotMetrics)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("packet"), TEXT("packet696_context_toggles_toolbar_contract_v1"));
		Root->SetStringField(TEXT("presentation"), TEXT("mandatory_context_status_plus_direct_deferred_tasks"));
		Root->SetNumberField(TEXT("narrow_width_px"), 360.0);
		Root->SetBoolField(TEXT("has_more_combo_button"), TypeCounts.Contains(TEXT("SComboButton")));
		Root->SetBoolField(TEXT("has_direct_deferred_tasks_button"), TextLabels.Contains(TEXT("Deferred Tasks")));
		Root->SetNumberField(TEXT("button_count"), TypeCounts.FindRef(TEXT("SButton")));
		Root->SetNumberField(TEXT("checkbox_count"), TypeCounts.FindRef(TEXT("SCheckBox")));
		Root->SetBoolField(TEXT("provider_status_compact"), TextLabels.Contains(TEXT("Codex CLI ready")));
		Root->SetBoolField(TEXT("model_chip_label_visible"), TextLabels.Contains(TEXT("Model")));
		Root->SetBoolField(TEXT("model_chip_value_visible"), TextLabels.Contains(TEXT("gpt-5.4")));
		Root->SetBoolField(TEXT("profile_chip_label_visible"), TextLabels.Contains(TEXT("Profile")));
		Root->SetBoolField(TEXT("profile_chip_value_visible"), TextLabels.Contains(TEXT("Expert")));
		Root->SetBoolField(TEXT("context_status_label_present"), TextLabels.Contains(TEXT("Context: UE + Project")));
		Root->SetBoolField(TEXT("ue57_context_label_visible"), TextLabels.Contains(TEXT("UE5.7 Context")));
		Root->SetBoolField(TEXT("project_context_label_visible"), TextLabels.Contains(TEXT("Project Context")));
		Root->SetBoolField(TEXT("no_visible_paste_button"), !TextLabels.Contains(TEXT("Paste")));

		TArray<TSharedPtr<FJsonValue>> ActionNames;
		for (const FClaudeToolbarActionDescriptor& Descriptor : SOsvayderToolbar::GetActionDescriptors())
		{
			ActionNames.Add(MakeShared<FJsonValueString>(Descriptor.Name.ToString()));
		}
		Root->SetArrayField(TEXT("action_names"), ActionNames);

		TArray<TSharedPtr<FJsonValue>> Missing;
		for (const FString& Label : MissingPrimaryLabels)
		{
			Missing.Add(MakeShared<FJsonValueString>(Label));
		}
		Root->SetArrayField(TEXT("missing_primary_action_labels"), Missing);
		Root->SetBoolField(TEXT("all_primary_action_labels_present"), MissingPrimaryLabels.Num() == 0);

		TArray<TSharedPtr<FJsonValue>> Primary;
		for (const FString& Label : PrimaryLabels)
		{
			Primary.Add(MakeShared<FJsonValueString>(Label));
		}
		Root->SetArrayField(TEXT("primary_action_labels"), Primary);

		TArray<TSharedPtr<FJsonValue>> Overflow;
		for (const FString& Label : OverflowLabels)
		{
			Overflow.Add(MakeShared<FJsonValueString>(Label));
		}
		Root->SetArrayField(TEXT("overflow_action_labels"), Overflow);

		TArray<TSharedPtr<FJsonValue>> HiddenMandatory;
		for (const FString& Label : HiddenMandatoryLabels)
		{
			HiddenMandatory.Add(MakeShared<FJsonValueString>(Label));
		}
		Root->SetArrayField(TEXT("hidden_mandatory_action_labels"), HiddenMandatory);
		Root->SetBoolField(TEXT("deferred_tasks_model_label_english"), OverflowLabels.Contains(TEXT("Deferred Tasks")));
		Root->SetNumberField(TEXT("observed_text_label_count"), TextLabels.Num());
		Root->SetStringField(TEXT("rendered_toolbar_screenshot_path"), ScreenshotMetrics.ScreenshotPath);
		Root->SetBoolField(TEXT("rendered_toolbar_screenshot_captured"), ScreenshotMetrics.bRawCaptureSucceeded);
		Root->SetNumberField(TEXT("rendered_toolbar_screenshot_width_px"), ScreenshotMetrics.Width);
		Root->SetNumberField(TEXT("rendered_toolbar_screenshot_height_px"), ScreenshotMetrics.Height);
		Root->SetNumberField(TEXT("rendered_toolbar_screenshot_pixel_count"), ScreenshotMetrics.PixelCount);
		Root->SetNumberField(TEXT("rendered_toolbar_unique_color_count"), ScreenshotMetrics.UniqueColorCount);
		Root->SetNumberField(TEXT("rendered_toolbar_non_black_pixel_count"), ScreenshotMetrics.NonBlackPixelCount);
		Root->SetNumberField(TEXT("rendered_toolbar_min_required_non_black_pixels"), ScreenshotMetrics.MinRequiredNonBlackPixels);
		Root->SetBoolField(TEXT("rendered_toolbar_visual_evidence_usable"), ScreenshotMetrics.bUsableEvidence);
		Root->SetStringField(TEXT("rendered_toolbar_visual_evidence_failure_reason"), ScreenshotMetrics.FailureReason);

		const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), TEXT("packet696_context_toggles_toolbar_contract"));
		IFileManager::Get().MakeDirectory(*OutputDir, true);
		const FString OutputPath = FPaths::Combine(OutputDir, TEXT("packet696_context_toggles_toolbar_contract_v1.receipt.json"));

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Root, Writer);
		FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return OutputPath;
	}

	FVisualEvidenceMetrics CaptureToolbarScreenshot(const TSharedRef<SWidget>& ToolbarWidget)
	{
		FVisualEvidenceMetrics Metrics;
		if (!FSlateApplication::IsInitialized())
		{
			Metrics.FailureReason = TEXT("slate_not_initialized");
			return Metrics;
		}

		const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), TEXT("packet696_context_toggles_toolbar_contract"));
		IFileManager::Get().MakeDirectory(*OutputDir, true);

		TSharedRef<SWindow> PreviewWindow =
			SNew(SWindow)
			.Title(FText::FromString(TEXT("packet696_context_toggles_toolbar_preview")))
			.ClientSize(FVector2D(680.0f, 220.0f))
			.SupportsMinimize(false)
			.SupportsMaximize(false);
		PreviewWindow->SetContent(ToolbarWidget);
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
		const FString BitmapPattern = FPaths::Combine(OutputDir, TEXT("packet696_context_toggles_toolbar_render.bmp"));
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolbarResponsive_ActionModelContainsExpectedActions,
	"OsvayderUE.Toolbar.Responsive.ActionModelContainsExpectedActions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FToolbarResponsive_ActionModelContainsExpectedActions::RunTest(const FString& /*Parameters*/)
{
	const TArray<FClaudeToolbarActionDescriptor>& Descriptors = SOsvayderToolbar::GetActionDescriptors();
	TestEqual(TEXT("toolbar action model should contain all existing actions"), Descriptors.Num(), 9);

	TSet<FName> Names;
	int32 ToggleCount = 0;
	int32 PrimaryCount = 0;
	int32 OverflowCount = 0;
	int32 HiddenMandatoryCount = 0;
	for (const FClaudeToolbarActionDescriptor& Descriptor : Descriptors)
	{
		TestFalse(TEXT("action name should not be None"), Descriptor.Name.IsNone());
		TestFalse(FString::Printf(TEXT("action %s should have a label"), *Descriptor.Name.ToString()), Descriptor.Label.IsEmpty());
		TestFalse(FString::Printf(TEXT("action %s should not be duplicated"), *Descriptor.Name.ToString()), Names.Contains(Descriptor.Name));
		Names.Add(Descriptor.Name);
		if (Descriptor.bToggle)
		{
			++ToggleCount;
		}
		if (Descriptor.bPrimaryToolbarAction)
		{
			++PrimaryCount;
		}
		else if (Descriptor.bScrollableAction)
		{
			++OverflowCount;
		}
		else
		{
			++HiddenMandatoryCount;
		}
	}

	TestTrue(TEXT("UE5.7 context action should be present"), Names.Contains(TEXT("ue57_context")));
	TestTrue(TEXT("Project context action should be present"), Names.Contains(TEXT("project_context")));
	TestTrue(TEXT("Refresh action should be present"), Names.Contains(TEXT("refresh_context")));
	TestTrue(TEXT("Restore action should be present"), Names.Contains(TEXT("restore_session")));
	TestTrue(TEXT("Restart survival action should be present"), Names.Contains(TEXT("restart_survival")));
	TestTrue(TEXT("Deferred tasks action should be present"), Names.Contains(TEXT("deferred_tasks")));
	TestTrue(TEXT("Browser verify action should be present"), Names.Contains(TEXT("browser_verify")));
	TestTrue(TEXT("New session action should be present"), Names.Contains(TEXT("new_session")));
	TestTrue(TEXT("Copy last action should be present"), Names.Contains(TEXT("copy_last")));
	TestEqual(TEXT("only the two context actions are toggles"), ToggleCount, 2);
	TestEqual(TEXT("only New Session remains a primary action"), PrimaryCount, 1);
	TestEqual(TEXT("only Deferred Tasks remains in overflow"), OverflowCount, 1);
	TestEqual(TEXT("mandatory context toggles and automated actions should be hidden from toolbar and overflow"), HiddenMandatoryCount, 7);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolbarResponsive_MandatoryContextsNormalizeOn,
	"OsvayderUE.Toolbar.Responsive.MandatoryContextsNormalizeOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FToolbarResponsive_MandatoryContextsNormalizeOn::RunTest(const FString& /*Parameters*/)
{
	bool bIncludeUE57Context = false;
	bool bIncludeProjectContext = false;

	SOsvayderEditorWidget::NormalizeMandatoryContextFlagsForNormalAssistantRun(
		bIncludeUE57Context,
		bIncludeProjectContext);

	TestTrue(TEXT("UE5.7 context is forced on for normal assistant runs"), bIncludeUE57Context);
	TestTrue(TEXT("Project context is forced on for normal assistant runs"), bIncludeProjectContext);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolbarResponsive_NarrowWidgetExposesEveryAction,
	"OsvayderUE.Toolbar.Responsive.NarrowWidgetExposesEveryAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FToolbarResponsive_NarrowWidgetExposesEveryAction::RunTest(const FString& /*Parameters*/)
{
	TSharedRef<SWidget> Toolbar =
		SNew(SOsvayderToolbar)
		.TitleText(FText::FromString(TEXT("Osvayder UE")))
		.ProviderSummaryText(FText::FromString(TEXT("Codex CLI ready")))
		.ProviderSummaryToolTip(FText::FromString(TEXT("Provider status tooltip")))
		.ModelText(FText::FromString(TEXT("gpt-5.4")))
		.ModelToolTip(FText::FromString(TEXT("Model tooltip")))
		.ProfileText(FText::FromString(TEXT("Expert")))
		.ProfileToolTip(FText::FromString(TEXT("Profile tooltip")))
		.bUE57ContextEnabled(true)
		.bProjectContextEnabled(true)
		.bRestoreEnabled(true)
		.bRestartSurvivalEnabled(true)
		.bShowBrowserVerify(true)
		.bBrowserVerifyEnabled(true)
		.DeferredTasksCount(3)
		.RestoreToolTip(FText::FromString(TEXT("Restore tooltip")))
		.RestartSurvivalToolTip(FText::FromString(TEXT("Restart tooltip")))
		.BrowserVerifyToolTip(FText::FromString(TEXT("Browser verify tooltip")));

	TSharedRef<SWidget> NarrowToolbar =
		SNew(SBox)
		.WidthOverride(360.0f)
		[
			Toolbar
		];

	TMap<FString, int32> TypeCounts;
	TSet<FString> TextLabels;
	OsvayderUEToolbarResponsiveTests::CollectWidgetTree(NarrowToolbar, TypeCounts, TextLabels);

	TestFalse(TEXT("narrow toolbar should not need a one-item More menu"), TypeCounts.Contains(TEXT("SComboButton")));
	TestFalse(TEXT("narrow toolbar should no longer rely on horizontal scroll for actions"), TypeCounts.Contains(TEXT("SScrollBox")));
	TestTrue(TEXT("toolbar should expose direct actions as buttons"), TypeCounts.FindRef(TEXT("SButton")) >= 2);
	TestEqual(TEXT("mandatory context toggles should not render as checkboxes"), TypeCounts.FindRef(TEXT("SCheckBox")), 0);
	TestTrue(TEXT("header should show Osvayder UE"), TextLabels.Contains(TEXT("Osvayder UE")));
	TestTrue(TEXT("provider chip should show a compact readiness summary"), TextLabels.Contains(TEXT("Codex CLI ready")));
	TestTrue(TEXT("model chip label should be visible in the representative fixture"), TextLabels.Contains(TEXT("Model")));
	TestTrue(TEXT("model chip value should be visible in the representative fixture"), TextLabels.Contains(TEXT("gpt-5.4")));
	TestTrue(TEXT("profile chip label should be visible in the representative fixture"), TextLabels.Contains(TEXT("Profile")));
	TestTrue(TEXT("profile chip value should be visible in the representative fixture"), TextLabels.Contains(TEXT("Expert")));
	TestTrue(TEXT("mandatory context status label should be present"), TextLabels.Contains(TEXT("Context: UE + Project")));
	TestFalse(TEXT("UE5.7 context toggle label should be hidden"), TextLabels.Contains(TEXT("UE5.7 Context")));
	TestFalse(TEXT("Project context toggle label should be hidden"), TextLabels.Contains(TEXT("Project Context")));
	TestTrue(TEXT("New Session stays visible as a priority action"), TextLabels.Contains(TEXT("New Session")));
	TestTrue(TEXT("Deferred Tasks should stay directly reachable"), TextLabels.Contains(TEXT("Deferred Tasks")));
	TestFalse(TEXT("toolbar should not expose a visible Paste button"), TextLabels.Contains(TEXT("Paste")));

	TArray<FString> MissingPrimaryLabels;
	TArray<FString> PrimaryLabels;
	TArray<FString> OverflowLabels;
	TArray<FString> HiddenMandatoryLabels;
	for (const FClaudeToolbarActionDescriptor& Descriptor : SOsvayderToolbar::GetActionDescriptors())
	{
		const FString Label = Descriptor.Label.ToString();
		if (!Descriptor.bScrollableAction)
		{
			HiddenMandatoryLabels.Add(Label);
		}
		else if (Descriptor.bPrimaryToolbarAction)
		{
			PrimaryLabels.Add(Label);
			if (!TextLabels.Contains(Label))
			{
				MissingPrimaryLabels.Add(Label);
			}
		}
		else
		{
			OverflowLabels.Add(Label);
		}
	}

	TestTrue(TEXT("Deferred Tasks should be the only English overflow model label"), OverflowLabels.Contains(TEXT("Deferred Tasks")));
	TestFalse(TEXT("More label should not be present when Deferred Tasks is direct"), TextLabels.Contains(TEXT("More")));
	TestFalse(TEXT("Restore Session should stay out of the Deferred Tasks model slot"), OverflowLabels.Contains(TEXT("Restore Session")));
	TestFalse(TEXT("Restart Survival should stay out of the Deferred Tasks model slot"), OverflowLabels.Contains(TEXT("Restart Survival")));
	TestFalse(TEXT("Copy Last should stay out of the Deferred Tasks model slot"), OverflowLabels.Contains(TEXT("Copy Last")));
	TestFalse(TEXT("Refresh Context should stay out of the Deferred Tasks model slot"), OverflowLabels.Contains(TEXT("Refresh Context")));
	TestFalse(TEXT("Browser Verify should stay out of the Deferred Tasks model slot"), OverflowLabels.Contains(TEXT("Browser Verify")));
	TestEqual(TEXT("Deferred Tasks should remain the only modeled overflow action"), OverflowLabels.Num(), 1);
	TestEqual(TEXT("visible Deferred Tasks label should stay English"),
		SOsvayderToolbar::GetDeferredTasksMenuLabel().ToString(),
		FString(TEXT("Deferred Tasks")));
	TestTrue(TEXT("UE5.7 context should be classified as hidden mandatory"), HiddenMandatoryLabels.Contains(TEXT("UE5.7 Context")));
	TestTrue(TEXT("Project context should be classified as hidden mandatory"), HiddenMandatoryLabels.Contains(TEXT("Project Context")));
	TestEqual(TEXT("all primary action labels should be present in the narrow toolbar widget tree"), MissingPrimaryLabels.Num(), 0);

	const OsvayderUEToolbarResponsiveTests::FVisualEvidenceMetrics ScreenshotMetrics =
		OsvayderUEToolbarResponsiveTests::CaptureToolbarScreenshot(NarrowToolbar);
	TestTrue(TEXT("toolbar render screenshot should be captured for visual QA"), ScreenshotMetrics.bRawCaptureSucceeded);
	TestTrue(TEXT("toolbar screenshot should contain more than one unique color"), ScreenshotMetrics.UniqueColorCount > 1);
	TestTrue(TEXT("toolbar screenshot should contain enough non-black pixels for usable visual proof"),
		ScreenshotMetrics.NonBlackPixelCount >= ScreenshotMetrics.MinRequiredNonBlackPixels);
	TestTrue(TEXT("toolbar screenshot should be usable visual evidence"), ScreenshotMetrics.bUsableEvidence);

	const FString ReceiptPath = OsvayderUEToolbarResponsiveTests::WriteToolbarReceipt(
		TypeCounts,
		TextLabels,
		MissingPrimaryLabels,
		PrimaryLabels,
		OverflowLabels,
		HiddenMandatoryLabels,
		ScreenshotMetrics);
	AddInfo(FString::Printf(TEXT("Toolbar responsive receipt: %s"), *ReceiptPath));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
