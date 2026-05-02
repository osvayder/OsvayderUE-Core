// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"
#include "Dom/JsonObject.h"

struct UNREALCLAUDE_API FAgentExecutionPowerLane
{
	EAgentExecutionPowerClass PowerClass = EAgentExecutionPowerClass::ReadOnlyAnalysis;
	bool bAvailableNow = false;
	bool bCurrentlyEffective = false;
	bool bEnforcedNow = false;
	FString SelectionState;
	FString TruthBoundary;
	TArray<FString> Basis;
};

enum class EAgentExecutionRuntimeLane : uint8
{
	ReadOnlyAnalysis,
	WorkspaceWriteProject,
	BoundedPluginMutation,
	ExpertHighRiskProviderShell
};

inline const TCHAR* AgentExecutionRuntimeLaneToString(const EAgentExecutionRuntimeLane Lane)
{
	switch (Lane)
	{
	case EAgentExecutionRuntimeLane::WorkspaceWriteProject:
		return TEXT("workspace_write_project");

	case EAgentExecutionRuntimeLane::BoundedPluginMutation:
		return TEXT("bounded_plugin_mutation");

	case EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell:
		return TEXT("expert_high_risk_provider_shell");

	case EAgentExecutionRuntimeLane::ReadOnlyAnalysis:
	default:
		return TEXT("read_only_analysis");
	}
}

struct UNREALCLAUDE_API FAgentExecutionLaneTaxonomyEntry
{
	EAgentExecutionRuntimeLane Lane = EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
	FString DisplayName;
	FString Meaning;
	FString CurrentAvailability;
	FString SelectionState;
	FString LegacyPowerClassAlias;
	bool bDirectFileToolsAllowed = false;
	bool bDirectShellToolsAllowed = false;
	bool bMutatingUnrealMcpAllowed = false;
	bool bPersistentSessionCarryOverAllowedByDefault = false;
	TArray<FString> TypicalUses;
	TArray<FString> AllowedMutationFamilies;
	FString TruthBoundary;
	TArray<FString> Basis;
};

struct UNREALCLAUDE_API FAgentExecutionProfileLaneMapping
{
	FString ExecutionProfile;
	FString ControlProfileId;
	EAgentExecutionRuntimeLane CanonicalLane = EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
	FString LegacyPowerClassAlias;
	FString MappingState;
	FString SessionPersistenceMode;
	FString TruthBoundary;
	TArray<FString> Basis;
};

struct UNREALCLAUDE_API FAgentExecutionProviderTransportMatrixRow
{
	EAgentExecutionRuntimeLane Lane = EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
	FString BackendLabel;
	FString Transport;
	FString ApprovalPolicy;
	FString SandboxPolicy;
	bool bUnrealMcpBridgeEnabled = false;
	bool bDirectFileToolsAllowed = false;
	bool bDirectShellToolsAllowed = false;
	FString SessionPersistenceMode;
	TArray<FString> AllowedMutationFamilies;
	FString CheckpointRequirement;
	FString UiBadge;
	TArray<FString> TraceReceiptExpectations;
	FString BehaviorState;
	bool bAvailableNow = false;
	bool bCurrentProfileRow = false;
	bool bCurrentEffectiveLane = false;
	bool bEnforcedNow = false;
	FString TruthBoundary;
	TArray<FString> Basis;
};

struct UNREALCLAUDE_API FAgentExecutionPolicyDenySchema
{
	FString SchemaVersion = TEXT("policy_deny_contract_v1");
	FString ResultType = TEXT("policy_denied");
	TArray<FString> RequiredFields;
	TArray<FString> VisibleSurfaces;
	TArray<FString> SupportedRuleIds;
	bool bPracticalProbeAvailableNow = false;
	FString TruthBoundary;
};

struct UNREALCLAUDE_API FAgentExecutionPolicyDenyContract
{
	FString SchemaVersion = TEXT("policy_deny_contract_v1");
	FString ResultType = TEXT("policy_denied");
	FString RequestedLane;
	FString EffectiveLane;
	FString GoverningFamily;
	FString RequestedAction;
	FString DenyReason;
	FString PolicyRuleId;
	bool bSaferAlternativeExists = false;
	FString SaferAlternativeLane;
	bool bExpertOptInRequired = false;
	bool bSilentFallbackPrevented = true;
	FString DecisionSource;
	TArray<FString> VisibleSurfaces;
	FString TruthBoundary;
	TArray<FString> Basis;
};

struct UNREALCLAUDE_API FAgentExecutionSessionBoundaryRule
{
	EAgentExecutionRuntimeLane FromLane = EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
	EAgentExecutionRuntimeLane ToLane = EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
	FString BoundaryMode;
	FString HistoryCarryOver;
	FString ProviderSessionReuse;
	FString BoundaryArtifact;
	FString TruthBoundary;
	TArray<FString> Basis;
};

