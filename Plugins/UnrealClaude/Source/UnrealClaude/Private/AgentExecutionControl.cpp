// Copyright Natali Caggiano. All Rights Reserved.

#include "AgentExecutionControl.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	bool StringArrayContainsIgnoreCase(const TArray<FString>& Values, const FString& Expected)
	{
		for (const FString& Value : Values)
		{
			if (Value.Equals(Expected, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	bool RequestsDirectFileTools(const TArray<FString>& AllowedTools)
	{
		return StringArrayContainsIgnoreCase(AllowedTools, TEXT("Write"))
			|| StringArrayContainsIgnoreCase(AllowedTools, TEXT("Edit"));
	}

	bool RequestsDirectShellTools(const TArray<FString>& AllowedTools)
	{
		return StringArrayContainsIgnoreCase(AllowedTools, TEXT("Bash"));
	}

	bool TryExtractScopedUnrealMcpToolName(const FString& AllowedTool, FString& OutRawToolName)
	{
		static const FString UnrealMcpPrefix = TEXT("mcp__unrealclaude__");
		if (!AllowedTool.StartsWith(UnrealMcpPrefix, ESearchCase::IgnoreCase))
		{
			return false;
		}

		OutRawToolName = AllowedTool.RightChop(UnrealMcpPrefix.Len()).TrimStartAndEnd();
		return !OutRawToolName.IsEmpty();
	}

	bool RequestsScopedCanonicalUnrealMcpTools(const TArray<FString>& AllowedTools)
	{
		for (const FString& AllowedTool : AllowedTools)
		{
			FString RawToolName;
			if (TryExtractScopedUnrealMcpToolName(AllowedTool, RawToolName) &&
				!RawToolName.Equals(TEXT("restart_survival"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> MakeJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}

		return JsonValues;
	}

	EAgentExecutionRuntimeLane RuntimeLaneFromPowerClass(const EAgentExecutionPowerClass PowerClass)
	{
		switch (PowerClass)
		{
		case EAgentExecutionPowerClass::WorkspaceWriteProject:
			return EAgentExecutionRuntimeLane::WorkspaceWriteProject;

		case EAgentExecutionPowerClass::BoundedMutationCapable:
			return EAgentExecutionRuntimeLane::BoundedPluginMutation;

		case EAgentExecutionPowerClass::HighRiskDirectFileShell:
			return EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell;

		case EAgentExecutionPowerClass::ReadOnlyAnalysis:
		default:
			return EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
		}
	}

	EAgentExecutionRuntimeLane RequestedRuntimeLaneFromProfile(const EAgentExecutionRunProfile Profile)
	{
		switch (Profile)
		{
		case EAgentExecutionRunProfile::BoundedPluginMutation:
			return EAgentExecutionRuntimeLane::BoundedPluginMutation;

		case EAgentExecutionRunProfile::ExplicitExpertOptIn:
			return EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell;

		case EAgentExecutionRunProfile::ConfiguredDefaultRuntime:
			return EAgentExecutionRuntimeLane::WorkspaceWriteProject;

		case EAgentExecutionRunProfile::ReadOnlyDiagnostic:
			return EAgentExecutionRuntimeLane::ReadOnlyAnalysis;

		default:
			return EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
		}
	}

	FString RuntimeLaneDisplayName(const EAgentExecutionRuntimeLane Lane)
	{
		switch (Lane)
		{
		case EAgentExecutionRuntimeLane::WorkspaceWriteProject:
			return TEXT("Workspace-Write Project");

		case EAgentExecutionRuntimeLane::BoundedPluginMutation:
			return TEXT("Bounded Plugin Mutation");

		case EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell:
			return TEXT("Expert High-Risk Provider Shell");

		case EAgentExecutionRuntimeLane::ReadOnlyAnalysis:
		default:
			return TEXT("Read-Only Analysis");
		}
	}

	FString RuntimeLaneMeaning(const EAgentExecutionRuntimeLane Lane)
	{
		switch (Lane)
		{
		case EAgentExecutionRuntimeLane::WorkspaceWriteProject:
			return TEXT("Ordinary project/workspace write execution with direct provider file/shell tools inside the configured working directory, but without mutating Unreal MCP or expert full-danger bypass.");

		case EAgentExecutionRuntimeLane::BoundedPluginMutation:
			return TEXT("Bounded, plugin-governed mutation only. Direct provider file/shell power stays denied outside explicit governed families.");

		case EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell:
			return TEXT("Explicit high-risk execution mode with direct provider file/shell power available and never silently entered.");

		case EAgentExecutionRuntimeLane::ReadOnlyAnalysis:
		default:
			return TEXT("Analysis-only execution with no direct file mutation, no direct shell mutation, and no mutating Unreal MCP path.");
		}
	}

	TArray<FString> BoundedMutationFamilies()
	{
		return {
			TEXT("mutation_group_revert"),
			TEXT("cpp_reflected_contracts")
		};
	}

	FString TransportForReadOnlyLane(const EUnrealClaudeProviderBackend Backend)
	{
		return Backend == EUnrealClaudeProviderBackend::CodexCli
			? TEXT("exec_per_message")
			: TEXT("cli_process");
	}

	FString TransportForWorkspaceWriteLane(const EUnrealClaudeProviderBackend Backend)
	{
		return Backend == EUnrealClaudeProviderBackend::CodexCli
			? TEXT("exec_per_message")
			: TEXT("cli_process");
	}

	FString ApprovalPolicyForReadOnlyLane(const EUnrealClaudeProviderBackend Backend)
	{
		return Backend == EUnrealClaudeProviderBackend::CodexCli
			? TEXT("provider_default_or_interactive")
			: TEXT("interactive_or_provider_default");
	}

	FString ApprovalPolicyForWorkspaceWriteLane(const EUnrealClaudeProviderBackend Backend)
	{
		return Backend == EUnrealClaudeProviderBackend::CodexCli
			? TEXT("ask_for_approval_never")
			: TEXT("interactive_or_provider_default");
	}

	FString SandboxPolicyForReadOnlyLane(const EUnrealClaudeProviderBackend)
	{
		return TEXT("provider_managed_or_unspecified");
	}

	FString SandboxPolicyForWorkspaceWriteLane(const EUnrealClaudeProviderBackend Backend)
	{
		return Backend == EUnrealClaudeProviderBackend::CodexCli
			? TEXT("workspace_write")
			: TEXT("provider_managed_or_unspecified");
	}

	FString BackendLabelForMatrix(const EUnrealClaudeProviderBackend Backend)
	{
		return UnrealClaudeProviderBackendToString(Backend);
	}

	FString LegacyPowerAliasForLane(const EAgentExecutionRuntimeLane Lane)
	{
		switch (Lane)
		{
		case EAgentExecutionRuntimeLane::WorkspaceWriteProject:
			return TEXT("workspace_write_project");

		case EAgentExecutionRuntimeLane::BoundedPluginMutation:
			return TEXT("bounded_mutation_capable");

		case EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell:
			return TEXT("high_risk_direct_file_shell");

		case EAgentExecutionRuntimeLane::ReadOnlyAnalysis:
		default:
			return TEXT("read_only_analysis");
		}
	}

	TSharedPtr<FJsonObject> MakePowerLaneJson(const FAgentExecutionPowerLane& Lane)
	{
		TSharedPtr<FJsonObject> LaneObject = MakeShared<FJsonObject>();
		LaneObject->SetStringField(TEXT("power_class"), AgentExecutionPowerClassToString(Lane.PowerClass));
		LaneObject->SetBoolField(TEXT("available_now"), Lane.bAvailableNow);
		LaneObject->SetBoolField(TEXT("currently_effective"), Lane.bCurrentlyEffective);
		LaneObject->SetBoolField(TEXT("enforced_now"), Lane.bEnforcedNow);
		LaneObject->SetStringField(TEXT("selection_state"), Lane.SelectionState);
		LaneObject->SetStringField(TEXT("truth_boundary"), Lane.TruthBoundary);
		LaneObject->SetArrayField(TEXT("basis"), MakeJsonStringArray(Lane.Basis));
		return LaneObject;
	}

	TSharedPtr<FJsonObject> MakeLaneTaxonomyJson(const FAgentExecutionLaneTaxonomyEntry& Entry)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("lane_id"), AgentExecutionRuntimeLaneToString(Entry.Lane));
		Object->SetStringField(TEXT("display_name"), Entry.DisplayName);
		Object->SetStringField(TEXT("meaning"), Entry.Meaning);
		Object->SetStringField(TEXT("current_availability"), Entry.CurrentAvailability);
		Object->SetStringField(TEXT("selection_state"), Entry.SelectionState);
		Object->SetStringField(TEXT("legacy_power_class_alias"), Entry.LegacyPowerClassAlias);
		Object->SetBoolField(TEXT("direct_file_tools_allowed"), Entry.bDirectFileToolsAllowed);
		Object->SetBoolField(TEXT("direct_shell_tools_allowed"), Entry.bDirectShellToolsAllowed);
		Object->SetBoolField(TEXT("mutating_unreal_mcp_allowed"), Entry.bMutatingUnrealMcpAllowed);
		Object->SetBoolField(TEXT("persistent_session_carry_over_allowed_by_default"), Entry.bPersistentSessionCarryOverAllowedByDefault);
		Object->SetArrayField(TEXT("typical_uses"), MakeJsonStringArray(Entry.TypicalUses));
		Object->SetArrayField(TEXT("allowed_mutation_families"), MakeJsonStringArray(Entry.AllowedMutationFamilies));
		Object->SetStringField(TEXT("truth_boundary"), Entry.TruthBoundary);
		Object->SetArrayField(TEXT("basis"), MakeJsonStringArray(Entry.Basis));
		return Object;
	}

	TSharedPtr<FJsonObject> MakeProfileLaneMappingJson(const FAgentExecutionProfileLaneMapping& Mapping)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("execution_profile"), Mapping.ExecutionProfile);
		Object->SetStringField(TEXT("control_profile_id"), Mapping.ControlProfileId);
		Object->SetStringField(TEXT("canonical_lane"), AgentExecutionRuntimeLaneToString(Mapping.CanonicalLane));
		Object->SetStringField(TEXT("legacy_power_class_alias"), Mapping.LegacyPowerClassAlias);
		Object->SetStringField(TEXT("mapping_state"), Mapping.MappingState);
		Object->SetStringField(TEXT("session_persistence_mode"), Mapping.SessionPersistenceMode);
		Object->SetStringField(TEXT("truth_boundary"), Mapping.TruthBoundary);
		Object->SetArrayField(TEXT("basis"), MakeJsonStringArray(Mapping.Basis));
		return Object;
	}

	TSharedPtr<FJsonObject> MakeProviderTransportMatrixRowJson(const FAgentExecutionProviderTransportMatrixRow& Row)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("lane_id"), AgentExecutionRuntimeLaneToString(Row.Lane));
		Object->SetStringField(TEXT("backend"), Row.BackendLabel);
		Object->SetStringField(TEXT("transport"), Row.Transport);
		Object->SetStringField(TEXT("approval_policy"), Row.ApprovalPolicy);
		Object->SetStringField(TEXT("sandbox_policy"), Row.SandboxPolicy);
		Object->SetBoolField(TEXT("unreal_mcp_bridge_enabled"), Row.bUnrealMcpBridgeEnabled);
		Object->SetBoolField(TEXT("direct_file_tools_allowed"), Row.bDirectFileToolsAllowed);
		Object->SetBoolField(TEXT("direct_shell_tools_allowed"), Row.bDirectShellToolsAllowed);
		Object->SetStringField(TEXT("session_persistence_mode"), Row.SessionPersistenceMode);
		Object->SetArrayField(TEXT("allowed_mutation_families"), MakeJsonStringArray(Row.AllowedMutationFamilies));
		Object->SetStringField(TEXT("checkpoint_requirement"), Row.CheckpointRequirement);
		Object->SetStringField(TEXT("ui_badge"), Row.UiBadge);
		Object->SetArrayField(TEXT("trace_receipt_expectations"), MakeJsonStringArray(Row.TraceReceiptExpectations));
		Object->SetStringField(TEXT("behavior_state"), Row.BehaviorState);
		Object->SetBoolField(TEXT("available_now"), Row.bAvailableNow);
		Object->SetBoolField(TEXT("current_profile_row"), Row.bCurrentProfileRow);
		Object->SetBoolField(TEXT("current_effective_lane"), Row.bCurrentEffectiveLane);
		Object->SetBoolField(TEXT("enforced_now"), Row.bEnforcedNow);
		Object->SetStringField(TEXT("truth_boundary"), Row.TruthBoundary);
		Object->SetArrayField(TEXT("basis"), MakeJsonStringArray(Row.Basis));
		return Object;
	}

	TSharedPtr<FJsonObject> MakePolicyDenySchemaJson(const FAgentExecutionPolicyDenySchema& Schema)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("schema_version"), Schema.SchemaVersion);
		Object->SetStringField(TEXT("result_type"), Schema.ResultType);
		Object->SetArrayField(TEXT("required_fields"), MakeJsonStringArray(Schema.RequiredFields));
		Object->SetArrayField(TEXT("visible_surfaces"), MakeJsonStringArray(Schema.VisibleSurfaces));
		Object->SetArrayField(TEXT("supported_rule_ids"), MakeJsonStringArray(Schema.SupportedRuleIds));
		Object->SetBoolField(TEXT("practical_probe_available_now"), Schema.bPracticalProbeAvailableNow);
		Object->SetStringField(TEXT("truth_boundary"), Schema.TruthBoundary);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeSessionBoundaryRuleJson(const FAgentExecutionSessionBoundaryRule& Rule)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("from_lane"), AgentExecutionRuntimeLaneToString(Rule.FromLane));
		Object->SetStringField(TEXT("to_lane"), AgentExecutionRuntimeLaneToString(Rule.ToLane));
		Object->SetStringField(TEXT("boundary_mode"), Rule.BoundaryMode);
		Object->SetStringField(TEXT("history_carry_over"), Rule.HistoryCarryOver);
		Object->SetStringField(TEXT("provider_session_reuse"), Rule.ProviderSessionReuse);
		Object->SetStringField(TEXT("boundary_artifact"), Rule.BoundaryArtifact);
		Object->SetStringField(TEXT("truth_boundary"), Rule.TruthBoundary);
		Object->SetArrayField(TEXT("basis"), MakeJsonStringArray(Rule.Basis));
		return Object;
	}

	TSharedPtr<FJsonObject> MakeSessionBoundaryManifestJson(const FAgentExecutionSessionBoundaryManifest& Manifest)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("schema_version"), Manifest.SchemaVersion);
		Object->SetStringField(TEXT("boundary_strategy_id"), Manifest.BoundaryStrategyId);
		Object->SetStringField(TEXT("silent_privilege_carry_over_policy"), Manifest.SilentPrivilegeCarryOverPolicy);
		Object->SetArrayField(TEXT("boundary_artifacts"), MakeJsonStringArray(Manifest.BoundaryArtifacts));
		Object->SetStringField(TEXT("truth_boundary"), Manifest.TruthBoundary);

		TArray<TSharedPtr<FJsonValue>> Rules;
		Rules.Reserve(Manifest.Rules.Num());
		for (const FAgentExecutionSessionBoundaryRule& Rule : Manifest.Rules)
		{
			Rules.Add(MakeShared<FJsonValueObject>(MakeSessionBoundaryRuleJson(Rule)));
		}
		Object->SetArrayField(TEXT("rules"), Rules);
		Object->SetNumberField(TEXT("rule_count"), Manifest.Rules.Num());
		return Object;
	}

	bool TryReadJsonStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (Value.IsValid())
			{
				OutValues.Add(Value->AsString());
			}
		}
		return true;
	}

	FAgentExecutionPolicyDenySchema BuildPolicyDenySchema()
	{
		FAgentExecutionPolicyDenySchema Schema;
		Schema.RequiredFields = {
			TEXT("requested_lane"),
			TEXT("effective_lane"),
			TEXT("governing_family"),
			TEXT("deny_reason"),
			TEXT("policy_rule_id"),
			TEXT("safer_alternative_exists"),
			TEXT("expert_opt_in_required")
		};
		Schema.VisibleSurfaces = {
			TEXT("tool_result_or_command_result"),
			TEXT("agent_trace"),
			TEXT("backend_run_receipt")
		};
		Schema.SupportedRuleIds = {
			TEXT("bounded_plugin_mutation.provider_prompt_dispatch_denied"),
			TEXT("bounded_plugin_mutation.broad_authoring_mutation_surface_denied"),
			TEXT("bounded_plugin_mutation.representative_high_risk_execution_surface_denied"),
			TEXT("bounded_plugin_mutation.external_ui_control_surface_denied"),
			TEXT("read_only_analysis.direct_file_tools_denied"),
			TEXT("read_only_analysis.direct_shell_tools_denied"),
			TEXT("read_only_analysis.broad_authoring_mutation_surface_denied"),
			TEXT("read_only_analysis.cpp_reflected_declaration_lane_denied"),
			TEXT("read_only_analysis.representative_high_risk_execution_surface_denied"),
			TEXT("read_only_analysis.external_ui_control_surface_denied"),
			TEXT("read_only_analysis.mutating_unreal_mcp_denied"),
			TEXT("read_only_analysis.session_persistence_boundary_denied"),
			TEXT("workspace_write_project.broad_authoring_mutation_surface_denied"),
			TEXT("workspace_write_project.cpp_reflected_declaration_lane_denied"),
			TEXT("workspace_write_project.representative_high_risk_execution_surface_denied"),
			TEXT("workspace_write_project.external_ui_control_surface_denied"),
			TEXT("workspace_write_project.mutating_unreal_mcp_denied"),
			TEXT("workspace_write_project.session_persistence_boundary_denied")
		};
		Schema.bPracticalProbeAvailableNow = true;
		Schema.TruthBoundary =
			TEXT("This deny contract schema is accepted now and has practical probe paths for configured workspace-write default denial on guarded MCP/session boundaries, explicit read-only helper denial, and bounded-runtime denial. ")
			TEXT("It does not yet mean the full runtime has broad hard enforcement across every prompt-dispatch path.");
		return Schema;
	}

	FAgentExecutionSessionBoundaryManifest BuildSessionBoundaryManifest()
	{
		FAgentExecutionSessionBoundaryManifest Manifest;
		Manifest.BoundaryArtifacts = {
			TEXT("provider_execution_control.profile_lane_mappings"),
			TEXT("provider_execution_control.session_boundary"),
			TEXT("run_started.execution_control_profile_id"),
			TEXT("run_started.session_persistence_mode"),
			TEXT("backend_run_receipt.provider_execution_control")
		};
		Manifest.TruthBoundary =
			TEXT("Session boundaries are currently truthfully expressed through execution-profile selection, session-persistence mode, and trace/receipt artifacts. ")
			TEXT("This slice does not claim a broad new session engine beyond those explicit boundaries.");

		auto AddRule = [&Manifest](
			const EAgentExecutionRuntimeLane FromLane,
			const EAgentExecutionRuntimeLane ToLane,
			const FString& BoundaryMode,
			const FString& HistoryCarryOver,
			const FString& ProviderSessionReuse,
			const FString& BoundaryArtifact,
			const FString& TruthBoundary,
			const TArray<FString>& Basis)
		{
			FAgentExecutionSessionBoundaryRule Rule;
			Rule.FromLane = FromLane;
			Rule.ToLane = ToLane;
			Rule.BoundaryMode = BoundaryMode;
			Rule.HistoryCarryOver = HistoryCarryOver;
			Rule.ProviderSessionReuse = ProviderSessionReuse;
			Rule.BoundaryArtifact = BoundaryArtifact;
			Rule.TruthBoundary = TruthBoundary;
			Rule.Basis = Basis;
			Manifest.Rules.Add(MoveTemp(Rule));
		};

		AddRule(
			EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell,
			EAgentExecutionRuntimeLane::WorkspaceWriteProject,
			TEXT("explicit_profile_switch_to_workspace_write_default"),
			TEXT("workspace-write default lane does not append to normal provider session history"),
			TEXT("forbidden"),
			TEXT("run_started.execution_control_profile_id + session_persistence_mode + provider_execution_control"),
			TEXT("Switching from expert/high-risk to the ordinary workspace-write default creates an explicit boundary because the default lane stays non-persisted, keeps Unreal MCP mutation disabled, and is separately labeled from the expert lane."),
			{
				TEXT("from_profile=explicit_expert_opt_in"),
				TEXT("to_profile=workspace_write_default_runtime_v1"),
				TEXT("workspace_write_session_persistence_mode=not_persisted")
			});

		AddRule(
			EAgentExecutionRuntimeLane::WorkspaceWriteProject,
			EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell,
			TEXT("explicit_expert_opt_in_required_for_high_risk_return"),
			TEXT("workspace-write default runs are not carried into normal provider history"),
			TEXT("allowed_only_after_explicit_profile_selection"),
			TEXT("provider_execution_control.profile_lane_mappings + backend_run_receipt.provider_execution_control"),
			TEXT("Returning from the workspace-write default lane to expert/high-risk requires explicit expert opt-in; default workspace-write history remains outside the normal provider session file."),
			{
				TEXT("workspace_write_session_file_updates=false"),
				TEXT("expert_session_persistence_mode=normal_provider_session")
			});

		AddRule(
			EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell,
			EAgentExecutionRuntimeLane::ReadOnlyAnalysis,
			TEXT("explicit_profile_switch_to_read_only_helper"),
			TEXT("read-only helper lane does not append to normal provider session history"),
			TEXT("forbidden"),
			TEXT("run_started.execution_control_profile_id + session_persistence_mode + provider_execution_control"),
			TEXT("Switching from expert/high-risk to the explicit read-only helper creates an explicit boundary because the helper lane stays separately labeled and non-persisted."),
			{
				TEXT("from_profile=explicit_expert_opt_in"),
				TEXT("to_profile=read_only_diagnostic_v1"),
				TEXT("read_only_session_persistence_mode=not_persisted")
			});

		AddRule(
			EAgentExecutionRuntimeLane::ReadOnlyAnalysis,
			EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell,
			TEXT("explicit_expert_opt_in_required_for_helper_return"),
			TEXT("read-only helper runs are not carried into normal provider history"),
			TEXT("allowed_only_after_explicit_profile_selection"),
			TEXT("provider_execution_control.profile_lane_mappings + backend_run_receipt.provider_execution_control"),
			TEXT("Returning from the read-only helper lane to expert/high-risk requires explicit expert opt-in; helper history remains outside the normal provider session file."),
			{
				TEXT("read_only_session_file_updates=false"),
				TEXT("expert_session_persistence_mode=normal_provider_session")
			});

		AddRule(
			EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell,
			EAgentExecutionRuntimeLane::BoundedPluginMutation,
			TEXT("explicit_profile_switch_to_bounded_non_persisted_lane"),
			TEXT("bounded lane runs do not append to normal provider session history"),
			TEXT("forbidden"),
			TEXT("run_started.execution_control_profile_id + session_persistence_mode + policy_denied + provider_execution_control"),
			TEXT("Switching from expert/high-risk to bounded plugin mutation creates an explicit boundary because the bounded runtime is separately labeled, non-persisted, and generic provider prompt dispatch is denied instead of falling through to expert execution."),
			{
				TEXT("from_profile=explicit_expert_opt_in"),
				TEXT("to_profile=bounded_plugin_mutation"),
				TEXT("bounded_session_persistence_mode=not_persisted"),
				TEXT("bounded_provider_prompt_dispatch_denied=true")
			});

		AddRule(
			EAgentExecutionRuntimeLane::BoundedPluginMutation,
			EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell,
			TEXT("explicit_opt_in_back_to_expert_runtime"),
			TEXT("bounded lane history stays outside the normal provider session file"),
			TEXT("allowed_only_after_explicit_profile_selection"),
			TEXT("provider_execution_control.profile_lane_mappings + backend_run_receipt.provider_execution_control"),
			TEXT("Returning from bounded plugin mutation to expert/high-risk requires explicit profile selection; bounded runs do not silently upgrade into the normal provider session history."),
			{
				TEXT("bounded_session_persistence_mode=not_persisted"),
				TEXT("expert_session_persistence_mode=normal_provider_session"),
				TEXT("silent_privilege_carry_over_policy=forbidden")
			});

		return Manifest;
	}
}

