// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "AgentExecutionControl.h"
#include "AgentPromptContract.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClaudeAgentTrace.h"
#include "UnrealClaudeCanonRouting.h"
#include "UnrealClaudeExecutionLog.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString MakeFreshCanonRoutingTestRoot(const FString& TestName)
	{
		FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UnrealClaude"),
			TEXT("Automation"),
			TEXT("CanonRouting"),
			TestName);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	struct FScopedCanonLedgerOverride
	{
		explicit FScopedCanonLedgerOverride(const FString& InLedgerPath)
		{
			FUnrealClaudeCanonLedger::SetTestLedgerPathOverride(InLedgerPath);
		}

		~FScopedCanonLedgerOverride()
		{
			FUnrealClaudeCanonLedger::ClearTestLedgerPathOverride();
		}
	};

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

	FAgentBackendStatus MakeSyntheticBackendStatus()
	{
		FAgentBackendStatus Status;
		Status.Backend = EUnrealClaudeProviderBackend::CodexCli;
		Status.DisplayName = TEXT("Codex CLI");
		Status.bAvailable = true;
		Status.bReady = true;
		Status.Readiness = EAgentBackendReadiness::Ready;
		Status.AuthState = EAgentBackendAuthState::Authenticated;
		return Status;
	}

	bool SaveJsonObjectToFile(const FString& Path, const TSharedPtr<FJsonObject>& Root)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
		{
			return false;
		}

		return FFileHelper::SaveStringToFile(JsonText, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool WriteSyntheticProvenRunReceipt(
		const FString& ReceiptPath,
		const FString& Prompt,
		const FString& Subsystem,
		const FString& RequestedToolFamily)
	{
		FAgentCanonExecution CanonExecution;
		CanonExecution.DetectedSubsystem = Subsystem;
		CanonExecution.TaskMode = TEXT("canon_discovery");
		CanonExecution.bCanonDiscoveryUsed = true;
		CanonExecution.bBriefWasProduced = true;
		CanonExecution.bBriefPartBRequired = true;
		CanonExecution.bBriefPartBProduced = true;
		CanonExecution.bBriefPartBProducedBeforeFirstMutatingTool = true;
		CanonExecution.RequestedToolFamily = RequestedToolFamily;
		CanonExecution.ActualToolFamily = RequestedToolFamily;
		CanonExecution.PrimaryMutationToolFamily = RequestedToolFamily;
		CanonExecution.bFallbackToolUsed = false;
		CanonExecution.bMutatingFallbackUsed = false;
		CanonExecution.VerificationOutcome = TEXT("pass");
		CanonExecution.ImplementationBriefLines = {
			FString::Printf(TEXT("subsystem = %s"), *Subsystem),
			FString::Printf(TEXT("requested_tool_family = %s"), *RequestedToolFamily),
			TEXT("approved_pattern = false"),
			TEXT("source_priority = project_local -> official_unreal_docs -> official_first_party_samples -> community_support_only")
		};

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("run_tag"), FPaths::GetBaseFilename(ReceiptPath));
		Root->SetStringField(TEXT("state"), TEXT("completed"));
		Root->SetStringField(TEXT("prompt"), Prompt);
		Root->SetBoolField(TEXT("success"), true);
		Root->SetStringField(TEXT("response"), TEXT("SYNTHETIC_CANON_PROOF_OK"));
		Root->SetObjectField(TEXT("canon_execution"), UnrealClaudeCanonRouting::MakeCanonExecutionJson(CanonExecution));
		return SaveJsonObjectToFile(ReceiptPath, Root);
	}

	void SeedInputAndAnimationLedger(
		FAutomationTestBase& Test,
		const FString& TestRoot,
		TArray<FUnrealClaudeCanonLedgerEntry>& OutEntries)
	{
		const FString BackendRunRoot = FPaths::Combine(TestRoot, TEXT("backend_runs"));
		const FString InputReceiptPath = FPaths::Combine(BackendRunRoot, TEXT("p17_seed_input.json"));
		const FString AnimationReceiptPath = FPaths::Combine(BackendRunRoot, TEXT("p17_seed_animation.json"));

		Test.TestTrue(
			TEXT("input proven receipt should be written"),
			WriteSyntheticProvenRunReceipt(
				InputReceiptPath,
				TEXT("Create a sprint input action and bind it through Enhanced Input."),
				TEXT("input"),
				TEXT("unreal_input")));
		Test.TestTrue(
			TEXT("animation proven receipt should be written"),
			WriteSyntheticProvenRunReceipt(
				AnimationReceiptPath,
				TEXT("Create a new AnimBP state machine through native animation tooling."),
				TEXT("animation"),
				TEXT("unreal_animation")));

		FString Error;
		FUnrealClaudeCanonPromotionRequest InputRequest;
		InputRequest.ProvenRunReceiptPath = InputReceiptPath;
		InputRequest.PatternKey = TEXT("input.sprint_binding");
		InputRequest.ExpectedSubsystem = TEXT("input");
		InputRequest.ShortTitle = TEXT("Sprint binding through Enhanced Input");
		InputRequest.ChosenPathSummary = TEXT("Use native Enhanced Input assets and mappings before any fallback lane.");
		InputRequest.WhyPreferred = TEXT("This project already routes player bindings through Enhanced Input assets.");
		InputRequest.BadPathToAvoid = TEXT("Do not start by patching shell files or UI-driving for input asset work.");
		InputRequest.LastConfirmedEngineContext = TEXT("UE5.7");

		FUnrealClaudeCanonLedgerEntry InputEntry;
		Test.TestTrue(
			TEXT("input proven receipt should promote into the ledger"),
			FUnrealClaudeCanonLedger::PromoteFromProvenRun(InputRequest, Error, &InputEntry));

		FUnrealClaudeCanonPromotionRequest AnimationRequest;
		AnimationRequest.ProvenRunReceiptPath = AnimationReceiptPath;
		AnimationRequest.PatternKey = TEXT("animation.state_machine_creation");
		AnimationRequest.ExpectedSubsystem = TEXT("animation");
		AnimationRequest.ShortTitle = TEXT("Animation state machine creation through AnimBP tooling");
		AnimationRequest.ChosenPathSummary = TEXT("Use native animation blueprint mutation before generic blueprint or shell fallback.");
		AnimationRequest.WhyPreferred = TEXT("The proof fixture already validates state-machine creation on the native animation path.");
		AnimationRequest.BadPathToAvoid = TEXT("Do not start with workspace shell edits for AnimBP graph mutation.");
		AnimationRequest.LastConfirmedEngineContext = TEXT("UE5.7");

		FUnrealClaudeCanonLedgerEntry AnimationEntry;
		Test.TestTrue(
			TEXT("animation proven receipt should promote into the ledger"),
			FUnrealClaudeCanonLedger::PromoteFromProvenRun(AnimationRequest, Error, &AnimationEntry));

		OutEntries = { InputEntry, AnimationEntry };
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_PromptContractTruth,
	"UnrealClaude.CanonRouting.PromptContractTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_LedgerRequiresExplicitPromotion,
	"UnrealClaude.CanonRouting.LedgerRequiresExplicitPromotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_BuildInitialExecutionUsesLedgerAndBrief,
	"UnrealClaude.CanonRouting.BuildInitialExecutionUsesLedgerAndBrief",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_ReceiptJsonCarriesCanonExecution,
	"UnrealClaude.CanonRouting.ReceiptJsonCarriesCanonExecution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_RuntimeToolExposureUsesCanonicalFamilies,
	"UnrealClaude.CanonRouting.RuntimeToolExposureUsesCanonicalFamilies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_PromoteFromProvenRunSeedsLedger,
	"UnrealClaude.CanonRouting.PromoteFromProvenRunSeedsLedger",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_SameSubsystemDifferentPromptUsesDirectedExecution,
	"UnrealClaude.CanonRouting.SameSubsystemDifferentPromptUsesDirectedExecution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_CrossSubsystemPromptDoesNotFalsePositive,
	"UnrealClaude.CanonRouting.CrossSubsystemPromptDoesNotFalsePositive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_ProjectLedgerSeedFromAcceptedProofs,
	"UnrealClaude.CanonRouting.ProjectLedgerSeedFromAcceptedProofs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_TraceRunIncludesCanonSummary,
	"UnrealClaude.CanonRouting.TraceRunIncludesCanonSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_AnimationReadOnlyToolUseDoesNotConsumeMutationSlot,
	"UnrealClaude.CanonRouting.AnimationReadOnlyToolUseDoesNotConsumeMutationSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_InventoryFeatureSliceSelectsRecipe,
	"UnrealClaude.CanonRouting.InventoryFeatureSliceSelectsRecipe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_InteractionAccessFeatureSliceSelectsRecipe,
	"UnrealClaude.CanonRouting.InteractionAccessFeatureSliceSelectsRecipe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_ProductAcceptancePromptsDoNotSelectInteractionAccess,
	"UnrealClaude.CanonRouting.ProductAcceptancePromptsDoNotSelectInteractionAccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_GenericGameplayExternalAssetPromptCarriesChecklistGuard,
	"UnrealClaude.CanonRouting.GenericGameplayExternalAssetPromptCarriesChecklistGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_GenericGameplayInputConflictPromptCarriesGuard,
	"UnrealClaude.CanonRouting.GenericGameplayInputConflictPromptCarriesGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_FeatureSlicePromptCarriesRoleAndRecipeContract,
	"UnrealClaude.CanonRouting.FeatureSlicePromptCarriesRoleAndRecipeContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_InteractionAccessResumeInputPhaseRefreshesContextAndPrefersInputTools,
	"UnrealClaude.CanonRouting.InteractionAccessResumeInputPhaseRefreshesContextAndPrefersInputTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_ItemOnlyPromptDoesNotFalsePositiveInventory,
	"UnrealClaude.CanonRouting.ItemOnlyPromptDoesNotFalsePositiveInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_FeatureReceiptCarriesTopLevelWorkflowSummary,
	"UnrealClaude.CanonRouting.FeatureReceiptCarriesTopLevelWorkflowSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCanonRouting_FeatureTraceRunIncludesWorkflowEvents,
	"UnrealClaude.CanonRouting.FeatureTraceRunIncludesWorkflowEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCanonRouting_PromptContractTruth::RunTest(const FString& Parameters)
{
	const FAgentPromptContract Contract = FAgentPromptContractBuilder::Build(true, false, FString(), FString());
	const FString Materialized = FAgentPromptMaterializer::MaterializeCanonicalText(Contract);

	TestTrue(TEXT("prompt should include the task mode classifier section"), Materialized.Contains(TEXT("TASK MODE CLASSIFIER"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should mention canon_discovery"), Materialized.Contains(TEXT("canon_discovery"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should mention directed_execution"), Materialized.Contains(TEXT("directed_execution"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should encode strict source priority"), Materialized.Contains(TEXT("project-local conventions, docs, and proven patterns"), ESearchCase::IgnoreCase)
		&& Materialized.Contains(TEXT("official Unreal documentation"), ESearchCase::IgnoreCase)
		&& Materialized.Contains(TEXT("official samples"), ESearchCase::IgnoreCase)
		&& Materialized.Contains(TEXT("community forums, GitHub"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should require an implementation brief"), Materialized.Contains(TEXT("Implementation Brief"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should require Implementation Brief Part B before the first mutating tool"), Materialized.Contains(TEXT("Implementation Brief Part B"), ESearchCase::IgnoreCase)
		&& Materialized.Contains(TEXT("before the first mutating tool call"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should include external asset blocker policy"),
		Materialized.Contains(TEXT("manual_asset_dependency_blocker"), ESearchCase::IgnoreCase)
			&& Materialized.Contains(TEXT("Fab/Marketplace/Epic Launcher"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should include acceptance checklist extraction"),
		Materialized.Contains(TEXT("ACCEPTANCE CHECKLIST EXTRACTION"), ESearchCase::CaseSensitive)
			&& Materialized.Contains(TEXT("runtime/PIE or screenshot evidence"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should include mechanic input conflict guard"),
		Materialized.Contains(TEXT("MECHANIC INPUT CONFLICT GUARD"), ESearchCase::CaseSensitive)
			&& Materialized.Contains(TEXT("mechanic_input_conflict_unresolved"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should include post-reattach tool surface blocker"),
		Materialized.Contains(TEXT("blocked_on_tool_surface"), ESearchCase::IgnoreCase)
			&& Materialized.Contains(TEXT("runtime/capture tools"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("tool rules should include routing few-shots"), Materialized.Contains(TEXT("task: fix an AnimBP transition"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("prompt should mention the interaction access recipe"), Materialized.Contains(TEXT("feature.interaction_access_slice_v1"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("prompt should require dedicated enhanced_input verification first for interaction-access input reuse"),
		Materialized.Contains(TEXT("current_phase = input_asset_authoring"), ESearchCase::IgnoreCase)
			&& Materialized.Contains(TEXT("enhanced_input"), ESearchCase::IgnoreCase)
			&& Materialized.Contains(TEXT("proof-map, actor, and viewport inspection belong to later phases"), ESearchCase::IgnoreCase));
	return true;
}

bool FCanonRouting_LedgerRequiresExplicitPromotion::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshCanonRoutingTestRoot(TEXT("LedgerRequiresExplicitPromotion"));
	const FString LedgerPath = FPaths::Combine(TestRoot, TEXT("canon_ledger.json"));
	FScopedCanonLedgerOverride LedgerOverride(LedgerPath);

	TArray<FAgentPromptContextBlock> ContextBlocks;
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Fix the input mapping context binding for the sprint action."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		ContextBlocks);

	TestEqual(TEXT("input prompt should classify as input subsystem"), Execution.DetectedSubsystem, FString(TEXT("input")));
	TestFalse(TEXT("ledger should not be created by prompt classification alone"), IFileManager::Get().FileExists(*LedgerPath));

	FUnrealClaudeCanonLedgerEntry Entry;
	Entry.PatternKey = TEXT("input.sprint.binding");
	Entry.Subsystem = TEXT("input");
	Entry.ShortTitle = TEXT("Sprint binding through Enhanced Input");
	Entry.ChosenPathSummary = TEXT("Prefer enhanced_input mapping updates before any UI-driving fallback.");
	Entry.WhyPreferred = TEXT("The project already routes sprint through Enhanced Input assets.");
	Entry.ProofReference = TEXT("Saved/UnrealClaude/backend_runs/p15_input_binding_proof.json");
	Entry.LastConfirmedEngineContext = TEXT("UE5.7");

	FString Error;
	TestTrue(TEXT("explicit promotion should create the ledger"), FUnrealClaudeCanonLedger::PromoteEntry(Entry, Error));
	TestTrue(TEXT("canon ledger file should exist after explicit promotion"), IFileManager::Get().FileExists(*LedgerPath));
	return true;
}

bool FCanonRouting_BuildInitialExecutionUsesLedgerAndBrief::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshCanonRoutingTestRoot(TEXT("BuildInitialExecutionUsesLedgerAndBrief"));
	const FString LedgerPath = FPaths::Combine(TestRoot, TEXT("canon_ledger.json"));
	FScopedCanonLedgerOverride LedgerOverride(LedgerPath);

	FUnrealClaudeCanonLedgerEntry Entry;
	Entry.PatternKey = TEXT("input.sprint.binding");
	Entry.Subsystem = TEXT("input");
	Entry.ShortTitle = TEXT("Sprint binding through Enhanced Input");
	Entry.ChosenPathSummary = TEXT("Use enhanced_input assets instead of ad hoc widget or shell edits.");
	Entry.WhyPreferred = TEXT("This project already uses Enhanced Input as the approved path.");
	Entry.ProofReference = TEXT("Saved/UnrealClaude/backend_runs/p15_input_binding_proof.json");
	Entry.BadPathToAvoid = TEXT("Do not patch input through UI-driving or forum snippets first.");
	Entry.LastConfirmedEngineContext = TEXT("UE5.7");

	FString Error;
	(void)FUnrealClaudeCanonLedger::PromoteEntry(Entry, Error);

	TArray<FAgentPromptContextBlock> ContextBlocks;
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Fix the sprint input mapping context and keep the approved local path."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		ContextBlocks);

	TestTrue(TEXT("approved pattern should be found from the project-local ledger"), Execution.bApprovedPatternFound);
	TestEqual(TEXT("risky subsystem with approved pattern should use directed execution"), Execution.TaskMode, FString(TEXT("directed_execution")));
	TestTrue(TEXT("risky directed execution should still produce a brief"), Execution.bBriefWasProduced);
	TestTrue(TEXT("risky directed execution should require brief part B"), Execution.bBriefPartBRequired);
	TestTrue(TEXT("brief should keep the required 3-5 line shape"), Execution.ImplementationBriefLines.Num() >= 3 && Execution.ImplementationBriefLines.Num() <= 5);

	const FAgentPromptContextBlock* ClassificationBlock = ContextBlocks.FindByPredicate([](const FAgentPromptContextBlock& Block)
	{
		return Block.Label == TEXT("TASK MODE CLASSIFICATION");
	});
	const FAgentPromptContextBlock* LedgerBlock = ContextBlocks.FindByPredicate([](const FAgentPromptContextBlock& Block)
	{
		return Block.Label == TEXT("PROJECT-LOCAL CANON LEDGER");
	});
	const FAgentPromptContextBlock* BriefBlock = ContextBlocks.FindByPredicate([](const FAgentPromptContextBlock& Block)
	{
		return Block.Label == TEXT("RUN-SCOPED IMPLEMENTATION BRIEF PART A");
	});

	TestNotNull(TEXT("task mode classification block should be added"), ClassificationBlock);
	TestNotNull(TEXT("project-local ledger block should be added"), LedgerBlock);
	TestNotNull(TEXT("run-scoped brief block should be added"), BriefBlock);
	if (LedgerBlock)
	{
		TestTrue(TEXT("ledger block should name the approved pattern"), LedgerBlock->Content.Contains(TEXT("input.sprint.binding"), ESearchCase::IgnoreCase));
	}

	return true;
}

bool FCanonRouting_RuntimeToolExposureUsesCanonicalFamilies::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshCanonRoutingTestRoot(TEXT("RuntimeToolExposureUsesCanonicalFamilies"));
	const FString LedgerPath = FPaths::Combine(TestRoot, TEXT("canon_ledger.json"));
	FScopedCanonLedgerOverride LedgerOverride(LedgerPath);

	FUnrealClaudeCanonLedgerEntry Entry;
	Entry.PatternKey = TEXT("input.sprint.binding");
	Entry.Subsystem = TEXT("input");
	Entry.ShortTitle = TEXT("Sprint binding through Enhanced Input");
	Entry.ChosenPathSummary = TEXT("Use enhanced_input assets instead of ad hoc widget or shell edits.");
	Entry.WhyPreferred = TEXT("This project already uses Enhanced Input as the approved path.");
	Entry.ProofReference = TEXT("Saved/UnrealClaude/backend_runs/p16_input_binding_proof.json");
	Entry.BadPathToAvoid = TEXT("Do not start with workspace shell mutation for an input asset change.");
	Entry.LastConfirmedEngineContext = TEXT("UE5.7");

	FString Error;
	(void)FUnrealClaudeCanonLedger::PromoteEntry(Entry, Error);

	FAgentRequestConfig Config;
	Config.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	Config.bSkipPermissions = false;
	Config.bExecutionProfileEnforcedNow = true;
	Config.bEnableUnrealMcpBridge = false;
	Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
	Config.ExecutionControlProfileId = TEXT("workspace_write_default_runtime_v1");
	Config.ExecutionTransportLabel = TEXT("persistent_app_server");
	Config.ExecutionControlPlumbingState = EAgentExecutionGovernanceState::Enforced;
	Config.DesiredFutureDefaultProviderPowerClass = EAgentExecutionPowerClass::WorkspaceWriteProject;
	Config.AllowedTools = {
		TEXT("Read"),
		TEXT("Write"),
		TEXT("Edit"),
		TEXT("Grep"),
		TEXT("Glob"),
		TEXT("Bash"),
		TEXT("mcp__unrealclaude__restart_survival")
	};
	Config.CanonExecution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Fix the sprint input mapping context and keep the approved local path."),
		Config.ExecutionProfile,
		Config.PromptContract.ContextBlocks);

	UnrealClaudeCanonRouting::ApplyRuntimeToolExposure(Config);

	TestEqual(TEXT("requested tool family should stay unreal_input"), Config.CanonExecution.RequestedToolFamily, FString(TEXT("unreal_input")));
	TestTrue(TEXT("runtime tool exposure should mark the config as adjusted"), Config.CanonExecution.bToolExposureAdjusted);
	TestTrue(TEXT("enabled families should include workspace inspection"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("workspace_inspection")));
	TestTrue(TEXT("enabled families should include unreal_input"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("unreal_input")));
	TestTrue(TEXT("enabled families should include unreal_blueprint"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("unreal_blueprint")));
	TestTrue(TEXT("enabled families should include unreal_assets"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("unreal_assets")));
	TestTrue(TEXT("enabled families should include restart_survival"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("restart_survival")));
	TestTrue(TEXT("allowed tools should include enhanced_input"), Config.AllowedTools.Contains(TEXT("mcp__unrealclaude__enhanced_input")));
	TestTrue(TEXT("allowed tools should include blueprint_modify"), Config.AllowedTools.Contains(TEXT("mcp__unrealclaude__blueprint_modify")));
	TestTrue(TEXT("allowed tools should include asset"), Config.AllowedTools.Contains(TEXT("mcp__unrealclaude__asset")));
	TestTrue(TEXT("allowed tools should preserve Bash for the ordinary workspace-write lane"), Config.AllowedTools.Contains(TEXT("Bash")));
	TestTrue(TEXT("allowed tools should preserve Write for the ordinary workspace-write lane"), Config.AllowedTools.Contains(TEXT("Write")));
	TestTrue(TEXT("allowed tools should preserve Edit for the ordinary workspace-write lane"), Config.AllowedTools.Contains(TEXT("Edit")));

	const FAgentProviderExecutionControlManifest Manifest = BuildAgentProviderExecutionControlManifest(
		MakeSyntheticBackendStatus(),
		Config);
	TestEqual(
		TEXT("canon exposure should keep the ordinary lane on workspace_write_project"),
		Manifest.CurrentEffectiveProviderPowerClass,
		EAgentExecutionPowerClass::WorkspaceWriteProject);
	TestEqual(
		TEXT("canon exposure should keep the ordinary runtime lane on workspace_write_project"),
		Manifest.CurrentEffectiveRuntimeLane,
		EAgentExecutionRuntimeLane::WorkspaceWriteProject);

	const FAgentPromptContextBlock* ToolExposureBlock = Config.PromptContract.ContextBlocks.FindByPredicate([](const FAgentPromptContextBlock& Block)
	{
		return Block.Label == TEXT("CANON TOOL EXPOSURE");
	});
	TestNotNull(TEXT("canon tool exposure block should be added"), ToolExposureBlock);
	if (ToolExposureBlock)
	{
		TestTrue(TEXT("tool exposure block should name the requested family"), ToolExposureBlock->Content.Contains(TEXT("requested_tool_family = unreal_input"), ESearchCase::IgnoreCase));
	}

	return true;
}

bool FCanonRouting_PromoteFromProvenRunSeedsLedger::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshCanonRoutingTestRoot(TEXT("PromoteFromProvenRunSeedsLedger"));
	const FString LedgerPath = FPaths::Combine(TestRoot, TEXT("canon_ledger.json"));
	FScopedCanonLedgerOverride LedgerOverride(LedgerPath);

	TArray<FUnrealClaudeCanonLedgerEntry> PromotedEntries;
	SeedInputAndAnimationLedger(*this, TestRoot, PromotedEntries);

	TestTrue(TEXT("canon ledger file should exist after proven-run promotion"), IFileManager::Get().FileExists(*LedgerPath));
	TestEqual(TEXT("two proven patterns should be promoted"), PromotedEntries.Num(), 2);

	TArray<FUnrealClaudeCanonLedgerEntry> LoadedEntries;
	FString Error;
	TestTrue(TEXT("canon ledger should load after proven-run promotion"), FUnrealClaudeCanonLedger::LoadEntries(LoadedEntries, Error));
	TestEqual(TEXT("loaded ledger should contain two entries"), LoadedEntries.Num(), 2);
	TestTrue(
		TEXT("input proof reference should stay project-relative"),
		LoadedEntries.ContainsByPredicate([](const FUnrealClaudeCanonLedgerEntry& Entry)
		{
			return Entry.PatternKey == TEXT("input.sprint_binding")
				&& Entry.ProofReference == TEXT("Saved/UnrealClaude/Automation/CanonRouting/PromoteFromProvenRunSeedsLedger/backend_runs/p17_seed_input.json");
		}));
	TestTrue(
		TEXT("animation proof reference should stay project-relative"),
		LoadedEntries.ContainsByPredicate([](const FUnrealClaudeCanonLedgerEntry& Entry)
		{
			return Entry.PatternKey == TEXT("animation.state_machine_creation")
				&& Entry.ProofReference == TEXT("Saved/UnrealClaude/Automation/CanonRouting/PromoteFromProvenRunSeedsLedger/backend_runs/p17_seed_animation.json");
		}));
	return true;
}

bool FCanonRouting_SameSubsystemDifferentPromptUsesDirectedExecution::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshCanonRoutingTestRoot(TEXT("SameSubsystemDifferentPromptUsesDirectedExecution"));
	const FString LedgerPath = FPaths::Combine(TestRoot, TEXT("canon_ledger.json"));
	FScopedCanonLedgerOverride LedgerOverride(LedgerPath);

	TArray<FUnrealClaudeCanonLedgerEntry> PromotedEntries;
	SeedInputAndAnimationLedger(*this, TestRoot, PromotedEntries);

	const FString FollowUpPrompt = TEXT("Add a crouch input action and bind it in the player mapping context while keeping the approved local path.");
	TestFalse(TEXT("same-subsystem follow-up prompt should not replay the original sprint wording"), FollowUpPrompt.Contains(TEXT("sprint"), ESearchCase::IgnoreCase));

	TArray<FAgentPromptContextBlock> ContextBlocks;
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		FollowUpPrompt,
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		ContextBlocks);

	TestEqual(TEXT("follow-up prompt should still detect the input subsystem"), Execution.DetectedSubsystem, FString(TEXT("input")));
	TestTrue(TEXT("approved pattern should be found from the same approved subsystem"), Execution.bApprovedPatternFound);
	TestEqual(TEXT("same approved risky subsystem should route to directed execution"), Execution.TaskMode, FString(TEXT("directed_execution")));
	TestEqual(TEXT("directed input follow-up should keep unreal_input as the requested family"), Execution.RequestedToolFamily, FString(TEXT("unreal_input")));
	TestEqual(TEXT("same-subsystem reuse should resolve to the promoted input pattern"), Execution.ApprovedPatternKey, FString(TEXT("input.sprint_binding")));

	const FAgentPromptContextBlock* LedgerBlock = ContextBlocks.FindByPredicate([](const FAgentPromptContextBlock& Block)
	{
		return Block.Label == TEXT("PROJECT-LOCAL CANON LEDGER");
	});
	TestNotNull(TEXT("ledger block should be present for directed execution"), LedgerBlock);
	if (LedgerBlock)
	{
		TestTrue(TEXT("ledger block should report subsystem-level matching"), LedgerBlock->Content.Contains(TEXT("match_basis = same_subsystem_approved_family"), ESearchCase::IgnoreCase));
	}

	return true;
}

bool FCanonRouting_CrossSubsystemPromptDoesNotFalsePositive::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshCanonRoutingTestRoot(TEXT("CrossSubsystemPromptDoesNotFalsePositive"));
	const FString LedgerPath = FPaths::Combine(TestRoot, TEXT("canon_ledger.json"));
	FScopedCanonLedgerOverride LedgerOverride(LedgerPath);

	TArray<FUnrealClaudeCanonLedgerEntry> PromotedEntries;
	SeedInputAndAnimationLedger(*this, TestRoot, PromotedEntries);

	TArray<FAgentPromptContextBlock> ContextBlocks;
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Save and restore the sprint tutorial completion flag in the player persistence snapshot."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		ContextBlocks);

	TestEqual(TEXT("cross-subsystem prompt should classify as persistence"), Execution.DetectedSubsystem, FString(TEXT("persistence")));
	TestFalse(TEXT("cross-subsystem prompt must not inherit approved input or animation patterns"), Execution.bApprovedPatternFound);
	TestEqual(TEXT("risky persistence prompt without a persistence ledger entry should stay in canon discovery"), Execution.TaskMode, FString(TEXT("canon_discovery")));

	const FAgentPromptContextBlock* LedgerBlock = ContextBlocks.FindByPredicate([](const FAgentPromptContextBlock& Block)
	{
		return Block.Label == TEXT("PROJECT-LOCAL CANON LEDGER");
	});
	TestNotNull(TEXT("ledger block should still be present for the negative case"), LedgerBlock);
	if (LedgerBlock)
	{
		TestTrue(TEXT("negative ledger block should keep approved_pattern_found=false"), LedgerBlock->Content.Contains(TEXT("approved_pattern_found = false"), ESearchCase::IgnoreCase));
		TestTrue(TEXT("negative ledger block should report zero persistence entries"), LedgerBlock->Content.Contains(TEXT("relevant_subsystem_entries = 0"), ESearchCase::IgnoreCase));
	}

	return true;
}

bool FCanonRouting_ProjectLedgerSeedFromAcceptedProofs::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshCanonRoutingTestRoot(TEXT("ProjectLedgerSeedFromAcceptedProofs"));
	const FString TestLedgerPath = FPaths::Combine(TestRoot, TEXT("canon_ledger.json"));
	FScopedCanonLedgerOverride LedgerOverride(TestLedgerPath);

	const FString BackendRunRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealClaude"), TEXT("backend_runs"));
	IFileManager::Get().MakeDirectory(*BackendRunRoot, true);

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString InputReceiptFileName = FString::Printf(TEXT("canon_input_accepted_%s.json"), *UniqueSuffix);
	const FString AnimationReceiptFileName = FString::Printf(TEXT("canon_animation_accepted_%s.json"), *UniqueSuffix);
	const FString InputReceiptPath = FPaths::Combine(BackendRunRoot, InputReceiptFileName);
	const FString AnimationReceiptPath = FPaths::Combine(BackendRunRoot, AnimationReceiptFileName);
	const FString InputProofReference = FString::Printf(TEXT("Saved/UnrealClaude/backend_runs/%s"), *InputReceiptFileName);
	const FString AnimationProofReference = FString::Printf(TEXT("Saved/UnrealClaude/backend_runs/%s"), *AnimationReceiptFileName);

	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*InputReceiptPath, false, true);
		IFileManager::Get().Delete(*AnimationReceiptPath, false, true);
	};

	TestTrue(
		TEXT("synthetic accepted input proof should be written"),
		WriteSyntheticProvenRunReceipt(
			InputReceiptPath,
			TEXT("Create a sprint input action and bind it through Enhanced Input."),
			TEXT("input"),
			TEXT("unreal_input")));
	TestTrue(
		TEXT("synthetic accepted animation proof should be written"),
		WriteSyntheticProvenRunReceipt(
			AnimationReceiptPath,
			TEXT("Create a new AnimBP state machine through native animation tooling."),
			TEXT("animation"),
			TEXT("unreal_animation")));

	FString Error;
	FUnrealClaudeCanonPromotionRequest InputRequest;
	InputRequest.ProvenRunReceiptPath = InputReceiptPath;
	InputRequest.PatternKey = TEXT("input.sprint_binding");
	InputRequest.ExpectedSubsystem = TEXT("input");
	InputRequest.ShortTitle = TEXT("Sprint binding through Enhanced Input");
	InputRequest.ChosenPathSummary = TEXT("Use native Enhanced Input assets and mappings before any fallback lane.");
	InputRequest.WhyPreferred = TEXT("This project already routes player bindings through Enhanced Input assets.");
	InputRequest.BadPathToAvoid = TEXT("Do not start by patching workspace files or UI-driving for input asset work.");
	InputRequest.LastConfirmedEngineContext = TEXT("UE5.7");
	TestTrue(
		TEXT("accepted input proof should seed the project ledger"),
		FUnrealClaudeCanonLedger::PromoteFromProvenRun(InputRequest, Error, nullptr));

	FUnrealClaudeCanonPromotionRequest AnimationRequest;
	AnimationRequest.ProvenRunReceiptPath = AnimationReceiptPath;
	AnimationRequest.PatternKey = TEXT("animation.state_machine_creation");
	AnimationRequest.ExpectedSubsystem = TEXT("animation");
	AnimationRequest.ShortTitle = TEXT("Animation state machine creation through AnimBP tooling");
	AnimationRequest.ChosenPathSummary = TEXT("Use native animation blueprint mutation before generic blueprint or shell fallback.");
	AnimationRequest.WhyPreferred = TEXT("The accepted animation proof fixture already validates state-machine creation on the native animation path.");
	AnimationRequest.BadPathToAvoid = TEXT("Do not start with workspace shell edits for AnimBP graph mutation.");
	AnimationRequest.LastConfirmedEngineContext = TEXT("UE5.7");
	TestTrue(
		TEXT("accepted animation proof should seed the project ledger"),
		FUnrealClaudeCanonLedger::PromoteFromProvenRun(AnimationRequest, Error, nullptr));

	TestTrue(TEXT("project ledger should exist after accepted-proof seeding"), IFileManager::Get().FileExists(*TestLedgerPath));

	TArray<FUnrealClaudeCanonLedgerEntry> LoadedEntries;
	TestTrue(TEXT("project ledger should load after accepted-proof seeding"), FUnrealClaudeCanonLedger::LoadEntries(LoadedEntries, Error));
	TestTrue(
		TEXT("project ledger should include the accepted input pattern"),
		LoadedEntries.ContainsByPredicate([&InputProofReference](const FUnrealClaudeCanonLedgerEntry& Entry)
		{
			return Entry.PatternKey == TEXT("input.sprint_binding")
				&& Entry.ProofReference == InputProofReference;
		}));
	TestTrue(
		TEXT("project ledger should include the accepted animation pattern"),
		LoadedEntries.ContainsByPredicate([&AnimationProofReference](const FUnrealClaudeCanonLedgerEntry& Entry)
		{
			return Entry.PatternKey == TEXT("animation.state_machine_creation")
				&& Entry.ProofReference == AnimationProofReference;
		}));
	return true;
}

bool FCanonRouting_ReceiptJsonCarriesCanonExecution::RunTest(const FString& Parameters)
{
	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("blueprint_modify");
	Receipt.bSuccess = true;
	Receipt.TargetType = TEXT("blueprint");
	Receipt.Status = TEXT("success");
	Receipt.CanonExecution.DetectedSubsystem = TEXT("animation");
	Receipt.CanonExecution.TaskMode = TEXT("canon_discovery");
	Receipt.CanonExecution.bCanonDiscoveryUsed = true;
	Receipt.CanonExecution.bBriefWasProduced = true;
	Receipt.CanonExecution.bBriefPartBRequired = true;
	Receipt.CanonExecution.bBriefPartBProduced = true;
	Receipt.CanonExecution.bBriefPartBProducedBeforeFirstMutatingTool = true;
	Receipt.CanonExecution.bToolExposureAdjusted = true;
	Receipt.CanonExecution.RequestedToolFamily = TEXT("unreal_animation");
	Receipt.CanonExecution.EnabledToolFamilyIds = {
		TEXT("workspace_inspection"),
		TEXT("unreal_animation"),
		TEXT("unreal_blueprint"),
		TEXT("restart_survival")
	};
	Receipt.CanonExecution.ActualToolFamily = TEXT("unreal_animation");
	Receipt.CanonExecution.PrimaryMutationToolFamily = TEXT("unreal_animation");
	Receipt.CanonExecution.AuxiliaryToolFamilies = { TEXT("workspace_file_build") };
	Receipt.CanonExecution.bMutatingFallbackUsed = false;
	Receipt.CanonExecution.VerificationOutcome = TEXT("pass");
	Receipt.CanonExecution.ImplementationBriefLines = {
		TEXT("subsystem = animation"),
		TEXT("chosen_path = canon_discovery"),
		TEXT("why_preferred = no approved pattern exists yet")
	};
	Receipt.CanonExecution.ImplementationBriefPartBLines = {
		TEXT("exact_path = AnimBP sprint transition"),
		TEXT("why_this_path = approved project-local animation lane"),
		TEXT("likely_targets = /Game/Characters/Hero/ABP_Hero"),
		TEXT("expected_verification = compile AnimBP and check transition"),
		TEXT("known_hazard = do not fall back to shell edits")
	};

	const TSharedPtr<FJsonObject> Json = Receipt.ToJson();
	const TSharedPtr<FJsonObject>* CanonObject = nullptr;
	TestTrue(TEXT("receipt JSON should include canon_execution"), Json->TryGetObjectField(TEXT("canon_execution"), CanonObject) && CanonObject && (*CanonObject).IsValid());
	if (!CanonObject || !(*CanonObject).IsValid())
	{
		return false;
	}

	FString TaskMode;
	bool bBriefPartBProduced = false;
	FString PrimaryMutationToolFamily;
	const TArray<TSharedPtr<FJsonValue>>* AuxiliaryFamilies = nullptr;
	(*CanonObject)->TryGetStringField(TEXT("task_mode"), TaskMode);
	(*CanonObject)->TryGetBoolField(TEXT("brief_part_b_produced"), bBriefPartBProduced);
	(*CanonObject)->TryGetStringField(TEXT("primary_mutation_tool_family"), PrimaryMutationToolFamily);
	TestEqual(TEXT("canon task mode should round-trip"), TaskMode, FString(TEXT("canon_discovery")));
	TestTrue(TEXT("receipt JSON should include brief part B production"), bBriefPartBProduced);
	TestEqual(TEXT("receipt JSON should include the primary mutation family"), PrimaryMutationToolFamily, FString(TEXT("unreal_animation")));
	TestTrue(TEXT("receipt JSON should include auxiliary tool families"),
		(*CanonObject)->TryGetArrayField(TEXT("auxiliary_tool_families"), AuxiliaryFamilies) && AuxiliaryFamilies && AuxiliaryFamilies->Num() == 1);
	return true;
}

bool FCanonRouting_TraceRunIncludesCanonSummary::RunTest(const FString& Parameters)
{
	FAgentRequestConfig Config;
	Config.WorkingDirectory = FPaths::ProjectDir();
	Config.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	Config.CanonExecution.DetectedSubsystem = TEXT("input");
	Config.CanonExecution.TaskMode = TEXT("canon_discovery");
	Config.CanonExecution.bCanonDiscoveryUsed = true;
	Config.CanonExecution.bBriefWasProduced = true;
	Config.CanonExecution.bBriefPartBRequired = true;
	Config.CanonExecution.RequestedToolFamily = TEXT("unreal_input");
	Config.CanonExecution.EnabledToolFamilyIds = {
		TEXT("workspace_inspection"),
		TEXT("unreal_input"),
		TEXT("unreal_blueprint"),
		TEXT("unreal_assets"),
		TEXT("restart_survival")
	};
	Config.CanonExecution.VerificationOutcome = TEXT("pending");
	Config.CanonExecution.ImplementationBriefLines = {
		TEXT("subsystem = input"),
		TEXT("chosen_path = canon_discovery"),
		TEXT("why_preferred = no approved local pattern exists")
	};

	FAgentBackendStatus Status;
	Status.Backend = EUnrealClaudeProviderBackend::CodexCli;
	Status.DisplayName = TEXT("Codex CLI");
	Status.bAvailable = true;
	Status.bReady = true;
	Status.Readiness = EAgentBackendReadiness::Ready;
	Status.AuthState = EAgentBackendAuthState::Authenticated;

	const FString RunId = FUnrealClaudeAgentTraceLog::Get().BeginRun(
		Status,
		Config,
		TEXT("Fix the input mapping context using the canonical path."),
		true,
		false);

	FAgentRunEvent InspectionToolUse;
	InspectionToolUse.Type = EAgentRunEventType::ToolUse;
	InspectionToolUse.Backend = Status.Backend;
	InspectionToolUse.ToolName = TEXT("command_execution");
	InspectionToolUse.ToolCallId = TEXT("call_0");
	InspectionToolUse.ToolInput = TEXT("Get-Content -Path 'Docs/UnrealClaude/70_PluginSettings.md' -TotalCount 40");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, InspectionToolUse);

	FAgentRunEvent StatusToolUse;
	StatusToolUse.Type = EAgentRunEventType::ToolUse;
	StatusToolUse.Backend = Status.Backend;
	StatusToolUse.ToolName = TEXT("unrealclaude/unreal_status");
	StatusToolUse.ToolCallId = TEXT("call_1");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, StatusToolUse);

	FAgentRunEvent QueryToolUse;
	QueryToolUse.Type = EAgentRunEventType::ToolUse;
	QueryToolUse.Backend = Status.Backend;
	QueryToolUse.ToolName = TEXT("unrealclaude/enhanced_input");
	QueryToolUse.ToolCallId = TEXT("call_2");
	QueryToolUse.ToolInput = TEXT("{\"operation\":\"query_context\",\"context_path\":\"/Game/Input/IMC_Player\"}");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, QueryToolUse);

	FAgentRunEvent PartBText;
	PartBText.Type = EAgentRunEventType::TextContent;
	PartBText.Backend = Status.Backend;
	PartBText.Text =
		TEXT("Implementation Brief Part B:\n")
		TEXT("- exact_path = /Game/Input/IMC_Player through enhanced_input\n")
		TEXT("- why_this_path = canonical project-local input asset lane\n")
		TEXT("- likely_targets = IMC_Player and related Input Actions\n")
		TEXT("- expected_verification = requery bindings after mutation\n")
		TEXT("- known_hazard = avoid first-mutation fallback to workspace shell\n");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, PartBText);

	FAgentRunEvent ToolUse;
	ToolUse.Type = EAgentRunEventType::ToolUse;
	ToolUse.Backend = Status.Backend;
	ToolUse.ToolName = TEXT("unrealclaude/enhanced_input");
	ToolUse.ToolCallId = TEXT("call_3");
	ToolUse.ToolInput = TEXT("{\"operation\":\"add_mapping\",\"context_path\":\"/Game/Input/IMC_Player\",\"action_path\":\"/Game/Input/IA_Sprint\",\"key\":\"LeftShift\"}");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, ToolUse);

	FAgentRunEvent AuxiliaryToolUse;
	AuxiliaryToolUse.Type = EAgentRunEventType::ToolUse;
	AuxiliaryToolUse.Backend = Status.Backend;
	AuxiliaryToolUse.ToolName = TEXT("Bash");
	AuxiliaryToolUse.ToolCallId = TEXT("call_4");
	AuxiliaryToolUse.ToolInput = TEXT("dotnet build GDR_Shooter_YEditor");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, AuxiliaryToolUse);
	FUnrealClaudeAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, TEXT("done"), true);

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 24;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records =
		FUnrealClaudeAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);

	bool bSawTaskRouting = false;
	bool bSawBrief = false;
	bool bSawBriefPartB = false;
	bool bSawSummary = false;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		if (!Record.IsValid())
		{
			continue;
		}

		FString EventType;
		Record->TryGetStringField(TEXT("event_type"), EventType);
		bSawTaskRouting = bSawTaskRouting || EventType == TEXT("canon_task_routing");
		bSawBrief = bSawBrief || EventType == TEXT("canon_implementation_brief");
		bSawBriefPartB = bSawBriefPartB || EventType == TEXT("canon_implementation_brief_part_b");
		bSawSummary = bSawSummary || EventType == TEXT("canon_execution_summary");
	}

	TestTrue(TEXT("trace should include canon task routing"), bSawTaskRouting);
	TestTrue(TEXT("trace should include implementation brief event"), bSawBrief);
	TestTrue(TEXT("trace should include implementation brief part B event"), bSawBriefPartB);
	TestTrue(TEXT("trace should include canon summary"), bSawSummary);

	const TSharedPtr<FJsonObject> CanonSummary = UnrealClaudeCanonRouting::ExtractCanonExecutionFromTraceRecords(Records);
	TestTrue(TEXT("trace records should expose canon execution summary"), CanonSummary.IsValid());
	if (!CanonSummary.IsValid())
	{
		return false;
	}

	FString PrimaryMutationToolFamily;
	FString VerificationOutcome;
	bool bBriefPartBProduced = false;
	bool bBriefPartBBeforeMutation = false;
	bool bMutatingFallbackUsed = true;
	const TArray<TSharedPtr<FJsonValue>>* AuxiliaryFamilies = nullptr;
	bool bSawWorkspaceBuildAuxiliary = false;
	bool bSawUnrealStatusAsReadOnlyInspection = false;
	bool bSawEnhancedInputQueryAsReadOnly = false;
	bool bSawEnhancedInputMutation = false;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		if (!Record.IsValid())
		{
			continue;
		}

		FString EventType;
		Record->TryGetStringField(TEXT("event_type"), EventType);
		if (EventType != TEXT("tool_use"))
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Payload = GetObjectFieldOrNull(Record, TEXT("payload"));
		if (!Payload.IsValid())
		{
			continue;
		}

		FString ToolName;
		FString ClassifiedToolFamily;
		bool bClassifiedMutatingTool = true;
		Payload->TryGetStringField(TEXT("tool_name"), ToolName);
		Payload->TryGetStringField(TEXT("classified_tool_family"), ClassifiedToolFamily);
		Payload->TryGetBoolField(TEXT("classified_mutating_tool"), bClassifiedMutatingTool);
		if (ToolName == TEXT("unrealclaude/unreal_status"))
		{
			bSawUnrealStatusAsReadOnlyInspection =
				!bClassifiedMutatingTool && ClassifiedToolFamily == TEXT("workspace_inspection");
		}
		else if (ToolName == TEXT("unrealclaude/enhanced_input"))
		{
			FString ToolInput;
			Payload->TryGetStringField(TEXT("tool_input"), ToolInput);
			if (ToolInput.Contains(TEXT("query_context")))
			{
				bSawEnhancedInputQueryAsReadOnly = !bClassifiedMutatingTool && ClassifiedToolFamily == TEXT("unreal_input");
			}
			else if (ToolInput.Contains(TEXT("add_mapping")))
			{
				bSawEnhancedInputMutation = bClassifiedMutatingTool && ClassifiedToolFamily == TEXT("unreal_input");
			}
		}
	}
	CanonSummary->TryGetStringField(TEXT("primary_mutation_tool_family"), PrimaryMutationToolFamily);
	CanonSummary->TryGetStringField(TEXT("verification_outcome"), VerificationOutcome);
	CanonSummary->TryGetBoolField(TEXT("brief_part_b_produced"), bBriefPartBProduced);
	CanonSummary->TryGetBoolField(TEXT("brief_part_b_produced_before_first_mutating_tool"), bBriefPartBBeforeMutation);
	CanonSummary->TryGetBoolField(TEXT("mutating_fallback_used"), bMutatingFallbackUsed);
	if (CanonSummary->TryGetArrayField(TEXT("auxiliary_tool_families"), AuxiliaryFamilies) && AuxiliaryFamilies)
	{
		for (const TSharedPtr<FJsonValue>& Value : *AuxiliaryFamilies)
		{
			FString FamilyId;
			if (Value.IsValid() && Value->TryGetString(FamilyId) && FamilyId == TEXT("workspace_file_build"))
			{
				bSawWorkspaceBuildAuxiliary = true;
				break;
			}
		}
	}
	TestTrue(TEXT("unreal_status should stay read-only workspace inspection"), bSawUnrealStatusAsReadOnlyInspection);
	TestTrue(TEXT("enhanced_input query should stay read-only while preserving the unreal_input family"), bSawEnhancedInputQueryAsReadOnly);
	TestTrue(TEXT("enhanced_input mutation should still classify as a mutating unreal_input tool"), bSawEnhancedInputMutation);
	TestEqual(TEXT("primary mutation family should reflect the first mutating Unreal tool"), PrimaryMutationToolFamily, FString(TEXT("unreal_input")));
	TestTrue(TEXT("brief part B should be captured"), bBriefPartBProduced);
	TestTrue(TEXT("brief part B should be captured before the first mutating tool"), bBriefPartBBeforeMutation);
	TestFalse(TEXT("requested mutation family should not count as mutating fallback"), bMutatingFallbackUsed);
	TestTrue(TEXT("auxiliary tool families should include the later workspace build assist"), bSawWorkspaceBuildAuxiliary);
	TestEqual(TEXT("successful completion should produce a pass outcome"), VerificationOutcome, FString(TEXT("pass")));
	return true;
}

