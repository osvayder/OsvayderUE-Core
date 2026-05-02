// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeDeferredTasksBrowser.h"

#include "ClaudeSubsystem.h"
#include "JsonUtils.h"
#include "UnrealClaudeAgentTrace.h"
#include "UnrealClaudeModule.h"
#include "UnrealClaudeTaskRecoveryDialog.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UnrealClaudeDeferredTasks"

namespace
{
	constexpr float BrowserWidth = 920.0f;
	constexpr float BrowserHeight = 680.0f;
	constexpr float DetailsWidth = 980.0f;
	constexpr float DetailsHeight = 720.0f;

	FString TruncateAtWordBoundary(const FString& Input, const int32 MaxChars)
	{
		if (Input.Len() <= MaxChars)
		{
			return Input;
		}

		int32 Cut = MaxChars;
		while (Cut > 0 && !FChar::IsWhitespace(Input[Cut]))
		{
			--Cut;
		}
		if (Cut <= 0)
		{
			Cut = MaxChars;
		}

		return Input.Left(Cut) + TEXT(" ...");
	}

	bool TryParseIso8601Utc(const FString& IsoText, FDateTime& OutDateTime)
	{
		if (IsoText.IsEmpty())
		{
			return false;
		}

		return FDateTime::ParseIso8601(*IsoText, OutDateTime);
	}

	FString MakeArchiveSafeFileTag(const FString& Value)
	{
		FString Safe = Value;
		for (const TCHAR Character : { TEXT('\\'), TEXT('/'), TEXT(':'), TEXT('*'), TEXT('?'), TEXT('"'), TEXT('<'), TEXT('>'), TEXT('|'), TEXT(' ') })
		{
			Safe.ReplaceCharInline(Character, TEXT('_'));
		}
		return Safe;
	}

