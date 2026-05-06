// Copyright Natali Caggiano. All Rights Reserved.

#include "AgentOrchestrator.h"
#include "AgentExecutionControl.h"
#include "AgentPromptContract.h"
#include "OsvayderEditorWidget.h"
#include "OsvayderCodeRunner.h"
#include "OsvayderSessionManager.h"
#include "CodexCliRunner.h"
#include "IOsvayderRunner.h"
#include "ProjectContext.h"
#include "ScriptExecutionManager.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUECanonRouting.h"
#include "OsvayderUEConstants.h"
#include "OsvayderUERelayAgent.h"
#include "OsvayderUERecipeRegistry.h"
#include "OsvayderUERestartSurvival.h"
#include "OsvayderUERoleRegistry.h"
#include "OsvayderUESettings.h"
#include "Misc/Paths.h"

namespace
{
	FString ResolveConfiguredRuntimeRestartSurvivalTransitionResponse(
		const EOsvayderUEProviderBackend ActiveBackend,
		const EAgentExecutionRunProfile ExecutionProfile,
		const FString& ProviderSessionId)
	{
		if (ActiveBackend != EOsvayderUEProviderBackend::CodexCli
			|| ExecutionProfile != EAgentExecutionRunProfile::ConfiguredDefaultRuntime
			|| !FCodexCliRunner::ShouldUsePersistentConversationTransport()
			|| !FOsvayderUERestartSurvivalManager::HasPreparedRequestAutoStartArm()
			|| !FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest())
		{
			return FString();
		}

		if (ProviderSessionId.IsEmpty())
		{
			return FString();
		}

		FOsvayderUERestartSurvivalPreparedRestoreRequest PreparedRequest;
		FString ValidationError;
		if (!FOsvayderUERestartSurvivalManager::LoadPreparedRestoreRequest(PreparedRequest, ValidationError))
		{
			return FString();
		}

		if (!PreparedRequest.bTaskDrivenHandoff
			|| !PreparedRequest.bAutoStartAfterResponse
			|| !PreparedRequest.bAutonomousClosedEditorEscalation)
		{
			return FString();
		}

		if (!FOsvayderUERestartSurvivalManager::ValidatePreparedRestoreRequestForStart(
			PreparedRequest,
			ActiveBackend,
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
			ProviderSessionId,
			ValidationError))
		{
			return FString();
		}

		return FOsvayderUERestartSurvivalManager::BuildPreparedRequestClosedEditorTransitionNotice(PreparedRequest);
	}

	void EnsureFeatureWorkflowRoleContract(FAgentFeatureWorkflowState& Workflow)
	{
		if (!Workflow.HasAnySignal())
		{
			return;
		}

		if (Workflow.RoleId.TrimStartAndEnd().IsEmpty())
		{
			Workflow.RoleId = OsvayderUERoleRegistry::WorkerRoleId();
		}

		if (Workflow.EvidenceSchemaVersion <= 0 && !Workflow.RecipeId.TrimStartAndEnd().IsEmpty())
		{
			FOsvayderUERecipeEvidenceContract RecipeContract;
			if (OsvayderUERecipeRegistry::TryGetRecipeEvidenceContract(Workflow.RecipeId, RecipeContract))
			{
				Workflow.EvidenceSchemaVersion = RecipeContract.EvidenceSchemaVersion;
			}
		}
	}
}

FAgentOrchestrator::FAgentOrchestrator()
{
	SessionManager = MakeUnique<FOsvayderSessionManager>();
}

FAgentOrchestrator::~FAgentOrchestrator()
{
}

