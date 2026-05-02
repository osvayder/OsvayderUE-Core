// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentExecutionControl.h"
#include "Dom/JsonObject.h"

enum class EAgentCapabilityFamilyMode : uint8
{
	ReadOnly,
	MutationCapable,
	Mixed,
};

UNREALCLAUDE_API const TCHAR* AgentCapabilityFamilyModeToString(EAgentCapabilityFamilyMode Mode);

struct UNREALCLAUDE_API FAgentCapabilityRiskFamilyManifest
{
	FString FamilyId;
	FString DisplayName;
	FString Purpose;
	EAgentCapabilityFamilyMode CapabilityMode = EAgentCapabilityFamilyMode::ReadOnly;
	FString MutationClass;
	FString RiskClass;
	FString RequiredProofTier;
	FString ReverificationExpectation;
	FString Revertability;
	FString CheckpointExpectation;
	FString StopCondition;
	bool bWritesUserOwnedState = false;
	bool bWritesPluginOwnedState = false;
	bool bRequiresFreshBuildForAcceptance = false;
	bool bRequiresRepresentativeLiveProof = false;
	bool bRequiresRevertOrCheckpointForMutation = false;
	TArray<FString> RepresentativeSurfaces;
	TArray<FString> MajorControlDependencies;
	TArray<FString> CurrentRealityBasis;
	FString TruthBoundary;
};

struct UNREALCLAUDE_API FAgentCapabilityRiskToolMapping
{
	FString ToolName;
	FString SurfaceIdentifier;
	FString MappingScope;
	FString GoverningFamilyId;
	FString GoverningFamilyDisplayName;
	FString AmbiguityState;
	FString AmbiguityDetail;
	EAgentCapabilityFamilyMode CapabilityMode = EAgentCapabilityFamilyMode::ReadOnly;
	FString MutationClass;
	FString RiskClass;
	FString RequiredProofTier;
	FString Revertability;
	FString CheckpointExpectation;
	FString StopCondition;
	bool bWritesUserOwnedState = false;
	bool bWritesPluginOwnedState = false;
	bool bRequiresFreshBuildForAcceptance = false;
	bool bRequiresRepresentativeLiveProof = false;
	bool bRequiresRevertOrCheckpointForMutation = false;
	FString TruthBoundary;
};

struct UNREALCLAUDE_API FAgentCapabilityRiskToolCoverageBucket
{
	FString BucketId;
	FString DisplayName;
	TArray<FString> ToolNames;
};

struct UNREALCLAUDE_API FAgentCapabilityRiskToolCoverageManifest
{
	FString SchemaVersion = TEXT("capability_risk_tool_mapping_coverage_v1");
	FString ManifestSource = TEXT("live_registry_vs_partial_mapping_truth");
	FString RegistryInventorySource;
	int32 RegistryToolCount = 0;
	int32 MappedToolCount = 0;
	int32 UnmappedToolCount = 0;
	double MappingCoverageRatio = 0.0;
	TArray<FString> MappedToolNames;
	TArray<FString> UnmappedToolNames;
	TArray<FAgentCapabilityRiskToolCoverageBucket> UnmappedBuckets;
	FString TruthBoundary;
};

struct UNREALCLAUDE_API FAgentCapabilityRiskySurfaceCoverageBucket
{
	FString BucketId;
	FString DisplayName;
	TArray<FString> SurfaceKeys;
};

struct UNREALCLAUDE_API FAgentCapabilityRiskySurfaceLaneBehavior
{
	FString ControlProfileId;
	FString ExecutionProfile;
	FString RuntimeLane;
	FString ExecutionTransport;
	FString SessionPersistenceMode;
	FString BehaviorState;
	FString PolicyRuleId;
	bool bBehaviorEnforcedNow = false;
	FString EnforcementState;
	TArray<FString> Basis;
	FString TruthBoundary;
};

struct UNREALCLAUDE_API FAgentCapabilityRiskySurfaceEntry
{
	FString ToolName;
	FString SurfaceIdentifier;
	FString SurfaceKind;
	FString RiskFocus;
	FString GovernanceState;
	FString GovernanceBucketId;
	FString GovernanceBucketDisplayName;
	FString MappingScope;
	FString FamilyMappingState;
	FString GoverningFamilyId;
	FString GoverningFamilyDisplayName;
	FString MutationClass;
	FString RiskClass;
	bool bSafeOrBoundedBehaviorExplicit = false;
	FAgentCapabilityRiskySurfaceLaneBehavior ConfiguredDefaultRuntime;
	FAgentCapabilityRiskySurfaceLaneBehavior BoundedPluginMutation;
	FAgentCapabilityRiskySurfaceLaneBehavior ExplicitExpertOptIn;
	FString TruthBoundary;
};