	FString BuildDetailsText(const FUnrealClaudeActivePlan& Plan, const FString& ArchivePath)
	{
		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("Archive file: %s"), *ArchivePath));
		Lines.Add(FString::Printf(TEXT("PlanId: %s"), *Plan.PlanId));
		Lines.Add(FString::Printf(TEXT("Status: %s"), *Plan.Status));
		Lines.Add(FString::Printf(TEXT("ResultStatus: %s"), *Plan.ResultStatus));
		Lines.Add(FString::Printf(TEXT("CreatedAtUtc: %s"), *Plan.CreatedAtUtc));
		Lines.Add(FString::Printf(TEXT("UpdatedAtUtc: %s"), *Plan.UpdatedAtUtc));
		Lines.Add(FString::Printf(TEXT("CurrentMechanicId: %s"), *Plan.CurrentMechanicId));
		Lines.Add(FString::Printf(TEXT("CurrentToolCallId: %s"), *Plan.CurrentToolCallId));
		Lines.Add(FString::Printf(TEXT("CurrentAction: %s"), *Plan.CurrentAction));
		Lines.Add(FString::Printf(TEXT("CurrentActionRu: %s"), *Plan.CurrentActionRu));
		Lines.Add(FString::Printf(TEXT("ResumeHint: %s"), *Plan.ResumeHint));
		Lines.Add(FString::Printf(TEXT("UserRecoveryChoice: %s"), *Plan.UserRecoveryChoice));
		Lines.Add(FString::Printf(TEXT("UserClosedReason: %s"), *Plan.UserClosedReason));
		Lines.Add(FString::Printf(TEXT("InterruptionReason: %s"), *Plan.InterruptionReason));
		Lines.Add(FString::Printf(TEXT("InterruptionDetectedAtUtc: %s"), *Plan.InterruptionDetectedAtUtc));
		Lines.Add(FString::Printf(TEXT("InterruptionElapsedSeconds: %d"), Plan.InterruptionElapsedSeconds));
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("OriginalUserTask:"));
		Lines.Add(Plan.OriginalUserTask);
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("Summary:"));
		Lines.Add(Plan.Summary);
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("SummaryRu:"));
		Lines.Add(Plan.SummaryRu);
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("TechnicalDetail:"));
		Lines.Add(Plan.TechnicalDetail);
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("VerificationChecklist:"));
		if (Plan.VerificationChecklist.Num() == 0)
		{
			Lines.Add(TEXT("(none)"));
		}
		else
		{
			for (const FString& Item : Plan.VerificationChecklist)
			{
				Lines.Add(FString::Printf(TEXT("- %s"), *Item));
			}
		}

		Lines.Add(TEXT(""));
		Lines.Add(TEXT("Mechanics:"));
		if (Plan.Mechanics.Num() == 0)
		{
			Lines.Add(TEXT("(none)"));
		}
		else
		{
			for (int32 Index = 0; Index < Plan.Mechanics.Num(); ++Index)
			{
				const FUnrealClaudePlanMechanicEntry& Mechanic = Plan.Mechanics[Index];
				Lines.Add(FString::Printf(
					TEXT("%d. id=%s | status=%s | label=%s | label_ru=%s | last_summary=%s"),
					Index + 1,
					*Mechanic.MechanicId,
					*Mechanic.Status,
					*Mechanic.Label,
					*Mechanic.LabelRu,
					*Mechanic.LastSummary));
			}
		}

		Lines.Add(TEXT(""));
		Lines.Add(TEXT("ToolCalls:"));
		if (Plan.ToolCalls.Num() == 0)
		{
			Lines.Add(TEXT("(none)"));
		}
		else
		{
			for (int32 Index = 0; Index < Plan.ToolCalls.Num(); ++Index)
			{
				const FUnrealClaudePlanToolCallEntry& ToolCall = Plan.ToolCalls[Index];
				Lines.Add(FString::Printf(
					TEXT("%d. id=%s | mechanic=%s | tool=%s | status=%s | result=%s"),
					Index + 1,
					*ToolCall.ToolCallId,
					*ToolCall.MechanicId,
					*ToolCall.ToolName,
					*ToolCall.Status,
					*ToolCall.ResultStatus));
				if (!ToolCall.Summary.IsEmpty())
				{
					Lines.Add(FString::Printf(TEXT("    summary: %s"), *ToolCall.Summary));
				}
				if (!ToolCall.SummaryRu.IsEmpty())
				{
					Lines.Add(FString::Printf(TEXT("    summary_ru: %s"), *ToolCall.SummaryRu));
				}
				if (!ToolCall.TechnicalDetail.IsEmpty())
				{
					Lines.Add(FString::Printf(TEXT("    technical_detail: %s"), *ToolCall.TechnicalDetail));
				}
			}
		}

		return FString::Join(Lines, TEXT("\n"));
	}

	class FDeferredTasksBrowserController : public TSharedFromThis<FDeferredTasksBrowserController>
	{
	public:
		explicit FDeferredTasksBrowserController(const FUnrealClaudeDeferredTasksBrowser::FOnPlanResumed& InOnPlanResumed)
			: OnPlanResumed(InOnPlanResumed)
		{
		}

		void Show()
		{
			if (!FSlateApplication::IsInitialized())
			{
				UE_LOG(LogUnrealClaude, Warning, TEXT("DeferredTasksBrowser requires Slate application."));
				return;
			}

			RefreshEntries();
			Window = SNew(SWindow)
				.Title(LOCTEXT("DeferredTasksWindowTitle", "Отложенные задачи"))
				.ClientSize(FVector2D(BrowserWidth, BrowserHeight))
				.SupportsMaximize(false)
				.SupportsMinimize(false);
			Window->SetContent(BuildContent());
			FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(), nullptr);
		}

	private:
		void RefreshEntries()
		{
			Entries = FUnrealClaudeDeferredTasksEnumerator::ListDeferredPlans();
		}

		void Rebuild()
		{
			if (!Window.IsValid())
			{
				return;
			}

			RefreshEntries();
			Window->SetContent(BuildContent());
		}

		TSharedRef<SWidget> BuildContent()
		{
			return SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(12.0f, 12.0f, 12.0f, 6.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Отложенные задачи (%d)"), Entries.Num())))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(12.0f, 0.0f, 12.0f, 6.0f)
				[
					BuildEntriesList()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(12.0f, 6.0f, 12.0f, 12.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("DeferredTasksClose", "Закрыть"))
					.OnClicked_Lambda([WeakThis = TWeakPtr<FDeferredTasksBrowserController>(SharedThis(this))]()
					{
						const TSharedPtr<FDeferredTasksBrowserController> Pinned = WeakThis.Pin();
						if (Pinned.IsValid() && Pinned->Window.IsValid())
						{
							Pinned->Window->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				];
		}

		TSharedRef<SWidget> BuildEntriesList()
		{
			if (Entries.Num() == 0)
			{
				return SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.Padding(16.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT(
							"DeferredTasksEmptyState",
							"Нет отложенных задач. Задачи, которые вы отложите через 'Close as Irrelevant' или 'Start Fresh', появятся здесь."))
						.AutoWrapText(true)
					];
			}

			TSharedRef<SScrollBox> ScrollBox = SNew(SScrollBox);
			const FDateTime NowUtc = FDateTime::UtcNow();

			for (const FUnrealClaudeDeferredPlanEntry& Entry : Entries)
			{
				FUnrealClaudeActivePlan Plan;
				FString LoadError;
				const bool bPlanLoaded = FUnrealClaudeDeferredTasksBrowser::LoadArchivedPlan(Entry, Plan, LoadError);

				FTaskRecoverySummary Summary;
				if (bPlanLoaded)
				{
					Summary = FUnrealClaudeTaskRecoverySummarizer::BuildRecoverySummary(Plan, NowUtc, Entry.ArchiveFilePath);
				}
				else
				{
					Summary.ShortTitle = Entry.ShortTitle;
					Summary.ElapsedSincePause = TEXT("(unknown)");
					Summary.ProgressSummary = LoadError.IsEmpty()
						? TEXT("Не удалось прочитать архив задачи.")
						: LoadError;
				}

				ScrollBox->AddSlot()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					BuildEntryCard(Entry, Summary, bPlanLoaded, LoadError)
				];
			}

			return ScrollBox;
		}

		TSharedRef<SWidget> BuildEntryCard(
			const FUnrealClaudeDeferredPlanEntry& Entry,
			const FTaskRecoverySummary& Summary,
			const bool bPlanLoaded,
			const FString& LoadError)
		{
			return SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(12.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(Summary.ShortTitle.IsEmpty() ? Entry.ShortTitle : Summary.ShortTitle))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FUnrealClaudeDeferredTasksBrowser::BuildMetaLine(Entry, Summary)))
						.AutoWrapText(true)
						.ColorAndOpacity(FLinearColor(0.78f, 0.78f, 0.82f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Summary.ProgressSummary))
						.AutoWrapText(true)
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, LoadError.IsEmpty() ? 0.0f : 8.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(LoadError))
						.AutoWrapText(true)
						.Visibility(LoadError.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
						.ColorAndOpacity(FLinearColor(1.0f, 0.72f, 0.3f))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Right)
					.Padding(0.0f, 10.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("DeferredTasksResume", "Возобновить"))
							.IsEnabled(bPlanLoaded)
							.OnClicked_Lambda([WeakThis = TWeakPtr<FDeferredTasksBrowserController>(SharedThis(this)), Entry]()
							{
								const TSharedPtr<FDeferredTasksBrowserController> Pinned = WeakThis.Pin();
								return Pinned.IsValid() ? Pinned->HandleResume(Entry) : FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("DeferredTasksDetails", "Детали"))
							.IsEnabled(bPlanLoaded)
							.OnClicked_Lambda([WeakThis = TWeakPtr<FDeferredTasksBrowserController>(SharedThis(this)), Entry]()
							{
								const TSharedPtr<FDeferredTasksBrowserController> Pinned = WeakThis.Pin();
								if (Pinned.IsValid())
								{
									Pinned->ShowDetails(Entry);
								}
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("DeferredTasksDelete", "Удалить"))
							.OnClicked_Lambda([WeakThis = TWeakPtr<FDeferredTasksBrowserController>(SharedThis(this)), Entry]()
							{
								const TSharedPtr<FDeferredTasksBrowserController> Pinned = WeakThis.Pin();
								return Pinned.IsValid() ? Pinned->HandleDelete(Entry) : FReply::Handled();
							})
						]
					]
				];
		}

		FReply HandleResume(const FUnrealClaudeDeferredPlanEntry& Entry)
		{
			FString ShortTitle;
			FString Error;
			if (!FUnrealClaudeDeferredTasksBrowser::ResumePlan(Entry, ShortTitle, Error))
			{
				FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error.IsEmpty()
					? TEXT("Не удалось возобновить отложенную задачу.")
					: Error));
				return FReply::Handled();
			}

			if (OnPlanResumed.IsBound())
			{
				OnPlanResumed.Execute(ShortTitle);
			}
			if (Window.IsValid())
			{
				Window->RequestDestroyWindow();
			}
			return FReply::Handled();
		}

		FReply HandleDelete(const FUnrealClaudeDeferredPlanEntry& Entry)
		{
			const FString Prompt = FString::Printf(
				TEXT("Удалить задачу '%s' навсегда? Это действие необратимо."),
				*Entry.ShortTitle);
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Prompt)) != EAppReturnType::Yes)
			{
				return FReply::Handled();
			}

			FString Error;
			if (!FUnrealClaudeDeferredTasksBrowser::DeletePlan(Entry, Error))
			{
				FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error.IsEmpty()
					? TEXT("Не удалось удалить архив задачи.")
					: Error));
				return FReply::Handled();
			}

			Rebuild();
			return FReply::Handled();
		}

		void ShowDetails(const FUnrealClaudeDeferredPlanEntry& Entry)
		{
			FUnrealClaudeActivePlan Plan;
			FString Error;
			if (!FUnrealClaudeDeferredTasksBrowser::LoadArchivedPlan(Entry, Plan, Error))
			{
				FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error.IsEmpty()
					? TEXT("Не удалось прочитать архив задачи.")
					: Error));
				return;
			}

			TSharedRef<SWindow> DetailsWindow = SNew(SWindow)
				.Title(LOCTEXT("DeferredTaskDetailsTitle", "Детали отложенной задачи"))
				.ClientSize(FVector2D(DetailsWidth, DetailsHeight))
				.SupportsMaximize(false)
				.SupportsMinimize(false);
			DetailsWindow->SetContent(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(12.0f)
				[
					SNew(SMultiLineEditableTextBox)
					.IsReadOnly(true)
					.Text(FText::FromString(BuildDetailsText(Plan, Entry.ArchiveFilePath)))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(12.0f, 0.0f, 12.0f, 12.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("DeferredTaskDetailsClose", "Закрыть"))
					.OnClicked_Lambda([DetailsWindow]()
					{
						DetailsWindow->RequestDestroyWindow();
						return FReply::Handled();
					})
				]);
			FSlateApplication::Get().AddModalWindow(DetailsWindow, Window);
		}

		FUnrealClaudeDeferredTasksBrowser::FOnPlanResumed OnPlanResumed;
		TSharedPtr<SWindow> Window;
		TArray<FUnrealClaudeDeferredPlanEntry> Entries;
	};
}