void FAgentOrchestrator::SendPrompt(
	const FString& Prompt,
	FOnAgentResponse OnComplete,
	const FAgentPromptOptions& Options)
{
	const EOsvayderUEProviderBackend ActiveBackend = GetConfiguredBackend();
	FAgentRequestConfig Config = BuildRequestConfig(Prompt, Options);

	IAgentBackend* Backend = GetOrCreateBackend(ActiveBackend);
	FAgentBackendStatus Status;
	Status.Backend = ActiveBackend;
	Status.DisplayName = ActiveBackend == EOsvayderUEProviderBackend::CodexCli
		? TEXT("Codex CLI")
		: TEXT("Claude CLI");

	if (Backend)
	{
		Status = Backend->GetStatus();
	}

	const FString RunId = FOsvayderUEAgentTraceLog::Get().BeginRun(
		Status,
		Config,
		Prompt,
		Options.bIncludeEngineContext,
		Options.bIncludeProjectContext);

	if (!Backend)
	{
		const FString ErrorMessage = TEXT("Failed to initialize assistant backend");
		FOsvayderUEAgentTraceLog::Get().LogBackendFailure(RunId, ActiveBackend, ErrorMessage, TEXT("backend_init"));
		FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, ActiveBackend, ErrorMessage, false);
		OnComplete.ExecuteIfBound(TEXT("Failed to initialize assistant backend"), false);
		return;
	}

	if (!AgentBackendCanExecutePrompt(Status))
	{
		const FString ErrorMessage = Status.Detail.IsEmpty()
			? FString::Printf(TEXT("%s is not available"), *Status.DisplayName)
			: Status.Detail;
		FOsvayderUEAgentTraceLog::Get().LogBackendFailure(RunId, ActiveBackend, ErrorMessage, TEXT("readiness_gate"));
		FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, ActiveBackend, ErrorMessage, false);
		OnComplete.ExecuteIfBound(ErrorMessage, false);
		return;
	}

	FAgentExecutionPolicyDenyContract PolicyDenyContract;
	if (TryBuildAgentExecutionPolicyDenyContract(Status, Config, TEXT("provider_prompt_dispatch"), PolicyDenyContract))
	{
		const TSharedPtr<FJsonObject> DenyPayload = MakeAgentExecutionPolicyDenyContractJson(PolicyDenyContract);
		FOsvayderUEAgentTraceLog::Get().AppendEvent(TEXT("policy_denied"), ActiveBackend, DenyPayload, RunId);

		FString ErrorMessage = FString::Printf(
			TEXT("Policy denied [%s]: %s"),
			*PolicyDenyContract.PolicyRuleId,
			*PolicyDenyContract.DenyReason);
		if (PolicyDenyContract.bExpertOptInRequired)
		{
			ErrorMessage += TEXT(" Expert opt-in is required to use the high-risk provider runtime.");
		}

		FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, ActiveBackend, ErrorMessage, false);
		OnComplete.ExecuteIfBound(ErrorMessage, false);
		return;
	}

	const FOnAgentStreamEvent OriginalStreamEvent = Config.OnStreamEvent;
	Config.OnStreamEvent.Unbind();
	Config.OnStreamEvent.BindLambda([RunId, OriginalStreamEvent](const FAgentRunEvent& Event)
	{
		FOsvayderUEAgentTraceLog::Get().AppendObservedEvent(RunId, Event);
		OriginalStreamEvent.ExecuteIfBound(Event);
	});

	const EAgentSessionPersistenceMode SessionPersistenceMode = Config.SessionPersistenceMode;
	const EAgentExecutionRunProfile ExecutionProfile = Config.ExecutionProfile;
	const FString VisibleHistoryPrompt = Options.VisibleHistoryPromptOverride.IsEmpty()
		? Prompt
		: Options.VisibleHistoryPromptOverride;
	FOnAgentResponse WrappedComplete;
	WrappedComplete.BindLambda([this, VisibleHistoryPrompt, OnComplete, ActiveBackend, RunId, SessionPersistenceMode, ExecutionProfile](const FString& Response, bool bSuccess)
	{
		FCodexCliRunner* CodexRunner = ActiveBackend == EOsvayderUEProviderBackend::CodexCli
			? static_cast<FCodexCliRunner*>(GetOrCreateBackend(ActiveBackend))
			: nullptr;
		FString ProviderSessionId = CodexRunner != nullptr
			? CodexRunner->GetActivePersistentThreadId()
			: FString();
		if (CodexRunner != nullptr && ProviderSessionId.IsEmpty())
		{
			FString IgnoredThreadStatePath;
			CodexRunner->ExportActiveThreadStateForRestartSurvival(IgnoredThreadStatePath, ProviderSessionId);
		}
		const FString SanitizedResponse =
			ResolveConfiguredRuntimeRestartSurvivalTransitionResponse(ActiveBackend, ExecutionProfile, ProviderSessionId);
		FString UserFacingResponse = SanitizedResponse.IsEmpty() ? Response : SanitizedResponse;
		if (bSuccess && SanitizedResponse.IsEmpty())
		{
			FOsvayderUEActivePlan ActivePlan;
			FString ActivePlanError;
			if (FOsvayderUERelayAgentManager::LoadActivePlan(ActivePlan, ActivePlanError))
			{
				const FOsvayderUEActivePlanCloseoutDecision CloseoutDecision =
					SOsvayderEditorWidget::EvaluateActivePlanCloseoutFromCurrentArtifacts(ActivePlan, true);
				UserFacingResponse = SOsvayderEditorWidget::RewriteResponseForTruthfulActivePlanCloseout(
					ActivePlan,
					CloseoutDecision,
					UserFacingResponse);
			}
		}

		if (bSuccess && SessionManager.IsValid())
		{
			IAgentBackend* Backend = GetOrCreateBackend(ActiveBackend);
			const FAgentBackendStatus BackendStatus = Backend ? Backend->GetStatus() : FAgentBackendStatus();

			if (ExecutionProfile == EAgentExecutionRunProfile::ConfiguredDefaultRuntime)
			{
				SessionManager->AddExchange(ActiveBackend, VisibleHistoryPrompt, UserFacingResponse);
				SessionManager->SaveVisibleSession(ActiveBackend, BackendStatus);
			}
			else if (SessionPersistenceMode == EAgentSessionPersistenceMode::NormalProviderSession)
			{
				SessionManager->AddProviderSessionExchange(ActiveBackend, VisibleHistoryPrompt, UserFacingResponse);
				SessionManager->SaveSession(ActiveBackend, BackendStatus);
			}
		}

		if (!bSuccess && !FOsvayderUEAgentTraceLog::Get().IsRunMarkedCancelled(RunId))
		{
			FOsvayderUEAgentTraceLog::Get().LogBackendFailure(RunId, ActiveBackend, Response, TEXT("backend_completion"));
		}

		FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, ActiveBackend, Response, bSuccess);
		OnComplete.ExecuteIfBound(UserFacingResponse, bSuccess);
	});

	if (!Backend->ExecuteAsync(Config, WrappedComplete, Options.OnProgress))
	{
		const FString ErrorMessage = FString::Printf(TEXT("Failed to start %s"), *Status.DisplayName);
		FOsvayderUEAgentTraceLog::Get().LogBackendFailure(RunId, ActiveBackend, ErrorMessage, TEXT("execute_async_start"));
		FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, ActiveBackend, ErrorMessage, false);
		OnComplete.ExecuteIfBound(ErrorMessage, false);
	}
}