FAgentProviderExecutionControlManifest BuildAgentProviderExecutionControlManifest(
	const FAgentBackendStatus& Status,
	const FAgentRequestConfig& Config)
{
	FAgentProviderExecutionControlManifest Manifest;
	Manifest.ControlProfileId = Config.ExecutionControlProfileId;
	Manifest.Backend = Status.Backend;
	Manifest.BackendDisplayName = Status.DisplayName;
	Manifest.ExecutionTransportLabel = Config.ExecutionTransportLabel;
	Manifest.ExecutionProfile = AgentExecutionRunProfileToString(Config.ExecutionProfile);
	Manifest.ExecutionControlPlumbingState = Config.ExecutionControlPlumbingState;
	Manifest.SessionPersistenceMode = Config.SessionPersistenceMode;
	Manifest.DesiredFutureDefaultProviderPowerClass = Config.DesiredFutureDefaultProviderPowerClass;
	Manifest.bPermissionBypassEnabled = Config.bSkipPermissions;
	Manifest.bUnrealMcpBridgeEnabled = Config.bEnableUnrealMcpBridge;
	Manifest.RequestedAllowedTools = Config.AllowedTools;
	Manifest.bRequestedToolAllowListPresent = Config.AllowedTools.Num() > 0;
	Manifest.bRequestedDirectFileTools = RequestsDirectFileTools(Config.AllowedTools);
	Manifest.bRequestedDirectShellTools = RequestsDirectShellTools(Config.AllowedTools);
	Manifest.bMutatingMcpToolsTreatedAsAvailable = Config.bEnableUnrealMcpBridge;
	const bool bScopedCanonicalUnrealMcpRequested = RequestsScopedCanonicalUnrealMcpTools(Config.AllowedTools);

	if (Config.ExecutionProfile == EAgentExecutionRunProfile::BoundedPluginMutation)
	{
		Manifest.ManifestSource = TEXT("configured_backend_bounded_plugin_mutation_runtime");
		Manifest.CurrentEffectivePowerSource = TEXT("explicit_bounded_plugin_mutation_profile");
	}
	else if (Config.ExecutionProfile == EAgentExecutionRunProfile::ExplicitExpertOptIn)
	{
		Manifest.ManifestSource = TEXT("configured_backend_explicit_expert_opt_in_runtime");
		Manifest.CurrentEffectivePowerSource = TEXT("explicit_expert_opt_in_profile");
	}
	else if (Config.ExecutionProfile == EAgentExecutionRunProfile::ReadOnlyDiagnostic)
	{
		Manifest.ManifestSource = TEXT("configured_backend_read_only_diagnostic_runtime");
		Manifest.CurrentEffectivePowerSource = TEXT("explicit_read_only_helper_profile");
	}
	else
	{
		Manifest.ManifestSource = TEXT("configured_backend_workspace_write_default_runtime");
		Manifest.CurrentEffectivePowerSource = TEXT("explicit_workspace_write_default_profile");
	}

	const bool bWorkspaceWriteDefaultRequested =
		Config.ExecutionProfile == EAgentExecutionRunProfile::ConfiguredDefaultRuntime &&
		(Manifest.bRequestedDirectFileTools || Manifest.bRequestedDirectShellTools);

	switch (Config.SessionPersistenceMode)
	{
	case EAgentSessionPersistenceMode::HelperIsolatedStore:
		Manifest.bTouchesNormalProviderSessionHistory = false;
		Manifest.bWritesProviderSessionFileOnSuccess = false;
		Manifest.SessionPersistenceTruthBoundary =
			TEXT("successful runs on this profile do not append to the normal provider session/history path; ")
			TEXT("they are expected to persist only in a helper-isolated store.");
		Manifest.SessionPersistenceBasis = {
			TEXT("session_persistence_mode=helper_isolated_store"),
			TEXT("normal_provider_session_history_touched=false"),
			TEXT("provider_session_file_updated_on_success=false")
		};
		break;

	case EAgentSessionPersistenceMode::NotPersisted:
		Manifest.bTouchesNormalProviderSessionHistory = false;
		Manifest.bWritesProviderSessionFileOnSuccess = false;
		Manifest.SessionPersistenceTruthBoundary =
			TEXT("successful runs on this profile do not append to the normal provider session/history path and do not update the provider session file; ")
			TEXT("the result remains observable through the immediate response path, backend-run receipts, and agent trace only.");
		Manifest.SessionPersistenceBasis = {
			TEXT("session_persistence_mode=not_persisted"),
			TEXT("normal_provider_session_history_touched=false"),
			TEXT("provider_session_file_updated_on_success=false")
		};
		break;

	case EAgentSessionPersistenceMode::NormalProviderSession:
	default:
		Manifest.bTouchesNormalProviderSessionHistory = true;
		Manifest.bWritesProviderSessionFileOnSuccess = true;
		Manifest.SessionPersistenceTruthBoundary =
			TEXT("successful runs on this profile append to the normal provider session/history path and update the provider session file.");
		Manifest.SessionPersistenceBasis = {
			TEXT("session_persistence_mode=normal_provider_session"),
			TEXT("normal_provider_session_history_touched=true"),
			TEXT("provider_session_file_updated_on_success=true")
		};
		break;
	}

	switch (Status.Backend)
	{
	case EUnrealClaudeProviderBackend::CodexCli:
		Manifest.ApprovalPolicy = Config.bSkipPermissions
			? TEXT("ask_for_approval_never")
			: (bWorkspaceWriteDefaultRequested ? TEXT("ask_for_approval_never") : TEXT("provider_default_or_interactive"));
		Manifest.SandboxMode = Config.bSkipPermissions
			? TEXT("danger-full-access")
			: (bWorkspaceWriteDefaultRequested ? TEXT("workspace-write") : TEXT("provider_managed_or_unspecified"));
		Manifest.bExplicitToolAllowListEnforced = false;
		Manifest.EffectiveToolBudgetMode = TEXT("provider_default_without_explicit_allow_list_enforcement");
		Manifest.bDirectFilePowerTreatedAsAvailable = Manifest.bRequestedDirectFileTools;
		Manifest.bDirectShellPowerTreatedAsAvailable = Manifest.bRequestedDirectShellTools;
		break;

	case EUnrealClaudeProviderBackend::ClaudeCli:
	default:
		Manifest.ApprovalPolicy = Config.bSkipPermissions
			? TEXT("dangerously_skip_permissions")
			: TEXT("interactive_or_provider_default");
		Manifest.SandboxMode = TEXT("provider_managed_or_unspecified");
		Manifest.bExplicitToolAllowListEnforced = Config.bToolAllowListEnforced && Status.Capabilities.bSupportsToolAllowList;
		Manifest.EffectiveToolBudgetMode = Manifest.bExplicitToolAllowListEnforced
			? TEXT("explicit_allow_list_enforced")
			: TEXT("provider_default_without_explicit_allow_list_enforcement");
		Manifest.bDirectFilePowerTreatedAsAvailable = Manifest.bExplicitToolAllowListEnforced
			? Manifest.bRequestedDirectFileTools
			: (Config.bSkipPermissions || Manifest.bRequestedDirectFileTools);
		Manifest.bDirectShellPowerTreatedAsAvailable = Manifest.bExplicitToolAllowListEnforced
			? Manifest.bRequestedDirectShellTools
			: (Config.bSkipPermissions || Manifest.bRequestedDirectShellTools);
		break;
	}

	Manifest.bBoundedMutationLaneAvailable = true;
	const bool bBoundedRuntimeProfileSelected = Config.ExecutionProfile == EAgentExecutionRunProfile::BoundedPluginMutation;
	const bool bExplicitExpertProfileSelected = Config.ExecutionProfile == EAgentExecutionRunProfile::ExplicitExpertOptIn;

	const bool bHighRiskPowerTreatedAsAvailable =
		Config.bSkipPermissions ||
		(Manifest.bMutatingMcpToolsTreatedAsAvailable && !bBoundedRuntimeProfileSelected);

	if (bHighRiskPowerTreatedAsAvailable)
	{
		Manifest.CurrentEffectiveProviderPowerClass = EAgentExecutionPowerClass::HighRiskDirectFileShell;
	}
	else if (bBoundedRuntimeProfileSelected || (Manifest.bBoundedMutationLaneAvailable && Manifest.bUnrealMcpBridgeEnabled))
	{
		Manifest.CurrentEffectiveProviderPowerClass = EAgentExecutionPowerClass::BoundedMutationCapable;
	}
	else if (bWorkspaceWriteDefaultRequested)
	{
		Manifest.CurrentEffectiveProviderPowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
	}
	else
	{
		Manifest.CurrentEffectiveProviderPowerClass = EAgentExecutionPowerClass::ReadOnlyAnalysis;
	}

	Manifest.CurrentEffectiveRuntimeLane = RuntimeLaneFromPowerClass(Manifest.CurrentEffectiveProviderPowerClass);
	Manifest.DesiredFutureDefaultRuntimeLane = RuntimeLaneFromPowerClass(Manifest.DesiredFutureDefaultProviderPowerClass);
	Manifest.bCurrentEffectivePowerEnforcedNow = Config.bExecutionProfileEnforcedNow;
	Manifest.bDesiredFutureDefaultEnforcedNow =
		Manifest.CurrentEffectiveProviderPowerClass == Manifest.DesiredFutureDefaultProviderPowerClass;
	Manifest.bFutureTighteningDescribedOnly = !Manifest.bDesiredFutureDefaultEnforcedNow;

	if (Config.ExecutionProfile == EAgentExecutionRunProfile::BoundedPluginMutation)
	{
		Manifest.TruthBoundary =
			TEXT("current_effective_provider_power on this bounded profile is derived from an enforced governed-runtime split: ")
			TEXT("permission bypass is off, direct provider file/shell power is denied, the normal provider session store is not reused, and generic provider prompt dispatch is blocked instead of silently falling through to expert execution. ")
			TEXT("Bounded mutation remains limited to explicit plugin-owned families with receipts/checkpoints; it does not widen arbitrary provider shell/file power.");
	}
	else if (Config.ExecutionProfile == EAgentExecutionRunProfile::ExplicitExpertOptIn)
	{
		Manifest.TruthBoundary =
			TEXT("current_effective_provider_power on this explicit expert profile is derived from a deliberate high-risk opt-in: ")
			TEXT("permission bypass stays on, direct provider file/shell power remains available, normal provider session persistence is allowed, and this lane is no longer the silent default. ")
			TEXT("The truth claim here is explicit opt-in, not broad hardening of the expert lane itself.");
	}
	else if (Config.ExecutionProfile == EAgentExecutionRunProfile::ReadOnlyDiagnostic)
	{
		Manifest.TruthBoundary =
			TEXT("current_effective_provider_power on this helper profile is derived from an enforced lower-risk split: ")
			TEXT("permission bypass is off, Unreal MCP bridge is disabled, direct write/shell tools are not requested, and Codex is forced off the persistent danger-full-access path. ")
			TEXT("On providers without explicit allow-list enforcement this remains a plugin-side lower-risk split, not a claim of universal provider sandboxing. ")
			TEXT("This lower-risk state is real for this helper profile and remains distinct from the ordinary workspace-write default runtime.");
	}
	else
	{
		if (Status.Backend == EUnrealClaudeProviderBackend::CodexCli &&
			Config.ExecutionTransportLabel == TEXT("persistent_app_server"))
		{
			Manifest.TruthBoundary =
				TEXT("current_effective_provider_power on the configured default runtime is now derived from a real workspace-write project lane: ")
				TEXT("permission bypass is off, Unreal MCP bridge is disabled, direct provider write/shell tools are requested, the working directory remains project-scoped, the Codex runtime uses persistent app-server transport with a workspace-write turn policy and project-scoped writable roots, and the normal provider session store is not reused. ")
				TEXT("Narrow canon-scoped Unreal MCP tools may also be requested per run without reopening the full mutating bridge. ")
				TEXT("This is the ordinary writable project/workspace lane, not helper read-only and not the explicit expert full-danger lane.");
		}
		else
		{
			Manifest.TruthBoundary =
				TEXT("current_effective_provider_power on the configured default runtime is now derived from a real workspace-write project lane: ")
				TEXT("permission bypass is off, Unreal MCP bridge is disabled, direct provider write/shell tools are requested, the working directory remains project-scoped, and the normal provider session store is not reused. ")
				TEXT("Narrow canon-scoped Unreal MCP tools may also be requested per run without reopening the full mutating bridge. ")
				TEXT("This is the ordinary writable project/workspace lane, not helper read-only and not the explicit expert full-danger lane.");
		}
	}

	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("execution_profile=%s"), AgentExecutionRunProfileToString(Config.ExecutionProfile)));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("permission_bypass_enabled=%s"), Manifest.bPermissionBypassEnabled ? TEXT("true") : TEXT("false")));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("approval_policy=%s"), *Manifest.ApprovalPolicy));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("sandbox_mode=%s"), *Manifest.SandboxMode));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("effective_tool_budget_mode=%s"), *Manifest.EffectiveToolBudgetMode));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("execution_transport=%s"), *Manifest.ExecutionTransportLabel));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("persistent_transport_forced_disabled=%s"), Config.bForceDisablePersistentConversationTransport ? TEXT("true") : TEXT("false")));
	Manifest.CurrentEffectivePowerBasis.Add(TEXT("working_directory_scope=project_dir"));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("unreal_mcp_bridge_enabled=%s"), Manifest.bUnrealMcpBridgeEnabled ? TEXT("true") : TEXT("false")));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("mutating_mcp_tools_treated_as_available=%s"), Manifest.bMutatingMcpToolsTreatedAsAvailable ? TEXT("true") : TEXT("false")));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("scoped_canonical_unreal_mcp_requested=%s"), bScopedCanonicalUnrealMcpRequested ? TEXT("true") : TEXT("false")));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("direct_file_power_treated_as_available=%s"), Manifest.bDirectFilePowerTreatedAsAvailable ? TEXT("true") : TEXT("false")));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("direct_shell_power_treated_as_available=%s"), Manifest.bDirectShellPowerTreatedAsAvailable ? TEXT("true") : TEXT("false")));
	if (Status.Backend == EUnrealClaudeProviderBackend::CodexCli &&
		Config.ExecutionTransportLabel == TEXT("persistent_app_server") &&
		Config.ExecutionProfile == EAgentExecutionRunProfile::ConfiguredDefaultRuntime)
	{
		Manifest.CurrentEffectivePowerBasis.Add(TEXT("persistent_thread_state_boundary=not_persisted"));
#if PLATFORM_WINDOWS
		Manifest.CurrentEffectivePowerBasis.Add(TEXT("windows_workspace_write_sandbox_backend=unelevated"));
#endif
	}
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("provider_prompt_dispatch_denied=%s"), bBoundedRuntimeProfileSelected ? TEXT("true") : TEXT("false")));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("expert_opt_in_profile_selected=%s"), bExplicitExpertProfileSelected ? TEXT("true") : TEXT("false")));
	Manifest.CurrentEffectivePowerBasis.Add(FString::Printf(TEXT("current_effective_runtime_lane=%s"), AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)));

	{
		FAgentExecutionPowerLane Lane;
		Lane.PowerClass = EAgentExecutionPowerClass::ReadOnlyAnalysis;
		Lane.bAvailableNow = true;
		Lane.bCurrentlyEffective = Manifest.CurrentEffectiveProviderPowerClass == Lane.PowerClass;
		Lane.bEnforcedNow = Lane.bCurrentlyEffective && Manifest.bCurrentEffectivePowerEnforcedNow;
		Lane.SelectionState = Lane.bCurrentlyEffective
			? TEXT("current_selected_profile")
			: TEXT("available_as_helper_profile");
		Lane.TruthBoundary = Lane.bCurrentlyEffective
			? TEXT("read-only analysis is enforced on the explicit helper profile with no direct file/shell power and no normal provider session carry-over.")
			: TEXT("lower-risk analysis remains available as the explicit helper lane, separate from the ordinary workspace-write default.");
		Lane.Basis = Lane.bCurrentlyEffective
			? Manifest.CurrentEffectivePowerBasis
			: TArray<FString>{ TEXT("read_like_operations_present"), TEXT("read_only_helper_profile_available") };
		Manifest.PowerLanes.Add(Lane);
	}

	{
		FAgentExecutionPowerLane Lane;
		Lane.PowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
		Lane.bAvailableNow = true;
		Lane.bCurrentlyEffective = Manifest.CurrentEffectiveProviderPowerClass == Lane.PowerClass;
		Lane.bEnforcedNow = Lane.bCurrentlyEffective && Manifest.bCurrentEffectivePowerEnforcedNow;
		Lane.SelectionState = Lane.bCurrentlyEffective
			? TEXT("current_default")
			: TEXT("available_as_default_runtime");
		Lane.TruthBoundary = Lane.bCurrentlyEffective
			? TEXT("workspace-write project power is enforced on the configured default runtime: direct provider file/shell tools are available for ordinary project/workspace work without expert permission bypass or mutating Unreal MCP, and any persistent Codex transport stays boundary-isolated from the normal provider session store.")
			: TEXT("workspace-write project power exists as the ordinary configured default lane without silently widening into expert full-danger power or reusing the expert persistence boundary.");
		Lane.Basis = Lane.bCurrentlyEffective
			? Manifest.CurrentEffectivePowerBasis
			: TArray<FString>{ TEXT("configured_default_runtime_available"), TEXT("working_directory_scope=project_dir"), TEXT("direct_file_and_shell_tools_requested=true"), TEXT("boundary_isolated_persistent_transport_allowed=true"), TEXT("explicit_expert_opt_in_required_for_full_danger=true") };
		Manifest.PowerLanes.Add(Lane);
	}

	{
		FAgentExecutionPowerLane Lane;
		Lane.PowerClass = EAgentExecutionPowerClass::BoundedMutationCapable;
		Lane.bAvailableNow = Manifest.bBoundedMutationLaneAvailable;
		Lane.bCurrentlyEffective = Manifest.CurrentEffectiveProviderPowerClass == Lane.PowerClass;
		Lane.bEnforcedNow = Lane.bCurrentlyEffective && Manifest.bCurrentEffectivePowerEnforcedNow;
		Lane.SelectionState = Lane.bCurrentlyEffective
			? (Config.ExecutionProfile == EAgentExecutionRunProfile::ConfiguredDefaultRuntime ? TEXT("current_default") : TEXT("current_selected_profile"))
			: (Lane.bAvailableNow ? TEXT("available_not_default") : TEXT("disabled_on_current_profile"));
		Lane.TruthBoundary = Lane.bCurrentlyEffective
			? TEXT("bounded mutation-capable power is currently enforced on the explicit bounded runtime profile, where generic provider prompt dispatch is denied and only governed plugin-owned mutation families remain in scope.")
			: (Lane.bAvailableNow
				? TEXT("bounded mutation-capable power exists when governed plugin mutation lanes are available, but it is not the active default while broader high-risk power remains available.")
				: TEXT("bounded mutation-capable power is intentionally unavailable on this helper profile because governed mutation lanes are disabled."));
		Lane.Basis = Lane.bAvailableNow
			? (Lane.bCurrentlyEffective
				? Manifest.CurrentEffectivePowerBasis
				: TArray<FString>{ TEXT("plugin_bounded_mutation_lanes_present"), TEXT("mutation_group"), TEXT("cpp_reflection_bounded_mutation") })
			: TArray<FString>{ TEXT("unreal_mcp_bridge_enabled=false") };
		Manifest.PowerLanes.Add(Lane);
	}

	{
		FAgentExecutionPowerLane Lane;
		Lane.PowerClass = EAgentExecutionPowerClass::HighRiskDirectFileShell;
		Lane.bAvailableNow = true;
		Lane.bCurrentlyEffective = Manifest.CurrentEffectiveProviderPowerClass == Lane.PowerClass;
		Lane.bEnforcedNow = Lane.bCurrentlyEffective && Manifest.bCurrentEffectivePowerEnforcedNow;
		Lane.SelectionState = Lane.bCurrentlyEffective
			? TEXT("current_selected_profile")
			: TEXT("available_as_explicit_opt_in");
		Lane.TruthBoundary = Lane.bCurrentlyEffective
			? TEXT("high-risk direct file/shell power is current only when the explicit expert opt-in profile is selected.")
			: TEXT("high-risk direct file/shell power remains available as an explicit expert opt-in lane but is not the current configured default.");
		Lane.Basis = Lane.bCurrentlyEffective
			? Manifest.CurrentEffectivePowerBasis
			: TArray<FString>{ TEXT("explicit_expert_opt_in_profile_available=true"), TEXT("never_silent_default=true") };
		Manifest.PowerLanes.Add(Lane);
	}

	{
		FAgentExecutionLaneTaxonomyEntry Entry;
		Entry.Lane = EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
		Entry.DisplayName = RuntimeLaneDisplayName(Entry.Lane);
		Entry.Meaning = RuntimeLaneMeaning(Entry.Lane);
		Entry.CurrentAvailability = Manifest.CurrentEffectiveRuntimeLane == Entry.Lane
			? TEXT("current_effective_on_selected_profile")
			: TEXT("available_as_explicit_helper_profile");
		Entry.SelectionState = RequestedRuntimeLaneFromProfile(Config.ExecutionProfile) == Entry.Lane ? TEXT("selected_profile_maps_here") : TEXT("available_not_selected");
		Entry.LegacyPowerClassAlias = LegacyPowerAliasForLane(Entry.Lane);
		Entry.bPersistentSessionCarryOverAllowedByDefault = false;
		Entry.TypicalUses = { TEXT("diagnostics"), TEXT("read_only_project_inspection"), TEXT("safe_helper_workflows") };
		Entry.TruthBoundary = TEXT("This lane remains the explicit read-only helper lane. It does not imply bounded mutation, workspace-write default power, or expert file/shell power.");
		Entry.Basis = { TEXT("direct_file_tools_allowed=false"), TEXT("direct_shell_tools_allowed=false"), TEXT("mutating_unreal_mcp_allowed=false"), TEXT("default_session_persistence=not_persisted") };
		Manifest.RuntimeLaneTaxonomy.Add(MoveTemp(Entry));
	}

	{
		FAgentExecutionLaneTaxonomyEntry Entry;
		Entry.Lane = EAgentExecutionRuntimeLane::WorkspaceWriteProject;
		Entry.DisplayName = RuntimeLaneDisplayName(Entry.Lane);
		Entry.Meaning = RuntimeLaneMeaning(Entry.Lane);
		Entry.CurrentAvailability = Manifest.CurrentEffectiveRuntimeLane == Entry.Lane
			? TEXT("current_effective_on_selected_profile")
			: TEXT("available_as_configured_default_runtime");
		Entry.SelectionState = RequestedRuntimeLaneFromProfile(Config.ExecutionProfile) == Entry.Lane
			? TEXT("selected_profile_maps_here")
			: TEXT("available_not_selected");
		Entry.LegacyPowerClassAlias = LegacyPowerAliasForLane(Entry.Lane);
		Entry.bDirectFileToolsAllowed = true;
		Entry.bDirectShellToolsAllowed = true;
		Entry.bPersistentSessionCarryOverAllowedByDefault = false;
		Entry.TypicalUses = { TEXT("ordinary_project_file_editing"), TEXT("workspace_scoped_refactors"), TEXT("default_backend_work_sessions") };
		Entry.TruthBoundary = TEXT("This lane is now the ordinary configured default runtime: it allows direct provider file/shell work for project/workspace tasks while keeping mutating Unreal MCP and expert full-danger opt-in separate, and any persistent Codex transport stays boundary-isolated from the normal provider session store.");
		Entry.Basis = { TEXT("direct_file_tools_allowed=true"), TEXT("direct_shell_tools_allowed=true"), TEXT("mutating_unreal_mcp_allowed=false"), TEXT("default_session_persistence=not_persisted"), TEXT("working_directory_scope=project_dir"), TEXT("boundary_isolated_persistent_transport_allowed=true") };
		Manifest.RuntimeLaneTaxonomy.Add(MoveTemp(Entry));
	}

	{
		FAgentExecutionLaneTaxonomyEntry Entry;
		Entry.Lane = EAgentExecutionRuntimeLane::BoundedPluginMutation;
		Entry.DisplayName = RuntimeLaneDisplayName(Entry.Lane);
		Entry.Meaning = RuntimeLaneMeaning(Entry.Lane);
		Entry.CurrentAvailability = Manifest.CurrentEffectiveRuntimeLane == Entry.Lane
			? TEXT("current_effective_on_selected_profile")
			: TEXT("available_as_explicit_bounded_runtime_profile");
		Entry.SelectionState = RequestedRuntimeLaneFromProfile(Config.ExecutionProfile) == Entry.Lane
			? TEXT("selected_profile_maps_here")
			: TEXT("available_not_selected");
		Entry.LegacyPowerClassAlias = LegacyPowerAliasForLane(Entry.Lane);
		Entry.AllowedMutationFamilies = BoundedMutationFamilies();
		Entry.TypicalUses = { TEXT("bounded_mcp_mutation_lanes"), TEXT("bounded_reflected_cpp_lanes"), TEXT("checkpointed_grouped_mutations") };
		Entry.TruthBoundary = TEXT("This lane is now a first-class bounded runtime profile. Generic provider prompt dispatch remains denied here, and mutation stays limited to explicit plugin-owned governed families with checkpoint/receipt obligations.");
		Entry.Basis = { TEXT("direct_file_tools_allowed=false"), TEXT("direct_shell_tools_allowed=false"), TEXT("provider_prompt_dispatch_denied=true"), TEXT("allowed_mutation_families_are_explicit_allow_list"), TEXT("checkpoint_or_receipt_requirements_apply") };
		Manifest.RuntimeLaneTaxonomy.Add(MoveTemp(Entry));
	}

	{
		FAgentExecutionLaneTaxonomyEntry Entry;
		Entry.Lane = EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell;
		Entry.DisplayName = RuntimeLaneDisplayName(Entry.Lane);
		Entry.Meaning = RuntimeLaneMeaning(Entry.Lane);
		Entry.CurrentAvailability = Manifest.CurrentEffectiveRuntimeLane == Entry.Lane ? TEXT("current_effective_on_selected_profile") : TEXT("available_as_explicit_expert_opt_in");
		Entry.SelectionState = RequestedRuntimeLaneFromProfile(Config.ExecutionProfile) == Entry.Lane ? TEXT("selected_profile_maps_here") : TEXT("available_not_selected");
		Entry.LegacyPowerClassAlias = LegacyPowerAliasForLane(Entry.Lane);
		Entry.bDirectFileToolsAllowed = true;
		Entry.bDirectShellToolsAllowed = true;
		Entry.bMutatingUnrealMcpAllowed = true;
		Entry.bPersistentSessionCarryOverAllowedByDefault = true;
		Entry.TypicalUses = { TEXT("expert_operator_workflows"), TEXT("exceptional_recovery"), TEXT("advanced_engineering_paths") };
		Entry.TruthBoundary = TEXT("This lane remains real and high-risk, but it is now an explicit expert opt-in lane rather than the silent configured default runtime.");
		Entry.Basis = { TEXT("direct_file_tools_allowed=true"), TEXT("direct_shell_tools_allowed=true"), TEXT("mutating_unreal_mcp_allowed=true"), TEXT("never_silently_entered_is_required_design_rule") };
		Manifest.RuntimeLaneTaxonomy.Add(MoveTemp(Entry));
	}

	{
		FAgentExecutionProfileLaneMapping Mapping;
		Mapping.ExecutionProfile = TEXT("configured_default_runtime");
		Mapping.ControlProfileId = TEXT("workspace_write_default_runtime_v1");
		Mapping.CanonicalLane = EAgentExecutionRuntimeLane::WorkspaceWriteProject;
		Mapping.LegacyPowerClassAlias = TEXT("workspace_write_project");
		Mapping.MappingState = TEXT("accepted_workspace_write_default_runtime_mapping");
		Mapping.SessionPersistenceMode = TEXT("not_persisted");
		Mapping.TruthBoundary = TEXT("The configured default runtime now maps to the workspace-write project lane, keeps the normal provider session boundary, may use boundary-isolated persistent Codex transport, and does not silently widen into the explicit expert lane.");
		Mapping.Basis = { TEXT("permission_bypass_enabled=false"), TEXT("unreal_mcp_bridge_enabled=false"), TEXT("direct_provider_file_shell_allowed=true"), TEXT("session_persistence_mode=not_persisted"), TEXT("working_directory_scope=project_dir"), TEXT("boundary_isolated_persistent_transport_allowed=true") };
		Manifest.ProfileLaneMappings.Add(MoveTemp(Mapping));
	}

	{
		FAgentExecutionProfileLaneMapping Mapping;
		Mapping.ExecutionProfile = TEXT("bounded_plugin_mutation");
		Mapping.ControlProfileId = TEXT("bounded_plugin_mutation_v1");
		Mapping.CanonicalLane = EAgentExecutionRuntimeLane::BoundedPluginMutation;
		Mapping.LegacyPowerClassAlias = TEXT("bounded_mutation_capable");
		Mapping.MappingState = TEXT("accepted_bounded_runtime_profile_mapping");
		Mapping.SessionPersistenceMode = TEXT("not_persisted");
		Mapping.TruthBoundary = TEXT("The explicit bounded runtime profile maps to the bounded plugin mutation lane, keeps a session boundary against the normal provider session store, and denies generic provider prompt dispatch without silent fallback.");
		Mapping.Basis = { TEXT("permission_bypass_enabled=false"), TEXT("direct_provider_file_shell_denied=true"), TEXT("session_persistence_mode=not_persisted"), TEXT("provider_prompt_dispatch_denied=true") };
		Manifest.ProfileLaneMappings.Add(MoveTemp(Mapping));
	}

	{
		FAgentExecutionProfileLaneMapping Mapping;
		Mapping.ExecutionProfile = TEXT("read_only_diagnostic");
		Mapping.ControlProfileId = TEXT("read_only_diagnostic_v1");
		Mapping.CanonicalLane = EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
		Mapping.LegacyPowerClassAlias = TEXT("read_only_analysis");
		Mapping.MappingState = TEXT("accepted_helper_profile_mapping");
		Mapping.SessionPersistenceMode = TEXT("not_persisted");
		Mapping.TruthBoundary = TEXT("The explicit helper profile maps to the read-only analysis lane and keeps a session boundary against the normal provider session store.");
		Mapping.Basis = { TEXT("permission_bypass_enabled=false"), TEXT("unreal_mcp_bridge_enabled=false"), TEXT("session_persistence_mode=not_persisted") };
		Manifest.ProfileLaneMappings.Add(MoveTemp(Mapping));
	}

	{
		FAgentExecutionProfileLaneMapping Mapping;
		Mapping.ExecutionProfile = TEXT("explicit_expert_opt_in");
		Mapping.ControlProfileId = TEXT("explicit_expert_opt_in_v1");
		Mapping.CanonicalLane = EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell;
		Mapping.LegacyPowerClassAlias = TEXT("high_risk_direct_file_shell");
		Mapping.MappingState = TEXT("accepted_explicit_expert_opt_in_runtime_mapping");
		Mapping.SessionPersistenceMode = TEXT("normal_provider_session");
		Mapping.TruthBoundary = TEXT("The explicit expert opt-in profile keeps the old high-risk provider file/shell lane alive as a deliberate opt-in instead of the configured default.");
		Mapping.Basis = { TEXT("permission_bypass_enabled=true"), TEXT("unreal_mcp_bridge_enabled=true"), TEXT("direct_file_and_shell_tools_available=true"), TEXT("explicit_opt_in_required=true") };
		Manifest.ProfileLaneMappings.Add(MoveTemp(Mapping));
	}

	{
		FAgentExecutionProviderTransportMatrixRow Row;
		Row.Lane = EAgentExecutionRuntimeLane::WorkspaceWriteProject;
		Row.BackendLabel = BackendLabelForMatrix(Status.Backend);
		Row.Transport = TransportForWorkspaceWriteLane(Status.Backend);
		Row.ApprovalPolicy = ApprovalPolicyForWorkspaceWriteLane(Status.Backend);
		Row.SandboxPolicy = SandboxPolicyForWorkspaceWriteLane(Status.Backend);
		Row.bDirectFileToolsAllowed = true;
		Row.bDirectShellToolsAllowed = true;
		Row.SessionPersistenceMode = TEXT("not_persisted");
		Row.AllowedMutationFamilies = { TEXT("workspace_scoped_provider_file_editing") };
		Row.CheckpointRequirement = TEXT("not_required");
		Row.UiBadge = TEXT("workspace_write_default");
		Row.TraceReceiptExpectations = {
			TEXT("run_started shows workspace_write_project control profile"),
			TEXT("run_started shows session_persistence_mode=not_persisted"),
			TEXT("backend_run_receipt carries provider_execution_control"),
			TEXT("feature_slice runs surface feature_workflow in agent_trace and backend_run_receipt"),
			TEXT("explicit_expert_opt_in is not implied")
		};
		Row.BehaviorState = TEXT("configured_default_runtime_workspace_write");
		Row.bAvailableNow = true;
		Row.bCurrentProfileRow = RequestedRuntimeLaneFromProfile(Config.ExecutionProfile) == Row.Lane;
		Row.bCurrentEffectiveLane = Manifest.CurrentEffectiveRuntimeLane == Row.Lane;
		Row.bEnforcedNow = Row.bCurrentProfileRow;
		Row.TruthBoundary = TEXT("This row describes the ordinary configured default runtime: direct provider file/shell work is available for project/workspace tasks without expert permission bypass, mutating Unreal MCP remains disabled, and any persistent Codex transport stays boundary-isolated from the normal provider session store.");
		Row.Basis = {
			TEXT("direct_file_tools_allowed=true"),
			TEXT("direct_shell_tools_allowed=true"),
			TEXT("unreal_mcp_bridge_enabled=false"),
			TEXT("session_persistence_mode=not_persisted"),
			TEXT("working_directory_scope=project_dir"),
			TEXT("boundary_isolated_persistent_transport_allowed=true")
		};
		Manifest.ProviderTransportMatrix.Add(MoveTemp(Row));
	}

	{
		FAgentExecutionProviderTransportMatrixRow Row;
		Row.Lane = EAgentExecutionRuntimeLane::ReadOnlyAnalysis;
		Row.BackendLabel = BackendLabelForMatrix(Status.Backend);
		Row.Transport = TransportForReadOnlyLane(Status.Backend);
		Row.ApprovalPolicy = ApprovalPolicyForReadOnlyLane(Status.Backend);
		Row.SandboxPolicy = SandboxPolicyForReadOnlyLane(Status.Backend);
		Row.SessionPersistenceMode = TEXT("not_persisted");
		Row.CheckpointRequirement = TEXT("not_required");
		Row.UiBadge = TEXT("safe_read_only");
		Row.TraceReceiptExpectations = { TEXT("run_started shows read_only_analysis control profile"), TEXT("run_started shows session_persistence_mode=not_persisted"), TEXT("backend_run_receipt carries provider_execution_control") };
		Row.BehaviorState = TEXT("explicit_helper_profile_real");
		Row.bAvailableNow = true;
		Row.bCurrentProfileRow = RequestedRuntimeLaneFromProfile(Config.ExecutionProfile) == Row.Lane;
		Row.bCurrentEffectiveLane = Manifest.CurrentEffectiveRuntimeLane == Row.Lane;
		Row.bEnforcedNow = Row.bCurrentProfileRow;
		Row.TruthBoundary = TEXT("This row describes the explicit helper profile. It remains read-only and distinct from the workspace-write default runtime.");
		Row.Basis = { TEXT("direct_file_tools_allowed=false"), TEXT("direct_shell_tools_allowed=false"), TEXT("unreal_mcp_bridge_enabled=false"), TEXT("session_persistence_mode=not_persisted") };
		Manifest.ProviderTransportMatrix.Add(MoveTemp(Row));
	}

	{
		FAgentExecutionProviderTransportMatrixRow Row;
		Row.Lane = EAgentExecutionRuntimeLane::BoundedPluginMutation;
		Row.BackendLabel = BackendLabelForMatrix(Status.Backend);
		Row.Transport = TEXT("governed_plugin_surface_only");
		Row.ApprovalPolicy = TEXT("bounded_by_plugin_policy");
		Row.SandboxPolicy = TEXT("bounded_plugin_scope");
		Row.SessionPersistenceMode = TEXT("not_persisted");
		Row.AllowedMutationFamilies = BoundedMutationFamilies();
		Row.CheckpointRequirement = TEXT("required");
		Row.UiBadge = TEXT("bounded_mutation");
		Row.TraceReceiptExpectations = { TEXT("receipts_or_checkpoints_required"), TEXT("policy_denied visible for generic provider prompt dispatch"), TEXT("governed_family_visible_in_trace_or_receipt") };
		Row.BehaviorState = TEXT("first_class_runtime_profile_provider_dispatch_denied");
		Row.bAvailableNow = true;
		Row.bCurrentProfileRow = RequestedRuntimeLaneFromProfile(Config.ExecutionProfile) == Row.Lane;
		Row.bCurrentEffectiveLane = Manifest.CurrentEffectiveRuntimeLane == Row.Lane;
		Row.bEnforcedNow = Row.bCurrentProfileRow;
		Row.TruthBoundary = TEXT("This row defines the accepted bounded runtime profile now: generic provider prompt dispatch is denied here, and bounded mutation remains limited to governed plugin-owned families with explicit receipts/checkpoints.");
		Row.Basis = { TEXT("direct_file_tools_allowed=false"), TEXT("direct_shell_tools_allowed=false"), TEXT("provider_prompt_dispatch_denied=true"), TEXT("allowed_mutation_families_are_allow_list_only"), TEXT("checkpoint_requirement=required") };
		Manifest.ProviderTransportMatrix.Add(MoveTemp(Row));
	}

	{
		FAgentExecutionProviderTransportMatrixRow Row;
		Row.Lane = EAgentExecutionRuntimeLane::ExpertHighRiskProviderShell;
		Row.BackendLabel = BackendLabelForMatrix(Status.Backend);
		Row.Transport = Config.ExecutionProfile == EAgentExecutionRunProfile::ExplicitExpertOptIn
			? Manifest.ExecutionTransportLabel
			: (Status.Backend == EUnrealClaudeProviderBackend::CodexCli ? TEXT("persistent_app_server_or_exec_per_message") : TEXT("cli_process"));
		Row.ApprovalPolicy = Status.Backend == EUnrealClaudeProviderBackend::CodexCli ? TEXT("ask_for_approval_never") : TEXT("dangerously_skip_permissions");
		Row.SandboxPolicy = Status.Backend == EUnrealClaudeProviderBackend::CodexCli ? TEXT("danger-full-access") : TEXT("provider_managed_or_unspecified");
		Row.bUnrealMcpBridgeEnabled = true;
		Row.bDirectFileToolsAllowed = true;
		Row.bDirectShellToolsAllowed = true;
		Row.SessionPersistenceMode = TEXT("normal_provider_session");
		Row.AllowedMutationFamilies = { TEXT("unrestricted_provider_file_shell_power") };
		Row.CheckpointRequirement = TEXT("optional_but_traceable");
		Row.UiBadge = TEXT("danger_expert");
		Row.TraceReceiptExpectations = { TEXT("danger_label_visible"), TEXT("explicit_expert_opt_in control profile visible"), TEXT("never_silent_fallback_from_safe_lane") };
		Row.BehaviorState = TEXT("explicit_expert_opt_in_runtime");
		Row.bAvailableNow = true;
		Row.bCurrentProfileRow = RequestedRuntimeLaneFromProfile(Config.ExecutionProfile) == Row.Lane;
		Row.bCurrentEffectiveLane = Manifest.CurrentEffectiveRuntimeLane == Row.Lane;
		Row.bEnforcedNow = Row.bCurrentProfileRow;
		Row.TruthBoundary = TEXT("This row describes the explicit expert opt-in runtime lane. It remains high-risk by design, but it is no longer the configured default.");
		Row.Basis = { TEXT("permission_bypass_enabled=true"), TEXT("direct_file_tools_allowed=true"), TEXT("direct_shell_tools_allowed=true"), TEXT("session_persistence_mode=normal_provider_session"), TEXT("explicit_opt_in_required=true") };
		Manifest.ProviderTransportMatrix.Add(MoveTemp(Row));
	}

	Manifest.PolicyDenySchema = BuildPolicyDenySchema();
	Manifest.SessionBoundary = BuildSessionBoundaryManifest();
	return Manifest;
}