TArray<FUnrealClaudeDeferredPlanEntry> FUnrealClaudeDeferredTasksEnumerator::ListDeferredPlans()
{
	TArray<FUnrealClaudeDeferredPlanEntry> Results;
	TArray<FString> ArchiveFiles;
	IFileManager::Get().FindFilesRecursive(
		ArchiveFiles,
		*FUnrealClaudeRelayAgentManager::GetPlanArchiveDir(),
		TEXT("*.active_plan.json"),
		true,
		false,
		false);

	for (const FString& ArchivePath : ArchiveFiles)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *ArchivePath))
		{
			continue;
		}

		const TSharedPtr<FJsonObject> RootObject = FJsonUtils::Parse(JsonText);
		if (!RootObject.IsValid())
		{
			continue;
		}

		FString Status;
		RootObject->TryGetStringField(TEXT("status"), Status);
		if (!(Status == TEXT("abandoned_by_user")
			|| Status == TEXT("abandoned_for_fresh_session")))
		{
			continue;
		}

		FUnrealClaudeDeferredPlanEntry Entry;
		Entry.ArchiveFilePath = ArchivePath;
		RootObject->TryGetStringField(TEXT("plan_id"), Entry.PlanId);
		RootObject->TryGetStringField(TEXT("original_user_task"), Entry.OriginalUserTask);
		RootObject->TryGetStringField(TEXT("user_closed_reason"), Entry.UserClosedReason);
		RootObject->TryGetStringField(TEXT("updated_at_utc"), Entry.UpdatedAtUtc);
		Entry.Status = Status;
		Entry.ShortTitle = TruncateAtWordBoundary(Entry.OriginalUserTask, 80);
		if (Entry.ShortTitle.IsEmpty())
		{
			Entry.ShortTitle = TEXT("(без названия)");
		}

		Results.Add(MoveTemp(Entry));
	}

	Results.Sort([](const FUnrealClaudeDeferredPlanEntry& A, const FUnrealClaudeDeferredPlanEntry& B)
	{
		return A.UpdatedAtUtc > B.UpdatedAtUtc;
	});
	return Results;
}

