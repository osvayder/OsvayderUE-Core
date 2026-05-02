// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "ClaudeEditorWidget.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SClaudeToolbar.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UnrealClaudeToolbarResponsiveTests
{
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

	FString WriteToolbarReceipt(
		const TMap<FString, int32>& TypeCounts,
		const TSet<FString>& TextLabels,
		const TArray<FString>& MissingPrimaryLabels,
		const TArray<FString>& PrimaryLabels,
		const TArray<FString>& OverflowLabels,
		const TArray<FString>& HiddenMandatoryLabels,
		const FString& ScreenshotPath,
		const bool bScreenshotCaptured)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("packet"), TEXT("packet696_context_toggles_toolbar_contract_v1"));
		Root->SetStringField(TEXT("presentation"), TEXT("mandatory_context_status_plus_more_overflow"));
		Root->SetNumberField(TEXT("narrow_width_px"), 360.0);
		Root->SetBoolField(TEXT("has_more_combo_button"), TypeCounts.Contains(TEXT("SComboButton")));
		Root->SetNumberField(TEXT("button_count"), TypeCounts.FindRef(TEXT("SButton")));
		Root->SetNumberField(TEXT("checkbox_count"), TypeCounts.FindRef(TEXT("SCheckBox")));
		Root->SetBoolField(TEXT("context_status_label_present"), TextLabels.Contains(TEXT("Context: UE + Project")));
		Root->SetBoolField(TEXT("ue57_context_label_visible"), TextLabels.Contains(TEXT("UE5.7 Context")));
		Root->SetBoolField(TEXT("project_context_label_visible"), TextLabels.Contains(TEXT("Project Context")));

		TArray<TSharedPtr<FJsonValue>> ActionNames;
		for (const FClaudeToolbarActionDescriptor& Descriptor : SClaudeToolbar::GetActionDescriptors())
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
		Root->SetBoolField(TEXT("deferred_label_readable_cyrillic"), OverflowLabels.Contains(TEXT("Отложенные задачи")));
		Root->SetNumberField(TEXT("observed_text_label_count"), TextLabels.Num());
		Root->SetStringField(TEXT("rendered_toolbar_screenshot_path"), ScreenshotPath);
		Root->SetBoolField(TEXT("rendered_toolbar_screenshot_captured"), bScreenshotCaptured);

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

	bool CaptureToolbarScreenshot(const TSharedRef<SWidget>& ToolbarWidget, FString& OutScreenshotPath)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
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
		FSlateApplication::Get().Tick();

		TArray<FColor> ColorData;
		FIntVector ImageSize = FIntVector::ZeroValue;
		const bool bCaptured = FSlateApplication::Get().TakeScreenshot(PreviewWindow, ColorData, ImageSize);

		FSlateApplication::Get().RequestDestroyWindow(PreviewWindow);
		FSlateApplication::Get().Tick();

		if (!bCaptured || ImageSize.X <= 0 || ImageSize.Y <= 0 || ColorData.Num() == 0)
		{
			return false;
		}

		FString ActualFilename;
		const FString BitmapPattern = FPaths::Combine(OutputDir, TEXT("packet696_context_toggles_toolbar_render.bmp"));
		FFileHelper::CreateBitmap(*BitmapPattern, ImageSize.X, ImageSize.Y, ColorData.GetData(), nullptr, &IFileManager::Get(), &ActualFilename, false);
		if (ActualFilename.IsEmpty())
		{
			ActualFilename = BitmapPattern;
		}

		OutScreenshotPath = ActualFilename;
		FPaths::NormalizeFilename(OutScreenshotPath);
		return IFileManager::Get().FileExists(*OutScreenshotPath);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolbarResponsive_ActionModelContainsExpectedActions,
	"UnrealClaude.Toolbar.Responsive.ActionModelContainsExpectedActions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FToolbarResponsive_ActionModelContainsExpectedActions::RunTest(const FString& /*Parameters*/)
{
	const TArray<FClaudeToolbarActionDescriptor>& Descriptors = SClaudeToolbar::GetActionDescriptors();
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
	TestEqual(TEXT("six lower-frequency controls should move into overflow"), OverflowCount, 6);
	TestEqual(TEXT("two mandatory context toggles should be hidden from toolbar and overflow"), HiddenMandatoryCount, 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolbarResponsive_MandatoryContextsNormalizeOn,
	"UnrealClaude.Toolbar.Responsive.MandatoryContextsNormalizeOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FToolbarResponsive_MandatoryContextsNormalizeOn::RunTest(const FString& /*Parameters*/)
{
	bool bIncludeUE57Context = false;
	bool bIncludeProjectContext = false;

	SClaudeEditorWidget::NormalizeMandatoryContextFlagsForNormalAssistantRun(
		bIncludeUE57Context,
		bIncludeProjectContext);

	TestTrue(TEXT("UE5.7 context is forced on for normal assistant runs"), bIncludeUE57Context);
	TestTrue(TEXT("Project context is forced on for normal assistant runs"), bIncludeProjectContext);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolbarResponsive_NarrowWidgetExposesEveryAction,
	"UnrealClaude.Toolbar.Responsive.NarrowWidgetExposesEveryAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FToolbarResponsive_NarrowWidgetExposesEveryAction::RunTest(const FString& /*Parameters*/)
{
	TSharedRef<SWidget> Toolbar =
		SNew(SClaudeToolbar)
		.TitleText(FText::FromString(TEXT("Osvayder UE")))
		.ProviderSummaryText(FText::FromString(TEXT("Codex CLI [default] profile + auth mode + speed + reasoning + verbosity")))
		.ProviderSummaryToolTip(FText::FromString(TEXT("Provider status tooltip")))
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
	UnrealClaudeToolbarResponsiveTests::CollectWidgetTree(NarrowToolbar, TypeCounts, TextLabels);

	TestTrue(TEXT("narrow toolbar should include a More/overflow combo button"), TypeCounts.Contains(TEXT("SComboButton")));
	TestFalse(TEXT("narrow toolbar should no longer rely on horizontal scroll for actions"), TypeCounts.Contains(TEXT("SScrollBox")));
	TestTrue(TEXT("primary New Session action should remain accessible as a button"), TypeCounts.FindRef(TEXT("SButton")) >= 1);
	TestEqual(TEXT("mandatory context toggles should not render as checkboxes"), TypeCounts.FindRef(TEXT("SCheckBox")), 0);
	TestTrue(TEXT("header should show Osvayder UE"), TextLabels.Contains(TEXT("Osvayder UE")));
	TestTrue(TEXT("mandatory context status label should be present"), TextLabels.Contains(TEXT("Context: UE + Project")));
	TestFalse(TEXT("UE5.7 context toggle label should be hidden"), TextLabels.Contains(TEXT("UE5.7 Context")));
	TestFalse(TEXT("Project context toggle label should be hidden"), TextLabels.Contains(TEXT("Project Context")));
	TestTrue(TEXT("More overflow label should be present"), TextLabels.Contains(TEXT("More")));
	TestTrue(TEXT("New Session stays visible as a priority action"), TextLabels.Contains(TEXT("New Session")));

	TArray<FString> MissingPrimaryLabels;
	TArray<FString> PrimaryLabels;
	TArray<FString> OverflowLabels;
	TArray<FString> HiddenMandatoryLabels;
	for (const FClaudeToolbarActionDescriptor& Descriptor : SClaudeToolbar::GetActionDescriptors())
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

	TestTrue(TEXT("deferred tasks label should be readable Cyrillic in the overflow model"), OverflowLabels.Contains(TEXT("Отложенные задачи")));
	TestTrue(TEXT("restore action should stay available in overflow"), OverflowLabels.Contains(TEXT("Restore Session")));
	TestTrue(TEXT("restart survival action should stay available in overflow"), OverflowLabels.Contains(TEXT("Restart Survival")));
	TestTrue(TEXT("copy last action should stay available in overflow"), OverflowLabels.Contains(TEXT("Copy Last")));
	TestTrue(TEXT("UE5.7 context should be classified as hidden mandatory"), HiddenMandatoryLabels.Contains(TEXT("UE5.7 Context")));
	TestTrue(TEXT("Project context should be classified as hidden mandatory"), HiddenMandatoryLabels.Contains(TEXT("Project Context")));
	TestEqual(TEXT("all primary action labels should be present in the narrow toolbar widget tree"), MissingPrimaryLabels.Num(), 0);

	FString ScreenshotPath;
	const bool bScreenshotCaptured = UnrealClaudeToolbarResponsiveTests::CaptureToolbarScreenshot(NarrowToolbar, ScreenshotPath);
	TestTrue(TEXT("toolbar render screenshot should be captured for visual QA"), bScreenshotCaptured);

	const FString ReceiptPath = UnrealClaudeToolbarResponsiveTests::WriteToolbarReceipt(TypeCounts, TextLabels, MissingPrimaryLabels, PrimaryLabels, OverflowLabels, HiddenMandatoryLabels, ScreenshotPath, bScreenshotCaptured);
	AddInfo(FString::Printf(TEXT("Toolbar responsive receipt: %s"), *ReceiptPath));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