bool TryBuildAgentExecutionPolicyDenyContract(
	const FAgentBackendStatus& Status,
	const FAgentRequestConfig& Config,
	const FString& RequestedAction,
	FAgentExecutionPolicyDenyContract& OutContract)
{
	const FAgentProviderExecutionControlManifest Manifest = BuildAgentProviderExecutionControlManifest(Status, Config);
	OutContract = FAgentExecutionPolicyDenyContract();
	OutContract.RequestedLane = AgentExecutionRuntimeLaneToString(RequestedRuntimeLaneFromProfile(Config.ExecutionProfile));
	OutContract.EffectiveLane = AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane);
	OutContract.GoverningFamily = TEXT("provider_control_status");
	OutContract.DecisionSource = TEXT("execution_control_policy_runtime_gate");
	OutContract.VisibleSurfaces = { TEXT("tool_result_or_command_result"), TEXT("agent_trace"), TEXT("backend_run_receipt") };
	OutContract.TruthBoundary = TEXT("This denied result is real for the accepted current policy-enforcement paths. It proves first-class deny shape and visibility, not yet broad runtime-wide enforcement across every provider surface.");

	if (Config.ExecutionProfile == EAgentExecutionRunProfile::BoundedPluginMutation)
	{
		OutContract.RequestedAction = RequestedAction.IsEmpty() ? TEXT("provider_prompt_dispatch") : RequestedAction;
		OutContract.DenyReason = TEXT("bounded_plugin_mutation only permits explicit plugin-owned governed mutation families and cannot route an arbitrary provider prompt through the unrestricted provider runtime.");
		OutContract.PolicyRuleId = TEXT("bounded_plugin_mutation.provider_prompt_dispatch_denied");
		OutContract.bExpertOptInRequired = true;
		OutContract.Basis = {
			TEXT("requested_lane=bounded_plugin_mutation"),
			TEXT("provider_prompt_dispatch_requested=true"),
			FString::Printf(TEXT("effective_lane=%s"), AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)),
			TEXT("governed_transport=governed_plugin_surface_only"),
			TEXT("silent_fallback_occurred=false")
		};
		return true;
	}

	if (Config.ExecutionProfile == EAgentExecutionRunProfile::ExplicitExpertOptIn)
	{
		return false;
	}

	const bool bReadOnlyDiagnosticProfile = Config.ExecutionProfile == EAgentExecutionRunProfile::ReadOnlyDiagnostic;
	const bool bWorkspaceWriteDefaultProfile = Config.ExecutionProfile == EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	if (!bReadOnlyDiagnosticProfile && !bWorkspaceWriteDefaultProfile)
	{
		return false;
	}

	const bool bRequestedDirectFileTools = RequestsDirectFileTools(Config.AllowedTools);
	const bool bRequestedDirectShellTools = RequestsDirectShellTools(Config.AllowedTools);
	const bool bMutatingUnrealMcpEnabled = Config.bEnableUnrealMcpBridge;
	const bool bSessionPersistenceBoundaryViolated = Config.SessionPersistenceMode != EAgentSessionPersistenceMode::NotPersisted;

	if (bWorkspaceWriteDefaultProfile)
	{
		if (!bMutatingUnrealMcpEnabled && !bSessionPersistenceBoundaryViolated)
		{
			return false;
		}

		OutContract.RequestedLane = AgentExecutionRuntimeLaneToString(EAgentExecutionRuntimeLane::WorkspaceWriteProject);
		OutContract.EffectiveLane = AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane);
		OutContract.DecisionSource = TEXT("execution_control_policy_probe");
		OutContract.TruthBoundary = TEXT("This denied result is real for the accepted workspace-write default runtime: ordinary project/workspace file work is allowed there, but mutating Unreal MCP and persisted normal-session carry-over remain denied.");
		OutContract.RequestedAction = RequestedAction.IsEmpty()
			? (bMutatingUnrealMcpEnabled ? TEXT("mutating_unreal_mcp") : TEXT("persistent_session_carry_over"))
			: RequestedAction;

		if (bMutatingUnrealMcpEnabled)
		{
			OutContract.DenyReason = TEXT("workspace_write_project does not silently reopen mutating Unreal MCP surfaces on the ordinary workspace-write default lane.");
			OutContract.PolicyRuleId = TEXT("workspace_write_project.mutating_unreal_mcp_denied");
			OutContract.bSaferAlternativeExists = true;
			OutContract.SaferAlternativeLane = TEXT("bounded_plugin_mutation");
			OutContract.Basis = {
				TEXT("unreal_mcp_bridge_enabled=true"),
				TEXT("requested_lane=workspace_write_project"),
				TEXT("safer_alternative_lane=bounded_plugin_mutation"),
				TEXT("silent_fallback_occurred=false")
			};
			return true;
		}

		OutContract.DenyReason = TEXT("workspace_write_project keeps a non-persisted session boundary and cannot silently reuse the normal provider session store.");
		OutContract.PolicyRuleId = TEXT("workspace_write_project.session_persistence_boundary_denied");
		OutContract.bSaferAlternativeExists = true;
		OutContract.SaferAlternativeLane = TEXT("workspace_write_project");
		OutContract.Basis = {
			FString::Printf(TEXT("session_persistence_mode=%s"), AgentSessionPersistenceModeToString(Config.SessionPersistenceMode)),
			TEXT("requested_lane=workspace_write_project"),
			TEXT("required_session_persistence_mode=not_persisted"),
			TEXT("silent_fallback_occurred=false")
		};
		return true;
	}

	if (!bRequestedDirectFileTools && !bRequestedDirectShellTools && !bMutatingUnrealMcpEnabled && !bSessionPersistenceBoundaryViolated)
	{
		return false;
	}

	OutContract.RequestedLane = AgentExecutionRuntimeLaneToString(EAgentExecutionRuntimeLane::ReadOnlyAnalysis);
	OutContract.EffectiveLane = AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane);
	OutContract.DecisionSource = TEXT("execution_control_policy_probe");
	OutContract.TruthBoundary = TEXT("This denied result is real for the explicit read-only helper execution path. It proves first-class deny shape and visibility, not yet broad runtime-wide enforcement.");

	if (bRequestedDirectFileTools)
	{
		OutContract.RequestedAction = RequestedAction.IsEmpty() ? TEXT("direct_file_tools") : RequestedAction;
		OutContract.DenyReason = TEXT("read_only_analysis forbids direct provider file mutation tools on the explicit read-only helper lane.");
		OutContract.PolicyRuleId = TEXT("read_only_analysis.direct_file_tools_denied");
		OutContract.bExpertOptInRequired = true;
		OutContract.Basis = {
			TEXT("requested_direct_file_tools=true"),
			TEXT("requested_lane=read_only_analysis"),
			FString::Printf(TEXT("effective_lane=%s"), AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)),
			TEXT("silent_fallback_occurred=false")
		};
		return true;
	}

	OutContract.RequestedAction = RequestedAction.IsEmpty()
		? (bRequestedDirectShellTools ? TEXT("direct_shell_tools") : (bMutatingUnrealMcpEnabled ? TEXT("mutating_unreal_mcp") : TEXT("persistent_session_carry_over")))
		: RequestedAction;
	if (bRequestedDirectShellTools)
	{
		OutContract.DenyReason = TEXT("read_only_analysis forbids direct shell execution on the explicit read-only helper lane.");
		OutContract.PolicyRuleId = TEXT("read_only_analysis.direct_shell_tools_denied");
		OutContract.bExpertOptInRequired = true;
		OutContract.Basis = {
			TEXT("requested_direct_shell_tools=true"),
			TEXT("requested_lane=read_only_analysis"),
			FString::Printf(TEXT("effective_lane=%s"), AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)),
			TEXT("silent_fallback_occurred=false")
		};
		return true;
	}

	if (bMutatingUnrealMcpEnabled)
	{
		OutContract.DenyReason = TEXT("read_only_analysis forbids enabling mutating Unreal MCP surfaces on the explicit read-only helper lane.");
		OutContract.PolicyRuleId = TEXT("read_only_analysis.mutating_unreal_mcp_denied");
		OutContract.bSaferAlternativeExists = true;
		OutContract.SaferAlternativeLane = TEXT("bounded_plugin_mutation");
		OutContract.Basis = {
			TEXT("unreal_mcp_bridge_enabled=true"),
			TEXT("requested_lane=read_only_analysis"),
			TEXT("safer_alternative_lane=bounded_plugin_mutation"),
			TEXT("silent_fallback_occurred=false")
		};
		return true;
	}

	OutContract.DenyReason = TEXT("read_only_analysis requires a non-persisted boundary and cannot silently reuse the normal provider session store.");
	OutContract.PolicyRuleId = TEXT("read_only_analysis.session_persistence_boundary_denied");
	OutContract.bSaferAlternativeExists = true;
	OutContract.SaferAlternativeLane = TEXT("read_only_analysis");
	OutContract.Basis = {
		FString::Printf(TEXT("session_persistence_mode=%s"), AgentSessionPersistenceModeToString(Config.SessionPersistenceMode)),
		TEXT("required_session_persistence_mode=not_persisted"),
		TEXT("silent_fallback_occurred=false")
	};
	return true;
}