int32 FUnrealClaudeDeferredTasksEnumerator::CountDeferredPlans()
{
	return ListDeferredPlans().Num();
}

void FUnrealClaudeDeferredTasksBrowser::Show(const FOnPlanResumed& OnPlanResumed)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogUnrealClaude, Error, TEXT("DeferredTasksBrowser::Show must be called on game thread."));
		return;
	}

	MakeShared<FDeferredTasksBrowserController>(OnPlanResumed)->Show();
}

bool FUnrealClaudeDeferredTasksBrowser::LoadArchivedPlan(
	const FUnrealClaudeDeferredPlanEntry& Entry,
	FUnrealClaudeActivePlan& OutPlan,
	FString& OutError)
{
	return FUnrealClaudeRelayAgentManager::LoadPlanFromPath(Entry.ArchiveFilePath, OutPlan, OutError);
}

bool FUnrealClaudeDeferredTasksBrowser::ResumePlan(
	const FUnrealClaudeDeferredPlanEntry& Entry,
	FString& OutShortTitle,
	FString& OutError)
{
	OutShortTitle = Entry.ShortTitle;

	FUnrealClaudeActivePlan ResumedPlan;
	if (!LoadArchivedPlan(Entry, ResumedPlan, OutError))
	{
		return false;
	}

	FString BackupPath;
	if (!BackupCurrentActivePlanIfPresent(BackupPath, OutError))
	{
		return false;
	}

	ResumedPlan.UserRecoveryChoice.Empty();
	ResumedPlan.UserClosedReason.Empty();
	ResumedPlan.InterruptionDetectedAtUtc.Empty();
	ResumedPlan.InterruptionReason.Empty();
	ResumedPlan.InterruptionElapsedSeconds = 0;
	ResumedPlan.Status = TEXT("active");

	if (!ResumedPlan.OriginalUserTask.IsEmpty())
	{
		OutShortTitle = TruncateAtWordBoundary(ResumedPlan.OriginalUserTask, 80);
	}

	if (!FUnrealClaudeRelayAgentManager::SaveActivePlan(ResumedPlan, OutError))
	{
		return false;
	}

	if (!IFileManager::Get().Delete(*Entry.ArchiveFilePath, false, true, true))
	{
		OutError = FString::Printf(TEXT("Could not delete deferred archive after resume: %s"), *Entry.ArchiveFilePath);
		return false;
	}

	const FTaskRecoverySummary Summary =
		FUnrealClaudeTaskRecoverySummarizer::BuildRecoverySummary(
			ResumedPlan,
			FDateTime::UtcNow(),
			FUnrealClaudeRelayAgentManager::GetActivePlanPath());
	FClaudeCodeSubsystem::Get().SetPendingTaskRecoveryContext(
		FUnrealClaudeTaskRecoveryContextBuilder::BuildContextBlockText(Summary));

	int64 ElapsedSinceDeferralSeconds = 0;
	FDateTime DeferredAtUtc;
	if (TryParseIso8601Utc(Entry.UpdatedAtUtc, DeferredAtUtc))
	{
		ElapsedSinceDeferralSeconds = static_cast<int64>((FDateTime::UtcNow() - DeferredAtUtc).GetTotalSeconds());
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("resumed_plan_id"), ResumedPlan.PlanId);
	Payload->SetStringField(TEXT("archive_file_path"), Entry.ArchiveFilePath);
	Payload->SetNumberField(TEXT("elapsed_since_deferral_seconds"), ElapsedSinceDeferralSeconds);
	if (!BackupPath.IsEmpty())
	{
		Payload->SetStringField(TEXT("preexisting_active_plan_backup_path"), BackupPath);
	}
	FUnrealClaudeAgentTraceLog::Get().AppendEvent(
		TEXT("deferred_task_resumed"),
		FClaudeCodeSubsystem::Get().GetConfiguredBackend(),
		Payload);
	return true;
}