FString FAgentOrchestrator::GetUE57SystemPrompt() const
{
	const FAgentPromptContract Contract = FAgentPromptContractBuilder::Build(
		true,
		false,
		FString(),
		FString());
	return FAgentPromptMaterializer::MaterializeCanonicalText(Contract);
}

FString FAgentOrchestrator::GetProjectContextPrompt() const
{
	FProjectContextManager& ContextManager = FProjectContextManager::Get();
	ContextManager.GetContext();
	FString Context = ContextManager.FormatContextForPrompt();

	FString ScriptHistory = FScriptExecutionManager::Get().FormatHistoryForContext(10);
	if (!ScriptHistory.IsEmpty())
	{
		Context += TEXT("\n\n") + ScriptHistory;
	}

	return Context;
}

void FAgentOrchestrator::SetCustomSystemPrompt(const FString& InCustomPrompt)
{
	CustomSystemPrompt = InCustomPrompt;
}

void FAgentOrchestrator::SetPendingPolicyRoutingAdvisory(const FString& AdvisoryText)
{
	// 626 P6-prep wiring: single-slot last-write-wins. If two policy decisions
	// occur between BuildRequestConfig calls (rare — would require two
	// command_execution events in the same turn window before the agent
	// submits its next prompt), keep the more recent one. The older decision
	// remains in agent_trace.jsonl for forensic inspection.
	PendingPolicyRoutingAdvisory = AdvisoryText;
}

FString FAgentOrchestrator::ConsumePendingPolicyRoutingAdvisory()
{
	FString Consumed = PendingPolicyRoutingAdvisory;
	PendingPolicyRoutingAdvisory.Empty();
	return Consumed;
}

void FAgentOrchestrator::SetPendingTaskRecoveryContext(const FString& ContextText)
{
	// 632 wiring: single-slot last-write-wins mirror of the 626 P6-prep policy
	// routing advisory. If a user clicks "Continue" twice somehow (or dev
	// scripts the dialog flow), the newer context overrides; older one still
	// appears in agent_trace.jsonl for forensic inspection.
	PendingTaskRecoveryContext = ContextText;
}

FString FAgentOrchestrator::ConsumePendingTaskRecoveryContext()
{
	FString Consumed = PendingTaskRecoveryContext;
	PendingTaskRecoveryContext.Empty();
	return Consumed;
}

const TArray<TPair<FString, FString>>& FAgentOrchestrator::GetHistory() const
{
	static TArray<TPair<FString, FString>> EmptyHistory;
	return SessionManager.IsValid()
		? SessionManager->GetHistory(GetConfiguredBackend())
		: EmptyHistory;
}

void FAgentOrchestrator::ClearHistory()
{
	if (SessionManager.IsValid())
	{
		const EOsvayderUEProviderBackend ActiveBackend = GetConfiguredBackend();
		SessionManager->ClearHistory(ActiveBackend);
		SessionManager->SaveVisibleSession(ActiveBackend, GetConfiguredBackendStatus());
	}

	if (IAgentBackend* Backend = GetActiveBackend())
	{
		Backend->ResetConversation();
	}
}

void FAgentOrchestrator::CancelCurrentRequest()
{
	if (IAgentBackend* Backend = GetActiveBackend())
	{
		Backend->Cancel();
	}
}

bool FAgentOrchestrator::SaveSession()
{
	return SessionManager.IsValid()
		? SessionManager->SaveVisibleSession(GetConfiguredBackend(), GetConfiguredBackendStatus())
		: false;
}

bool FAgentOrchestrator::LoadSession()
{
	return LoadSessionWithResult().WasLoaded();
}

FAgentSessionRestoreResult FAgentOrchestrator::LoadSessionWithResult()
{
	return BuildProfileScopedRestoreResult(EAgentExecutionRunProfile::ConfiguredDefaultRuntime);
}

FAgentSavedSessionIndex FAgentOrchestrator::DescribeSavedSessions() const
{
	return DescribeSavedSessionsForProfile(EAgentExecutionRunProfile::ConfiguredDefaultRuntime);
}

