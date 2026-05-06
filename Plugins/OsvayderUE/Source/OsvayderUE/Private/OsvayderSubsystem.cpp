// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderSubsystem.h"
#include "AgentOrchestrator.h"

FOsvayderCodeSubsystem& FOsvayderCodeSubsystem::Get()
{
	static FOsvayderCodeSubsystem Instance;
	return Instance;
}

FOsvayderCodeSubsystem::FOsvayderCodeSubsystem()
{
	Orchestrator = MakeUnique<FAgentOrchestrator>();
}

FOsvayderCodeSubsystem::~FOsvayderCodeSubsystem()
{
}

void FOsvayderCodeSubsystem::SendPrompt(
	const FString& Prompt,
	FOnOsvayderResponse OnComplete,
	const FOsvayderPromptOptions& Options)
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

void FOsvayderCodeSubsystem::SendPrompt(
	const FString& Prompt,
	FOnOsvayderResponse OnComplete,
	bool bIncludeUE57Context,
	FOnOsvayderProgress OnProgress,
	bool bIncludeProjectContext)
{
	FOsvayderPromptOptions Options;
	Options.bIncludeEngineContext = bIncludeUE57Context;
	Options.bIncludeProjectContext = bIncludeProjectContext;
	Options.OnProgress = OnProgress;
	SendPrompt(Prompt, OnComplete, Options);
}

FString FOsvayderCodeSubsystem::GetUE57SystemPrompt() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetUE57SystemPrompt() : FString();
}

FString FOsvayderCodeSubsystem::GetProjectContextPrompt() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetProjectContextPrompt() : FString();
}

void FOsvayderCodeSubsystem::SetCustomSystemPrompt(const FString& InCustomPrompt)
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->SetCustomSystemPrompt(InCustomPrompt);
	}
}

void FOsvayderCodeSubsystem::SetPendingPolicyRoutingAdvisory(const FString& AdvisoryText)
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->SetPendingPolicyRoutingAdvisory(AdvisoryText);
	}
}

void FOsvayderCodeSubsystem::SetPendingTaskRecoveryContext(const FString& ContextText)
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->SetPendingTaskRecoveryContext(ContextText);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
FString FOsvayderCodeSubsystem::ConsumePendingTaskRecoveryContextForTests()
{
	return Orchestrator.IsValid()
		? Orchestrator->ConsumePendingTaskRecoveryContext()
		: FString();
}
#endif

const TArray<TPair<FString, FString>>& FOsvayderCodeSubsystem::GetHistory() const
{
	static TArray<TPair<FString, FString>> EmptyHistory;
	return Orchestrator.IsValid() ? Orchestrator->GetHistory() : EmptyHistory;
}

void FOsvayderCodeSubsystem::ClearHistory()
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->ClearHistory();
	}
}

void FOsvayderCodeSubsystem::CancelCurrentRequest()
{
	if (Orchestrator.IsValid())
	{
		Orchestrator->CancelCurrentRequest();
	}
}

bool FOsvayderCodeSubsystem::SaveSession()
{
	return Orchestrator.IsValid() ? Orchestrator->SaveSession() : false;
}

bool FOsvayderCodeSubsystem::LoadSession()
{
	return Orchestrator.IsValid() ? Orchestrator->LoadSession() : false;
}

FAgentSessionRestoreResult FOsvayderCodeSubsystem::LoadSessionWithResult()
{
	return Orchestrator.IsValid() ? Orchestrator->LoadSessionWithResult() : FAgentSessionRestoreResult();
}

FAgentSavedSessionIndex FOsvayderCodeSubsystem::DescribeSavedSessions() const
{
	return Orchestrator.IsValid() ? Orchestrator->DescribeSavedSessions() : FAgentSavedSessionIndex();
}

FAgentSavedSessionIndex FOsvayderCodeSubsystem::DescribeSavedSessionsForProfile(const EAgentExecutionRunProfile Profile) const
{
	return Orchestrator.IsValid()
		? Orchestrator->DescribeSavedSessionsForProfile(Profile)
		: FAgentSavedSessionIndex();
}

bool FOsvayderCodeSubsystem::HasSavedSession() const
{
	return Orchestrator.IsValid() ? Orchestrator->HasSavedSession() : false;
}

FString FOsvayderCodeSubsystem::GetSessionFilePath() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetSessionFilePath() : FString();
}

FString FOsvayderCodeSubsystem::GetSessionFilePathForProfile(const EAgentExecutionRunProfile Profile) const
{
	return Orchestrator.IsValid()
		? Orchestrator->GetSessionFilePathForProfile(Profile)
		: FString();
}

IOsvayderRunner* FOsvayderCodeSubsystem::GetRunner() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetClaudeRunner() : nullptr;
}

IAgentBackend* FOsvayderCodeSubsystem::GetActiveBackend() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetActiveBackend() : nullptr;
}

EOsvayderUEProviderBackend FOsvayderCodeSubsystem::GetConfiguredBackend() const
{
	return Orchestrator.IsValid()
		? Orchestrator->GetConfiguredBackend()
		: EOsvayderUEProviderBackend::CodexCli;
}

FAgentBackendStatus FOsvayderCodeSubsystem::GetConfiguredBackendStatus() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetConfiguredBackendStatus() : FAgentBackendStatus();
}

FAgentProviderExecutionControlManifest FOsvayderCodeSubsystem::GetConfiguredBackendExecutionControlManifest() const
{
	return Orchestrator.IsValid()
		? Orchestrator->GetConfiguredBackendExecutionControlManifest()
		: FAgentProviderExecutionControlManifest();
}

FAgentProviderExecutionControlManifest FOsvayderCodeSubsystem::GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile Profile) const
{
	return Orchestrator.IsValid()
		? Orchestrator->GetConfiguredBackendExecutionControlManifestForProfile(Profile)
		: FAgentProviderExecutionControlManifest();
}

TArray<FAgentBackendStatus> FOsvayderCodeSubsystem::GetBackendStatuses() const
{
	return Orchestrator.IsValid() ? Orchestrator->GetBackendStatuses() : TArray<FAgentBackendStatus>();
}