bool FUnrealClaudeDeferredTasksBrowser::DeletePlan(
	const FUnrealClaudeDeferredPlanEntry& Entry,
	FString& OutError)
{
	if (!IFileManager::Get().FileExists(*Entry.ArchiveFilePath))
	{
		return true;
	}

	if (!IFileManager::Get().Delete(*Entry.ArchiveFilePath, false, true, true))
	{
		OutError = FString::Printf(TEXT("Could not delete deferred archive: %s"), *Entry.ArchiveFilePath);
		return false;
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("deleted_plan_id"), Entry.PlanId);
	Payload->SetStringField(TEXT("archive_file_path"), Entry.ArchiveFilePath);
	FUnrealClaudeAgentTraceLog::Get().AppendEvent(
		TEXT("deferred_task_deleted"),
		FClaudeCodeSubsystem::Get().GetConfiguredBackend(),
		Payload);
	return true;
}

FString FUnrealClaudeDeferredTasksBrowser::BuildStatusLabel(const FString& Status)
{
	if (Status == TEXT("abandoned_by_user"))
	{
		return TEXT("отложена пользователем");
	}
	if (Status == TEXT("abandoned_for_fresh_session"))
	{
		return TEXT("отложена для новой сессии");
	}

	return Status.IsEmpty() ? TEXT("не указан") : Status;
}

