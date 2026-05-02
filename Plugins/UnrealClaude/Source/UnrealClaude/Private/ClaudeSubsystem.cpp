// Copyright Natali Caggiano. All Rights Reserved.

#include "ClaudeSubsystem.h"
#include "AgentOrchestrator.h"

FClaudeCodeSubsystem& FClaudeCodeSubsystem::Get()
{
	static FClaudeCodeSubsystem Instance;
	return Instance;
}

FClaudeCodeSubsystem::FClaudeCodeSubsystem()
{
	Orchestrator = MakeUnique<FAgentOrchestrator>();
}

FClaudeCodeSubsystem::~FClaudeCodeSubsystem()
{
}

void FClaudeCodeSubsystem::SendPrompt(
	const FString& Prompt,
	FOnClaudeResponse OnComplete,
	const FClaudePromptOptions& Options)
{
	if (!Orchestrator.IsValid())
	{
		OnComplete.ExecuteIfBound(TEXT("Assistant runtime is not initialized"), false);
		return;
	}

	FAgentPromptOptions AgentOptions;
	AgentOptions.bIncludeEngineContext = Options.bIncludeEngineContext;
	AgentOptions.bIncludeProjectContext = Options.bIncludeProjectContext;
	AgentOptions.ExecutionProfile = Options.ExecutionProfile;
	AgentOptions.OnProgress = Options.OnProgress;
	AgentOptions.OnStreamEvent = Options.OnStreamEvent;
	AgentOptions.AttachedImagePaths = Options.AttachedImagePaths;
	AgentOptions.bIsTransportRetryReplay = Options.bIsTransportRetryReplay;
	AgentOptions.TransportRetrySourceRunId = Options.TransportRetrySourceRunId;
	AgentOptions.TransportRetryFailureFamily = Options.TransportRetryFailureFamily;
	AgentOptions.VisibleHistoryPromptOverride = Options.VisibleHistoryPromptOverride;

	Orchestrator->SendPrompt(Prompt, OnComplete, AgentOptions);
}

void FClaudeCodeSubsystem::SendPrompt(
	const FString& Prompt,
	FOnClaudeResponse OnComplete,
	bool bIncludeUE57Context,
	FOnClaudeProgress OnProgress,
	bool bIncludeProjectContext)
{
	FClaudePromptOptions Options;
	Options.bIncludeEngineContext = bIncludeUE57Context;
	Options.bIncludeProjectContext = bIncludeProjectContext;
	Options.OnProgress = OnProgress;
	SendPrompt(Prompt, OnComplete, Options);
}

FString FClaudeCodeSubsystem::GetUE57SystemPrompt() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetUE57SystemPrompt() : FString();
}

FString FClaudeCodeSubsystem::GetProjectContextPrompt() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetProjectContextPrompt() : FString();
}

void FClaudeCodeSubsystem::SetCustomSystemPrompt(const FString& InCustomPrompt)
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->SetCustomSystemPrompt(InCustomPrompt);
	}
}

void FClaudeCodeSubsystem::SetPendingPolicyRoutingAdvisory(const FString& AdvisoryText)
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->SetPendingPolicyRoutingAdvisory(AdvisoryText);
	}
}

void FClaudeCodeSubsystem::SetPendingTaskRecoveryContext(const FString& ContextText)
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->SetPendingTaskRecoveryContext(ContextText);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
FString FClaudeCodeSubsystem::ConsumePendingTaskRecoveryContextForTests()
{
	return Orchestrator.IsValid()
		? Orchestrator->ConsumePendingTaskRecoveryContext()
		: FString();
}
#endif

const TArray<TPair<FString, FString>>& FClaudeCodeSubsystem::GetHistory() const
{
	static TArray<TPair<FString, FString>> EmptyHistory;
	return Orchestrator.IsValid() ? Orchestrator->GetHistory() : EmptyHistory;
}

void FClaudeCodeSubsystem::ClearHistory()
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->ClearHistory();
	}
}

void FClaudeCodeSubsystem::CancelCurrentRequest()
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->CancelCurrentRequest();
	}
}

bool FClaudeCodeSubsystem::SaveSession()
{
	return Orchestrator.IsValid() ? Orchestrator->SaveSession() : false;
}

bool FClaudeCodeSubsystem::LoadSession()
{
	return Orchestrator.IsValid() ? Orchestrator->LoadSession() : false;
}

FAgentSessionRestoreResult FClaudeCodeSubsystem::LoadSessionWithResult()
{
	return Orchestrator.IsValid() ? Orchestrator->LoadSessionWithResult() : FAgentSessionRestoreResult();
}

FAgentSavedSessionIndex FClaudeCodeSubsystem::DescribeSavedSessions() const
{
	return Orchestrator.IsValid() ? Orchestrator->DescribeSavedSessions() : FAgentSavedSessionIndex();
}

FAgentSavedSessionIndex FClaudeCodeSubsystem::DescribeSavedSessionsForProfile(const EAgentExecutionRunProfile Profile) const
{
	return Orchestrator.IsValid()
		? Orchestrator->DescribeSavedSessionsForProfile(Profile)
		: FAgentSavedSessionIndex();
}

bool FClaudeCodeSubsystem::HasSavedSession() const
{
	return Orchestrator.IsValid() ? Orchestrator->HasSavedSession() : false;
}

FString FClaudeCodeSubsystem::GetSessionFilePath() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetSessionFilePath() : FString();
}

FString FClaudeCodeSubsystem::GetSessionFilePathForProfile(const EAgentExecutionRunProfile Profile) const
{
	return Orchestrator.IsValid()
		? Orchestrator->GetSessionFilePathForProfile(Profile)
		: FString();
}

IClaudeRunner* FClaudeCodeSubsystem::GetRunner() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetClaudeRunner() : nullptr;
}

IAgentBackend* FClaudeCodeSubsystem::GetActiveBackend() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetActiveBackend() : nullptr;
}

EUnrealClaudeProviderBackend FClaudeCodeSubsystem::GetConfiguredBackend() const
{
	return Orchestrator.IsValid()
		? Orchestrator->GetConfiguredBackend()
		: EUnrealClaudeProviderBackend::ClaudeCli;
}

FAgentBackendStatus FClaudeCodeSubsystem::GetConfiguredBackendStatus() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetConfiguredBackendStatus() : FAgentBackendStatus();
}

FAgentProviderExecutionControlManifest FClaudeCodeSubsystem::GetConfiguredBackendExecutionControlManifest() const
{
	return Orchestrator.IsValid()
		? Orchestrator->GetConfiguredBackendExecutionControlManifest()
		: FAgentProviderExecutionControlManifest();
}

FAgentProviderExecutionControlManifest FClaudeCodeSubsystem::GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile Profile) const
{
	return Orchestrator.IsValid()
		? Orchestrator->GetConfiguredBackendExecutionControlManifestForProfile(Profile)
		: FAgentProviderExecutionControlManifest();
}

TArray<FAgentBackendStatus> FClaudeCodeSubsystem::GetBackendStatuses() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetBackendStatuses() : TArray<FAgentBackendStatus>();
}
