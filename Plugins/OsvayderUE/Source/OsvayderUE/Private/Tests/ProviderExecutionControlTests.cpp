// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "AgentExecutionControl.h"
#include "OsvayderSubsystem.h"
#include "MCP/MCPAsyncTask.h"
#include "MCP/MCPTaskQueue.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_PluginSettings.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUECanonRouting.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	TSharedPtr<FJsonObject> GetObjectFieldOrNull(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* NestedObject = nullptr;
		if (!Object->TryGetObjectField(FieldName, NestedObject) || !NestedObject || !(*NestedObject).IsValid())
		{
			return nullptr;
		}

		return *NestedObject;
	}

	bool JsonStringArrayContains(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& ExpectedValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString ActualValue;
			if (Value.IsValid() && Value->TryGetString(ActualValue) && ActualValue == ExpectedValue)
			{
				return true;
			}
		}

		return false;
	}

	TSharedPtr<FJsonObject> FindPowerLane(const TSharedPtr<FJsonObject>& ManifestObject, const FString& PowerClass)
	{
		if (!ManifestObject.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* PowerLanes = nullptr;
		if (!ManifestObject->TryGetArrayField(TEXT("power_lanes"), PowerLanes) || !PowerLanes)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& LaneValue : *PowerLanes)
		{
			const TSharedPtr<FJsonObject> LaneObject = LaneValue.IsValid() ? LaneValue->AsObject() : nullptr;
			if (!LaneObject.IsValid())
			{
				continue;
			}

			FString CurrentPowerClass;
			if (LaneObject->TryGetStringField(TEXT("power_class"), CurrentPowerClass) && CurrentPowerClass == PowerClass)
			{
				return LaneObject;
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> FindObjectInArrayByStringField(
		const TSharedPtr<FJsonObject>& Parent,
		const FString& ArrayField,
		const FString& KeyField,
		const FString& ExpectedValue)
	{
		if (!Parent.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Parent->TryGetArrayField(ArrayField, Values) || !Values)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Object.IsValid())
			{
				continue;
			}

			FString CurrentValue;
			if (Object->TryGetStringField(KeyField, CurrentValue) && CurrentValue == ExpectedValue)
			{
				return Object;
			}
		}

		return nullptr;
	}

	FAgentBackendStatus MakeSyntheticStatus(const EOsvayderUEProviderBackend Backend)
	{
		FAgentBackendStatus Status;
		Status.Backend = Backend;
		Status.DisplayName = Backend == EOsvayderUEProviderBackend::CodexCli ? TEXT("Codex CLI") : TEXT("Claude CLI");
		Status.bAvailable = true;
		Status.bReady = true;
		Status.Readiness = EAgentBackendReadiness::Ready;
		Status.AuthState = EAgentBackendAuthState::Authenticated;
		Status.Capabilities.Backend = Backend;
		Status.Capabilities.DisplayName = Status.DisplayName;
		Status.Capabilities.bSupportsToolAllowList = Backend == EOsvayderUEProviderBackend::ClaudeCli;
		return Status;
	}

	FAgentRequestConfig MakeSyntheticConfig(
		const EOsvayderUEProviderBackend Backend,
		const EAgentExecutionRunProfile Profile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime)
	{
		FAgentRequestConfig Config;
		Config.WorkingDirectory = FPaths::ProjectDir();
		Config.ExecutionProfile = Profile;
		Config.DesiredFutureDefaultProviderPowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
		Config.bExecutionProfileEnforcedNow = true;
		Config.bToolAllowListEnforced = Backend == EOsvayderUEProviderBackend::ClaudeCli;
		Config.bEnableUnrealMcpBridge = true;

		switch (Profile)
		{
		case EAgentExecutionRunProfile::BoundedPluginMutation:
			Config.ExecutionControlProfileId = TEXT("bounded_plugin_mutation_v1");
			Config.ExecutionTransportLabel = TEXT("governed_plugin_surface_only");
			Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
			Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
			Config.bSkipPermissions = false;
			Config.bEnableUnrealMcpBridge = false;
			Config.bForceDisablePersistentConversationTransport = Backend == EOsvayderUEProviderBackend::CodexCli;
			break;

		case EAgentExecutionRunProfile::ReadOnlyDiagnostic:
			Config.ExecutionControlProfileId = TEXT("read_only_diagnostic_v1");
			Config.ExecutionTransportLabel = Backend == EOsvayderUEProviderBackend::CodexCli
				? TEXT("exec_per_message")
				: TEXT("cli_process");
			Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
			Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
			Config.bSkipPermissions = false;
			Config.bEnableUnrealMcpBridge = false;
			Config.bForceDisablePersistentConversationTransport = Backend == EOsvayderUEProviderBackend::CodexCli;
			Config.AllowedTools = {
				TEXT("Read"),
				TEXT("Grep"),
				TEXT("Glob")
			};
			break;

		case EAgentExecutionRunProfile::ExplicitExpertOptIn:
			Config.ExecutionControlProfileId = TEXT("explicit_expert_opt_in_v1");
			Config.ExecutionTransportLabel = Backend == EOsvayderUEProviderBackend::CodexCli
				? TEXT("persistent_app_server")
				: TEXT("cli_process");
			Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
			Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NormalProviderSession;
			Config.bSkipPermissions = true;
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
			Config.ExecutionControlProfileId = TEXT("workspace_write_default_runtime_v1");
			Config.ExecutionTransportLabel = Backend == EOsvayderUEProviderBackend::CodexCli
				? TEXT("persistent_app_server")
				: TEXT("cli_process");
			Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
			Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
			Config.bSkipPermissions = false;
			Config.bEnableUnrealMcpBridge = false;
			Config.bForceDisablePersistentConversationTransport = false;
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

		return Config;
	}

	TSharedRef<FJsonObject> MakeBroadMutationExpertProbeParams(const FString& ToolName)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));

		if (ToolName == TEXT("asset")
			|| ToolName == TEXT("character")
			|| ToolName == TEXT("character_data")
			|| ToolName == TEXT("enhanced_input")
			|| ToolName == TEXT("material"))
		{
			Params->SetStringField(TEXT("operation"), TEXT("__invalid__"));
		}

		return Params;
	}

	bool LooksLikeBroadMutationExpertValidationFailure(const FString& ToolName, const FMCPToolResult& Result)
	{
		if (Result.Data.IsValid() && Result.Data->HasField(TEXT("policy_denied_contract")))
		{
			return false;
		}

		if (Result.Message.StartsWith(TEXT("Policy denied")))
		{
			return false;
		}

		if (ToolName == TEXT("delete_actors"))
		{
			return Result.Message.Contains(TEXT("No actors specified"));
		}

		if (ToolName == TEXT("move_actor") || ToolName == TEXT("set_property"))
		{
			return Result.Message.Contains(TEXT("actor_name")) || Result.Message.Contains(TEXT("Missing"));
		}

		if (ToolName == TEXT("spawn_actor"))
		{
			return Result.Message.Contains(TEXT("class")) || Result.Message.Contains(TEXT("Missing"));
		}

		return Result.Message.Contains(TEXT("Unknown operation"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_CodexManifestTruth,
	"OsvayderUE.ProviderExecutionControl.Manifest.CodexTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_CodexManifestTruth::RunTest(const FString& Parameters)
{
	const FAgentProviderExecutionControlManifest Manifest = BuildAgentProviderExecutionControlManifest(
		MakeSyntheticStatus(EOsvayderUEProviderBackend::CodexCli),
		MakeSyntheticConfig(EOsvayderUEProviderBackend::CodexCli));

	TestEqual(TEXT("Codex current effective provider power should now be workspace-write"), FString(AgentExecutionPowerClassToString(Manifest.CurrentEffectiveProviderPowerClass)), FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("Codex desired future default should now match workspace-write default"), FString(AgentExecutionPowerClassToString(Manifest.DesiredFutureDefaultProviderPowerClass)), FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("Codex governance state should be enforced for workspace-write default"), FString(AgentExecutionGovernanceStateToString(Manifest.ExecutionControlPlumbingState)), FString(TEXT("enforced")));
	TestEqual(TEXT("Codex approval policy should now be explicit never-ask for workspace-write default"), Manifest.ApprovalPolicy, FString(TEXT("ask_for_approval_never")));
	TestEqual(TEXT("Codex sandbox mode should now surface workspace-write"), Manifest.SandboxMode, FString(TEXT("workspace-write")));
	TestEqual(TEXT("Codex transport should now surface persistent app-server for workspace-write default"), Manifest.ExecutionTransportLabel, FString(TEXT("persistent_app_server")));
	TestFalse(TEXT("Codex should not claim explicit tool allow-list enforcement"), Manifest.bExplicitToolAllowListEnforced);
	TestTrue(TEXT("Codex direct file power should now be treated as available on workspace-write default"), Manifest.bDirectFilePowerTreatedAsAvailable);
	TestTrue(TEXT("Codex direct shell power should now be treated as available on workspace-write default"), Manifest.bDirectShellPowerTreatedAsAvailable);
	TestTrue(TEXT("Codex desired future default should now be the enforced default"), Manifest.bDesiredFutureDefaultEnforcedNow);
	TestFalse(TEXT("Codex workspace-write default should keep normal provider history untouched"), Manifest.bTouchesNormalProviderSessionHistory);
	TestFalse(TEXT("Codex workspace-write default should not update provider session file on success"), Manifest.bWritesProviderSessionFileOnSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_ClaudeManifestTruth,
	"OsvayderUE.ProviderExecutionControl.Manifest.ClaudeTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_ClaudeManifestTruth::RunTest(const FString& Parameters)
{
	const FAgentProviderExecutionControlManifest Manifest = BuildAgentProviderExecutionControlManifest(
		MakeSyntheticStatus(EOsvayderUEProviderBackend::ClaudeCli),
		MakeSyntheticConfig(EOsvayderUEProviderBackend::ClaudeCli));

	TestEqual(TEXT("Claude current effective provider power should now be workspace-write"), FString(AgentExecutionPowerClassToString(Manifest.CurrentEffectiveProviderPowerClass)), FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("Claude approval policy should no longer skip permissions"), Manifest.ApprovalPolicy, FString(TEXT("interactive_or_provider_default")));
	TestEqual(TEXT("Claude sandbox mode should stay provider managed"), Manifest.SandboxMode, FString(TEXT("provider_managed_or_unspecified")));
	TestTrue(TEXT("Claude should report explicit tool allow-list enforcement"), Manifest.bExplicitToolAllowListEnforced);
	TestEqual(TEXT("Claude effective tool budget mode should expose allow-list enforcement"), Manifest.EffectiveToolBudgetMode, FString(TEXT("explicit_allow_list_enforced")));
	TestTrue(TEXT("Claude direct file power should now be treated as available"), Manifest.bDirectFilePowerTreatedAsAvailable);
	TestTrue(TEXT("Claude direct shell power should now be treated as available"), Manifest.bDirectShellPowerTreatedAsAvailable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_ReadOnlyManifestTruth,
	"OsvayderUE.ProviderExecutionControl.Manifest.ReadOnlyDiagnosticTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_ReadOnlyManifestTruth::RunTest(const FString& Parameters)
{
	const FAgentProviderExecutionControlManifest Manifest = BuildAgentProviderExecutionControlManifest(
		MakeSyntheticStatus(EOsvayderUEProviderBackend::CodexCli),
		MakeSyntheticConfig(EOsvayderUEProviderBackend::CodexCli, EAgentExecutionRunProfile::ReadOnlyDiagnostic));

	TestEqual(TEXT("Read-only helper current effective provider power should be read only"), FString(AgentExecutionPowerClassToString(Manifest.CurrentEffectiveProviderPowerClass)), FString(TEXT("read_only_analysis")));
	TestEqual(TEXT("Read-only helper desired future default should now track workspace-write default"), FString(AgentExecutionPowerClassToString(Manifest.DesiredFutureDefaultProviderPowerClass)), FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("Read-only helper governance state should be enforced"), FString(AgentExecutionGovernanceStateToString(Manifest.ExecutionControlPlumbingState)), FString(TEXT("enforced")));
	TestEqual(TEXT("Read-only helper control profile should be stable"), Manifest.ControlProfileId, FString(TEXT("read_only_diagnostic_v1")));
	TestEqual(TEXT("Read-only helper transport should force exec per message on Codex"), Manifest.ExecutionTransportLabel, FString(TEXT("exec_per_message")));
	TestEqual(TEXT("Read-only helper approval policy should stay provider managed"), Manifest.ApprovalPolicy, FString(TEXT("provider_default_or_interactive")));
	TestFalse(TEXT("Read-only helper should not enable permission bypass"), Manifest.bPermissionBypassEnabled);
	TestFalse(TEXT("Read-only helper should disable Unreal MCP"), Manifest.bUnrealMcpBridgeEnabled);
	TestFalse(TEXT("Read-only helper should not treat mutating MCP tools as available"), Manifest.bMutatingMcpToolsTreatedAsAvailable);
	TestFalse(TEXT("Read-only helper should not treat direct file power as available"), Manifest.bDirectFilePowerTreatedAsAvailable);
	TestFalse(TEXT("Read-only helper should not treat direct shell power as available"), Manifest.bDirectShellPowerTreatedAsAvailable);
	TestEqual(TEXT("Read-only helper should surface not_persisted session mode"), FString(AgentSessionPersistenceModeToString(Manifest.SessionPersistenceMode)), FString(TEXT("not_persisted")));
	TestFalse(TEXT("Read-only helper should not touch normal provider history"), Manifest.bTouchesNormalProviderSessionHistory);
	TestFalse(TEXT("Read-only helper should not update provider session file on success"), Manifest.bWritesProviderSessionFileOnSuccess);
	TestTrue(TEXT("Read-only helper should enforce the current effective power now"), Manifest.bCurrentEffectivePowerEnforcedNow);
	TestFalse(TEXT("Read-only helper should not claim the broader future default is already enforced"), Manifest.bDesiredFutureDefaultEnforcedNow);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_BoundedManifestTruth,
	"OsvayderUE.ProviderExecutionControl.Manifest.BoundedPluginMutationTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_BoundedManifestTruth::RunTest(const FString& Parameters)
{
	const FAgentProviderExecutionControlManifest Manifest = BuildAgentProviderExecutionControlManifest(
		MakeSyntheticStatus(EOsvayderUEProviderBackend::CodexCli),
		MakeSyntheticConfig(EOsvayderUEProviderBackend::CodexCli, EAgentExecutionRunProfile::BoundedPluginMutation));

	TestEqual(TEXT("Bounded profile current effective provider power should be bounded"), FString(AgentExecutionPowerClassToString(Manifest.CurrentEffectiveProviderPowerClass)), FString(TEXT("bounded_mutation_capable")));
	TestEqual(TEXT("Bounded profile current effective runtime lane should be bounded"), FString(AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)), FString(TEXT("bounded_plugin_mutation")));
	TestEqual(TEXT("Bounded profile governance state should be enforced"), FString(AgentExecutionGovernanceStateToString(Manifest.ExecutionControlPlumbingState)), FString(TEXT("enforced")));
	TestEqual(TEXT("Bounded profile control profile should be stable"), Manifest.ControlProfileId, FString(TEXT("bounded_plugin_mutation_v1")));
	TestEqual(TEXT("Bounded profile transport should stay governed"), Manifest.ExecutionTransportLabel, FString(TEXT("governed_plugin_surface_only")));
	TestFalse(TEXT("Bounded profile should not enable permission bypass"), Manifest.bPermissionBypassEnabled);
	TestFalse(TEXT("Bounded profile should disable direct mutating MCP exposure on generic prompt dispatch"), Manifest.bMutatingMcpToolsTreatedAsAvailable);
	TestFalse(TEXT("Bounded profile should not treat direct file power as available"), Manifest.bDirectFilePowerTreatedAsAvailable);
	TestFalse(TEXT("Bounded profile should not treat direct shell power as available"), Manifest.bDirectShellPowerTreatedAsAvailable);
	TestEqual(TEXT("Bounded profile should surface not_persisted session mode"), FString(AgentSessionPersistenceModeToString(Manifest.SessionPersistenceMode)), FString(TEXT("not_persisted")));
	TestFalse(TEXT("Bounded profile should not touch normal provider history"), Manifest.bTouchesNormalProviderSessionHistory);
	TestFalse(TEXT("Bounded profile should not update provider session file on success"), Manifest.bWritesProviderSessionFileOnSuccess);
	TestFalse(TEXT("Bounded profile should not claim the ordinary workspace-write default is already enforced here"), Manifest.bDesiredFutureDefaultEnforcedNow);
	TestTrue(TEXT("Bounded profile should still describe the distinct default outside this lane"), Manifest.bFutureTighteningDescribedOnly);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_ExplicitExpertManifestTruth,
	"OsvayderUE.ProviderExecutionControl.Manifest.ExplicitExpertOptInTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_ExplicitExpertManifestTruth::RunTest(const FString& Parameters)
{
	const FAgentProviderExecutionControlManifest Manifest = BuildAgentProviderExecutionControlManifest(
		MakeSyntheticStatus(EOsvayderUEProviderBackend::CodexCli),
		MakeSyntheticConfig(EOsvayderUEProviderBackend::CodexCli, EAgentExecutionRunProfile::ExplicitExpertOptIn));

	TestEqual(TEXT("Explicit expert profile current effective provider power should stay high risk"), FString(AgentExecutionPowerClassToString(Manifest.CurrentEffectiveProviderPowerClass)), FString(TEXT("high_risk_direct_file_shell")));
	TestEqual(TEXT("Explicit expert profile current effective runtime lane should be expert"), FString(AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)), FString(TEXT("expert_high_risk_provider_shell")));
	TestEqual(TEXT("Explicit expert profile governance state should be enforced"), FString(AgentExecutionGovernanceStateToString(Manifest.ExecutionControlPlumbingState)), FString(TEXT("enforced")));
	TestEqual(TEXT("Explicit expert profile control profile should be stable"), Manifest.ControlProfileId, FString(TEXT("explicit_expert_opt_in_v1")));
	TestEqual(TEXT("Explicit expert profile should keep normal provider session mode"), FString(AgentSessionPersistenceModeToString(Manifest.SessionPersistenceMode)), FString(TEXT("normal_provider_session")));
	TestTrue(TEXT("Explicit expert profile should enable permission bypass"), Manifest.bPermissionBypassEnabled);
	TestTrue(TEXT("Explicit expert profile should treat direct file power as available"), Manifest.bDirectFilePowerTreatedAsAvailable);
	TestTrue(TEXT("Explicit expert profile should treat direct shell power as available"), Manifest.bDirectShellPowerTreatedAsAvailable);
	TestTrue(TEXT("Explicit expert profile should touch normal provider history"), Manifest.bTouchesNormalProviderSessionHistory);
	TestTrue(TEXT("Explicit expert profile should update provider session file on success"), Manifest.bWritesProviderSessionFileOnSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_CanonicalLaneSchemaTruth,
	"OsvayderUE.ProviderExecutionControl.Manifest.CanonicalLaneSchemaTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_CanonicalLaneSchemaTruth::RunTest(const FString& Parameters)
{
	const FAgentProviderExecutionControlManifest Manifest = BuildAgentProviderExecutionControlManifest(
		MakeSyntheticStatus(EOsvayderUEProviderBackend::CodexCli),
		MakeSyntheticConfig(EOsvayderUEProviderBackend::CodexCli));

	TestEqual(TEXT("current effective runtime lane should map to workspace-write default"), FString(AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)), FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("desired future default runtime lane should map to workspace-write default"), FString(AgentExecutionRuntimeLaneToString(Manifest.DesiredFutureDefaultRuntimeLane)), FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("runtime lane taxonomy should expose the four canonical lanes"), Manifest.RuntimeLaneTaxonomy.Num(), 4);
	TestEqual(TEXT("profile lane mappings should expose the current four runtime profiles"), Manifest.ProfileLaneMappings.Num(), 4);
	TestEqual(TEXT("provider transport matrix should expose the four canonical rows"), Manifest.ProviderTransportMatrix.Num(), 4);
	TestEqual(TEXT("policy deny schema version should stay stable"), Manifest.PolicyDenySchema.SchemaVersion, FString(TEXT("policy_deny_contract_v1")));
	TestEqual(TEXT("session boundary schema version should stay stable"), Manifest.SessionBoundary.SchemaVersion, FString(TEXT("session_boundary_truth_v1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_PolicyDenyContractTruth,
	"OsvayderUE.ProviderExecutionControl.Manifest.PolicyDenyContractTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_PolicyDenyContractTruth::RunTest(const FString& Parameters)
{
	FAgentRequestConfig Config = MakeSyntheticConfig(EOsvayderUEProviderBackend::CodexCli, EAgentExecutionRunProfile::ReadOnlyDiagnostic);
	Config.AllowedTools.Add(TEXT("Write"));

	FAgentExecutionPolicyDenyContract Contract;
	const bool bDenied = TryBuildAgentExecutionPolicyDenyContract(
		MakeSyntheticStatus(EOsvayderUEProviderBackend::CodexCli),
		Config,
		TEXT("direct_file_tools"),
		Contract);

	TestTrue(TEXT("read-only direct file request should build a deny contract"), bDenied);
	if (!bDenied)
	{
		return false;
	}

	TestEqual(TEXT("deny contract schema should stay stable"), Contract.SchemaVersion, FString(TEXT("policy_deny_contract_v1")));
	TestEqual(TEXT("requested lane should stay read_only_analysis"), Contract.RequestedLane, FString(TEXT("read_only_analysis")));
	TestEqual(TEXT("effective lane should stay read_only_analysis for the helper probe"), Contract.EffectiveLane, FString(TEXT("read_only_analysis")));
	TestEqual(TEXT("governing family should stay provider_control_status"), Contract.GoverningFamily, FString(TEXT("provider_control_status")));
	TestEqual(TEXT("policy rule id should stay specific"), Contract.PolicyRuleId, FString(TEXT("read_only_analysis.direct_file_tools_denied")));
	TestTrue(TEXT("deny contract should require expert opt-in for direct file power"), Contract.bExpertOptInRequired);
	TestFalse(TEXT("deny contract should not claim a safer alternative for direct file power"), Contract.bSaferAlternativeExists);
	TestTrue(TEXT("deny contract should prevent silent fallback"), Contract.bSilentFallbackPrevented);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_BoundedPolicyDenyContractTruth,
	"OsvayderUE.ProviderExecutionControl.Manifest.BoundedPolicyDenyContractTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_BoundedPolicyDenyContractTruth::RunTest(const FString& Parameters)
{
	const FAgentRequestConfig Config = MakeSyntheticConfig(EOsvayderUEProviderBackend::CodexCli, EAgentExecutionRunProfile::BoundedPluginMutation);

	FAgentExecutionPolicyDenyContract Contract;
	const bool bDenied = TryBuildAgentExecutionPolicyDenyContract(
		MakeSyntheticStatus(EOsvayderUEProviderBackend::CodexCli),
		Config,
		TEXT("provider_prompt_dispatch"),
		Contract);

	TestTrue(TEXT("bounded provider prompt dispatch should build a deny contract"), bDenied);
	if (!bDenied)
	{
		return false;
	}

	TestEqual(TEXT("bounded deny requested lane should stay bounded"), Contract.RequestedLane, FString(TEXT("bounded_plugin_mutation")));
	TestEqual(TEXT("bounded deny effective lane should stay bounded"), Contract.EffectiveLane, FString(TEXT("bounded_plugin_mutation")));
	TestEqual(TEXT("bounded deny policy rule should stay specific"), Contract.PolicyRuleId, FString(TEXT("bounded_plugin_mutation.provider_prompt_dispatch_denied")));
	TestTrue(TEXT("bounded deny should require expert opt-in"), Contract.bExpertOptInRequired);
	TestFalse(TEXT("bounded deny should not claim a generic safer alternative"), Contract.bSaferAlternativeExists);
	TestTrue(TEXT("bounded deny should prevent silent fallback"), Contract.bSilentFallbackPrevented);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_RepresentativeMcpSurfaceDenyTruth,
	"OsvayderUE.ProviderExecutionControl.MCP.RepresentativeSurfaceDenyTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_RepresentativeMcpSurfaceDenyTruth::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;

	TSharedRef<FJsonObject> SpawnActorParams = MakeShared<FJsonObject>();
	SpawnActorParams->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
	const FMCPToolResult SpawnActorResult = Registry.ExecuteTool(TEXT("spawn_actor"), SpawnActorParams);
	TestFalse(TEXT("spawn_actor should be denied on configured_default_runtime"), SpawnActorResult.bSuccess);
	TestTrue(TEXT("spawn_actor deny should carry structured data"), SpawnActorResult.Data.IsValid());
	if (SpawnActorResult.Data.IsValid())
	{
		FString ResultType;
		TestTrue(TEXT("spawn_actor deny should expose result_type"), SpawnActorResult.Data->TryGetStringField(TEXT("result_type"), ResultType));
		TestEqual(TEXT("spawn_actor deny should expose policy_denied result type"), ResultType, FString(TEXT("policy_denied")));

		const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(SpawnActorResult.Data, TEXT("policy_denied_contract"));
		TestTrue(TEXT("spawn_actor deny should expose policy contract"), ContractObject.IsValid());
		if (ContractObject.IsValid())
		{
			FString RequestedLane;
			FString PolicyRuleId;
			TestTrue(TEXT("spawn_actor deny should expose requested_lane"), ContractObject->TryGetStringField(TEXT("requested_lane"), RequestedLane));
			TestTrue(TEXT("spawn_actor deny should expose policy_rule_id"), ContractObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
			TestEqual(TEXT("spawn_actor deny should stay on workspace_write_project lane"), RequestedLane, FString(TEXT("workspace_write_project")));
			TestEqual(TEXT("spawn_actor deny should use broad authoring mutation rule"), PolicyRuleId, FString(TEXT("workspace_write_project.broad_authoring_mutation_surface_denied")));
		}
	}

	TSharedRef<FJsonObject> ConsoleParams = MakeShared<FJsonObject>();
	ConsoleParams->SetStringField(TEXT("execution_profile"), TEXT("bounded_plugin_mutation"));
	const FMCPToolResult ConsoleResult = Registry.ExecuteTool(TEXT("run_console_command"), ConsoleParams);
	TestFalse(TEXT("run_console_command should be denied on bounded_plugin_mutation"), ConsoleResult.bSuccess);
	TestTrue(TEXT("run_console_command deny should carry structured data"), ConsoleResult.Data.IsValid());
	if (ConsoleResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(ConsoleResult.Data, TEXT("policy_denied_contract"));
		TestTrue(TEXT("run_console_command deny should expose policy contract"), ContractObject.IsValid());
		if (ContractObject.IsValid())
		{
			FString RequestedLane;
			FString PolicyRuleId;
			TestTrue(TEXT("run_console_command deny should expose requested_lane"), ContractObject->TryGetStringField(TEXT("requested_lane"), RequestedLane));
			TestTrue(TEXT("run_console_command deny should expose policy_rule_id"), ContractObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
			TestEqual(TEXT("run_console_command deny should stay on bounded lane"), RequestedLane, FString(TEXT("bounded_plugin_mutation")));
			TestEqual(TEXT("run_console_command deny should use representative high-risk rule"), PolicyRuleId, FString(TEXT("bounded_plugin_mutation.representative_high_risk_execution_surface_denied")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_BroadMutationBucketDenyTruth,
	"OsvayderUE.ProviderExecutionControl.MCP.BroadMutationBucketDenyTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_BroadMutationBucketDenyTruth::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;

	const TArray<FString> BroadMutationTools = {
		TEXT("spawn_actor"),
		TEXT("delete_actors"),
		TEXT("move_actor"),
		TEXT("set_property"),
		TEXT("asset"),
		TEXT("character"),
		TEXT("character_data"),
		TEXT("enhanced_input"),
		TEXT("material")
	};

	for (const FString& ToolName : BroadMutationTools)
	{
		TSharedRef<FJsonObject> DefaultParams = MakeShared<FJsonObject>();
		DefaultParams->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
		const FMCPToolResult DefaultResult = Registry.ExecuteTool(ToolName, DefaultParams);
		TestFalse(FString::Printf(TEXT("%s should be denied on configured_default_runtime"), *ToolName), DefaultResult.bSuccess);
		TestTrue(FString::Printf(TEXT("%s default deny should carry structured data"), *ToolName), DefaultResult.Data.IsValid());
		if (DefaultResult.Data.IsValid())
		{
			FString ResultType;
			TestTrue(FString::Printf(TEXT("%s default deny should expose result_type"), *ToolName), DefaultResult.Data->TryGetStringField(TEXT("result_type"), ResultType));
			TestEqual(FString::Printf(TEXT("%s default deny should expose policy_denied result type"), *ToolName), ResultType, FString(TEXT("policy_denied")));

			const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(DefaultResult.Data, TEXT("policy_denied_contract"));
			TestTrue(FString::Printf(TEXT("%s default deny should expose policy contract"), *ToolName), ContractObject.IsValid());
			if (ContractObject.IsValid())
			{
				FString RequestedLane;
				FString GoverningFamily;
				FString PolicyRuleId;
				FString TruthBoundary;
				TestTrue(FString::Printf(TEXT("%s default deny should expose requested_lane"), *ToolName), ContractObject->TryGetStringField(TEXT("requested_lane"), RequestedLane));
				TestTrue(FString::Printf(TEXT("%s default deny should expose governing_family"), *ToolName), ContractObject->TryGetStringField(TEXT("governing_family"), GoverningFamily));
				TestTrue(FString::Printf(TEXT("%s default deny should expose policy_rule_id"), *ToolName), ContractObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
				TestTrue(FString::Printf(TEXT("%s default deny should expose truth_boundary"), *ToolName), ContractObject->TryGetStringField(TEXT("truth_boundary"), TruthBoundary));
				TestEqual(FString::Printf(TEXT("%s default deny should stay on workspace_write_project lane"), *ToolName), RequestedLane, FString(TEXT("workspace_write_project")));
				TestEqual(FString::Printf(TEXT("%s default deny should stay in broad_authoring_mutation_backlog"), *ToolName), GoverningFamily, FString(TEXT("broad_authoring_mutation_backlog")));
				TestEqual(FString::Printf(TEXT("%s default deny should use broad mutation rule"), *ToolName), PolicyRuleId, FString(TEXT("workspace_write_project.broad_authoring_mutation_surface_denied")));
				TestTrue(FString::Printf(TEXT("%s default deny should expose full broad mutation bucket basis"), *ToolName),
					JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("registry_runtime_gate=full_broad_authoring_mutation_bucket")));
				TestTrue(FString::Printf(TEXT("%s default deny should expose tool-surface granularity basis"), *ToolName),
					JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("tool_surface_granularity=true")));
				TestTrue(FString::Printf(TEXT("%s default deny truth boundary should mention full broad mutation bucket"), *ToolName),
					TruthBoundary.Contains(TEXT("full broad authoring mutation backlog bucket")));
			}
		}

		TSharedRef<FJsonObject> BoundedParams = MakeShared<FJsonObject>();
		BoundedParams->SetStringField(TEXT("execution_profile"), TEXT("bounded_plugin_mutation"));
		const FMCPToolResult BoundedResult = Registry.ExecuteTool(ToolName, BoundedParams);
		TestFalse(FString::Printf(TEXT("%s should be denied on bounded_plugin_mutation"), *ToolName), BoundedResult.bSuccess);
		TestTrue(FString::Printf(TEXT("%s bounded deny should carry structured data"), *ToolName), BoundedResult.Data.IsValid());
		if (BoundedResult.Data.IsValid())
		{
			const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(BoundedResult.Data, TEXT("policy_denied_contract"));
			TestTrue(FString::Printf(TEXT("%s bounded deny should expose policy contract"), *ToolName), ContractObject.IsValid());
			if (ContractObject.IsValid())
			{
				FString RequestedLane;
				FString GoverningFamily;
				FString PolicyRuleId;
				FString TruthBoundary;
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose requested_lane"), *ToolName), ContractObject->TryGetStringField(TEXT("requested_lane"), RequestedLane));
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose governing_family"), *ToolName), ContractObject->TryGetStringField(TEXT("governing_family"), GoverningFamily));
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose policy_rule_id"), *ToolName), ContractObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose truth_boundary"), *ToolName), ContractObject->TryGetStringField(TEXT("truth_boundary"), TruthBoundary));
				TestEqual(FString::Printf(TEXT("%s bounded deny should stay on bounded lane"), *ToolName), RequestedLane, FString(TEXT("bounded_plugin_mutation")));
				TestEqual(FString::Printf(TEXT("%s bounded deny should stay in broad_authoring_mutation_backlog"), *ToolName), GoverningFamily, FString(TEXT("broad_authoring_mutation_backlog")));
				TestEqual(FString::Printf(TEXT("%s bounded deny should use broad mutation rule"), *ToolName), PolicyRuleId, FString(TEXT("bounded_plugin_mutation.broad_authoring_mutation_surface_denied")));
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose full broad mutation bucket basis"), *ToolName),
					JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("registry_runtime_gate=full_broad_authoring_mutation_bucket")));
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose tool-surface granularity basis"), *ToolName),
					JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("tool_surface_granularity=true")));
				TestTrue(FString::Printf(TEXT("%s bounded deny truth boundary should mention full broad mutation bucket"), *ToolName),
					TruthBoundary.Contains(TEXT("full broad authoring mutation backlog bucket")));
			}
		}

		const FMCPToolResult ExpertResult = Registry.ExecuteTool(ToolName, MakeBroadMutationExpertProbeParams(ToolName));
		TestFalse(FString::Printf(TEXT("%s expert probe should fail harmlessly instead of mutating"), *ToolName), ExpertResult.bSuccess);
		TestTrue(FString::Printf(TEXT("%s expert probe should bypass the policy deny gate"), *ToolName),
			LooksLikeBroadMutationExpertValidationFailure(ToolName, ExpertResult));
	}

	TSharedRef<FJsonObject> WrappedDefaultParams = MakeShared<FJsonObject>();
	WrappedDefaultParams->SetStringField(TEXT("tool_name"), TEXT("spawn_actor"));
	WrappedDefaultParams->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
	WrappedDefaultParams->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
	const FMCPToolResult WrappedDefaultResult = Registry.ExecuteTool(TEXT("task_submit"), WrappedDefaultParams);
	TestFalse(TEXT("task_submit should deny spawn_actor on configured_default_runtime"), WrappedDefaultResult.bSuccess);
	TestTrue(TEXT("task_submit broad mutation deny should expose structured data"), WrappedDefaultResult.Data.IsValid());
	if (WrappedDefaultResult.Data.IsValid())
	{
		FString TargetToolName;
		TestTrue(TEXT("task_submit broad mutation deny should expose target_tool_name"), WrappedDefaultResult.Data->TryGetStringField(TEXT("target_tool_name"), TargetToolName));
		TestEqual(TEXT("task_submit broad mutation deny should point at spawn_actor"), TargetToolName, FString(TEXT("spawn_actor")));

		const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(WrappedDefaultResult.Data, TEXT("policy_denied_contract"));
		TestTrue(TEXT("task_submit broad mutation deny should expose policy contract"), ContractObject.IsValid());
		if (ContractObject.IsValid())
		{
			FString RequestedAction;
			FString PolicyRuleId;
			TestTrue(TEXT("task_submit broad mutation deny should expose requested_action"), ContractObject->TryGetStringField(TEXT("requested_action"), RequestedAction));
			TestTrue(TEXT("task_submit broad mutation deny should expose policy_rule_id"), ContractObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
			TestEqual(TEXT("task_submit broad mutation deny should expose wrapper action"), RequestedAction, FString(TEXT("task_submit:spawn_actor")));
			TestEqual(TEXT("task_submit broad mutation deny should use broad mutation rule"), PolicyRuleId, FString(TEXT("workspace_write_project.broad_authoring_mutation_surface_denied")));
			TestTrue(TEXT("task_submit broad mutation deny should expose full broad mutation bucket basis"),
				JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("registry_runtime_gate=full_broad_authoring_mutation_bucket")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_TaskSubmitPolicyBoundaryTruth,
	"OsvayderUE.ProviderExecutionControl.MCP.TaskSubmitPolicyBoundaryTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_TaskSubmitPolicyBoundaryTruth::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	const TSharedPtr<FMCPTaskQueue> TaskQueue = Registry.GetTaskQueue();
	TestTrue(TEXT("task queue should exist"), TaskQueue.IsValid());
	if (!TaskQueue.IsValid())
	{
		return false;
	}

	TSharedRef<FJsonObject> SafeSubmitParams = MakeShared<FJsonObject>();
	SafeSubmitParams->SetStringField(TEXT("tool_name"), TEXT("execute_script"));
	SafeSubmitParams->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
	SafeSubmitParams->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
	const FMCPToolResult SafeSubmitResult = Registry.ExecuteTool(TEXT("task_submit"), SafeSubmitParams);
	TestFalse(TEXT("task_submit should deny representative high-risk target on safer default"), SafeSubmitResult.bSuccess);
	TestTrue(TEXT("task_submit deny should expose structured data"), SafeSubmitResult.Data.IsValid());
	if (SafeSubmitResult.Data.IsValid())
	{
		FString TargetToolName;
		TestTrue(TEXT("task_submit deny should expose target_tool_name"), SafeSubmitResult.Data->TryGetStringField(TEXT("target_tool_name"), TargetToolName));
		TestEqual(TEXT("task_submit deny should point at execute_script"), TargetToolName, FString(TEXT("execute_script")));

		const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(SafeSubmitResult.Data, TEXT("policy_denied_contract"));
		TestTrue(TEXT("task_submit deny should expose policy contract"), ContractObject.IsValid());
		if (ContractObject.IsValid())
		{
			FString RequestedAction;
			TestTrue(TEXT("task_submit deny should expose requested_action"), ContractObject->TryGetStringField(TEXT("requested_action"), RequestedAction));
			TestEqual(TEXT("task_submit deny should expose wrapper action"), RequestedAction, FString(TEXT("task_submit:execute_script")));
		}
	}

	TSharedRef<FJsonObject> ExpertSubmitParams = MakeShared<FJsonObject>();
	ExpertSubmitParams->SetStringField(TEXT("tool_name"), TEXT("execute_script"));
	ExpertSubmitParams->SetStringField(TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));
	ExpertSubmitParams->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
	const FMCPToolResult ExpertSubmitResult = Registry.ExecuteTool(TEXT("task_submit"), ExpertSubmitParams);
	TestTrue(TEXT("task_submit should allow representative high-risk target on expert lane"), ExpertSubmitResult.bSuccess);
	TestTrue(TEXT("expert task_submit should expose result data"), ExpertSubmitResult.Data.IsValid());
	if (!ExpertSubmitResult.bSuccess || !ExpertSubmitResult.Data.IsValid())
	{
		return false;
	}

	FString TaskIdString;
	TestTrue(TEXT("expert task_submit should return a task_id"), ExpertSubmitResult.Data->TryGetStringField(TEXT("task_id"), TaskIdString));
	FGuid TaskId;
	TestTrue(TEXT("expert task_submit task_id should parse"), FGuid::Parse(TaskIdString, TaskId));
	const TSharedPtr<FMCPAsyncTask> Task = TaskQueue->GetTask(TaskId);
	TestTrue(TEXT("submitted expert task should exist"), Task.IsValid());
	if (!Task.IsValid() || !Task->Parameters.IsValid())
	{
		return false;
	}

	FString PropagatedExecutionProfile;
	TestTrue(TEXT("submitted expert task should propagate execution_profile"), Task->Parameters->TryGetStringField(TEXT("execution_profile"), PropagatedExecutionProfile));
	TestEqual(TEXT("submitted expert task should keep explicit expert profile"), PropagatedExecutionProfile, FString(TEXT("explicit_expert_opt_in")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_CanonScopedEnhancedInputAllowedOnConfiguredDefault,
	"OsvayderUE.ProviderExecutionControl.MCP.CanonScopedEnhancedInputAllowedOnConfiguredDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_CanonScopedEnhancedInputAllowedOnConfiguredDefault::RunTest(const FString& Parameters)
{
	const FString Prompt = TEXT("Create a new sprint input action and mapping context using the canonical Enhanced Input path.");
	FAgentRequestConfig Config = MakeSyntheticConfig(
		EOsvayderUEProviderBackend::CodexCli,
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime);
	Config.CanonExecution = OsvayderUECanonRouting::BuildInitialCanonExecution(
		Prompt,
		Config.ExecutionProfile,
		Config.PromptContract.ContextBlocks);
	OsvayderUECanonRouting::ApplyRuntimeToolExposure(Config);

	TestEqual(TEXT("canon-scoped prompt should classify to unreal_input"), Config.CanonExecution.RequestedToolFamily, FString(TEXT("unreal_input")));
	TestTrue(TEXT("canon-scoped prompt should expose enhanced_input"), Config.AllowedTools.Contains(TEXT("mcp__osvayderue__enhanced_input")));

	const FAgentBackendStatus Status = MakeSyntheticStatus(EOsvayderUEProviderBackend::CodexCli);
	const FString RunId = FOsvayderUEAgentTraceLog::Get().BeginRun(Status, Config, Prompt, true, true);

	FMCPToolRegistry Registry;
	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
	ParamsObject->SetStringField(TEXT("operation"), TEXT("create_input_action"));
	const FMCPToolResult Result = Registry.ExecuteTool(TEXT("enhanced_input"), ParamsObject);

	FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, TEXT("validation failed as expected"), false);

	TestFalse(TEXT("probe should still fail because required params are missing"), Result.bSuccess);
	TestFalse(TEXT("canon-scoped enhanced_input should not be blocked by policy"), Result.Message.StartsWith(TEXT("Policy denied")));
	TestTrue(
		TEXT("canon-scoped enhanced_input failure should fall through to tool validation"),
		Result.Message.Contains(TEXT("action_name")) || Result.Message.Contains(TEXT("Missing")));
	TestTrue(
		TEXT("canon-scoped enhanced_input failure should not carry policy_denied_contract"),
		!Result.Data.IsValid() || !Result.Data->HasField(TEXT("policy_denied_contract")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_ExternalUiBucketDenyTruth,
	"OsvayderUE.ProviderExecutionControl.MCP.ExternalUiBucketDenyTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_ExternalUiBucketDenyTruth::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;

	const TArray<FString> ExternalUiTools = {
		TEXT("osvayder_mouse_click"),
		TEXT("osvayder_mouse_double_click"),
		TEXT("osvayder_mouse_move"),
		TEXT("osvayder_mouse_drag"),
		TEXT("osvayder_mouse_scroll"),
		TEXT("osvayder_keyboard_type"),
		TEXT("osvayder_keyboard_hotkey"),
		TEXT("osvayder_keyboard_press"),
		TEXT("osvayder_focus_window")
	};

	for (const FString& ToolName : ExternalUiTools)
	{
		TSharedRef<FJsonObject> DefaultParams = MakeShared<FJsonObject>();
		DefaultParams->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
		const FMCPToolResult DefaultResult = Registry.ExecuteTool(ToolName, DefaultParams);
		TestFalse(FString::Printf(TEXT("%s should be denied on configured_default_runtime"), *ToolName), DefaultResult.bSuccess);
		TestTrue(FString::Printf(TEXT("%s default deny should carry structured data"), *ToolName), DefaultResult.Data.IsValid());
		if (DefaultResult.Data.IsValid())
		{
			FString ResultType;
			TestTrue(FString::Printf(TEXT("%s default deny should expose result_type"), *ToolName), DefaultResult.Data->TryGetStringField(TEXT("result_type"), ResultType));
			TestEqual(FString::Printf(TEXT("%s default deny should expose policy_denied result type"), *ToolName), ResultType, FString(TEXT("policy_denied")));

			const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(DefaultResult.Data, TEXT("policy_denied_contract"));
			TestTrue(FString::Printf(TEXT("%s default deny should expose policy contract"), *ToolName), ContractObject.IsValid());
			if (ContractObject.IsValid())
			{
				FString RequestedLane;
				FString GoverningFamily;
				FString PolicyRuleId;
				FString TruthBoundary;
				TestTrue(FString::Printf(TEXT("%s default deny should expose requested_lane"), *ToolName), ContractObject->TryGetStringField(TEXT("requested_lane"), RequestedLane));
				TestTrue(FString::Printf(TEXT("%s default deny should expose governing_family"), *ToolName), ContractObject->TryGetStringField(TEXT("governing_family"), GoverningFamily));
				TestTrue(FString::Printf(TEXT("%s default deny should expose policy_rule_id"), *ToolName), ContractObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
				TestTrue(FString::Printf(TEXT("%s default deny should expose truth_boundary"), *ToolName), ContractObject->TryGetStringField(TEXT("truth_boundary"), TruthBoundary));
				TestEqual(FString::Printf(TEXT("%s default deny should stay on workspace_write_project lane"), *ToolName), RequestedLane, FString(TEXT("workspace_write_project")));
				TestEqual(FString::Printf(TEXT("%s default deny should stay in external_ui_control_backlog"), *ToolName), GoverningFamily, FString(TEXT("external_ui_control_backlog")));
				TestEqual(FString::Printf(TEXT("%s default deny should use external UI rule"), *ToolName), PolicyRuleId, FString(TEXT("workspace_write_project.external_ui_control_surface_denied")));
				TestTrue(FString::Printf(TEXT("%s default deny should expose full-bucket runtime gate basis"), *ToolName),
					JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("registry_runtime_gate=full_external_ui_control_bucket")));
				TestTrue(FString::Printf(TEXT("%s default deny truth boundary should mention full external UI bucket"), *ToolName),
					TruthBoundary.Contains(TEXT("full external UI control backlog bucket")));
			}
		}

		TSharedRef<FJsonObject> BoundedParams = MakeShared<FJsonObject>();
		BoundedParams->SetStringField(TEXT("execution_profile"), TEXT("bounded_plugin_mutation"));
		const FMCPToolResult BoundedResult = Registry.ExecuteTool(ToolName, BoundedParams);
		TestFalse(FString::Printf(TEXT("%s should be denied on bounded_plugin_mutation"), *ToolName), BoundedResult.bSuccess);
		TestTrue(FString::Printf(TEXT("%s bounded deny should carry structured data"), *ToolName), BoundedResult.Data.IsValid());
		if (BoundedResult.Data.IsValid())
		{
			const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(BoundedResult.Data, TEXT("policy_denied_contract"));
			TestTrue(FString::Printf(TEXT("%s bounded deny should expose policy contract"), *ToolName), ContractObject.IsValid());
			if (ContractObject.IsValid())
			{
				FString RequestedLane;
				FString PolicyRuleId;
				FString TruthBoundary;
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose requested_lane"), *ToolName), ContractObject->TryGetStringField(TEXT("requested_lane"), RequestedLane));
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose policy_rule_id"), *ToolName), ContractObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose truth_boundary"), *ToolName), ContractObject->TryGetStringField(TEXT("truth_boundary"), TruthBoundary));
				TestEqual(FString::Printf(TEXT("%s bounded deny should stay on bounded lane"), *ToolName), RequestedLane, FString(TEXT("bounded_plugin_mutation")));
				TestEqual(FString::Printf(TEXT("%s bounded deny should use external UI rule"), *ToolName), PolicyRuleId, FString(TEXT("bounded_plugin_mutation.external_ui_control_surface_denied")));
				TestTrue(FString::Printf(TEXT("%s bounded deny should expose full-bucket runtime gate basis"), *ToolName),
					JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("registry_runtime_gate=full_external_ui_control_bucket")));
				TestTrue(FString::Printf(TEXT("%s bounded deny truth boundary should mention full external UI bucket"), *ToolName),
					TruthBoundary.Contains(TEXT("full external UI control backlog bucket")));
			}
		}

		TSharedRef<FJsonObject> ExpertParams = MakeShared<FJsonObject>();
		ExpertParams->SetStringField(TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));
		FMCPToolResult ExpertDenyResult;
		const bool bExpertDenied = Registry.TryBuildGovernanceDenyResult(ToolName, ExpertParams, ExpertDenyResult);
		TestFalse(FString::Printf(TEXT("%s should stay reachable beyond the deny gate on explicit expert lane"), *ToolName), bExpertDenied);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_ExpertLaneStillSeparateForRepresentativeSurface,
	"OsvayderUE.ProviderExecutionControl.MCP.ExpertLaneStillSeparateForRepresentativeSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_ExpertLaneStillSeparateForRepresentativeSurface::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));
	const FMCPToolResult Result = Registry.ExecuteTool(TEXT("spawn_actor"), Params);

	TestFalse(TEXT("spawn_actor without class should still fail"), Result.bSuccess);
	TestTrue(TEXT("expert lane call should not be policy-denied"), !Result.Message.Contains(TEXT("Policy denied")));
	TestTrue(TEXT("expert lane call should not carry policy_denied_contract"), !Result.Data.IsValid() || !Result.Data->HasField(TEXT("policy_denied_contract")));
	TestTrue(TEXT("expert lane failure should fall through to tool validation"), Result.Message.Contains(TEXT("class")) || Result.Message.Contains(TEXT("Missing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_IntegratedRuntimeProofBaseline,
	"OsvayderUE.ProviderExecutionControl.MCP.IntegratedRuntimeProofBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_IntegratedRuntimeProofBaseline::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	bool bOverallSuccess = true;

	auto CheckPolicyDeny = [this, &Registry](
		const FString& ScenarioLabel,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Params,
		const FString& ExpectedRequestedLane,
		const FString& ExpectedGoverningFamily,
		const FString& ExpectedRuleId,
		const FString& ExpectedRuntimeGateBasis,
		const bool bExpectToolSurfaceGranularity,
		const FString& ExpectedTruthBoundaryNeedle,
		const FString& ExpectedTargetToolName = FString(),
		const FString& ExpectedRequestedAction = FString()) -> bool
	{
		bool bLocalSuccess = true;
		const FMCPToolResult Result = Registry.ExecuteTool(ToolName, Params);
		bLocalSuccess &= TestFalse(FString::Printf(TEXT("%s should be policy denied"), *ScenarioLabel), Result.bSuccess);
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should return structured deny data"), *ScenarioLabel), Result.Data.IsValid());
		if (!Result.Data.IsValid())
		{
			return false;
		}

		FString ResultType;
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose result_type"), *ScenarioLabel), Result.Data->TryGetStringField(TEXT("result_type"), ResultType));
		bLocalSuccess &= TestEqual(FString::Printf(TEXT("%s should expose policy_denied result_type"), *ScenarioLabel), ResultType, FString(TEXT("policy_denied")));

		if (!ExpectedTargetToolName.IsEmpty())
		{
			FString TargetToolName;
			bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose target_tool_name"), *ScenarioLabel), Result.Data->TryGetStringField(TEXT("target_tool_name"), TargetToolName));
			bLocalSuccess &= TestEqual(FString::Printf(TEXT("%s should keep the wrapped target tool"), *ScenarioLabel), TargetToolName, ExpectedTargetToolName);
		}

		const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(Result.Data, TEXT("policy_denied_contract"));
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose policy_denied_contract"), *ScenarioLabel), ContractObject.IsValid());
		if (!ContractObject.IsValid())
		{
			return false;
		}

		FString RequestedLane;
		FString GoverningFamily;
		FString PolicyRuleId;
		FString TruthBoundary;
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose requested_lane"), *ScenarioLabel), ContractObject->TryGetStringField(TEXT("requested_lane"), RequestedLane));
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose governing_family"), *ScenarioLabel), ContractObject->TryGetStringField(TEXT("governing_family"), GoverningFamily));
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose policy_rule_id"), *ScenarioLabel), ContractObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose truth_boundary"), *ScenarioLabel), ContractObject->TryGetStringField(TEXT("truth_boundary"), TruthBoundary));
		bLocalSuccess &= TestEqual(FString::Printf(TEXT("%s should keep the requested lane"), *ScenarioLabel), RequestedLane, ExpectedRequestedLane);
		bLocalSuccess &= TestEqual(FString::Printf(TEXT("%s should keep the governing family"), *ScenarioLabel), GoverningFamily, ExpectedGoverningFamily);
		bLocalSuccess &= TestEqual(FString::Printf(TEXT("%s should keep the deny rule id"), *ScenarioLabel), PolicyRuleId, ExpectedRuleId);
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose the expected runtime gate basis"), *ScenarioLabel),
			JsonStringArrayContains(ContractObject, TEXT("basis"), ExpectedRuntimeGateBasis));
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose the expected tool-surface granularity basis"), *ScenarioLabel),
			JsonStringArrayContains(
				ContractObject,
				TEXT("basis"),
				bExpectToolSurfaceGranularity ? TEXT("tool_surface_granularity=true") : TEXT("tool_surface_granularity=false")));
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s truth_boundary should mention the current proof boundary"), *ScenarioLabel),
			TruthBoundary.Contains(ExpectedTruthBoundaryNeedle));

		if (!ExpectedRequestedAction.IsEmpty())
		{
			FString RequestedAction;
			bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should expose requested_action"), *ScenarioLabel), ContractObject->TryGetStringField(TEXT("requested_action"), RequestedAction));
			bLocalSuccess &= TestEqual(FString::Printf(TEXT("%s should expose the wrapped requested_action"), *ScenarioLabel), RequestedAction, ExpectedRequestedAction);
		}

		return bLocalSuccess;
	};

	auto CheckExplicitExpertValidationFailure = [this, &Registry](
		const FString& ScenarioLabel,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Params,
		const FString& ExpectedMessageNeedle) -> bool
	{
		bool bLocalSuccess = true;
		const FMCPToolResult Result = Registry.ExecuteTool(ToolName, Params);
		bLocalSuccess &= TestFalse(FString::Printf(TEXT("%s should fail harmlessly instead of mutating"), *ScenarioLabel), Result.bSuccess);
		bLocalSuccess &= TestFalse(FString::Printf(TEXT("%s should not surface a policy deny message"), *ScenarioLabel), Result.Message.StartsWith(TEXT("Policy denied")));
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should fall through to validation messaging"), *ScenarioLabel), Result.Message.Contains(ExpectedMessageNeedle));
		bLocalSuccess &= TestTrue(FString::Printf(TEXT("%s should not carry policy_denied_contract"), *ScenarioLabel),
			!Result.Data.IsValid() || !Result.Data->HasField(TEXT("policy_denied_contract")));
		return bLocalSuccess;
	};

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
		bOverallSuccess &= CheckPolicyDeny(
			TEXT("configured_default_runtime spawn_actor"),
			TEXT("spawn_actor"),
			ParamsObject,
			TEXT("workspace_write_project"),
			TEXT("broad_authoring_mutation_backlog"),
			TEXT("workspace_write_project.broad_authoring_mutation_surface_denied"),
			TEXT("registry_runtime_gate=full_broad_authoring_mutation_bucket"),
			true,
			TEXT("full broad authoring mutation backlog bucket"));
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
		bOverallSuccess &= CheckPolicyDeny(
			TEXT("configured_default_runtime run_console_command"),
			TEXT("run_console_command"),
			ParamsObject,
			TEXT("workspace_write_project"),
			TEXT("high_risk_execution_backlog"),
			TEXT("workspace_write_project.representative_high_risk_execution_surface_denied"),
			TEXT("registry_runtime_gate=representative_surface_only"),
			false,
			TEXT("currently governed high-risk execution backlog surfaces"));
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
		bOverallSuccess &= CheckPolicyDeny(
			TEXT("configured_default_runtime osvayder_mouse_click"),
			TEXT("osvayder_mouse_click"),
			ParamsObject,
			TEXT("workspace_write_project"),
			TEXT("external_ui_control_backlog"),
			TEXT("workspace_write_project.external_ui_control_surface_denied"),
			TEXT("registry_runtime_gate=full_external_ui_control_bucket"),
			false,
			TEXT("full external UI control backlog bucket"));
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("bounded_plugin_mutation"));
		bOverallSuccess &= CheckPolicyDeny(
			TEXT("bounded_plugin_mutation spawn_actor"),
			TEXT("spawn_actor"),
			ParamsObject,
			TEXT("bounded_plugin_mutation"),
			TEXT("broad_authoring_mutation_backlog"),
			TEXT("bounded_plugin_mutation.broad_authoring_mutation_surface_denied"),
			TEXT("registry_runtime_gate=full_broad_authoring_mutation_bucket"),
			true,
			TEXT("full broad authoring mutation backlog bucket"));
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("bounded_plugin_mutation"));
		bOverallSuccess &= CheckPolicyDeny(
			TEXT("bounded_plugin_mutation run_console_command"),
			TEXT("run_console_command"),
			ParamsObject,
			TEXT("bounded_plugin_mutation"),
			TEXT("high_risk_execution_backlog"),
			TEXT("bounded_plugin_mutation.representative_high_risk_execution_surface_denied"),
			TEXT("registry_runtime_gate=representative_surface_only"),
			false,
			TEXT("currently governed high-risk execution backlog surfaces"));
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("bounded_plugin_mutation"));
		bOverallSuccess &= CheckPolicyDeny(
			TEXT("bounded_plugin_mutation osvayder_mouse_click"),
			TEXT("osvayder_mouse_click"),
			ParamsObject,
			TEXT("bounded_plugin_mutation"),
			TEXT("external_ui_control_backlog"),
			TEXT("bounded_plugin_mutation.external_ui_control_surface_denied"),
			TEXT("registry_runtime_gate=full_external_ui_control_bucket"),
			false,
			TEXT("full external UI control backlog bucket"));
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("bounded_plugin_mutation"));
		ParamsObject->SetStringField(TEXT("operation"), TEXT("get_reflected_contract"));
		ParamsObject->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
		ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
		ParamsObject->SetBoolField(TEXT("include_properties"), true);
		ParamsObject->SetBoolField(TEXT("include_functions"), false);
		ParamsObject->SetNumberField(TEXT("member_limit"), 64);

		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("cpp_reflection"), ParamsObject);
		bOverallSuccess &= TestTrue(TEXT("bounded_plugin_mutation cpp_reflection should stay allowed for governed family proof"), Result.bSuccess);
		bOverallSuccess &= TestTrue(TEXT("bounded_plugin_mutation cpp_reflection should return data"), Result.Data.IsValid());
		if (Result.bSuccess && Result.Data.IsValid())
		{
			bOverallSuccess &= TestFalse(TEXT("bounded_plugin_mutation cpp_reflection should not return policy_denied_contract"),
				Result.Data->HasField(TEXT("policy_denied_contract")));

			const TSharedPtr<FJsonObject>* ContractObjectPtr = nullptr;
			bOverallSuccess &= TestTrue(TEXT("bounded_plugin_mutation cpp_reflection should expose contract object"),
				Result.Data->TryGetObjectField(TEXT("contract"), ContractObjectPtr) && ContractObjectPtr && (*ContractObjectPtr).IsValid());
			if (ContractObjectPtr && (*ContractObjectPtr).IsValid())
			{
				FString CppName;
				FString Kind;
				(*ContractObjectPtr)->TryGetStringField(TEXT("cpp_name"), CppName);
				(*ContractObjectPtr)->TryGetStringField(TEXT("kind"), Kind);
				bOverallSuccess &= TestEqual(TEXT("bounded_plugin_mutation cpp_reflection should resolve UOsvayderUESettings"), CppName, FString(TEXT("UOsvayderUESettings")));
				bOverallSuccess &= TestEqual(TEXT("bounded_plugin_mutation cpp_reflection should stay on class contract"), Kind, FString(TEXT("class")));
			}
		}
		else
		{
			return false;
		}
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));
		bOverallSuccess &= CheckExplicitExpertValidationFailure(
			TEXT("explicit_expert_opt_in spawn_actor"),
			TEXT("spawn_actor"),
			ParamsObject,
			TEXT("class"));
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));
		bOverallSuccess &= CheckExplicitExpertValidationFailure(
			TEXT("explicit_expert_opt_in run_console_command"),
			TEXT("run_console_command"),
			ParamsObject,
			TEXT("command"));
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));
		ParamsObject->SetStringField(TEXT("title"), TEXT("__provider_execution_control_integrated_runtime_focus_probe__"));
		FMCPToolResult DenyResult;
		const bool bDenied = Registry.TryBuildGovernanceDenyResult(TEXT("osvayder_focus_window"), ParamsObject, DenyResult);
		bOverallSuccess &= TestFalse(TEXT("explicit_expert_opt_in osvayder_focus_window should stay reachable beyond the deny gate"), bDenied);
	}

	return bOverallSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_PluginSettingsManifest,
	"OsvayderUE.ProviderExecutionControl.PluginSettingsManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_CppDeclarationLaneGovernanceTruth,
	"OsvayderUE.ProviderExecutionControl.MCP.CppDeclarationLaneGovernanceTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_CppDeclarationLaneGovernanceTruth::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	bool bOverallSuccess = true;

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
		ParamsObject->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
		ParamsObject->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
		ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
		ParamsObject->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
		ParamsObject->SetStringField(TEXT("new_member_name"), TEXT("bHc5Slice1GovernedPreviewProbe"));
		ParamsObject->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
		ParamsObject->SetStringField(TEXT("category"), TEXT("Assistant"));
		ParamsObject->SetStringField(TEXT("default_value"), TEXT("false"));

		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("cpp_reflection"), ParamsObject);
		bOverallSuccess &= TestFalse(TEXT("configured_default_runtime should deny bounded declaration preview lane"), Result.bSuccess);
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should return structured data"), Result.Data.IsValid());
		if (!Result.Data.IsValid())
		{
			return false;
		}

		FString ResultType;
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should expose result_type"), Result.Data->TryGetStringField(TEXT("result_type"), ResultType));
		bOverallSuccess &= TestEqual(TEXT("configured_default_runtime deny should expose policy_denied result_type"), ResultType, FString(TEXT("policy_denied")));

		const TSharedPtr<FJsonObject> ContractObject = GetObjectFieldOrNull(Result.Data, TEXT("policy_denied_contract"));
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should expose policy_denied_contract"), ContractObject.IsValid());
		if (!ContractObject.IsValid())
		{
			return false;
		}

		FString RequestedLane;
		FString EffectiveLane;
		FString GoverningFamily;
		FString PolicyRuleId;
		FString RequestedAction;
		FString SaferAlternativeLane;
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should expose requested_lane"), ContractObject->TryGetStringField(TEXT("requested_lane"), RequestedLane));
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should expose effective_lane"), ContractObject->TryGetStringField(TEXT("effective_lane"), EffectiveLane));
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should expose governing_family"), ContractObject->TryGetStringField(TEXT("governing_family"), GoverningFamily));
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should expose policy_rule_id"), ContractObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should expose requested_action"), ContractObject->TryGetStringField(TEXT("requested_action"), RequestedAction));
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should expose safer_alternative_lane"), ContractObject->TryGetStringField(TEXT("safer_alternative_lane"), SaferAlternativeLane));
		bOverallSuccess &= TestEqual(TEXT("configured_default_runtime deny should stay on workspace_write_project"), RequestedLane, FString(TEXT("workspace_write_project")));
		bOverallSuccess &= TestEqual(TEXT("configured_default_runtime deny effective lane should stay workspace_write_project"), EffectiveLane, FString(TEXT("workspace_write_project")));
		bOverallSuccess &= TestEqual(TEXT("configured_default_runtime deny should keep cpp_reflected_contracts family"), GoverningFamily, FString(TEXT("cpp_reflected_contracts")));
		bOverallSuccess &= TestEqual(TEXT("configured_default_runtime deny should use declaration-lane rule"), PolicyRuleId, FString(TEXT("workspace_write_project.cpp_reflected_declaration_lane_denied")));
		bOverallSuccess &= TestEqual(TEXT("configured_default_runtime deny should point at bounded_plugin_mutation as safer alternative"), SaferAlternativeLane, FString(TEXT("bounded_plugin_mutation")));
		bOverallSuccess &= TestEqual(TEXT("configured_default_runtime deny should expose operation-specific requested_action"),
			RequestedAction,
			FString(TEXT("mcp_tool:cpp_reflection.preview_reflected_property_declaration")));
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should capture requested operation basis"),
			JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("requested_operation=preview_reflected_property_declaration")));
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should capture declaration runtime gate basis"),
			JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("registry_runtime_gate=cpp_reflected_declaration_lane")));
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime deny should keep operation-level granularity basis"),
			JsonStringArrayContains(ContractObject, TEXT("basis"), TEXT("tool_surface_granularity=false")));
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("configured_default_runtime"));
		ParamsObject->SetStringField(TEXT("operation"), TEXT("get_reflected_contract"));
		ParamsObject->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
		ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
		ParamsObject->SetBoolField(TEXT("include_properties"), true);
		ParamsObject->SetBoolField(TEXT("include_functions"), false);
		ParamsObject->SetNumberField(TEXT("member_limit"), 64);

		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("cpp_reflection"), ParamsObject);
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime should still allow read-only reflected contract discovery"), Result.bSuccess);
		bOverallSuccess &= TestTrue(TEXT("configured_default_runtime read-only discovery should return data"), Result.Data.IsValid());
		if (Result.bSuccess && Result.Data.IsValid())
		{
			bOverallSuccess &= TestFalse(TEXT("configured_default_runtime read-only discovery should not return policy_denied_contract"),
				Result.Data->HasField(TEXT("policy_denied_contract")));
		}
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("bounded_plugin_mutation"));
		ParamsObject->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
		ParamsObject->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
		ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
		ParamsObject->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
		ParamsObject->SetStringField(TEXT("new_member_name"), TEXT("bHc5Slice1GovernedPreviewProbe"));
		ParamsObject->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
		ParamsObject->SetStringField(TEXT("category"), TEXT("Assistant"));
		ParamsObject->SetStringField(TEXT("default_value"), TEXT("false"));

		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("cpp_reflection"), ParamsObject);
		bOverallSuccess &= TestTrue(TEXT("bounded_plugin_mutation should still allow declaration preview lane"), Result.bSuccess);
		bOverallSuccess &= TestTrue(TEXT("bounded_plugin_mutation declaration preview should return data"), Result.Data.IsValid());
		if (Result.bSuccess && Result.Data.IsValid())
		{
			bOverallSuccess &= TestFalse(TEXT("bounded_plugin_mutation declaration preview should not return policy_denied_contract"),
				Result.Data->HasField(TEXT("policy_denied_contract")));

			FString Operation;
			FString PreviewSchemaVersion;
			bOverallSuccess &= TestTrue(TEXT("bounded_plugin_mutation declaration preview should expose operation"), Result.Data->TryGetStringField(TEXT("operation"), Operation));
			bOverallSuccess &= TestTrue(TEXT("bounded_plugin_mutation declaration preview should expose preview_schema_version"), Result.Data->TryGetStringField(TEXT("preview_schema_version"), PreviewSchemaVersion));
			bOverallSuccess &= TestEqual(TEXT("bounded_plugin_mutation declaration preview should stay on preview_reflected_property_declaration"),
				Operation,
				FString(TEXT("preview_reflected_property_declaration")));
			bOverallSuccess &= TestEqual(TEXT("bounded_plugin_mutation declaration preview should keep stable schema"),
				PreviewSchemaVersion,
				FString(TEXT("reflected_property_declaration_preview_v1")));
		}
		else
		{
			return false;
		}
	}

	{
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));
		ParamsObject->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
		ParamsObject->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
		ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
		ParamsObject->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
		ParamsObject->SetStringField(TEXT("new_member_name"), TEXT("bHc5Slice1GovernedExpertProbe"));
		ParamsObject->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
		ParamsObject->SetStringField(TEXT("category"), TEXT("Assistant"));
		ParamsObject->SetStringField(TEXT("default_value"), TEXT("false"));

		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("cpp_reflection"), ParamsObject);
		bOverallSuccess &= TestTrue(TEXT("explicit_expert_opt_in should remain reachable for the bounded declaration preview lane"), Result.bSuccess);
		bOverallSuccess &= TestTrue(TEXT("explicit_expert_opt_in declaration preview should return data"), Result.Data.IsValid());
		if (Result.bSuccess && Result.Data.IsValid())
		{
			bOverallSuccess &= TestFalse(TEXT("explicit_expert_opt_in declaration preview should not return policy_denied_contract"),
				Result.Data->HasField(TEXT("policy_denied_contract")));
		}
	}

	return bOverallSuccess;
}