struct UNREALCLAUDE_API FAgentCapabilityRiskySurfaceCoverageManifest
{
	FString SchemaVersion = TEXT("capability_risk_risky_surface_coverage_v1");
	FString ManifestSource = TEXT("high_risk_and_mutation_surface_governance_baseline");
	FString ClassificationLevel = TEXT("risky_tool_or_runtime_surface");
	int32 SurfaceCount = 0;
	int32 HighRiskExecutionSurfaceCount = 0;
	int32 MutationCapableSurfaceCount = 0;
	int32 FamilyMappedSurfaceCount = 0;
	int32 ExplicitBacklogSurfaceCount = 0;
	int32 SaferLaneBehaviorExplicitCount = 0;
	int32 SaferLaneAmbiguousSurfaceCount = 0;
	int32 SafeOrBoundedEnforcedSurfaceCount = 0;
	int32 SafeOrBoundedDescribedOnlySurfaceCount = 0;
	TArray<FString> FamilyMappedSurfaceKeys;
	TArray<FString> ExplicitBacklogSurfaceKeys;
	TArray<FAgentCapabilityRiskySurfaceCoverageBucket> BacklogBuckets;
	TArray<FAgentCapabilityRiskySurfaceEntry> Surfaces;
	FString TruthBoundary;
};

struct UNREALCLAUDE_API FAgentCppScopeCategoryManifest
{
	FString CategoryId;
	FString DisplayName;
	FString ScopeState;
	FString CurrentAvailability;
	FString Purpose;
	FString MutationBreadth;
	FString RequiredProofTier;
	FString RebuildExpectation;
	FString RevertExpectation;
	FString CheckpointExpectation;
	FString StopCondition;
	bool bRequiresFreshBuildForAcceptance = false;
	bool bRequiresRepresentativeLiveProof = false;
	bool bRequiresRestoreOrRevertForMutation = false;
	TArray<FString> RepresentativeSurfaces;
	TArray<FString> CurrentLaneMappings;
	TArray<FString> CurrentRealityBasis;
	FString TruthBoundary;
};

struct UNREALCLAUDE_API FAgentCppScopeDefinitionManifest
{
	FString SchemaVersion = TEXT("cpp_scope_definition_v1");
	FString ManifestSource = TEXT("post_ultra_cpp_scope_definition_foundation");
	FString ScopeBoundary = TEXT("plugin_only_cpp_scope_governance");
	FString TruthBoundary;
	TArray<FAgentCppScopeCategoryManifest> AllowedNow;
	TArray<FAgentCppScopeCategoryManifest> FutureTarget;
	TArray<FAgentCppScopeCategoryManifest> ForbiddenNow;
};

struct UNREALCLAUDE_API FAgentCapabilityRiskToolMappingManifest
{
	FString SchemaVersion = TEXT("capability_risk_tool_mapping_v1");
	FString ManifestSource = TEXT("tool_family_obligation_readback_foundation");
	FString ClassificationLevel = TEXT("concrete_tool_or_surface");
	FString TruthBoundary;
	TArray<FAgentCapabilityRiskToolMapping> ToolMappings;
	FAgentCapabilityRiskToolCoverageManifest Coverage;
};

struct UNREALCLAUDE_API FAgentCapabilityRiskManifest
{
	FString SchemaVersion = TEXT("capability_risk_manifest_v1");
	FString ManifestSource = TEXT("plugin_family_classification_foundation");
	FString ClassificationLevel = TEXT("family");
	FString ScopeBoundary = TEXT("plugin_only_governance_foundation");
	FString TruthBoundary;
	TArray<FAgentCapabilityRiskFamilyManifest> Families;
	FAgentCapabilityRiskySurfaceCoverageManifest RiskySurfaceCoverage;
	FAgentCppScopeDefinitionManifest CppScopeDefinition;
	FAgentCapabilityRiskToolMappingManifest ToolMapping;
};

UNREALCLAUDE_API FAgentCapabilityRiskManifest BuildAgentCapabilityRiskManifest(
	const FAgentProviderExecutionControlManifest& DefaultRuntimeManifest,
	const FAgentProviderExecutionControlManifest& ReadOnlyHelperManifest,
	const FAgentProviderExecutionControlManifest& BoundedRuntimeManifest,
	const FAgentProviderExecutionControlManifest& ExplicitExpertManifest);

UNREALCLAUDE_API TSharedPtr<FJsonObject> MakeAgentCapabilityRiskManifestJson(
	const FAgentCapabilityRiskManifest& Manifest);