bool FCanonRouting_InventoryFeatureSliceSelectsRecipe::RunTest(const FString& Parameters)
{
	TArray<FAgentPromptContextBlock> ContextBlocks;
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Implement a basic inventory system with test items, a widget, and a toggle input to open it."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		ContextBlocks);

	TestEqual(TEXT("inventory-like prompt should classify as feature_slice"), Execution.TaskMode, FString(TEXT("feature_slice")));
	TestEqual(TEXT("feature_slice should select the inventory recipe"), Execution.RequestedToolFamily, FString(TEXT("feature.inventory_basic_ui_v1")));
	TestTrue(TEXT("feature workflow should be seeded"), Execution.FeatureWorkflow.HasAnySignal());
	TestEqual(TEXT("feature workflow should start at data_model"), Execution.FeatureWorkflow.CurrentPhase, FString(TEXT("data_model")));
	TestEqual(TEXT("feature workflow should expose the expected phase count"), Execution.FeatureWorkflow.Phases.Num(), 7);

	FAgentRequestConfig Config;
	Config.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	Config.AllowedTools = { TEXT("read") };
	Config.CanonExecution = Execution;
	UnrealClaudeCanonRouting::ApplyRuntimeToolExposure(Config);

	TestTrue(TEXT("feature tool exposure should include workspace file build"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("workspace_file_build")));
	TestTrue(TEXT("feature tool exposure should include unreal_input"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("unreal_input")));
	TestTrue(TEXT("feature tool exposure should include unreal_ui"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("unreal_ui")));
	TestTrue(TEXT("feature tool exposure should include restart_survival"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("restart_survival")));
	return true;
}

