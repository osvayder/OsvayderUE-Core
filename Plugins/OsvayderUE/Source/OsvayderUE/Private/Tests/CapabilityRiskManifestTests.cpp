// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "OsvayderSubsystem.h"
#include "MCP/Tools/MCPTool_PluginSettings.h"
#include "OsvayderUECapabilityRiskManifest.h"

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

	TSharedPtr<FJsonObject> FindFamilyById(const TSharedPtr<FJsonObject>& ManifestObject, const FString& FamilyId)
	{
		if (!ManifestObject.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Families = nullptr;
		if (!ManifestObject->TryGetArrayField(TEXT("families"), Families) || !Families)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& FamilyValue : *Families)
		{
			const TSharedPtr<FJsonObject> FamilyObject = FamilyValue.IsValid() ? FamilyValue->AsObject() : nullptr;
			if (!FamilyObject.IsValid())
			{
				continue;
			}

			FString CurrentFamilyId;
			if (FamilyObject->TryGetStringField(TEXT("family_id"), CurrentFamilyId) && CurrentFamilyId == FamilyId)
			{
				return FamilyObject;
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> GetToolMappingObjectOrNull(const TSharedPtr<FJsonObject>& ManifestObject)
	{
		return GetObjectFieldOrNull(ManifestObject, TEXT("tool_mapping"));
	}

	TSharedPtr<FJsonObject> FindToolMapping(
		const TSharedPtr<FJsonObject>& ManifestObject,
		const FString& ToolName,
		const FString& SurfaceIdentifier = FString())
	{
		const TSharedPtr<FJsonObject> ToolMappingObject = GetToolMappingObjectOrNull(ManifestObject);
		if (!ToolMappingObject.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Mappings = nullptr;
		if (!ToolMappingObject->TryGetArrayField(TEXT("mappings"), Mappings) || !Mappings)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& MappingValue : *Mappings)
		{
			const TSharedPtr<FJsonObject> MappingObject = MappingValue.IsValid() ? MappingValue->AsObject() : nullptr;
			if (!MappingObject.IsValid())
			{
				continue;
			}

			FString CurrentToolName;
			MappingObject->TryGetStringField(TEXT("tool_name"), CurrentToolName);
			if (CurrentToolName != ToolName)
			{
				continue;
			}

			if (SurfaceIdentifier.IsEmpty())
			{
				return MappingObject;
			}

			FString CurrentSurfaceIdentifier;
			MappingObject->TryGetStringField(TEXT("surface_identifier"), CurrentSurfaceIdentifier);
			if (CurrentSurfaceIdentifier == SurfaceIdentifier)
			{
				return MappingObject;
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> GetCoverageObjectOrNull(const TSharedPtr<FJsonObject>& ManifestObject)
	{
		return GetObjectFieldOrNull(GetToolMappingObjectOrNull(ManifestObject), TEXT("coverage"));
	}

	TSharedPtr<FJsonObject> GetRiskySurfaceCoverageObjectOrNull(const TSharedPtr<FJsonObject>& ManifestObject)
	{
		return GetObjectFieldOrNull(ManifestObject, TEXT("risky_surface_coverage"));
	}

	TSharedPtr<FJsonObject> FindRiskySurface(
		const TSharedPtr<FJsonObject>& ManifestObject,
		const FString& ToolName,
		const FString& SurfaceIdentifier = FString())
	{
		const TSharedPtr<FJsonObject> CoverageObject = GetRiskySurfaceCoverageObjectOrNull(ManifestObject);
		if (!CoverageObject.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Surfaces = nullptr;
		if (!CoverageObject->TryGetArrayField(TEXT("surfaces"), Surfaces) || !Surfaces)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& SurfaceValue : *Surfaces)
		{
			const TSharedPtr<FJsonObject> SurfaceObject = SurfaceValue.IsValid() ? SurfaceValue->AsObject() : nullptr;
			if (!SurfaceObject.IsValid())
			{
				continue;
			}

			FString CurrentToolName;
			SurfaceObject->TryGetStringField(TEXT("tool_name"), CurrentToolName);
			if (CurrentToolName != ToolName)
			{
				continue;
			}

			if (SurfaceIdentifier.IsEmpty())
			{
				return SurfaceObject;
			}

			FString CurrentSurfaceIdentifier;
			SurfaceObject->TryGetStringField(TEXT("surface_identifier"), CurrentSurfaceIdentifier);
			if (CurrentSurfaceIdentifier == SurfaceIdentifier)
			{
				return SurfaceObject;
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> GetCppScopeDefinitionObjectOrNull(const TSharedPtr<FJsonObject>& ManifestObject)
	{
		return GetObjectFieldOrNull(ManifestObject, TEXT("cpp_scope_definition"));
	}

	TSharedPtr<FJsonObject> FindCppScopeCategory(
		const TSharedPtr<FJsonObject>& ManifestObject,
		const FString& CollectionFieldName,
		const FString& CategoryId)
	{
		const TSharedPtr<FJsonObject> ScopeDefinitionObject = GetCppScopeDefinitionObjectOrNull(ManifestObject);
		if (!ScopeDefinitionObject.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Categories = nullptr;
		if (!ScopeDefinitionObject->TryGetArrayField(CollectionFieldName, Categories) || !Categories)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& CategoryValue : *Categories)
		{
			const TSharedPtr<FJsonObject> CategoryObject = CategoryValue.IsValid() ? CategoryValue->AsObject() : nullptr;
			if (!CategoryObject.IsValid())
			{
				continue;
			}

			FString CurrentCategoryId;
			if (CategoryObject->TryGetStringField(TEXT("category_id"), CurrentCategoryId) && CurrentCategoryId == CategoryId)
			{
				return CategoryObject;
			}
		}

		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_SchemaFoundation,
	"OsvayderUE.CapabilityRiskManifest.SchemaFoundation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_SchemaFoundation::RunTest(const FString& Parameters)
{
	const FAgentCapabilityRiskManifest Manifest = BuildAgentCapabilityRiskManifest(
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifest(),
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile::ReadOnlyDiagnostic),
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile::BoundedPluginMutation),
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(EAgentExecutionRunProfile::ExplicitExpertOptIn));

	TestEqual(TEXT("schema version should stay stable"), Manifest.SchemaVersion, FString(TEXT("capability_risk_manifest_v1")));
	TestEqual(TEXT("manifest source should stay stable"), Manifest.ManifestSource, FString(TEXT("plugin_family_classification_foundation")));
	TestEqual(TEXT("classification level should stay family"), Manifest.ClassificationLevel, FString(TEXT("family")));
	TestEqual(TEXT("family count should cover the accepted first set"), Manifest.Families.Num(), 8);
	TestEqual(TEXT("tool mapping schema should stay stable"), Manifest.ToolMapping.SchemaVersion, FString(TEXT("capability_risk_tool_mapping_v1")));
	TestEqual(TEXT("tool mapping manifest source should stay stable"), Manifest.ToolMapping.ManifestSource, FString(TEXT("tool_family_obligation_readback_foundation")));
	TestEqual(TEXT("tool mapping classification level should stay concrete tool or surface"), Manifest.ToolMapping.ClassificationLevel, FString(TEXT("concrete_tool_or_surface")));
	TestEqual(TEXT("tool mapping count should cover the accepted HC3 first pass"), Manifest.ToolMapping.ToolMappings.Num(), 15);
	TestEqual(TEXT("tool mapping coverage schema should stay stable"), Manifest.ToolMapping.Coverage.SchemaVersion, FString(TEXT("capability_risk_tool_mapping_coverage_v1")));
	TestEqual(TEXT("tool mapping coverage manifest source should stay stable"), Manifest.ToolMapping.Coverage.ManifestSource, FString(TEXT("live_registry_vs_partial_mapping_truth")));
	TestEqual(TEXT("mapped tool count should stay the accepted HC3 partial baseline"), Manifest.ToolMapping.Coverage.MappedToolCount, 13);
	TestTrue(TEXT("registry tool count should be at least the mapped set"), Manifest.ToolMapping.Coverage.RegistryToolCount >= Manifest.ToolMapping.Coverage.MappedToolCount);
	TestEqual(TEXT("unmapped count should remain registry minus mapped"), Manifest.ToolMapping.Coverage.UnmappedToolCount, Manifest.ToolMapping.Coverage.RegistryToolCount - Manifest.ToolMapping.Coverage.MappedToolCount);
	TestEqual(TEXT("risky surface coverage schema should stay stable"), Manifest.RiskySurfaceCoverage.SchemaVersion, FString(TEXT("capability_risk_risky_surface_coverage_v1")));
	TestEqual(TEXT("risky surface coverage manifest source should stay stable"), Manifest.RiskySurfaceCoverage.ManifestSource, FString(TEXT("high_risk_and_mutation_surface_governance_baseline")));
	TestEqual(TEXT("risky surface count should stay the accepted HC3 first set"), Manifest.RiskySurfaceCoverage.SurfaceCount, 27);
	TestEqual(TEXT("family-mapped risky surface count should stay explicit"), Manifest.RiskySurfaceCoverage.FamilyMappedSurfaceCount, 6);
	TestEqual(TEXT("explicit risky backlog count should stay explicit"), Manifest.RiskySurfaceCoverage.ExplicitBacklogSurfaceCount, 21);
	TestEqual(TEXT("safer-lane explicit behavior count should cover every risky surface"), Manifest.RiskySurfaceCoverage.SaferLaneBehaviorExplicitCount, Manifest.RiskySurfaceCoverage.SurfaceCount);
	TestEqual(TEXT("safer-lane ambiguous risky surface count should stay zero"), Manifest.RiskySurfaceCoverage.SaferLaneAmbiguousSurfaceCount, 0);
	TestEqual(TEXT("safe-or-bounded enforced surface count should reflect full broad mutation and external UI bucket hardening"), Manifest.RiskySurfaceCoverage.SafeOrBoundedEnforcedSurfaceCount, 21);
	TestEqual(TEXT("safe-or-bounded described-only surface count should now shrink to the unresolved remainder"), Manifest.RiskySurfaceCoverage.SafeOrBoundedDescribedOnlySurfaceCount, 6);
	TestEqual(TEXT("cpp scope definition schema should stay stable"), Manifest.CppScopeDefinition.SchemaVersion, FString(TEXT("cpp_scope_definition_v1")));
	TestEqual(TEXT("cpp scope definition manifest source should stay stable"), Manifest.CppScopeDefinition.ManifestSource, FString(TEXT("post_ultra_cpp_scope_definition_foundation")));
	TestEqual(TEXT("cpp allowed-now category count should stay stable"), Manifest.CppScopeDefinition.AllowedNow.Num(), 3);
	TestEqual(TEXT("cpp future-target category count should stay stable"), Manifest.CppScopeDefinition.FutureTarget.Num(), 2);
	TestEqual(TEXT("cpp forbidden-now category count should stay stable"), Manifest.CppScopeDefinition.ForbiddenNow.Num(), 3);

	int32 ReadOnlyCount = 0;
	int32 MutationCapableCount = 0;
	int32 MixedCount = 0;
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
	}

	TestEqual(TEXT("read-only family count should stay stable"), ReadOnlyCount, 4);
	TestEqual(TEXT("mutation-capable family count should stay stable"), MutationCapableCount, 2);
	TestEqual(TEXT("mixed family count should stay stable"), MixedCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_PluginSettingsReadback,
	"OsvayderUE.CapabilityRiskManifest.PluginSettingsReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_PluginSettingsReadback::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	TestTrue(TEXT("capability_risk_manifest should exist"), ManifestObject.IsValid());
	if (!ManifestObject.IsValid())
	{
		return false;
	}

	FString SchemaVersion;
	int32 FamilyCount = 0;
	int32 ReadOnlyCount = 0;
	int32 MutationCapableCount = 0;
	int32 MixedCount = 0;
	int32 HighRiskCount = 0;
	int32 ToolCount = 0;
	int32 AmbiguousMappingCount = 0;
	int32 MappedToolCount = 0;
	int32 UnmappedToolCount = 0;
	double CoverageRatio = -1.0;
	int32 RiskySurfaceCount = 0;
	int32 RiskyMappedCount = 0;
	int32 RiskyBacklogCount = 0;
	int32 SaferLaneExplicitCount = 0;
	int32 SaferLaneAmbiguousCount = -1;
	int32 SafeOrBoundedEnforcedCount = -1;
	int32 SafeOrBoundedDescribedOnlyCount = -1;
	int32 AllowedNowCount = 0;
	int32 FutureTargetCount = 0;
	int32 ForbiddenNowCount = 0;
	TestTrue(TEXT("schema_version should exist"), ManifestObject->TryGetStringField(TEXT("schema_version"), SchemaVersion));
	TestTrue(TEXT("family_count should exist"), ManifestObject->TryGetNumberField(TEXT("family_count"), FamilyCount));
	TestTrue(TEXT("read_only_family_count should exist"), ManifestObject->TryGetNumberField(TEXT("read_only_family_count"), ReadOnlyCount));
	TestTrue(TEXT("mutation_capable_family_count should exist"), ManifestObject->TryGetNumberField(TEXT("mutation_capable_family_count"), MutationCapableCount));
	TestTrue(TEXT("mixed_family_count should exist"), ManifestObject->TryGetNumberField(TEXT("mixed_family_count"), MixedCount));
	TestTrue(TEXT("high_risk_family_count should exist"), ManifestObject->TryGetNumberField(TEXT("high_risk_family_count"), HighRiskCount));
	const TSharedPtr<FJsonObject> ToolMappingObject = GetToolMappingObjectOrNull(ManifestObject);
	TestTrue(TEXT("tool_mapping should exist"), ToolMappingObject.IsValid());
	if (ToolMappingObject.IsValid())
	{
		TestTrue(TEXT("tool_count should exist"), ToolMappingObject->TryGetNumberField(TEXT("tool_count"), ToolCount));
		TestTrue(TEXT("ambiguous_mapping_count should exist"), ToolMappingObject->TryGetNumberField(TEXT("ambiguous_mapping_count"), AmbiguousMappingCount));
		const TSharedPtr<FJsonObject> CoverageObject = GetCoverageObjectOrNull(ManifestObject);
		TestTrue(TEXT("coverage should exist"), CoverageObject.IsValid());
		if (CoverageObject.IsValid())
		{
			TestTrue(TEXT("mapped_tool_count should exist"), CoverageObject->TryGetNumberField(TEXT("mapped_tool_count"), MappedToolCount));
			TestTrue(TEXT("unmapped_tool_count should exist"), CoverageObject->TryGetNumberField(TEXT("unmapped_tool_count"), UnmappedToolCount));
			TestTrue(TEXT("mapping_coverage_ratio should exist"), CoverageObject->TryGetNumberField(TEXT("mapping_coverage_ratio"), CoverageRatio));
		}
	}
	const TSharedPtr<FJsonObject> RiskySurfaceCoverageObject = GetRiskySurfaceCoverageObjectOrNull(ManifestObject);
	TestTrue(TEXT("risky_surface_coverage should exist"), RiskySurfaceCoverageObject.IsValid());
	if (RiskySurfaceCoverageObject.IsValid())
	{
		TestTrue(TEXT("surface_count should exist"), RiskySurfaceCoverageObject->TryGetNumberField(TEXT("surface_count"), RiskySurfaceCount));
		TestTrue(TEXT("family_mapped_surface_count should exist"), RiskySurfaceCoverageObject->TryGetNumberField(TEXT("family_mapped_surface_count"), RiskyMappedCount));
		TestTrue(TEXT("explicit_backlog_surface_count should exist"), RiskySurfaceCoverageObject->TryGetNumberField(TEXT("explicit_backlog_surface_count"), RiskyBacklogCount));
		TestTrue(TEXT("safer_lane_behavior_explicit_count should exist"), RiskySurfaceCoverageObject->TryGetNumberField(TEXT("safer_lane_behavior_explicit_count"), SaferLaneExplicitCount));
		TestTrue(TEXT("safer_lane_ambiguous_surface_count should exist"), RiskySurfaceCoverageObject->TryGetNumberField(TEXT("safer_lane_ambiguous_surface_count"), SaferLaneAmbiguousCount));
		TestTrue(TEXT("safe_or_bounded_enforced_surface_count should exist"), RiskySurfaceCoverageObject->TryGetNumberField(TEXT("safe_or_bounded_enforced_surface_count"), SafeOrBoundedEnforcedCount));
		TestTrue(TEXT("safe_or_bounded_described_only_surface_count should exist"), RiskySurfaceCoverageObject->TryGetNumberField(TEXT("safe_or_bounded_described_only_surface_count"), SafeOrBoundedDescribedOnlyCount));
	}
	const TSharedPtr<FJsonObject> CppScopeDefinitionObject = GetCppScopeDefinitionObjectOrNull(ManifestObject);
	TestTrue(TEXT("cpp_scope_definition should exist"), CppScopeDefinitionObject.IsValid());
	if (CppScopeDefinitionObject.IsValid())
	{
		TestTrue(TEXT("allowed_now_count should exist"), CppScopeDefinitionObject->TryGetNumberField(TEXT("allowed_now_count"), AllowedNowCount));
		TestTrue(TEXT("future_target_count should exist"), CppScopeDefinitionObject->TryGetNumberField(TEXT("future_target_count"), FutureTargetCount));
		TestTrue(TEXT("forbidden_now_count should exist"), CppScopeDefinitionObject->TryGetNumberField(TEXT("forbidden_now_count"), ForbiddenNowCount));
	}
	TestEqual(TEXT("schema version should match"), SchemaVersion, FString(TEXT("capability_risk_manifest_v1")));
	TestEqual(TEXT("family count should stay stable"), FamilyCount, 8);
	TestEqual(TEXT("read-only family count should stay stable"), ReadOnlyCount, 4);
	TestEqual(TEXT("mutation-capable family count should stay stable"), MutationCapableCount, 2);
	TestEqual(TEXT("mixed family count should stay stable"), MixedCount, 2);
	TestEqual(TEXT("high-risk family count should stay stable"), HighRiskCount, 1);
	TestEqual(TEXT("tool mapping count should stay stable"), ToolCount, 15);
	TestEqual(TEXT("ambiguous mapping count should stay narrow and explicit"), AmbiguousMappingCount, 3);
	TestEqual(TEXT("mapped tool count should stay stable"), MappedToolCount, 13);
	TestTrue(TEXT("unmapped tool count should stay non-zero while coverage remains partial"), UnmappedToolCount > 0);
	TestTrue(TEXT("coverage ratio should stay between zero and one"), CoverageRatio > 0.0 && CoverageRatio < 1.0);
	TestEqual(TEXT("risky surface count should stay stable"), RiskySurfaceCount, 27);
	TestEqual(TEXT("family-mapped risky surface count should stay stable"), RiskyMappedCount, 6);
	TestEqual(TEXT("explicit risky backlog count should stay stable"), RiskyBacklogCount, 21);
	TestEqual(TEXT("safer-lane explicit risky behavior count should cover all risky surfaces"), SaferLaneExplicitCount, 27);
	TestEqual(TEXT("safer-lane ambiguous risky surface count should stay zero"), SaferLaneAmbiguousCount, 0);
	TestEqual(TEXT("safe-or-bounded enforced risky surface count should reflect full broad mutation and external UI bucket hardening"), SafeOrBoundedEnforcedCount, 21);
	TestEqual(TEXT("safe-or-bounded described-only risky surface count should stay explicit for the unresolved remainder"), SafeOrBoundedDescribedOnlyCount, 6);
	TestEqual(TEXT("allowed_now_count should stay stable"), AllowedNowCount, 3);
	TestEqual(TEXT("future_target_count should stay stable"), FutureTargetCount, 2);
	TestEqual(TEXT("forbidden_now_count should stay stable"), ForbiddenNowCount, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_ProviderControlFamilyTruth,
	"OsvayderUE.CapabilityRiskManifest.ProviderControlFamilyTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_ProviderControlFamilyTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> FamilyObject = FindFamilyById(ManifestObject, TEXT("provider_control_status"));
	TestTrue(TEXT("provider_control_status family should exist"), FamilyObject.IsValid());
	if (!FamilyObject.IsValid())
	{
		return false;
	}

	FString CapabilityMode;
	FString RiskClass;
	FString RequiredProofTier;
	FString StopCondition;
	bool bWritesUserOwnedState = true;
	bool bRequiresRepresentativeLiveProof = false;
	TestTrue(TEXT("capability_mode should exist"), FamilyObject->TryGetStringField(TEXT("capability_mode"), CapabilityMode));
	TestTrue(TEXT("risk_class should exist"), FamilyObject->TryGetStringField(TEXT("risk_class"), RiskClass));
	TestTrue(TEXT("required_proof_tier should exist"), FamilyObject->TryGetStringField(TEXT("required_proof_tier"), RequiredProofTier));
	TestTrue(TEXT("stop_condition should exist"), FamilyObject->TryGetStringField(TEXT("stop_condition"), StopCondition));
	TestTrue(TEXT("writes_user_owned_state should exist"), FamilyObject->TryGetBoolField(TEXT("writes_user_owned_state"), bWritesUserOwnedState));
	TestTrue(TEXT("requires_representative_live_proof should exist"), FamilyObject->TryGetBoolField(TEXT("requires_representative_live_proof"), bRequiresRepresentativeLiveProof));
	TestEqual(TEXT("provider control family should stay read_only"), CapabilityMode, FString(TEXT("read_only")));
	TestEqual(TEXT("provider control family should stay medium runtime control truth"), RiskClass, FString(TEXT("medium_runtime_control_truth")));
	TestEqual(TEXT("provider control family proof tier should stay fresh build plus live readback"), RequiredProofTier, FString(TEXT("fresh_build_automation_plus_live_readback")));
	TestFalse(TEXT("provider control family should not write user-owned state"), bWritesUserOwnedState);
	TestTrue(TEXT("provider control family should require representative live proof"), bRequiresRepresentativeLiveProof);
	TestTrue(TEXT("provider control family should surface provider_execution_control as a representative surface"),
		JsonStringArrayContains(FamilyObject, TEXT("representative_surfaces"), TEXT("plugin_settings.assistant_backend.provider_execution_control")));
	TestTrue(TEXT("provider control family should surface dynamic default current power basis"),
		JsonStringArrayContains(FamilyObject, TEXT("current_reality_basis"), TEXT("default_current_power=workspace_write_project")));
	TestTrue(TEXT("provider control family should surface bounded runtime lane basis"),
		JsonStringArrayContains(FamilyObject, TEXT("current_reality_basis"), TEXT("bounded_runtime_lane=bounded_plugin_mutation")));
	TestTrue(TEXT("provider control family should surface explicit expert current power basis"),
		JsonStringArrayContains(FamilyObject, TEXT("current_reality_basis"), TEXT("expert_current_power=high_risk_direct_file_shell")));
	TestTrue(TEXT("provider control family should surface helper persistence basis"),
		JsonStringArrayContains(FamilyObject, TEXT("current_reality_basis"), TEXT("helper_session_persistence_mode=not_persisted")));
	TestTrue(TEXT("provider control family stop condition should keep truth mismatch escalation"),
		StopCondition.Contains(TEXT("stronger_enforcement_than_current_runtime_reality")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_MutationFamiliesTruth,
	"OsvayderUE.CapabilityRiskManifest.MutationFamiliesTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_MutationFamiliesTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> BlueprintMutationFamily = FindFamilyById(ManifestObject, TEXT("blueprint_mutation"));
	const TSharedPtr<FJsonObject> MutationGroupFamily = FindFamilyById(ManifestObject, TEXT("mutation_group_revert"));
	const TSharedPtr<FJsonObject> CppReflectionFamily = FindFamilyById(ManifestObject, TEXT("cpp_reflected_contracts"));
	TestTrue(TEXT("blueprint_mutation family should exist"), BlueprintMutationFamily.IsValid());
	TestTrue(TEXT("mutation_group_revert family should exist"), MutationGroupFamily.IsValid());
	TestTrue(TEXT("cpp_reflected_contracts family should exist"), CppReflectionFamily.IsValid());
	if (!BlueprintMutationFamily.IsValid() || !MutationGroupFamily.IsValid() || !CppReflectionFamily.IsValid())
	{
		return false;
	}

	FString BlueprintMode;
	FString BlueprintRisk;
	FString BlueprintCheckpointExpectation;
	bool bBlueprintWritesUserOwnedState = false;
	BlueprintMutationFamily->TryGetStringField(TEXT("capability_mode"), BlueprintMode);
	BlueprintMutationFamily->TryGetStringField(TEXT("risk_class"), BlueprintRisk);
	BlueprintMutationFamily->TryGetStringField(TEXT("checkpoint_expectation"), BlueprintCheckpointExpectation);
	BlueprintMutationFamily->TryGetBoolField(TEXT("writes_user_owned_state"), bBlueprintWritesUserOwnedState);
	TestEqual(TEXT("blueprint mutation family should stay mutation_capable"), BlueprintMode, FString(TEXT("mutation_capable")));
	TestEqual(TEXT("blueprint mutation family should stay high risk"), BlueprintRisk, FString(TEXT("high_broad_authoring_mutation")));
	TestEqual(TEXT("blueprint mutation family should stay without first-class checkpointing"), BlueprintCheckpointExpectation, FString(TEXT("not_yet_first_class")));
	TestTrue(TEXT("blueprint mutation family should write user-owned state"), bBlueprintWritesUserOwnedState);

	FString MutationGroupMode;
	FString MutationGroupRisk;
	FString MutationGroupRevertability;
	FString MutationGroupCheckpointExpectation;
	bool bMutationGroupRequiresCheckpoint = false;
	MutationGroupFamily->TryGetStringField(TEXT("capability_mode"), MutationGroupMode);
	MutationGroupFamily->TryGetStringField(TEXT("risk_class"), MutationGroupRisk);
	MutationGroupFamily->TryGetStringField(TEXT("revertability"), MutationGroupRevertability);
	MutationGroupFamily->TryGetStringField(TEXT("checkpoint_expectation"), MutationGroupCheckpointExpectation);
	MutationGroupFamily->TryGetBoolField(TEXT("requires_revert_or_checkpoint_for_mutation"), bMutationGroupRequiresCheckpoint);
	TestEqual(TEXT("mutation group family should stay mutation_capable"), MutationGroupMode, FString(TEXT("mutation_capable")));
	TestEqual(TEXT("mutation group family should stay bounded checkpointed mutation"), MutationGroupRisk, FString(TEXT("medium_bounded_mutation_with_checkpoint_revert")));
	TestEqual(TEXT("mutation group family should stay first-class revertable"), MutationGroupRevertability, FString(TEXT("first_class_checkpoint_revert_available")));
	TestEqual(TEXT("mutation group family should require checkpoints before apply"), MutationGroupCheckpointExpectation, FString(TEXT("required_before_apply")));
	TestTrue(TEXT("mutation group family should require revert/checkpoint"), bMutationGroupRequiresCheckpoint);

	FString CppReflectionMode;
	FString CppReflectionProofTier;
	bool bCppRequiresFreshBuild = false;
	CppReflectionFamily->TryGetStringField(TEXT("capability_mode"), CppReflectionMode);
	CppReflectionFamily->TryGetStringField(TEXT("required_proof_tier"), CppReflectionProofTier);
	CppReflectionFamily->TryGetBoolField(TEXT("requires_fresh_build_for_acceptance"), bCppRequiresFreshBuild);
	TestEqual(TEXT("cpp reflected contracts family should stay mixed"), CppReflectionMode, FString(TEXT("mixed")));
	TestEqual(TEXT("cpp reflected contracts proof tier should stay rebuild-based"), CppReflectionProofTier, FString(TEXT("fresh_build_automation_plus_live_preview_apply_rebuild_readback_revert")));
	TestTrue(TEXT("cpp reflected contracts family should require fresh build for acceptance"), bCppRequiresFreshBuild);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_ReadOnlyToolMappingTruth,
	"OsvayderUE.CapabilityRiskManifest.ReadOnlyToolMappingTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_ReadOnlyToolMappingTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> MappingObject = FindToolMapping(ManifestObject, TEXT("dependency_health"));
	TestTrue(TEXT("dependency_health mapping should exist"), MappingObject.IsValid());
	if (!MappingObject.IsValid())
	{
		return false;
	}

	FString GoverningFamilyId;
	FString CapabilityMode;
	FString RiskClass;
	FString RequiredProofTier;
	FString AmbiguityState;
	bool bRequiresRepresentativeLiveProof = false;
	MappingObject->TryGetStringField(TEXT("governing_family_id"), GoverningFamilyId);
	MappingObject->TryGetStringField(TEXT("capability_mode"), CapabilityMode);
	MappingObject->TryGetStringField(TEXT("risk_class"), RiskClass);
	MappingObject->TryGetStringField(TEXT("required_proof_tier"), RequiredProofTier);
	MappingObject->TryGetStringField(TEXT("ambiguity_state"), AmbiguityState);
	MappingObject->TryGetBoolField(TEXT("requires_representative_live_proof"), bRequiresRepresentativeLiveProof);
	TestEqual(TEXT("dependency_health should map to dependency_metadata_truth"), GoverningFamilyId, FString(TEXT("dependency_metadata_truth")));
	TestEqual(TEXT("dependency_health should stay read_only"), CapabilityMode, FString(TEXT("read_only")));
	TestEqual(TEXT("dependency_health should stay medium governance read-only analysis"), RiskClass, FString(TEXT("medium_governance_read_only_analysis")));
	TestEqual(TEXT("dependency_health proof tier should stay fresh build plus live readback"), RequiredProofTier, FString(TEXT("fresh_build_automation_plus_live_readback")));
	TestEqual(TEXT("dependency_health should stay an exact tool mapping"), AmbiguityState, FString(TEXT("exact_tool_mapping")));
	TestTrue(TEXT("dependency_health should require representative live proof"), bRequiresRepresentativeLiveProof);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_BroadMutationToolMappingTruth,
	"OsvayderUE.CapabilityRiskManifest.BroadMutationToolMappingTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_BroadMutationToolMappingTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> MappingObject = FindToolMapping(ManifestObject, TEXT("blueprint_modify"));
	TestTrue(TEXT("blueprint_modify mapping should exist"), MappingObject.IsValid());
	if (!MappingObject.IsValid())
	{
		return false;
	}

	FString GoverningFamilyId;
	FString CapabilityMode;
	FString RiskClass;
	FString CheckpointExpectation;
	bool bWritesUserOwnedState = false;
	MappingObject->TryGetStringField(TEXT("governing_family_id"), GoverningFamilyId);
	MappingObject->TryGetStringField(TEXT("capability_mode"), CapabilityMode);
	MappingObject->TryGetStringField(TEXT("risk_class"), RiskClass);
	MappingObject->TryGetStringField(TEXT("checkpoint_expectation"), CheckpointExpectation);
	MappingObject->TryGetBoolField(TEXT("writes_user_owned_state"), bWritesUserOwnedState);
	TestEqual(TEXT("blueprint_modify should map to blueprint_mutation"), GoverningFamilyId, FString(TEXT("blueprint_mutation")));
	TestEqual(TEXT("blueprint_modify should stay mutation_capable"), CapabilityMode, FString(TEXT("mutation_capable")));
	TestEqual(TEXT("blueprint_modify should stay high broad authoring mutation"), RiskClass, FString(TEXT("high_broad_authoring_mutation")));
	TestEqual(TEXT("blueprint_modify should stay without first-class checkpointing"), CheckpointExpectation, FString(TEXT("not_yet_first_class")));
	TestTrue(TEXT("blueprint_modify should write user-owned state"), bWritesUserOwnedState);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_BoundedPostUltraToolMappingTruth,
	"OsvayderUE.CapabilityRiskManifest.BoundedPostUltraToolMappingTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_BoundedPostUltraToolMappingTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> MappingObject = FindToolMapping(ManifestObject, TEXT("mutation_group"));
	TestTrue(TEXT("mutation_group mapping should exist"), MappingObject.IsValid());
	if (!MappingObject.IsValid())
	{
		return false;
	}

	FString GoverningFamilyId;
	FString CapabilityMode;
	FString Revertability;
	FString CheckpointExpectation;
	bool bRequiresCheckpoint = false;
	MappingObject->TryGetStringField(TEXT("governing_family_id"), GoverningFamilyId);
	MappingObject->TryGetStringField(TEXT("capability_mode"), CapabilityMode);
	MappingObject->TryGetStringField(TEXT("revertability"), Revertability);
	MappingObject->TryGetStringField(TEXT("checkpoint_expectation"), CheckpointExpectation);
	MappingObject->TryGetBoolField(TEXT("requires_revert_or_checkpoint_for_mutation"), bRequiresCheckpoint);
	TestEqual(TEXT("mutation_group should map to mutation_group_revert"), GoverningFamilyId, FString(TEXT("mutation_group_revert")));
	TestEqual(TEXT("mutation_group should stay mutation_capable"), CapabilityMode, FString(TEXT("mutation_capable")));
	TestEqual(TEXT("mutation_group should stay first-class revertable"), Revertability, FString(TEXT("first_class_checkpoint_revert_available")));
	TestEqual(TEXT("mutation_group should require checkpoints before apply"), CheckpointExpectation, FString(TEXT("required_before_apply")));
	TestTrue(TEXT("mutation_group should require revert/checkpoint"), bRequiresCheckpoint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_CppScopeSchemaFoundation,
	"OsvayderUE.CapabilityRiskManifest.CppScopeSchemaFoundation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_CppScopeSchemaFoundation::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> ScopeDefinitionObject = GetCppScopeDefinitionObjectOrNull(ManifestObject);
	TestTrue(TEXT("cpp_scope_definition should exist"), ScopeDefinitionObject.IsValid());
	if (!ScopeDefinitionObject.IsValid())
	{
		return false;
	}

	FString SchemaVersion;
	FString ManifestSource;
	int32 AllowedNowCount = 0;
	int32 FutureTargetCount = 0;
	int32 ForbiddenNowCount = 0;
	ScopeDefinitionObject->TryGetStringField(TEXT("schema_version"), SchemaVersion);
	ScopeDefinitionObject->TryGetStringField(TEXT("manifest_source"), ManifestSource);
	ScopeDefinitionObject->TryGetNumberField(TEXT("allowed_now_count"), AllowedNowCount);
	ScopeDefinitionObject->TryGetNumberField(TEXT("future_target_count"), FutureTargetCount);
	ScopeDefinitionObject->TryGetNumberField(TEXT("forbidden_now_count"), ForbiddenNowCount);
	TestEqual(TEXT("cpp scope schema version should stay stable"), SchemaVersion, FString(TEXT("cpp_scope_definition_v1")));
	TestEqual(TEXT("cpp scope manifest source should stay stable"), ManifestSource, FString(TEXT("post_ultra_cpp_scope_definition_foundation")));
	TestEqual(TEXT("allowed_now count should stay stable"), AllowedNowCount, 3);
	TestEqual(TEXT("future_target count should stay stable"), FutureTargetCount, 2);
	TestEqual(TEXT("forbidden_now count should stay stable"), ForbiddenNowCount, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_CppScopeAllowedNowTruth,
	"OsvayderUE.CapabilityRiskManifest.CppScopeAllowedNowTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_CppScopeAllowedNowTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> CategoryObject = FindCppScopeCategory(
		ManifestObject,
		TEXT("allowed_now"),
		TEXT("bounded_reflected_metadata_mutation"));
	TestTrue(TEXT("bounded_reflected_metadata_mutation should exist under allowed_now"), CategoryObject.IsValid());
	if (!CategoryObject.IsValid())
	{
		return false;
	}

	FString CurrentAvailability;
	FString MutationBreadth;
	FString RequiredProofTier;
	FString RebuildExpectation;
	bool bRequiresFreshBuild = false;
	bool bRequiresRestoreOrRevert = false;
	CategoryObject->TryGetStringField(TEXT("current_availability"), CurrentAvailability);
	CategoryObject->TryGetStringField(TEXT("mutation_breadth"), MutationBreadth);
	CategoryObject->TryGetStringField(TEXT("required_proof_tier"), RequiredProofTier);
	CategoryObject->TryGetStringField(TEXT("rebuild_expectation"), RebuildExpectation);
	CategoryObject->TryGetBoolField(TEXT("requires_fresh_build_for_acceptance"), bRequiresFreshBuild);
	CategoryObject->TryGetBoolField(TEXT("requires_restore_or_revert_for_mutation"), bRequiresRestoreOrRevert);
	TestEqual(TEXT("bounded reflected metadata mutation should stay accepted live bounded"), CurrentAvailability, FString(TEXT("accepted_live_bounded")));
	TestEqual(TEXT("bounded reflected metadata mutation breadth should stay narrow"), MutationBreadth, FString(TEXT("bounded_plugin_owned_header_metadata_upsert")));
	TestEqual(TEXT("bounded reflected metadata mutation proof tier should stay rebuild-based"), RequiredProofTier, FString(TEXT("fresh_build_automation_plus_live_preview_apply_rebuild_readback")));
	TestEqual(TEXT("bounded reflected metadata mutation rebuild expectation should stay explicit"), RebuildExpectation, FString(TEXT("fresh_preflight_rebuild_required_after_apply")));
	TestTrue(TEXT("bounded reflected metadata mutation should require fresh build"), bRequiresFreshBuild);
	TestTrue(TEXT("bounded reflected metadata mutation should require restore or revert expectation"), bRequiresRestoreOrRevert);
	TestTrue(TEXT("bounded reflected metadata mutation should map current cpp_reflection apply lane"),
		JsonStringArrayContains(CategoryObject, TEXT("current_lane_mappings"), TEXT("cpp_reflection.apply_property_metadata_mutation")));
	TestTrue(TEXT("bounded reflected metadata mutation should map current mutation_group apply lane"),
		JsonStringArrayContains(CategoryObject, TEXT("current_lane_mappings"), TEXT("mutation_group.apply_group")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_CppScopeFutureTargetPreviewTruth,
	"OsvayderUE.CapabilityRiskManifest.CppScopeFutureTargetPreviewTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_CppScopeFutureTargetPreviewTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> CategoryObject = FindCppScopeCategory(
		ManifestObject,
		TEXT("future_target"),
		TEXT("structured_reflected_declaration_authoring"));
	TestTrue(TEXT("structured_reflected_declaration_authoring should exist under future_target"), CategoryObject.IsValid());
	if (!CategoryObject.IsValid())
	{
		return false;
	}

	FString CurrentAvailability;
	FString TruthBoundary;
	CategoryObject->TryGetStringField(TEXT("current_availability"), CurrentAvailability);
	CategoryObject->TryGetStringField(TEXT("truth_boundary"), TruthBoundary);
	TestEqual(TEXT("structured reflected declaration authoring should expose single-shape apply foundation truth"), CurrentAvailability, FString(TEXT("single_shape_apply_foundation_live")));
	TestTrue(TEXT("future target should map preview_reflected_property_declaration"), JsonStringArrayContains(CategoryObject, TEXT("current_lane_mappings"), TEXT("cpp_reflection.preview_reflected_property_declaration")));
	TestTrue(TEXT("future target should map apply_reflected_property_declaration"), JsonStringArrayContains(CategoryObject, TEXT("current_lane_mappings"), TEXT("cpp_reflection.apply_reflected_property_declaration")));
	TestTrue(TEXT("future target should map revert_reflected_property_declaration"), JsonStringArrayContains(CategoryObject, TEXT("current_lane_mappings"), TEXT("cpp_reflection.revert_reflected_property_declaration")));
	TestTrue(TEXT("future target truth boundary should keep breadth future-scoped"), TruthBoundary.Contains(TEXT("broader reflected declaration authoring")));

	const TSharedPtr<FJsonObject> FamilyObject = FindFamilyById(ManifestObject, TEXT("cpp_reflected_contracts"));
	TestTrue(TEXT("cpp_reflected_contracts family should exist"), FamilyObject.IsValid());
	if (FamilyObject.IsValid())
	{
		FString FamilyTruthBoundary;
		FamilyObject->TryGetStringField(TEXT("truth_boundary"), FamilyTruthBoundary);
		TestTrue(TEXT("cpp reflected contracts family should expose preview declaration surface"), JsonStringArrayContains(FamilyObject, TEXT("representative_surfaces"), TEXT("cpp_reflection.preview_reflected_property_declaration")));
		TestTrue(TEXT("cpp reflected contracts family should expose apply declaration surface"), JsonStringArrayContains(FamilyObject, TEXT("representative_surfaces"), TEXT("cpp_reflection.apply_reflected_property_declaration")));
		TestTrue(TEXT("cpp reflected contracts family should expose revert declaration surface"), JsonStringArrayContains(FamilyObject, TEXT("representative_surfaces"), TEXT("cpp_reflection.revert_reflected_property_declaration")));
		TestTrue(TEXT("cpp reflected contracts family should expose failed-build diagnostic surface"), JsonStringArrayContains(FamilyObject, TEXT("representative_surfaces"), TEXT("cpp_reflection.inspect_reflected_property_declaration_build_failure")));
		TestTrue(TEXT("cpp reflected contracts family should keep bounded declaration truth"), FamilyTruthBoundary.Contains(TEXT("one plugin-owned bool-property shape")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_CppScopeForbiddenNowTruth,
	"OsvayderUE.CapabilityRiskManifest.CppScopeForbiddenNowTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_CppScopeForbiddenNowTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> CategoryObject = FindCppScopeCategory(
		ManifestObject,
		TEXT("forbidden_now"),
		TEXT("arbitrary_cpp_implementation_body_editing"));
	TestTrue(TEXT("arbitrary_cpp_implementation_body_editing should exist under forbidden_now"), CategoryObject.IsValid());
	if (!CategoryObject.IsValid())
	{
		return false;
	}

	FString CurrentAvailability;
	FString RequiredProofTier;
	FString RebuildExpectation;
	FString StopCondition;
	CategoryObject->TryGetStringField(TEXT("current_availability"), CurrentAvailability);
	CategoryObject->TryGetStringField(TEXT("required_proof_tier"), RequiredProofTier);
	CategoryObject->TryGetStringField(TEXT("rebuild_expectation"), RebuildExpectation);
	CategoryObject->TryGetStringField(TEXT("stop_condition"), StopCondition);
	TestEqual(TEXT("arbitrary implementation-body editing should stay forbidden"), CurrentAvailability, FString(TEXT("explicitly_forbidden")));
	TestEqual(TEXT("arbitrary implementation-body editing should stay not accepted"), RequiredProofTier, FString(TEXT("not_accepted")));
	TestEqual(TEXT("arbitrary implementation-body editing should stay not permitted"), RebuildExpectation, FString(TEXT("not_permitted")));
	TestTrue(TEXT("arbitrary implementation-body editing should keep immediate escalation wording"),
		StopCondition.Contains(TEXT("stop_and_escalate_immediately")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_CppScopeLaneMappingTruth,
	"OsvayderUE.CapabilityRiskManifest.CppScopeLaneMappingTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_CppScopeLaneMappingTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> DiscoveryCategory = FindCppScopeCategory(
		ManifestObject,
		TEXT("allowed_now"),
		TEXT("reflected_contract_discovery"));
	const TSharedPtr<FJsonObject> HandshakeCategory = FindCppScopeCategory(
		ManifestObject,
		TEXT("allowed_now"),
		TEXT("compile_diagnostic_rebuild_handshake"));
	TestTrue(TEXT("reflected_contract_discovery should exist"), DiscoveryCategory.IsValid());
	TestTrue(TEXT("compile_diagnostic_rebuild_handshake should exist"), HandshakeCategory.IsValid());
	if (!DiscoveryCategory.IsValid() || !HandshakeCategory.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("reflected contract discovery should map list_reflected_contracts"),
		JsonStringArrayContains(DiscoveryCategory, TEXT("current_lane_mappings"), TEXT("cpp_reflection.list_reflected_contracts")));
	TestTrue(TEXT("reflected contract discovery should map get_reflected_contract"),
		JsonStringArrayContains(DiscoveryCategory, TEXT("current_lane_mappings"), TEXT("cpp_reflection.get_reflected_contract")));
	TestTrue(TEXT("compile handshake category should map cpp_reflection apply"),
		JsonStringArrayContains(HandshakeCategory, TEXT("current_lane_mappings"), TEXT("cpp_reflection.apply_property_metadata_mutation")));
	TestTrue(TEXT("compile handshake category should map declaration apply"),
		JsonStringArrayContains(HandshakeCategory, TEXT("current_lane_mappings"), TEXT("cpp_reflection.apply_reflected_property_declaration")));
	TestTrue(TEXT("compile handshake category should map declaration revert"),
		JsonStringArrayContains(HandshakeCategory, TEXT("current_lane_mappings"), TEXT("cpp_reflection.revert_reflected_property_declaration")));
	TestTrue(TEXT("compile handshake category should map declaration failed-build diagnostics"),
		JsonStringArrayContains(HandshakeCategory, TEXT("current_lane_mappings"), TEXT("cpp_reflection.inspect_reflected_property_declaration_build_failure")));
	TestTrue(TEXT("compile handshake category should map mutation_group revert"),
		JsonStringArrayContains(HandshakeCategory, TEXT("current_lane_mappings"), TEXT("mutation_group.revert_group")));
	TestTrue(TEXT("compile handshake category should map plugin build freshness readback"),
		JsonStringArrayContains(HandshakeCategory, TEXT("current_lane_mappings"), TEXT("plugin_settings.assistant_backend.plugin_build_fresh")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_CoverageTruth,
	"OsvayderUE.CapabilityRiskManifest.CoverageTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_CoverageTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> CoverageObject = GetCoverageObjectOrNull(ManifestObject);
	TestTrue(TEXT("coverage should exist"), CoverageObject.IsValid());
	if (!CoverageObject.IsValid())
	{
		return false;
	}

	FString CoverageSchemaVersion;
	FString InventorySource;
	int32 RegistryToolCount = 0;
	int32 MappedToolCount = 0;
	int32 UnmappedToolCount = 0;
	double CoverageRatio = -1.0;
	CoverageObject->TryGetStringField(TEXT("schema_version"), CoverageSchemaVersion);
	CoverageObject->TryGetStringField(TEXT("registry_inventory_source"), InventorySource);
	CoverageObject->TryGetNumberField(TEXT("registry_tool_count"), RegistryToolCount);
	CoverageObject->TryGetNumberField(TEXT("mapped_tool_count"), MappedToolCount);
	CoverageObject->TryGetNumberField(TEXT("unmapped_tool_count"), UnmappedToolCount);
	CoverageObject->TryGetNumberField(TEXT("mapping_coverage_ratio"), CoverageRatio);
	TestEqual(TEXT("coverage schema version should stay stable"), CoverageSchemaVersion, FString(TEXT("capability_risk_tool_mapping_coverage_v1")));
	TestTrue(TEXT("inventory source should stay explicit"), InventorySource == TEXT("live_mcp_registry") || InventorySource == TEXT("expected_tools_fallback"));
	TestEqual(TEXT("mapped tool count should stay the accepted HC3 partial baseline"), MappedToolCount, 13);
	TestEqual(TEXT("unmapped tool count should remain registry minus mapped"), UnmappedToolCount, RegistryToolCount - MappedToolCount);
	TestTrue(TEXT("coverage ratio should remain partial"), CoverageRatio > 0.0 && CoverageRatio < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_ExplicitUnmappedExampleTruth,
	"OsvayderUE.CapabilityRiskManifest.ExplicitUnmappedExampleTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_ExplicitUnmappedExampleTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> CoverageObject = GetCoverageObjectOrNull(ManifestObject);
	TestTrue(TEXT("coverage should exist"), CoverageObject.IsValid());
	if (!CoverageObject.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("spawn_actor should stay explicitly unmapped in the current partial baseline"),
		JsonStringArrayContains(CoverageObject, TEXT("unmapped_tool_names"), TEXT("spawn_actor")));

	const TArray<TSharedPtr<FJsonValue>>* Buckets = nullptr;
	TestTrue(TEXT("unmapped_buckets should exist"), CoverageObject->TryGetArrayField(TEXT("unmapped_buckets"), Buckets) && Buckets);
	if (!Buckets)
	{
		return false;
	}

	bool bFoundLegacyBucket = false;
	for (const TSharedPtr<FJsonValue>& BucketValue : *Buckets)
	{
		const TSharedPtr<FJsonObject> BucketObject = BucketValue.IsValid() ? BucketValue->AsObject() : nullptr;
		if (!BucketObject.IsValid())
		{
			continue;
		}

		FString BucketId;
		BucketObject->TryGetStringField(TEXT("bucket_id"), BucketId);
		if (BucketId == TEXT("legacy_authoring_breadth"))
		{
			bFoundLegacyBucket = JsonStringArrayContains(BucketObject, TEXT("tool_names"), TEXT("spawn_actor"));
			break;
		}
	}

	TestTrue(TEXT("spawn_actor should be bucketed under legacy_authoring_breadth"), bFoundLegacyBucket);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_RiskySurfaceCoverageTruth,
	"OsvayderUE.CapabilityRiskManifest.RiskySurfaceCoverageTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_RiskySurfaceCoverageTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> CoverageObject = GetRiskySurfaceCoverageObjectOrNull(ManifestObject);
	TestTrue(TEXT("risky surface coverage should exist"), CoverageObject.IsValid());
	if (!CoverageObject.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("family-mapped surface keys should include bounded runtime profile"),
		JsonStringArrayContains(CoverageObject, TEXT("family_mapped_surface_keys"), TEXT("plugin_settings:assistant_backend.execution_profiles.bounded_plugin_mutation")));
	TestTrue(TEXT("family-mapped surface keys should include explicit expert runtime profile"),
		JsonStringArrayContains(CoverageObject, TEXT("family_mapped_surface_keys"), TEXT("plugin_settings:assistant_backend.execution_profiles.explicit_expert_opt_in")));

	const TSharedPtr<FJsonObject> BoundedRuntimeSurface = FindRiskySurface(
		ManifestObject,
		TEXT("plugin_settings"),
		TEXT("assistant_backend.execution_profiles.bounded_plugin_mutation"));
	TestTrue(TEXT("bounded runtime risky surface should exist"), BoundedRuntimeSurface.IsValid());
	if (BoundedRuntimeSurface.IsValid())
	{
		FString GovernanceState;
		FString RiskFocus;
		BoundedRuntimeSurface->TryGetStringField(TEXT("governance_state"), GovernanceState);
		BoundedRuntimeSurface->TryGetStringField(TEXT("risk_focus"), RiskFocus);
		TestEqual(TEXT("bounded runtime surface should stay family-mapped"), GovernanceState, FString(TEXT("family_mapped_surface")));
		TestEqual(TEXT("bounded runtime surface should stay mutation-focused"), RiskFocus, FString(TEXT("mutation_capable_surface")));

		const TSharedPtr<FJsonObject> BoundedBehavior = GetObjectFieldOrNull(BoundedRuntimeSurface, TEXT("bounded_plugin_mutation"));
		TestTrue(TEXT("bounded runtime behavior should exist"), BoundedBehavior.IsValid());
		if (BoundedBehavior.IsValid())
		{
			FString BehaviorState;
			BoundedBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			TestEqual(TEXT("bounded runtime surface should show governed mutation availability"), BehaviorState, FString(TEXT("current_governed_mutation_profile_available")));
		}
	}

	const TSharedPtr<FJsonObject> ExplicitExpertSurface = FindRiskySurface(
		ManifestObject,
		TEXT("plugin_settings"),
		TEXT("assistant_backend.execution_profiles.explicit_expert_opt_in"));
	TestTrue(TEXT("explicit expert risky surface should exist"), ExplicitExpertSurface.IsValid());
	if (ExplicitExpertSurface.IsValid())
	{
		FString RiskFocus;
		ExplicitExpertSurface->TryGetStringField(TEXT("risk_focus"), RiskFocus);
		TestEqual(TEXT("explicit expert surface should stay high-risk execution focused"), RiskFocus, FString(TEXT("high_risk_execution_surface")));

		const TSharedPtr<FJsonObject> ExpertBehavior = GetObjectFieldOrNull(ExplicitExpertSurface, TEXT("explicit_expert_opt_in"));
		TestTrue(TEXT("explicit expert behavior should exist"), ExpertBehavior.IsValid());
		if (ExpertBehavior.IsValid())
		{
			FString BehaviorState;
			ExpertBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			TestEqual(TEXT("explicit expert surface should show active high-risk lane"), BehaviorState, FString(TEXT("active_high_risk_execution_surface")));
		}
	}

	const TSharedPtr<FJsonObject> CppReflectionSurface = FindRiskySurface(ManifestObject, TEXT("cpp_reflection"));
	TestTrue(TEXT("cpp_reflection risky surface should exist"), CppReflectionSurface.IsValid());
	if (CppReflectionSurface.IsValid())
	{
		FString GoverningFamilyId;
		CppReflectionSurface->TryGetStringField(TEXT("governing_family_id"), GoverningFamilyId);
		TestEqual(TEXT("cpp_reflection should stay mapped to cpp_reflected_contracts"), GoverningFamilyId, FString(TEXT("cpp_reflected_contracts")));

		const TSharedPtr<FJsonObject> BoundedBehavior = GetObjectFieldOrNull(CppReflectionSurface, TEXT("bounded_plugin_mutation"));
		TestTrue(TEXT("cpp_reflection bounded behavior should exist"), BoundedBehavior.IsValid());
		if (BoundedBehavior.IsValid())
		{
			FString BehaviorState;
			BoundedBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			TestEqual(TEXT("cpp_reflection should stay governed-family allowed on bounded lane"), BehaviorState, FString(TEXT("governed_family_allowed")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilityRiskManifest_RiskySurfaceBacklogTruth,
	"OsvayderUE.CapabilityRiskManifest.RiskySurfaceBacklogTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCapabilityRiskManifest_RiskySurfaceBacklogTruth::RunTest(const FString& Parameters)
{
	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ManifestObject = GetObjectFieldOrNull(Result.Data, TEXT("capability_risk_manifest"));
	const TSharedPtr<FJsonObject> CoverageObject = GetRiskySurfaceCoverageObjectOrNull(ManifestObject);
	TestTrue(TEXT("risky surface coverage should exist"), CoverageObject.IsValid());
	if (!CoverageObject.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("spawn_actor should stay explicitly backlog-mapped in risky surface coverage"),
		JsonStringArrayContains(CoverageObject, TEXT("explicit_backlog_surface_keys"), TEXT("spawn_actor")));
	TestTrue(TEXT("execute_script should stay explicitly backlog-mapped in risky surface coverage"),
		JsonStringArrayContains(CoverageObject, TEXT("explicit_backlog_surface_keys"), TEXT("execute_script")));

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
		const TSharedPtr<FJsonObject> Surface = FindRiskySurface(ManifestObject, ToolName);
		TestTrue(FString::Printf(TEXT("%s risky surface should exist"), *ToolName), Surface.IsValid());
		if (!Surface.IsValid())
		{
			continue;
		}

		FString GovernanceBucketId;
		Surface->TryGetStringField(TEXT("governance_bucket_id"), GovernanceBucketId);
		TestEqual(FString::Printf(TEXT("%s should stay in broad authoring backlog"), *ToolName), GovernanceBucketId, FString(TEXT("broad_authoring_mutation_backlog")));

		const TSharedPtr<FJsonObject> DefaultBehavior = GetObjectFieldOrNull(Surface, TEXT("configured_default_runtime"));
		const TSharedPtr<FJsonObject> BoundedBehavior = GetObjectFieldOrNull(Surface, TEXT("bounded_plugin_mutation"));
		const TSharedPtr<FJsonObject> ExpertBehavior = GetObjectFieldOrNull(Surface, TEXT("explicit_expert_opt_in"));
		TestTrue(FString::Printf(TEXT("%s default behavior should exist"), *ToolName), DefaultBehavior.IsValid());
		TestTrue(FString::Printf(TEXT("%s bounded behavior should exist"), *ToolName), BoundedBehavior.IsValid());
		TestTrue(FString::Printf(TEXT("%s expert behavior should exist"), *ToolName), ExpertBehavior.IsValid());
		if (DefaultBehavior.IsValid())
		{
			FString BehaviorState;
			FString PolicyRuleId;
			FString EnforcementState;
			bool bBehaviorEnforcedNow = false;
			DefaultBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			DefaultBehavior->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId);
			DefaultBehavior->TryGetStringField(TEXT("enforcement_state"), EnforcementState);
			DefaultBehavior->TryGetBoolField(TEXT("behavior_enforced_now"), bBehaviorEnforcedNow);
			TestEqual(FString::Printf(TEXT("%s should now show direct deny on safer default"), *ToolName), BehaviorState, FString(TEXT("direct_policy_denied_broad_authoring_mutation_surface")));
			TestEqual(FString::Printf(TEXT("%s should keep workspace-write default broad mutation deny policy rule"), *ToolName), PolicyRuleId, FString(TEXT("workspace_write_project.broad_authoring_mutation_surface_denied")));
			TestEqual(FString::Printf(TEXT("%s safer-default enforcement state should be direct policy deny"), *ToolName), EnforcementState, FString(TEXT("direct_policy_deny_contract")));
			TestTrue(FString::Printf(TEXT("%s safer-default behavior should be enforced now"), *ToolName), bBehaviorEnforcedNow);
		}
		if (BoundedBehavior.IsValid())
		{
			FString BehaviorState;
			FString PolicyRuleId;
			FString EnforcementState;
			bool bBehaviorEnforcedNow = false;
			BoundedBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			BoundedBehavior->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId);
			BoundedBehavior->TryGetStringField(TEXT("enforcement_state"), EnforcementState);
			BoundedBehavior->TryGetBoolField(TEXT("behavior_enforced_now"), bBehaviorEnforcedNow);
			TestEqual(FString::Printf(TEXT("%s should now show direct deny on bounded lane"), *ToolName), BehaviorState, FString(TEXT("direct_policy_denied_broad_authoring_mutation_surface")));
			TestEqual(FString::Printf(TEXT("%s should keep bounded broad mutation deny policy rule"), *ToolName), PolicyRuleId, FString(TEXT("bounded_plugin_mutation.broad_authoring_mutation_surface_denied")));
			TestEqual(FString::Printf(TEXT("%s bounded enforcement state should be direct policy deny"), *ToolName), EnforcementState, FString(TEXT("direct_policy_deny_contract")));
			TestTrue(FString::Printf(TEXT("%s bounded behavior should be enforced now"), *ToolName), bBehaviorEnforcedNow);
		}
		if (ExpertBehavior.IsValid())
		{
			FString BehaviorState;
			ExpertBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			TestEqual(FString::Printf(TEXT("%s should stay reachable only on explicit expert lane"), *ToolName), BehaviorState, FString(TEXT("reachable_via_enabled_unreal_mcp_bridge")));
		}
	}

	const TSharedPtr<FJsonObject> ExecuteScriptSurface = FindRiskySurface(ManifestObject, TEXT("execute_script"));
	TestTrue(TEXT("execute_script risky surface should exist"), ExecuteScriptSurface.IsValid());
	if (ExecuteScriptSurface.IsValid())
	{
		FString RiskFocus;
		FString GovernanceBucketId;
		const TSharedPtr<FJsonObject> DefaultBehavior = GetObjectFieldOrNull(ExecuteScriptSurface, TEXT("configured_default_runtime"));
		const TSharedPtr<FJsonObject> BoundedBehavior = GetObjectFieldOrNull(ExecuteScriptSurface, TEXT("bounded_plugin_mutation"));
		ExecuteScriptSurface->TryGetStringField(TEXT("risk_focus"), RiskFocus);
		ExecuteScriptSurface->TryGetStringField(TEXT("governance_bucket_id"), GovernanceBucketId);
		TestEqual(TEXT("execute_script should stay high-risk execution focused"), RiskFocus, FString(TEXT("high_risk_execution_surface")));
		TestEqual(TEXT("execute_script should stay in high-risk execution backlog"), GovernanceBucketId, FString(TEXT("high_risk_execution_backlog")));
		TestTrue(TEXT("execute_script default behavior should exist"), DefaultBehavior.IsValid());
		TestTrue(TEXT("execute_script bounded behavior should exist"), BoundedBehavior.IsValid());
		if (DefaultBehavior.IsValid())
		{
			FString BehaviorState;
			FString PolicyRuleId;
			bool bBehaviorEnforcedNow = false;
			DefaultBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			DefaultBehavior->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId);
			DefaultBehavior->TryGetBoolField(TEXT("behavior_enforced_now"), bBehaviorEnforcedNow);
			TestEqual(TEXT("execute_script should now show direct deny on safer default"), BehaviorState, FString(TEXT("direct_policy_denied_representative_high_risk_execution_surface")));
			TestEqual(TEXT("execute_script should keep workspace-write default representative deny policy rule"), PolicyRuleId, FString(TEXT("workspace_write_project.representative_high_risk_execution_surface_denied")));
			TestTrue(TEXT("execute_script safer-default behavior should be enforced now"), bBehaviorEnforcedNow);
		}
		if (BoundedBehavior.IsValid())
		{
			FString BehaviorState;
			FString PolicyRuleId;
			bool bBehaviorEnforcedNow = false;
			BoundedBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			BoundedBehavior->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId);
			BoundedBehavior->TryGetBoolField(TEXT("behavior_enforced_now"), bBehaviorEnforcedNow);
			TestEqual(TEXT("execute_script should now show direct deny on bounded lane"), BehaviorState, FString(TEXT("direct_policy_denied_representative_high_risk_execution_surface")));
			TestEqual(TEXT("execute_script should keep bounded representative deny policy rule"), PolicyRuleId, FString(TEXT("bounded_plugin_mutation.representative_high_risk_execution_surface_denied")));
			TestTrue(TEXT("execute_script bounded behavior should be enforced now"), bBehaviorEnforcedNow);
		}
	}

	const TSharedPtr<FJsonObject> TaskSubmitSurface = FindRiskySurface(ManifestObject, TEXT("task_submit"));
	TestTrue(TEXT("task_submit risky surface should exist"), TaskSubmitSurface.IsValid());
	if (TaskSubmitSurface.IsValid())
	{
		const TSharedPtr<FJsonObject> DefaultBehavior = GetObjectFieldOrNull(TaskSubmitSurface, TEXT("configured_default_runtime"));
		const TSharedPtr<FJsonObject> BoundedBehavior = GetObjectFieldOrNull(TaskSubmitSurface, TEXT("bounded_plugin_mutation"));
		TestTrue(TEXT("task_submit default behavior should exist"), DefaultBehavior.IsValid());
		TestTrue(TEXT("task_submit bounded behavior should exist"), BoundedBehavior.IsValid());
		if (DefaultBehavior.IsValid())
		{
			bool bBehaviorEnforcedNow = false;
			FString BehaviorState;
			DefaultBehavior->TryGetBoolField(TEXT("behavior_enforced_now"), bBehaviorEnforcedNow);
			DefaultBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			TestTrue(TEXT("task_submit safer-default behavior should be enforced now"), bBehaviorEnforcedNow);
			TestEqual(TEXT("task_submit should now show direct deny on safer default"), BehaviorState, FString(TEXT("direct_policy_denied_representative_high_risk_execution_surface")));
		}
		if (BoundedBehavior.IsValid())
		{
			bool bBehaviorEnforcedNow = false;
			FString BehaviorState;
			BoundedBehavior->TryGetBoolField(TEXT("behavior_enforced_now"), bBehaviorEnforcedNow);
			BoundedBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			TestTrue(TEXT("task_submit bounded behavior should be enforced now"), bBehaviorEnforcedNow);
			TestEqual(TEXT("task_submit should now show direct deny on bounded lane"), BehaviorState, FString(TEXT("direct_policy_denied_representative_high_risk_execution_surface")));
		}
	}

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
		const TSharedPtr<FJsonObject> Surface = FindRiskySurface(ManifestObject, ToolName);
		TestTrue(FString::Printf(TEXT("%s risky surface should exist"), *ToolName), Surface.IsValid());
		if (!Surface.IsValid())
		{
			continue;
		}

		FString RiskFocus;
		FString GovernanceBucketId;
		const TSharedPtr<FJsonObject> DefaultBehavior = GetObjectFieldOrNull(Surface, TEXT("configured_default_runtime"));
		const TSharedPtr<FJsonObject> BoundedBehavior = GetObjectFieldOrNull(Surface, TEXT("bounded_plugin_mutation"));
		const TSharedPtr<FJsonObject> ExpertBehavior = GetObjectFieldOrNull(Surface, TEXT("explicit_expert_opt_in"));
		Surface->TryGetStringField(TEXT("risk_focus"), RiskFocus);
		Surface->TryGetStringField(TEXT("governance_bucket_id"), GovernanceBucketId);
		TestEqual(FString::Printf(TEXT("%s should stay high-risk execution focused"), *ToolName), RiskFocus, FString(TEXT("high_risk_execution_surface")));
		TestEqual(FString::Printf(TEXT("%s should stay in external UI backlog"), *ToolName), GovernanceBucketId, FString(TEXT("external_ui_control_backlog")));
		TestTrue(FString::Printf(TEXT("%s default behavior should exist"), *ToolName), DefaultBehavior.IsValid());
		TestTrue(FString::Printf(TEXT("%s bounded behavior should exist"), *ToolName), BoundedBehavior.IsValid());
		TestTrue(FString::Printf(TEXT("%s expert behavior should exist"), *ToolName), ExpertBehavior.IsValid());
		if (DefaultBehavior.IsValid())
		{
			FString BehaviorState;
			FString PolicyRuleId;
			FString EnforcementState;
			bool bBehaviorEnforcedNow = false;
			DefaultBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			DefaultBehavior->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId);
			DefaultBehavior->TryGetStringField(TEXT("enforcement_state"), EnforcementState);
			DefaultBehavior->TryGetBoolField(TEXT("behavior_enforced_now"), bBehaviorEnforcedNow);
			TestEqual(FString::Printf(TEXT("%s should now show direct deny on safer default"), *ToolName), BehaviorState, FString(TEXT("direct_policy_denied_external_ui_control_surface")));
			TestEqual(FString::Printf(TEXT("%s should keep workspace-write default external UI deny policy rule"), *ToolName), PolicyRuleId, FString(TEXT("workspace_write_project.external_ui_control_surface_denied")));
			TestEqual(FString::Printf(TEXT("%s safer-default enforcement state should be direct policy deny"), *ToolName), EnforcementState, FString(TEXT("direct_policy_deny_contract")));
			TestTrue(FString::Printf(TEXT("%s safer-default behavior should be enforced now"), *ToolName), bBehaviorEnforcedNow);
		}
		if (BoundedBehavior.IsValid())
		{
			FString BehaviorState;
			FString PolicyRuleId;
			FString EnforcementState;
			bool bBehaviorEnforcedNow = false;
			BoundedBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			BoundedBehavior->TryGetStringField(TEXT("policy_rule_id"), PolicyRuleId);
			BoundedBehavior->TryGetStringField(TEXT("enforcement_state"), EnforcementState);
			BoundedBehavior->TryGetBoolField(TEXT("behavior_enforced_now"), bBehaviorEnforcedNow);
			TestEqual(FString::Printf(TEXT("%s should now show direct deny on bounded lane"), *ToolName), BehaviorState, FString(TEXT("direct_policy_denied_external_ui_control_surface")));
			TestEqual(FString::Printf(TEXT("%s should keep bounded external UI deny policy rule"), *ToolName), PolicyRuleId, FString(TEXT("bounded_plugin_mutation.external_ui_control_surface_denied")));
			TestEqual(FString::Printf(TEXT("%s bounded enforcement state should be direct policy deny"), *ToolName), EnforcementState, FString(TEXT("direct_policy_deny_contract")));
			TestTrue(FString::Printf(TEXT("%s bounded behavior should be enforced now"), *ToolName), bBehaviorEnforcedNow);
		}
		if (ExpertBehavior.IsValid())
		{
			FString BehaviorState;
			ExpertBehavior->TryGetStringField(TEXT("behavior_state"), BehaviorState);
			TestEqual(FString::Printf(TEXT("%s should stay reachable only on explicit expert lane"), *ToolName), BehaviorState, FString(TEXT("reachable_via_enabled_unreal_mcp_bridge")));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Buckets = nullptr;
	TestTrue(TEXT("risky surface backlog buckets should exist"), CoverageObject->TryGetArrayField(TEXT("backlog_buckets"), Buckets) && Buckets);
	if (!Buckets)
	{
		return false;
	}

	bool bFoundBroadMutationBucket = false;
	bool bFoundExternalUiBucket = false;
	for (const TSharedPtr<FJsonValue>& BucketValue : *Buckets)
	{
		const TSharedPtr<FJsonObject> BucketObject = BucketValue.IsValid() ? BucketValue->AsObject() : nullptr;
		if (!BucketObject.IsValid())
		{
			continue;
		}

		FString BucketId;
		BucketObject->TryGetStringField(TEXT("bucket_id"), BucketId);
		if (BucketId == TEXT("broad_authoring_mutation_backlog"))
		{
			bFoundBroadMutationBucket =
				JsonStringArrayContains(BucketObject, TEXT("surface_keys"), TEXT("spawn_actor"))
				&& JsonStringArrayContains(BucketObject, TEXT("surface_keys"), TEXT("material"));
		}
		else if (BucketId == TEXT("external_ui_control_backlog"))
		{
			bFoundExternalUiBucket = JsonStringArrayContains(BucketObject, TEXT("surface_keys"), TEXT("osvayder_mouse_click"));
		}
	}

	TestTrue(TEXT("broad authoring mutation backlog should include spawn_actor and material"), bFoundBroadMutationBucket);
	TestTrue(TEXT("external UI control backlog should include osvayder_mouse_click"), bFoundExternalUiBucket);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
