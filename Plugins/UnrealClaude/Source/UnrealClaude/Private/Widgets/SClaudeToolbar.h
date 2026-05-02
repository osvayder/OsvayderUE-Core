// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

DECLARE_DELEGATE(FOnToolbarAction)
DECLARE_DELEGATE_OneParam(FOnCheckboxChanged, bool)

enum class EClaudeToolbarActionId : uint8
{
	UE57Context,
	ProjectContext,
	RefreshContext,
	RestoreSession,
	RestartSurvival,
	DeferredTasks,
	BrowserVerify,
	NewSession,
	CopyLast
};

struct FClaudeToolbarActionDescriptor
{
	EClaudeToolbarActionId Id = EClaudeToolbarActionId::RefreshContext;
	FName Name;
	FText Label;
	bool bToggle = false;
	bool bScrollableAction = true;
	bool bPrimaryToolbarAction = false;
};

/**
 * Toolbar widget for Claude Editor
 * Handles prompt-context toggles, session history actions, and auth actions
 */
class SClaudeToolbar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SClaudeToolbar)
		: _TitleText(FText::FromString(TEXT("Osvayder UE")))
		, _ProviderSummaryText(FText::GetEmpty())
		, _bUE57ContextEnabled(true)
		, _bProjectContextEnabled(true)
		, _bRestoreEnabled(false)
		, _bRestartSurvivalEnabled(false)
		, _bShowBrowserVerify(false)
		, _bBrowserVerifyEnabled(false)
		, _DeferredTasksCount(0)
	{}
		SLATE_ATTRIBUTE(FText, TitleText)
		SLATE_ATTRIBUTE(FText, ProviderSummaryText)
		SLATE_ATTRIBUTE(FText, ProviderSummaryToolTip)
		SLATE_ATTRIBUTE(bool, bUE57ContextEnabled)
		SLATE_ATTRIBUTE(bool, bProjectContextEnabled)
		SLATE_ATTRIBUTE(bool, bRestoreEnabled)
		SLATE_ATTRIBUTE(bool, bRestartSurvivalEnabled)
		SLATE_ATTRIBUTE(bool, bShowBrowserVerify)
		SLATE_ATTRIBUTE(bool, bBrowserVerifyEnabled)
		SLATE_ATTRIBUTE(int32, DeferredTasksCount)
		SLATE_ATTRIBUTE(FText, RestoreToolTip)
		SLATE_ATTRIBUTE(FText, RestartSurvivalToolTip)
		SLATE_ATTRIBUTE(FText, BrowserVerifyToolTip)
		SLATE_EVENT(FOnCheckboxChanged, OnUE57ContextChanged)
		SLATE_EVENT(FOnCheckboxChanged, OnProjectContextChanged)
		SLATE_EVENT(FOnToolbarAction, OnRefreshContext)
		SLATE_EVENT(FOnToolbarAction, OnRestoreSession)
		SLATE_EVENT(FOnToolbarAction, OnRestartSurvival)
		SLATE_EVENT(FOnToolbarAction, OnOpenDeferredTasks)
		SLATE_EVENT(FOnToolbarAction, OnBrowserVerify)
		SLATE_EVENT(FOnToolbarAction, OnNewSession)
		SLATE_EVENT(FOnToolbarAction, OnCopyLast)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	static const TArray<FClaudeToolbarActionDescriptor>& GetActionDescriptors();

private:
	enum class EToolbarActionWidgetKind : uint8
	{
		Button,
		CheckBox,
		DeferredTasksButton
	};

	struct FToolbarActionEntry
	{
		FClaudeToolbarActionDescriptor Descriptor;
		EToolbarActionWidgetKind WidgetKind = EToolbarActionWidgetKind::Button;
		TAttribute<FText> ToolTip;
		TAttribute<bool> bEnabled;
		TAttribute<EVisibility> Visibility;
		TFunction<FReply()> Execute;
		TFunction<ECheckBoxState()> GetCheckState;
		TFunction<void(ECheckBoxState)> SetCheckState;
	};

	TAttribute<FText> TitleText;
	TAttribute<FText> ProviderSummaryText;
	TAttribute<FText> ProviderSummaryToolTip;
	TAttribute<bool> bUE57ContextEnabled;
	TAttribute<bool> bProjectContextEnabled;
	TAttribute<bool> bRestoreEnabled;
	TAttribute<bool> bRestartSurvivalEnabled;
	TAttribute<bool> bShowBrowserVerify;
	TAttribute<bool> bBrowserVerifyEnabled;
	TAttribute<int32> DeferredTasksCount;
	TAttribute<FText> RestoreToolTip;
	TAttribute<FText> RestartSurvivalToolTip;
	TAttribute<FText> BrowserVerifyToolTip;

	FOnCheckboxChanged OnUE57ContextChanged;
	FOnCheckboxChanged OnProjectContextChanged;
	FOnToolbarAction OnRefreshContext;
	FOnToolbarAction OnRestoreSession;
	FOnToolbarAction OnRestartSurvival;
	FOnToolbarAction OnOpenDeferredTasks;
	FOnToolbarAction OnBrowserVerify;
	FOnToolbarAction OnNewSession;
	FOnToolbarAction OnCopyLast;

	TArray<FToolbarActionEntry> BuildToolbarActions();
	TSharedRef<SWidget> BuildToolbarHeader();
	TSharedRef<SWidget> BuildPrimaryActionStrip();
	TSharedRef<SWidget> BuildOverflowMenuButton(const TArray<FToolbarActionEntry>& Actions);
	TSharedRef<SWidget> BuildOverflowMenuContent(const TArray<FToolbarActionEntry>& Actions);
	TSharedRef<SWidget> BuildActionWidget(const FToolbarActionEntry& Action);
	TSharedRef<SWidget> BuildDeferredTasksWidget(const FToolbarActionEntry& Action);
};