bool TryParsePolicyDenyContractFromToolResultJson(
	const FString& ToolResultJson,
	FAgentExecutionPolicyDenyContract& OutContract)
{
	OutContract = FAgentExecutionPolicyDenyContract();
	if (ToolResultJson.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolResultJson);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return false;
	}

	FString ResultType;
	if (!RootObject->TryGetStringField(TEXT("result_type"), ResultType)
		|| !ResultType.Equals(TEXT("policy_denied"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ContractObject = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("policy_denied_contract"), ContractObject)
		|| ContractObject == nullptr
		|| !(*ContractObject).IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>& Object = *ContractObject;
	Object->TryGetStringField(TEXT("schema_version"), OutContract.SchemaVersion);
	Object->TryGetStringField(TEXT("result_type"), OutContract.ResultType);
	Object->TryGetStringField(TEXT("requested_lane"), OutContract.RequestedLane);
	Object->TryGetStringField(TEXT("effective_lane"), OutContract.EffectiveLane);
	Object->TryGetStringField(TEXT("governing_family"), OutContract.GoverningFamily);
	Object->TryGetStringField(TEXT("requested_action"), OutContract.RequestedAction);
	Object->TryGetStringField(TEXT("deny_reason"), OutContract.DenyReason);
	Object->TryGetStringField(TEXT("policy_rule_id"), OutContract.PolicyRuleId);
	Object->TryGetStringField(TEXT("safer_alternative_lane"), OutContract.SaferAlternativeLane);
	Object->TryGetStringField(TEXT("decision_source"), OutContract.DecisionSource);
	Object->TryGetStringField(TEXT("truth_boundary"), OutContract.TruthBoundary);
	Object->TryGetBoolField(TEXT("safer_alternative_exists"), OutContract.bSaferAlternativeExists);
	Object->TryGetBoolField(TEXT("expert_opt_in_required"), OutContract.bExpertOptInRequired);
	Object->TryGetBoolField(TEXT("silent_fallback_prevented"), OutContract.bSilentFallbackPrevented);
	TryReadJsonStringArray(Object, TEXT("visible_surfaces"), OutContract.VisibleSurfaces);
	TryReadJsonStringArray(Object, TEXT("basis"), OutContract.Basis);
	return !OutContract.PolicyRuleId.IsEmpty();
}

TSharedPtr<FJsonObject> MakeAgentExecutionPolicyDenyContractJson(const FAgentExecutionPolicyDenyContract& Contract)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), Contract.SchemaVersion);
	Object->SetStringField(TEXT("result_type"), Contract.ResultType);
	Object->SetStringField(TEXT("requested_lane"), Contract.RequestedLane);
	Object->SetStringField(TEXT("effective_lane"), Contract.EffectiveLane);
	Object->SetStringField(TEXT("governing_family"), Contract.GoverningFamily);
	Object->SetStringField(TEXT("requested_action"), Contract.RequestedAction);
	Object->SetStringField(TEXT("deny_reason"), Contract.DenyReason);
	Object->SetStringField(TEXT("policy_rule_id"), Contract.PolicyRuleId);
	Object->SetBoolField(TEXT("safer_alternative_exists"), Contract.bSaferAlternativeExists);
	Object->SetStringField(TEXT("safer_alternative_lane"), Contract.SaferAlternativeLane);
	Object->SetBoolField(TEXT("expert_opt_in_required"), Contract.bExpertOptInRequired);
	Object->SetBoolField(TEXT("silent_fallback_prevented"), Contract.bSilentFallbackPrevented);
	Object->SetStringField(TEXT("decision_source"), Contract.DecisionSource);
	Object->SetArrayField(TEXT("visible_surfaces"), MakeJsonStringArray(Contract.VisibleSurfaces));
	Object->SetStringField(TEXT("truth_boundary"), Contract.TruthBoundary);
	Object->SetArrayField(TEXT("basis"), MakeJsonStringArray(Contract.Basis));
	return Object;
}