struct UNREALCLAUDE_API FAgentExecutionSessionBoundaryManifest
{
	FString SchemaVersion = TEXT("session_boundary_truth_v1");
	FString BoundaryStrategyId = TEXT("profile_and_session_persistence_split");
	FString SilentPrivilegeCarryOverPolicy = TEXT("forbidden");
	TArray<FString> BoundaryArtifacts;
	TArray<FAgentExecutionSessionBoundaryRule> Rules;
	FString TruthBoundary;
};

struct UNREALCLAUDE_API FAgentProviderExecutionControlManifest
{
	FString SchemaVersion = TEXT("provider_execution_control_v1");
	FString ManifestSource = TEXT("configured_backend_default_runtime");
	FString ControlProfileId;
	EUnrealClaudeProviderBackend Backend = EUnrealClaudeProviderBackend::ClaudeCli;
	FString BackendDisplayName;
	FString ExecutionTransportLabel;
	FString ExecutionProfile;
	EAgentExecutionGovernanceState ExecutionControlPlumbingState = EAgentExecutionGovernanceState::PartialPlumbing;
	EAgentSessionPersistenceMode SessionPersistenceMode = EAgentSessionPersistenceMode::NormalProviderSession;
	EAgentExecutionPowerClass CurrentEffectiveProviderPowerClass = EAgentExecutionPowerClass::HighRiskDirectFileShell;
	EAgentExecutionPowerClass DesiredFutureDefaultProviderPowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
	FString CurrentEffectivePowerSource = TEXT("runtime_flags_and_tool_budget");
	bool bCurrentEffectivePowerEnforcedNow = false;
	bool bDesiredFutureDefaultEnforcedNow = false;
	bool bFutureTighteningDescribedOnly = true;
	FString TruthBoundary;
	bool bPermissionBypassEnabled = false;
	FString ApprovalPolicy;
	FString SandboxMode;
	bool bRequestedToolAllowListPresent = false;
	bool bExplicitToolAllowListEnforced = false;
	FString EffectiveToolBudgetMode;
	bool bUnrealMcpBridgeEnabled = true;
	bool bMutatingMcpToolsTreatedAsAvailable = false;
	bool bRequestedDirectFileTools = false;
	bool bRequestedDirectShellTools = false;
	bool bDirectFilePowerTreatedAsAvailable = false;
	bool bDirectShellPowerTreatedAsAvailable = false;
	bool bBoundedMutationLaneAvailable = true;
	bool bTouchesNormalProviderSessionHistory = true;
	bool bWritesProviderSessionFileOnSuccess = true;
	TArray<FString> RequestedAllowedTools;
	TArray<FString> CurrentEffectivePowerBasis;
	FString SessionPersistenceTruthBoundary;
	TArray<FString> SessionPersistenceBasis;
	EAgentExecutionRuntimeLane CurrentEffectiveRuntimeLane = EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell;
	EAgentExecutionRuntimeLane DesiredFutureDefaultRuntimeLane = EAgentExecutionRuntimeLane::WorkspaceWriteProject;
	TArray<FAgentExecutionLaneTaxonomyEntry> RuntimeLaneTaxonomy;
	TArray<FAgentExecutionProfileLaneMapping> ProfileLaneMappings;
	TArray<FAgentExecutionProviderTransportMatrixRow> ProviderTransportMatrix;
	FAgentExecutionPolicyDenySchema PolicyDenySchema;
	FAgentExecutionSessionBoundaryManifest SessionBoundary;
	TArray<FAgentExecutionPowerLane> PowerLanes;
};

UNREALCLAUDE_API FAgentProviderExecutionControlManifest BuildAgentProviderExecutionControlManifest(
	const FAgentBackendStatus& Status,
	const FAgentRequestConfig& Config);

UNREALCLAUDE_API bool TryBuildAgentExecutionPolicyDenyContract(
	const FAgentBackendStatus& Status,
	const FAgentRequestConfig& Config,
	const FString& RequestedAction,
	FAgentExecutionPolicyDenyContract& OutContract);

UNREALCLAUDE_API bool TryParsePolicyDenyContractFromToolResultJson(
	const FString& ToolResultJson,
	FAgentExecutionPolicyDenyContract& OutContract);

UNREALCLAUDE_API TSharedPtr<FJsonObject> MakeAgentExecutionPolicyDenyContractJson(
	const FAgentExecutionPolicyDenyContract& Contract);

UNREALCLAUDE_API TSharedPtr<FJsonObject> MakeAgentProviderExecutionControlJson(
	const FAgentProviderExecutionControlManifest& Manifest);
