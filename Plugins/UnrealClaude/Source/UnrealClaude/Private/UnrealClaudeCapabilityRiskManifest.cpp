// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeCapabilityRiskManifest.h"
#include "UnrealClaudeConstants.h"
#include "UnrealClaudeModule.h"
#include "Algo/Unique.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/UnrealClaudeMCPServer.h"

namespace
{
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

	TSharedPtr<FJsonObject> MakeFamilyJson(const FAgentCapabilityRiskFamilyManifest& Family)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("family_id"), Family.FamilyId);
		Object->SetStringField(TEXT("display_name"), Family.DisplayName);
		Object->SetStringField(TEXT("purpose"), Family.Purpose);
		Object->SetStringField(TEXT("capability_mode"), AgentCapabilityFamilyModeToString(Family.CapabilityMode));
		Object->SetStringField(TEXT("mutation_class"), Family.MutationClass);
		Object->SetStringField(TEXT("risk_class"), Family.RiskClass);
		Object->SetStringField(TEXT("required_proof_tier"), Family.RequiredProofTier);
		Object->SetStringField(TEXT("reverification_expectation"), Family.ReverificationExpectation);
		Object->SetStringField(TEXT("revertability"), Family.Revertability);
		Object->SetStringField(TEXT("checkpoint_expectation"), Family.CheckpointExpectation);
		Object->SetStringField(TEXT("stop_condition"), Family.StopCondition);
		Object->SetBoolField(TEXT("writes_user_owned_state"), Family.bWritesUserOwnedState);
		Object->SetBoolField(TEXT("writes_plugin_owned_state"), Family.bWritesPluginOwnedState);
		Object->SetBoolField(TEXT("requires_fresh_build_for_acceptance"), Family.bRequiresFreshBuildForAcceptance);
		Object->SetBoolField(TEXT("requires_representative_live_proof"), Family.bRequiresRepresentativeLiveProof);
		Object->SetBoolField(TEXT("requires_revert_or_checkpoint_for_mutation"), Family.bRequiresRevertOrCheckpointForMutation);
		Object->SetArrayField(TEXT("representative_surfaces"), MakeJsonStringArray(Family.RepresentativeSurfaces));
		Object->SetArrayField(TEXT("major_control_dependencies"), MakeJsonStringArray(Family.MajorControlDependencies));
		Object->SetArrayField(TEXT("current_reality_basis"), MakeJsonStringArray(Family.CurrentRealityBasis));
		Object->SetStringField(TEXT("truth_boundary"), Family.TruthBoundary);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeToolMappingJson(const FAgentCapabilityRiskToolMapping& Mapping)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("tool_name"), Mapping.ToolName);
		Object->SetStringField(TEXT("surface_identifier"), Mapping.SurfaceIdentifier);
		Object->SetStringField(TEXT("mapping_scope"), Mapping.MappingScope);
		Object->SetStringField(TEXT("governing_family_id"), Mapping.GoverningFamilyId);
		Object->SetStringField(TEXT("governing_family_display_name"), Mapping.GoverningFamilyDisplayName);
		Object->SetStringField(TEXT("ambiguity_state"), Mapping.AmbiguityState);
		Object->SetStringField(TEXT("ambiguity_detail"), Mapping.AmbiguityDetail);
		Object->SetStringField(TEXT("capability_mode"), AgentCapabilityFamilyModeToString(Mapping.CapabilityMode));
		Object->SetStringField(TEXT("mutation_class"), Mapping.MutationClass);
		Object->SetStringField(TEXT("risk_class"), Mapping.RiskClass);
		Object->SetStringField(TEXT("required_proof_tier"), Mapping.RequiredProofTier);
		Object->SetStringField(TEXT("revertability"), Mapping.Revertability);
		Object->SetStringField(TEXT("checkpoint_expectation"), Mapping.CheckpointExpectation);
		Object->SetStringField(TEXT("stop_condition"), Mapping.StopCondition);
		Object->SetBoolField(TEXT("writes_user_owned_state"), Mapping.bWritesUserOwnedState);
		Object->SetBoolField(TEXT("writes_plugin_owned_state"), Mapping.bWritesPluginOwnedState);
		Object->SetBoolField(TEXT("requires_fresh_build_for_acceptance"), Mapping.bRequiresFreshBuildForAcceptance);
		Object->SetBoolField(TEXT("requires_representative_live_proof"), Mapping.bRequiresRepresentativeLiveProof);
		Object->SetBoolField(TEXT("requires_revert_or_checkpoint_for_mutation"), Mapping.bRequiresRevertOrCheckpointForMutation);
		Object->SetStringField(TEXT("truth_boundary"), Mapping.TruthBoundary);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeCoverageBucketJson(const FAgentCapabilityRiskToolCoverageBucket& Bucket)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("bucket_id"), Bucket.BucketId);
		Object->SetStringField(TEXT("display_name"), Bucket.DisplayName);
		Object->SetNumberField(TEXT("tool_count"), Bucket.ToolNames.Num());
		Object->SetArrayField(TEXT("tool_names"), MakeJsonStringArray(Bucket.ToolNames));
		return Object;
	}

	TSharedPtr<FJsonObject> MakeRiskySurfaceCoverageBucketJson(const FAgentCapabilityRiskySurfaceCoverageBucket& Bucket)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("bucket_id"), Bucket.BucketId);
		Object->SetStringField(TEXT("display_name"), Bucket.DisplayName);
		Object->SetNumberField(TEXT("surface_count"), Bucket.SurfaceKeys.Num());
		Object->SetArrayField(TEXT("surface_keys"), MakeJsonStringArray(Bucket.SurfaceKeys));
		return Object;
	}

	TSharedPtr<FJsonObject> MakeRiskySurfaceLaneBehaviorJson(const FAgentCapabilityRiskySurfaceLaneBehavior& Behavior)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("control_profile_id"), Behavior.ControlProfileId);
		Object->SetStringField(TEXT("execution_profile"), Behavior.ExecutionProfile);
		Object->SetStringField(TEXT("runtime_lane"), Behavior.RuntimeLane);
		Object->SetStringField(TEXT("execution_transport"), Behavior.ExecutionTransport);
		Object->SetStringField(TEXT("session_persistence_mode"), Behavior.SessionPersistenceMode);
		Object->SetStringField(TEXT("behavior_state"), Behavior.BehaviorState);
		Object->SetStringField(TEXT("policy_rule_id"), Behavior.PolicyRuleId);
		Object->SetBoolField(TEXT("behavior_enforced_now"), Behavior.bBehaviorEnforcedNow);
		Object->SetStringField(TEXT("enforcement_state"), Behavior.EnforcementState);
		Object->SetArrayField(TEXT("basis"), MakeJsonStringArray(Behavior.Basis));
		Object->SetStringField(TEXT("truth_boundary"), Behavior.TruthBoundary);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeRiskySurfaceEntryJson(const FAgentCapabilityRiskySurfaceEntry& Entry)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("tool_name"), Entry.ToolName);
		Object->SetStringField(TEXT("surface_identifier"), Entry.SurfaceIdentifier);
		Object->SetStringField(TEXT("surface_kind"), Entry.SurfaceKind);
		Object->SetStringField(TEXT("risk_focus"), Entry.RiskFocus);
		Object->SetStringField(TEXT("governance_state"), Entry.GovernanceState);
		Object->SetStringField(TEXT("governance_bucket_id"), Entry.GovernanceBucketId);
		Object->SetStringField(TEXT("governance_bucket_display_name"), Entry.GovernanceBucketDisplayName);
		Object->SetStringField(TEXT("mapping_scope"), Entry.MappingScope);
		Object->SetStringField(TEXT("family_mapping_state"), Entry.FamilyMappingState);
		Object->SetStringField(TEXT("governing_family_id"), Entry.GoverningFamilyId);
		Object->SetStringField(TEXT("governing_family_display_name"), Entry.GoverningFamilyDisplayName);
		Object->SetStringField(TEXT("mutation_class"), Entry.MutationClass);
		Object->SetStringField(TEXT("risk_class"), Entry.RiskClass);
		Object->SetBoolField(TEXT("safe_or_bounded_behavior_explicit"), Entry.bSafeOrBoundedBehaviorExplicit);
		Object->SetObjectField(TEXT("configured_default_runtime"), MakeRiskySurfaceLaneBehaviorJson(Entry.ConfiguredDefaultRuntime));
		Object->SetObjectField(TEXT("bounded_plugin_mutation"), MakeRiskySurfaceLaneBehaviorJson(Entry.BoundedPluginMutation));
		Object->SetObjectField(TEXT("explicit_expert_opt_in"), MakeRiskySurfaceLaneBehaviorJson(Entry.ExplicitExpertOptIn));
		Object->SetStringField(TEXT("truth_boundary"), Entry.TruthBoundary);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeCppScopeCategoryJson(const FAgentCppScopeCategoryManifest& Category)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("category_id"), Category.CategoryId);
		Object->SetStringField(TEXT("display_name"), Category.DisplayName);
		Object->SetStringField(TEXT("scope_state"), Category.ScopeState);
		Object->SetStringField(TEXT("current_availability"), Category.CurrentAvailability);
		Object->SetStringField(TEXT("purpose"), Category.Purpose);
		Object->SetStringField(TEXT("mutation_breadth"), Category.MutationBreadth);
		Object->SetStringField(TEXT("required_proof_tier"), Category.RequiredProofTier);
		Object->SetStringField(TEXT("rebuild_expectation"), Category.RebuildExpectation);
		Object->SetStringField(TEXT("revert_expectation"), Category.RevertExpectation);
		Object->SetStringField(TEXT("checkpoint_expectation"), Category.CheckpointExpectation);
		Object->SetStringField(TEXT("stop_condition"), Category.StopCondition);
		Object->SetBoolField(TEXT("requires_fresh_build_for_acceptance"), Category.bRequiresFreshBuildForAcceptance);
		Object->SetBoolField(TEXT("requires_representative_live_proof"), Category.bRequiresRepresentativeLiveProof);
		Object->SetBoolField(TEXT("requires_restore_or_revert_for_mutation"), Category.bRequiresRestoreOrRevertForMutation);
		Object->SetArrayField(TEXT("representative_surfaces"), MakeJsonStringArray(Category.RepresentativeSurfaces));
		Object->SetArrayField(TEXT("current_lane_mappings"), MakeJsonStringArray(Category.CurrentLaneMappings));
		Object->SetArrayField(TEXT("current_reality_basis"), MakeJsonStringArray(Category.CurrentRealityBasis));
		Object->SetStringField(TEXT("truth_boundary"), Category.TruthBoundary);
		return Object;
	}

	void SortAndDeduplicateStrings(TArray<FString>& Values)
	{
		Values.Sort();
		Values.SetNum(Algo::Unique(Values));
	}

	TArray<FString> CollectCurrentToolInventory(FString& InventorySource)
	{
		TArray<FString> ToolNames;
		InventorySource = TEXT("expected_tools_fallback");

		if (FUnrealClaudeModule::IsAvailable())
		{
			const TSharedPtr<FUnrealClaudeMCPServer> MCPServer = FUnrealClaudeModule::Get().GetMCPServer();
			if (MCPServer.IsValid() && MCPServer->IsRunning())
			{
				const TSharedPtr<FMCPToolRegistry> ToolRegistry = MCPServer->GetToolRegistry();
				if (ToolRegistry.IsValid())
				{
					const TArray<FMCPToolInfo> Tools = ToolRegistry->GetAllTools();
					for (const FMCPToolInfo& Tool : Tools)
					{
						ToolNames.Add(Tool.Name);
					}

					if (ToolNames.Num() > 0)
					{
						InventorySource = TEXT("live_mcp_registry");
					}
				}
			}
		}

		if (ToolNames.Num() == 0)
		{
			ToolNames = UnrealClaudeConstants::MCPServer::ExpectedTools;
		}

		SortAndDeduplicateStrings(ToolNames);
		return ToolNames;
	}

	FAgentCapabilityRiskToolCoverageBucket MakeCoverageBucket(
		const FString& BucketId,
		const FString& DisplayName,
		const TArray<FString>& ToolNames)
	{
		FAgentCapabilityRiskToolCoverageBucket Bucket;
		Bucket.BucketId = BucketId;
		Bucket.DisplayName = DisplayName;
		Bucket.ToolNames = ToolNames;
		return Bucket;
	}

	FString ClassifyUnmappedToolBucketId(const FString& ToolName)
	{
		if (ToolName.StartsWith(TEXT("osvayder_")))
		{
			return TEXT("external_osvayder_tools");
		}

		if (ToolName.StartsWith(TEXT("task_")))
		{
			return TEXT("async_task_wrappers");
		}

		if (ToolName == TEXT("run_console_command")
			|| ToolName == TEXT("get_output_log")
			|| ToolName == TEXT("capture_viewport")
			|| ToolName == TEXT("project_memory_status")
			|| ToolName == TEXT("execution_log_status")
			|| ToolName == TEXT("cleanup_scripts")
			|| ToolName == TEXT("get_script_history")
			|| ToolName == TEXT("open_level"))
		{
			return TEXT("editor_runtime_helper_tools");
		}

		return TEXT("legacy_authoring_breadth");
	}

	FString GetCoverageBucketDisplayName(const FString& BucketId)
	{
		if (BucketId == TEXT("external_osvayder_tools"))
		{
			return TEXT("External Osvayder Tools");
		}

		if (BucketId == TEXT("async_task_wrappers"))
		{
			return TEXT("Async Task Wrappers");
		}

		if (BucketId == TEXT("editor_runtime_helper_tools"))
		{
			return TEXT("Editor/Runtime Helper Tools");
		}

		return TEXT("Legacy Authoring Breadth");
	}

	FString MakeRiskySurfaceKey(const FString& ToolName, const FString& SurfaceIdentifier)
	{
		return SurfaceIdentifier.IsEmpty()
			? ToolName
			: FString::Printf(TEXT("%s:%s"), *ToolName, *SurfaceIdentifier);
	}

	bool IsMutatingOsvayderTool(const FString& ToolName)
	{
		return ToolName == TEXT("osvayder_mouse_click")
			|| ToolName == TEXT("osvayder_mouse_double_click")
			|| ToolName == TEXT("osvayder_mouse_move")
			|| ToolName == TEXT("osvayder_mouse_drag")
			|| ToolName == TEXT("osvayder_mouse_scroll")
			|| ToolName == TEXT("osvayder_keyboard_type")
			|| ToolName == TEXT("osvayder_keyboard_hotkey")
			|| ToolName == TEXT("osvayder_keyboard_press")
			|| ToolName == TEXT("osvayder_focus_window");
	}

	bool IsExternalUiControlBacklogTool(const FString& ToolName)
	{
		return ToolName == TEXT("osvayder_mouse_click")
			|| ToolName == TEXT("osvayder_mouse_double_click")
			|| ToolName == TEXT("osvayder_mouse_move")
			|| ToolName == TEXT("osvayder_mouse_drag")
			|| ToolName == TEXT("osvayder_mouse_scroll")
			|| ToolName == TEXT("osvayder_keyboard_type")
			|| ToolName == TEXT("osvayder_keyboard_hotkey")
			|| ToolName == TEXT("osvayder_keyboard_press")
			|| ToolName == TEXT("osvayder_focus_window");
	}

	FString GetRiskySurfaceBacklogBucketDisplayName(const FString& BucketId)
	{
		if (BucketId == TEXT("external_ui_control_backlog"))
		{
			return TEXT("External UI Control Backlog");
		}

		if (BucketId == TEXT("high_risk_execution_backlog"))
		{
			return TEXT("High-Risk Execution Backlog");
		}

		return TEXT("Broad Authoring Mutation Backlog");
	}

	const FAgentCapabilityRiskFamilyManifest* FindFamilyById(
		const FAgentCapabilityRiskManifest& Manifest,
		const FString& FamilyId)
	{
		for (const FAgentCapabilityRiskFamilyManifest& Family : Manifest.Families)
		{
			if (Family.FamilyId == FamilyId)
			{
				return &Family;
			}
		}

		return nullptr;
	}

	const FAgentCapabilityRiskToolMapping* FindToolMapping(
		const FAgentCapabilityRiskToolMappingManifest& ToolMappingManifest,
		const FString& ToolName,
		const FString& SurfaceIdentifier = FString())
	{
		for (const FAgentCapabilityRiskToolMapping& Mapping : ToolMappingManifest.ToolMappings)
		{
			if (Mapping.ToolName != ToolName)
			{
				continue;
			}

			if (SurfaceIdentifier.IsEmpty() || Mapping.SurfaceIdentifier == SurfaceIdentifier)
			{
				return &Mapping;
			}
		}

		return nullptr;
	}

	const FAgentExecutionProviderTransportMatrixRow* FindTransportRowByLane(
		const FAgentProviderExecutionControlManifest& Manifest,
		const EAgentExecutionRuntimeLane Lane)
	{
		for (const FAgentExecutionProviderTransportMatrixRow& Row : Manifest.ProviderTransportMatrix)
		{
			if (Row.Lane == Lane)
			{
				return &Row;
			}
		}

		return nullptr;
	}

	FAgentCapabilityRiskySurfaceLaneBehavior MakeRiskySurfaceLaneBehavior(
		const FAgentProviderExecutionControlManifest& Manifest,
		const FString& BehaviorState,
		const FString& PolicyRuleId,
		const bool bBehaviorEnforcedNow,
		const FString& EnforcementState,
		const TArray<FString>& AdditionalBasis,
		const FString& TruthBoundary)
	{
		FAgentCapabilityRiskySurfaceLaneBehavior Behavior;
		Behavior.ControlProfileId = Manifest.ControlProfileId;
		Behavior.ExecutionProfile = Manifest.ExecutionProfile;
		Behavior.RuntimeLane = AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane);
		Behavior.ExecutionTransport = Manifest.ExecutionTransportLabel;
		Behavior.SessionPersistenceMode = AgentSessionPersistenceModeToString(Manifest.SessionPersistenceMode);
		Behavior.BehaviorState = BehaviorState;
		Behavior.PolicyRuleId = PolicyRuleId;
		Behavior.bBehaviorEnforcedNow = bBehaviorEnforcedNow;
		Behavior.EnforcementState = EnforcementState;
		Behavior.Basis = Manifest.CurrentEffectivePowerBasis;
		Behavior.Basis.Append(AdditionalBasis);
		Behavior.TruthBoundary = TruthBoundary;
		SortAndDeduplicateStrings(Behavior.Basis);
		return Behavior;
	}

	FAgentCapabilityRiskToolMappingManifest BuildToolMappingManifest(const FAgentCapabilityRiskManifest& Manifest)
	{
		FAgentCapabilityRiskToolMappingManifest ToolManifest;
		ToolManifest.TruthBoundary =
			TEXT("This tool/surface mapping is a reviewer-readable obligation index layered on top of the accepted family taxonomy. ")
			TEXT("It maps concrete tool names or surface identifiers to governing families and inherited obligations, but it is not yet a hard per-tool enforcement engine.");

		auto AddMapping = [&ToolManifest, &Manifest](
			const FString& ToolName,
			const FString& SurfaceIdentifier,
			const FString& MappingScope,
			const FString& FamilyId,
			const FString& AmbiguityState,
			const FString& AmbiguityDetail)
		{
			const FAgentCapabilityRiskFamilyManifest* Family = FindFamilyById(Manifest, FamilyId);
			if (!Family)
			{
				return;
			}

			FAgentCapabilityRiskToolMapping Mapping;
			Mapping.ToolName = ToolName;
			Mapping.SurfaceIdentifier = SurfaceIdentifier;
			Mapping.MappingScope = MappingScope;
			Mapping.GoverningFamilyId = Family->FamilyId;
			Mapping.GoverningFamilyDisplayName = Family->DisplayName;
			Mapping.AmbiguityState = AmbiguityState;
			Mapping.AmbiguityDetail = AmbiguityDetail;
			Mapping.CapabilityMode = Family->CapabilityMode;
			Mapping.MutationClass = Family->MutationClass;
			Mapping.RiskClass = Family->RiskClass;
			Mapping.RequiredProofTier = Family->RequiredProofTier;
			Mapping.Revertability = Family->Revertability;
			Mapping.CheckpointExpectation = Family->CheckpointExpectation;
			Mapping.StopCondition = Family->StopCondition;
			Mapping.bWritesUserOwnedState = Family->bWritesUserOwnedState;
			Mapping.bWritesPluginOwnedState = Family->bWritesPluginOwnedState;
			Mapping.bRequiresFreshBuildForAcceptance = Family->bRequiresFreshBuildForAcceptance;
			Mapping.bRequiresRepresentativeLiveProof = Family->bRequiresRepresentativeLiveProof;
			Mapping.bRequiresRevertOrCheckpointForMutation = Family->bRequiresRevertOrCheckpointForMutation;
			Mapping.TruthBoundary =
				FString::Printf(
					TEXT("This mapping inherits obligations from family `%s`. Classification here is reviewer-readable only and does not itself enforce runtime safety."),
					*Family->FamilyId);

			ToolManifest.ToolMappings.Add(MoveTemp(Mapping));
		};

		AddMapping(
			TEXT("plugin_settings"),
			TEXT("assistant_backend.provider_execution_control"),
			TEXT("tool_surface"),
			TEXT("provider_control_status"),
			TEXT("surface_scoped_mapping"),
			TEXT("plugin_settings is a broad settings/introspection tool; this slice maps its provider/control readback surface, not every field it returns."));
		AddMapping(
			TEXT("plugin_settings"),
			TEXT("assistant_backend.execution_profiles.bounded_plugin_mutation"),
			TEXT("tool_surface"),
			TEXT("provider_control_status"),
			TEXT("surface_scoped_mapping"),
			TEXT("This surface-scoped mapping covers the bounded runtime profile readback as a first-class governed mutation lane, not the entire plugin_settings response."));
		AddMapping(
			TEXT("plugin_settings"),
			TEXT("assistant_backend.execution_profiles.explicit_expert_opt_in"),
			TEXT("tool_surface"),
			TEXT("provider_control_status"),
			TEXT("surface_scoped_mapping"),
			TEXT("This surface-scoped mapping covers the explicit expert runtime readback as the accepted high-risk execution lane, not the entire plugin_settings response."));
		AddMapping(TEXT("report_export"), TEXT(""), TEXT("tool"), TEXT("report_evidence_trace"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("report_artifact_status"), TEXT(""), TEXT("tool"), TEXT("report_evidence_trace"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("agent_trace_status"), TEXT(""), TEXT("tool"), TEXT("report_evidence_trace"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("dependency_health"), TEXT(""), TEXT("tool"), TEXT("dependency_metadata_truth"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("metadata_truth"), TEXT(""), TEXT("tool"), TEXT("dependency_metadata_truth"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("map_runtime_proof"), TEXT(""), TEXT("tool"), TEXT("runtime_proof"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("oss_session_proof"), TEXT(""), TEXT("tool"), TEXT("runtime_proof"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("blueprint_query"), TEXT(""), TEXT("tool"), TEXT("blueprint_query"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("blueprint_modify"), TEXT(""), TEXT("tool"), TEXT("blueprint_mutation"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("anim_blueprint_modify"), TEXT(""), TEXT("tool"), TEXT("blueprint_mutation"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("cpp_reflection"), TEXT(""), TEXT("tool"), TEXT("cpp_reflected_contracts"), TEXT("exact_tool_mapping"), TEXT(""));
		AddMapping(TEXT("mutation_group"), TEXT(""), TEXT("tool"), TEXT("mutation_group_revert"), TEXT("exact_tool_mapping"), TEXT(""));

		return ToolManifest;
	}

	FAgentCapabilityRiskToolCoverageManifest BuildToolCoverageManifest(const FAgentCapabilityRiskToolMappingManifest& ToolMappingManifest)
	{
		FAgentCapabilityRiskToolCoverageManifest Coverage;
		Coverage.TruthBoundary =
			TEXT("Coverage is calculated against the current registered tool inventory when available, with a fallback to the expected tool list only when live registry access is unavailable. ")
			TEXT("Unmapped means intentionally unmapped in the current governance layer, not implicitly safe or low-risk.");

		TArray<FString> RegistryToolNames = CollectCurrentToolInventory(Coverage.RegistryInventorySource);
		TArray<FString> MappedToolNames;
		for (const FAgentCapabilityRiskToolMapping& Mapping : ToolMappingManifest.ToolMappings)
		{
			MappedToolNames.Add(Mapping.ToolName);
		}
		SortAndDeduplicateStrings(MappedToolNames);

		TArray<FString> RegistryMappedToolNames;
		TArray<FString> UnmappedToolNames;
		for (const FString& ToolName : RegistryToolNames)
		{
			if (MappedToolNames.Contains(ToolName))
			{
				RegistryMappedToolNames.Add(ToolName);
			}
			else
			{
				UnmappedToolNames.Add(ToolName);
			}
		}

		TMap<FString, TArray<FString>> BucketMap;
		for (const FString& ToolName : UnmappedToolNames)
		{
			BucketMap.FindOrAdd(ClassifyUnmappedToolBucketId(ToolName)).Add(ToolName);
		}

		TArray<FString> SortedBucketIds;
		BucketMap.GetKeys(SortedBucketIds);
		SortedBucketIds.Sort();
		for (const FString& BucketId : SortedBucketIds)
		{
			TArray<FString>& ToolNames = BucketMap.FindChecked(BucketId);
			SortAndDeduplicateStrings(ToolNames);
			Coverage.UnmappedBuckets.Add(MakeCoverageBucket(BucketId, GetCoverageBucketDisplayName(BucketId), ToolNames));
		}

		Coverage.RegistryToolCount = RegistryToolNames.Num();
		Coverage.MappedToolCount = RegistryMappedToolNames.Num();
		Coverage.UnmappedToolCount = UnmappedToolNames.Num();
		Coverage.MappingCoverageRatio = Coverage.RegistryToolCount > 0
			? static_cast<double>(Coverage.MappedToolCount) / static_cast<double>(Coverage.RegistryToolCount)
			: 0.0;
		Coverage.MappedToolNames = RegistryMappedToolNames;
		Coverage.UnmappedToolNames = UnmappedToolNames;
		return Coverage;
	}

	FAgentCapabilityRiskySurfaceCoverageManifest BuildRiskySurfaceCoverageManifest(
		const FAgentCapabilityRiskToolMappingManifest& ToolMappingManifest,
		const FAgentProviderExecutionControlManifest& DefaultRuntimeManifest,
		const FAgentProviderExecutionControlManifest& BoundedRuntimeManifest,
		const FAgentProviderExecutionControlManifest& ExplicitExpertManifest)
	{
		FAgentCapabilityRiskySurfaceCoverageManifest Coverage;
		Coverage.TruthBoundary =
			TEXT("This risky-surface coverage view maps high-risk execution surfaces and mutation-capable surfaces first. ")
			TEXT("Lane behavior is stated against the current workspace-write default, bounded, and explicit-expert provider runtime profiles only. ")
			TEXT("Direct MCP deny-contract gates are real for a narrow accepted subset of backlog surfaces, including the full external UI control bucket, but this view still does not claim that every out-of-band MCP caller is already hard-governed.");

		const FAgentExecutionProviderTransportMatrixRow* BoundedRow = FindTransportRowByLane(
			BoundedRuntimeManifest,
			EAgentExecutionRuntimeLane::BoundedPluginMutation);

		auto MakeConfiguredDefaultMutationBlocked = [&DefaultRuntimeManifest](const FString& SurfaceKey)
		{
			return MakeRiskySurfaceLaneBehavior(
				DefaultRuntimeManifest,
				TEXT("blocked_no_mutating_bridge"),
				TEXT(""),
				false,
				TEXT("described_bridge_disabled"),
				{
					TEXT("unreal_mcp_bridge_enabled=false"),
					TEXT("mutating_mcp_tools_treated_as_available=false"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("The configured workspace-write default runtime keeps mutating Unreal MCP surfaces disabled and does not expose them on the ordinary provider file/shell lane."));
		};

		auto MakeConfiguredDefaultNotExposed = [&DefaultRuntimeManifest](const FString& SurfaceKey)
		{
			return MakeRiskySurfaceLaneBehavior(
				DefaultRuntimeManifest,
				TEXT("not_exposed_unreal_mcp_bridge_disabled"),
				TEXT(""),
				false,
				TEXT("described_bridge_disabled"),
				{
					TEXT("unreal_mcp_bridge_enabled=false"),
					TEXT("execution_transport=exec_per_message"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("The configured workspace-write default runtime does not expose this risky surface because the ordinary default keeps the Unreal MCP bridge disabled even while direct project/workspace file editing stays available."));
		};

		auto MakeBoundedDenied = [&BoundedRuntimeManifest](const FString& SurfaceKey)
		{
			return MakeRiskySurfaceLaneBehavior(
				BoundedRuntimeManifest,
				TEXT("provider_prompt_dispatch_denied_outside_governed_families"),
				TEXT("bounded_plugin_mutation.provider_prompt_dispatch_denied"),
				false,
				TEXT("described_provider_prompt_dispatch_gate"),
				{
					TEXT("provider_prompt_dispatch_denied=true"),
					TEXT("allowed_mutation_families_are_allow_list_only"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("The bounded runtime does not silently widen to this surface. Outside the accepted governed families, provider prompt dispatch stays denied instead of falling through to expert power."));
		};

		auto MakeConfiguredDefaultBroadMutationDenied = [&DefaultRuntimeManifest](const FString& SurfaceKey)
		{
			return MakeRiskySurfaceLaneBehavior(
				DefaultRuntimeManifest,
				TEXT("direct_policy_denied_broad_authoring_mutation_surface"),
				TEXT("workspace_write_project.broad_authoring_mutation_surface_denied"),
				true,
				TEXT("direct_policy_deny_contract"),
				{
					TEXT("direct_mcp_runtime_gate=true"),
					TEXT("requested_lane=workspace_write_project"),
					TEXT("registry_runtime_gate=full_broad_authoring_mutation_bucket"),
					TEXT("tool_surface_granularity=true"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("This broad authoring mutation backlog surface now has a real direct MCP policy-deny contract on the workspace-write default lane for the full bucket as currently modeled at tool-surface granularity."));
		};

		auto MakeConfiguredDefaultRepresentativeHighRiskDenied = [&DefaultRuntimeManifest](const FString& SurfaceKey)
		{
			return MakeRiskySurfaceLaneBehavior(
				DefaultRuntimeManifest,
				TEXT("direct_policy_denied_representative_high_risk_execution_surface"),
				TEXT("workspace_write_project.representative_high_risk_execution_surface_denied"),
				true,
				TEXT("direct_policy_deny_contract"),
				{
					TEXT("direct_mcp_runtime_gate=true"),
					TEXT("requested_lane=workspace_write_project"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("This representative high-risk execution surface now has a real direct MCP policy-deny contract on the workspace-write default lane."));
		};

		auto MakeBoundedBroadMutationDenied = [&BoundedRuntimeManifest](const FString& SurfaceKey)
		{
			return MakeRiskySurfaceLaneBehavior(
				BoundedRuntimeManifest,
				TEXT("direct_policy_denied_broad_authoring_mutation_surface"),
				TEXT("bounded_plugin_mutation.broad_authoring_mutation_surface_denied"),
				true,
				TEXT("direct_policy_deny_contract"),
				{
					TEXT("direct_mcp_runtime_gate=true"),
					TEXT("requested_lane=bounded_plugin_mutation"),
					TEXT("registry_runtime_gate=full_broad_authoring_mutation_bucket"),
					TEXT("tool_surface_granularity=true"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("This broad authoring mutation backlog surface now has a real direct MCP policy-deny contract on the bounded lane for the full bucket as currently modeled at tool-surface granularity rather than only inheriting the generic provider prompt-dispatch deny story."));
		};

		auto MakeBoundedRepresentativeHighRiskDenied = [&BoundedRuntimeManifest](const FString& SurfaceKey)
		{
			return MakeRiskySurfaceLaneBehavior(
				BoundedRuntimeManifest,
				TEXT("direct_policy_denied_representative_high_risk_execution_surface"),
				TEXT("bounded_plugin_mutation.representative_high_risk_execution_surface_denied"),
				true,
				TEXT("direct_policy_deny_contract"),
				{
					TEXT("direct_mcp_runtime_gate=true"),
					TEXT("requested_lane=bounded_plugin_mutation"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("This representative high-risk execution surface now has a real direct MCP policy-deny contract on the bounded lane instead of relying only on provider prompt-dispatch denial semantics."));
		};

		auto MakeConfiguredDefaultExternalUiDenied = [&DefaultRuntimeManifest](const FString& SurfaceKey)
		{
			return MakeRiskySurfaceLaneBehavior(
				DefaultRuntimeManifest,
				TEXT("direct_policy_denied_external_ui_control_surface"),
				TEXT("workspace_write_project.external_ui_control_surface_denied"),
				true,
				TEXT("direct_policy_deny_contract"),
				{
					TEXT("direct_mcp_runtime_gate=true"),
					TEXT("requested_lane=workspace_write_project"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("This external UI control surface now has a real direct MCP policy-deny contract on the workspace-write default lane instead of being described only through disabled bridge posture."));
		};

		auto MakeBoundedExternalUiDenied = [&BoundedRuntimeManifest](const FString& SurfaceKey)
		{
			return MakeRiskySurfaceLaneBehavior(
				BoundedRuntimeManifest,
				TEXT("direct_policy_denied_external_ui_control_surface"),
				TEXT("bounded_plugin_mutation.external_ui_control_surface_denied"),
				true,
				TEXT("direct_policy_deny_contract"),
				{
					TEXT("direct_mcp_runtime_gate=true"),
					TEXT("requested_lane=bounded_plugin_mutation"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("This external UI control surface now has a real direct MCP policy-deny contract on the bounded lane instead of relying only on provider prompt-dispatch denial semantics."));
		};

		auto MakeBoundedGovernedAllowed = [&BoundedRuntimeManifest, BoundedRow](const FString& SurfaceKey, const FString& FamilyId)
		{
			TArray<FString> Basis = {
				FString::Printf(TEXT("surface=%s"), *SurfaceKey),
				FString::Printf(TEXT("governing_family=%s"), *FamilyId),
				TEXT("provider_prompt_dispatch_denied_for_non_allow_list_families=true")
			};

			if (BoundedRow)
			{
				for (const FString& AllowedFamily : BoundedRow->AllowedMutationFamilies)
				{
					Basis.Add(FString::Printf(TEXT("allowed_family=%s"), *AllowedFamily));
				}
			}

			return MakeRiskySurfaceLaneBehavior(
				BoundedRuntimeManifest,
				TEXT("governed_family_allowed"),
				TEXT(""),
				false,
				TEXT("described_governed_family_readback"),
				Basis,
				TEXT("This surface is one of the currently accepted bounded plugin-mutation families and remains callable on the bounded lane without widening direct provider file/shell power."));
		};

		auto MakeExpertReachable = [&ExplicitExpertManifest](const FString& SurfaceKey, const FString& BehaviorState)
		{
			return MakeRiskySurfaceLaneBehavior(
				ExplicitExpertManifest,
				BehaviorState,
				TEXT(""),
				false,
				TEXT("explicit_expert_selection_boundary"),
				{
					TEXT("explicit_opt_in_required=true"),
					TEXT("unreal_mcp_bridge_enabled=true"),
					FString::Printf(TEXT("surface=%s"), *SurfaceKey)
				},
				TEXT("This surface is only reachable from the explicit expert opt-in runtime, which keeps high-risk provider power available by deliberate selection."));
		};

		auto MakeLaneSurfaceBehavior = [](const FAgentProviderExecutionControlManifest& Manifest, const FString& BehaviorState, const FString& TruthBoundary)
		{
			return MakeRiskySurfaceLaneBehavior(
				Manifest,
				BehaviorState,
				TEXT(""),
				false,
				TEXT("described_manifest_readback"),
				{
					FString::Printf(TEXT("control_profile_id=%s"), *Manifest.ControlProfileId),
					FString::Printf(TEXT("runtime_lane=%s"), AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane))
				},
				TruthBoundary);
		};

		TMap<FString, TArray<FString>> BacklogBucketMap;
		auto AddEntry = [&Coverage, &BacklogBucketMap, &ToolMappingManifest](
			const FString& ToolName,
			const FString& SurfaceIdentifier,
			const FString& SurfaceKind,
			const FString& RiskFocus,
			const FString& GovernanceState,
			const FString& GovernanceBucketId,
			const FString& GovernanceBucketDisplayName,
			const FString& MutationClass,
			const FString& RiskClass,
			const FAgentCapabilityRiskySurfaceLaneBehavior& ConfiguredDefaultRuntime,
			const FAgentCapabilityRiskySurfaceLaneBehavior& BoundedPluginMutation,
			const FAgentCapabilityRiskySurfaceLaneBehavior& ExplicitExpertOptIn,
			const FString& TruthBoundary)
		{
			const FAgentCapabilityRiskToolMapping* Mapping = FindToolMapping(ToolMappingManifest, ToolName, SurfaceIdentifier);

			FAgentCapabilityRiskySurfaceEntry Entry;
			Entry.ToolName = ToolName;
			Entry.SurfaceIdentifier = SurfaceIdentifier;
			Entry.SurfaceKind = SurfaceKind;
			Entry.RiskFocus = RiskFocus;
			Entry.GovernanceState = GovernanceState;
			Entry.GovernanceBucketId = GovernanceBucketId;
			Entry.GovernanceBucketDisplayName = GovernanceBucketDisplayName;
			Entry.MappingScope = Mapping ? Mapping->MappingScope : TEXT("tool");
			Entry.FamilyMappingState = Mapping ? Mapping->AmbiguityState : TEXT("unmapped_backlog");
			Entry.GoverningFamilyId = Mapping ? Mapping->GoverningFamilyId : FString();
			Entry.GoverningFamilyDisplayName = Mapping ? Mapping->GoverningFamilyDisplayName : FString();
			Entry.MutationClass = Mapping ? Mapping->MutationClass : MutationClass;
			Entry.RiskClass = Mapping ? Mapping->RiskClass : RiskClass;
			Entry.bSafeOrBoundedBehaviorExplicit = true;
			Entry.ConfiguredDefaultRuntime = ConfiguredDefaultRuntime;
			Entry.BoundedPluginMutation = BoundedPluginMutation;
			Entry.ExplicitExpertOptIn = ExplicitExpertOptIn;
			Entry.TruthBoundary = TruthBoundary;
			Coverage.Surfaces.Add(MoveTemp(Entry));

			const FString SurfaceKey = MakeRiskySurfaceKey(ToolName, SurfaceIdentifier);
			if (GovernanceState == TEXT("family_mapped_surface"))
			{
				Coverage.FamilyMappedSurfaceKeys.Add(SurfaceKey);
			}
			else
			{
				Coverage.ExplicitBacklogSurfaceKeys.Add(SurfaceKey);
				BacklogBucketMap.FindOrAdd(GovernanceBucketId).Add(SurfaceKey);
			}
		};

		AddEntry(
			TEXT("plugin_settings"),
			TEXT("assistant_backend.execution_profiles.bounded_plugin_mutation"),
			TEXT("tool_surface"),
			TEXT("mutation_capable_surface"),
			TEXT("family_mapped_surface"),
			TEXT("mapped_execution_surface"),
			TEXT("Mapped Execution Surfaces"),
			TEXT("bounded_runtime_profile_surface"),
			TEXT("medium_controlled_mutation_runtime_surface"),
			MakeLaneSurfaceBehavior(
				DefaultRuntimeManifest,
				TEXT("safer_default_read_only_profile_selected"),
				TEXT("The configured workspace-write default runtime is distinct from the bounded runtime lane and does not silently enter that governed mutation surface.")),
			MakeLaneSurfaceBehavior(
				BoundedRuntimeManifest,
				TEXT("current_governed_mutation_profile_available"),
				TEXT("This is the accepted bounded runtime profile surface for governed plugin mutation.")),
			MakeLaneSurfaceBehavior(
				ExplicitExpertManifest,
				TEXT("separate_explicit_expert_profile_required"),
				TEXT("Expert/high-risk execution remains a different profile and does not collapse back into the bounded runtime surface.")),
			TEXT("This entry keeps the bounded mutation runtime surface visible in the same governance coverage view as risky tools, without relabeling it as unrestricted provider power."));

		AddEntry(
			TEXT("plugin_settings"),
			TEXT("assistant_backend.execution_profiles.explicit_expert_opt_in"),
			TEXT("tool_surface"),
			TEXT("high_risk_execution_surface"),
			TEXT("family_mapped_surface"),
			TEXT("mapped_execution_surface"),
			TEXT("Mapped Execution Surfaces"),
			TEXT("explicit_high_risk_runtime_profile_surface"),
			TEXT("high_risk_execution_surface"),
			MakeLaneSurfaceBehavior(
				DefaultRuntimeManifest,
				TEXT("expert_opt_in_required_not_current_default"),
				TEXT("The configured default runtime is no longer the expert lane and must not silently present expert power as active.")),
			MakeLaneSurfaceBehavior(
				BoundedRuntimeManifest,
				TEXT("expert_opt_in_required_not_bounded_lane"),
				TEXT("The bounded runtime is separate from expert/high-risk execution and cannot silently widen into it.")),
			MakeLaneSurfaceBehavior(
				ExplicitExpertManifest,
				TEXT("active_high_risk_execution_surface"),
				TEXT("This is the accepted explicit expert opt-in surface for the high-risk provider runtime.")),
			TEXT("This entry keeps the explicit high-risk execution lane mapped and visible so the expert boundary stays machine-readable instead of implicit."));

		AddEntry(
			TEXT("blueprint_modify"),
			TEXT(""),
			TEXT("tool"),
			TEXT("mutation_capable_surface"),
			TEXT("family_mapped_surface"),
			TEXT("mapped_mutation_surface"),
			TEXT("Mapped Mutation Surfaces"),
			TEXT("broad_asset_authoring_mutation"),
			TEXT("high_broad_authoring_mutation"),
			MakeConfiguredDefaultMutationBlocked(TEXT("blueprint_modify")),
			MakeBoundedDenied(TEXT("blueprint_modify")),
			MakeExpertReachable(TEXT("blueprint_modify"), TEXT("reachable_via_enabled_unreal_mcp_bridge")),
			TEXT("Blueprint mutation is family-mapped, but it is still too broad for the current bounded allow list and remains expert-only from provider runtime."));

		AddEntry(
			TEXT("anim_blueprint_modify"),
			TEXT(""),
			TEXT("tool"),
			TEXT("mutation_capable_surface"),
			TEXT("family_mapped_surface"),
			TEXT("mapped_mutation_surface"),
			TEXT("Mapped Mutation Surfaces"),
			TEXT("broad_asset_authoring_mutation"),
			TEXT("high_broad_authoring_mutation"),
			MakeConfiguredDefaultMutationBlocked(TEXT("anim_blueprint_modify")),
			MakeBoundedDenied(TEXT("anim_blueprint_modify")),
			MakeExpertReachable(TEXT("anim_blueprint_modify"), TEXT("reachable_via_enabled_unreal_mcp_bridge")),
			TEXT("Anim Blueprint mutation is now explicitly mapped with the same governance posture as other broad Blueprint mutation surfaces."));

		AddEntry(
			TEXT("cpp_reflection"),
			TEXT(""),
			TEXT("tool"),
			TEXT("mutation_capable_surface"),
			TEXT("family_mapped_surface"),
			TEXT("mapped_mutation_surface"),
			TEXT("Mapped Mutation Surfaces"),
			TEXT("bounded_reflected_metadata_and_single_shape_declaration_mutation"),
			TEXT("medium_bounded_mutation_with_rebuild"),
			MakeConfiguredDefaultMutationBlocked(TEXT("cpp_reflection")),
			MakeBoundedGovernedAllowed(TEXT("cpp_reflection"), TEXT("cpp_reflected_contracts")),
			MakeExpertReachable(TEXT("cpp_reflection"), TEXT("reachable_via_enabled_unreal_mcp_bridge")),
			TEXT("cpp_reflection remains one of the accepted bounded mutation families and is distinct from unrestricted provider file/shell power."));

		AddEntry(
			TEXT("mutation_group"),
			TEXT(""),
			TEXT("tool"),
			TEXT("mutation_capable_surface"),
			TEXT("family_mapped_surface"),
			TEXT("mapped_mutation_surface"),
			TEXT("Mapped Mutation Surfaces"),
			TEXT("bounded_checkpointed_mutation"),
			TEXT("medium_bounded_mutation_with_checkpoint_revert"),
			MakeConfiguredDefaultMutationBlocked(TEXT("mutation_group")),
			MakeBoundedGovernedAllowed(TEXT("mutation_group"), TEXT("mutation_group_revert")),
			MakeExpertReachable(TEXT("mutation_group"), TEXT("reachable_via_enabled_unreal_mcp_bridge")),
			TEXT("mutation_group remains the checkpointed bounded mutation family and is one of the explicit governed surfaces on the bounded lane."));

		for (const FString& ToolName : {
			FString(TEXT("spawn_actor")),
			FString(TEXT("delete_actors")),
			FString(TEXT("move_actor")),
			FString(TEXT("set_property")),
			FString(TEXT("asset")),
			FString(TEXT("character")),
			FString(TEXT("character_data")),
			FString(TEXT("enhanced_input")),
			FString(TEXT("material"))
		})
		{
			AddEntry(
				ToolName,
				TEXT(""),
				TEXT("tool"),
				TEXT("mutation_capable_surface"),
				TEXT("explicit_unmapped_backlog"),
				TEXT("broad_authoring_mutation_backlog"),
				TEXT("Broad Authoring Mutation Backlog"),
				TEXT("unmapped_broad_authoring_mutation"),
				TEXT("high_mutation_backlog"),
				MakeConfiguredDefaultBroadMutationDenied(ToolName),
				MakeBoundedBroadMutationDenied(ToolName),
				MakeExpertReachable(ToolName, TEXT("reachable_via_enabled_unreal_mcp_bridge")),
				TEXT("This mutation-capable surface is still backlog/unmapped at the family layer, but the safer and bounded runtime lanes now hard-deny the full broad authoring mutation backlog bucket at tool-surface granularity instead of silently exposing it."));
		}

		for (const FString& ToolName : {
			FString(TEXT("execute_script")),
			FString(TEXT("run_console_command")),
			FString(TEXT("task_submit"))
		})
		{
			AddEntry(
				ToolName,
				TEXT(""),
				TEXT("tool"),
				TEXT("high_risk_execution_surface"),
				TEXT("explicit_unmapped_backlog"),
				TEXT("high_risk_execution_backlog"),
				TEXT("High-Risk Execution Backlog"),
				TEXT("unmapped_high_risk_execution_surface"),
				TEXT("high_risk_execution_backlog"),
				(ToolName == TEXT("execute_script") || ToolName == TEXT("run_console_command") || ToolName == TEXT("task_submit"))
					? MakeConfiguredDefaultRepresentativeHighRiskDenied(ToolName)
					: MakeConfiguredDefaultNotExposed(ToolName),
				(ToolName == TEXT("execute_script") || ToolName == TEXT("run_console_command") || ToolName == TEXT("task_submit"))
					? MakeBoundedRepresentativeHighRiskDenied(ToolName)
					: MakeBoundedDenied(ToolName),
				MakeExpertReachable(ToolName, TEXT("reachable_high_risk_execution_surface")),
				TEXT("This high-risk execution surface remains backlog/unmapped, but the safer default and bounded lanes keep it blocked or denied instead of silently reusing expert execution."));
		}

		for (const FString& ToolName : {
			FString(TEXT("osvayder_mouse_click")),
			FString(TEXT("osvayder_mouse_double_click")),
			FString(TEXT("osvayder_mouse_move")),
			FString(TEXT("osvayder_mouse_drag")),
			FString(TEXT("osvayder_mouse_scroll")),
			FString(TEXT("osvayder_keyboard_type")),
			FString(TEXT("osvayder_keyboard_hotkey")),
			FString(TEXT("osvayder_keyboard_press")),
			FString(TEXT("osvayder_focus_window"))
		})
		{
			AddEntry(
				ToolName,
				TEXT(""),
				TEXT("tool"),
				TEXT("high_risk_execution_surface"),
				TEXT("explicit_unmapped_backlog"),
				TEXT("external_ui_control_backlog"),
				TEXT("External UI Control Backlog"),
				TEXT("unmapped_external_ui_control"),
				TEXT("high_external_ui_control_backlog"),
				IsExternalUiControlBacklogTool(ToolName)
					? MakeConfiguredDefaultExternalUiDenied(ToolName)
					: MakeConfiguredDefaultNotExposed(ToolName),
				IsExternalUiControlBacklogTool(ToolName)
					? MakeBoundedExternalUiDenied(ToolName)
					: MakeBoundedDenied(ToolName),
				MakeExpertReachable(ToolName, TEXT("reachable_via_enabled_unreal_mcp_bridge")),
				TEXT("These external UI control surfaces remain explicitly backlog/unmapped, but they are not ambiguous on safer lanes: read-only does not expose them and bounded runtime denies generic provider dispatch."));
		}

		Coverage.Surfaces.Sort([](const FAgentCapabilityRiskySurfaceEntry& A, const FAgentCapabilityRiskySurfaceEntry& B)
		{
			return MakeRiskySurfaceKey(A.ToolName, A.SurfaceIdentifier) < MakeRiskySurfaceKey(B.ToolName, B.SurfaceIdentifier);
		});

		SortAndDeduplicateStrings(Coverage.FamilyMappedSurfaceKeys);
		SortAndDeduplicateStrings(Coverage.ExplicitBacklogSurfaceKeys);

		TArray<FString> SortedBucketIds;
		BacklogBucketMap.GetKeys(SortedBucketIds);
		SortedBucketIds.Sort();
		for (const FString& BucketId : SortedBucketIds)
		{
			TArray<FString>& SurfaceKeys = BacklogBucketMap.FindChecked(BucketId);
			SortAndDeduplicateStrings(SurfaceKeys);

			FAgentCapabilityRiskySurfaceCoverageBucket Bucket;
			Bucket.BucketId = BucketId;
			Bucket.DisplayName = GetRiskySurfaceBacklogBucketDisplayName(BucketId);
			Bucket.SurfaceKeys = SurfaceKeys;
			Coverage.BacklogBuckets.Add(MoveTemp(Bucket));
		}

		Coverage.SurfaceCount = Coverage.Surfaces.Num();
		Coverage.FamilyMappedSurfaceCount = Coverage.FamilyMappedSurfaceKeys.Num();
		Coverage.ExplicitBacklogSurfaceCount = Coverage.ExplicitBacklogSurfaceKeys.Num();
		for (const FAgentCapabilityRiskySurfaceEntry& Entry : Coverage.Surfaces)
		{
			if (Entry.RiskFocus == TEXT("high_risk_execution_surface"))
			{
				++Coverage.HighRiskExecutionSurfaceCount;
			}
			else if (Entry.RiskFocus == TEXT("mutation_capable_surface"))
			{
				++Coverage.MutationCapableSurfaceCount;
			}

			if (Entry.bSafeOrBoundedBehaviorExplicit)
			{
				++Coverage.SaferLaneBehaviorExplicitCount;
			}

			if (Entry.ConfiguredDefaultRuntime.bBehaviorEnforcedNow || Entry.BoundedPluginMutation.bBehaviorEnforcedNow)
			{
				++Coverage.SafeOrBoundedEnforcedSurfaceCount;
			}
		}

		Coverage.SaferLaneAmbiguousSurfaceCount = Coverage.SurfaceCount - Coverage.SaferLaneBehaviorExplicitCount;
		Coverage.SafeOrBoundedDescribedOnlySurfaceCount = Coverage.SurfaceCount - Coverage.SafeOrBoundedEnforcedSurfaceCount;
		return Coverage;
	}

	FAgentCppScopeDefinitionManifest BuildCppScopeDefinitionManifest()
	{
		FAgentCppScopeDefinitionManifest ScopeDefinition;
		ScopeDefinition.TruthBoundary =
			TEXT("This scope definition states what counts as accepted C++ authoring now, what is only a future target, and what remains explicitly forbidden. ")
			TEXT("It is a governance/readback layer and does not itself enable broader native-source power.");

		auto AddCategory = [](
			TArray<FAgentCppScopeCategoryManifest>& Categories,
			const FString& CategoryId,
			const FString& DisplayName,
			const FString& ScopeState,
			const FString& CurrentAvailability,
			const FString& Purpose,
			const FString& MutationBreadth,
			const FString& RequiredProofTier,
			const FString& RebuildExpectation,
			const FString& RevertExpectation,
			const FString& CheckpointExpectation,
			const FString& StopCondition,
			const bool bRequiresFreshBuildForAcceptance,
			const bool bRequiresRepresentativeLiveProof,
			const bool bRequiresRestoreOrRevertForMutation,
			const TArray<FString>& RepresentativeSurfaces,
			const TArray<FString>& CurrentLaneMappings,
			const TArray<FString>& CurrentRealityBasis,
			const FString& TruthBoundary)
		{
			FAgentCppScopeCategoryManifest Category;
			Category.CategoryId = CategoryId;
			Category.DisplayName = DisplayName;
			Category.ScopeState = ScopeState;
			Category.CurrentAvailability = CurrentAvailability;
			Category.Purpose = Purpose;
			Category.MutationBreadth = MutationBreadth;
			Category.RequiredProofTier = RequiredProofTier;
			Category.RebuildExpectation = RebuildExpectation;
			Category.RevertExpectation = RevertExpectation;
			Category.CheckpointExpectation = CheckpointExpectation;
			Category.StopCondition = StopCondition;
			Category.bRequiresFreshBuildForAcceptance = bRequiresFreshBuildForAcceptance;
			Category.bRequiresRepresentativeLiveProof = bRequiresRepresentativeLiveProof;
			Category.bRequiresRestoreOrRevertForMutation = bRequiresRestoreOrRevertForMutation;
			Category.RepresentativeSurfaces = RepresentativeSurfaces;
			Category.CurrentLaneMappings = CurrentLaneMappings;
			Category.CurrentRealityBasis = CurrentRealityBasis;
			Category.TruthBoundary = TruthBoundary;
			Categories.Add(MoveTemp(Category));
		};

		AddCategory(
			ScopeDefinition.AllowedNow,
			TEXT("reflected_contract_discovery"),
			TEXT("Reflected Contract Discovery"),
			TEXT("allowed_now"),
			TEXT("accepted_live_bounded"),
			TEXT("Read reflected native UCLASS/USTRUCT/UENUM contracts and bounded reflected member metadata under loaded /Script modules."),
			TEXT("read_only_reflected_discovery"),
			TEXT("fresh_build_automation_plus_live_readback"),
			TEXT("not_required_per_query"),
			TEXT("not_applicable_read_only"),
			TEXT("not_applicable"),
			TEXT("stop_and_escalate_if discovery claims full arbitrary C++ parsing or authoring beyond reflected contract data"),
			true,
			true,
			false,
			{
				TEXT("cpp_reflection.list_reflected_contracts"),
				TEXT("cpp_reflection.get_reflected_contract")
			},
			{
				TEXT("cpp_reflection.list_reflected_contracts"),
				TEXT("cpp_reflection.get_reflected_contract")
			},
			{
				TEXT("current read path is reflected /Script contract discovery only"),
				TEXT("practical proof is currently anchored to plugin module UnrealClaude"),
				TEXT("host project has no root Source module in this round")
			},
			TEXT("Allowed now means bounded reflected discovery only. It does not imply full source understanding or arbitrary native code editing."));

		AddCategory(
			ScopeDefinition.AllowedNow,
			TEXT("bounded_reflected_metadata_mutation"),
			TEXT("Bounded Reflected Metadata Mutation"),
			TEXT("allowed_now"),
			TEXT("accepted_live_bounded"),
			TEXT("Perform one narrow plugin-owned reflected UPROPERTY metadata upsert lane with preview/apply and truthful rebuild-based proof."),
			TEXT("bounded_plugin_owned_header_metadata_upsert"),
			TEXT("fresh_build_automation_plus_live_preview_apply_rebuild_readback"),
			TEXT("fresh_preflight_rebuild_required_after_apply"),
			TEXT("manual_restore_or_mutation_group_revert"),
			TEXT("receipt_required_checkpoint_optional_via_mutation_group"),
			TEXT("stop_and_escalate_if request exceeds plugin-owned reflected UPROPERTY metadata upsert or if rebuild proof has not completed"),
			true,
			true,
			true,
			{
				TEXT("cpp_reflection.preview_property_metadata_mutation"),
				TEXT("cpp_reflection.apply_property_metadata_mutation"),
				TEXT("mutation_group.preview_group"),
				TEXT("mutation_group.apply_group"),
				TEXT("mutation_group.revert_group")
			},
			{
				TEXT("cpp_reflection.preview_property_metadata_mutation"),
				TEXT("cpp_reflection.apply_property_metadata_mutation"),
				TEXT("mutation_group.preview_group"),
				TEXT("mutation_group.apply_group"),
				TEXT("mutation_group.revert_group")
			},
			{
				TEXT("current write lane is plugin-owned reflected UPROPERTY metadata only"),
				TEXT("allowed metadata keys remain bounded to DisplayName ToolTip ClampMin ClampMax UIMin UIMax MultiLine"),
				TEXT("arbitrary .cpp implementation-body editing is outside the accepted lane")
			},
			TEXT("Allowed now does not mean broad C++ mutation. The accepted lane remains bounded, plugin-owned, reflected, and rebuild-gated."));

		AddCategory(
			ScopeDefinition.AllowedNow,
			TEXT("compile_diagnostic_rebuild_handshake"),
			TEXT("Compile/Diagnostic/Rebuild Handshake"),
			TEXT("allowed_now"),
			TEXT("accepted_live_bounded"),
			TEXT("Expose truthful build freshness, rebuild-required state, and restore/revert expectations around the current bounded reflected mutation lane."),
			TEXT("build_validation_and_restore_control"),
			TEXT("fresh_build_automation_plus_live_apply_rebuild_readback"),
			TEXT("fresh_preflight_rebuild_required_for_acceptance"),
			TEXT("restore_or_revert_expected_before_closeout"),
			TEXT("required_for_grouped_lane"),
			TEXT("stop_and_escalate_if in-process compile is implied as accepted proof or if mutation closeout skips rebuild/restore truth"),
			true,
			true,
			true,
			{
				TEXT("cpp_reflection.apply_reflected_property_declaration.compile_handshake"),
				TEXT("cpp_reflection.revert_reflected_property_declaration.compile_handshake"),
				TEXT("cpp_reflection.inspect_reflected_property_declaration_build_failure"),
				TEXT("cpp_reflection.apply_property_metadata_mutation.compile_handshake"),
				TEXT("mutation_group.apply_group"),
				TEXT("mutation_group.revert_group"),
				TEXT("plugin_settings.assistant_backend.plugin_build_fresh")
			},
			{
				TEXT("cpp_reflection.apply_reflected_property_declaration"),
				TEXT("cpp_reflection.revert_reflected_property_declaration"),
				TEXT("cpp_reflection.inspect_reflected_property_declaration_build_failure"),
				TEXT("cpp_reflection.apply_property_metadata_mutation"),
				TEXT("mutation_group.apply_group"),
				TEXT("mutation_group.revert_group"),
				TEXT("plugin_settings.assistant_backend.plugin_build_fresh")
			},
			{
				TEXT("supports_in_process_reflected_compile=false remains accepted truth"),
				TEXT("fresh rebuild is required before reflected readback is accepted proof"),
				TEXT("single-shape declaration apply now has first-class receipt-backed restore/revert closeout"),
				TEXT("receipt-linked failed-build diagnostics are now inspectable without widening declaration breadth")
			},
			TEXT("Allowed now means truthful compile/diagnostic/rebuild governance around the current bounded lane, not general native compilation autonomy."));

		AddCategory(
			ScopeDefinition.FutureTarget,
			TEXT("structured_reflected_declaration_authoring"),
			TEXT("Structured Reflected Declaration Authoring"),
			TEXT("future_target"),
			TEXT("single_shape_apply_foundation_live"),
			TEXT("Extend the current metadata-only lane toward safe reflected declaration authoring for properties, functions, and UCLASS/USTRUCT/UENUM contract changes."),
			TEXT("contract_safe_reflected_declaration_authoring"),
			TEXT("preview_apply_restore_rebuild_readback_required"),
			TEXT("fresh_preflight_rebuild_required_after_apply"),
			TEXT("first_class_restore_expected"),
			TEXT("required_before_apply"),
			TEXT("stop_and_escalate_if declared as generally available before contract-safe preview/apply/restore boundaries exist"),
			true,
			true,
			true,
			{
				TEXT("cpp_reflection.preview_reflected_property_declaration"),
				TEXT("cpp_reflection.apply_reflected_property_declaration"),
				TEXT("cpp_reflection.revert_reflected_property_declaration"),
				TEXT("future.cpp_reflection.add_reflected_property"),
				TEXT("future.cpp_reflection.add_reflected_function"),
				TEXT("future.cpp_reflection.update_reflected_contract")
			},
			{
				TEXT("cpp_reflection.preview_reflected_property_declaration"),
				TEXT("cpp_reflection.apply_reflected_property_declaration"),
				TEXT("cpp_reflection.revert_reflected_property_declaration"),
				TEXT("extends bounded_reflected_metadata_mutation")
			},
			{
				TEXT("documented as future target in 475_PostULTRA_CppProgramDraft.md"),
				TEXT("current build exposes one preview/apply/revert reflected bool property declaration foundation"),
				TEXT("current hardened runtime keeps that live foundation on bounded_plugin_mutation instead of the workspace-write default or read-only helper"),
				TEXT("broader reflected declaration categories remain future work")
			},
			TEXT("Future target in breadth only. One bounded preview/apply/revert bool-property declaration foundation is live, but broader reflected declaration authoring is not generally available today."));

		AddCategory(
			ScopeDefinition.FutureTarget,
			TEXT("checkpointed_reflected_contract_batching"),
			TEXT("Checkpointed Reflected Contract Batching"),
			TEXT("future_target"),
			TEXT("planned_not_broadly_enabled"),
			TEXT("Allow richer grouped reflected-contract authoring with explicit checkpoint, restore, and rebuild closeout semantics."),
			TEXT("bounded_checkpointed_contract_batching"),
			TEXT("preview_apply_revert_rebuild_live_readback_required"),
			TEXT("fresh_preflight_rebuild_required_after_apply"),
			TEXT("first_class_checkpoint_revert_required"),
			TEXT("required_before_apply"),
			TEXT("stop_and_escalate_if grouped native authoring widens before stronger control and restore guarantees are in place"),
			true,
			true,
			true,
			{
				TEXT("future.mutation_group.reflected_contract_batch"),
				TEXT("future.cpp_reflection.contract_batch_preview")
			},
			{
				TEXT("builds on current mutation_group bounded lane")
			},
			{
				TEXT("mutation_group is currently limited to cpp_property_metadata_upsert"),
				TEXT("broader reflected contract batching is not yet accepted")
			},
			TEXT("Future target only. Current grouped mutation support does not yet imply broad reflected declaration batching."));

		AddCategory(
			ScopeDefinition.ForbiddenNow,
			TEXT("arbitrary_cpp_implementation_body_editing"),
			TEXT("Arbitrary C++ Implementation-Body Editing"),
			TEXT("forbidden_now"),
			TEXT("explicitly_forbidden"),
			TEXT("Free-form or broad `.cpp` implementation-body editing as a normal product lane."),
			TEXT("arbitrary_native_source_editing"),
			TEXT("not_accepted"),
			TEXT("not_permitted"),
			TEXT("not_applicable"),
			TEXT("not_applicable"),
			TEXT("stop_and_escalate_immediately_if requested_as_normal_lane"),
			false,
			false,
			false,
			{
				TEXT("none")
			},
			{
				TEXT("forbidden_now_no_current_lane")
			},
			{
				TEXT("475_PostULTRA_CppProgramDraft.md explicitly forbids broad arbitrary .cpp editing"),
				TEXT("current accepted CF1 lane is metadata-only and rebuild-gated")
			},
			TEXT("Forbidden now means the product must not imply that arbitrary implementation-body editing is accepted, structured, or generally available."));

		AddCategory(
			ScopeDefinition.ForbiddenNow,
			TEXT("broad_native_autonomous_refactor"),
			TEXT("Broad Native Autonomous Refactor"),
			TEXT("forbidden_now"),
			TEXT("explicitly_forbidden"),
			TEXT("Large-scale autonomous native refactors, free-form repairs, or broad codebase mutation without stronger control and restore guarantees."),
			TEXT("broad_native_autonomous_mutation"),
			TEXT("not_accepted"),
			TEXT("not_permitted"),
			TEXT("not_applicable"),
			TEXT("not_applicable"),
			TEXT("stop_and_escalate_immediately_if requested_without stronger control tightening"),
			false,
			false,
			false,
			{
				TEXT("none")
			},
			{
				TEXT("forbidden_now_no_current_lane")
			},
			{
				TEXT("post-ULTRA track requires control tightening before C++ breadth increase"),
				TEXT("current mutation support remains bounded and reviewable")
			},
			TEXT("Forbidden now means no broad autonomous native-code refactor lane should be marketed or implied from the current build."));

		AddCategory(
			ScopeDefinition.ForbiddenNow,
			TEXT("host_project_cpp_gameplay_mutation_outside_plugin_scope"),
			TEXT("Host-Project C++ Gameplay Mutation Outside Plugin Scope"),
			TEXT("forbidden_now"),
			TEXT("explicitly_forbidden"),
			TEXT("Direct host-project gameplay/module mutation outside the accepted plugin-only post-ULTRA scope definition slices."),
			TEXT("out_of_scope_host_project_native_mutation"),
			TEXT("not_accepted"),
			TEXT("not_permitted"),
			TEXT("not_applicable"),
			TEXT("not_applicable"),
			TEXT("stop_and_escalate_if packet scope would leave Plugins/UnrealClaude or imply host-project native editing"),
			false,
			false,
			false,
			{
				TEXT("none")
			},
			{
				TEXT("forbidden_now_no_current_lane")
			},
			{
				TEXT("current post-ULTRA slices are plugin-only"),
				TEXT("host project currently has no root Source module in this repo round")
			},
			TEXT("Forbidden now means this slice does not authorize host-project gameplay C++ mutation outside the accepted plugin-only scope."));

		return ScopeDefinition;
	}
}

const TCHAR* AgentCapabilityFamilyModeToString(const EAgentCapabilityFamilyMode Mode)
{
	switch (Mode)
	{
	case EAgentCapabilityFamilyMode::ReadOnly:
		return TEXT("read_only");

	case EAgentCapabilityFamilyMode::MutationCapable:
		return TEXT("mutation_capable");

	case EAgentCapabilityFamilyMode::Mixed:
	default:
		return TEXT("mixed");
	}
}

FAgentCapabilityRiskManifest BuildAgentCapabilityRiskManifest(
	const FAgentProviderExecutionControlManifest& DefaultRuntimeManifest,
	const FAgentProviderExecutionControlManifest& ReadOnlyHelperManifest,
	const FAgentProviderExecutionControlManifest& BoundedRuntimeManifest,
	const FAgentProviderExecutionControlManifest& ExplicitExpertManifest)
{
	FAgentCapabilityRiskManifest Manifest;
	Manifest.TruthBoundary =
		TEXT("This manifest is a family-level governance/classification foundation derived from current plugin reality. ")
		TEXT("It summarizes family posture, proof depth, and control dependencies, but it is not yet a per-tool policy engine or a hard-enforcement layer.");

	auto AddFamily = [&Manifest](FAgentCapabilityRiskFamilyManifest&& Family)
	{
		Manifest.Families.Add(MoveTemp(Family));
	};

	{
		FAgentCapabilityRiskFamilyManifest Family;
		Family.FamilyId = TEXT("provider_control_status");
		Family.DisplayName = TEXT("Provider Control Status");
		Family.Purpose = TEXT("Truthful readback of backend status, execution-control posture, helper-lane control split, and local session/history persistence state.");
		Family.CapabilityMode = EAgentCapabilityFamilyMode::ReadOnly;
		Family.MutationClass = TEXT("none_read_only");
		Family.RiskClass = TEXT("medium_runtime_control_truth");
		Family.RequiredProofTier = TEXT("fresh_build_automation_plus_live_readback");
		Family.ReverificationExpectation = TEXT("mandatory_on_runtime_control_or_readback_change");
		Family.Revertability = TEXT("not_applicable_read_only");
		Family.CheckpointExpectation = TEXT("not_applicable");
		Family.StopCondition = TEXT("stop_and_escalate_if_manifest_or_readback_implies_stronger_enforcement_than_current_runtime_reality");
		Family.bRequiresFreshBuildForAcceptance = true;
		Family.bRequiresRepresentativeLiveProof = true;
		Family.RepresentativeSurfaces = {
			TEXT("plugin_settings.assistant_backend.provider_execution_control"),
			TEXT("plugin_settings.assistant_backend.execution_profiles"),
			TEXT("agent_trace.run_started"),
			TEXT("backend_run_json.execution_control_fields")
		};
		Family.MajorControlDependencies = {
			TEXT("AgentExecutionControl"),
			TEXT("AgentOrchestrator runtime config"),
			TEXT("plugin_settings readback"),
			TEXT("agent_trace readback")
		};
		Family.CurrentRealityBasis = {
			FString::Printf(TEXT("default_control_profile_id=%s"), *DefaultRuntimeManifest.ControlProfileId),
			FString::Printf(TEXT("default_current_power=%s"), AgentExecutionPowerClassToString(DefaultRuntimeManifest.CurrentEffectiveProviderPowerClass)),
			FString::Printf(TEXT("default_runtime_lane=%s"), AgentExecutionRuntimeLaneToString(DefaultRuntimeManifest.CurrentEffectiveRuntimeLane)),
			FString::Printf(TEXT("default_plumbing_state=%s"), AgentExecutionGovernanceStateToString(DefaultRuntimeManifest.ExecutionControlPlumbingState)),
			FString::Printf(TEXT("helper_control_profile_id=%s"), *ReadOnlyHelperManifest.ControlProfileId),
			FString::Printf(TEXT("helper_current_power=%s"), AgentExecutionPowerClassToString(ReadOnlyHelperManifest.CurrentEffectiveProviderPowerClass)),
			FString::Printf(TEXT("helper_session_persistence_mode=%s"), AgentSessionPersistenceModeToString(ReadOnlyHelperManifest.SessionPersistenceMode)),
			FString::Printf(TEXT("bounded_control_profile_id=%s"), *BoundedRuntimeManifest.ControlProfileId),
			FString::Printf(TEXT("bounded_runtime_lane=%s"), AgentExecutionRuntimeLaneToString(BoundedRuntimeManifest.CurrentEffectiveRuntimeLane)),
			FString::Printf(TEXT("expert_control_profile_id=%s"), *ExplicitExpertManifest.ControlProfileId),
			FString::Printf(TEXT("expert_current_power=%s"), AgentExecutionPowerClassToString(ExplicitExpertManifest.CurrentEffectiveProviderPowerClass))
		};
		Family.TruthBoundary =
			TEXT("The configured default runtime is now a real safer read-only lane, bounded plugin mutation remains a separate governed lane, and unrestricted high-risk provider power lives only behind explicit expert opt-in. ")
			TEXT("This family reports the currently accepted control split; it does not claim that every risky tool path is already fully hard-enforced.");
		AddFamily(MoveTemp(Family));
	}

	{
		FAgentCapabilityRiskFamilyManifest Family;
		Family.FamilyId = TEXT("report_evidence_trace");
		Family.DisplayName = TEXT("Report Evidence Trace");
		Family.Purpose = TEXT("Export, summarize, and re-read plugin-owned evidence artifacts and observable traces without mutating host gameplay/content.");
		Family.CapabilityMode = EAgentCapabilityFamilyMode::Mixed;
		Family.MutationClass = TEXT("plugin_internal_state_export");
		Family.RiskClass = TEXT("low_internal_state_evidence_contract");
		Family.RequiredProofTier = TEXT("fresh_build_automation_plus_live_readback");
		Family.ReverificationExpectation = TEXT("mandatory_on_export_schema_or_trace_contract_change");
		Family.Revertability = TEXT("not_required_plugin_internal_state_only");
		Family.CheckpointExpectation = TEXT("not_applicable");
		Family.StopCondition = TEXT("stop_and_escalate_if_exported_artifacts_claim_stronger_proof_than_source_evidence_or_if_internal_state_writes_escape_plugin_owned_paths");
		Family.bWritesPluginOwnedState = true;
		Family.bRequiresFreshBuildForAcceptance = true;
		Family.bRequiresRepresentativeLiveProof = true;
		Family.RepresentativeSurfaces = {
			TEXT("report_export"),
			TEXT("report_artifact_status"),
			TEXT("agent_trace_status"),
			TEXT("execution_log_status")
		};
		Family.MajorControlDependencies = {
			TEXT("Saved/UnrealClaude/Reports"),
			TEXT("Saved/UnrealClaude/agent_trace.jsonl"),
			TEXT("Saved/UnrealClaude/diagnostic_trace.jsonl"),
			TEXT("report_artifact contract")
		};
		Family.CurrentRealityBasis = {
			TEXT("report_export writes plugin-owned reports only"),
			TEXT("agent_trace_status is observable-trace readback only"),
			TEXT("execution_log_status is receipt telemetry readback only")
		};
		Family.TruthBoundary =
			TEXT("This family may write plugin-owned evidence artifacts, but it is not a host-project mutation family. ")
			TEXT("Its truth contract is about evidence/export fidelity and bounded readback, not gameplay/content authoring.");
		AddFamily(MoveTemp(Family));
	}

	{
		FAgentCapabilityRiskFamilyManifest Family;
		Family.FamilyId = TEXT("runtime_proof");
		Family.DisplayName = TEXT("Runtime Proof");
		Family.Purpose = TEXT("Bounded runtime viability and OSS/session baseline proof without relabeling package presence or editor context as full gameplay/session proof.");
		Family.CapabilityMode = EAgentCapabilityFamilyMode::ReadOnly;
		Family.MutationClass = TEXT("none_read_only");
		Family.RiskClass = TEXT("medium_runtime_claim_read_only");
		Family.RequiredProofTier = TEXT("fresh_build_automation_plus_live_runtime_proof");
		Family.ReverificationExpectation = TEXT("mandatory_on_runtime_probe_contract_change");
		Family.Revertability = TEXT("not_applicable_read_only");
		Family.CheckpointExpectation = TEXT("not_applicable");
		Family.StopCondition = TEXT("stop_and_escalate_if_current_package_presence_or_editor_snapshot_is_marketed_as_gameplay_host_join_or_session_health_proof");
		Family.bRequiresFreshBuildForAcceptance = true;
		Family.bRequiresRepresentativeLiveProof = true;
		Family.RepresentativeSurfaces = {
			TEXT("map_runtime_proof"),
			TEXT("oss_session_proof")
		};
		Family.MajorControlDependencies = {
			TEXT("headless editor/runtime probe contract"),
			TEXT("current context snapshot contract"),
			TEXT("report_artifact export contract")
		};
		Family.CurrentRealityBasis = {
			TEXT("map_runtime_proof remains bounded to headless editor-load/runtime evidence"),
			TEXT("oss_session_proof remains bounded to configured baseline plus current-context snapshot")
		};
		Family.TruthBoundary =
			TEXT("This family proves bounded runtime facts only. ")
			TEXT("It does not erase the distinction between package presence, editor context, runtime-active state, and full gameplay/session proof.");
		AddFamily(MoveTemp(Family));
	}

	{
		FAgentCapabilityRiskFamilyManifest Family;
		Family.FamilyId = TEXT("dependency_metadata_truth");
		Family.DisplayName = TEXT("Dependency Metadata Truth");
		Family.Purpose = TEXT("Classify dependency/metadata/shipping truth from logs, declaration surfaces, cook surfaces, and current discovery without auto-repair.");
		Family.CapabilityMode = EAgentCapabilityFamilyMode::ReadOnly;
		Family.MutationClass = TEXT("none_read_only");
		Family.RiskClass = TEXT("medium_governance_read_only_analysis");
		Family.RequiredProofTier = TEXT("fresh_build_automation_plus_live_readback");
		Family.ReverificationExpectation = TEXT("mandatory_on_classifier_or_truth_bucket_change");
		Family.Revertability = TEXT("not_applicable_read_only");
		Family.CheckpointExpectation = TEXT("not_applicable");
		Family.StopCondition = TEXT("stop_and_escalate_if_historical_noise_is_presented_as_current_blocker_without_current_state_proof_or_if_runtime_impact_is_overclaimed");
		Family.bWritesPluginOwnedState = true;
		Family.bRequiresFreshBuildForAcceptance = true;
		Family.bRequiresRepresentativeLiveProof = true;
		Family.RepresentativeSurfaces = {
			TEXT("dependency_health"),
			TEXT("metadata_truth"),
			TEXT("report_artifact export for analytical findings")
		};
		Family.MajorControlDependencies = {
			TEXT("log corpus classification"),
			TEXT("asset registry/current discovery"),
			TEXT("shipping/cook metadata surfaces"),
			TEXT("report_artifact export contract")
		};
		Family.CurrentRealityBasis = {
			TEXT("dependency_health is recommendation-first and read-only"),
			TEXT("metadata_truth compares declaration vs shipping/current discovery and is read-only"),
			TEXT("both families can export plugin-owned reports without mutating host content")
		};
		Family.TruthBoundary =
			TEXT("This family explains current truth and recommendation posture; it is not yet an automatic repair family and not runtime gameplay proof.");
		AddFamily(MoveTemp(Family));
	}

	{
		FAgentCapabilityRiskFamilyManifest Family;
		Family.FamilyId = TEXT("blueprint_query");
		Family.DisplayName = TEXT("Blueprint Query");
		Family.Purpose = TEXT("Inspect Blueprint structure, graph state, defaults, properties, and related read-only authoring metadata.");
		Family.CapabilityMode = EAgentCapabilityFamilyMode::ReadOnly;
		Family.MutationClass = TEXT("none_read_only");
		Family.RiskClass = TEXT("low_read_only_introspection");
		Family.RequiredProofTier = TEXT("representative_live_query_smoke");
		Family.ReverificationExpectation = TEXT("mandatory_on_query_schema_change");
		Family.Revertability = TEXT("not_applicable_read_only");
		Family.CheckpointExpectation = TEXT("not_applicable");
		Family.StopCondition = TEXT("stop_and_escalate_if_query_surface_is_used_to_claim_mutation_or_runtime_behavior_without_separate_proof");
		Family.bRequiresRepresentativeLiveProof = true;
		Family.RepresentativeSurfaces = {
			TEXT("blueprint_query"),
			TEXT("get_class_defaults"),
			TEXT("get_editable_properties"),
			TEXT("get_widget_tree")
		};
		Family.MajorControlDependencies = {
			TEXT("reflection/introspection helpers"),
			TEXT("Blueprint editor query surfaces"),
			TEXT("asset load/read safety")
		};
		Family.CurrentRealityBasis = {
			TEXT("query surfaces are read-only"),
			TEXT("query surfaces still require truthful distinction between metadata and runtime proof")
		};
		Family.TruthBoundary =
			TEXT("This family reads editor/asset state only. ")
			TEXT("It does not mutate user-owned assets and it does not turn readback into runtime behavior proof.");
		AddFamily(MoveTemp(Family));
	}

	{
		FAgentCapabilityRiskFamilyManifest Family;
		Family.FamilyId = TEXT("blueprint_mutation");
		Family.DisplayName = TEXT("Blueprint Mutation");
		Family.Purpose = TEXT("Author and modify Blueprint graphs, components, defaults, and composed Blueprint-centered asset changes.");
		Family.CapabilityMode = EAgentCapabilityFamilyMode::MutationCapable;
		Family.MutationClass = TEXT("broad_asset_authoring_mutation");
		Family.RiskClass = TEXT("high_broad_authoring_mutation");
		Family.RequiredProofTier = TEXT("representative_live_mutation_smoke");
		Family.ReverificationExpectation = TEXT("mandatory_on_mutation_logic_or_scope_boundary_change");
		Family.Revertability = TEXT("manual_asset_or_source_recovery");
		Family.CheckpointExpectation = TEXT("not_yet_first_class");
		Family.StopCondition = TEXT("stop_and_escalate_on_scope_boundary_conflict_unclear_recovery_or_truth_mismatch_after_partial_apply");
		Family.bWritesUserOwnedState = true;
		Family.bRequiresRepresentativeLiveProof = true;
		Family.RepresentativeSurfaces = {
			TEXT("blueprint_modify"),
			TEXT("configure_actor_class"),
			TEXT("set_class_defaults"),
			TEXT("set_component_properties_batch"),
			TEXT("modify_collection")
		};
		Family.MajorControlDependencies = {
			TEXT("scope policy"),
			TEXT("execution receipts"),
			TEXT("Blueprint compile/validation path"),
			TEXT("PluginOnly vs PluginAndProject constraints")
		};
		Family.CurrentRealityBasis = {
			TEXT("mutation breadth is broad across Blueprint authoring surfaces"),
			TEXT("no first-class grouped checkpoint/revert contract exists for general Blueprint mutation"),
			TEXT("scope and receipt truthfulness are critical controls")
		};
		Family.TruthBoundary =
			TEXT("This family is materially broader and riskier than read-only introspection. ")
			TEXT("It should remain classified as high-risk broad authoring mutation until a tighter per-family enforcement/recovery story exists.");
		AddFamily(MoveTemp(Family));
	}

	{
		FAgentCapabilityRiskFamilyManifest Family;
		Family.FamilyId = TEXT("cpp_reflected_contracts");
		Family.DisplayName = TEXT("C++ Reflected Contracts");
		Family.Purpose = TEXT("Discover reflected native contracts, preview/apply/revert one narrow reflected bool property declaration shape, and perform one bounded plugin-owned UPROPERTY metadata mutation lane with truthful rebuild handshake.");
		Family.CapabilityMode = EAgentCapabilityFamilyMode::Mixed;
		Family.MutationClass = TEXT("bounded_reflected_metadata_and_single_shape_declaration_mutation");
		Family.RiskClass = TEXT("medium_bounded_mutation_with_rebuild");
		Family.RequiredProofTier = TEXT("fresh_build_automation_plus_live_preview_apply_rebuild_readback_revert");
		Family.ReverificationExpectation = TEXT("mandatory_on_reflection_or_mutation_contract_change");
		Family.Revertability = TEXT("single_shape_receipt_checkpoint_revert_available");
		Family.CheckpointExpectation = TEXT("receipt_required_checkpointed_for_single_shape_declaration_apply");
		Family.StopCondition = TEXT("stop_and_escalate_if_request_exceeds_plugin_owned reflected metadata or single-shape bool declaration lanes, or if rebuild/readback closeout has not completed");
		Family.bWritesUserOwnedState = true;
		Family.bRequiresFreshBuildForAcceptance = true;
		Family.bRequiresRepresentativeLiveProof = true;
		Family.RepresentativeSurfaces = {
			TEXT("cpp_reflection.list_reflected_contracts"),
			TEXT("cpp_reflection.get_reflected_contract"),
			TEXT("cpp_reflection.preview_reflected_property_declaration"),
			TEXT("cpp_reflection.apply_reflected_property_declaration"),
			TEXT("cpp_reflection.revert_reflected_property_declaration"),
			TEXT("cpp_reflection.inspect_reflected_property_declaration_build_failure"),
			TEXT("cpp_reflection.preview_property_metadata_mutation"),
			TEXT("cpp_reflection.apply_property_metadata_mutation")
		};
		Family.MajorControlDependencies = {
			TEXT("reflected UHT contract availability"),
			TEXT("PluginOnly scope policy"),
			TEXT("fresh rebuild handshake"),
			TEXT("mutation receipt path")
		};
		Family.CurrentRealityBasis = {
			TEXT("read path is reflected-contract discovery only"),
			TEXT("declaration authoring currently exposes one plugin-owned bool property preview/apply/revert foundation"),
			TEXT("workspace-write default/read-only helper now policy-deny the bounded reflected declaration preview/apply/revert/failed-build/evidence cycle"),
			TEXT("bounded_plugin_mutation remains the accepted governed runtime for that declaration lane, while explicit_expert_opt_in stays separate"),
			TEXT("failed-build closeout now includes one receipt-linked diagnostic inspection path for the same bounded declaration lane"),
			TEXT("write paths are bounded to plugin-owned reflected UPROPERTY metadata plus one single-shape bool declaration lane"),
			TEXT("in-process compile is not claimed; rebuild is required for accepted proof")
		};
		Family.TruthBoundary =
			TEXT("This family is not full arbitrary C++ authoring. ")
			TEXT("Its declaration lane is still bounded to one plugin-owned bool-property shape with receipt/checkpoint/revert closeout, while broader reflected declaration authoring remains future work.");
		AddFamily(MoveTemp(Family));
	}

	{
		FAgentCapabilityRiskFamilyManifest Family;
		Family.FamilyId = TEXT("mutation_group_revert");
		Family.DisplayName = TEXT("Mutation Group Revert");
		Family.Purpose = TEXT("Preview, apply, abort, and revert bounded grouped mutations with durable manifests, receipts, and checkpoints.");
		Family.CapabilityMode = EAgentCapabilityFamilyMode::MutationCapable;
		Family.MutationClass = TEXT("bounded_checkpointed_mutation");
		Family.RiskClass = TEXT("medium_bounded_mutation_with_checkpoint_revert");
		Family.RequiredProofTier = TEXT("fresh_build_automation_plus_live_preview_apply_revert");
		Family.ReverificationExpectation = TEXT("mandatory_on_group_state_machine_or_checkpoint_contract_change");
		Family.Revertability = TEXT("first_class_checkpoint_revert_available");
		Family.CheckpointExpectation = TEXT("required_before_apply");
		Family.StopCondition = TEXT("stop_and_escalate_if_grouped_mutation_lacks_truthful_checkpoint_revert_support_or_if_scope_drift_breaks_restoration");
		Family.bWritesUserOwnedState = true;
		Family.bWritesPluginOwnedState = true;
		Family.bRequiresFreshBuildForAcceptance = true;
		Family.bRequiresRepresentativeLiveProof = true;
		Family.bRequiresRevertOrCheckpointForMutation = true;
		Family.RepresentativeSurfaces = {
			TEXT("mutation_group.preview_group"),
			TEXT("mutation_group.apply_group"),
			TEXT("mutation_group.abort_group"),
			TEXT("mutation_group.revert_group")
		};
		Family.MajorControlDependencies = {
			TEXT("durable mutation group manifests/checkpoints"),
			TEXT("scope policy"),
			TEXT("underlying bounded mutation lanes"),
			TEXT("live restore verification")
		};
		Family.CurrentRealityBasis = {
			TEXT("current grouped lane is bounded to cpp_property_metadata_upsert"),
			TEXT("checkpoint artifacts and receipts are durable"),
			TEXT("live revert proof is part of accepted closeout")
		};
		Family.TruthBoundary =
			TEXT("This family is safer than broad mutation because it has first-class checkpoint/revert support, but it remains bounded to the mutation families that currently plug into it.");
		AddFamily(MoveTemp(Family));
	}

	Manifest.CppScopeDefinition = BuildCppScopeDefinitionManifest();
	Manifest.ToolMapping = BuildToolMappingManifest(Manifest);
	Manifest.ToolMapping.Coverage = BuildToolCoverageManifest(Manifest.ToolMapping);
	Manifest.RiskySurfaceCoverage = BuildRiskySurfaceCoverageManifest(
		Manifest.ToolMapping,
		DefaultRuntimeManifest,
		BoundedRuntimeManifest,
		ExplicitExpertManifest);
	return Manifest;
}

TSharedPtr<FJsonObject> MakeAgentCapabilityRiskManifestJson(const FAgentCapabilityRiskManifest& Manifest)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema_version"), Manifest.SchemaVersion);
	Object->SetStringField(TEXT("manifest_source"), Manifest.ManifestSource);
	Object->SetStringField(TEXT("classification_level"), Manifest.ClassificationLevel);
	Object->SetStringField(TEXT("scope_boundary"), Manifest.ScopeBoundary);
	Object->SetStringField(TEXT("truth_boundary"), Manifest.TruthBoundary);

	int32 ReadOnlyCount = 0;
	int32 MutationCapableCount = 0;
	int32 MixedCount = 0;
	int32 HighRiskCount = 0;
	int32 AmbiguousToolMappingCount = 0;
	int32 ReadOnlyToolMappingCount = 0;
	int32 MutationCapableToolMappingCount = 0;
	int32 MixedToolMappingCount = 0;
	int32 AllowedCppScopeCount = 0;
	int32 FutureTargetCppScopeCount = 0;
	int32 ForbiddenCppScopeCount = 0;

	TArray<TSharedPtr<FJsonValue>> FamilyObjects;
	TArray<TSharedPtr<FJsonValue>> RiskySurfaceObjects;
	TArray<TSharedPtr<FJsonValue>> RiskySurfaceBacklogBucketObjects;
	TArray<TSharedPtr<FJsonValue>> AllowedCppScopeObjects;
	TArray<TSharedPtr<FJsonValue>> FutureTargetCppScopeObjects;
	TArray<TSharedPtr<FJsonValue>> ForbiddenCppScopeObjects;
	TArray<TSharedPtr<FJsonValue>> ToolMappingObjects;
	TArray<TSharedPtr<FJsonValue>> CoverageBucketObjects;
	FamilyObjects.Reserve(Manifest.Families.Num());
	RiskySurfaceObjects.Reserve(Manifest.RiskySurfaceCoverage.Surfaces.Num());
	RiskySurfaceBacklogBucketObjects.Reserve(Manifest.RiskySurfaceCoverage.BacklogBuckets.Num());
	AllowedCppScopeObjects.Reserve(Manifest.CppScopeDefinition.AllowedNow.Num());
	FutureTargetCppScopeObjects.Reserve(Manifest.CppScopeDefinition.FutureTarget.Num());
	ForbiddenCppScopeObjects.Reserve(Manifest.CppScopeDefinition.ForbiddenNow.Num());
	ToolMappingObjects.Reserve(Manifest.ToolMapping.ToolMappings.Num());
	CoverageBucketObjects.Reserve(Manifest.ToolMapping.Coverage.UnmappedBuckets.Num());
	for (const FAgentCapabilityRiskFamilyManifest& Family : Manifest.Families)
	{
		switch (Family.CapabilityMode)
		{
		case EAgentCapabilityFamilyMode::ReadOnly:
			++ReadOnlyCount;
			break;

		case EAgentCapabilityFamilyMode::MutationCapable:
			++MutationCapableCount;
			break;

		case EAgentCapabilityFamilyMode::Mixed:
		default:
			++MixedCount;
			break;
		}

		if (Family.RiskClass.Contains(TEXT("high_")))
		{
			++HighRiskCount;
		}

		FamilyObjects.Add(MakeShared<FJsonValueObject>(MakeFamilyJson(Family)));
	}

	for (const FAgentCapabilityRiskySurfaceEntry& Entry : Manifest.RiskySurfaceCoverage.Surfaces)
	{
		RiskySurfaceObjects.Add(MakeShared<FJsonValueObject>(MakeRiskySurfaceEntryJson(Entry)));
	}

	for (const FAgentCapabilityRiskySurfaceCoverageBucket& Bucket : Manifest.RiskySurfaceCoverage.BacklogBuckets)
	{
		RiskySurfaceBacklogBucketObjects.Add(MakeShared<FJsonValueObject>(MakeRiskySurfaceCoverageBucketJson(Bucket)));
	}

	for (const FAgentCppScopeCategoryManifest& Category : Manifest.CppScopeDefinition.AllowedNow)
	{
		++AllowedCppScopeCount;
		AllowedCppScopeObjects.Add(MakeShared<FJsonValueObject>(MakeCppScopeCategoryJson(Category)));
	}

	for (const FAgentCppScopeCategoryManifest& Category : Manifest.CppScopeDefinition.FutureTarget)
	{
		++FutureTargetCppScopeCount;
		FutureTargetCppScopeObjects.Add(MakeShared<FJsonValueObject>(MakeCppScopeCategoryJson(Category)));
	}

	for (const FAgentCppScopeCategoryManifest& Category : Manifest.CppScopeDefinition.ForbiddenNow)
	{
		++ForbiddenCppScopeCount;
		ForbiddenCppScopeObjects.Add(MakeShared<FJsonValueObject>(MakeCppScopeCategoryJson(Category)));
	}

	for (const FAgentCapabilityRiskToolMapping& Mapping : Manifest.ToolMapping.ToolMappings)
	{
		switch (Mapping.CapabilityMode)
		{
		case EAgentCapabilityFamilyMode::ReadOnly:
			++ReadOnlyToolMappingCount;
			break;

		case EAgentCapabilityFamilyMode::MutationCapable:
			++MutationCapableToolMappingCount;
			break;

		case EAgentCapabilityFamilyMode::Mixed:
		default:
			++MixedToolMappingCount;
			break;
		}

		if (Mapping.AmbiguityState != TEXT("exact_tool_mapping"))
		{
			++AmbiguousToolMappingCount;
		}

		ToolMappingObjects.Add(MakeShared<FJsonValueObject>(MakeToolMappingJson(Mapping)));
	}

	for (const FAgentCapabilityRiskToolCoverageBucket& Bucket : Manifest.ToolMapping.Coverage.UnmappedBuckets)
	{
		CoverageBucketObjects.Add(MakeShared<FJsonValueObject>(MakeCoverageBucketJson(Bucket)));
	}

	Object->SetNumberField(TEXT("family_count"), Manifest.Families.Num());
	Object->SetNumberField(TEXT("read_only_family_count"), ReadOnlyCount);
	Object->SetNumberField(TEXT("mutation_capable_family_count"), MutationCapableCount);
	Object->SetNumberField(TEXT("mixed_family_count"), MixedCount);
	Object->SetNumberField(TEXT("high_risk_family_count"), HighRiskCount);
	Object->SetArrayField(TEXT("families"), FamilyObjects);

	TSharedPtr<FJsonObject> RiskySurfaceCoverageObject = MakeShared<FJsonObject>();
	RiskySurfaceCoverageObject->SetStringField(TEXT("schema_version"), Manifest.RiskySurfaceCoverage.SchemaVersion);
	RiskySurfaceCoverageObject->SetStringField(TEXT("manifest_source"), Manifest.RiskySurfaceCoverage.ManifestSource);
	RiskySurfaceCoverageObject->SetStringField(TEXT("classification_level"), Manifest.RiskySurfaceCoverage.ClassificationLevel);
	RiskySurfaceCoverageObject->SetNumberField(TEXT("surface_count"), Manifest.RiskySurfaceCoverage.SurfaceCount);
	RiskySurfaceCoverageObject->SetNumberField(TEXT("high_risk_execution_surface_count"), Manifest.RiskySurfaceCoverage.HighRiskExecutionSurfaceCount);
	RiskySurfaceCoverageObject->SetNumberField(TEXT("mutation_capable_surface_count"), Manifest.RiskySurfaceCoverage.MutationCapableSurfaceCount);
	RiskySurfaceCoverageObject->SetNumberField(TEXT("family_mapped_surface_count"), Manifest.RiskySurfaceCoverage.FamilyMappedSurfaceCount);
	RiskySurfaceCoverageObject->SetNumberField(TEXT("explicit_backlog_surface_count"), Manifest.RiskySurfaceCoverage.ExplicitBacklogSurfaceCount);
	RiskySurfaceCoverageObject->SetNumberField(TEXT("safer_lane_behavior_explicit_count"), Manifest.RiskySurfaceCoverage.SaferLaneBehaviorExplicitCount);
	RiskySurfaceCoverageObject->SetNumberField(TEXT("safer_lane_ambiguous_surface_count"), Manifest.RiskySurfaceCoverage.SaferLaneAmbiguousSurfaceCount);
	RiskySurfaceCoverageObject->SetNumberField(TEXT("safe_or_bounded_enforced_surface_count"), Manifest.RiskySurfaceCoverage.SafeOrBoundedEnforcedSurfaceCount);
	RiskySurfaceCoverageObject->SetNumberField(TEXT("safe_or_bounded_described_only_surface_count"), Manifest.RiskySurfaceCoverage.SafeOrBoundedDescribedOnlySurfaceCount);
	RiskySurfaceCoverageObject->SetArrayField(TEXT("family_mapped_surface_keys"), MakeJsonStringArray(Manifest.RiskySurfaceCoverage.FamilyMappedSurfaceKeys));
	RiskySurfaceCoverageObject->SetArrayField(TEXT("explicit_backlog_surface_keys"), MakeJsonStringArray(Manifest.RiskySurfaceCoverage.ExplicitBacklogSurfaceKeys));
	RiskySurfaceCoverageObject->SetArrayField(TEXT("backlog_buckets"), RiskySurfaceBacklogBucketObjects);
	RiskySurfaceCoverageObject->SetArrayField(TEXT("surfaces"), RiskySurfaceObjects);
	RiskySurfaceCoverageObject->SetStringField(TEXT("truth_boundary"), Manifest.RiskySurfaceCoverage.TruthBoundary);
	Object->SetObjectField(TEXT("risky_surface_coverage"), RiskySurfaceCoverageObject);

	TSharedPtr<FJsonObject> CppScopeDefinitionObject = MakeShared<FJsonObject>();
	CppScopeDefinitionObject->SetStringField(TEXT("schema_version"), Manifest.CppScopeDefinition.SchemaVersion);
	CppScopeDefinitionObject->SetStringField(TEXT("manifest_source"), Manifest.CppScopeDefinition.ManifestSource);
	CppScopeDefinitionObject->SetStringField(TEXT("scope_boundary"), Manifest.CppScopeDefinition.ScopeBoundary);
	CppScopeDefinitionObject->SetStringField(TEXT("truth_boundary"), Manifest.CppScopeDefinition.TruthBoundary);
	CppScopeDefinitionObject->SetNumberField(TEXT("allowed_now_count"), AllowedCppScopeCount);
	CppScopeDefinitionObject->SetNumberField(TEXT("future_target_count"), FutureTargetCppScopeCount);
	CppScopeDefinitionObject->SetNumberField(TEXT("forbidden_now_count"), ForbiddenCppScopeCount);
	CppScopeDefinitionObject->SetArrayField(TEXT("allowed_now"), AllowedCppScopeObjects);
	CppScopeDefinitionObject->SetArrayField(TEXT("future_target"), FutureTargetCppScopeObjects);
	CppScopeDefinitionObject->SetArrayField(TEXT("forbidden_now"), ForbiddenCppScopeObjects);
	Object->SetObjectField(TEXT("cpp_scope_definition"), CppScopeDefinitionObject);

	TSharedPtr<FJsonObject> ToolMappingObject = MakeShared<FJsonObject>();
	ToolMappingObject->SetStringField(TEXT("schema_version"), Manifest.ToolMapping.SchemaVersion);
	ToolMappingObject->SetStringField(TEXT("manifest_source"), Manifest.ToolMapping.ManifestSource);
	ToolMappingObject->SetStringField(TEXT("classification_level"), Manifest.ToolMapping.ClassificationLevel);
	ToolMappingObject->SetStringField(TEXT("truth_boundary"), Manifest.ToolMapping.TruthBoundary);
	ToolMappingObject->SetNumberField(TEXT("tool_count"), Manifest.ToolMapping.ToolMappings.Num());
	ToolMappingObject->SetNumberField(TEXT("ambiguous_mapping_count"), AmbiguousToolMappingCount);
	ToolMappingObject->SetNumberField(TEXT("read_only_tool_count"), ReadOnlyToolMappingCount);
	ToolMappingObject->SetNumberField(TEXT("mutation_capable_tool_count"), MutationCapableToolMappingCount);
	ToolMappingObject->SetNumberField(TEXT("mixed_tool_count"), MixedToolMappingCount);
	ToolMappingObject->SetArrayField(TEXT("mappings"), ToolMappingObjects);

	TSharedPtr<FJsonObject> CoverageObject = MakeShared<FJsonObject>();
	CoverageObject->SetStringField(TEXT("schema_version"), Manifest.ToolMapping.Coverage.SchemaVersion);
	CoverageObject->SetStringField(TEXT("manifest_source"), Manifest.ToolMapping.Coverage.ManifestSource);
	CoverageObject->SetStringField(TEXT("registry_inventory_source"), Manifest.ToolMapping.Coverage.RegistryInventorySource);
	CoverageObject->SetNumberField(TEXT("registry_tool_count"), Manifest.ToolMapping.Coverage.RegistryToolCount);
	CoverageObject->SetNumberField(TEXT("mapped_tool_count"), Manifest.ToolMapping.Coverage.MappedToolCount);
	CoverageObject->SetNumberField(TEXT("unmapped_tool_count"), Manifest.ToolMapping.Coverage.UnmappedToolCount);
	CoverageObject->SetNumberField(TEXT("mapping_coverage_ratio"), Manifest.ToolMapping.Coverage.MappingCoverageRatio);
	CoverageObject->SetArrayField(TEXT("mapped_tool_names"), MakeJsonStringArray(Manifest.ToolMapping.Coverage.MappedToolNames));
	CoverageObject->SetArrayField(TEXT("unmapped_tool_names"), MakeJsonStringArray(Manifest.ToolMapping.Coverage.UnmappedToolNames));
	CoverageObject->SetArrayField(TEXT("unmapped_buckets"), CoverageBucketObjects);
	CoverageObject->SetStringField(TEXT("truth_boundary"), Manifest.ToolMapping.Coverage.TruthBoundary);
	ToolMappingObject->SetObjectField(TEXT("coverage"), CoverageObject);
	Object->SetObjectField(TEXT("tool_mapping"), ToolMappingObject);
	return Object;
}