bool FCanonRouting_InteractionAccessFeatureSliceSelectsRecipe::RunTest(const FString& Parameters)
{
	TArray<FAgentPromptContextBlock> ContextBlocks;
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Implement prison access interaction with a locked door, technical box, line trace prompt, and E interact input."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		ContextBlocks);

	TestEqual(TEXT("interaction/access prompt should classify as feature_slice"), Execution.TaskMode, FString(TEXT("feature_slice")));
	TestEqual(TEXT("interaction/access prompt should select the interaction recipe"), Execution.RequestedToolFamily, FString(TEXT("feature.interaction_access_slice_v1")));
	TestTrue(TEXT("interaction recipe should seed a feature workflow"), Execution.FeatureWorkflow.HasAnySignal());
	TestEqual(TEXT("interaction recipe should start at project_context_preflight"), Execution.FeatureWorkflow.CurrentPhase, FString(TEXT("project_context_preflight")));
	TestEqual(TEXT("interaction recipe should expose the expected phase count"), Execution.FeatureWorkflow.Phases.Num(), 10);

	FAgentRequestConfig Config;
	Config.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	Config.AllowedTools = { TEXT("read") };
	Config.CanonExecution = Execution;
	UnrealClaudeCanonRouting::ApplyRuntimeToolExposure(Config);

	TestTrue(TEXT("interaction feature tool exposure should include workspace file build"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("workspace_file_build")));
	TestTrue(TEXT("interaction feature tool exposure should include unreal_input"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("unreal_input")));
	TestTrue(TEXT("interaction feature tool exposure should include unreal_editor_tools"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("unreal_editor_tools")));
	TestTrue(TEXT("interaction feature tool exposure should include restart_survival"), Config.CanonExecution.EnabledToolFamilyIds.Contains(TEXT("restart_survival")));
	return true;
}