FString FUnrealClaudeDeferredTasksBrowser::BuildReasonLabel(const FString& Reason)
{
	return Reason.IsEmpty() ? TEXT("не указана") : Reason;
}

FString FUnrealClaudeDeferredTasksBrowser::BuildMetaLine(
	const FUnrealClaudeDeferredPlanEntry& Entry,
	const FTaskRecoverySummary& Summary)
{
	return FString::Printf(
		TEXT("Отложено: %s · Причина: %s · Статус: %s"),
		*Summary.ElapsedSincePause,
		*BuildReasonLabel(Entry.UserClosedReason),
		*BuildStatusLabel(Entry.Status));
}

FString FUnrealClaudeDeferredTasksBrowser::BuildBackupArchivePath(const FString& ExistingPlanId)
{
	const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
	const FString SafePlanId = MakeArchiveSafeFileTag(
		ExistingPlanId.IsEmpty() ? TEXT("existing_active_plan") : ExistingPlanId);
	return FPaths::Combine(
		FUnrealClaudeRelayAgentManager::GetPlanArchiveDir(),
		FString::Printf(TEXT("%s-%s.resume_backup.active_plan.json"), *Timestamp, *SafePlanId));
}

bool FUnrealClaudeDeferredTasksBrowser::BackupCurrentActivePlanIfPresent(
	FString& OutBackupPath,
	FString& OutError)
{
	const FString ActivePlanPath = FUnrealClaudeRelayAgentManager::GetActivePlanPath();
	if (IFileManager::Get().FileSize(*ActivePlanPath) <= 0)
	{
		OutBackupPath.Reset();
		return true;
	}

	FUnrealClaudeActivePlan ExistingPlan;
	FString ExistingPlanId;
	FString LoadError;
	if (FUnrealClaudeRelayAgentManager::LoadActivePlan(ExistingPlan, LoadError))
	{
		ExistingPlanId = ExistingPlan.PlanId;
	}

	OutBackupPath = BuildBackupArchivePath(ExistingPlanId);
	IFileManager::Get().MakeDirectory(*FUnrealClaudeRelayAgentManager::GetPlanArchiveDir(), true);
	if (IFileManager::Get().Copy(*OutBackupPath, *ActivePlanPath, true, true) != COPY_OK)
	{
		OutError = FString::Printf(TEXT("Could not back up current active plan to %s"), *OutBackupPath);
		return false;
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
