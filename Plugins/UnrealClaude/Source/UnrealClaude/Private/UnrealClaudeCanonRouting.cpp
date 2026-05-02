// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeCanonRouting.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Containers/Set.h"
#include "Misc/EngineVersion.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClaudeRecipeRegistry.h"
#include "UnrealClaudeRoleRegistry.h"

#include <initializer_list>

namespace
{
	FString GCanonLedgerPathOverride;

	bool ContainsAny(const FString& Text, std::initializer_list<const TCHAR*> Needles)
	{
		for (const TCHAR* Needle : Needles)
		{
			if (Needle != nullptr && Text.Contains(Needle, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool IsClassifierTokenChar(const TCHAR Character)
	{
		return FChar::IsAlnum(Character) || Character == TCHAR('_');
	}

	bool ContainsToken(const FString& Text, const TCHAR* Needle)
	{
		if (Needle == nullptr)
		{
			return false;
		}

		const FString NeedleText(Needle);
		if (NeedleText.IsEmpty())
		{
			return false;
		}

		int32 SearchStart = 0;
		while (SearchStart < Text.Len())
		{
			const int32 Index = Text.Find(NeedleText, ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchStart);
			if (Index == INDEX_NONE)
			{
				return false;
			}

			const int32 EndIndex = Index + NeedleText.Len();
			const bool bHasLeftBoundary = Index == 0 || !IsClassifierTokenChar(Text[Index - 1]);
			const bool bHasRightBoundary = EndIndex >= Text.Len() || !IsClassifierTokenChar(Text[EndIndex]);
			if (bHasLeftBoundary && bHasRightBoundary)
			{
				return true;
			}

			SearchStart = EndIndex;
		}

		return false;
	}

	bool ContainsAnyToken(const FString& Text, std::initializer_list<const TCHAR*> Needles)
	{
		for (const TCHAR* Needle : Needles)
		{
			if (ContainsToken(Text, Needle))
			{
				return true;
			}
		}
		return false;
	}

	FString NormalizeForTokens(const FString& InText)
	{
		FString Normalized = InText.ToLower();
		for (TCHAR& Character : Normalized)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_')))
			{
				Character = TEXT(' ');
			}
		}
		return Normalized;
	}

	TArray<FString> Tokenize(const FString& InText)
	{
		TArray<FString> Tokens;
		NormalizeForTokens(InText).ParseIntoArrayWS(Tokens);
		Tokens.RemoveAll([](const FString& Token)
		{
			return Token.Len() < 3;
		});
		TSet<FString> UniqueTokens;
		for (const FString& Token : Tokens)
		{
			UniqueTokens.Add(Token);
		}

		TArray<FString> Result = UniqueTokens.Array();
		Result.Sort();
		return Result;
	}

	void AddContextBlock(TArray<FAgentPromptContextBlock>& Blocks, const FString& Label, const FString& Content)
	{
		const FString TrimmedContent = Content.TrimStartAndEnd();
		if (TrimmedContent.IsEmpty())
		{
			return;
		}

		FAgentPromptContextBlock& Block = Blocks.AddDefaulted_GetRef();
		Block.Label = Label;
		Block.Content = TrimmedContent;
	}

	void UpsertContextBlock(TArray<FAgentPromptContextBlock>& Blocks, const FString& Label, const FString& Content)
	{
		const FString TrimmedContent = Content.TrimStartAndEnd();
		if (TrimmedContent.IsEmpty())
		{
			return;
		}

		if (FAgentPromptContextBlock* ExistingBlock = Blocks.FindByPredicate([&Label](const FAgentPromptContextBlock& Block)
		{
			return Block.Label.Equals(Label, ESearchCase::CaseSensitive);
		}))
		{
			ExistingBlock->Content = TrimmedContent;
			return;
		}

		AddContextBlock(Blocks, Label, TrimmedContent);
	}

	FString JoinLines(const TArray<FString>& Lines)
	{
		FString Result;
		for (const FString& Line : Lines)
		{
			if (Line.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}

			if (!Result.IsEmpty())
			{
				Result += TEXT("\n");
			}

			Result += Line;
		}
		return Result;
	}

	FString ResolveLedgerPath()
	{
		if (!GCanonLedgerPathOverride.IsEmpty())
		{
			return GCanonLedgerPathOverride;
		}

		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealClaude"), TEXT("canon_ledger.json"));
	}

	FString ResolveProvenRunReceiptPath(const FString& InPathOrRunTag)
	{
		const FString Trimmed = InPathOrRunTag.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return FString();
		}

		if (!Trimmed.Contains(TEXT("/")) && !Trimmed.Contains(TEXT("\\")) && !Trimmed.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase))
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealClaude"), TEXT("backend_runs"), Trimmed + TEXT(".json"));
		}

		return FPaths::IsRelative(Trimmed)
			? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Trimmed)
			: Trimmed;
	}

	FString MakeProjectRelativePath(const FString& InPath)
	{
		FString AbsolutePath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*InPath);
		FPaths::MakeStandardFilename(AbsolutePath);
		const int32 SavedMarkerIndex = AbsolutePath.Find(TEXT("/Saved/"), ESearchCase::IgnoreCase);
		if (SavedMarkerIndex != INDEX_NONE)
		{
			return AbsolutePath.Mid(SavedMarkerIndex + 1);
		}

		FString ProjectDir = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*FPaths::ProjectDir());
		FPaths::MakeStandardFilename(ProjectDir);
		if (!ProjectDir.EndsWith(TEXT("/")))
		{
			ProjectDir += TEXT("/");
		}

		FString RelativePath = AbsolutePath;
		if (FPaths::MakePathRelativeTo(RelativePath, *ProjectDir))
		{
			FPaths::NormalizeFilename(RelativePath);
			return RelativePath;
		}

		if (AbsolutePath.StartsWith(ProjectDir, ESearchCase::IgnoreCase))
		{
			return AbsolutePath.RightChop(ProjectDir.Len());
		}

		return AbsolutePath;
	}

	FString BuildCurrentEngineContextTag()
	{
		const FEngineVersion EngineVersion = FEngineVersion::Current();
		return FString::Printf(TEXT("UE%d.%d"), EngineVersion.GetMajor(), EngineVersion.GetMinor());
	}

	bool LoadJsonObjectFromPath(const FString& InPath, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		OutObject.Reset();
		OutError.Reset();

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *InPath))
		{
			OutError = FString::Printf(TEXT("Failed to read JSON file: %s"), *InPath);
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to parse JSON file: %s"), *InPath);
			return false;
		}

		return true;
	}

	FString DetectSubsystem(const FString& Prompt)
	{
		const FString Lower = Prompt.ToLower();

		if (ContainsAny(Lower, {
			TEXT("animation"), TEXT("anim"), TEXT("montage"), TEXT("blendspace"), TEXT("state machine"), TEXT("control rig"),
			TEXT("\u0430\u043d\u0438\u043c"), TEXT("\u043c\u043e\u043d\u0442\u0430\u0436"), TEXT("\u0440\u0438\u0433")
		}))
		{
			return TEXT("animation");
		}

		if (ContainsAny(Lower, {
			TEXT("input"), TEXT("mapping context"), TEXT("input action"), TEXT("enhanced input"),
			TEXT("\u0432\u0432\u043e\u0434"), TEXT("\u043a\u043d\u043e\u043f"), TEXT("\u0431\u0438\u043d\u0434")
		}))
		{
			return TEXT("input");
		}

		if (ContainsAny(Lower, {
			TEXT("replication"), TEXT("replicate"), TEXT("server rpc"), TEXT("client rpc"), TEXT("netmulticast"), TEXT("authority"),
			TEXT("network"), TEXT("\u0441\u0435\u0442"), TEXT("\u0440\u0435\u043f\u043b"), TEXT("\u0441\u0435\u0440\u0432\u0435\u0440"), TEXT("\u043a\u043b\u0438\u0435\u043d\u0442")
		}))
		{
			return TEXT("networking");
		}

		if (ContainsAny(Lower, {
			TEXT("save"), TEXT("load"), TEXT("persistence"), TEXT("serialize"), TEXT("snapshot"), TEXT("restore"),
			TEXT("\u0441\u043e\u0445\u0440"), TEXT("\u0437\u0430\u0433\u0440"), TEXT("\u043f\u0435\u0440\u0441\u0438\u0441"), TEXT("\u0432\u043e\u0441\u0441\u0442\u0430\u043d")
		}))
		{
			return TEXT("persistence");
		}

		if (ContainsAny(Lower, {
			TEXT("widget"), TEXT("umg"), TEXT("hud"), TEXT("button"), TEXT("menu"), TEXT("common ui"), TEXT("screen"),
			TEXT("\u0432\u0438\u0434\u0436\u0435\u0442"), TEXT("\u0438\u043d\u0442\u0435\u0440\u0444\u0435\u0439\u0441"), TEXT("\u043a\u043d\u043e\u043f"), TEXT("\u043c\u0435\u043d\u044e")
		}))
		{
			return TEXT("ui");
		}

		if (ContainsAny(Lower, {
			TEXT("physics"), TEXT("collision"), TEXT("overlap"), TEXT("chaos"), TEXT("impulse"), TEXT("ragdoll"), TEXT("trace"),
			TEXT("\u0444\u0438\u0437\u0438\u043a"), TEXT("\u043a\u043e\u043b\u043b\u0438\u0437"), TEXT("\u0442\u0440\u0435\u0439\u0441"), TEXT("\u043e\u0432\u0435\u0440\u043b\u0430\u043f")
		}))
		{
			return TEXT("physics");
		}

		if (ContainsAny(Lower, {
			TEXT("material"), TEXT("shader"), TEXT("niagara"), TEXT("vfx"),
			TEXT("\u043c\u0430\u0442\u0435\u0440\u0438\u0430\u043b"), TEXT("\u0448\u0435\u0439\u0434\u0435\u0440"), TEXT("\u043d\u0438\u0430\u0433\u0430\u0440\u0430")
		}))
		{
			return TEXT("materials_vfx");
		}

		if (ContainsAny(Lower, {
			TEXT("blueprint"), TEXT("bp_"), TEXT("graph"), TEXT("anim blueprint"),
			TEXT("\u0431\u043b\u0443\u043f\u0440\u0438\u043d\u0442"), TEXT("\u0433\u0440\u0430\u0444")
		}))
		{
			return TEXT("blueprint");
		}

		if (ContainsAny(Lower, {
			TEXT("asset"), TEXT("uasset"), TEXT("mesh"), TEXT("skeletal"), TEXT("texture"), TEXT("level"),
			TEXT("\u0430\u0441\u0441\u0435\u0442"), TEXT("\u043c\u0435\u0448"), TEXT("\u0442\u0435\u043a\u0441\u0442\u0443\u0440"), TEXT("\u0443\u0440\u043e\u0432\u0435\u043d\u044c")
		}))
		{
			return TEXT("assets");
		}

		return TEXT("general");
	}

	bool IsRiskySubsystem(const FString& Subsystem)
	{
		return Subsystem == TEXT("animation")
			|| Subsystem == TEXT("input")
			|| Subsystem == TEXT("networking")
			|| Subsystem == TEXT("persistence")
			|| Subsystem == TEXT("ui")
			|| Subsystem == TEXT("physics");
	}

	bool HasMutationVerb(const FString& Prompt)
	{
		const FString Lower = Prompt.ToLower();
		return ContainsAnyToken(Lower, {
			TEXT("fix"), TEXT("change"), TEXT("update"), TEXT("replace"), TEXT("wire"), TEXT("connect"), TEXT("create"), TEXT("add"),
			TEXT("edit"), TEXT("modify"), TEXT("implement"), TEXT("patch"), TEXT("refactor")
		})
		|| ContainsAny(Lower, {
			TEXT("\u0438\u0441\u043f\u0440\u0430\u0432"), TEXT("\u0438\u0437\u043c\u0435\u043d"), TEXT("\u0437\u0430\u043c\u0435\u043d"), TEXT("\u0441\u043e\u0437\u0434"), TEXT("\u0434\u043e\u0431\u0430\u0432"),
			TEXT("\u043f\u043e\u0447\u0438\u043d"), TEXT("\u043f\u0435\u0440\u0435\u043f\u0438\u0448"), TEXT("\u0441\u0432\u044f\u0436"), TEXT("\u043f\u0440\u0430\u0432\u043a")
		});
	}

	bool LooksReadOnlyPrompt(const FString& Prompt)
	{
		const FString Lower = Prompt.ToLower();
		if (HasMutationVerb(Lower))
		{
			return false;
		}

		return ContainsAny(Lower, {
			TEXT("review"), TEXT("inspect"), TEXT("analyze"), TEXT("analysis"), TEXT("explain"), TEXT("why"), TEXT("read"), TEXT("status"),
			TEXT("\u0447\u0438\u0442\u0430\u0439"), TEXT("\u043f\u0440\u043e\u0447\u0438\u0442"), TEXT("\u043f\u0440\u043e\u0432\u0435\u0440"), TEXT("\u043e\u0431\u044a\u044f\u0441\u043d"), TEXT("\u0441\u0442\u0430\u0442\u0443\u0441")
		});
	}

	bool LooksUiDrivingPrompt(const FString& Prompt)
	{
		return ContainsAny(Prompt.ToLower(), {
			TEXT("click"), TEXT("button"), TEXT("menu"), TEXT("screenshot"), TEXT("focus window"), TEXT("cursor"), TEXT("type into"),
			TEXT("\u043d\u0430\u0436\u043c"), TEXT("\u043a\u043b\u0438\u043a"), TEXT("\u043a\u043d\u043e\u043f"), TEXT("\u0441\u043a\u0440\u0438\u043d"), TEXT("\u043e\u043a\u043d\u043e")
		});
	}

	bool LooksRestartSurvivalPrompt(const FString& Prompt)
	{
		return ContainsAny(Prompt.ToLower(), {
			TEXT("restart survival"), TEXT("restart_survival"), TEXT("close unreal"), TEXT("relaunch unreal"), TEXT("reopen unreal"),
			TEXT("\u0437\u0430\u043a\u0440\u043e\u0439 unreal"), TEXT("\u0437\u0430\u043a\u0440\u043e\u0439 \u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440"),
			TEXT("\u043f\u0435\u0440\u0435\u0437\u0430\u043f\u0443\u0441\u0442"), TEXT("\u043e\u0442\u043a\u0440\u043e\u0439 unreal")
		});
	}

	bool LooksWorkspaceFileBuildPrompt(const FString& Prompt)
	{
		return ContainsAny(Prompt.ToLower(), {
			TEXT(".cpp"), TEXT(".h"), TEXT(".json"), TEXT("file"), TEXT("build"), TEXT("compile"), TEXT("preflight"), TEXT("log"), TEXT("dll"),
			TEXT("\u0444\u0430\u0439\u043b"), TEXT("\u0441\u0431\u043e\u0440"), TEXT("\u043a\u043e\u043c\u043f\u0438\u043b"), TEXT("\u043b\u043e\u0433")
		});
	}

	const TCHAR* const InventoryBasicFeatureRecipeId = TEXT("feature.inventory_basic_ui_v1");
	const TCHAR* const InteractionAccessFeatureRecipeId = TEXT("feature.interaction_access_slice_v1");

	bool LooksInteractionAccessFeaturePrompt(const FString& Prompt)
	{
		const FString Lower = Prompt.ToLower();
		const bool bHasInteractionIntent = ContainsAnyToken(Lower, {
			TEXT("interaction"), TEXT("interact"), TEXT("interactable"), TEXT("line trace"), TEXT("look at"),
			TEXT("press e"), TEXT("ia_interact"), TEXT("bpi_interactable")
		})
		|| ContainsAny(Lower, {
			TEXT("\u0438\u043d\u0442\u0435\u0440\u0430\u043a"), TEXT("\u0432\u0437\u0430\u0438\u043c"), TEXT("\u0442\u0440\u0435\u0439\u0441")
		});

		const bool bHasAccessDomain = ContainsAnyToken(Lower, {
			TEXT("door"), TEXT("locked"), TEXT("unlock"), TEXT("access"), TEXT("control box"), TEXT("technical box"),
			TEXT("alarm"), TEXT("switch"), TEXT("storage room"), TEXT("storage door"), TEXT("prison"), TEXT("gate")
		})
		|| ContainsAny(Lower, {
			TEXT("\u0434\u0432\u0435\u0440"), TEXT("\u0437\u0430\u043c\u043e\u043a"), TEXT("\u0434\u043e\u0441\u0442\u0443\u043f"),
			TEXT("\u043f\u0443\u043b\u044c\u0442"), TEXT("\u0442\u0435\u0445\u043d"), TEXT("\u0441\u043a\u043b\u0430\u0434"), TEXT("\u0442\u044e\u0440\u044c\u043c")
		});

		const bool bHasImplementationSurface = ContainsAnyToken(Lower, {
			TEXT("input action"), TEXT("mapping context"), TEXT("enhanced input"), TEXT("prompt"), TEXT("interface"),
			TEXT("actor"), TEXT("placed actor"), TEXT("proof map"), TEXT("automation")
		})
		|| ContainsAny(Lower, {
			TEXT("\u0432\u0432\u043e\u0434"), TEXT("\u043a\u043d\u043e\u043f"), TEXT("\u043f\u0440\u043e\u043c\u043f\u0442"),
			TEXT("\u0438\u043d\u0442\u0435\u0440\u0444\u0435\u0439\u0441"), TEXT("\u0430\u0432\u0442\u043e\u043c\u0430\u0446"), TEXT("\u043a\u0430\u0440\u0442")
		});

		return bHasInteractionIntent && (bHasAccessDomain || bHasImplementationSurface);
	}

	bool LooksInventoryBasicFeaturePrompt(const FString& Prompt)
	{
		const FString Lower = Prompt.ToLower();
		const bool bHasStrongInventoryIntent = ContainsAny(Lower, {
			TEXT("inventory"), TEXT("inventory widget"), TEXT("inventory screen"), TEXT("inventory component"),
			TEXT("slot"), TEXT("slots"), TEXT("pickup"), TEXT("loot"), TEXT("backpack"), TEXT("hotbar"),
			TEXT("equipment"), TEXT("equip"), TEXT("stash"),
			TEXT("\u0438\u043d\u0432\u0435\u043d\u0442"), TEXT("\u0441\u043b\u043e\u0442"), TEXT("\u043b\u0443\u0442"),
			TEXT("\u043f\u0438\u043a\u0430\u043f"), TEXT("\u044d\u043a\u0438\u043f"), TEXT("\u0440\u044e\u043a\u0437\u0430\u043a")
		});
		if (!bHasStrongInventoryIntent)
		{
			return false;
		}

		int32 FeatureSignalCount = 0;
		if (ContainsAny(Lower, {
			TEXT("widget"), TEXT("umg"), TEXT("hud"), TEXT("ui"), TEXT("screen"), TEXT("menu"),
			TEXT("\u0432\u0438\u0434\u0436\u0435\u0442"), TEXT("\u0438\u043d\u0442\u0435\u0440\u0444\u0435\u0439\u0441"), TEXT("\u044d\u043a\u0440\u0430\u043d")
		}))
		{
			++FeatureSignalCount;
		}

		if (ContainsAny(Lower, {
			TEXT("input"), TEXT("toggle"), TEXT("tab"), TEXT("open inventory"), TEXT("mapping context"), TEXT("enhanced input"),
			TEXT("\u043a\u043d\u043e\u043f"), TEXT("\u0442\u043e\u0433\u0433\u043b"), TEXT("\u043e\u0442\u043a\u0440"), TEXT("\u043f\u0435\u0440\u0435\u043a\u043b")
		}))
		{
			++FeatureSignalCount;
		}

		if (ContainsAny(Lower, {
			TEXT("component"), TEXT("controller"), TEXT("character"), TEXT("owner"), TEXT("data"), TEXT("struct"), TEXT("array"), TEXT("test item"),
			TEXT("\u043a\u043e\u043c\u043f\u043e\u043d\u0435\u043d"), TEXT("\u043a\u043e\u043d\u0442\u0440\u043e\u043b\u043b\u0435\u0440"), TEXT("\u043f\u0435\u0440\u0441\u043e\u043d"), TEXT("\u0434\u0430\u043d\u043d"), TEXT("\u0442\u0435\u0441\u0442\u043e\u0432")
		}))
		{
			++FeatureSignalCount;
		}

		return FeatureSignalCount >= 1;
	}

	FString MakeDeterministicFeatureWorkflowId(const FString& Prompt, const FString& RecipeId)
	{
		const FString FingerprintSource = RecipeId + TEXT("|") + NormalizeForTokens(Prompt);
		return FString::Printf(TEXT("feature_%08x"), FCrc::StrCrc32(*FingerprintSource));
	}

	FAgentFeatureWorkflowPhaseState MakeFeaturePhaseState(const FString& PhaseId, const FString& Label)
	{
		FAgentFeatureWorkflowPhaseState Phase;
		Phase.PhaseId = PhaseId;
		Phase.Label = Label;
		return Phase;
	}

	FAgentFeatureWorkflowState MakeInventoryBasicFeatureWorkflow(const FString& Prompt)
	{
		FAgentFeatureWorkflowState Workflow;
		Workflow.FeatureWorkflowId = MakeDeterministicFeatureWorkflowId(Prompt, InventoryBasicFeatureRecipeId);
		Workflow.RecipeId = InventoryBasicFeatureRecipeId;
		Workflow.RoleId = UnrealClaudeRoleRegistry::WorkerRoleId();
		Workflow.EvidenceSchemaVersion =
			UnrealClaudeRecipeRegistry::GetInventoryBasicUiRecipeEvidenceContract().EvidenceSchemaVersion;
		Workflow.CurrentPhase = TEXT("data_model");
		Workflow.CompileProofState = TEXT("pending");
		Workflow.bRuntimeProofRequired = true;
		Workflow.RuntimeProofState = TEXT("pending");
		Workflow.Phases = {
			MakeFeaturePhaseState(TEXT("data_model"), TEXT("Data model")),
			MakeFeaturePhaseState(TEXT("runtime_owner"), TEXT("Runtime owner")),
			MakeFeaturePhaseState(TEXT("input_controller"), TEXT("Input controller")),
			MakeFeaturePhaseState(TEXT("ui_widget"), TEXT("UI widget")),
			MakeFeaturePhaseState(TEXT("compile_gate"), TEXT("Compile gate")),
			MakeFeaturePhaseState(TEXT("runtime_proof"), TEXT("Runtime proof")),
			MakeFeaturePhaseState(TEXT("memory_update"), TEXT("Memory update"))
		};
		return Workflow;
	}

	FAgentFeatureWorkflowState MakeInteractionAccessFeatureWorkflow(const FString& Prompt)
	{
		FAgentFeatureWorkflowState Workflow;
		Workflow.FeatureWorkflowId = MakeDeterministicFeatureWorkflowId(Prompt, InteractionAccessFeatureRecipeId);
		Workflow.RecipeId = InteractionAccessFeatureRecipeId;
		Workflow.RoleId = UnrealClaudeRoleRegistry::WorkerRoleId();
		Workflow.EvidenceSchemaVersion =
			UnrealClaudeRecipeRegistry::GetInteractionAccessRecipeEvidenceContract().EvidenceSchemaVersion;
		Workflow.CurrentPhase = TEXT("project_context_preflight");
		Workflow.CompileProofState = TEXT("pending");
		Workflow.bRuntimeProofRequired = true;
		Workflow.RuntimeProofState = TEXT("pending");
		Workflow.AuthoringLaneState = TEXT("pending");
		Workflow.ProofPrerequisiteState = TEXT("pending");
		Workflow.Phases = {
			MakeFeaturePhaseState(TEXT("project_context_preflight"), TEXT("Project context preflight")),
			MakeFeaturePhaseState(TEXT("interaction_contract"), TEXT("Interaction contract")),
			MakeFeaturePhaseState(TEXT("input_asset_authoring"), TEXT("Input asset authoring")),
			MakeFeaturePhaseState(TEXT("runtime_actor_state"), TEXT("Runtime actor state")),
			MakeFeaturePhaseState(TEXT("attempt_resolver_and_logging"), TEXT("Attempt resolver and logging")),
			MakeFeaturePhaseState(TEXT("proof_context_setup"), TEXT("Proof context setup")),
			MakeFeaturePhaseState(TEXT("compile_gate"), TEXT("Compile gate")),
			MakeFeaturePhaseState(TEXT("automation_discovery_gate"), TEXT("Automation discovery gate")),
			MakeFeaturePhaseState(TEXT("runtime_proof"), TEXT("Runtime proof")),
			MakeFeaturePhaseState(TEXT("memory_update"), TEXT("Memory update"))
		};
		return Workflow;
	}

	FString DetermineFeatureRecipeId(const FString& Prompt)
	{
		if (LooksInteractionAccessFeaturePrompt(Prompt))
		{
			return InteractionAccessFeatureRecipeId;
		}

		if (LooksInventoryBasicFeaturePrompt(Prompt))
		{
			return InventoryBasicFeatureRecipeId;
		}

		return FString();
	}

	FString BuildFeatureWorkflowContext(const FAgentFeatureWorkflowState& Workflow)
	{
		if (!Workflow.HasAnySignal())
		{
			return FString();
		}

		TArray<FString> PhaseIds;
		for (const FAgentFeatureWorkflowPhaseState& Phase : Workflow.Phases)
		{
			PhaseIds.Add(Phase.PhaseId);
		}

		FString Context = FString::Printf(
			TEXT("feature_workflow_id = %s\nrecipe_id = %s\nrole_id = %s\nevidence_schema_version = %d\ncurrent_phase = %s\nphase_order = %s\ncompile_proof_required = %s\ncompile_proof_state = %s\nruntime_proof_required = %s\nruntime_proof_state = %s\nstop_loss_policy = same_phase_fail_twice | compile_fail_twice | runtime_fail_twice | ad_hoc_proof_attempts_gt_3 | command_execution_without_phase_advance_gt_5_with_blocker_family"),
			*Workflow.FeatureWorkflowId,
			*Workflow.RecipeId,
			*Workflow.RoleId,
			Workflow.EvidenceSchemaVersion,
			*Workflow.CurrentPhase,
			*FString::Join(PhaseIds, TEXT(" -> ")),
			Workflow.bCompileProofRequired ? TEXT("true") : TEXT("false"),
			*Workflow.CompileProofState,
			Workflow.bRuntimeProofRequired ? TEXT("true") : TEXT("false"),
			*Workflow.RuntimeProofState);
		if (!Workflow.AuthoringLaneState.IsEmpty())
		{
			Context += FString::Printf(TEXT("\nauthoring_lane_state = %s"), *Workflow.AuthoringLaneState);
		}
		if (!Workflow.AuthoringPolicyRuleId.IsEmpty())
		{
			Context += FString::Printf(TEXT("\nauthoring_policy_rule_id = %s"), *Workflow.AuthoringPolicyRuleId);
		}
		if (!Workflow.AuthoringDecision.IsEmpty())
		{
			Context += FString::Printf(TEXT("\nauthoring_decision = %s"), *Workflow.AuthoringDecision);
		}
		if (!Workflow.ProofPrerequisiteState.IsEmpty())
		{
			Context += FString::Printf(TEXT("\nproof_prerequisite_state = %s"), *Workflow.ProofPrerequisiteState);
		}
		Context += FString::Printf(
			TEXT("\nknown_proof_map_available = %s\nplaced_runtime_actors_available = %s\nreduced_proof_mode_allowed = %s"),
			Workflow.bKnownProofMapAvailable ? TEXT("true") : TEXT("false"),
			Workflow.bPlacedRuntimeActorsAvailable ? TEXT("true") : TEXT("false"),
			Workflow.bReducedProofModeAllowed ? TEXT("true") : TEXT("false"));
		if (Workflow.RecipeId == InteractionAccessFeatureRecipeId
			&& Workflow.CurrentPhase == TEXT("input_asset_authoring"))
		{
			Context += TEXT(
				"\nphase_local_preferred_tool_family = unreal_input"
				"\nphase_local_success_path = use enhanced_input read-only queries against the existing IMC/action path first; corroborate with asset/project-local evidence only if needed"
				"\nphase_local_drift_guard = proof-map, actor, and viewport inspection belong to later phases and must not consume the early stop-loss budget before input verification");
		}
		else if (Workflow.RecipeId == InteractionAccessFeatureRecipeId
			&& Workflow.CurrentPhase == TEXT("proof_context_setup"))
		{
			Context += TEXT(
				"\nphase_local_preferred_tool_family = unreal_editor_tools"
				"\nphase_local_success_path = prove the existing fixture by opening the known proof map and observing already-placed actors before considering any spawn/move mutation");
		}
		if (!Workflow.AutomationDiscoveryCommand.IsEmpty())
		{
			Context += FString::Printf(TEXT("\nautomation_discovery_command = %s"), *Workflow.AutomationDiscoveryCommand);
		}
		if (Workflow.AutomationDiscoveryCount != INDEX_NONE)
		{
			Context += FString::Printf(TEXT("\nautomation_discovery_count = %d"), Workflow.AutomationDiscoveryCount);
		}
		if (Workflow.AutomationExecutedCount != INDEX_NONE)
		{
			Context += FString::Printf(TEXT("\nautomation_executed_count = %d"), Workflow.AutomationExecutedCount);
		}
		if (Workflow.AutomationPassedCount != INDEX_NONE)
		{
			Context += FString::Printf(TEXT("\nautomation_passed_count = %d"), Workflow.AutomationPassedCount);
		}
		if (Workflow.AutomationFailedCount != INDEX_NONE)
		{
			Context += FString::Printf(TEXT("\nautomation_failed_count = %d"), Workflow.AutomationFailedCount);
		}
		if (!Workflow.BlockerFamily.IsEmpty())
		{
			Context += FString::Printf(TEXT("\nblocker_family = %s"), *Workflow.BlockerFamily);
		}
		if (!Workflow.BlockerDetail.IsEmpty())
		{
			Context += FString::Printf(TEXT("\nblocker_detail = %s"), *Workflow.BlockerDetail);
		}
		return Context;
	}

	FString BuildTaskModeClassificationContext(const FAgentCanonExecution& Execution)
	{
		return FString::Printf(
			TEXT("task_mode = %s\ndetected_subsystem = %s\napproved_pattern_found = %s\nrequested_tool_family = %s\nsource_priority = project_local -> official_unreal_docs -> official_first_party_samples -> community_support_only\nacceptance_checklist_rule = extract gameplay/mechanic checklist before first mutation\nmechanic_input_conflict_rule = audit requested controls against existing input bindings before implementation\nmechanic_input_conflict_blocker_code = mechanic_input_conflict_unresolved\nexternal_asset_blocker_code = manual_asset_dependency_blocker"),
			*Execution.TaskMode,
			*Execution.DetectedSubsystem,
			Execution.bApprovedPatternFound ? TEXT("true") : TEXT("false"),
			*Execution.RequestedToolFamily);
	}

	FString DetermineTaskMode(
		const FString& Prompt,
		const FString& Subsystem,
		const bool bApprovedPatternFound,
		const EAgentExecutionRunProfile Profile)
	{
		if (Profile == EAgentExecutionRunProfile::ReadOnlyDiagnostic || LooksReadOnlyPrompt(Prompt))
		{
			return TEXT("read_only_analysis");
		}

		if (LooksRestartSurvivalPrompt(Prompt))
		{
			return TEXT("restart_survival_eligible_blocker");
		}

		if (LooksInteractionAccessFeaturePrompt(Prompt)
			|| LooksInventoryBasicFeaturePrompt(Prompt))
		{
			return TEXT("feature_slice");
		}

		if (LooksUiDrivingPrompt(Prompt))
		{
			return TEXT("ui_driving");
		}

		if (IsRiskySubsystem(Subsystem))
		{
			return bApprovedPatternFound ? TEXT("directed_execution") : TEXT("canon_discovery");
		}

		if (LooksWorkspaceFileBuildPrompt(Prompt))
		{
			return TEXT("workspace_file_build");
		}

		if (HasMutationVerb(Prompt) && Subsystem != TEXT("general"))
		{
			return TEXT("bounded_unreal_mutation");
		}

		return bApprovedPatternFound ? TEXT("directed_execution") : TEXT("workspace_file_build");
	}

	FString DetermineRequestedToolFamily(const FString& TaskMode, const FString& Subsystem, const FString& FeatureRecipeId)
	{
		if (TaskMode == TEXT("read_only_analysis"))
		{
			return TEXT("workspace_inspection");
		}

		if (TaskMode == TEXT("ui_driving"))
		{
			return TEXT("osvayder_ui");
		}

		if (TaskMode == TEXT("restart_survival_eligible_blocker"))
		{
			return TEXT("restart_survival");
		}

		if (TaskMode == TEXT("feature_slice"))
		{
			return FeatureRecipeId.IsEmpty() ? FString(InventoryBasicFeatureRecipeId) : FeatureRecipeId;
		}

		if (TaskMode == TEXT("workspace_file_build"))
		{
			return TEXT("workspace_file_build");
		}

		if (Subsystem == TEXT("animation"))
		{
			return TEXT("unreal_animation");
		}

		if (Subsystem == TEXT("input"))
		{
			return TEXT("unreal_input");
		}

		if (Subsystem == TEXT("networking"))
		{
			return TEXT("unreal_networking");
		}

		if (Subsystem == TEXT("persistence"))
		{
			return TEXT("unreal_persistence");
		}

		if (Subsystem == TEXT("ui"))
		{
			return TEXT("unreal_ui");
		}

		if (Subsystem == TEXT("physics"))
		{
			return TEXT("unreal_physics");
		}

		if (Subsystem == TEXT("blueprint"))
		{
			return TEXT("unreal_blueprint");
		}

		if (Subsystem == TEXT("assets"))
		{
			return TEXT("unreal_assets");
		}

		if (Subsystem == TEXT("materials_vfx"))
		{
			return TEXT("unreal_materials_vfx");
		}

		return TEXT("workspace_file_build");
	}

	void AddUniqueString(TArray<FString>& Values, const FString& Value)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return;
		}

		if (!Values.ContainsByPredicate([&Trimmed](const FString& Existing)
		{
			return Existing.Equals(Trimmed, ESearchCase::IgnoreCase);
		}))
		{
			Values.Add(Trimmed);
		}
	}

	FString NormalizeObservedToolName(const FString& ToolName)
	{
		FString CanonicalName = ToolName.TrimStartAndEnd();
		if (CanonicalName.StartsWith(TEXT("mcp__unrealclaude__")))
		{
			CanonicalName.RightChopInline(18, EAllowShrinking::No);
		}
		if (CanonicalName.StartsWith(TEXT("unrealclaude/")))
		{
			CanonicalName.RightChopInline(13, EAllowShrinking::No);
		}
		if (CanonicalName.StartsWith(TEXT("unreal_")))
		{
			CanonicalName.RightChopInline(7, EAllowShrinking::No);
		}
		return CanonicalName.ToLower();
	}

	bool IsDirectWorkspaceTool(const FString& NormalizedToolName)
	{
		return NormalizedToolName == TEXT("read")
			|| NormalizedToolName == TEXT("write")
			|| NormalizedToolName == TEXT("edit")
			|| NormalizedToolName == TEXT("grep")
			|| NormalizedToolName == TEXT("glob")
			|| NormalizedToolName == TEXT("bash");
	}

	FString EncodeAllowedToolName(const FString& ToolName)
	{
		const FString NormalizedToolName = NormalizeObservedToolName(ToolName);
		if (IsDirectWorkspaceTool(NormalizedToolName))
		{
			if (NormalizedToolName == TEXT("read")) return TEXT("Read");
			if (NormalizedToolName == TEXT("write")) return TEXT("Write");
			if (NormalizedToolName == TEXT("edit")) return TEXT("Edit");
			if (NormalizedToolName == TEXT("grep")) return TEXT("Grep");
			if (NormalizedToolName == TEXT("glob")) return TEXT("Glob");
			if (NormalizedToolName == TEXT("bash")) return TEXT("Bash");
		}
		return FString::Printf(TEXT("mcp__unrealclaude__%s"), *NormalizedToolName);
	}

	TArray<FString> ConcreteToolsForFamily(const FString& FamilyId)
	{
		if (FamilyId == TEXT("workspace_inspection"))
		{
			return { TEXT("read"), TEXT("grep"), TEXT("glob") };
		}
		if (FamilyId == TEXT("workspace_file_edit"))
		{
			return { TEXT("read"), TEXT("write"), TEXT("edit"), TEXT("grep"), TEXT("glob") };
		}
		if (FamilyId == TEXT("workspace_file_build"))
		{
			return { TEXT("read"), TEXT("write"), TEXT("edit"), TEXT("grep"), TEXT("glob"), TEXT("bash") };
		}
		if (FamilyId == TEXT("restart_survival"))
		{
			return { TEXT("restart_survival") };
		}
		if (FamilyId == TEXT("osvayder_ui"))
		{
			return { TEXT("osvayder_take_screenshot"), TEXT("osvayder_click"), TEXT("osvayder_type") };
		}
		if (FamilyId == TEXT("unreal_assets"))
		{
			return { TEXT("asset"), TEXT("asset_search"), TEXT("asset_dependencies"), TEXT("asset_referencers") };
		}
		if (FamilyId == TEXT("unreal_blueprint"))
		{
			return { TEXT("blueprint_modify"), TEXT("blueprint_query") };
		}
		if (FamilyId == TEXT("unreal_animation"))
		{
			return { TEXT("anim_blueprint_modify") };
		}
		if (FamilyId == TEXT("unreal_input"))
		{
			return { TEXT("enhanced_input") };
		}
		if (FamilyId == TEXT("unreal_networking"))
		{
			return { TEXT("blueprint_modify"), TEXT("cpp_reflection") };
		}
		if (FamilyId == TEXT("unreal_persistence"))
		{
			return { TEXT("blueprint_modify"), TEXT("cpp_reflection"), TEXT("write"), TEXT("edit") };
		}
		if (FamilyId == TEXT("unreal_physics"))
		{
			return { TEXT("blueprint_modify"), TEXT("set_property") };
		}
		if (FamilyId == TEXT("unreal_materials_vfx"))
		{
			return { TEXT("material"), TEXT("niagara") };
		}
		if (FamilyId == TEXT("unreal_editor_tools"))
		{
			return { TEXT("spawn_actor"), TEXT("move_actor"), TEXT("delete_actors"), TEXT("set_property"), TEXT("get_level_actors"), TEXT("open_level"), TEXT("capture_viewport"), TEXT("get_output_log") };
		}
		if (FamilyId == TEXT("unreal_ui"))
		{
			return { TEXT("blueprint_modify"), TEXT("asset"), TEXT("capture_viewport") };
		}
		return {};
	}

	bool FamilyContainsTool(const FString& FamilyId, const FString& ToolName)
	{
		const FString NormalizedToolName = NormalizeObservedToolName(ToolName);
		for (const FString& CandidateTool : ConcreteToolsForFamily(FamilyId))
		{
			if (NormalizeObservedToolName(CandidateTool) == NormalizedToolName)
			{
				return true;
			}
		}
		return false;
	}

	TArray<FString> ResolveEnabledToolFamilyIds(const FAgentCanonExecution& Execution)
	{
		TArray<FString> FamilyIds;
		const FString& RequestedFamily = Execution.RequestedToolFamily;
		if (RequestedFamily.IsEmpty())
		{
			return FamilyIds;
		}

		if (RequestedFamily == TEXT("workspace_inspection"))
		{
			AddUniqueString(FamilyIds, TEXT("workspace_inspection"));
			return FamilyIds;
		}

		if (RequestedFamily == TEXT("workspace_file_build"))
		{
			AddUniqueString(FamilyIds, TEXT("workspace_file_build"));
			AddUniqueString(FamilyIds, TEXT("restart_survival"));
			return FamilyIds;
		}

		if (RequestedFamily == InventoryBasicFeatureRecipeId)
		{
			AddUniqueString(FamilyIds, TEXT("workspace_inspection"));
			AddUniqueString(FamilyIds, TEXT("workspace_file_build"));
			AddUniqueString(FamilyIds, TEXT("unreal_input"));
			AddUniqueString(FamilyIds, TEXT("unreal_ui"));
			AddUniqueString(FamilyIds, TEXT("unreal_blueprint"));
			AddUniqueString(FamilyIds, TEXT("unreal_assets"));
			AddUniqueString(FamilyIds, TEXT("restart_survival"));
			return FamilyIds;
		}

		if (RequestedFamily == InteractionAccessFeatureRecipeId)
		{
			if (Execution.FeatureWorkflow.CurrentPhase == TEXT("input_asset_authoring"))
			{
				AddUniqueString(FamilyIds, TEXT("unreal_input"));
				AddUniqueString(FamilyIds, TEXT("unreal_assets"));
				AddUniqueString(FamilyIds, TEXT("workspace_inspection"));
				AddUniqueString(FamilyIds, TEXT("workspace_file_build"));
				AddUniqueString(FamilyIds, TEXT("unreal_blueprint"));
				AddUniqueString(FamilyIds, TEXT("unreal_editor_tools"));
			}
			else
			{
				AddUniqueString(FamilyIds, TEXT("workspace_inspection"));
				AddUniqueString(FamilyIds, TEXT("workspace_file_build"));
				AddUniqueString(FamilyIds, TEXT("unreal_input"));
				AddUniqueString(FamilyIds, TEXT("unreal_blueprint"));
				AddUniqueString(FamilyIds, TEXT("unreal_assets"));
				AddUniqueString(FamilyIds, TEXT("unreal_editor_tools"));
			}
			AddUniqueString(FamilyIds, TEXT("restart_survival"));
			return FamilyIds;
		}

		if (RequestedFamily == TEXT("restart_survival"))
		{
			AddUniqueString(FamilyIds, TEXT("workspace_inspection"));
			AddUniqueString(FamilyIds, TEXT("restart_survival"));
			return FamilyIds;
		}

		if (RequestedFamily == TEXT("osvayder_ui"))
		{
			AddUniqueString(FamilyIds, TEXT("workspace_inspection"));
			AddUniqueString(FamilyIds, TEXT("osvayder_ui"));
			AddUniqueString(FamilyIds, TEXT("restart_survival"));
			return FamilyIds;
		}

		if (RequestedFamily.StartsWith(TEXT("unreal_")))
		{
			AddUniqueString(FamilyIds, TEXT("workspace_inspection"));
			AddUniqueString(FamilyIds, RequestedFamily);

			if (RequestedFamily == TEXT("unreal_animation") || RequestedFamily == TEXT("unreal_input"))
			{
				AddUniqueString(FamilyIds, TEXT("unreal_blueprint"));
				AddUniqueString(FamilyIds, TEXT("unreal_assets"));
			}
			else if (RequestedFamily == TEXT("unreal_blueprint"))
			{
				AddUniqueString(FamilyIds, TEXT("unreal_assets"));
			}
			else if (RequestedFamily == TEXT("unreal_networking"))
			{
				AddUniqueString(FamilyIds, TEXT("unreal_blueprint"));
				AddUniqueString(FamilyIds, TEXT("unreal_assets"));
			}
			else if (RequestedFamily == TEXT("unreal_persistence"))
			{
				AddUniqueString(FamilyIds, TEXT("unreal_blueprint"));
				AddUniqueString(FamilyIds, TEXT("unreal_assets"));
				AddUniqueString(FamilyIds, TEXT("workspace_file_edit"));
			}
			else if (RequestedFamily == TEXT("unreal_physics"))
			{
				AddUniqueString(FamilyIds, TEXT("unreal_blueprint"));
				AddUniqueString(FamilyIds, TEXT("unreal_assets"));
				AddUniqueString(FamilyIds, TEXT("unreal_editor_tools"));
			}
			else if (RequestedFamily == TEXT("unreal_materials_vfx"))
			{
				AddUniqueString(FamilyIds, TEXT("unreal_assets"));
			}
			else if (RequestedFamily == TEXT("unreal_assets"))
			{
				// already added
			}
			else if (RequestedFamily == TEXT("unreal_ui"))
			{
				AddUniqueString(FamilyIds, TEXT("unreal_assets"));
			}

			AddUniqueString(FamilyIds, TEXT("restart_survival"));
		}

		return FamilyIds;
	}

	TArray<FString> BuildAllowedToolsForFamilies(const TArray<FString>& FamilyIds)
	{
		TArray<FString> AllowedTools;
		for (const FString& FamilyId : FamilyIds)
		{
			for (const FString& ToolName : ConcreteToolsForFamily(FamilyId))
			{
				AddUniqueString(AllowedTools, EncodeAllowedToolName(ToolName));
			}
		}
		return AllowedTools;
	}

	bool TryExtractJsonStringFieldFromToolInput(
		const FString& ToolInput,
		const FString& FieldName,
		FString& OutValue)
	{
		OutValue.Reset();

		const FString TrimmedInput = ToolInput.TrimStartAndEnd();
		if (TrimmedInput.IsEmpty())
		{
			return false;
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TrimmedInput);
		if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
		{
			return JsonObject->TryGetStringField(FieldName, OutValue) && !OutValue.IsEmpty();
		}

		FString CompactLower = TrimmedInput.ToLower();
		CompactLower.ReplaceInline(TEXT(" "), TEXT(""));
		CompactLower.ReplaceInline(TEXT("\t"), TEXT(""));
		CompactLower.ReplaceInline(TEXT("\r"), TEXT(""));
		CompactLower.ReplaceInline(TEXT("\n"), TEXT(""));

		const FString Prefix = FString::Printf(TEXT("\"%s\":\""), *FieldName.ToLower());
		const int32 PrefixIndex = CompactLower.Find(Prefix, ESearchCase::CaseSensitive);
		if (PrefixIndex == INDEX_NONE)
		{
			return false;
		}

		const int32 ValueStart = PrefixIndex + Prefix.Len();
		int32 ValueEnd = ValueStart;
		while (ValueEnd < CompactLower.Len())
		{
			if (CompactLower[ValueEnd] == TEXT('"') && CompactLower[ValueEnd - 1] != TEXT('\\'))
			{
				break;
			}
			++ValueEnd;
		}

		if (ValueEnd <= ValueStart || ValueEnd > CompactLower.Len())
		{
			return false;
		}

		OutValue = CompactLower.Mid(ValueStart, ValueEnd - ValueStart);
		return !OutValue.IsEmpty();
	}

	bool IsReadOnlyEnhancedInputOperation(const FString& Operation)
	{
		return Operation.Equals(TEXT("query_context"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("query_action"), ESearchCase::IgnoreCase);
	}

	bool IsReadOnlyEnhancedInputToolUse(const FString& ToolInput)
	{
		FString Operation;
		return TryExtractJsonStringFieldFromToolInput(ToolInput, TEXT("operation"), Operation)
			&& IsReadOnlyEnhancedInputOperation(Operation);
	}

	bool IsReadOnlyAnimBlueprintOperation(const FString& Operation)
	{
		return Operation.Equals(TEXT("get_info"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("get_state_machine"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("find_animations"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("get_transition_nodes"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("inspect_node_pins"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("validate_blueprint"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("get_state_machine_diagram"), ESearchCase::IgnoreCase);
	}

	bool IsReadOnlyAnimBlueprintToolUse(const FString& ToolInput)
	{
		FString Operation;
		return TryExtractJsonStringFieldFromToolInput(ToolInput, TEXT("operation"), Operation)
			&& IsReadOnlyAnimBlueprintOperation(Operation);
	}

	bool IsReadOnlyToolName(const FString& ToolName)
	{
		const FString NormalizedToolName = NormalizeObservedToolName(ToolName);
		return NormalizedToolName == TEXT("read")
			|| NormalizedToolName == TEXT("grep")
			|| NormalizedToolName == TEXT("glob")
			|| NormalizedToolName == TEXT("asset_search")
			|| NormalizedToolName == TEXT("asset_dependencies")
			|| NormalizedToolName == TEXT("asset_referencers")
			|| NormalizedToolName == TEXT("blueprint_query")
			|| NormalizedToolName == TEXT("get_level_actors")
			|| NormalizedToolName == TEXT("capture_viewport")
			|| NormalizedToolName == TEXT("get_output_log")
			|| NormalizedToolName == TEXT("open_level")
			|| NormalizedToolName == TEXT("plugin_settings")
			|| NormalizedToolName == TEXT("project_memory_status")
			|| NormalizedToolName == TEXT("execution_log_status")
			|| NormalizedToolName == TEXT("task_status")
			|| NormalizedToolName == TEXT("task_result")
			|| NormalizedToolName == TEXT("task_list")
			|| NormalizedToolName == TEXT("get_script_history")
			|| NormalizedToolName == TEXT("unreal_status")
			|| NormalizedToolName == TEXT("status");
	}

	bool IsReadOnlyShellToolInput(const FString& ToolInput)
	{
		const FString NormalizedInput = ToolInput.ToLower();
		if (NormalizedInput.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		const bool bHasReadSignal = ContainsAny(
			NormalizedInput,
			{
				TEXT("get-content"),
				TEXT("select-string"),
				TEXT("test-path"),
				TEXT("resolve-path"),
				TEXT("get-childitem"),
				TEXT("dir "),
				TEXT("dir'"),
				TEXT("dir\""),
				TEXT("ls "),
				TEXT("ls'"),
				TEXT("ls\""),
				TEXT("cat "),
				TEXT("cat'"),
				TEXT("cat\""),
				TEXT("type "),
				TEXT("type'"),
				TEXT("type\""),
				TEXT("rg "),
				TEXT("rg.exe"),
				TEXT("grep "),
				TEXT("findstr "),
				TEXT("git status"),
				TEXT("git diff"),
				TEXT("git show"),
				TEXT("git log")
			});
		if (!bHasReadSignal)
		{
			return false;
		}

		const bool bHasWriteSignal = ContainsAny(
			NormalizedInput,
			{
				TEXT("set-content"),
				TEXT("add-content"),
				TEXT("out-file"),
				TEXT("copy-item"),
				TEXT("move-item"),
				TEXT("remove-item"),
				TEXT("rename-item"),
				TEXT("new-item"),
				TEXT("mkdir"),
				TEXT("md "),
				TEXT(" ni "),
				TEXT("del "),
				TEXT("rm "),
				TEXT("rmdir"),
				TEXT("git apply"),
				TEXT("patch "),
				TEXT("unrealbuildtool"),
				TEXT("msbuild"),
				TEXT("dotnet build"),
				TEXT("npm run"),
				TEXT("npm test"),
				TEXT("runuat"),
				TEXT("buildcookrun")
			});
		return !bHasWriteSignal;
	}

	bool IsReadOnlyToolUseInternal(const FString& ToolName, const FString& ToolInput)
	{
		const FString NormalizedToolName = NormalizeObservedToolName(ToolName);
		if (NormalizedToolName == TEXT("enhanced_input") && IsReadOnlyEnhancedInputToolUse(ToolInput))
		{
			return true;
		}
		if (NormalizedToolName == TEXT("anim_blueprint_modify") && IsReadOnlyAnimBlueprintToolUse(ToolInput))
		{
			return true;
		}
		if (NormalizedToolName == TEXT("bash")
			|| NormalizedToolName == TEXT("command_execution")
			|| NormalizedToolName == TEXT("execute_terminal"))
		{
			return IsReadOnlyShellToolInput(ToolInput);
		}
		return IsReadOnlyToolName(ToolName);
	}

	bool IsMutatingToolName(const FString& ToolName)
	{
		return !IsReadOnlyToolName(ToolName);
	}

	bool IsMutatingToolUseInternal(const FString& ToolName, const FString& ToolInput)
	{
		return !IsReadOnlyToolUseInternal(ToolName, ToolInput);
	}

	bool RequiresBriefPartB(const FAgentCanonExecution& Execution)
	{
		return Execution.TaskMode == TEXT("feature_slice")
			|| (IsRiskySubsystem(Execution.DetectedSubsystem)
			&& (Execution.TaskMode == TEXT("canon_discovery")
				|| Execution.TaskMode == TEXT("directed_execution")
				|| Execution.TaskMode == TEXT("bounded_unreal_mutation")));
	}

	FString StripBriefBulletPrefix(const FString& InLine)
	{
		FString Line = InLine.TrimStartAndEnd();
		while (Line.StartsWith(TEXT("-")) || Line.StartsWith(TEXT("*")))
		{
			Line.RightChopInline(1, EAllowShrinking::No);
			Line = Line.TrimStartAndEnd();
		}
		if (Line.Len() >= 2 && FChar::IsDigit(Line[0]) && Line[1] == TEXT('.'))
		{
			Line.RightChopInline(2, EAllowShrinking::No);
			Line = Line.TrimStartAndEnd();
		}
		return Line;
	}

	TArray<FString> ParseImplementationBriefPartBLines(const FString& InText)
	{
		TArray<FString> Lines;
		if (InText.TrimStartAndEnd().IsEmpty())
		{
			return Lines;
		}

		const FString Marker = TEXT("implementation brief part b");
		const FString LowerText = InText.ToLower();
		const int32 MarkerIndex = LowerText.Find(Marker, ESearchCase::CaseSensitive);
		if (MarkerIndex == INDEX_NONE)
		{
			return Lines;
		}

		TArray<FString> RawLines;
		InText.Mid(MarkerIndex).ParseIntoArrayLines(RawLines, false);
		bool bPastHeader = false;
		for (const FString& RawLine : RawLines)
		{
			const FString Trimmed = RawLine.TrimStartAndEnd();
			if (!bPastHeader)
			{
				if (Trimmed.Contains(TEXT("Implementation Brief Part B"), ESearchCase::IgnoreCase))
				{
					bPastHeader = true;
				}
				continue;
			}

			if (Trimmed.IsEmpty())
			{
				if (Lines.Num() > 0)
				{
					break;
				}
				continue;
			}

			Lines.Add(StripBriefBulletPrefix(Trimmed));
			if (Lines.Num() >= 5)
			{
				break;
			}
		}

		if (Lines.Num() < 3)
		{
			Lines.Reset();
		}
		return Lines;
	}

	FString BuildToolExposureContext(const FAgentCanonExecution& Execution)
	{
		const FString EnabledFamilies = Execution.EnabledToolFamilyIds.Num() > 0
			? FString::Join(Execution.EnabledToolFamilyIds, TEXT(", "))
			: TEXT("none");
		FString Context = FString::Printf(
			TEXT("requested_tool_family = %s\nenabled_tool_family_ids = %s\ntool_exposure_rule = prefer the requested family for mutation and keep workspace fallback auxiliary/observable only\nbrief_part_b_rule = for risky mutation emit 'Implementation Brief Part B:' with 3-5 lines before the first mutating tool call"),
			*Execution.RequestedToolFamily,
			*EnabledFamilies);
		if (Execution.RequestedToolFamily == InteractionAccessFeatureRecipeId
			&& Execution.FeatureWorkflow.CurrentPhase == TEXT("input_asset_authoring"))
		{
			Context += TEXT(
				"\nphase_preferred_tool_family = unreal_input"
				"\nphase_first_read_only_path = use enhanced_input query/read operations on the existing persistent IMC/action path before generic inspection"
				"\nphase_auxiliary_families = unreal_assets -> workspace_inspection"
				"\nphase_drift_guard = do not spend this phase on proof-map, actor, or viewport inspection before the input-path verification attempt");
		}
		return Context;
	}

	TArray<FString> BuildImplementationBriefLines(const FAgentCanonExecution& Execution)
	{
		const bool bBriefRequired =
			Execution.TaskMode == TEXT("canon_discovery")
			|| (Execution.TaskMode == TEXT("directed_execution") && IsRiskySubsystem(Execution.DetectedSubsystem))
			|| Execution.TaskMode == TEXT("bounded_unreal_mutation")
			|| Execution.TaskMode == TEXT("feature_slice");

		if (!bBriefRequired)
		{
			return {};
		}

		TArray<FString> Lines;
		const FString ApprovedPatternSummary = Execution.bApprovedPatternFound
			? FString::Printf(TEXT("true (%s)"), *Execution.ApprovedPatternKey)
			: TEXT("false");
		Lines.Add(FString::Printf(TEXT("subsystem = %s"), *Execution.DetectedSubsystem));
		Lines.Add(FString::Printf(TEXT("task_mode = %s"), *Execution.TaskMode));
		Lines.Add(FString::Printf(TEXT("requested_tool_family = %s"), *Execution.RequestedToolFamily));
		Lines.Add(FString::Printf(TEXT("approved_pattern = %s"), *ApprovedPatternSummary));
		Lines.Add(TEXT("source_priority = project_local -> official_unreal_docs -> official_first_party_samples -> community_support_only"));
		return Lines;
	}

	TArray<FString> TokensForSubsystem(const FString& Subsystem)
	{
		if (Subsystem == TEXT("animation"))
		{
			return { TEXT("anim"), TEXT("animation"), TEXT("montage"), TEXT("blendspace"), TEXT("state"), TEXT("machine"), TEXT("rig") };
		}
		if (Subsystem == TEXT("input"))
		{
			return { TEXT("input"), TEXT("mapping"), TEXT("context"), TEXT("bind"), TEXT("action"), TEXT("enhanced") };
		}
		if (Subsystem == TEXT("networking"))
		{
			return { TEXT("network"), TEXT("replication"), TEXT("authority"), TEXT("server"), TEXT("client"), TEXT("rpc") };
		}
		if (Subsystem == TEXT("persistence"))
		{
			return { TEXT("save"), TEXT("load"), TEXT("serialize"), TEXT("snapshot"), TEXT("restore"), TEXT("persistence") };
		}
		if (Subsystem == TEXT("ui"))
		{
			return { TEXT("widget"), TEXT("umg"), TEXT("hud"), TEXT("menu"), TEXT("ui"), TEXT("screen") };
		}
		if (Subsystem == TEXT("physics"))
		{
			return { TEXT("physics"), TEXT("collision"), TEXT("overlap"), TEXT("chaos"), TEXT("impulse"), TEXT("ragdoll") };
		}
		if (Subsystem == TEXT("blueprint"))
		{
			return { TEXT("blueprint"), TEXT("graph") };
		}
		if (Subsystem == TEXT("assets"))
		{
			return { TEXT("asset"), TEXT("uasset"), TEXT("mesh"), TEXT("texture"), TEXT("level") };
		}
		if (Subsystem == TEXT("materials_vfx"))
		{
			return { TEXT("material"), TEXT("shader"), TEXT("niagara"), TEXT("vfx") };
		}
		return {};
	}

	int32 ScorePatternMatch(
		const FUnrealClaudeCanonLedgerEntry& Entry,
		const TArray<FString>& PromptTokens,
		const FString& DetectedSubsystem)
	{
		const bool bSameSubsystem = Entry.Subsystem.Equals(DetectedSubsystem, ESearchCase::IgnoreCase);
		int32 Score = 0;
		if (bSameSubsystem && IsRiskySubsystem(DetectedSubsystem) && DetectedSubsystem != TEXT("general"))
		{
			Score += 100;
		}
		else if (bSameSubsystem && DetectedSubsystem != TEXT("general"))
		{
			Score += 4;
		}

		const FString SearchBlob = FString::Printf(
			TEXT("%s %s %s %s"),
			*Entry.PatternKey,
			*Entry.ShortTitle,
			*Entry.ChosenPathSummary,
			*Entry.WhyPreferred).ToLower();

		for (const FString& Token : PromptTokens)
		{
			if (SearchBlob.Contains(Token, ESearchCase::CaseSensitive))
			{
				Score += 1;
			}
		}

		const FString NormalizedPatternKey = NormalizeForTokens(Entry.PatternKey);
		for (const FString& Token : Tokenize(Entry.PatternKey))
		{
			if (PromptTokens.Contains(Token))
			{
				Score += 2;
			}
		}
		if (!NormalizedPatternKey.IsEmpty() && NormalizeForTokens(FString::Join(PromptTokens, TEXT(" "))).Contains(NormalizedPatternKey, ESearchCase::CaseSensitive))
		{
			Score += 5;
		}

		if (DetectedSubsystem != TEXT("general") && !bSameSubsystem)
		{
			for (const FString& ConflictingToken : TokensForSubsystem(Entry.Subsystem))
			{
				if (PromptTokens.Contains(ConflictingToken))
				{
					Score -= 2;
				}
			}
		}

		return Score;
	}

	bool TryBuildLedgerEntryFromProvenRun(
		const FUnrealClaudeCanonPromotionRequest& Request,
		FUnrealClaudeCanonLedgerEntry& OutEntry,
		FString& OutError)
	{
		OutEntry = FUnrealClaudeCanonLedgerEntry();
		OutError.Reset();

		if (!Request.IsValid())
		{
			OutError = TEXT("Canon promotion request is incomplete");
			return false;
		}

		const FString ReceiptPath = ResolveProvenRunReceiptPath(Request.ProvenRunReceiptPath);
		if (ReceiptPath.IsEmpty())
		{
			OutError = TEXT("Canon promotion request has no proven receipt path");
			return false;
		}

		if (!IFileManager::Get().FileExists(*ReceiptPath))
		{
			OutError = FString::Printf(TEXT("Proven receipt does not exist: %s"), *ReceiptPath);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		if (!LoadJsonObjectFromPath(ReceiptPath, Root, OutError))
		{
			return false;
		}

		FString State;
		Root->TryGetStringField(TEXT("state"), State);
		if (!State.Equals(TEXT("completed"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Proven receipt is not completed: %s"), *ReceiptPath);
			return false;
		}

		bool bSuccess = false;
		if (!Root->TryGetBoolField(TEXT("success"), bSuccess) || !bSuccess)
		{
			OutError = FString::Printf(TEXT("Proven receipt is not successful: %s"), *ReceiptPath);
			return false;
		}

		const TSharedPtr<FJsonObject>* CanonObject = nullptr;
		if (!Root->TryGetObjectField(TEXT("canon_execution"), CanonObject) || CanonObject == nullptr || !(*CanonObject).IsValid())
		{
			OutError = FString::Printf(TEXT("Proven receipt is missing canon_execution: %s"), *ReceiptPath);
			return false;
		}

		FString DetectedSubsystem;
		(*CanonObject)->TryGetStringField(TEXT("detected_subsystem"), DetectedSubsystem);
		if (DetectedSubsystem.IsEmpty() || DetectedSubsystem == TEXT("general"))
		{
			OutError = FString::Printf(TEXT("Proven receipt does not identify a promotable subsystem: %s"), *ReceiptPath);
			return false;
		}

		if (!Request.ExpectedSubsystem.IsEmpty() && !DetectedSubsystem.Equals(Request.ExpectedSubsystem, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("Proven receipt subsystem mismatch: expected %s, got %s"),
				*Request.ExpectedSubsystem,
				*DetectedSubsystem);
			return false;
		}

		FString VerificationOutcome;
		(*CanonObject)->TryGetStringField(TEXT("verification_outcome"), VerificationOutcome);
		if (!VerificationOutcome.Equals(TEXT("pass"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Proven receipt verification did not pass: %s"), *ReceiptPath);
			return false;
		}

		bool bMutatingFallbackUsed = true;
		if (!(*CanonObject)->TryGetBoolField(TEXT("mutating_fallback_used"), bMutatingFallbackUsed) || bMutatingFallbackUsed)
		{
			OutError = FString::Printf(TEXT("Proven receipt used mutating fallback or does not report it cleanly: %s"), *ReceiptPath);
			return false;
		}

		FString PrimaryMutationToolFamily;
		(*CanonObject)->TryGetStringField(TEXT("primary_mutation_tool_family"), PrimaryMutationToolFamily);
		if (PrimaryMutationToolFamily.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Proven receipt is missing primary_mutation_tool_family: %s"), *ReceiptPath);
			return false;
		}

		OutEntry.PatternKey = Request.PatternKey.TrimStartAndEnd();
		OutEntry.Subsystem = DetectedSubsystem;
		OutEntry.ShortTitle = Request.ShortTitle.TrimStartAndEnd();
		OutEntry.ChosenPathSummary = Request.ChosenPathSummary.TrimStartAndEnd();
		OutEntry.WhyPreferred = Request.WhyPreferred.TrimStartAndEnd();
		OutEntry.ProofReference = MakeProjectRelativePath(ReceiptPath);
		OutEntry.BadPathToAvoid = Request.BadPathToAvoid.TrimStartAndEnd();
		OutEntry.LastConfirmedEngineContext = Request.LastConfirmedEngineContext.TrimStartAndEnd();
		if (OutEntry.LastConfirmedEngineContext.IsEmpty())
		{
			OutEntry.LastConfirmedEngineContext = BuildCurrentEngineContextTag();
		}

		if (!OutEntry.IsValid())
		{
			OutError = TEXT("Canon ledger entry built from proven receipt is incomplete");
			return false;
		}

		return true;
	}

	TSharedPtr<FJsonObject> MakeLedgerEntryJson(const FUnrealClaudeCanonLedgerEntry& Entry)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("pattern_key"), Entry.PatternKey);
		Object->SetStringField(TEXT("subsystem"), Entry.Subsystem);
		Object->SetStringField(TEXT("short_title"), Entry.ShortTitle);
		Object->SetStringField(TEXT("chosen_path_summary"), Entry.ChosenPathSummary);
		Object->SetStringField(TEXT("why_preferred"), Entry.WhyPreferred);
		Object->SetStringField(TEXT("proof_reference"), Entry.ProofReference);
		if (!Entry.BadPathToAvoid.IsEmpty())
		{
			Object->SetStringField(TEXT("bad_path_to_avoid"), Entry.BadPathToAvoid);
		}
		if (!Entry.LastConfirmedEngineContext.IsEmpty())
		{
			Object->SetStringField(TEXT("last_confirmed_engine_context"), Entry.LastConfirmedEngineContext);
		}
		return Object;
	}

	bool TryParseLedgerEntry(const TSharedPtr<FJsonObject>& Object, FUnrealClaudeCanonLedgerEntry& OutEntry)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		Object->TryGetStringField(TEXT("pattern_key"), OutEntry.PatternKey);
		Object->TryGetStringField(TEXT("subsystem"), OutEntry.Subsystem);
		Object->TryGetStringField(TEXT("short_title"), OutEntry.ShortTitle);
		Object->TryGetStringField(TEXT("chosen_path_summary"), OutEntry.ChosenPathSummary);
		Object->TryGetStringField(TEXT("why_preferred"), OutEntry.WhyPreferred);
		Object->TryGetStringField(TEXT("proof_reference"), OutEntry.ProofReference);
		Object->TryGetStringField(TEXT("bad_path_to_avoid"), OutEntry.BadPathToAvoid);
		Object->TryGetStringField(TEXT("last_confirmed_engine_context"), OutEntry.LastConfirmedEngineContext);
		return OutEntry.IsValid();
	}
}

FString FUnrealClaudeCanonLedger::GetLedgerPath()
{
	return ResolveLedgerPath();
}

bool FUnrealClaudeCanonLedger::LoadEntries(TArray<FUnrealClaudeCanonLedgerEntry>& OutEntries, FString& OutError)
{
	OutEntries.Reset();
	OutError.Reset();

	const FString LedgerPath = ResolveLedgerPath();
	if (!IFileManager::Get().FileExists(*LedgerPath))
	{
		return true;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *LedgerPath))
	{
		OutError = FString::Printf(TEXT("Failed to read canon ledger: %s"), *LedgerPath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to parse canon ledger JSON: %s"), *LedgerPath);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Root->TryGetArrayField(TEXT("entries"), Entries) || Entries == nullptr)
	{
		return true;
	}

	for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
	{
		const TSharedPtr<FJsonObject> EntryObject = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
		FUnrealClaudeCanonLedgerEntry Entry;
		if (TryParseLedgerEntry(EntryObject, Entry))
		{
			OutEntries.Add(Entry);
		}
	}

	return true;
}

bool FUnrealClaudeCanonLedger::SaveEntries(const TArray<FUnrealClaudeCanonLedgerEntry>& Entries, FString& OutError)
{
	OutError.Reset();

	const FString LedgerPath = ResolveLedgerPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LedgerPath), true);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("canon_ledger_v1"));
	Root->SetStringField(TEXT("updated_at_utc"), FDateTime::UtcNow().ToIso8601());

	TArray<TSharedPtr<FJsonValue>> EntryValues;
	for (const FUnrealClaudeCanonLedgerEntry& Entry : Entries)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		EntryValues.Add(MakeShared<FJsonValueObject>(MakeLedgerEntryJson(Entry)));
	}
	Root->SetArrayField(TEXT("entries"), EntryValues);

	FString JsonText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	if (!FFileHelper::SaveStringToFile(JsonText, *LedgerPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write canon ledger: %s"), *LedgerPath);
		return false;
	}

	return true;
}

bool FUnrealClaudeCanonLedger::PromoteEntry(const FUnrealClaudeCanonLedgerEntry& Entry, FString& OutError)
{
	OutError.Reset();

	if (!Entry.IsValid())
	{
		OutError = TEXT("Canon ledger entry is incomplete");
		return false;
	}

	TArray<FUnrealClaudeCanonLedgerEntry> Entries;
	if (!LoadEntries(Entries, OutError))
	{
		return false;
	}

	const int32 ExistingIndex = Entries.IndexOfByPredicate([&Entry](const FUnrealClaudeCanonLedgerEntry& Existing)
	{
		return Existing.PatternKey.Equals(Entry.PatternKey, ESearchCase::IgnoreCase);
	});

	if (ExistingIndex != INDEX_NONE)
	{
		Entries[ExistingIndex] = Entry;
	}
	else
	{
		Entries.Add(Entry);
	}

	return SaveEntries(Entries, OutError);
}

bool FUnrealClaudeCanonLedger::PromoteFromProvenRun(
	const FUnrealClaudeCanonPromotionRequest& Request,
	FString& OutError,
	FUnrealClaudeCanonLedgerEntry* OutPromotedEntry)
{
	FUnrealClaudeCanonLedgerEntry Entry;
	if (!TryBuildLedgerEntryFromProvenRun(Request, Entry, OutError))
	{
		return false;
	}

	const bool bPromoted = PromoteEntry(Entry, OutError);
	if (bPromoted && OutPromotedEntry != nullptr)
	{
		*OutPromotedEntry = Entry;
	}
	return bPromoted;
}

FString FUnrealClaudeCanonLedger::BuildPromptContext(
	const FString& Prompt,
	const FString& DetectedSubsystem,
	bool& bOutApprovedPatternFound,
	FString& OutApprovedPatternKey)
{
	bOutApprovedPatternFound = false;
	OutApprovedPatternKey.Reset();

	TArray<FUnrealClaudeCanonLedgerEntry> Entries;
	FString LoadError;
	const bool bLoaded = LoadEntries(Entries, LoadError);
	const FString LedgerPath = ResolveLedgerPath();
	const TArray<FString> PromptTokens = Tokenize(Prompt);
	const bool bSubsystemLevelReuseEligible = IsRiskySubsystem(DetectedSubsystem) && DetectedSubsystem != TEXT("general");

	int32 BestScore = 0;
	const FUnrealClaudeCanonLedgerEntry* BestEntry = nullptr;
	int32 RelevantSubsystemEntries = 0;
	for (const FUnrealClaudeCanonLedgerEntry& Entry : Entries)
	{
		const bool bSameSubsystem = Entry.Subsystem.Equals(DetectedSubsystem, ESearchCase::IgnoreCase);
		if (bSameSubsystem)
		{
			++RelevantSubsystemEntries;
		}

		if (bSubsystemLevelReuseEligible && !bSameSubsystem)
		{
			continue;
		}

		const int32 Score = ScorePatternMatch(Entry, PromptTokens, DetectedSubsystem);
		if (Score > BestScore)
		{
			BestScore = Score;
			BestEntry = &Entry;
		}
	}

	const int32 ApprovalThreshold = bSubsystemLevelReuseEligible ? 100 : 3;
	if (bLoaded && BestEntry != nullptr && BestScore >= ApprovalThreshold)
	{
		bOutApprovedPatternFound = true;
		OutApprovedPatternKey = BestEntry->PatternKey;
	}

	if (!bLoaded)
	{
		return FString::Printf(
			TEXT("ledger_path = %s\nledger_status = read_error\nload_error = %s\npromotion_rule = explicit_post_verification_only"),
			*LedgerPath,
			*LoadError);
	}

	if (BestEntry != nullptr && BestScore >= ApprovalThreshold)
	{
		FString Context = FString::Printf(
			TEXT("ledger_path = %s\napproved_pattern_found = true\npattern_key = %s\nshort_title = %s\nchosen_path_summary = %s\nwhy_preferred = %s\nproof_reference = %s\nmatch_basis = %s\nmatch_score = %d"),
			*LedgerPath,
			*BestEntry->PatternKey,
			*BestEntry->ShortTitle,
			*BestEntry->ChosenPathSummary,
			*BestEntry->WhyPreferred,
			*BestEntry->ProofReference,
			bSubsystemLevelReuseEligible ? TEXT("same_subsystem_approved_family") : TEXT("token_replay_support"),
			BestScore);
		if (!BestEntry->BadPathToAvoid.IsEmpty())
		{
			Context += FString::Printf(TEXT("\nbad_path_to_avoid = %s"), *BestEntry->BadPathToAvoid);
		}
		if (!BestEntry->LastConfirmedEngineContext.IsEmpty())
		{
			Context += FString::Printf(TEXT("\nlast_confirmed_engine_context = %s"), *BestEntry->LastConfirmedEngineContext);
		}
		Context += TEXT("\npromotion_rule = explicit_post_verification_only");
		return Context;
	}

	return FString::Printf(
		TEXT("ledger_path = %s\napproved_pattern_found = false\nmatched_pattern_key = none\nentry_count = %d\nrelevant_subsystem_entries = %d\nmatch_basis = %s\npromotion_rule = explicit_post_verification_only"),
		*LedgerPath,
		Entries.Num(),
		RelevantSubsystemEntries,
		bSubsystemLevelReuseEligible ? TEXT("same_subsystem_approved_family") : TEXT("token_replay_support"));
}

void FUnrealClaudeCanonLedger::SetTestLedgerPathOverride(const FString& InPath)
{
	GCanonLedgerPathOverride = InPath;
}

void FUnrealClaudeCanonLedger::ClearTestLedgerPathOverride()
{
	GCanonLedgerPathOverride.Reset();
}

namespace UnrealClaudeCanonRouting
{
	FAgentCanonExecution BuildInitialCanonExecution(
		const FString& Prompt,
		const EAgentExecutionRunProfile Profile,
		TArray<FAgentPromptContextBlock>& InOutContextBlocks)
	{
		FAgentCanonExecution Execution;
		Execution.DetectedSubsystem = DetectSubsystem(Prompt);
		Execution.LedgerPath = FUnrealClaudeCanonLedger::GetLedgerPath();

		FString ApprovedPatternKey;
		const FString LedgerContext = FUnrealClaudeCanonLedger::BuildPromptContext(
			Prompt,
			Execution.DetectedSubsystem,
			Execution.bApprovedPatternFound,
			ApprovedPatternKey);
		Execution.ApprovedPatternKey = ApprovedPatternKey;
		Execution.TaskMode = DetermineTaskMode(Prompt, Execution.DetectedSubsystem, Execution.bApprovedPatternFound, Profile);
		Execution.bCanonDiscoveryUsed = Execution.TaskMode == TEXT("canon_discovery");
		const FString FeatureRecipeId = Execution.TaskMode == TEXT("feature_slice")
			? DetermineFeatureRecipeId(Prompt)
			: FString();
		Execution.RequestedToolFamily = DetermineRequestedToolFamily(
			Execution.TaskMode,
			Execution.DetectedSubsystem,
			FeatureRecipeId);
		if (Execution.TaskMode == TEXT("feature_slice")
			&& Execution.RequestedToolFamily == InventoryBasicFeatureRecipeId)
		{
			Execution.FeatureWorkflow = MakeInventoryBasicFeatureWorkflow(Prompt);
		}
		else if (Execution.TaskMode == TEXT("feature_slice")
			&& Execution.RequestedToolFamily == InteractionAccessFeatureRecipeId)
		{
			Execution.FeatureWorkflow = MakeInteractionAccessFeatureWorkflow(Prompt);
		}
		Execution.VerificationOutcome = TEXT("pending");
		Execution.bBriefPartBRequired = RequiresBriefPartB(Execution);
		Execution.ImplementationBriefLines = BuildImplementationBriefLines(Execution);
		Execution.bBriefWasProduced = Execution.ImplementationBriefLines.Num() > 0;

		AddContextBlock(
			InOutContextBlocks,
			TEXT("TASK MODE CLASSIFICATION"),
			BuildTaskModeClassificationContext(Execution));

		AddContextBlock(InOutContextBlocks, TEXT("PROJECT-LOCAL CANON LEDGER"), LedgerContext);
		if (Execution.FeatureWorkflow.HasAnySignal())
		{
			AddContextBlock(
				InOutContextBlocks,
				TEXT("FEATURE WORKFLOW"),
				BuildFeatureWorkflowContext(Execution.FeatureWorkflow));
		}

		if (Execution.bBriefWasProduced)
		{
			AddContextBlock(
				InOutContextBlocks,
				TEXT("RUN-SCOPED IMPLEMENTATION BRIEF PART A"),
				JoinLines(Execution.ImplementationBriefLines));
		}

		return Execution;
	}

	void ApplyRuntimeToolExposure(FAgentRequestConfig& Config)
	{
		if (Config.ExecutionProfile != EAgentExecutionRunProfile::ConfiguredDefaultRuntime)
		{
			return;
		}

		TArray<FString> EnabledFamilies = ResolveEnabledToolFamilyIds(Config.CanonExecution);
		if (EnabledFamilies.Num() == 0)
		{
			return;
		}

		const TArray<FString> FamilyAllowedTools = BuildAllowedToolsForFamilies(EnabledFamilies);
		if (FamilyAllowedTools.Num() == 0)
		{
			return;
		}

		TArray<FString> AdjustedAllowedTools = Config.AllowedTools;
		for (const FString& AllowedTool : FamilyAllowedTools)
		{
			AddUniqueString(AdjustedAllowedTools, AllowedTool);
		}

		const FString PreviousAllowedTools = FString::Join(Config.AllowedTools, TEXT(","));
		const FString NewAllowedTools = FString::Join(AdjustedAllowedTools, TEXT(","));
		Config.AllowedTools = AdjustedAllowedTools;
		Config.CanonExecution.EnabledToolFamilyIds = EnabledFamilies;
		Config.CanonExecution.ImplementationBriefLines = BuildImplementationBriefLines(Config.CanonExecution);
		Config.CanonExecution.bBriefWasProduced = Config.CanonExecution.ImplementationBriefLines.Num() > 0;
		Config.CanonExecution.bToolExposureAdjusted =
			!PreviousAllowedTools.Equals(NewAllowedTools, ESearchCase::CaseSensitive);

		UpsertContextBlock(
			Config.PromptContract.ContextBlocks,
			TEXT("TASK MODE CLASSIFICATION"),
			BuildTaskModeClassificationContext(Config.CanonExecution));
		if (Config.CanonExecution.FeatureWorkflow.HasAnySignal())
		{
			UpsertContextBlock(
				Config.PromptContract.ContextBlocks,
				TEXT("FEATURE WORKFLOW"),
				BuildFeatureWorkflowContext(Config.CanonExecution.FeatureWorkflow));
		}
		if (Config.CanonExecution.bBriefWasProduced)
		{
			UpsertContextBlock(
				Config.PromptContract.ContextBlocks,
				TEXT("RUN-SCOPED IMPLEMENTATION BRIEF PART A"),
				JoinLines(Config.CanonExecution.ImplementationBriefLines));
		}
		UpsertContextBlock(
			Config.PromptContract.ContextBlocks,
			TEXT("CANON TOOL EXPOSURE"),
			BuildToolExposureContext(Config.CanonExecution));
	}

	FString DetermineToolFamily(const FAgentCanonExecution& Execution, const FString& ToolName, const FString& ToolInput)
	{
		const FString CanonicalName = NormalizeObservedToolName(ToolName);

		if (!Execution.RequestedToolFamily.IsEmpty() && FamilyContainsTool(Execution.RequestedToolFamily, CanonicalName))
		{
			return Execution.RequestedToolFamily;
		}

		for (const FString& FamilyId : Execution.EnabledToolFamilyIds)
		{
			if (!FamilyId.Equals(Execution.RequestedToolFamily, ESearchCase::IgnoreCase) && FamilyContainsTool(FamilyId, CanonicalName))
			{
				return FamilyId;
			}
		}

		if (CanonicalName == TEXT("read") || CanonicalName == TEXT("grep") || CanonicalName == TEXT("glob"))
		{
			return TEXT("workspace_inspection");
		}

		if (CanonicalName == TEXT("unreal_status") || CanonicalName == TEXT("status"))
		{
			return TEXT("workspace_inspection");
		}

		if (CanonicalName == TEXT("write") || CanonicalName == TEXT("edit"))
		{
			return TEXT("workspace_file_edit");
		}

		if (CanonicalName == TEXT("bash")
			|| CanonicalName == TEXT("command_execution")
			|| CanonicalName == TEXT("execute_terminal"))
		{
			return IsReadOnlyShellToolInput(ToolInput)
				? TEXT("workspace_inspection")
				: TEXT("workspace_file_build");
		}

		if (CanonicalName == TEXT("restart_survival"))
		{
			return TEXT("restart_survival");
		}

		if (CanonicalName.StartsWith(TEXT("osvayder_")))
		{
			return TEXT("osvayder_ui");
		}

		if (CanonicalName.Contains(TEXT("anim_blueprint")) || CanonicalName.Contains(TEXT("animation")))
		{
			return TEXT("unreal_animation");
		}

		if (CanonicalName.Contains(TEXT("blueprint")))
		{
			return TEXT("unreal_blueprint");
		}

		if (CanonicalName.Contains(TEXT("input")) || CanonicalName == TEXT("enhanced_input"))
		{
			return TEXT("unreal_input");
		}

		if (CanonicalName.Contains(TEXT("material")) || CanonicalName.Contains(TEXT("niagara")))
		{
			return TEXT("unreal_materials_vfx");
		}

		if (CanonicalName.Contains(TEXT("asset")) || CanonicalName.Contains(TEXT("dependencies")) || CanonicalName.Contains(TEXT("referencers")))
		{
			return TEXT("unreal_assets");
		}

		if (CanonicalName.Contains(TEXT("actor")) || CanonicalName.Contains(TEXT("level")) || CanonicalName.Contains(TEXT("property")))
		{
			return TEXT("unreal_editor_tools");
		}

		if (CanonicalName.Contains(TEXT("task_")) || CanonicalName.Contains(TEXT("viewport")) || CanonicalName.Contains(TEXT("console_command")))
		{
			return TEXT("unreal_editor_tools");
		}

		return TEXT("other");
	}

	bool IsMutatingToolUse(const FString& ToolName, const FString& ToolInput)
	{
		return IsMutatingToolUseInternal(ToolName, ToolInput);
	}

	void UpdateFromToolUse(
		FAgentCanonExecution& Execution,
		const FString& ToolName,
		const FString& ToolInput,
		const FString& PreMutationAssistantText)
	{
		const FString ToolFamily = DetermineToolFamily(Execution, ToolName, ToolInput);
		if (ToolFamily.IsEmpty())
		{
			return;
		}

		const bool bIsMutatingTool = IsMutatingToolUseInternal(ToolName, ToolInput);

		if (!Execution.ActualToolFamily.IsEmpty()
			&& !Execution.ActualToolFamily.Equals(ToolFamily, ESearchCase::IgnoreCase))
		{
			Execution.bFallbackToolUsed = true;
		}

		if (!Execution.RequestedToolFamily.IsEmpty()
			&& !Execution.RequestedToolFamily.Equals(ToolFamily, ESearchCase::IgnoreCase))
		{
			Execution.bFallbackToolUsed = true;
		}

		Execution.ActualToolFamily = ToolFamily;

		if (bIsMutatingTool)
		{
			if (Execution.PrimaryMutationToolFamily.IsEmpty())
			{
				Execution.ImplementationBriefPartBLines = ParseImplementationBriefPartBLines(PreMutationAssistantText);
				Execution.bBriefPartBProduced = Execution.ImplementationBriefPartBLines.Num() >= 3;
				Execution.bBriefPartBProducedBeforeFirstMutatingTool = Execution.bBriefPartBProduced;
				Execution.PrimaryMutationToolFamily = ToolFamily;
				if (!Execution.RequestedToolFamily.IsEmpty()
					&& !Execution.RequestedToolFamily.Equals(ToolFamily, ESearchCase::IgnoreCase))
				{
					Execution.bMutatingFallbackUsed = true;
					Execution.bFallbackToolUsed = true;
				}
				return;
			}

			if (!Execution.PrimaryMutationToolFamily.Equals(ToolFamily, ESearchCase::IgnoreCase))
			{
				AddUniqueString(Execution.AuxiliaryToolFamilies, ToolFamily);
			}
			return;
		}

		if (!ToolFamily.Equals(Execution.RequestedToolFamily, ESearchCase::IgnoreCase))
		{
			AddUniqueString(Execution.AuxiliaryToolFamilies, ToolFamily);
		}
	}

	void MarkPolicyDeny(FAgentCanonExecution& Execution)
	{
		Execution.bPolicyDenyOccurred = true;
	}

	void Finalize(FAgentCanonExecution& Execution, const bool bSuccess, const bool bTimedOut)
	{
		if (Execution.bPolicyDenyOccurred)
		{
			Execution.VerificationOutcome = TEXT("blocked_policy_deny");
			return;
		}

		Execution.VerificationOutcome = bSuccess
			? TEXT("pass")
			: (bTimedOut ? TEXT("timeout") : TEXT("fail"));
	}

	TSharedPtr<FJsonObject> MakeCanonExecutionJson(const FAgentCanonExecution& Execution)
	{
		if (!Execution.HasAnySignal())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		if (!Execution.DetectedSubsystem.IsEmpty())
		{
			Object->SetStringField(TEXT("detected_subsystem"), Execution.DetectedSubsystem);
		}
		if (!Execution.TaskMode.IsEmpty())
		{
			Object->SetStringField(TEXT("task_mode"), Execution.TaskMode);
		}
		Object->SetBoolField(TEXT("canon_discovery_used"), Execution.bCanonDiscoveryUsed);
		Object->SetBoolField(TEXT("brief_was_produced"), Execution.bBriefWasProduced);
		Object->SetBoolField(TEXT("brief_part_b_required"), Execution.bBriefPartBRequired);
		Object->SetBoolField(TEXT("brief_part_b_produced"), Execution.bBriefPartBProduced);
		Object->SetBoolField(TEXT("brief_part_b_produced_before_first_mutating_tool"), Execution.bBriefPartBProducedBeforeFirstMutatingTool);
		Object->SetBoolField(TEXT("approved_pattern_found"), Execution.bApprovedPatternFound);
		Object->SetBoolField(TEXT("tool_exposure_adjusted"), Execution.bToolExposureAdjusted);
		if (!Execution.RequestedToolFamily.IsEmpty())
		{
			Object->SetStringField(TEXT("requested_tool_family"), Execution.RequestedToolFamily);
		}
		if (Execution.EnabledToolFamilyIds.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> EnabledFamilies;
			for (const FString& FamilyId : Execution.EnabledToolFamilyIds)
			{
				EnabledFamilies.Add(MakeShared<FJsonValueString>(FamilyId));
			}
			Object->SetArrayField(TEXT("enabled_tool_family_ids"), EnabledFamilies);
		}
		if (!Execution.ActualToolFamily.IsEmpty())
		{
			Object->SetStringField(TEXT("actual_tool_family"), Execution.ActualToolFamily);
		}
		if (!Execution.PrimaryMutationToolFamily.IsEmpty())
		{
			Object->SetStringField(TEXT("primary_mutation_tool_family"), Execution.PrimaryMutationToolFamily);
		}
		if (Execution.AuxiliaryToolFamilies.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> AuxiliaryFamilies;
			for (const FString& FamilyId : Execution.AuxiliaryToolFamilies)
			{
				AuxiliaryFamilies.Add(MakeShared<FJsonValueString>(FamilyId));
			}
			Object->SetArrayField(TEXT("auxiliary_tool_families"), AuxiliaryFamilies);
		}
		Object->SetBoolField(TEXT("fallback_tool_used"), Execution.bFallbackToolUsed);
		Object->SetBoolField(TEXT("mutating_fallback_used"), Execution.bMutatingFallbackUsed);
		Object->SetBoolField(TEXT("policy_deny_occurred"), Execution.bPolicyDenyOccurred);
		if (!Execution.VerificationOutcome.IsEmpty())
		{
			Object->SetStringField(TEXT("verification_outcome"), Execution.VerificationOutcome);
		}
		if (Execution.ImplementationBriefLines.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> BriefValues;
			for (const FString& Line : Execution.ImplementationBriefLines)
			{
				BriefValues.Add(MakeShared<FJsonValueString>(Line));
			}
			Object->SetArrayField(TEXT("implementation_brief_lines"), BriefValues);
		}
		if (Execution.ImplementationBriefPartBLines.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> BriefPartBValues;
			for (const FString& Line : Execution.ImplementationBriefPartBLines)
			{
				BriefPartBValues.Add(MakeShared<FJsonValueString>(Line));
			}
			Object->SetArrayField(TEXT("implementation_brief_part_b_lines"), BriefPartBValues);
		}
		if (!Execution.ApprovedPatternKey.IsEmpty())
		{
			Object->SetStringField(TEXT("approved_pattern_key"), Execution.ApprovedPatternKey);
		}
		if (!Execution.LedgerPath.IsEmpty())
		{
			Object->SetStringField(TEXT("ledger_path"), Execution.LedgerPath);
		}
		if (Execution.FeatureWorkflow.HasAnySignal())
		{
			Object->SetObjectField(TEXT("feature_workflow"), Execution.FeatureWorkflow.ToJsonObject());
		}
		return Object;
	}

	TSharedPtr<FJsonObject> ExtractCanonExecutionFromTraceRecords(const TArray<TSharedPtr<FJsonObject>>& TraceRecords)
	{
		for (int32 Index = TraceRecords.Num() - 1; Index >= 0; --Index)
		{
			const TSharedPtr<FJsonObject>& Record = TraceRecords[Index];
			if (!Record.IsValid())
			{
				continue;
			}

			FString EventType;
			Record->TryGetStringField(TEXT("event_type"), EventType);
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			if (!Record->TryGetObjectField(TEXT("payload"), Payload) || Payload == nullptr || !(*Payload).IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* CanonObject = nullptr;
			if (EventType == TEXT("canon_execution_summary"))
			{
				return *Payload;
			}

			if (EventType == TEXT("run_completed")
				&& (*Payload)->TryGetObjectField(TEXT("canon_execution"), CanonObject)
				&& CanonObject != nullptr
				&& (*CanonObject).IsValid())
			{
				return *CanonObject;
			}
		}

		return nullptr;
	}
}