bool FCanonRouting_ProductAcceptancePromptsDoNotSelectInteractionAccess::RunTest(const FString& Parameters)
{
	const TArray<FString> ProductAcceptancePrompts = {
		TEXT("Verify product UI/session/toolbar recovery acceptance using visible UnrealEditor.exe evidence, current-prefix automation logs, and receipt artifacts. Cite ProductAcceptancePromptsDoNotSelectInteractionAccess. Do not create gameplay assets."),
		TEXT("Produce a readiness summary from the current acceptance receipt directory, prompt manifest, automation results, and closeout artifacts. Mention the old feature.interaction_access_slice_v1 false route only as rejected evidence. Do not start a new feature workflow.")
	};

	for (const FString& Prompt : ProductAcceptancePrompts)
	{
		TArray<FAgentPromptContextBlock> ContextBlocks;
		const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
			Prompt,
			EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
			ContextBlocks);

		TestNotEqual(
			TEXT("product acceptance prompt must not route into interaction-access feature_slice"),
			Execution.RequestedToolFamily,
			FString(TEXT("feature.interaction_access_slice_v1")));
		TestFalse(
			TEXT("product acceptance prompt must not seed an interaction-access feature workflow"),
			Execution.FeatureWorkflow.HasAnySignal());
	}

	return true;
}