FAgentSavedSessionIndex FAgentOrchestrator::DescribeSavedSessionsForProfile(const EAgentExecutionRunProfile Profile) const
{
	return BuildProfileScopedSavedSessions(Profile);
}

bool FAgentOrchestrator::HasSavedSession() const
{
	return DescribeSavedSessions().CurrentProviderSession.bHasSession;
}

FString FAgentOrchestrator::GetSessionFilePath() const
{
	return GetSessionFilePathForProfile(EAgentExecutionRunProfile::ConfiguredDefaultRuntime);
}

FString FAgentOrchestrator::GetSessionFilePathForProfile(const EAgentExecutionRunProfile Profile) const
{
	if (!SessionManager.IsValid())
	{
		return FString();
	}

	if (ProfileUsesNormalProviderSession(Profile))
	{
		return SessionManager->GetSessionFilePath(GetConfiguredBackend());
	}

	if (ProfileUsesProjectLocalVisibleRestore(Profile))
	{
		return SessionManager->GetVisibleSessionFilePath(GetConfiguredBackend());
	}

	return FString();
}

bool FAgentOrchestrator::ProfileUsesProjectLocalVisibleRestore(const EAgentExecutionRunProfile Profile) const
{
	return Profile == EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
}

bool FAgentOrchestrator::ProfileUsesNormalProviderSession(const EAgentExecutionRunProfile Profile) const
{
	return BuildExecutionControlBaselineConfig(Profile).SessionPersistenceMode == EAgentSessionPersistenceMode::NormalProviderSession;
}

FAgentSavedSessionIndex FAgentOrchestrator::BuildProfileScopedSavedSessions(const EAgentExecutionRunProfile Profile) const
{
	if (!SessionManager.IsValid())
	{
		return FAgentSavedSessionIndex();
	}

	const EOsvayderUEProviderBackend ActiveBackend = GetConfiguredBackend();
	if (ProfileUsesNormalProviderSession(Profile))
	{
		return SessionManager->DescribeSavedSessions(ActiveBackend);
	}

	if (ProfileUsesProjectLocalVisibleRestore(Profile))
	{
		return SessionManager->DescribeVisibleSavedSessions(ActiveBackend);
	}

	const FAgentSavedSessionIndex BaseSessions = SessionManager->DescribeSavedSessions(ActiveBackend);
	FAgentSavedSessionIndex ScopedSessions = BaseSessions;
	const FAgentSessionMetadata NormalProviderSession = BaseSessions.CurrentProviderSession;
	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	const FString DisplayName = Status.DisplayName.IsEmpty()
		? (ActiveBackend == EOsvayderUEProviderBackend::CodexCli ? TEXT("Codex CLI") : TEXT("Claude CLI"))
		: Status.DisplayName;
	const FString BoundaryDetail = NormalProviderSession.bHasSession
		? FString::Printf(
			TEXT("The %s runtime does not reuse the normal %s provider session store. Explicit expert opt-in is required to restore the saved session at %s."),
			*FString(AgentExecutionRunProfileToString(Profile)),
			*DisplayName,
			*NormalProviderSession.SessionFilePath)
		: FString::Printf(
			TEXT("The %s runtime does not use the normal %s provider session store and remains boundary-isolated."),
			*FString(AgentExecutionRunProfileToString(Profile)),
			*DisplayName);

	ScopedSessions.CurrentProviderSession = FAgentSessionMetadata();
	ScopedSessions.CurrentProviderSession.Backend = ActiveBackend;
	ScopedSessions.CurrentProviderSession.BackendDisplayName = DisplayName;
	ScopedSessions.CurrentProviderSession.SessionFilePath = FString();
	ScopedSessions.CurrentProviderSession.Detail = BoundaryDetail;
	return ScopedSessions;
}

FAgentSessionRestoreResult FAgentOrchestrator::BuildProfileScopedRestoreResult(const EAgentExecutionRunProfile Profile) const
{
	if (!SessionManager.IsValid())
	{
		return FAgentSessionRestoreResult();
	}

	if (ProfileUsesNormalProviderSession(Profile))
	{
		return SessionManager->LoadSession(GetConfiguredBackend());
	}

	if (ProfileUsesProjectLocalVisibleRestore(Profile))
	{
		return SessionManager->LoadVisibleSession(GetConfiguredBackend());
	}

	FAgentSessionRestoreResult Result;
	Result.SavedSessions = BuildProfileScopedSavedSessions(Profile);
	Result.RequestedSession = Result.SavedSessions.CurrentProviderSession;
	Result.Outcome = EAgentSessionRestoreOutcome::NoSession;

	const FAgentSavedSessionIndex ExpertSessions = SessionManager->DescribeSavedSessions(GetConfiguredBackend());
	if (ExpertSessions.CurrentProviderSession.bHasSession)
	{
		Result.FailureReason = FString::Printf(
			TEXT("The current %s runtime is boundary-isolated and does not auto-restore the normal provider session. Explicit expert opt-in is required to use the saved session at %s."),
			*FString(AgentExecutionRunProfileToString(Profile)),
			*ExpertSessions.CurrentProviderSession.SessionFilePath);
	}
	else
	{
		Result.FailureReason = FString::Printf(
			TEXT("No lane-scoped session is available for %s because this runtime is boundary-isolated and does not persist normal provider session history."),
			*FString(AgentExecutionRunProfileToString(Profile)));
	}

	Result.RequestedSession.Detail = Result.FailureReason;
	return Result;
}