TSharedPtr<FJsonObject> MakeAgentProviderExecutionControlJson(const FAgentProviderExecutionControlManifest& Manifest)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), Manifest.SchemaVersion);
	Object->SetStringField(TEXT("manifest_source"), Manifest.ManifestSource);
	Object->SetStringField(TEXT("control_profile_id"), Manifest.ControlProfileId);
	Object->SetStringField(TEXT("backend"), UnrealClaudeProviderBackendToString(Manifest.Backend));
	Object->SetStringField(TEXT("backend_display_name"), Manifest.BackendDisplayName);
	Object->SetStringField(TEXT("execution_transport"), Manifest.ExecutionTransportLabel);
	Object->SetStringField(TEXT("execution_profile"), Manifest.ExecutionProfile);
	Object->SetStringField(TEXT("execution_control_plumbing_state"), AgentExecutionGovernanceStateToString(Manifest.ExecutionControlPlumbingState));
	Object->SetStringField(TEXT("session_persistence_mode"), AgentSessionPersistenceModeToString(Manifest.SessionPersistenceMode));
	Object->SetStringField(TEXT("current_effective_provider_power"), AgentExecutionPowerClassToString(Manifest.CurrentEffectiveProviderPowerClass));
	Object->SetStringField(TEXT("desired_future_default_provider_power"), AgentExecutionPowerClassToString(Manifest.DesiredFutureDefaultProviderPowerClass));
	Object->SetStringField(TEXT("current_effective_runtime_lane"), AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane));
	Object->SetStringField(TEXT("desired_future_default_runtime_lane"), AgentExecutionRuntimeLaneToString(Manifest.DesiredFutureDefaultRuntimeLane));
	Object->SetStringField(TEXT("current_effective_power_source"), Manifest.CurrentEffectivePowerSource);
	Object->SetBoolField(TEXT("current_effective_power_enforced_now"), Manifest.bCurrentEffectivePowerEnforcedNow);
	Object->SetBoolField(TEXT("desired_future_default_enforced_now"), Manifest.bDesiredFutureDefaultEnforcedNow);
	Object->SetBoolField(TEXT("future_tightening_described_only"), Manifest.bFutureTighteningDescribedOnly);
	Object->SetStringField(TEXT("truth_boundary"), Manifest.TruthBoundary);
	Object->SetBoolField(TEXT("permission_bypass_enabled"), Manifest.bPermissionBypassEnabled);
	Object->SetStringField(TEXT("approval_policy"), Manifest.ApprovalPolicy);
	Object->SetStringField(TEXT("sandbox_mode"), Manifest.SandboxMode);
	Object->SetBoolField(TEXT("requested_tool_allow_list_present"), Manifest.bRequestedToolAllowListPresent);
	Object->SetBoolField(TEXT("explicit_tool_allow_list_enforced"), Manifest.bExplicitToolAllowListEnforced);
	Object->SetStringField(TEXT("effective_tool_budget_mode"), Manifest.EffectiveToolBudgetMode);
	Object->SetBoolField(TEXT("unreal_mcp_bridge_enabled"), Manifest.bUnrealMcpBridgeEnabled);
	Object->SetBoolField(TEXT("mutating_mcp_tools_treated_as_available"), Manifest.bMutatingMcpToolsTreatedAsAvailable);
	Object->SetBoolField(TEXT("requested_direct_file_tools"), Manifest.bRequestedDirectFileTools);
	Object->SetBoolField(TEXT("requested_direct_shell_tools"), Manifest.bRequestedDirectShellTools);
	Object->SetBoolField(TEXT("direct_file_power_treated_as_available"), Manifest.bDirectFilePowerTreatedAsAvailable);
	Object->SetBoolField(TEXT("direct_shell_power_treated_as_available"), Manifest.bDirectShellPowerTreatedAsAvailable);
	Object->SetBoolField(TEXT("bounded_mutation_lane_available"), Manifest.bBoundedMutationLaneAvailable);
	Object->SetBoolField(TEXT("normal_provider_session_history_touched"), Manifest.bTouchesNormalProviderSessionHistory);
	Object->SetBoolField(TEXT("provider_session_file_updated_on_success"), Manifest.bWritesProviderSessionFileOnSuccess);
	Object->SetArrayField(TEXT("requested_allowed_tools"), MakeJsonStringArray(Manifest.RequestedAllowedTools));
	Object->SetArrayField(TEXT("current_effective_power_basis"), MakeJsonStringArray(Manifest.CurrentEffectivePowerBasis));
	Object->SetStringField(TEXT("session_persistence_truth_boundary"), Manifest.SessionPersistenceTruthBoundary);
	Object->SetArrayField(TEXT("session_persistence_basis"), MakeJsonStringArray(Manifest.SessionPersistenceBasis));

	TArray<TSharedPtr<FJsonValue>> PowerLaneObjects;
	PowerLaneObjects.Reserve(Manifest.PowerLanes.Num());
	for (const FAgentExecutionPowerLane& Lane : Manifest.PowerLanes)
	{
		PowerLaneObjects.Add(MakeShared<FJsonValueObject>(MakePowerLaneJson(Lane)));
	}
	Object->SetArrayField(TEXT("power_lanes"), PowerLaneObjects);

	TArray<TSharedPtr<FJsonValue>> TaxonomyObjects;
	TaxonomyObjects.Reserve(Manifest.RuntimeLaneTaxonomy.Num());
	for (const FAgentExecutionLaneTaxonomyEntry& Entry : Manifest.RuntimeLaneTaxonomy)
	{
		TaxonomyObjects.Add(MakeShared<FJsonValueObject>(MakeLaneTaxonomyJson(Entry)));
	}
	Object->SetArrayField(TEXT("runtime_lane_taxonomy"), TaxonomyObjects);
	Object->SetNumberField(TEXT("runtime_lane_count"), Manifest.RuntimeLaneTaxonomy.Num());

	TArray<TSharedPtr<FJsonValue>> ProfileMappingObjects;
	ProfileMappingObjects.Reserve(Manifest.ProfileLaneMappings.Num());
	for (const FAgentExecutionProfileLaneMapping& Mapping : Manifest.ProfileLaneMappings)
	{
		ProfileMappingObjects.Add(MakeShared<FJsonValueObject>(MakeProfileLaneMappingJson(Mapping)));
	}
	Object->SetArrayField(TEXT("profile_lane_mappings"), ProfileMappingObjects);
	Object->SetNumberField(TEXT("profile_lane_mapping_count"), Manifest.ProfileLaneMappings.Num());

	TArray<TSharedPtr<FJsonValue>> MatrixObjects;
	MatrixObjects.Reserve(Manifest.ProviderTransportMatrix.Num());
	for (const FAgentExecutionProviderTransportMatrixRow& Row : Manifest.ProviderTransportMatrix)
	{
		MatrixObjects.Add(MakeShared<FJsonValueObject>(MakeProviderTransportMatrixRowJson(Row)));
	}
	Object->SetArrayField(TEXT("provider_transport_matrix"), MatrixObjects);
	Object->SetNumberField(TEXT("provider_transport_matrix_row_count"), Manifest.ProviderTransportMatrix.Num());

	Object->SetObjectField(TEXT("policy_deny_contract"), MakePolicyDenySchemaJson(Manifest.PolicyDenySchema));
	Object->SetObjectField(TEXT("session_boundary"), MakeSessionBoundaryManifestJson(Manifest.SessionBoundary));
	return Object;
}