bool FCanonRouting_GenericGameplayExternalAssetPromptCarriesChecklistGuard::RunTest(const FString& Parameters)
{
	FAgentPromptContract Contract = FAgentPromptContractBuilder::Build(true, false, FString(), FString());
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Implement a wallrun gameplay mechanic and use a Marketplace/Fab parkour animation pack if needed."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		Contract.ContextBlocks);

	const FString Materialized = FAgentPromptMaterializer::MaterializeCanonicalText(Contract);
	TestFalse(TEXT("generic gameplay asset prompt should not false-route into an existing feature recipe"),
		Execution.FeatureWorkflow.HasAnySignal());
	TestTrue(TEXT("generic gameplay prompt should carry checklist extraction guard"),
		Materialized.Contains(TEXT("acceptance_checklist_rule"), ESearchCase::IgnoreCase)
			&& Materialized.Contains(TEXT("before first mutation"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("generic gameplay prompt should carry external asset blocker code"),
		Materialized.Contains(TEXT("external_asset_blocker_code = manual_asset_dependency_blocker"), ESearchCase::IgnoreCase)
			&& Materialized.Contains(TEXT("code-only/placeholders"), ESearchCase::IgnoreCase));
	return true;
}

bool FCanonRouting_GenericGameplayInputConflictPromptCarriesGuard::RunTest(const FString& Parameters)
{
	FAgentPromptContract Contract = FAgentPromptContractBuilder::Build(true, false, FString(), FString());
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Implement wallrun on Space+Shift in the Combat character even though Space already flies and Shift already sprints."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		Contract.ContextBlocks);

	const FString Materialized = FAgentPromptMaterializer::MaterializeCanonicalText(Contract);
	TestFalse(TEXT("input-conflict gameplay prompt should not false-route into an existing feature recipe"),
		Execution.FeatureWorkflow.HasAnySignal());
	TestTrue(TEXT("generic gameplay prompt should require conflict audit before implementation"),
		Materialized.Contains(TEXT("mechanic_input_conflict_rule"), ESearchCase::IgnoreCase)
			&& Materialized.Contains(TEXT("before implementation"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("generic gameplay prompt should carry unresolved conflict blocker code"),
		Materialized.Contains(TEXT("mechanic_input_conflict_blocker_code = mechanic_input_conflict_unresolved"), ESearchCase::IgnoreCase)
			&& Materialized.Contains(TEXT("existing input bindings"), ESearchCase::IgnoreCase));
	return true;
}

bool FCanonRouting_FeatureSlicePromptCarriesRoleAndRecipeContract::RunTest(const FString& Parameters)
{
	FAgentPromptContract Contract = FAgentPromptContractBuilder::Build(true, false, FString(), FString());
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Implement prison access interaction with a locked door, technical box, line trace prompt, and E interact input."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		Contract.ContextBlocks);

	TestEqual(TEXT("feature prompt should select feature_slice"), Execution.TaskMode, FString(TEXT("feature_slice")));
	TestEqual(TEXT("feature prompt should carry worker role"), Execution.FeatureWorkflow.RoleId, FString(TEXT("worker")));
	TestEqual(TEXT("feature prompt should carry recipe id"), Execution.FeatureWorkflow.RecipeId, FString(TEXT("feature.interaction_access_slice_v1")));
	TestEqual(TEXT("feature prompt should carry evidence schema version"), Execution.FeatureWorkflow.EvidenceSchemaVersion, 1);

	const bool bRoleContextResolved = FAgentPromptContractBuilder::AppendRoleContractContext(
		Contract,
		Execution.FeatureWorkflow.RoleId,
		Execution.FeatureWorkflow.RecipeId,
		Execution.FeatureWorkflow.EvidenceSchemaVersion);
	TestTrue(TEXT("role contract prompt context should resolve"), bRoleContextResolved);

	const FString Materialized = FAgentPromptMaterializer::MaterializeCanonicalText(Contract);
	TestTrue(TEXT("materialized prompt should include role contract section"),
		Materialized.Contains(TEXT("[ROLE CONTRACT]"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("materialized prompt should include role id"),
		Materialized.Contains(TEXT("role_id = worker"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("materialized prompt should include recipe id"),
		Materialized.Contains(TEXT("recipe_id = feature.interaction_access_slice_v1"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("materialized prompt should include evidence schema version"),
		Materialized.Contains(TEXT("evidence_schema_version = 1"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("materialized prompt should preserve recipe evidence contract input"),
		Materialized.Contains(TEXT("recipe_evidence_contract_v1"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("feature workflow block should not lose role id"),
		Materialized.Contains(TEXT("[FEATURE WORKFLOW]"), ESearchCase::CaseSensitive)
			&& Materialized.Contains(TEXT("role_id = worker"), ESearchCase::CaseSensitive));
	return true;
}

bool FCanonRouting_InteractionAccessResumeInputPhaseRefreshesContextAndPrefersInputTools::RunTest(const FString& Parameters)
{
	TArray<FAgentPromptContextBlock> ContextBlocks;
	FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Continue the prison-access slice on the existing proof map and reuse the existing persistent interact input assets."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		ContextBlocks);

	Execution.TaskMode = TEXT("feature_slice");
	Execution.RequestedToolFamily = TEXT("feature.interaction_access_slice_v1");
	Execution.FeatureWorkflow.CurrentPhase = TEXT("input_asset_authoring");
	Execution.FeatureWorkflow.AuthoringLaneState = TEXT("pending");
	Execution.FeatureWorkflow.ProofPrerequisiteState = TEXT("satisfied");
	Execution.FeatureWorkflow.bKnownProofMapAvailable = true;
	Execution.FeatureWorkflow.bPlacedRuntimeActorsAvailable = false;

	FAgentRequestConfig Config;
	Config.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	Config.AllowedTools = {
		TEXT("Read"),
		TEXT("Write"),
		TEXT("Edit"),
		TEXT("Grep"),
		TEXT("Glob"),
		TEXT("Bash"),
		TEXT("mcp__unrealclaude__restart_survival")
	};
	Config.CanonExecution = Execution;
	Config.PromptContract.ContextBlocks = ContextBlocks;

	UnrealClaudeCanonRouting::ApplyRuntimeToolExposure(Config);

	const int32 InputFamilyIndex = Config.CanonExecution.EnabledToolFamilyIds.IndexOfByPredicate([](const FString& FamilyId)
	{
		return FamilyId == TEXT("unreal_input");
	});
	const int32 AssetFamilyIndex = Config.CanonExecution.EnabledToolFamilyIds.IndexOfByPredicate([](const FString& FamilyId)
	{
		return FamilyId == TEXT("unreal_assets");
	});
	const int32 InspectionFamilyIndex = Config.CanonExecution.EnabledToolFamilyIds.IndexOfByPredicate([](const FString& FamilyId)
	{
		return FamilyId == TEXT("workspace_inspection");
	});

	TestEqual(TEXT("resume in input_asset_authoring should prefer unreal_input first"), InputFamilyIndex, 0);
	TestTrue(TEXT("resume in input_asset_authoring should keep unreal_assets available"), AssetFamilyIndex != INDEX_NONE);
	TestTrue(TEXT("workspace_inspection should remain auxiliary behind dedicated input verification"), InspectionFamilyIndex > AssetFamilyIndex);

	int32 FeatureWorkflowBlockCount = 0;
	const FAgentPromptContextBlock* FeatureWorkflowBlock = nullptr;
	const FAgentPromptContextBlock* ToolExposureBlock = nullptr;
	for (const FAgentPromptContextBlock& Block : Config.PromptContract.ContextBlocks)
	{
		if (Block.Label == TEXT("FEATURE WORKFLOW"))
		{
			++FeatureWorkflowBlockCount;
			FeatureWorkflowBlock = &Block;
		}
		else if (Block.Label == TEXT("CANON TOOL EXPOSURE"))
		{
			ToolExposureBlock = &Block;
		}
	}

	TestEqual(TEXT("resume should keep a single synchronized FEATURE WORKFLOW block"), FeatureWorkflowBlockCount, 1);
	TestNotNull(TEXT("FEATURE WORKFLOW block should be present"), FeatureWorkflowBlock);
	TestNotNull(TEXT("CANON TOOL EXPOSURE block should be present"), ToolExposureBlock);
	if (!FeatureWorkflowBlock || !ToolExposureBlock)
	{
		return false;
	}

	TestTrue(
		TEXT("FEATURE WORKFLOW block should reflect the resumed input phase"),
		FeatureWorkflowBlock->Content.Contains(TEXT("current_phase = input_asset_authoring"), ESearchCase::CaseSensitive));
	TestFalse(
		TEXT("FEATURE WORKFLOW block should no longer advertise the initial preflight phase"),
		FeatureWorkflowBlock->Content.Contains(TEXT("current_phase = project_context_preflight"), ESearchCase::CaseSensitive));
	TestTrue(
		TEXT("FEATURE WORKFLOW block should publish phase-local preferred family guidance"),
		FeatureWorkflowBlock->Content.Contains(TEXT("phase_local_preferred_tool_family = unreal_input"), ESearchCase::CaseSensitive)
			&& FeatureWorkflowBlock->Content.Contains(TEXT("enhanced_input read-only queries"), ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("CANON TOOL EXPOSURE should encode the input-phase drift guard"),
		ToolExposureBlock->Content.Contains(TEXT("phase_preferred_tool_family = unreal_input"), ESearchCase::CaseSensitive)
			&& ToolExposureBlock->Content.Contains(TEXT("enhanced_input query/read operations"), ESearchCase::IgnoreCase)
			&& ToolExposureBlock->Content.Contains(TEXT("proof-map, actor, or viewport inspection"), ESearchCase::IgnoreCase));

	return true;
}

bool FCanonRouting_ItemOnlyPromptDoesNotFalsePositiveInventory::RunTest(const FString& Parameters)
{
	TArray<FAgentPromptContextBlock> ContextBlocks;
	const FAgentCanonExecution Execution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Create a generic quest item actor with replicated state and an on-pickup event."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		ContextBlocks);

	TestTrue(TEXT("item-only prompt should not select the inventory feature recipe"), Execution.RequestedToolFamily != TEXT("feature.inventory_basic_ui_v1"));
	TestFalse(TEXT("item-only prompt should not seed a feature workflow"), Execution.FeatureWorkflow.HasAnySignal());
	return true;
}

bool FCanonRouting_FeatureReceiptCarriesTopLevelWorkflowSummary::RunTest(const FString& Parameters)
{
	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("enhanced_input");
	Receipt.bSuccess = true;
	Receipt.TargetType = TEXT("feature");
	Receipt.Status = TEXT("success");
	Receipt.CanonExecution.TaskMode = TEXT("feature_slice");
	Receipt.CanonExecution.RequestedToolFamily = TEXT("feature.inventory_basic_ui_v1");
	Receipt.CanonExecution.FeatureWorkflow.FeatureWorkflowId = TEXT("feature_test_1");
	Receipt.CanonExecution.FeatureWorkflow.RecipeId = TEXT("feature.inventory_basic_ui_v1");
	Receipt.CanonExecution.FeatureWorkflow.CurrentPhase = TEXT("runtime_proof");
	Receipt.CanonExecution.FeatureWorkflow.bCompileProofRequired = true;
	Receipt.CanonExecution.FeatureWorkflow.CompileProofState = TEXT("passed");
	Receipt.CanonExecution.FeatureWorkflow.bRuntimeProofRequired = true;
	Receipt.CanonExecution.FeatureWorkflow.RuntimeProofState = TEXT("pending");

	const TSharedPtr<FJsonObject> Json = Receipt.ToJson();
	const TSharedPtr<FJsonObject>* WorkflowObject = nullptr;
	TestTrue(TEXT("receipt JSON should include top-level feature_workflow"), Json->TryGetObjectField(TEXT("feature_workflow"), WorkflowObject) && WorkflowObject && (*WorkflowObject).IsValid());
	if (!WorkflowObject || !(*WorkflowObject).IsValid())
	{
		return false;
	}

	FString RecipeId;
	FString CurrentPhase;
	(*WorkflowObject)->TryGetStringField(TEXT("recipe_id"), RecipeId);
	(*WorkflowObject)->TryGetStringField(TEXT("current_phase"), CurrentPhase);
	TestEqual(TEXT("receipt feature_workflow recipe should round-trip"), RecipeId, FString(TEXT("feature.inventory_basic_ui_v1")));
	TestEqual(TEXT("receipt feature_workflow current phase should round-trip"), CurrentPhase, FString(TEXT("runtime_proof")));
	return true;
}

bool FCanonRouting_FeatureTraceRunIncludesWorkflowEvents::RunTest(const FString& Parameters)
{
	TArray<FAgentPromptContextBlock> ContextBlocks;
	FAgentRequestConfig Config;
	Config.WorkingDirectory = FPaths::ProjectDir();
	Config.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	Config.CanonExecution = UnrealClaudeCanonRouting::BuildInitialCanonExecution(
		TEXT("Create inventory slots, a widget, and input to toggle the inventory screen."),
		EAgentExecutionRunProfile::ConfiguredDefaultRuntime,
		ContextBlocks);

	FAgentBackendStatus Status = MakeSyntheticBackendStatus();
	const FString RunId = FUnrealClaudeAgentTraceLog::Get().BeginRun(
		Status,
		Config,
		TEXT("Create inventory slots, a widget, and input to toggle the inventory screen."),
		true,
		false);
	FUnrealClaudeAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, TEXT("done"), true);

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 24;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records =
		FUnrealClaudeAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);

	bool bSawWorkflowStarted = false;
	bool bSawPhaseStarted = false;
	bool bSawFeatureWorkflowInSummary = false;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		if (!Record.IsValid())
		{
			continue;
		}

		FString EventType;
		Record->TryGetStringField(TEXT("event_type"), EventType);
		bSawWorkflowStarted = bSawWorkflowStarted || EventType == TEXT("workflow_started");
		bSawPhaseStarted = bSawPhaseStarted || EventType == TEXT("phase_started");
		if (EventType == TEXT("canon_execution_summary"))
		{
			bSawFeatureWorkflowInSummary =
				GetObjectFieldOrNull(Record, TEXT("payload")).IsValid()
				&& GetObjectFieldOrNull(GetObjectFieldOrNull(Record, TEXT("payload")), TEXT("feature_workflow")).IsValid();
		}
	}

	TestTrue(TEXT("feature trace should include workflow_started"), bSawWorkflowStarted);
	TestTrue(TEXT("feature trace should include phase_started"), bSawPhaseStarted);
	TestTrue(TEXT("feature trace should include feature_workflow in the final summary"), bSawFeatureWorkflowInSummary);
	return true;
}

bool FCanonRouting_AnimationReadOnlyToolUseDoesNotConsumeMutationSlot::RunTest(const FString& Parameters)
{
	FAgentRequestConfig Config;
	Config.WorkingDirectory = FPaths::ProjectDir();
	Config.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	Config.CanonExecution.DetectedSubsystem = TEXT("animation");
	Config.CanonExecution.TaskMode = TEXT("canon_discovery");
	Config.CanonExecution.bCanonDiscoveryUsed = true;
	Config.CanonExecution.bBriefWasProduced = true;
	Config.CanonExecution.bBriefPartBRequired = true;
	Config.CanonExecution.RequestedToolFamily = TEXT("unreal_animation");
	Config.CanonExecution.EnabledToolFamilyIds = {
		TEXT("workspace_inspection"),
		TEXT("unreal_animation"),
		TEXT("unreal_blueprint"),
		TEXT("restart_survival")
	};
	Config.CanonExecution.VerificationOutcome = TEXT("pending");
	Config.CanonExecution.ImplementationBriefLines = {
		TEXT("subsystem = animation"),
		TEXT("chosen_path = canon_discovery"),
		TEXT("why_preferred = no approved local pattern exists")
	};

	FAgentBackendStatus Status = MakeSyntheticBackendStatus();
	const FString RunId = FUnrealClaudeAgentTraceLog::Get().BeginRun(
		Status,
		Config,
		TEXT("Fix an AnimBP transition using the canonical animation path."),
		true,
		false);

	FAgentRunEvent ReadOnlyAnimToolUse;
	ReadOnlyAnimToolUse.Type = EAgentRunEventType::ToolUse;
	ReadOnlyAnimToolUse.Backend = Status.Backend;
	ReadOnlyAnimToolUse.ToolName = TEXT("unrealclaude/anim_blueprint_modify");
	ReadOnlyAnimToolUse.ToolCallId = TEXT("call_1");
	ReadOnlyAnimToolUse.ToolInput = TEXT("{\"operation\":\"get_info\",\"blueprint_path\":\"/Game/UnrealClaude/CanonProof/Animation/ABP_P16_CanonAnimProof_V1\"}");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, ReadOnlyAnimToolUse);

	FAgentRunEvent PartBText;
	PartBText.Type = EAgentRunEventType::TextContent;
	PartBText.Backend = Status.Backend;
	PartBText.Text =
		TEXT("Implementation Brief Part B:\n")
		TEXT("- exact_path = /Game/UnrealClaude/CanonProof/Animation/ABP_P16_CanonAnimProof_V1 through anim_blueprint_modify\n")
		TEXT("- why_this_path = canonical animation MCP lane for AnimBP state-machine work\n")
		TEXT("- likely_targets = locomotion state machine and transition rules\n")
		TEXT("- expected_verification = validate the AnimBP after the structural change\n")
		TEXT("- known_hazard = do not let read-only inspection consume the first mutation slot\n");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, PartBText);

	FAgentRunEvent MutatingAnimToolUse;
	MutatingAnimToolUse.Type = EAgentRunEventType::ToolUse;
	MutatingAnimToolUse.Backend = Status.Backend;
	MutatingAnimToolUse.ToolName = TEXT("unrealclaude/anim_blueprint_modify");
	MutatingAnimToolUse.ToolCallId = TEXT("call_2");
	MutatingAnimToolUse.ToolInput = TEXT("{\"operation\":\"create_state_machine\",\"blueprint_path\":\"/Game/UnrealClaude/CanonProof/Animation/ABP_P16_CanonAnimProof_V1\",\"state_machine_name\":\"P16ProofSM\"}");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, MutatingAnimToolUse);

	FAgentRunEvent AuxiliaryToolUse;
	AuxiliaryToolUse.Type = EAgentRunEventType::ToolUse;
	AuxiliaryToolUse.Backend = Status.Backend;
	AuxiliaryToolUse.ToolName = TEXT("Bash");
	AuxiliaryToolUse.ToolCallId = TEXT("call_3");
	AuxiliaryToolUse.ToolInput = TEXT("dotnet build GDR_Shooter_YEditor");
	FUnrealClaudeAgentTraceLog::Get().AppendObservedEvent(RunId, AuxiliaryToolUse);
	FUnrealClaudeAgentTraceLog::Get().CompleteRun(RunId, Status.Backend, TEXT("done"), true);

	FAgentTraceQueryOptions QueryOptions;
	QueryOptions.RunId = RunId;
	QueryOptions.bLatestOnly = false;
	QueryOptions.Count = 24;

	FString ResolvedRunId;
	int32 TotalLoaded = 0;
	const TArray<TSharedPtr<FJsonObject>> Records =
		FUnrealClaudeAgentTraceLog::Get().QueryEvents(QueryOptions, ResolvedRunId, TotalLoaded);

	const TSharedPtr<FJsonObject> CanonSummary = UnrealClaudeCanonRouting::ExtractCanonExecutionFromTraceRecords(Records);
	TestTrue(TEXT("trace records should expose canon execution summary"), CanonSummary.IsValid());
	if (!CanonSummary.IsValid())
	{
		return false;
	}

	bool bSawReadOnlyAnimationInspection = false;
	bool bSawMutatingAnimationTool = false;
	FString PrimaryMutationToolFamily;
	FString VerificationOutcome;
	bool bBriefPartBProduced = false;
	bool bBriefPartBBeforeMutation = false;
	bool bMutatingFallbackUsed = true;
	const TArray<TSharedPtr<FJsonValue>>* AuxiliaryFamilies = nullptr;
	bool bSawWorkspaceBuildAuxiliary = false;

	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		if (!Record.IsValid())
		{
			continue;
		}

		FString EventType;
		Record->TryGetStringField(TEXT("event_type"), EventType);
		if (EventType != TEXT("tool_use"))
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Payload = GetObjectFieldOrNull(Record, TEXT("payload"));
		if (!Payload.IsValid())
		{
			continue;
		}

		FString ToolName;
		FString ToolInput;
		FString ClassifiedToolFamily;
		bool bClassifiedMutatingTool = true;
		Payload->TryGetStringField(TEXT("tool_name"), ToolName);
		Payload->TryGetStringField(TEXT("tool_input"), ToolInput);
		Payload->TryGetStringField(TEXT("classified_tool_family"), ClassifiedToolFamily);
		Payload->TryGetBoolField(TEXT("classified_mutating_tool"), bClassifiedMutatingTool);
		if (ToolName == TEXT("unrealclaude/anim_blueprint_modify"))
		{
			if (ToolInput.Contains(TEXT("get_info")))
			{
				bSawReadOnlyAnimationInspection =
					!bClassifiedMutatingTool && ClassifiedToolFamily == TEXT("unreal_animation");
			}
			else if (ToolInput.Contains(TEXT("create_state_machine")))
			{
				bSawMutatingAnimationTool =
					bClassifiedMutatingTool && ClassifiedToolFamily == TEXT("unreal_animation");
			}
		}
	}

	CanonSummary->TryGetStringField(TEXT("primary_mutation_tool_family"), PrimaryMutationToolFamily);
	CanonSummary->TryGetStringField(TEXT("verification_outcome"), VerificationOutcome);
	CanonSummary->TryGetBoolField(TEXT("brief_part_b_produced"), bBriefPartBProduced);
	CanonSummary->TryGetBoolField(TEXT("brief_part_b_produced_before_first_mutating_tool"), bBriefPartBBeforeMutation);
	CanonSummary->TryGetBoolField(TEXT("mutating_fallback_used"), bMutatingFallbackUsed);
	if (CanonSummary->TryGetArrayField(TEXT("auxiliary_tool_families"), AuxiliaryFamilies) && AuxiliaryFamilies)
	{
		for (const TSharedPtr<FJsonValue>& Value : *AuxiliaryFamilies)
		{
			FString FamilyId;
			if (Value.IsValid() && Value->TryGetString(FamilyId) && FamilyId == TEXT("workspace_file_build"))
			{
				bSawWorkspaceBuildAuxiliary = true;
				break;
			}
		}
	}

	TestTrue(TEXT("anim get_info should stay read-only while preserving the unreal_animation family"), bSawReadOnlyAnimationInspection);
	TestTrue(TEXT("animation structural change should still classify as a mutating unreal_animation tool"), bSawMutatingAnimationTool);
	TestEqual(TEXT("primary mutation family should reflect the first mutating animation tool"), PrimaryMutationToolFamily, FString(TEXT("unreal_animation")));
	TestTrue(TEXT("brief part B should be captured"), bBriefPartBProduced);
	TestTrue(TEXT("brief part B should be captured before the first mutating animation tool"), bBriefPartBBeforeMutation);
	TestFalse(TEXT("requested animation family should not count as mutating fallback"), bMutatingFallbackUsed);
	TestTrue(TEXT("auxiliary tool families should include the later workspace build assist"), bSawWorkspaceBuildAuxiliary);
	TestEqual(TEXT("successful completion should produce a pass outcome"), VerificationOutcome, FString(TEXT("pass")));
	return true;
}

#endif