EOsvayderUEProviderBackend FAgentOrchestrator::GetConfiguredBackend() const
{
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	return Settings ? Settings->PreferredBackend : EOsvayderUEProviderBackend::CodexCli;
}

FAgentBackendStatus FAgentOrchestrator::GetConfiguredBackendStatus() const
{
	if (IAgentBackend* Backend = GetOrCreateBackend(GetConfiguredBackend()))
	{
		return Backend->GetStatus();
	}
	return FAgentBackendStatus();
}

FAgentProviderExecutionControlManifest FAgentOrchestrator::GetConfiguredBackendExecutionControlManifest() const
{
	return GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile::ConfiguredDefaultRuntime);
}

FAgentProviderExecutionControlManifest FAgentOrchestrator::GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile Profile) const
{
	const FAgentBackendStatus Status = GetConfiguredBackendStatus();
	const FAgentRequestConfig BaselineConfig = BuildExecutionControlBaselineConfig(Profile);
	return BuildAgentProviderExecutionControlManifest(Status, BaselineConfig);
}

bool FAgentOrchestrator::ShouldResumeFeatureWorkflowFromActivePlan(const FOsvayderUEActivePlan& ActivePlan)
{
	const FAgentFeatureWorkflowState& Workflow = ActivePlan.FeatureWorkflow;
	if (!Workflow.HasAnySignal())
	{
		return false;
	}

	const auto IsTerminalText = [](const FString& Value)
	{
		return Value.Equals(TEXT("done"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("failed"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("cancelled"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("abandoned_for_fresh_session"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("abandoned_by_user"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("achieved_fully"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("not_achieved"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("stop_loss"), ESearchCase::IgnoreCase);
	};

	const FString StopLossContext =
		(Workflow.StopLossReason + TEXT(" ") + Workflow.BlockerFamily + TEXT(" ") + Workflow.BlockerDetail).ToLower();
	const bool bRecoverableExternalRuntimeProofStopLoss =
		Workflow.RecipeId.Equals(OsvayderUERecipeRegistry::InteractionAccessRecipeId(), ESearchCase::CaseSensitive)
		&& Workflow.bRuntimeProofRequired
		&& Workflow.bStopLossTriggered
		&& (StopLossContext.Contains(TEXT("proof_prerequisites_missing"))
			|| StopLossContext.Contains(TEXT("proof_prerequisite"))
			|| StopLossContext.Contains(TEXT("ad_hoc_runtime_proof_attempts"))
			|| StopLossContext.Contains(TEXT("phase_failed_twice:attempt_resolver_and_logging")));
	if (bRecoverableExternalRuntimeProofStopLoss)
	{
		return true;
	}

	if (Workflow.bStopLossTriggered || IsTerminalText(Workflow.TerminalStatus))
	{
		return false;
	}
	if (IsTerminalText(ActivePlan.Status) || IsTerminalText(ActivePlan.ResultStatus))
	{
		return false;
	}

	return true;
}

TArray<FAgentBackendStatus> FAgentOrchestrator::GetBackendStatuses() const
{
	TArray<FAgentBackendStatus> Statuses;
	if (IAgentBackend* Backend = GetOrCreateBackend(EOsvayderUEProviderBackend::ClaudeCli))
	{
		Statuses.Add(Backend->GetStatus());
	}
	if (IAgentBackend* Backend = GetOrCreateBackend(EOsvayderUEProviderBackend::CodexCli))
	{
		Statuses.Add(Backend->GetStatus());
	}
	return Statuses;
}

IAgentBackend* FAgentOrchestrator::GetActiveBackend() const
{
	return GetOrCreateBackend(GetConfiguredBackend());
}

IOsvayderRunner* FAgentOrchestrator::GetClaudeRunner() const
{
	IAgentBackend* Backend = GetActiveBackend();
	if (!Backend || Backend->GetBackendType() != EOsvayderUEProviderBackend::ClaudeCli)
	{
		return nullptr;
	}

	return static_cast<IOsvayderRunner*>(Backend);
}

IAgentBackend* FAgentOrchestrator::GetOrCreateBackend(EOsvayderUEProviderBackend Backend) const
{
	switch (Backend)
	{
	case EOsvayderUEProviderBackend::CodexCli:
		if (!CodexBackend.IsValid())
		{
			CodexBackend = MakeUnique<FCodexCliRunner>();
		}
		return CodexBackend.Get();

	case EOsvayderUEProviderBackend::ClaudeCli:
	default:
		if (!ClaudeBackend.IsValid())
		{
			ClaudeBackend = MakeUnique<FOsvayderCodeRunner>();
		}
		return ClaudeBackend.Get();
	}
}

FAgentRequestConfig FAgentOrchestrator::BuildExecutionControlBaselineConfig(EAgentExecutionRunProfile Profile) const
{
	FAgentRequestConfig Config;
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	const EOsvayderUEProviderBackend ActiveBackend = GetConfiguredBackend();

	Config.WorkingDirectory = FPaths::ProjectDir();
	Config.ExecutionProfile = Profile;

	switch (Profile)
	{
	case EAgentExecutionRunProfile::BoundedPluginMutation:
		Config.bSkipPermissions = false;
		Config.bExecutionProfileEnforcedNow = true;
		Config.bEnableUnrealMcpBridge = false;
		Config.bForceDisablePersistentConversationTransport = ActiveBackend == EOsvayderUEProviderBackend::CodexCli;
		Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
		Config.ExecutionControlProfileId = TEXT("bounded_plugin_mutation_v1");
		Config.ExecutionTransportLabel = TEXT("governed_plugin_surface_only");
		Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
		Config.DesiredFutureDefaultProviderPowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
		break;

	case EAgentExecutionRunProfile::ReadOnlyDiagnostic:
		Config.bSkipPermissions = false;
		Config.bExecutionProfileEnforcedNow = true;
		Config.bEnableUnrealMcpBridge = false;
		Config.bForceDisablePersistentConversationTransport = ActiveBackend == EOsvayderUEProviderBackend::CodexCli;
		Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
		Config.ExecutionControlProfileId = TEXT("read_only_diagnostic_v1");
		Config.ExecutionTransportLabel = ActiveBackend == EOsvayderUEProviderBackend::CodexCli
			? TEXT("exec_per_message")
			: TEXT("cli_process");
		Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
		Config.DesiredFutureDefaultProviderPowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
		Config.AllowedTools = {
			TEXT("Read"),
			TEXT("Grep"),
			TEXT("Glob")
		};
		break;

	case EAgentExecutionRunProfile::ExplicitExpertOptIn:
		Config.bSkipPermissions = true;
		Config.bExecutionProfileEnforcedNow = true;
		Config.bEnableUnrealMcpBridge = true;
		Config.bForceDisablePersistentConversationTransport = false;
		Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NormalProviderSession;
		Config.ExecutionControlProfileId = TEXT("explicit_expert_opt_in_v1");
		Config.ExecutionTransportLabel = ActiveBackend == EOsvayderUEProviderBackend::CodexCli
			? (FCodexCliRunner::ShouldUsePersistentConversationTransport() ? TEXT("persistent_app_server") : TEXT("exec_per_message"))
			: TEXT("cli_process");
		Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
		Config.DesiredFutureDefaultProviderPowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
		Config.AllowedTools = {
			TEXT("Read"),
			TEXT("Write"),
			TEXT("Edit"),
			TEXT("Grep"),
			TEXT("Glob"),
			TEXT("Bash")
		};
		break;

	case EAgentExecutionRunProfile::ConfiguredDefaultRuntime:
	default:
		Config.bSkipPermissions = false;
		Config.bExecutionProfileEnforcedNow = true;
		Config.bEnableUnrealMcpBridge = false;
		Config.bForceDisablePersistentConversationTransport = false;
		Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
		Config.ExecutionControlProfileId = TEXT("workspace_write_default_runtime_v1");
		Config.ExecutionTransportLabel = ActiveBackend == EOsvayderUEProviderBackend::CodexCli
			? (FCodexCliRunner::ShouldUsePersistentConversationTransport() ? TEXT("persistent_app_server") : TEXT("exec_per_message"))
			: TEXT("cli_process");
		Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
		Config.DesiredFutureDefaultProviderPowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
		Config.AllowedTools = {
			TEXT("Read"),
			TEXT("Write"),
			TEXT("Edit"),
			TEXT("Grep"),
			TEXT("Glob"),
			TEXT("Bash"),
			TEXT("mcp__osvayderue__restart_survival")
		};
		break;
	}

	Config.bToolAllowListEnforced = ActiveBackend == EOsvayderUEProviderBackend::ClaudeCli;

	const FAgentBackendStatus ControlStatus = GetConfiguredBackendStatus();
	const FAgentProviderExecutionControlManifest ControlManifest = BuildAgentProviderExecutionControlManifest(ControlStatus, Config);
	Config.CurrentEffectiveProviderPowerClass = ControlManifest.CurrentEffectiveProviderPowerClass;

	if (Settings && ActiveBackend == EOsvayderUEProviderBackend::CodexCli)
	{
		Config.CodexSpeedMode = Settings->GetConfiguredCodexSpeedModeName();
		Config.CodexWorkMode = Settings->GetConfiguredCodexWorkModeName();
		Config.CodexReasoningEffort = Settings->GetConfiguredCodexReasoningEffortName();
		Config.CodexVerbosity = Settings->GetConfiguredCodexVerbosityName();
	}

	return Config;
}

FAgentRequestConfig FAgentOrchestrator::BuildRequestConfig(const FString& Prompt, const FAgentPromptOptions& Options) const
{
	FAgentRequestConfig Config = BuildExecutionControlBaselineConfig(Options.ExecutionProfile);
	if (Options.ExecutionProfile == EAgentExecutionRunProfile::ConfiguredDefaultRuntime)
	{
		Config.bUseRestartSurvivalProviderThreadState =
			FOsvayderUERestartSurvivalManager::HasPendingResume(GetConfiguredBackend());
	}

	const bool bUsePersistentCodexConversation =
		GetConfiguredBackend() == EOsvayderUEProviderBackend::CodexCli &&
		!Config.bForceDisablePersistentConversationTransport &&
		FCodexCliRunner::ShouldUsePersistentConversationTransport();
	const bool bUseBoundaryIsolatedPrompt =
		Config.SessionPersistenceMode != EAgentSessionPersistenceMode::NormalProviderSession &&
		Options.ExecutionProfile != EAgentExecutionRunProfile::ConfiguredDefaultRuntime;

	Config.Prompt = (bUsePersistentCodexConversation || bUseBoundaryIsolatedPrompt)
		? Prompt
		: BuildPromptWithHistory(Prompt, Options.ExecutionProfile);
	Config.ConversationBootstrapText = bUsePersistentCodexConversation
		? BuildConversationBootstrapText(Options.ExecutionProfile)
		: FString();
	Config.AttachedImagePaths = Options.AttachedImagePaths;
	Config.OnStreamEvent = Options.OnStreamEvent;
	Config.bIsTransportRetryReplay = Options.bIsTransportRetryReplay;
	Config.TransportRetrySourceRunId = Options.TransportRetrySourceRunId;
	Config.TransportRetryFailureFamily = Options.TransportRetryFailureFamily;
	const FString ProjectContextPrompt = Options.bIncludeProjectContext
		? GetProjectContextPrompt()
		: FString();

	Config.PromptContract = FAgentPromptContractBuilder::Build(
		Options.bIncludeEngineContext,
		Options.bIncludeProjectContext,
		ProjectContextPrompt,
		CustomSystemPrompt);

	// 626 P6-prep wiring (Option A+B hybrid): consume any pending policy-routing
	// advisory queued by the widget-side policy-gate hook and append it as a
	// dedicated context block so the agent sees the P3 policy decision in this
	// turn's system prompt. Single-slot queue; consumed-and-cleared here so a
	// second send after the advisory is already in flight won't re-inject stale
	// routing text. Empty advisory adds nothing (zero noise on normal turns).
	const FString PolicyAdvisory = const_cast<FAgentOrchestrator*>(this)->ConsumePendingPolicyRoutingAdvisory();
	if (!PolicyAdvisory.IsEmpty())
	{
		FAgentPromptContextBlock AdvisoryBlock;
		AdvisoryBlock.Label = TEXT("POLICY ROUTING ADVISORY");
		AdvisoryBlock.Content = PolicyAdvisory;
		Config.PromptContract.ContextBlocks.Add(AdvisoryBlock);
	}

	// 632 wiring: consume any pending task-recovery context queued when the
	// user clicked "Continue" on the recovery dialog. Injects a dedicated
	// [TASK RECOVERY] context block so the agent observes the interrupted
	// task's intent + summary in its next turn's system prompt. Single-slot
	// last-write-wins; consumed + cleared here (one-shot). Empty adds nothing.
	const FString RecoveryContext = const_cast<FAgentOrchestrator*>(this)->ConsumePendingTaskRecoveryContext();
	if (!RecoveryContext.IsEmpty())
	{
		FAgentPromptContextBlock RecoveryBlock;
		RecoveryBlock.Label = TEXT("TASK RECOVERY");
		RecoveryBlock.Content = RecoveryContext;
		Config.PromptContract.ContextBlocks.Add(RecoveryBlock);
	}

	if (Options.bIsTransportRetryReplay)
	{
		FAgentPromptContextBlock RetryBlock;
		RetryBlock.Label = TEXT("TRANSPORT RETRY CONTEXT");
		RetryBlock.Content = FString::Printf(
			TEXT("source_run_id = %s\nfailure_family = %s\nretry_kind = exact_last_prompt_replay\nresume_rule = previous request lost the backend transport before completion; continue the same active plan/workflow on a fresh backend session instead of reclassifying the task from scratch"),
			*Options.TransportRetrySourceRunId,
			*Options.TransportRetryFailureFamily);
		Config.PromptContract.ContextBlocks.Add(RetryBlock);
	}

	Config.CanonExecution = OsvayderUECanonRouting::BuildInitialCanonExecution(
		Prompt,
		Options.ExecutionProfile,
		Config.PromptContract.ContextBlocks);
	EnsureFeatureWorkflowRoleContract(Config.CanonExecution.FeatureWorkflow);
	FOsvayderUEActivePlan ActivePlan;
	FString ActivePlanError;
	if (FOsvayderUERelayAgentManager::LoadActivePlan(ActivePlan, ActivePlanError)
		&& ShouldResumeFeatureWorkflowFromActivePlan(ActivePlan))
	{
		EnsureFeatureWorkflowRoleContract(ActivePlan.FeatureWorkflow);
		Config.CanonExecution.TaskMode = TEXT("feature_slice");
		Config.CanonExecution.RequestedToolFamily = ActivePlan.FeatureWorkflow.RecipeId;
		Config.CanonExecution.FeatureWorkflow = ActivePlan.FeatureWorkflow;
		Config.CanonExecution.bBriefPartBRequired = true;

		FAgentPromptContextBlock ResumeBlock;
		ResumeBlock.Label = TEXT("FEATURE WORKFLOW RESUME");
		ResumeBlock.Content = FString::Printf(
			TEXT("feature_workflow_id = %s\nrecipe_id = %s\nrole_id = %s\nevidence_schema_version = %d\ncurrent_phase = %s\nresume_rule = preserve workflow identity and continue the same bounded phase instead of reclassifying the task from scratch"),
			*ActivePlan.FeatureWorkflow.FeatureWorkflowId,
			*ActivePlan.FeatureWorkflow.RecipeId,
			*ActivePlan.FeatureWorkflow.RoleId,
			ActivePlan.FeatureWorkflow.EvidenceSchemaVersion,
			*ActivePlan.FeatureWorkflow.CurrentPhase);
		Config.PromptContract.ContextBlocks.Add(ResumeBlock);
	}
	OsvayderUECanonRouting::ApplyRuntimeToolExposure(Config);
	EnsureFeatureWorkflowRoleContract(Config.CanonExecution.FeatureWorkflow);
	if (Config.CanonExecution.FeatureWorkflow.HasAnySignal())
	{
		FAgentPromptContractBuilder::AppendRoleContractContext(
			Config.PromptContract,
			Config.CanonExecution.FeatureWorkflow.RoleId,
			Config.CanonExecution.FeatureWorkflow.RecipeId,
			Config.CanonExecution.FeatureWorkflow.EvidenceSchemaVersion);
	}
	else
	{
		FAgentPromptContractBuilder::AppendRoleContractContext(
			Config.PromptContract,
			OsvayderUERoleRegistry::WorkerRoleId(),
			FString(),
			0);
	}
	const FAgentBackendStatus ControlStatus = GetConfiguredBackendStatus();
	const FAgentProviderExecutionControlManifest ControlManifest = BuildAgentProviderExecutionControlManifest(ControlStatus, Config);
	Config.CurrentEffectiveProviderPowerClass = ControlManifest.CurrentEffectiveProviderPowerClass;
	Config.SystemPrompt = FAgentPromptMaterializer::MaterializeCanonicalText(Config.PromptContract);

	return Config;
}

FString FAgentOrchestrator::BuildConversationBootstrapText(const EAgentExecutionRunProfile Profile) const
{
	if (!SessionManager.IsValid())
	{
		return FString();
	}

	const TArray<TPair<FString, FString>>& History = ProfileUsesNormalProviderSession(Profile)
		? SessionManager->GetProviderSessionHistory(GetConfiguredBackend())
		: SessionManager->GetHistory(GetConfiguredBackend());
	if (History.Num() == 0)
	{
		return FString();
	}

	FString Bootstrap;
	const int32 StartIndex = FMath::Max(0, History.Num() - OsvayderUEConstants::Session::MaxHistoryInPrompt);
	for (int32 Index = StartIndex; Index < History.Num(); ++Index)
	{
		const TPair<FString, FString>& Exchange = History[Index];
		Bootstrap += FString::Printf(TEXT("Human: %s\n\nAssistant: %s\n\n"), *Exchange.Key, *Exchange.Value);
	}

	return Bootstrap.TrimStartAndEnd();
}

FString FAgentOrchestrator::BuildPromptWithHistory(const FString& NewPrompt, const EAgentExecutionRunProfile Profile) const
{
	if (!SessionManager.IsValid())
	{
		return NewPrompt;
	}

	const TArray<TPair<FString, FString>>& History = ProfileUsesNormalProviderSession(Profile)
		? SessionManager->GetProviderSessionHistory(GetConfiguredBackend())
		: SessionManager->GetHistory(GetConfiguredBackend());
	if (History.Num() == 0)
	{
		return NewPrompt;
	}

	FString PromptWithHistory;
	const int32 StartIndex = FMath::Max(0, History.Num() - OsvayderUEConstants::Session::MaxHistoryInPrompt);

	for (int32 Index = StartIndex; Index < History.Num(); ++Index)
	{
		const TPair<FString, FString>& Exchange = History[Index];
		PromptWithHistory += FString::Printf(TEXT("Human: %s\n\nAssistant: %s\n\n"), *Exchange.Key, *Exchange.Value);
	}

	PromptWithHistory += FString::Printf(TEXT("Human: %s"), *NewPrompt);
	return PromptWithHistory;
}