bool FProviderExecutionControl_PluginSettingsManifest::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	const FAgentProviderExecutionControlManifest LiveExplicitExpertManifest =
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> BackendObject = GetObjectFieldOrNull(Result.Data, TEXT("assistant_backend"));
	TestTrue(TEXT("assistant_backend should exist"), BackendObject.IsValid());
	if (!BackendObject.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(BackendObject, TEXT("provider_execution_control"));
	TestTrue(TEXT("provider_execution_control should exist"), ManifestObject.IsValid());
	if (!ManifestObject.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ExecutionProfilesObject = GetObjectFieldOrNull(BackendObject, TEXT("execution_profiles"));
	TestTrue(TEXT("execution_profiles should exist"), ExecutionProfilesObject.IsValid());
	if (!ExecutionProfilesObject.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> BoundedManifestObject = GetObjectFieldOrNull(ExecutionProfilesObject, TEXT("bounded_plugin_mutation"));
	TestTrue(TEXT("execution_profiles.bounded_plugin_mutation should exist"), BoundedManifestObject.IsValid());
	if (!BoundedManifestObject.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ReadOnlyManifestObject = GetObjectFieldOrNull(ExecutionProfilesObject, TEXT("read_only_diagnostic"));
	TestTrue(TEXT("execution_profiles.read_only_diagnostic should exist"), ReadOnlyManifestObject.IsValid());
	if (!ReadOnlyManifestObject.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ExplicitExpertManifestObject = GetObjectFieldOrNull(ExecutionProfilesObject, TEXT("explicit_expert_opt_in"));
	TestTrue(TEXT("execution_profiles.explicit_expert_opt_in should exist"), ExplicitExpertManifestObject.IsValid());
	if (!ExplicitExpertManifestObject.IsValid())
	{
		return false;
	}

	FString SchemaVersion;
	FString CurrentPower;
	FString DesiredPower;
	FString PlumbingState;
	FString BackendName;
	FString SessionPersistenceMode;
	TestTrue(TEXT("schema_version should exist"), ManifestObject->TryGetStringField(TEXT("schema_version"), SchemaVersion));
	TestTrue(TEXT("current_effective_provider_power should exist"), ManifestObject->TryGetStringField(TEXT("current_effective_provider_power"), CurrentPower));
	TestTrue(TEXT("desired_future_default_provider_power should exist"), ManifestObject->TryGetStringField(TEXT("desired_future_default_provider_power"), DesiredPower));
	TestTrue(TEXT("execution_control_plumbing_state should exist"), ManifestObject->TryGetStringField(TEXT("execution_control_plumbing_state"), PlumbingState));
	TestTrue(TEXT("backend should exist"), ManifestObject->TryGetStringField(TEXT("backend"), BackendName));
	TestTrue(TEXT("session_persistence_mode should exist"), BackendObject->TryGetStringField(TEXT("session_persistence_mode"), SessionPersistenceMode));
	TestEqual(TEXT("manifest schema version should match"), SchemaVersion, FString(TEXT("provider_execution_control_v1")));
	TestEqual(TEXT("current effective power should now be workspace-write"), CurrentPower, FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("desired future default should now match workspace-write"), DesiredPower, FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("plumbing state should now be enforced"), PlumbingState, FString(TEXT("enforced")));
	TestEqual(TEXT("default current path should now be boundary-isolated"), SessionPersistenceMode, FString(TEXT("not_persisted")));

	bool bPermissionBypassEnabled = false;
	bool bDesiredFutureDefaultEnforcedNow = true;
	bool bDirectFilePowerTreatedAsAvailable = false;
	bool bDirectShellPowerTreatedAsAvailable = false;
	bool bNormalProviderSessionHistoryTouched = false;
	bool bProviderSessionFileUpdatedOnSuccess = false;
	FString CurrentRuntimeLane;
	FString DesiredRuntimeLane;
	TestTrue(TEXT("permission_bypass_enabled should exist"), ManifestObject->TryGetBoolField(TEXT("permission_bypass_enabled"), bPermissionBypassEnabled));
	TestTrue(TEXT("desired_future_default_enforced_now should exist"), ManifestObject->TryGetBoolField(TEXT("desired_future_default_enforced_now"), bDesiredFutureDefaultEnforcedNow));
	TestTrue(TEXT("direct_file_power_treated_as_available should exist"), ManifestObject->TryGetBoolField(TEXT("direct_file_power_treated_as_available"), bDirectFilePowerTreatedAsAvailable));
	TestTrue(TEXT("direct_shell_power_treated_as_available should exist"), ManifestObject->TryGetBoolField(TEXT("direct_shell_power_treated_as_available"), bDirectShellPowerTreatedAsAvailable));
	TestTrue(TEXT("normal_provider_session_history_touched should exist"), BackendObject->TryGetBoolField(TEXT("normal_provider_session_history_touched"), bNormalProviderSessionHistoryTouched));
	TestTrue(TEXT("provider_session_file_updated_on_success should exist"), BackendObject->TryGetBoolField(TEXT("provider_session_file_updated_on_success"), bProviderSessionFileUpdatedOnSuccess));
	TestTrue(TEXT("current_effective_runtime_lane should exist"), ManifestObject->TryGetStringField(TEXT("current_effective_runtime_lane"), CurrentRuntimeLane));
	TestTrue(TEXT("desired_future_default_runtime_lane should exist"), ManifestObject->TryGetStringField(TEXT("desired_future_default_runtime_lane"), DesiredRuntimeLane));
	TestFalse(TEXT("permission bypass should now be disabled in safer default"), bPermissionBypassEnabled);
	TestTrue(TEXT("desired future default should now be enforced on the ordinary default"), bDesiredFutureDefaultEnforcedNow);
	TestTrue(TEXT("direct file power should now be treated as available"), bDirectFilePowerTreatedAsAvailable);
	TestTrue(TEXT("direct shell power should now be treated as available"), bDirectShellPowerTreatedAsAvailable);
	TestFalse(TEXT("default path should not touch normal provider history"), bNormalProviderSessionHistoryTouched);
	TestFalse(TEXT("default path should not update provider session file on success"), bProviderSessionFileUpdatedOnSuccess);
	TestEqual(TEXT("current runtime lane should now stay workspace_write_project"), CurrentRuntimeLane, FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("desired future default runtime lane should now stay workspace_write_project"), DesiredRuntimeLane, FString(TEXT("workspace_write_project")));

	const TSharedPtr<FJsonObject> HighRiskLane = FindPowerLane(ManifestObject, TEXT("high_risk_direct_file_shell"));
	const TSharedPtr<FJsonObject> WorkspaceWriteLane = FindPowerLane(ManifestObject, TEXT("workspace_write_project"));
	const TSharedPtr<FJsonObject> ReadOnlyLane = FindPowerLane(ManifestObject, TEXT("read_only_analysis"));
	const TSharedPtr<FJsonObject> BoundedLane = FindPowerLane(ManifestObject, TEXT("bounded_mutation_capable"));
	TestTrue(TEXT("manifest should include workspace_write_project lane"), WorkspaceWriteLane.IsValid());
	TestTrue(TEXT("manifest should include read_only_analysis lane"), ReadOnlyLane.IsValid());
	TestTrue(TEXT("manifest should include bounded_mutation_capable lane"), BoundedLane.IsValid());
	TestTrue(TEXT("manifest should include high_risk_direct_file_shell lane"), HighRiskLane.IsValid());

	if (WorkspaceWriteLane.IsValid())
	{
		bool bCurrentlyEffective = false;
		TestTrue(TEXT("workspace-write lane should expose currently_effective"), WorkspaceWriteLane->TryGetBoolField(TEXT("currently_effective"), bCurrentlyEffective));
		TestTrue(TEXT("workspace-write lane should now be the current default"), bCurrentlyEffective);
	}

	if (HighRiskLane.IsValid())
	{
		bool bCurrentlyEffective = false;
		TestTrue(TEXT("high risk lane should expose currently_effective"), HighRiskLane->TryGetBoolField(TEXT("currently_effective"), bCurrentlyEffective));
		TestFalse(TEXT("high risk lane should no longer be current default"), bCurrentlyEffective);
	}

	if (BackendName == TEXT("CodexCli"))
	{
		FString ApprovalPolicy;
		FString SandboxMode;
		bool bExplicitAllowListEnforced = true;
		ManifestObject->TryGetStringField(TEXT("approval_policy"), ApprovalPolicy);
		ManifestObject->TryGetStringField(TEXT("sandbox_mode"), SandboxMode);
		ManifestObject->TryGetBoolField(TEXT("explicit_tool_allow_list_enforced"), bExplicitAllowListEnforced);
		TestEqual(TEXT("Codex readback should surface ask_for_approval_never"), ApprovalPolicy, FString(TEXT("ask_for_approval_never")));
		TestEqual(TEXT("Codex readback should surface workspace-write sandbox truth"), SandboxMode, FString(TEXT("workspace-write")));
		TestFalse(TEXT("Codex readback should stay truthful about allow-list enforcement"), bExplicitAllowListEnforced);
	}
	else
	{
		FString ApprovalPolicy;
		FString SandboxMode;
		bool bExplicitAllowListEnforced = false;
		ManifestObject->TryGetStringField(TEXT("approval_policy"), ApprovalPolicy);
		ManifestObject->TryGetStringField(TEXT("sandbox_mode"), SandboxMode);
		ManifestObject->TryGetBoolField(TEXT("explicit_tool_allow_list_enforced"), bExplicitAllowListEnforced);
		TestEqual(TEXT("Claude readback should surface interactive_or_provider_default"), ApprovalPolicy, FString(TEXT("interactive_or_provider_default")));
		TestEqual(TEXT("Claude readback should surface provider-managed sandbox truth"), SandboxMode, FString(TEXT("provider_managed_or_unspecified")));
		TestTrue(TEXT("Claude readback should surface allow-list enforcement"), bExplicitAllowListEnforced);
	}

	TestTrue(TEXT("manifest should surface Write in requested_allowed_tools"), JsonStringArrayContains(ManifestObject, TEXT("requested_allowed_tools"), TEXT("Write")));
	TestTrue(TEXT("manifest should surface Bash in requested_allowed_tools"), JsonStringArrayContains(ManifestObject, TEXT("requested_allowed_tools"), TEXT("Bash")));
	TestTrue(
		TEXT("manifest should surface narrow restart_survival MCP access in requested_allowed_tools"),
		JsonStringArrayContains(ManifestObject, TEXT("requested_allowed_tools"), TEXT("mcp__osvayderue__restart_survival")));

	const TSharedPtr<FJsonObject> WorkspaceLaneTaxonomy = FindObjectInArrayByStringField(ManifestObject, TEXT("runtime_lane_taxonomy"), TEXT("lane_id"), TEXT("workspace_write_project"));
	const TSharedPtr<FJsonObject> ReadOnlyLaneTaxonomy = FindObjectInArrayByStringField(ManifestObject, TEXT("runtime_lane_taxonomy"), TEXT("lane_id"), TEXT("read_only_analysis"));
	const TSharedPtr<FJsonObject> BoundedLaneTaxonomy = FindObjectInArrayByStringField(ManifestObject, TEXT("runtime_lane_taxonomy"), TEXT("lane_id"), TEXT("bounded_plugin_mutation"));
	const TSharedPtr<FJsonObject> ExpertLaneTaxonomy = FindObjectInArrayByStringField(ManifestObject, TEXT("runtime_lane_taxonomy"), TEXT("lane_id"), TEXT("expert_high_risk_provider_shell"));
	const TSharedPtr<FJsonObject> BoundedProfileMapping = FindObjectInArrayByStringField(ManifestObject, TEXT("profile_lane_mappings"), TEXT("execution_profile"), TEXT("bounded_plugin_mutation"));
	const TSharedPtr<FJsonObject> ReadOnlyProfileMapping = FindObjectInArrayByStringField(ManifestObject, TEXT("profile_lane_mappings"), TEXT("execution_profile"), TEXT("read_only_diagnostic"));
	const TSharedPtr<FJsonObject> DefaultProfileMapping = FindObjectInArrayByStringField(ManifestObject, TEXT("profile_lane_mappings"), TEXT("execution_profile"), TEXT("configured_default_runtime"));
	const TSharedPtr<FJsonObject> ExpertProfileMapping = FindObjectInArrayByStringField(ManifestObject, TEXT("profile_lane_mappings"), TEXT("execution_profile"), TEXT("explicit_expert_opt_in"));
	const TSharedPtr<FJsonObject> WorkspaceMatrixRow = FindObjectInArrayByStringField(ManifestObject, TEXT("provider_transport_matrix"), TEXT("lane_id"), TEXT("workspace_write_project"));
	const TSharedPtr<FJsonObject> BoundedMatrixRow = FindObjectInArrayByStringField(ManifestObject, TEXT("provider_transport_matrix"), TEXT("lane_id"), TEXT("bounded_plugin_mutation"));
	const TSharedPtr<FJsonObject> PolicyDenySchema = GetObjectFieldOrNull(ManifestObject, TEXT("policy_deny_contract"));
	const TSharedPtr<FJsonObject> SessionBoundary = GetObjectFieldOrNull(ManifestObject, TEXT("session_boundary"));
	TestTrue(TEXT("runtime lane taxonomy should include workspace_write_project"), WorkspaceLaneTaxonomy.IsValid());
	TestTrue(TEXT("runtime lane taxonomy should include read_only_analysis"), ReadOnlyLaneTaxonomy.IsValid());
	TestTrue(TEXT("runtime lane taxonomy should include bounded_plugin_mutation"), BoundedLaneTaxonomy.IsValid());
	TestTrue(TEXT("runtime lane taxonomy should include expert_high_risk_provider_shell"), ExpertLaneTaxonomy.IsValid());
	TestTrue(TEXT("profile lane mapping should include bounded_plugin_mutation"), BoundedProfileMapping.IsValid());
	TestTrue(TEXT("profile lane mapping should include read_only_diagnostic"), ReadOnlyProfileMapping.IsValid());
	TestTrue(TEXT("profile lane mapping should include configured_default_runtime"), DefaultProfileMapping.IsValid());
	TestTrue(TEXT("profile lane mapping should include explicit_expert_opt_in"), ExpertProfileMapping.IsValid());
	TestTrue(TEXT("provider transport matrix should include workspace_write_project row"), WorkspaceMatrixRow.IsValid());
	TestTrue(TEXT("provider transport matrix should include bounded_plugin_mutation row"), BoundedMatrixRow.IsValid());
	TestTrue(TEXT("policy_deny_contract schema should exist"), PolicyDenySchema.IsValid());
	TestTrue(TEXT("session_boundary schema should exist"), SessionBoundary.IsValid());
	if (BoundedProfileMapping.IsValid())
	{
		FString CanonicalLane;
		FString MappingSessionMode;
		TestTrue(TEXT("bounded profile mapping should expose canonical lane"), BoundedProfileMapping->TryGetStringField(TEXT("canonical_lane"), CanonicalLane));
		TestTrue(TEXT("bounded profile mapping should expose session mode"), BoundedProfileMapping->TryGetStringField(TEXT("session_persistence_mode"), MappingSessionMode));
		TestEqual(TEXT("bounded profile should map to bounded_plugin_mutation"), CanonicalLane, FString(TEXT("bounded_plugin_mutation")));
		TestEqual(TEXT("bounded profile should keep not_persisted session mode"), MappingSessionMode, FString(TEXT("not_persisted")));
	}
	if (ReadOnlyProfileMapping.IsValid())
	{
		FString CanonicalLane;
		FString MappingSessionMode;
		TestTrue(TEXT("read_only profile mapping should expose canonical lane"), ReadOnlyProfileMapping->TryGetStringField(TEXT("canonical_lane"), CanonicalLane));
		TestTrue(TEXT("read_only profile mapping should expose session mode"), ReadOnlyProfileMapping->TryGetStringField(TEXT("session_persistence_mode"), MappingSessionMode));
		TestEqual(TEXT("read_only profile should map to read_only_analysis"), CanonicalLane, FString(TEXT("read_only_analysis")));
		TestEqual(TEXT("read_only profile should keep not_persisted session mode"), MappingSessionMode, FString(TEXT("not_persisted")));
	}
	if (DefaultProfileMapping.IsValid())
	{
		FString CanonicalLane;
		FString MappingSessionMode;
		DefaultProfileMapping->TryGetStringField(TEXT("canonical_lane"), CanonicalLane);
		DefaultProfileMapping->TryGetStringField(TEXT("session_persistence_mode"), MappingSessionMode);
		TestEqual(TEXT("default profile should map to workspace_write_project lane"), CanonicalLane, FString(TEXT("workspace_write_project")));
		TestEqual(TEXT("default profile should keep not_persisted session mode"), MappingSessionMode, FString(TEXT("not_persisted")));
	}
	if (ExpertProfileMapping.IsValid())
	{
		FString CanonicalLane;
		FString MappingSessionMode;
		ExpertProfileMapping->TryGetStringField(TEXT("canonical_lane"), CanonicalLane);
		ExpertProfileMapping->TryGetStringField(TEXT("session_persistence_mode"), MappingSessionMode);
		TestEqual(TEXT("explicit expert profile should map to expert high risk lane"), CanonicalLane, FString(TEXT("expert_high_risk_provider_shell")));
		TestEqual(TEXT("explicit expert profile should keep normal provider session mode"), MappingSessionMode, FString(TEXT("normal_provider_session")));
	}
	if (BoundedMatrixRow.IsValid())
	{
		bool bDirectFileAllowed = true;
		bool bDirectShellAllowed = true;
		FString BehaviorState;
		FString BoundedSessionMode;
		BoundedMatrixRow->TryGetBoolField(TEXT("direct_file_tools_allowed"), bDirectFileAllowed);
		BoundedMatrixRow->TryGetBoolField(TEXT("direct_shell_tools_allowed"), bDirectShellAllowed);
		BoundedMatrixRow->TryGetStringField(TEXT("behavior_state"), BehaviorState);
		BoundedMatrixRow->TryGetStringField(TEXT("session_persistence_mode"), BoundedSessionMode);
		TestFalse(TEXT("bounded mutation matrix row should deny direct file tools"), bDirectFileAllowed);
		TestFalse(TEXT("bounded mutation matrix row should deny direct shell tools"), bDirectShellAllowed);
		TestEqual(TEXT("bounded mutation matrix row should expose first-class denied-provider behavior"), BehaviorState, FString(TEXT("first_class_runtime_profile_provider_dispatch_denied")));
		TestEqual(TEXT("bounded mutation matrix row should surface not_persisted session mode"), BoundedSessionMode, FString(TEXT("not_persisted")));
	}
	if (PolicyDenySchema.IsValid())
	{
		FString DenySchemaVersion;
		bool bPracticalProbeAvailableNow = false;
		PolicyDenySchema->TryGetStringField(TEXT("schema_version"), DenySchemaVersion);
		PolicyDenySchema->TryGetBoolField(TEXT("practical_probe_available_now"), bPracticalProbeAvailableNow);
		TestEqual(TEXT("policy deny schema version should stay stable"), DenySchemaVersion, FString(TEXT("policy_deny_contract_v1")));
		TestTrue(TEXT("policy deny schema should advertise a practical probe"), bPracticalProbeAvailableNow);
		TestTrue(TEXT("policy deny schema should advertise cpp reflected declaration deny rule"),
			JsonStringArrayContains(PolicyDenySchema, TEXT("supported_rule_ids"), TEXT("workspace_write_project.cpp_reflected_declaration_lane_denied")));
	}
	if (SessionBoundary.IsValid())
	{
		FString BoundarySchemaVersion;
		FString SilentCarryOverPolicy;
		SessionBoundary->TryGetStringField(TEXT("schema_version"), BoundarySchemaVersion);
		SessionBoundary->TryGetStringField(TEXT("silent_privilege_carry_over_policy"), SilentCarryOverPolicy);
		TestEqual(TEXT("session boundary schema version should stay stable"), BoundarySchemaVersion, FString(TEXT("session_boundary_truth_v1")));
		TestEqual(TEXT("silent privilege carry-over policy should be forbidden"), SilentCarryOverPolicy, FString(TEXT("forbidden")));
	}

	FString ReadOnlyPower;
	FString ReadOnlyPlumbingState;
	FString ReadOnlyTransport;
	FString ReadOnlySessionPersistenceMode;
	bool bReadOnlyCurrentPowerEnforcedNow = false;
	bool bReadOnlyPermissionBypassEnabled = true;
	bool bReadOnlyUnrealMcpBridgeEnabled = true;
	bool bReadOnlyNormalProviderSessionHistoryTouched = true;
	bool bReadOnlyProviderSessionFileUpdatedOnSuccess = true;
	TestTrue(TEXT("read_only current_effective_provider_power should exist"), ReadOnlyManifestObject->TryGetStringField(TEXT("current_effective_provider_power"), ReadOnlyPower));
	TestTrue(TEXT("read_only execution_control_plumbing_state should exist"), ReadOnlyManifestObject->TryGetStringField(TEXT("execution_control_plumbing_state"), ReadOnlyPlumbingState));
	TestTrue(TEXT("read_only execution_transport should exist"), ReadOnlyManifestObject->TryGetStringField(TEXT("execution_transport"), ReadOnlyTransport));
	TestTrue(TEXT("read_only session_persistence_mode should exist"), ReadOnlyManifestObject->TryGetStringField(TEXT("session_persistence_mode"), ReadOnlySessionPersistenceMode));
	TestTrue(TEXT("read_only current_effective_power_enforced_now should exist"), ReadOnlyManifestObject->TryGetBoolField(TEXT("current_effective_power_enforced_now"), bReadOnlyCurrentPowerEnforcedNow));
	TestTrue(TEXT("read_only permission_bypass_enabled should exist"), ReadOnlyManifestObject->TryGetBoolField(TEXT("permission_bypass_enabled"), bReadOnlyPermissionBypassEnabled));
	TestTrue(TEXT("read_only unreal_mcp_bridge_enabled should exist"), ReadOnlyManifestObject->TryGetBoolField(TEXT("unreal_mcp_bridge_enabled"), bReadOnlyUnrealMcpBridgeEnabled));
	TestTrue(TEXT("read_only normal_provider_session_history_touched should exist"), ReadOnlyManifestObject->TryGetBoolField(TEXT("normal_provider_session_history_touched"), bReadOnlyNormalProviderSessionHistoryTouched));
	TestTrue(TEXT("read_only provider_session_file_updated_on_success should exist"), ReadOnlyManifestObject->TryGetBoolField(TEXT("provider_session_file_updated_on_success"), bReadOnlyProviderSessionFileUpdatedOnSuccess));
	TestEqual(TEXT("read_only helper should surface read_only_analysis"), ReadOnlyPower, FString(TEXT("read_only_analysis")));
	TestEqual(TEXT("read_only helper should surface enforced plumbing"), ReadOnlyPlumbingState, FString(TEXT("enforced")));
	TestEqual(
		TEXT("read_only helper should surface transport for the configured backend"),
		ReadOnlyTransport,
		BackendName == TEXT("CodexCli") ? FString(TEXT("exec_per_message")) : FString(TEXT("cli_process")));
	TestEqual(TEXT("read_only helper should surface not_persisted session mode"), ReadOnlySessionPersistenceMode, FString(TEXT("not_persisted")));
	TestTrue(TEXT("read_only helper should surface current power as enforced now"), bReadOnlyCurrentPowerEnforcedNow);
	TestFalse(TEXT("read_only helper should disable permission bypass"), bReadOnlyPermissionBypassEnabled);
	TestFalse(TEXT("read_only helper should disable Unreal MCP bridge"), bReadOnlyUnrealMcpBridgeEnabled);
	TestFalse(TEXT("read_only helper should not touch normal provider history"), bReadOnlyNormalProviderSessionHistoryTouched);
	TestFalse(TEXT("read_only helper should not update provider session file on success"), bReadOnlyProviderSessionFileUpdatedOnSuccess);
	TestTrue(TEXT("read_only helper should not surface Write in requested_allowed_tools"), !JsonStringArrayContains(ReadOnlyManifestObject, TEXT("requested_allowed_tools"), TEXT("Write")));
	TestTrue(TEXT("read_only helper should not surface Bash in requested_allowed_tools"), !JsonStringArrayContains(ReadOnlyManifestObject, TEXT("requested_allowed_tools"), TEXT("Bash")));

	FString BoundedPower;
	FString BoundedPlumbingState;
	FString BoundedTransport;
	FString BoundedSessionPersistenceMode;
	bool bBoundedCurrentPowerEnforcedNow = false;
	bool bBoundedPermissionBypassEnabled = true;
	bool bBoundedNormalProviderSessionHistoryTouched = true;
	bool bBoundedProviderSessionFileUpdatedOnSuccess = true;
	TestTrue(TEXT("bounded current_effective_provider_power should exist"), BoundedManifestObject->TryGetStringField(TEXT("current_effective_provider_power"), BoundedPower));
	TestTrue(TEXT("bounded execution_control_plumbing_state should exist"), BoundedManifestObject->TryGetStringField(TEXT("execution_control_plumbing_state"), BoundedPlumbingState));
	TestTrue(TEXT("bounded execution_transport should exist"), BoundedManifestObject->TryGetStringField(TEXT("execution_transport"), BoundedTransport));
	TestTrue(TEXT("bounded session_persistence_mode should exist"), BoundedManifestObject->TryGetStringField(TEXT("session_persistence_mode"), BoundedSessionPersistenceMode));
	TestTrue(TEXT("bounded current_effective_power_enforced_now should exist"), BoundedManifestObject->TryGetBoolField(TEXT("current_effective_power_enforced_now"), bBoundedCurrentPowerEnforcedNow));
	TestTrue(TEXT("bounded permission_bypass_enabled should exist"), BoundedManifestObject->TryGetBoolField(TEXT("permission_bypass_enabled"), bBoundedPermissionBypassEnabled));
	TestTrue(TEXT("bounded normal_provider_session_history_touched should exist"), BoundedManifestObject->TryGetBoolField(TEXT("normal_provider_session_history_touched"), bBoundedNormalProviderSessionHistoryTouched));
	TestTrue(TEXT("bounded provider_session_file_updated_on_success should exist"), BoundedManifestObject->TryGetBoolField(TEXT("provider_session_file_updated_on_success"), bBoundedProviderSessionFileUpdatedOnSuccess));
	TestEqual(TEXT("bounded profile should surface bounded_mutation_capable"), BoundedPower, FString(TEXT("bounded_mutation_capable")));
	TestEqual(TEXT("bounded profile should surface enforced plumbing"), BoundedPlumbingState, FString(TEXT("enforced")));
	TestEqual(TEXT("bounded profile should surface governed transport"), BoundedTransport, FString(TEXT("governed_plugin_surface_only")));
	TestEqual(TEXT("bounded profile should surface not_persisted session mode"), BoundedSessionPersistenceMode, FString(TEXT("not_persisted")));
	TestTrue(TEXT("bounded profile should surface current power as enforced now"), bBoundedCurrentPowerEnforcedNow);
	TestFalse(TEXT("bounded profile should disable permission bypass"), bBoundedPermissionBypassEnabled);
	TestFalse(TEXT("bounded profile should not touch normal provider history"), bBoundedNormalProviderSessionHistoryTouched);
	TestFalse(TEXT("bounded profile should not update provider session file on success"), bBoundedProviderSessionFileUpdatedOnSuccess);
	TestTrue(TEXT("bounded profile should not surface Write in requested_allowed_tools"), !JsonStringArrayContains(BoundedManifestObject, TEXT("requested_allowed_tools"), TEXT("Write")));
	TestTrue(TEXT("bounded profile should not surface Bash in requested_allowed_tools"), !JsonStringArrayContains(BoundedManifestObject, TEXT("requested_allowed_tools"), TEXT("Bash")));

	FString ExpertPower;
	FString ExpertPlumbingState;
	FString ExpertTransport;
	FString ExpertSessionPersistenceMode;
	bool bExpertCurrentPowerEnforcedNow = false;
	bool bExpertPermissionBypassEnabled = false;
	bool bExpertNormalProviderSessionHistoryTouched = false;
	bool bExpertProviderSessionFileUpdatedOnSuccess = false;
	TestTrue(TEXT("expert current_effective_provider_power should exist"), ExplicitExpertManifestObject->TryGetStringField(TEXT("current_effective_provider_power"), ExpertPower));
	TestTrue(TEXT("expert execution_control_plumbing_state should exist"), ExplicitExpertManifestObject->TryGetStringField(TEXT("execution_control_plumbing_state"), ExpertPlumbingState));
	TestTrue(TEXT("expert execution_transport should exist"), ExplicitExpertManifestObject->TryGetStringField(TEXT("execution_transport"), ExpertTransport));
	TestTrue(TEXT("expert session_persistence_mode should exist"), ExplicitExpertManifestObject->TryGetStringField(TEXT("session_persistence_mode"), ExpertSessionPersistenceMode));
	TestTrue(TEXT("expert current_effective_power_enforced_now should exist"), ExplicitExpertManifestObject->TryGetBoolField(TEXT("current_effective_power_enforced_now"), bExpertCurrentPowerEnforcedNow));
	TestTrue(TEXT("expert permission_bypass_enabled should exist"), ExplicitExpertManifestObject->TryGetBoolField(TEXT("permission_bypass_enabled"), bExpertPermissionBypassEnabled));
	TestTrue(TEXT("expert normal_provider_session_history_touched should exist"), ExplicitExpertManifestObject->TryGetBoolField(TEXT("normal_provider_session_history_touched"), bExpertNormalProviderSessionHistoryTouched));
	TestTrue(TEXT("expert provider_session_file_updated_on_success should exist"), ExplicitExpertManifestObject->TryGetBoolField(TEXT("provider_session_file_updated_on_success"), bExpertProviderSessionFileUpdatedOnSuccess));
	TestEqual(TEXT("expert profile should surface high_risk_direct_file_shell"), ExpertPower, FString(TEXT("high_risk_direct_file_shell")));
	TestEqual(TEXT("expert profile should surface enforced plumbing"), ExpertPlumbingState, FString(TEXT("enforced")));
	TestEqual(
		TEXT("expert profile should surface transport for the configured backend"),
		ExpertTransport,
		LiveExplicitExpertManifest.ExecutionTransportLabel);
	TestEqual(TEXT("expert profile should surface normal_provider_session mode"), ExpertSessionPersistenceMode, FString(TEXT("normal_provider_session")));
	TestTrue(TEXT("expert profile should surface current power as enforced now"), bExpertCurrentPowerEnforcedNow);
	TestTrue(TEXT("expert profile should enable permission bypass"), bExpertPermissionBypassEnabled);
	TestTrue(TEXT("expert profile should touch normal provider history"), bExpertNormalProviderSessionHistoryTouched);
	TestTrue(TEXT("expert profile should update provider session file on success"), bExpertProviderSessionFileUpdatedOnSuccess);
	TestTrue(TEXT("expert profile should surface Write in requested_allowed_tools"), JsonStringArrayContains(ExplicitExpertManifestObject, TEXT("requested_allowed_tools"), TEXT("Write")));
	TestTrue(TEXT("expert profile should surface Bash in requested_allowed_tools"), JsonStringArrayContains(ExplicitExpertManifestObject, TEXT("requested_allowed_tools"), TEXT("Bash")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_AgentTraceRunStartedManifest,
	"OsvayderUE.ProviderExecutionControl.AgentTrace.RunStartedManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_AgentTraceRunStartedManifest::RunTest(const FString& Parameters)
{
	const FAgentBackendStatus Status = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
	FAgentRequestConfig Config = MakeSyntheticConfig(Status.Backend);
	Config.ExecutionTransportLabel = Status.Backend == EOsvayderUEProviderBackend::CodexCli
		? (FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifest().ExecutionTransportLabel)
		: TEXT("cli_process");

	const FString RunId = FOsvayderUEAgentTraceLog::Get().BeginRun(
		Status,
		Config,
		TEXT("provider execution control trace probe"),
		false,
		false);
	FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, TEXT("trace probe complete"), true);

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 8;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records = FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);
	TestEqual(TEXT("trace query should resolve the same run id"), ResolvedRunId, RunId);
	TestTrue(TEXT("trace query should load records for the run"), Records.Num() >= 2);
	if (Records.Num() < 2)
	{
		return false;
	}

	TSharedPtr<FJsonObject> RunStartedRecord;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		if (!Record.IsValid())
		{
			continue;
		}

		FString EventType;
		if (Record->TryGetStringField(TEXT("event_type"), EventType) && EventType == TEXT("run_started"))
		{
			RunStartedRecord = Record;
			break;
		}
	}

	TestTrue(TEXT("run_started trace record should exist"), RunStartedRecord.IsValid());
	if (!RunStartedRecord.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> PayloadObject = GetObjectFieldOrNull(RunStartedRecord, TEXT("payload"));
	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(PayloadObject, TEXT("provider_execution_control"));
	TestTrue(TEXT("run_started payload should include provider_execution_control"), ManifestObject.IsValid());
	if (!ManifestObject.IsValid())
	{
		return false;
	}

	FString CurrentPower;
	FString DesiredPower;
	FString ControlProfileId;
	FString SessionPersistenceMode;
	bool bNormalProviderSessionHistoryTouched = false;
	TestTrue(TEXT("trace manifest should expose current power"), ManifestObject->TryGetStringField(TEXT("current_effective_provider_power"), CurrentPower));
	TestTrue(TEXT("trace manifest should expose desired power"), ManifestObject->TryGetStringField(TEXT("desired_future_default_provider_power"), DesiredPower));
	TestTrue(TEXT("trace manifest should expose control profile"), ManifestObject->TryGetStringField(TEXT("control_profile_id"), ControlProfileId));
	TestTrue(TEXT("trace payload should expose session persistence mode"), PayloadObject->TryGetStringField(TEXT("session_persistence_mode"), SessionPersistenceMode));
	TestTrue(TEXT("trace payload should expose normal_provider_session_history_touched"), PayloadObject->TryGetBoolField(TEXT("normal_provider_session_history_touched"), bNormalProviderSessionHistoryTouched));
	TestEqual(TEXT("trace current power should now be workspace-write"), CurrentPower, FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("trace desired power should now track workspace-write default"), DesiredPower, FString(TEXT("workspace_write_project")));
	TestEqual(TEXT("trace control profile should stay stable"), ControlProfileId, FString(TEXT("workspace_write_default_runtime_v1")));
	TestEqual(TEXT("default trace should keep boundary-isolated session persistence"), SessionPersistenceMode, FString(TEXT("not_persisted")));
	TestFalse(TEXT("default trace should not touch normal provider history"), bNormalProviderSessionHistoryTouched);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_AgentTraceRunStartedReadOnlyManifest,
	"OsvayderUE.ProviderExecutionControl.AgentTrace.RunStartedReadOnlyManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_AgentTraceRunStartedExplicitExpertManifest,
	"OsvayderUE.ProviderExecutionControl.AgentTrace.RunStartedExplicitExpertManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_AgentTraceRunStartedExplicitExpertManifest::RunTest(const FString& Parameters)
{
	const FAgentBackendStatus Status = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
	const FAgentProviderExecutionControlManifest ExpertManifest =
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn);
	FAgentRequestConfig Config = MakeSyntheticConfig(Status.Backend, EAgentExecutionRunProfile::ExplicitExpertOptIn);
	Config.ExecutionTransportLabel = ExpertManifest.ExecutionTransportLabel;

	const FString RunId = FOsvayderUEAgentTraceLog::Get().BeginRun(
		Status,
		Config,
		TEXT("provider execution control explicit expert trace probe"),
		false,
		false);
	FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, TEXT("explicit expert trace probe complete"), true);

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 8;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records = FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);
	TestEqual(TEXT("explicit expert trace query should resolve the same run id"), ResolvedRunId, RunId);
	TestTrue(TEXT("explicit expert trace query should load records for the run"), Records.Num() >= 2);
	if (Records.Num() < 2)
	{
		return false;
	}

	TSharedPtr<FJsonObject> RunStartedRecord;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		if (!Record.IsValid())
		{
			continue;
		}

		FString EventType;
		if (Record->TryGetStringField(TEXT("event_type"), EventType) && EventType == TEXT("run_started"))
		{
			RunStartedRecord = Record;
			break;
		}
	}

	TestTrue(TEXT("explicit expert run_started trace record should exist"), RunStartedRecord.IsValid());
	if (!RunStartedRecord.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> PayloadObject = GetObjectFieldOrNull(RunStartedRecord, TEXT("payload"));
	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(PayloadObject, TEXT("provider_execution_control"));
	TestTrue(TEXT("explicit expert run_started payload should include provider_execution_control"), ManifestObject.IsValid());
	if (!ManifestObject.IsValid())
	{
		return false;
	}

	FString CurrentPower;
	FString ControlProfileId;
	FString ExecutionTransport;
	FString SessionPersistenceMode;
	bool bNormalProviderSessionHistoryTouched = false;
	TestTrue(TEXT("explicit expert trace manifest should expose current power"), ManifestObject->TryGetStringField(TEXT("current_effective_provider_power"), CurrentPower));
	TestTrue(TEXT("explicit expert trace manifest should expose control profile"), ManifestObject->TryGetStringField(TEXT("control_profile_id"), ControlProfileId));
	TestTrue(TEXT("explicit expert trace manifest should expose transport"), ManifestObject->TryGetStringField(TEXT("execution_transport"), ExecutionTransport));
	TestTrue(TEXT("explicit expert trace payload should expose session persistence mode"), PayloadObject->TryGetStringField(TEXT("session_persistence_mode"), SessionPersistenceMode));
	TestTrue(TEXT("explicit expert trace payload should expose normal_provider_session_history_touched"), PayloadObject->TryGetBoolField(TEXT("normal_provider_session_history_touched"), bNormalProviderSessionHistoryTouched));
	TestEqual(TEXT("explicit expert trace current power should be high risk"), CurrentPower, FString(TEXT("high_risk_direct_file_shell")));
	TestEqual(TEXT("explicit expert trace control profile should stay stable"), ControlProfileId, FString(TEXT("explicit_expert_opt_in_v1")));
	TestEqual(
		TEXT("explicit expert trace transport should match the selected backend"),
		ExecutionTransport,
		ExpertManifest.ExecutionTransportLabel);
	TestEqual(TEXT("explicit expert trace should surface normal_provider_session mode"), SessionPersistenceMode, FString(TEXT("normal_provider_session")));
	TestTrue(TEXT("explicit expert trace should touch normal provider history"), bNormalProviderSessionHistoryTouched);

	return true;
}

bool FProviderExecutionControl_AgentTraceRunStartedReadOnlyManifest::RunTest(const FString& Parameters)
{
	const FAgentBackendStatus Status = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
	FAgentRequestConfig Config = MakeSyntheticConfig(Status.Backend, EAgentExecutionRunProfile::ReadOnlyDiagnostic);
	Config.ExecutionTransportLabel = Status.Backend == EOsvayderUEProviderBackend::CodexCli
		? TEXT("exec_per_message")
		: TEXT("cli_process");

	const FString RunId = FOsvayderUEAgentTraceLog::Get().BeginRun(
		Status,
		Config,
		TEXT("provider execution control read-only trace probe"),
		false,
		false);
	FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, TEXT("read-only trace probe complete"), true);

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 8;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records = FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);
	TestEqual(TEXT("read_only trace query should resolve the same run id"), ResolvedRunId, RunId);
	TestTrue(TEXT("read_only trace query should load records for the run"), Records.Num() >= 2);
	if (Records.Num() < 2)
	{
		return false;
	}

	TSharedPtr<FJsonObject> RunStartedRecord;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		if (!Record.IsValid())
		{
			continue;
		}

		FString EventType;
		if (Record->TryGetStringField(TEXT("event_type"), EventType) && EventType == TEXT("run_started"))
		{
			RunStartedRecord = Record;
			break;
		}
	}

	TestTrue(TEXT("read_only run_started trace record should exist"), RunStartedRecord.IsValid());
	if (!RunStartedRecord.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> PayloadObject = GetObjectFieldOrNull(RunStartedRecord, TEXT("payload"));
	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(PayloadObject, TEXT("provider_execution_control"));
	TestTrue(TEXT("read_only run_started payload should include provider_execution_control"), ManifestObject.IsValid());
	if (!ManifestObject.IsValid())
	{
		return false;
	}

	FString CurrentPower;
	FString ControlProfileId;
	FString ExecutionTransport;
	FString SessionPersistenceMode;
	bool bNormalProviderSessionHistoryTouched = true;
	bool bProviderSessionFileUpdatedOnSuccess = true;
	TestTrue(TEXT("read_only trace manifest should expose current power"), ManifestObject->TryGetStringField(TEXT("current_effective_provider_power"), CurrentPower));
	TestTrue(TEXT("read_only trace manifest should expose control profile"), ManifestObject->TryGetStringField(TEXT("control_profile_id"), ControlProfileId));
	TestTrue(TEXT("read_only trace manifest should expose transport"), ManifestObject->TryGetStringField(TEXT("execution_transport"), ExecutionTransport));
	TestTrue(TEXT("read_only trace payload should expose session persistence mode"), PayloadObject->TryGetStringField(TEXT("session_persistence_mode"), SessionPersistenceMode));
	TestTrue(TEXT("read_only trace payload should expose normal_provider_session_history_touched"), PayloadObject->TryGetBoolField(TEXT("normal_provider_session_history_touched"), bNormalProviderSessionHistoryTouched));
	TestTrue(TEXT("read_only trace payload should expose provider_session_file_updated_on_success"), PayloadObject->TryGetBoolField(TEXT("provider_session_file_updated_on_success"), bProviderSessionFileUpdatedOnSuccess));
	TestEqual(TEXT("read_only trace current power should be read only"), CurrentPower, FString(TEXT("read_only_analysis")));
	TestEqual(TEXT("read_only trace control profile should stay stable"), ControlProfileId, FString(TEXT("read_only_diagnostic_v1")));
	TestEqual(
		TEXT("read_only trace transport should match the selected backend"),
		ExecutionTransport,
		Status.Backend == EOsvayderUEProviderBackend::CodexCli ? FString(TEXT("exec_per_message")) : FString(TEXT("cli_process")));
	TestEqual(TEXT("read_only trace should surface not_persisted session mode"), SessionPersistenceMode, FString(TEXT("not_persisted")));
	TestFalse(TEXT("read_only trace should not touch normal provider history"), bNormalProviderSessionHistoryTouched);
	TestFalse(TEXT("read_only trace should not update provider session file on success"), bProviderSessionFileUpdatedOnSuccess);
	TestTrue(TEXT("read_only trace should not surface Write in allowed_tools"), !JsonStringArrayContains(PayloadObject, TEXT("allowed_tools"), TEXT("Write")));
	TestTrue(TEXT("read_only trace should not surface Bash in allowed_tools"), !JsonStringArrayContains(PayloadObject, TEXT("allowed_tools"), TEXT("Bash")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProviderExecutionControl_AgentTracePolicyDeniedContract,
	"OsvayderUE.ProviderExecutionControl.AgentTrace.PolicyDeniedContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FProviderExecutionControl_AgentTracePolicyDeniedContract::RunTest(const FString& Parameters)
{
	const FAgentBackendStatus Status = MakeSyntheticStatus(EOsvayderUEProviderBackend::CodexCli);
	FAgentRequestConfig Config = MakeSyntheticConfig(EOsvayderUEProviderBackend::CodexCli, EAgentExecutionRunProfile::ReadOnlyDiagnostic);
	Config.AllowedTools.Add(TEXT("Write"));

	FAgentExecutionPolicyDenyContract Contract;
	TestTrue(TEXT("synthetic deny contract should materialize"), TryBuildAgentExecutionPolicyDenyContract(Status, Config, TEXT("direct_file_tools"), Contract));

	const FString RunId = FOsvayderUEAgentTraceLog::Get().BeginRun(
		Status,
		Config,
		TEXT("provider execution control deny trace probe"),
		false,
		false);
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("policy_denied"),
		Status.Backend,
		MakeAgentExecutionPolicyDenyContractJson(Contract),
		RunId);
	FOsvayderUEAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, Contract.DenyReason, false);

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 12;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records = FOsvayderUEAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);
	TestEqual(TEXT("deny trace query should resolve the same run id"), ResolvedRunId, RunId);
	TestTrue(TEXT("deny trace query should load records"), Records.Num() >= 3);
	if (Records.Num() < 3)
	{
		return false;
	}

	TSharedPtr<FJsonObject> PolicyDeniedRecord;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		if (!Record.IsValid())
		{
			continue;
		}

		FString EventType;
		if (Record->TryGetStringField(TEXT("event_type"), EventType) && EventType == TEXT("policy_denied"))
		{
			PolicyDeniedRecord = Record;
			break;
		}
	}

	TestTrue(TEXT("policy_denied trace record should exist"), PolicyDeniedRecord.IsValid());
	if (!PolicyDeniedRecord.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> PayloadObject = GetObjectFieldOrNull(PolicyDeniedRecord, TEXT("payload"));
	TestTrue(TEXT("policy_denied payload should exist"), PayloadObject.IsValid());
	if (!PayloadObject.IsValid())
	{
		return false;
	}

	FString RequestedLane;
	FString PolicyRuleId;
	bool bSilentFallbackPrevented = false;
	TestTrue(TEXT("policy_denied payload should expose requested_lane"), PayloadObject->TryGetStringField(TEXT("requested_lane"), RequestedLane));
	TestTrue(TEXT("policy_denied payload should expose policy_rule_id"), PayloadObject->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId));
	TestTrue(TEXT("policy_denied payload should expose silent_fallback_prevented"), PayloadObject->TryGetBoolField(TEXT("silent_fallback_prevented"), bSilentFallbackPrevented));
	TestEqual(TEXT("requested_lane should stay read_only_analysis"), RequestedLane, FString(TEXT("read_only_analysis")));
	TestEqual(TEXT("policy_rule_id should stay the direct file rule"), PolicyRuleId, FString(TEXT("read_only_analysis.direct_file_tools_denied")));
	TestTrue(TEXT("policy_denied payload should confirm silent fallback was prevented"), bSilentFallbackPrevented);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
