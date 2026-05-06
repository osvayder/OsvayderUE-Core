// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPToolRegistry.h"
#include "MCPSavePipeline.h"
#include "MCPTaskQueue.h"
#include "OsvayderSubsystem.h"
#include "OsvayderUEModule.h"
#include "OsvayderUEConstants.h"
#include "Containers/Ticker.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

// Include all tool implementations
#include "Tools/MCPTool_SpawnActor.h"
#include "Tools/MCPTool_GetLevelActors.h"
#include "Tools/MCPTool_SetProperty.h"
#include "Tools/MCPTool_RunConsoleCommand.h"
#include "Tools/MCPTool_DeleteActors.h"
#include "Tools/MCPTool_MoveActor.h"
#include "Tools/MCPTool_GetOutputLog.h"
#include "Tools/MCPTool_ExecuteScript.h"
#include "Tools/MCPTool_CleanupScripts.h"
#include "Tools/MCPTool_GetScriptHistory.h"
#include "Tools/MCPTool_CaptureViewport.h"
#include "Tools/MCPTool_BlueprintQuery.h"
#include "Tools/MCPTool_BlueprintModify.h"
#include "Tools/MCPTool_AnimBlueprintModify.h"
#include "Tools/MCPTool_LocalAnimationPackIntake.h"
#include "Tools/MCPTool_AnimationRetargetFixup.h"
#include "Tools/MCPTool_AssetSearch.h"
#include "Tools/MCPTool_AssetDependencies.h"
#include "Tools/MCPTool_AssetReferencers.h"
#include "Tools/MCPTool_EnhancedInput.h"
#include "Tools/MCPTool_Character.h"
#include "Tools/MCPTool_CharacterData.h"
#include "Tools/MCPTool_Material.h"
#include "Tools/MCPTool_Asset.h"
#include "Tools/MCPTool_OpenLevel.h"
#include "Tools/MCPTool_SandboxMapPlacement.h"

// Task queue tools
#include "Tools/MCPTool_TaskSubmit.h"
#include "Tools/MCPTool_TaskStatus.h"
#include "Tools/MCPTool_TaskResult.h"
#include "Tools/MCPTool_TaskList.h"
#include "Tools/MCPTool_TaskCancel.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Tools/MCPTool_AsyncGuardrailFixture.h"
#endif

// OsvayderEye external proxy tools
#include "Tools/MCPTool_EyeProxy.h"
// GAS and Niagara tools
#include "Tools/MCPTool_GAS.h"
#include "Tools/MCPTool_Niagara.h"
// Multiplayer tools
#include "Tools/MCPTool_Multiplayer.h"
// AI tools
#include "Tools/MCPTool_AI.h"
// Sequencer tools
#include "Tools/MCPTool_Sequencer.h"
// Plugin settings and memory
#include "Tools/MCPTool_PluginSettings.h"
#include "Tools/MCPTool_ProjectMemoryStatus.h"
#include "Tools/MCPTool_ExecutionLogStatus.h"
#include "Tools/MCPTool_AgentTraceStatus.h"
#include "Tools/MCPTool_ReportExport.h"
#include "Tools/MCPTool_ReportArtifactStatus.h"
#include "Tools/MCPTool_CppReflection.h"
#include "Tools/MCPTool_DependencyHealth.h"
#include "Tools/MCPTool_LiveCodingCompile.h"
#include "Tools/MCPTool_MetadataTruth.h"
#include "Tools/MCPTool_MapRuntimeProof.h"
#include "Tools/MCPTool_MechanicPreflight.h"
#include "Tools/MCPTool_MutationGroup.h"
#include "Tools/MCPTool_OssSessionProof.h"
#include "Tools/MCPTool_RestartSurvival.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUESettings.h"
#include "OsvayderUEAgentTrace.h"

namespace
{
	/**
	 * Thread-local reentrancy guard for the mutation-lifecycle wrapper.
	 *
	 * Invariant (P5 A-P5-1): if `WorkOnGameThread` is already somewhere on the
	 * stack for this thread, `GMCPLifecycleDepth` is > 0 and any nested
	 * `FMCPToolRegistry::ExecuteTool` from inside that running tool
	 * (typically a composite tool that dispatches subordinate MCP tools to
	 * build up its effect) MUST return the inner tool's raw result without
	 * attaching its own `lifecycle` field. The single enclosing (outer) call
	 * then runs `SnapshotDirtyPackages` once, `Run(FSaveSpec)` once,
	 * `BuildLifecycleJson` once, after the outer tool finishes. Result:
	 * exactly one `UPackage::SavePackage` pass per dirtied package per
	 * composite mutation, not N. Under test by
	 * `Pipeline_Reentrancy_BundledMutation_SavesOnce`.
	 *
	 * Why `thread_local`: the outer `WorkOnGameThread` always runs on the
	 * game thread (the non-game-thread caller path dispatches INTO the game
	 * thread via FTSTicker before executing the lambda). So the relevant
	 * counter is the game-thread counter. A background-thread caller that
	 * triggers the FTSTicker path never touches `GMCPLifecycleDepth` from
	 * its own thread — only the game-thread side increments. thread_local is
	 * the cheapest way to isolate this from any other thread that might run
	 * registry code for unrelated reasons (the plugin avoids that, but a
	 * thread-local zero default is cheap insurance).
	 *
	 * ReadOnly tools and `auto_save=false` tools return before the counter
	 * is incremented and do not participate in the guard; a ReadOnly outer
	 * that internally dispatches a modifying inner tool will let the inner
	 * run its own lifecycle wrap, which is the correct shape — a ReadOnly
	 * tool is not supposed to be persisting anything on its own.
	 */
	thread_local int32 GMCPLifecycleDepth = 0;

	struct FLifecyclePackageContext
	{
		TSet<UPackage*> DirtyBeforeTool;
		TSet<UPackage*> TouchedByTool;
		TSet<UPackage*> DirtyAfterTool;
		TArray<UPackage*> SaveTargets;
		FString SavePolicy;
		FString SkippedReason;
	};

	bool ResolveAutoSaveForMutation(const TSharedRef<FJsonObject>& Params)
	{
		bool bAutoSave = false;
		if (Params->TryGetBoolField(TEXT("auto_save"), bAutoSave))
		{
			return bAutoSave;
		}

		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		return Settings ? Settings->ShouldAutoSaveMutations() : true;
	}

	TSet<UPackage*> SnapshotDirtyPackages()
	{
		TSet<UPackage*> DirtySet;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (Package && Package->IsDirty())
			{
				DirtySet.Add(Package);
			}
		}
		return DirtySet;
	}

	TArray<FString> SortedPackageNamesFromSet(const TSet<UPackage*>& Packages)
	{
		TArray<FString> Names;
		Names.Reserve(Packages.Num());
		for (UPackage* Package : Packages)
		{
			if (Package)
			{
				Names.Add(Package->GetName());
			}
		}
		Names.Sort();
		return Names;
	}

	TArray<FString> SortedPackageNamesFromArray(const TArray<UPackage*>& Packages)
	{
		TArray<FString> Names;
		Names.Reserve(Packages.Num());
		for (UPackage* Package : Packages)
		{
			if (Package)
			{
				Names.Add(Package->GetName());
			}
		}
		Names.Sort();
		return Names;
	}

	void SetStringArrayField(TSharedPtr<FJsonObject> Object, const TCHAR* FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	}

	TSet<FString> PackageNamesFromLifecycleResults(const TArray<OsvayderUE::SavePipeline::FLifecyclePhaseResult>& Results)
	{
		TSet<FString> Names;
		for (const OsvayderUE::SavePipeline::FLifecyclePhaseResult& Result : Results)
		{
			if (!Result.PackageName.IsEmpty() && !Result.PackageName.Equals(TEXT("(batch)")))
			{
				Names.Add(Result.PackageName);
			}
		}
		return Names;
	}

	void AttachPackageObservationJson(
		const TSharedPtr<FJsonObject>& Lifecycle,
		const FLifecyclePackageContext& Context,
		const OsvayderUE::SavePipeline::FLifecycleOutcome* Outcome)
	{
		SetStringArrayField(Lifecycle, TEXT("dirty_before_tool"), SortedPackageNamesFromSet(Context.DirtyBeforeTool));
		SetStringArrayField(Lifecycle, TEXT("touched_by_tool"), SortedPackageNamesFromSet(Context.TouchedByTool));
		SetStringArrayField(Lifecycle, TEXT("dirty_after_tool"), SortedPackageNamesFromSet(Context.DirtyAfterTool));
		SetStringArrayField(Lifecycle, TEXT("save_targets"), SortedPackageNamesFromArray(Context.SaveTargets));

		TSet<FString> SavedNames;
		TSet<FString> FailedNames;
		TSet<FString> DeferredNames;
		if (Outcome)
		{
			SavedNames = PackageNamesFromLifecycleResults(Outcome->Saved);
			FailedNames = PackageNamesFromLifecycleResults(Outcome->Failed);
			DeferredNames = PackageNamesFromLifecycleResults(Outcome->Deferred);
		}

		TSet<UPackage*> ObservedPackages = Context.DirtyBeforeTool;
		ObservedPackages.Append(Context.TouchedByTool);
		ObservedPackages.Append(Context.DirtyAfterTool);
		for (UPackage* Package : Context.SaveTargets)
		{
			if (Package)
			{
				ObservedPackages.Add(Package);
			}
		}

		TArray<UPackage*> SortedPackages = ObservedPackages.Array();
		SortedPackages.Sort([](const UPackage& A, const UPackage& B)
		{
			return A.GetName() < B.GetName();
		});

		TArray<TSharedPtr<FJsonValue>> PackageRows;
		PackageRows.Reserve(SortedPackages.Num());
		for (UPackage* Package : SortedPackages)
		{
			if (!Package)
			{
				continue;
			}

			const FString PackageName = Package->GetName();
			const bool bDirtyBefore = Context.DirtyBeforeTool.Contains(Package);
			const bool bTouched = Context.TouchedByTool.Contains(Package);
			const bool bDirtyAfterTool = Context.DirtyAfterTool.Contains(Package);
			const bool bSaveTarget = Context.SaveTargets.Contains(Package);

			FString SaveResult = TEXT("not_saved");
			if (!Context.SkippedReason.IsEmpty())
			{
				SaveResult = bTouched || bDirtyAfterTool ? TEXT("skipped") : TEXT("not_saved");
			}
			else if (SavedNames.Contains(PackageName))
			{
				SaveResult = TEXT("saved_by_tool");
			}
			else if (FailedNames.Contains(PackageName))
			{
				SaveResult = TEXT("failed");
			}
			else if (DeferredNames.Contains(PackageName))
			{
				SaveResult = TEXT("deferred");
			}
			else if (bDirtyBefore && !bTouched && !bSaveTarget)
			{
				SaveResult = TEXT("not_saved_unrelated_dirty");
			}
			else if (!bDirtyAfterTool)
			{
				SaveResult = TEXT("no_save_needed");
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("package"), PackageName);
			Row->SetBoolField(TEXT("dirty_before_tool"), bDirtyBefore);
			Row->SetBoolField(TEXT("touched_by_tool"), bTouched);
			Row->SetBoolField(TEXT("dirty_after_tool"), bDirtyAfterTool);
			Row->SetBoolField(TEXT("dirty_after_lifecycle"), Package->IsDirty());
			Row->SetStringField(TEXT("save_policy"), Context.SavePolicy);
			Row->SetBoolField(TEXT("save_target"), bSaveTarget);
			Row->SetStringField(TEXT("save_result"), SaveResult);
			if (!Context.SkippedReason.IsEmpty())
			{
				Row->SetStringField(TEXT("skipped_reason"), Context.SkippedReason);
			}
			PackageRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Lifecycle->SetArrayField(TEXT("packages"), PackageRows);
	}

	TSharedPtr<FJsonObject> BuildSkippedLifecycleJson(const FLifecyclePackageContext& Context)
	{
		TSharedPtr<FJsonObject> Lifecycle = MakeShared<FJsonObject>();
		Lifecycle->SetBoolField(TEXT("auto_save"), false);
		Lifecycle->SetStringField(TEXT("skipped_reason"), Context.SkippedReason);
		AttachPackageObservationJson(Lifecycle, Context, nullptr);
		return Lifecycle;
	}

	TSharedPtr<FJsonObject> BuildLifecycleJson(
		const OsvayderUE::SavePipeline::FLifecycleOutcome& Outcome,
		const FLifecyclePackageContext* Context = nullptr)
	{
		TSharedPtr<FJsonObject> Lifecycle = MakeShared<FJsonObject>();

		// A15: when the only dirty packages are level packages, the lifecycle field is the skipped variant.
		const bool bOnlyLevelPackages =
			!Outcome.bAttemptedAssetSave
			&& Outcome.Saved.Num() == 0
			&& Outcome.Failed.Num() == 0
			&& Outcome.Deferred.Num() > 0;

		if (bOnlyLevelPackages)
		{
			Lifecycle->SetBoolField(TEXT("auto_save"), false);
			Lifecycle->SetStringField(TEXT("skipped_reason"), TEXT("level_package_deferred"));
			if (Context)
			{
				AttachPackageObservationJson(Lifecycle, *Context, &Outcome);
			}
			return Lifecycle;
		}

		Lifecycle->SetBoolField(TEXT("auto_save"), true);

		TArray<TSharedPtr<FJsonValue>> SavedJson;
		for (const OsvayderUE::SavePipeline::FLifecyclePhaseResult& R : Outcome.Saved)
		{
			SavedJson.Add(MakeShared<FJsonValueString>(R.PackageName));
		}
		Lifecycle->SetArrayField(TEXT("saved"), SavedJson);

		TArray<TSharedPtr<FJsonValue>> FailedJson;
		for (const OsvayderUE::SavePipeline::FLifecyclePhaseResult& R : Outcome.Failed)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset"), R.PackageName);
			Entry->SetStringField(TEXT("phase"), R.Phase);
			Entry->SetStringField(TEXT("error"), R.Error);
			FailedJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Lifecycle->SetArrayField(TEXT("failed"), FailedJson);

		// P2 compile[] field per spec §"Result schema augmentation".
		// Success entries live in Outcome.Compiled (phase="compile", bSuccess=true).
		// Failure entries are surfaced through Outcome.Failed with phase="compile"
		// (see also spec D1: compile-broken Blueprints are NOT saved to disk).
		if (Outcome.Compiled.Num() > 0 || Outcome.Failed.ContainsByPredicate(
			[](const OsvayderUE::SavePipeline::FLifecyclePhaseResult& R)
			{
				return R.Phase.Equals(TEXT("compile"), ESearchCase::IgnoreCase);
			}))
		{
			TArray<TSharedPtr<FJsonValue>> CompileJson;
			for (const OsvayderUE::SavePipeline::FLifecyclePhaseResult& R : Outcome.Compiled)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset"), R.PackageName);
				Entry->SetStringField(TEXT("result"), TEXT("ok"));
				CompileJson.Add(MakeShared<FJsonValueObject>(Entry));
			}
			for (const OsvayderUE::SavePipeline::FLifecyclePhaseResult& R : Outcome.Failed)
			{
				if (!R.Phase.Equals(TEXT("compile"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset"), R.PackageName);
				Entry->SetStringField(TEXT("result"), TEXT("failed"));
				Entry->SetStringField(TEXT("error"), R.Error);
				CompileJson.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Lifecycle->SetArrayField(TEXT("compile"), CompileJson);
		}

		if (Outcome.Deferred.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> DeferredJson;
			for (const OsvayderUE::SavePipeline::FLifecyclePhaseResult& R : Outcome.Deferred)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset"), R.PackageName);
				Entry->SetStringField(TEXT("skipped_reason"), R.SkippedReason);
				DeferredJson.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Lifecycle->SetArrayField(TEXT("deferred"), DeferredJson);
		}

		// P3 source_control object per spec §"Result schema augmentation".
		// Always emitted when the pipeline ran so agents can distinguish
		// "SC inactive" from "SC absent from report".
		{
			TSharedPtr<FJsonObject> SourceControl = MakeShared<FJsonObject>();
			SourceControl->SetBoolField(TEXT("active"), Outcome.bSourceControlActive);

			TArray<TSharedPtr<FJsonValue>> CheckedOutJson;
			for (const FString& File : Outcome.SourceControlCheckedOut)
			{
				CheckedOutJson.Add(MakeShared<FJsonValueString>(File));
			}
			SourceControl->SetArrayField(TEXT("checked_out"), CheckedOutJson);

			TArray<TSharedPtr<FJsonValue>> WarningsJson;
			for (const OsvayderUE::SavePipeline::FLifecyclePhaseResult& R : Outcome.SourceControlWarnings)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset"), R.PackageName);
				Entry->SetStringField(TEXT("detail"), R.Error);
				WarningsJson.Add(MakeShared<FJsonValueObject>(Entry));
			}
			SourceControl->SetArrayField(TEXT("warnings"), WarningsJson);

			Lifecycle->SetObjectField(TEXT("source_control"), SourceControl);
		}

		// P4 registry object per spec §"Result schema augmentation". Always
		// emitted when the pipeline ran so agents can distinguish "no new
		// assets in this mutation" from "registry field absent from report".
		{
			TSharedPtr<FJsonObject> Registry = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> NewlyRegisteredJson;
			for (const FString& PackageName : Outcome.NewlyRegistered)
			{
				NewlyRegisteredJson.Add(MakeShared<FJsonValueString>(PackageName));
			}
			Registry->SetArrayField(TEXT("newly_registered"), NewlyRegisteredJson);
			Lifecycle->SetObjectField(TEXT("registry"), Registry);
		}

		if (Context)
		{
			AttachPackageObservationJson(Lifecycle, *Context, &Outcome);
		}

		return Lifecycle;
	}

	bool ParseExecutionProfileString(const FString& Value, EAgentExecutionRunProfile& OutProfile)
	{
		if (Value.Equals(TEXT("configured_default_runtime"), ESearchCase::IgnoreCase))
		{
			OutProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
			return true;
		}

		if (Value.Equals(TEXT("read_only_diagnostic"), ESearchCase::IgnoreCase))
		{
			OutProfile = EAgentExecutionRunProfile::ReadOnlyDiagnostic;
			return true;
		}

		if (Value.Equals(TEXT("bounded_plugin_mutation"), ESearchCase::IgnoreCase))
		{
			OutProfile = EAgentExecutionRunProfile::BoundedPluginMutation;
			return true;
		}

		if (Value.Equals(TEXT("explicit_expert_opt_in"), ESearchCase::IgnoreCase))
		{
			OutProfile = EAgentExecutionRunProfile::ExplicitExpertOptIn;
			return true;
		}

		return false;
	}

	bool TryResolveExecutionProfileFromParams(
		const TSharedRef<FJsonObject>& Params,
		EAgentExecutionRunProfile& OutProfile,
		FString& OutProfileLabel,
		FMCPToolResult& OutError)
	{
		FString ExecutionProfileLabel;
		if (Params->TryGetStringField(TEXT("execution_profile"), ExecutionProfileLabel) && !ExecutionProfileLabel.IsEmpty())
		{
			if (!ParseExecutionProfileString(ExecutionProfileLabel, OutProfile))
			{
				OutError = FMCPToolResult::Error(FString::Printf(
					TEXT("Unknown execution_profile '%s'. Expected configured_default_runtime, read_only_diagnostic, bounded_plugin_mutation, or explicit_expert_opt_in."),
					*ExecutionProfileLabel));
				return false;
			}

			OutProfileLabel = ExecutionProfileLabel;
			return true;
		}

		OutProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
		OutProfileLabel = TEXT("configured_default_runtime");
		return true;
	}

	bool IsReadOnlyEnhancedInputOperation(const TSharedRef<FJsonObject>& Params)
	{
		FString Operation;
		if (!Params->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
		{
			return false;
		}

		return Operation.Equals(TEXT("query_context"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("query_action"), ESearchCase::IgnoreCase);
	}

	bool IsCanonScopedOrdinaryEnhancedInputAllowed(
		const EAgentExecutionRunProfile Profile,
		const FString& ToolName)
	{
		if (Profile != EAgentExecutionRunProfile::ConfiguredDefaultRuntime
			|| !ToolName.Equals(TEXT("enhanced_input"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		const FAgentBackendStatus BackendStatus = FOsvayderCodeSubsystem::Get().GetConfiguredBackendStatus();
		FAgentCanonExecution ActiveExecution;
		if (!FOsvayderUEAgentTraceLog::Get().TryGetActiveCanonExecutionForBackend(BackendStatus.Backend, ActiveExecution))
		{
			return false;
		}

		const bool bRequestedInputFamily = ActiveExecution.RequestedToolFamily.Equals(TEXT("unreal_input"), ESearchCase::IgnoreCase);
		const bool bInputFamilyEnabled = ActiveExecution.EnabledToolFamilyIds.ContainsByPredicate([](const FString& FamilyId)
		{
			return FamilyId.Equals(TEXT("unreal_input"), ESearchCase::IgnoreCase);
		});

		return bRequestedInputFamily && bInputFamilyEnabled;
	}

	enum class ERiskyBacklogKind : uint8
	{
		None,
		MutationBacklog,
		HighRiskExecutionBacklog,
		ExternalUiControlBacklog
	};

	bool IsBroadAuthoringMutationBacklogTool(const FString& ToolName)
	{
		return ToolName.Equals(TEXT("spawn_actor"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("delete_actors"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("move_actor"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("set_property"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("asset"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("character"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("character_data"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("enhanced_input"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("material"), ESearchCase::IgnoreCase);
	}

	bool IsHighRiskExecutionBacklogTool(const FString& ToolName)
	{
		return ToolName.Equals(TEXT("execute_script"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("run_console_command"), ESearchCase::IgnoreCase);
	}

	bool IsExternalUiControlBacklogTool(const FString& ToolName)
	{
		return ToolName.Equals(TEXT("osvayder_mouse_click"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("osvayder_mouse_double_click"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("osvayder_mouse_move"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("osvayder_mouse_drag"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("osvayder_mouse_scroll"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("osvayder_keyboard_type"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("osvayder_keyboard_hotkey"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("osvayder_keyboard_press"), ESearchCase::IgnoreCase)
			|| ToolName.Equals(TEXT("osvayder_focus_window"), ESearchCase::IgnoreCase);
	}

	ERiskyBacklogKind GetRiskyBacklogKind(const FString& ToolName)
	{
		if (IsBroadAuthoringMutationBacklogTool(ToolName))
		{
			return ERiskyBacklogKind::MutationBacklog;
		}

		if (IsHighRiskExecutionBacklogTool(ToolName))
		{
			return ERiskyBacklogKind::HighRiskExecutionBacklog;
		}

		if (IsExternalUiControlBacklogTool(ToolName))
		{
			return ERiskyBacklogKind::ExternalUiControlBacklog;
		}

		return ERiskyBacklogKind::None;
	}

	bool ShouldGateRiskyBacklogSurface(const FString& ToolName)
	{
		return GetRiskyBacklogKind(ToolName) != ERiskyBacklogKind::None;
	}

	bool IsCppReflectedDeclarationGovernedOperation(const FString& Operation)
	{
		return Operation.Equals(TEXT("preview_reflected_property_declaration"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("apply_reflected_property_declaration"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("revert_reflected_property_declaration"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("inspect_reflected_property_declaration_build_failure"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("build_reflected_property_declaration_evidence_bundle"), ESearchCase::IgnoreCase);
	}

	bool TryGetGovernedCppReflectionDeclarationOperation(
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Params,
		FString& OutOperation)
	{
		OutOperation.Reset();
		if (!ToolName.Equals(TEXT("cpp_reflection"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		if (!Params->TryGetStringField(TEXT("operation"), OutOperation) || OutOperation.IsEmpty())
		{
			return false;
		}

		return IsCppReflectedDeclarationGovernedOperation(OutOperation);
	}

	FString BuildPolicyDeniedMessage(const FAgentExecutionPolicyDenyContract& Contract)
	{
		FString Message = FString::Printf(
			TEXT("Policy denied [%s]: %s"),
			*Contract.PolicyRuleId,
			*Contract.DenyReason);
		if (Contract.bExpertOptInRequired)
		{
			Message += TEXT(" Explicit expert opt-in is required to reach this surface.");
		}

		return Message;
	}

	FMCPToolResult MakePolicyDenyToolResult(
		const FString& ToolName,
		const FString& TargetToolName,
		const FAgentExecutionPolicyDenyContract& Contract)
	{
		FMCPToolResult Result;
		Result.bSuccess = false;
		Result.Message = BuildPolicyDeniedMessage(Contract);
		Result.Data = MakeShared<FJsonObject>();
		Result.Data->SetStringField(TEXT("result_type"), TEXT("policy_denied"));
		Result.Data->SetStringField(TEXT("tool_name"), ToolName);
		if (!TargetToolName.IsEmpty())
		{
			Result.Data->SetStringField(TEXT("target_tool_name"), TargetToolName);
		}
		Result.Data->SetObjectField(TEXT("policy_denied_contract"), MakeAgentExecutionPolicyDenyContractJson(Contract));
		return Result;
	}

	void ConfigureRiskyBacklogSurfaceDenyContract(
		const FAgentProviderExecutionControlManifest& Manifest,
		const EAgentExecutionRunProfile Profile,
		const FString& RequestedAction,
		const FString& SurfaceToolName,
		const FString& GoverningFamily,
		const ERiskyBacklogKind SurfaceKind,
		FAgentExecutionPolicyDenyContract& OutContract)
	{
		OutContract = FAgentExecutionPolicyDenyContract();
		OutContract.RequestedLane = AgentExecutionRuntimeLaneToString(
			Profile == EAgentExecutionRunProfile::BoundedPluginMutation
				? EAgentExecutionRuntimeLane::BoundedPluginMutation
				: EAgentExecutionRuntimeLane::WorkspaceWriteProject);
		OutContract.EffectiveLane = AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane);
		OutContract.GoverningFamily = GoverningFamily;
		OutContract.RequestedAction = RequestedAction;
		OutContract.DecisionSource = TEXT("mcp_tool_registry_runtime_gate");
		OutContract.VisibleSurfaces = {
			TEXT("tool_result_or_command_result"),
			TEXT("mcp_http_response"),
			TEXT("execution_receipt")
		};
		OutContract.bExpertOptInRequired = true;
		OutContract.TruthBoundary =
			SurfaceKind == ERiskyBacklogKind::ExternalUiControlBacklog
				? TEXT("This denied result is currently real for the full external UI control backlog bucket that now has an explicit MCP runtime gate on safe and bounded lanes. ")
					TEXT("It does not claim universal hard closure for every remaining unmapped risky surface outside that bucket.")
				: SurfaceKind == ERiskyBacklogKind::MutationBacklog
					? TEXT("This denied result is currently real for the full broad authoring mutation backlog bucket as currently modeled at tool-surface granularity in risky-surface governance. ")
						TEXT("It does not claim finer-grained per-operation reopening for mixed tools or universal hard closure for every remaining unmapped risky surface outside that bucket.")
					: TEXT("This denied result is currently real for the currently governed high-risk execution backlog surfaces that have an explicit MCP runtime gate. ")
						TEXT("It does not claim universal hard closure for every remaining unmapped risky surface outside that governed set.");

		if (Profile == EAgentExecutionRunProfile::BoundedPluginMutation)
		{
			switch (SurfaceKind)
			{
			case ERiskyBacklogKind::MutationBacklog:
				OutContract.PolicyRuleId = TEXT("bounded_plugin_mutation.broad_authoring_mutation_surface_denied");
				OutContract.DenyReason = FString::Printf(
					TEXT("bounded_plugin_mutation denies direct MCP execution for '%s' because the full broad authoring mutation backlog bucket is hard-gated at tool-surface granularity on the bounded lane instead of widening to broad authoring mutation."),
					*SurfaceToolName);
				break;

			case ERiskyBacklogKind::ExternalUiControlBacklog:
				OutContract.PolicyRuleId = TEXT("bounded_plugin_mutation.external_ui_control_surface_denied");
				OutContract.DenyReason = FString::Printf(
					TEXT("bounded_plugin_mutation denies direct MCP execution for '%s' because external UI control surfaces remain outside the accepted governed mutation families and must not silently reopen desktop/UI control on the bounded lane."),
					*SurfaceToolName);
				break;

			case ERiskyBacklogKind::HighRiskExecutionBacklog:
			default:
				OutContract.PolicyRuleId = TEXT("bounded_plugin_mutation.representative_high_risk_execution_surface_denied");
				OutContract.DenyReason = FString::Printf(
					TEXT("bounded_plugin_mutation denies direct MCP execution for '%s' because high-risk execution surfaces remain outside the accepted governed mutation families."),
					*SurfaceToolName);
				break;
			}
			OutContract.Basis = {
				TEXT("requested_lane=bounded_plugin_mutation"),
				FString::Printf(TEXT("effective_lane=%s"), AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)),
				FString::Printf(TEXT("requested_tool=%s"), *SurfaceToolName),
				SurfaceKind == ERiskyBacklogKind::ExternalUiControlBacklog
					? TEXT("registry_runtime_gate=full_external_ui_control_bucket")
					: SurfaceKind == ERiskyBacklogKind::MutationBacklog
						? TEXT("registry_runtime_gate=full_broad_authoring_mutation_bucket")
						: TEXT("registry_runtime_gate=representative_surface_only"),
				SurfaceKind == ERiskyBacklogKind::MutationBacklog
					? TEXT("tool_surface_granularity=true")
					: TEXT("tool_surface_granularity=false"),
				TEXT("silent_fallback_occurred=false")
			};
			return;
		}

		switch (SurfaceKind)
		{
		case ERiskyBacklogKind::MutationBacklog:
			OutContract.PolicyRuleId = TEXT("workspace_write_project.broad_authoring_mutation_surface_denied");
			OutContract.DenyReason = FString::Printf(
				TEXT("workspace_write_project denies direct MCP execution for '%s' because the full broad authoring mutation backlog bucket is hard-gated at tool-surface granularity on the ordinary workspace-write default lane."),
				*SurfaceToolName);
			break;

		case ERiskyBacklogKind::ExternalUiControlBacklog:
			OutContract.PolicyRuleId = TEXT("workspace_write_project.external_ui_control_surface_denied");
			OutContract.DenyReason = FString::Printf(
				TEXT("workspace_write_project denies direct MCP execution for '%s' and does not silently reopen external UI control surfaces on the ordinary workspace-write default lane."),
				*SurfaceToolName);
			break;

		case ERiskyBacklogKind::HighRiskExecutionBacklog:
		default:
			OutContract.PolicyRuleId = TEXT("workspace_write_project.representative_high_risk_execution_surface_denied");
			OutContract.DenyReason = FString::Printf(
				TEXT("workspace_write_project denies direct MCP execution for '%s' and does not silently reopen high-risk execution surfaces on the ordinary workspace-write default lane."),
				*SurfaceToolName);
			break;
		}
		OutContract.Basis = {
			TEXT("requested_lane=workspace_write_project"),
			FString::Printf(TEXT("effective_lane=%s"), AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)),
			FString::Printf(TEXT("requested_tool=%s"), *SurfaceToolName),
			SurfaceKind == ERiskyBacklogKind::ExternalUiControlBacklog
				? TEXT("registry_runtime_gate=full_external_ui_control_bucket")
				: SurfaceKind == ERiskyBacklogKind::MutationBacklog
					? TEXT("registry_runtime_gate=full_broad_authoring_mutation_bucket")
					: TEXT("registry_runtime_gate=representative_surface_only"),
			SurfaceKind == ERiskyBacklogKind::MutationBacklog
				? TEXT("tool_surface_granularity=true")
				: TEXT("tool_surface_granularity=false"),
			TEXT("silent_fallback_occurred=false")
		};
	}

	void ConfigureCppReflectedDeclarationLaneDenyContract(
		const FAgentProviderExecutionControlManifest& Manifest,
		const EAgentExecutionRunProfile Profile,
		const FString& RequestedAction,
		const FString& Operation,
		FAgentExecutionPolicyDenyContract& OutContract)
	{
		OutContract = FAgentExecutionPolicyDenyContract();
		OutContract.RequestedLane = AgentExecutionRuntimeLaneToString(
			Profile == EAgentExecutionRunProfile::BoundedPluginMutation
				? EAgentExecutionRuntimeLane::BoundedPluginMutation
				: EAgentExecutionRuntimeLane::WorkspaceWriteProject);
		OutContract.EffectiveLane = AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane);
		OutContract.GoverningFamily = TEXT("cpp_reflected_contracts");
		OutContract.RequestedAction = RequestedAction;
		OutContract.DecisionSource = TEXT("mcp_tool_runtime_gate");
		OutContract.VisibleSurfaces = { TEXT("tool_result_or_command_result"), TEXT("agent_trace"), TEXT("backend_run_receipt") };
		OutContract.TruthBoundary =
			TEXT("This denied result is real for the hardened direct MCP runtime gate around the bounded reflected declaration lane. ")
			TEXT("It keeps the workspace-write default and read-only helper execution from silently reaching the accepted preview/apply/revert/failed-build/evidence cycle, while bounded_plugin_mutation remains the governed runtime lane.");
		OutContract.DenyReason =
			TEXT("workspace_write_project does not permit the bounded reflected declaration authoring/evidence lane on the ordinary workspace-write default lane. ")
			TEXT("Use bounded_plugin_mutation for the governed preview/apply/rebuild/readback/revert workflow.");
		OutContract.PolicyRuleId = TEXT("workspace_write_project.cpp_reflected_declaration_lane_denied");
		OutContract.bSaferAlternativeExists = true;
		OutContract.SaferAlternativeLane = TEXT("bounded_plugin_mutation");
		OutContract.Basis = {
			FString::Printf(TEXT("requested_operation=%s"), *Operation),
			TEXT("requested_lane=workspace_write_project"),
			FString::Printf(TEXT("effective_lane=%s"), AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane)),
			TEXT("governing_family=cpp_reflected_contracts"),
			TEXT("registry_runtime_gate=cpp_reflected_declaration_lane"),
			TEXT("tool_surface_granularity=false"),
			TEXT("safer_alternative_lane=bounded_plugin_mutation"),
			TEXT("silent_fallback_occurred=false")
		};
	}

	void ConfigureExpertUnarmedDenyContract(
		const FAgentProviderExecutionControlManifest& Manifest,
		const FString& RequestedAction,
		const FString& SurfaceToolName,
		const bool bHttpAuthValid,
		const bool bExpertSessionArmed,
		FAgentExecutionPolicyDenyContract& OutContract)
	{
		OutContract = FAgentExecutionPolicyDenyContract();
		OutContract.RequestedLane = TEXT("explicit_expert_opt_in");
		OutContract.EffectiveLane = AgentExecutionRuntimeLaneToString(Manifest.CurrentEffectiveRuntimeLane);
		OutContract.GoverningFamily = TEXT("expert_high_risk_auth_boundary");
		OutContract.RequestedAction = RequestedAction;
		OutContract.DecisionSource = TEXT("mcp_tool_registry_expert_gate");
		OutContract.VisibleSurfaces = {
			TEXT("tool_result_or_command_result"),
			TEXT("mcp_http_response"),
			TEXT("execution_receipt"),
			TEXT("mcp_denied_audit")
		};
		OutContract.PolicyRuleId = TEXT("mcp_http.explicit_expert_unarmed_denied");
		OutContract.DenyReason = FString::Printf(
			TEXT("explicit_expert_opt_in is not sufficient to execute high-risk MCP tool '%s'; token-authenticated requests also require a plugin/user expert session armed state. Packet 690 v1 conservatively denies because no UI/session arming state is active."),
			*SurfaceToolName);
		OutContract.bExpertOptInRequired = true;
		OutContract.TruthBoundary =
			TEXT("This denied result is a conservative v1 security boundary for explicit expert/high-risk MCP surfaces. ")
			TEXT("It proves JSON explicit_expert_opt_in alone cannot authorize high-risk tool execution; a later packet may add explicit UI/session arming to reopen this lane safely.");
		OutContract.Basis = {
			TEXT("requested_profile=explicit_expert_opt_in"),
			FString::Printf(TEXT("requested_tool=%s"), *SurfaceToolName),
			FString::Printf(TEXT("http_token_authenticated=%s"), bHttpAuthValid ? TEXT("true") : TEXT("false")),
			FString::Printf(TEXT("expert_session_armed=%s"), bExpertSessionArmed ? TEXT("true") : TEXT("false")),
			TEXT("json_opt_in_alone_authorizes=false"),
			TEXT("conservative_deny_by_default=true")
		};
	}
}

FMCPToolRegistry::FMCPToolRegistry()
{
	RegisterBuiltinTools();
}

FMCPToolRegistry::~FMCPToolRegistry()
{
	StopTaskQueue();
	Tools.Empty();
}

void FMCPToolRegistry::StartTaskQueue()
{
	if (TaskQueue.IsValid())
	{
		TaskQueue->Start();
	}
}

void FMCPToolRegistry::StopTaskQueue()
{
	if (TaskQueue.IsValid())
	{
		TaskQueue->Shutdown();
	}
}

void FMCPToolRegistry::RegisterBuiltinTools()
{
	UE_LOG(LogOsvayderUE, Log, TEXT("Registering MCP tools..."));

	// Register all built-in tools
	RegisterTool(MakeShared<FMCPTool_SpawnActor>());
	RegisterTool(MakeShared<FMCPTool_GetLevelActors>());
	RegisterTool(MakeShared<FMCPTool_SetProperty>());
	RegisterTool(MakeShared<FMCPTool_RunConsoleCommand>());
	RegisterTool(MakeShared<FMCPTool_DeleteActors>());
	RegisterTool(MakeShared<FMCPTool_MoveActor>());
	RegisterTool(MakeShared<FMCPTool_GetOutputLog>());

	// Script execution tools
	RegisterTool(MakeShared<FMCPTool_ExecuteScript>());
	RegisterTool(MakeShared<FMCPTool_CleanupScripts>());
	RegisterTool(MakeShared<FMCPTool_GetScriptHistory>());

	// Viewport capture
	RegisterTool(MakeShared<FMCPTool_CaptureViewport>());

	// Blueprint tools
	RegisterTool(MakeShared<FMCPTool_BlueprintQuery>());
	RegisterTool(MakeShared<FMCPTool_BlueprintModify>());
	RegisterTool(MakeShared<FMCPTool_AnimBlueprintModify>());
	RegisterTool(MakeShared<FMCPTool_LocalAnimationPackIntake>());
	RegisterTool(MakeShared<FMCPTool_AnimationRetargetFixup>());

	// Asset tools
	RegisterTool(MakeShared<FMCPTool_AssetSearch>());
	RegisterTool(MakeShared<FMCPTool_AssetDependencies>());
	RegisterTool(MakeShared<FMCPTool_AssetReferencers>());

	// Enhanced Input tools
	RegisterTool(MakeShared<FMCPTool_EnhancedInput>());

	// Character tools
	RegisterTool(MakeShared<FMCPTool_Character>());
	RegisterTool(MakeShared<FMCPTool_CharacterData>());

	// Material and Asset tools
	RegisterTool(MakeShared<FMCPTool_Material>());
	RegisterTool(MakeShared<FMCPTool_Asset>());

	// Level management tools
	RegisterTool(MakeShared<FMCPTool_OpenLevel>());
	RegisterTool(MakeShared<FMCPTool_SandboxMapPlacement>());

	// GAS and Niagara tools
	RegisterTool(MakeShared<FMCPTool_GAS>());
	RegisterTool(MakeShared<FMCPTool_Niagara>());

	// Multiplayer tools
	RegisterTool(MakeShared<FMCPTool_Multiplayer>());

	// AI tools
	RegisterTool(MakeShared<FMCPTool_AI>());

	// Sequencer tools
	RegisterTool(MakeShared<FMCPTool_Sequencer>());

	// Plugin settings and memory introspection
	RegisterTool(MakeShared<FMCPTool_PluginSettings>());
	RegisterTool(MakeShared<FMCPTool_ProjectMemoryStatus>());
	RegisterTool(MakeShared<FMCPTool_ExecutionLogStatus>());
	RegisterTool(MakeShared<FMCPTool_AgentTraceStatus>());
	RegisterTool(MakeShared<FMCPTool_ReportExport>());
	RegisterTool(MakeShared<FMCPTool_ReportArtifactStatus>());
	RegisterTool(MakeShared<FMCPTool_CppReflection>());
	RegisterTool(MakeShared<FMCPTool_DependencyHealth>());
	RegisterTool(MakeShared<FMCPTool_MetadataTruth>());
	RegisterTool(MakeShared<FMCPTool_MapRuntimeProof>());
	RegisterTool(MakeShared<FMCPTool_MechanicPreflight>());
	RegisterTool(MakeShared<FMCPTool_MutationGroup>());
	RegisterTool(MakeShared<FMCPTool_OssSessionProof>());
	// 619 P1: registered immediately BEFORE RestartSurvival so the registry
	// enumeration order reinforces the "LC first, restart as escalation"
	// contract per spec 619 §D1 + Fix #2 description framing.
	RegisterTool(MakeShared<FMCPTool_LiveCodingCompile>());
	RegisterTool(MakeShared<FMCPTool_RestartSurvival>());

	// Create and register async task queue tools
	// Task queue takes a raw pointer since the registry always outlives it
	TaskQueue = MakeShared<FMCPTaskQueue>(this);

	// Wire up execute_script to use the task queue for async execution
	// This allows script execution to handle permission dialogs without timing out
	if (TSharedPtr<IMCPTool>* ExecuteScriptToolPtr = Tools.Find(TEXT("execute_script")))
	{
		if (FMCPTool_ExecuteScript* ExecuteScriptTool = static_cast<FMCPTool_ExecuteScript*>(ExecuteScriptToolPtr->Get()))
		{
			ExecuteScriptTool->SetTaskQueue(TaskQueue);
			UE_LOG(LogOsvayderUE, Log, TEXT("  Wired up execute_script to task queue for async execution"));
		}
	}

	RegisterTool(MakeShared<FMCPTool_TaskSubmit>(TaskQueue));
	RegisterTool(MakeShared<FMCPTool_TaskStatus>(TaskQueue));
	RegisterTool(MakeShared<FMCPTool_TaskResult>(TaskQueue));
	RegisterTool(MakeShared<FMCPTool_TaskList>(TaskQueue));
	RegisterTool(MakeShared<FMCPTool_TaskCancel>(TaskQueue));
#if WITH_DEV_AUTOMATION_TESTS
	RegisterTool(MakeShared<FMCPTool_AsyncGuardrailFixture>());
#endif

	UE_LOG(LogOsvayderUE, Log, TEXT("Registered %d built-in MCP tools"), Tools.Num());

	// Auto-start OsvayderEye HTTP sidecar and discover tools
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	int32 EyeCount = -1;
	if (Settings && Settings->bEnableOsvayderEye)
	{
		// Try to start sidecar if not already running
		FString ServerPath = Settings->OsvayderEyeServerPath.FilePath;
		FString PythonPath = Settings->OsvayderEyePythonPath.FilePath;

		if (!ServerPath.IsEmpty() && !PythonPath.IsEmpty()
			&& IFileManager::Get().FileExists(*ServerPath)
			&& IFileManager::Get().FileExists(*PythonPath))
		{
			// Check if sidecar is already running by trying to connect
			EyeCount = EyeToolDiscovery::DiscoverAndRegister(*this, Settings->OsvayderEyeUrl);

			if (EyeCount < 0)
			{
				// Sidecar not running — launch it
				FString HttpServerPath = FPaths::GetPath(ServerPath) / TEXT("http_server.py");
				if (IFileManager::Get().FileExists(*HttpServerPath))
				{
					FString Args = FString::Printf(TEXT("\"%s\""), *HttpServerPath);
					FProcHandle Handle = FPlatformProcess::CreateProc(
						*PythonPath, *Args, false, true, true, nullptr, 0, nullptr, nullptr);

					if (Handle.IsValid())
					{
						UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderEye sidecar auto-started: %s %s"), *PythonPath, *Args);
						// Wait a moment for sidecar to start
						FPlatformProcess::Sleep(2.0f);
						// Try discovery again
						EyeCount = EyeToolDiscovery::DiscoverAndRegister(*this, Settings->OsvayderEyeUrl);
					}
					else
					{
						UE_LOG(LogOsvayderUE, Warning, TEXT("Failed to auto-start OsvayderEye sidecar"));
					}
				}
				else
				{
					UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderEye http_server.py not found at: %s"), *HttpServerPath);
		}
	}
		else
		{
			UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderEye paths not configured or files missing"));
		}
	}
	}
	else
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("OsvayderEye disabled in settings"));
	}
	if (EyeCount > 0)
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("Total MCP tools: %d (built-in) + %d (eye) = %d"), Tools.Num() - EyeCount, EyeCount, Tools.Num());
	}
}

void FMCPToolRegistry::RegisterTool(TSharedPtr<IMCPTool> Tool)
{
	if (!Tool.IsValid())
	{
		return;
	}

	FMCPToolInfo Info = Tool->GetInfo();
	if (Info.Name.IsEmpty())
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Cannot register tool with empty name"));
		return;
	}

	if (Tools.Contains(Info.Name))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Tool '%s' is already registered, replacing"), *Info.Name);
	}

	Tools.Add(Info.Name, Tool);
	InvalidateToolCache();
	UE_LOG(LogOsvayderUE, Log, TEXT("  Registered tool: %s"), *Info.Name);
}

void FMCPToolRegistry::UnregisterTool(const FString& ToolName)
{
	if (Tools.Remove(ToolName) > 0)
	{
		InvalidateToolCache();
		UE_LOG(LogOsvayderUE, Log, TEXT("Unregistered tool: %s"), *ToolName);
	}
}

void FMCPToolRegistry::InvalidateToolCache()
{
	bCacheValid = false;
	CachedToolInfo.Empty();
}

TArray<FMCPToolInfo> FMCPToolRegistry::GetAllTools() const
{
	// Return cached result if valid
	if (bCacheValid)
	{
		return CachedToolInfo;
	}

	// Rebuild cache
	CachedToolInfo.Empty(Tools.Num());
	for (const auto& Pair : Tools)
	{
		if (Pair.Value.IsValid())
		{
			CachedToolInfo.Add(Pair.Value->GetInfo());
		}
	}
	bCacheValid = true;

	return CachedToolInfo;
}

bool FMCPToolRegistry::TryBuildGovernanceDenyResult(
	const FString& ToolName,
	const TSharedRef<FJsonObject>& Params,
	FMCPToolResult& OutResult) const
{
	const bool bTaskSubmit = ToolName.Equals(TEXT("task_submit"), ESearchCase::IgnoreCase);
	FString TargetToolName = ToolName;
	TSharedRef<FJsonObject> EffectiveParams = Params;
	TSharedPtr<FJsonObject> EffectiveParamsObject;

	if (bTaskSubmit)
	{
		if (!Params->TryGetStringField(TEXT("tool_name"), TargetToolName) || TargetToolName.IsEmpty())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* NestedParams = nullptr;
		if (Params->TryGetObjectField(TEXT("params"), NestedParams) && NestedParams && (*NestedParams).IsValid())
		{
			EffectiveParamsObject = MakeShared<FJsonObject>();
			EffectiveParamsObject->Values = (*NestedParams)->Values;
		}
		else
		{
			EffectiveParamsObject = MakeShared<FJsonObject>();
		}

		FString TopLevelProfile;
		if (Params->TryGetStringField(TEXT("execution_profile"), TopLevelProfile)
			&& !TopLevelProfile.IsEmpty()
			&& EffectiveParamsObject.IsValid()
			&& !EffectiveParamsObject->HasField(TEXT("execution_profile")))
		{
			EffectiveParamsObject->SetStringField(TEXT("execution_profile"), TopLevelProfile);
		}

		const TCHAR* InternalBoolFields[] = { TEXT("_mcp_http_auth_valid"), TEXT("_mcp_expert_session_armed") };
		for (const TCHAR* InternalBoolField : InternalBoolFields)
		{
			bool bInternalValue = false;
			if (Params->TryGetBoolField(InternalBoolField, bInternalValue)
				&& EffectiveParamsObject.IsValid()
				&& !EffectiveParamsObject->HasField(InternalBoolField))
			{
				EffectiveParamsObject->SetBoolField(InternalBoolField, bInternalValue);
			}
		}

		EffectiveParams = EffectiveParamsObject.ToSharedRef();
	}

	FString GovernedCppReflectionOperation;
	const bool bGovernedCppReflectionDeclarationOperation = TryGetGovernedCppReflectionDeclarationOperation(
		TargetToolName,
		EffectiveParams,
		GovernedCppReflectionOperation);
	const bool bReadOnlyEnhancedInput = TargetToolName.Equals(TEXT("enhanced_input"), ESearchCase::IgnoreCase)
		&& IsReadOnlyEnhancedInputOperation(EffectiveParams);
	const bool bRiskyBacklogSurface = !bReadOnlyEnhancedInput && ShouldGateRiskyBacklogSurface(TargetToolName);
	if (!bRiskyBacklogSurface && !bGovernedCppReflectionDeclarationOperation)
	{
		return false;
	}

	EAgentExecutionRunProfile Profile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
	FString ResolvedProfileLabel;
	FMCPToolResult ProfileError;
	if (!TryResolveExecutionProfileFromParams(EffectiveParams, Profile, ResolvedProfileLabel, ProfileError))
	{
		OutResult = ProfileError;
		return true;
	}

	if (bRiskyBacklogSurface && Profile != EAgentExecutionRunProfile::ExplicitExpertOptIn
		&& IsCanonScopedOrdinaryEnhancedInputAllowed(Profile, TargetToolName))
	{
		return false;
	}

	const FAgentProviderExecutionControlManifest Manifest =
		FOsvayderCodeSubsystem::Get().GetConfiguredBackendExecutionControlManifestForProfile(Profile);

	if (Profile == EAgentExecutionRunProfile::ExplicitExpertOptIn)
	{
		bool bHttpAuthValid = false;
		EffectiveParams->TryGetBoolField(TEXT("_mcp_http_auth_valid"), bHttpAuthValid);
		bool bExpertSessionArmed = false;
		EffectiveParams->TryGetBoolField(TEXT("_mcp_expert_session_armed"), bExpertSessionArmed);

		if (!(bHttpAuthValid && bExpertSessionArmed))
		{
			FAgentExecutionPolicyDenyContract Contract;
			ConfigureExpertUnarmedDenyContract(
				Manifest,
				bTaskSubmit
					? FString::Printf(TEXT("task_submit:%s"), *TargetToolName)
					: FString::Printf(TEXT("mcp_tool:%s"), *TargetToolName),
				TargetToolName,
				bHttpAuthValid,
				bExpertSessionArmed,
				Contract);
			if (bTaskSubmit)
			{
				Contract.DecisionSource = TEXT("mcp_task_submit_expert_gate");
				Contract.Basis.Add(FString::Printf(TEXT("submitted_via_tool=%s"), *ToolName));
				Contract.Basis.Add(TEXT("task_wrapper_bypass_prevented=true"));
			}

			OutResult = MakePolicyDenyToolResult(ToolName, TargetToolName, Contract);
			OutResult.Data->SetStringField(TEXT("error_category"), TEXT("expert_unarmed"));
			OutResult.Data->SetBoolField(TEXT("mcp_http_auth_valid"), bHttpAuthValid);
			OutResult.Data->SetBoolField(TEXT("expert_session_armed"), bExpertSessionArmed);
			return true;
		}

		return false;
	}

	if (bGovernedCppReflectionDeclarationOperation)
	{
		if (Profile == EAgentExecutionRunProfile::BoundedPluginMutation)
		{
			return false;
		}

		FAgentExecutionPolicyDenyContract Contract;
		ConfigureCppReflectedDeclarationLaneDenyContract(
			Manifest,
			Profile,
			bTaskSubmit
				? FString::Printf(TEXT("task_submit:%s.%s"), *TargetToolName, *GovernedCppReflectionOperation)
				: FString::Printf(TEXT("mcp_tool:%s.%s"), *TargetToolName, *GovernedCppReflectionOperation),
			GovernedCppReflectionOperation,
			Contract);

		if (bTaskSubmit)
		{
			Contract.DecisionSource = TEXT("mcp_task_submit_runtime_gate");
			Contract.Basis.Add(FString::Printf(TEXT("submitted_via_tool=%s"), *ToolName));
			Contract.Basis.Add(TEXT("task_wrapper_bypass_prevented=true"));
		}

		OutResult = MakePolicyDenyToolResult(ToolName, TargetToolName, Contract);
		return true;
	}

	const ERiskyBacklogKind SurfaceKind = GetRiskyBacklogKind(TargetToolName);
	FAgentExecutionPolicyDenyContract Contract;
	ConfigureRiskyBacklogSurfaceDenyContract(
		Manifest,
		Profile,
		bTaskSubmit
			? FString::Printf(TEXT("task_submit:%s"), *TargetToolName)
			: FString::Printf(TEXT("mcp_tool:%s"), *TargetToolName),
		TargetToolName,
		SurfaceKind == ERiskyBacklogKind::MutationBacklog
			? TEXT("broad_authoring_mutation_backlog")
			: SurfaceKind == ERiskyBacklogKind::ExternalUiControlBacklog
				? TEXT("external_ui_control_backlog")
				: TEXT("high_risk_execution_backlog"),
		SurfaceKind,
		Contract);

	if (bTaskSubmit)
	{
		Contract.DecisionSource = TEXT("mcp_task_submit_runtime_gate");
		Contract.Basis.Add(FString::Printf(TEXT("submitted_via_tool=%s"), *ToolName));
		Contract.Basis.Add(TEXT("task_wrapper_bypass_prevented=true"));
	}

	OutResult = MakePolicyDenyToolResult(ToolName, TargetToolName, Contract);
	return true;
}

FMCPToolResult FMCPToolRegistry::ExecuteTool(const FString& ToolName, const TSharedRef<FJsonObject>& Params)
{
	// Start trace timing
	double TraceStartTime = FPlatformTime::Seconds();
	FString TraceId = FExecutionReceipt::GenerateTraceId();
	bool bWaitedOnGameThread = false;

	// Execute on game thread to ensure safe access to engine objects
	FMCPToolResult Result;
	TSharedPtr<IMCPTool>* FoundTool = nullptr;

	if (TryBuildGovernanceDenyResult(ToolName, Params, Result))
	{
		UE_LOG(LogOsvayderUE, Log, TEXT("Tool '%s' blocked by execution policy: %s"), *ToolName, *Result.Message);
	}
	else
	{
		FoundTool = Tools.Find(ToolName);
		if (!FoundTool || !FoundTool->IsValid())
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Tool '%s' not found"), *ToolName));
		}

		UE_LOG(LogOsvayderUE, Log, TEXT("Executing MCP tool: %s"), *ToolName);

		// Mutation-lifecycle wrapper: runs entirely on the game thread so UPackage state
		// is safe to snapshot, diff, and save. Reentrancy, ReadOnly bypass, and the
		// per-call auto_save override are all resolved inside the game-thread lambda.
		TSharedPtr<IMCPTool> ToolShared = *FoundTool;
		auto WorkOnGameThread = [ToolShared, Params](FMCPToolResult& Out)
		{
			check(IsInGameThread());

			const FMCPToolAnnotations Annotations = ToolShared->GetInfo().Annotations;

			// Nested call from inside another lifecycle — the outer pass will diff+save.
			if (GMCPLifecycleDepth > 0)
			{
				Out = ToolShared->Execute(Params);
				return;
			}

			// Read-only tool — no lifecycle attached.
			if (Annotations.IsReadOnly())
			{
				Out = ToolShared->Execute(Params);
				return;
			}

			// Per-call opt-out: run the tool, attach the skipped-reason lifecycle marker on success.
			if (!ResolveAutoSaveForMutation(Params))
			{
				FLifecyclePackageContext LifecycleContext;
				LifecycleContext.SavePolicy = TEXT("user_opt_out");
				LifecycleContext.SkippedReason = TEXT("user_opt_out");
				LifecycleContext.DirtyBeforeTool = SnapshotDirtyPackages();
				FDelegateHandle DirtyTrackerHandle = UPackage::PackageMarkedDirtyEvent.AddLambda(
					[&LifecycleContext](UPackage* ModifiedPackage, bool /*bWasDirty*/)
					{
						if (ModifiedPackage)
						{
							LifecycleContext.TouchedByTool.Add(ModifiedPackage);
						}
					});
				{
					ON_SCOPE_EXIT { UPackage::PackageMarkedDirtyEvent.Remove(DirtyTrackerHandle); };
					Out = ToolShared->Execute(Params);
				}
				LifecycleContext.DirtyAfterTool = SnapshotDirtyPackages();
				if (Out.bSuccess)
				{
					if (!Out.Data.IsValid())
					{
						Out.Data = MakeShared<FJsonObject>();
					}
					Out.Data->SetObjectField(TEXT("lifecycle"), BuildSkippedLifecycleJson(LifecycleContext));
				}
				return;
			}

			// Full lifecycle path: snapshot dirty packages, run tool, diff, run pipeline, attach result.
			FLifecyclePackageContext LifecycleContext;
			LifecycleContext.SavePolicy = TEXT("auto_save");
			LifecycleContext.DirtyBeforeTool = SnapshotDirtyPackages();
			FDelegateHandle DirtyTrackerHandle = UPackage::PackageMarkedDirtyEvent.AddLambda(
				[&LifecycleContext](UPackage* ModifiedPackage, bool /*bWasDirty*/)
				{
					if (ModifiedPackage)
					{
						LifecycleContext.TouchedByTool.Add(ModifiedPackage);
					}
				});

			{
				ON_SCOPE_EXIT { UPackage::PackageMarkedDirtyEvent.Remove(DirtyTrackerHandle); };

				++GMCPLifecycleDepth;
				ON_SCOPE_EXIT { --GMCPLifecycleDepth; };

				Out = ToolShared->Execute(Params);
			}

			if (!Out.bSuccess)
			{
				// Do not persist a failed mutation.
				return;
			}

			LifecycleContext.DirtyAfterTool = SnapshotDirtyPackages();
			TArray<UPackage*> PackagesToSave;
			PackagesToSave.Reserve(LifecycleContext.DirtyAfterTool.Num());
			for (UPackage* Package : LifecycleContext.DirtyAfterTool)
			{
				if (!LifecycleContext.DirtyBeforeTool.Contains(Package)
					|| LifecycleContext.TouchedByTool.Contains(Package))
				{
					PackagesToSave.Add(Package);
				}
			}

			if (PackagesToSave.Num() == 0)
			{
				// Tool succeeded but dirtied no new packages — no lifecycle field.
				return;
			}

			OsvayderUE::SavePipeline::FSaveSpec SaveSpec;
			LifecycleContext.SaveTargets = PackagesToSave;
			SaveSpec.Packages = MoveTemp(PackagesToSave);
			SaveSpec.bIsExplicitToolCall = false;
			const OsvayderUE::SavePipeline::FLifecycleOutcome Outcome =
				OsvayderUE::SavePipeline::Run(SaveSpec);

			if (!Out.Data.IsValid())
			{
				Out.Data = MakeShared<FJsonObject>();
			}
			Out.Data->SetObjectField(TEXT("lifecycle"), BuildLifecycleJson(Outcome, &LifecycleContext));
		};

		if (IsInGameThread())
		{
			WorkOnGameThread(Result);
		}
		else if (IsEngineExitRequested())
		{
			// P5 A-P5-2 shutdown sentinel: if the engine is already requesting
			// exit, dispatching to the game thread can race with subsystem
			// teardown (streaming manager, UObject system, editor). Short-circuit
			// with a structured `skipped_reason=engine_exit_requested` lifecycle
			// rather than waiting for a 30 s timeout on a thread that is never
			// going to tick again.
			UE_LOG(LogOsvayderUE, Warning,
				TEXT("Tool '%s' dispatch skipped: IsEngineExitRequested()==true before game-thread dispatch"),
				*ToolName);
			TSharedPtr<FJsonObject> Lifecycle = MakeShared<FJsonObject>();
			Lifecycle->SetBoolField(TEXT("auto_save"), false);
			Lifecycle->SetStringField(TEXT("skipped_reason"), TEXT("engine_exit_requested"));
			Result = FMCPToolResult::Error(TEXT("Engine is requesting exit; tool dispatch skipped."));
			Result.Data = MakeShared<FJsonObject>();
			Result.Data->SetObjectField(TEXT("lifecycle"), Lifecycle);
		}
		else
		{
			bWaitedOnGameThread = true;
			// If called from non-game thread, dispatch to game thread and wait with timeout.
			// Use shared pointers for all state to avoid use-after-free if timeout occurs.
			TSharedPtr<FMCPToolResult> SharedResult = MakeShared<FMCPToolResult>();
			TSharedPtr<FEvent, ESPMode::ThreadSafe> CompletionEvent = MakeShareable(FPlatformProcess::GetSynchEventFromPool(),
				[](FEvent* Event) { FPlatformProcess::ReturnSynchEventToPool(Event); });
			TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> bTaskCompleted = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);
			TSharedPtr<FCriticalSection, ESPMode::ThreadSafe> DispatchStateLock = MakeShared<FCriticalSection, ESPMode::ThreadSafe>();
			TSharedPtr<int32, ESPMode::ThreadSafe> DispatchState = MakeShared<int32, ESPMode::ThreadSafe>(0);

			// Use FTSTicker to dispatch to game thread at a safe point between subsystem ticks.
			// AsyncTask(GameThread) can fire during streaming manager iteration, causing
			// re-entrancy into LevelRenderAssetManagersLock (assertion crash).
			const FString ToolNameCopy = ToolName;
			FTSTicker::GetCoreTicker().AddTicker(TEXT("MCPTool_Execute"), 0.0f,
				[Work = MoveTemp(WorkOnGameThread), SharedResult, CompletionEvent, bTaskCompleted, DispatchStateLock, DispatchState, ToolNameCopy](float) -> bool
			{
				bool bCanExecute = false;
				{
					FScopeLock Lock(DispatchStateLock.Get());
					if (*DispatchState == 0)
					{
						*DispatchState = 1;
						bCanExecute = true;
					}
				}

				if (!bCanExecute)
				{
					TSharedPtr<FJsonObject> Lifecycle = MakeShared<FJsonObject>();
					Lifecycle->SetBoolField(TEXT("auto_save"), false);
					Lifecycle->SetStringField(TEXT("skipped_reason"), TEXT("game_thread_timeout_before_dispatch"));
					FMCPToolResult SkippedResult = FMCPToolResult::Error(FString::Printf(
						TEXT("Tool '%s' timed out before game-thread dispatch; execution skipped to prevent late mutation"),
						*ToolNameCopy));
					SkippedResult.Data = MakeShared<FJsonObject>();
					SkippedResult.Data->SetObjectField(TEXT("lifecycle"), Lifecycle);
					*SharedResult = SkippedResult;
					*bTaskCompleted = true;
					CompletionEvent->Trigger();
					return false;
				}

				// P5 A-P5-2 shutdown sentinel (game-thread side): if exit was
				// requested between FTSTicker schedule and run, skip the work.
				// Signal completion anyway so the caller's Wait returns promptly
				// instead of timing out at 30 s during shutdown.
				if (IsEngineExitRequested())
				{
					TSharedPtr<FJsonObject> Lifecycle = MakeShared<FJsonObject>();
					Lifecycle->SetBoolField(TEXT("auto_save"), false);
					Lifecycle->SetStringField(TEXT("skipped_reason"), TEXT("engine_exit_requested"));
					FMCPToolResult ExitResult = FMCPToolResult::Error(FString::Printf(
						TEXT("Tool '%s' skipped on game thread: IsEngineExitRequested()==true at dispatch time"),
						*ToolNameCopy));
					ExitResult.Data = MakeShared<FJsonObject>();
					ExitResult.Data->SetObjectField(TEXT("lifecycle"), Lifecycle);
					*SharedResult = ExitResult;
					*bTaskCompleted = true;
					CompletionEvent->Trigger();
					return false;
				}

				Work(*SharedResult);
				*bTaskCompleted = true;
				CompletionEvent->Trigger();
				return false; // One-shot, don't reschedule
			});

			// Wait with timeout to prevent indefinite hangs.
			const uint32 TimeoutMs = OsvayderUEConstants::MCPServer::GameThreadTimeoutMs;
			const bool bSignaled = CompletionEvent->Wait(TimeoutMs);

			if (!bSignaled || !(*bTaskCompleted))
			{
				bool bStartedOnGameThread = false;
				{
					FScopeLock Lock(DispatchStateLock.Get());
					if (*DispatchState == 0)
					{
						*DispatchState = 2;
					}
					bStartedOnGameThread = (*DispatchState == 1);
				}

				if (bStartedOnGameThread)
				{
					UE_LOG(LogOsvayderUE, Warning,
						TEXT("Tool '%s' exceeded %d ms after game-thread execution started; waiting for truthful completion instead of returning timeout while mutation may still land"),
						*ToolName, TimeoutMs);
					CompletionEvent->Wait();
					Result = *SharedResult;
				}
				else
				{
				// P5 A-P5-3: surface the timeout as a structured lifecycle
				// failure (`failed[*].phase="game_thread_timeout"`) rather than
				// a raw Error. Downstream consumers (agent receipt parsers,
				// diagnostic_trace.jsonl readers) can then classify the failure
				// the same way they classify compile/save failures instead of
				// special-casing "tool execution timed out" strings.
				UE_LOG(LogOsvayderUE, Error,
					TEXT("Tool '%s' execution timed out after %d ms on the game thread; surfacing as lifecycle.failed[game_thread_timeout]"),
					*ToolName, TimeoutMs);
				TSharedPtr<FJsonObject> Lifecycle = MakeShared<FJsonObject>();
				Lifecycle->SetBoolField(TEXT("auto_save"), false);
				TArray<TSharedPtr<FJsonValue>> FailedJson;
				TSharedPtr<FJsonObject> FailedEntry = MakeShared<FJsonObject>();
				FailedEntry->SetStringField(TEXT("asset"), FString());
				FailedEntry->SetStringField(TEXT("phase"), TEXT("game_thread_timeout"));
				FailedEntry->SetStringField(TEXT("error"), FString::Printf(
					TEXT("Tool '%s' did not complete on the game thread within %d ms"),
					*ToolName, TimeoutMs));
				FailedJson.Add(MakeShared<FJsonValueObject>(FailedEntry));
				Lifecycle->SetArrayField(TEXT("failed"), FailedJson);

				Result = FMCPToolResult::Error(FString::Printf(
					TEXT("Tool execution timed out after %d seconds"), TimeoutMs / 1000));
				Result.Data = MakeShared<FJsonObject>();
				Result.Data->SetObjectField(TEXT("lifecycle"), Lifecycle);
				}
			}
			else
			{
				// Copy result from shared storage.
				Result = *SharedResult;
			}
		}
	}

	UE_LOG(LogOsvayderUE, Log, TEXT("Tool '%s' execution %s: %s"),
		*ToolName,
		Result.bSuccess ? TEXT("succeeded") : TEXT("failed"),
		*Result.Message);

	// Compute trace duration
	double TraceEndTime = FPlatformTime::Seconds();
	double TraceDurationMs = (TraceEndTime - TraceStartTime) * 1000.0;

	// Log execution receipt with diagnostic trace for all tools (not just modifying)
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = ToolName;
		Receipt.bSuccess = Result.bSuccess;
		Receipt.TargetType = ToolName.Contains(TEXT("blueprint")) ? TEXT("blueprint")
			: ToolName.Contains(TEXT("actor")) ? TEXT("actor")
			: ToolName.Contains(TEXT("material")) ? TEXT("material")
			: ToolName.Contains(TEXT("osvayder")) ? TEXT("screen")
			: TEXT("other");
		if (Result.Data.IsValid())
		{
			FString BP;
			if (Result.Data->TryGetStringField(TEXT("blueprint_path"), BP))
			{
				Receipt.Targets.Add(BP);
			}
			// Extract task_id for async tool correlation
			FString ExtractedTaskId;
			if (Result.Data->TryGetStringField(TEXT("task_id"), ExtractedTaskId))
			{
				Receipt.TaskId = ExtractedTaskId;
				Receipt.bUsedTaskQueue = true;
			}
		}
		Receipt.bPartialSuccess = Result.Data.IsValid() && Result.Data->HasField(TEXT("partial_success")) && Result.Data->GetBoolField(TEXT("partial_success"));

		// Trace fields
		Receipt.TraceId = TraceId;
		Receipt.DurationMs = TraceDurationMs;
		Receipt.bWaitedOnGameThread = bWaitedOnGameThread;
		Receipt.Status = Result.bSuccess ? (Receipt.bPartialSuccess ? TEXT("partial_success") : TEXT("success")) : TEXT("failed");
		Receipt.Summary = Result.Message;
		if (!Result.bSuccess)
		{
			Receipt.ErrorOrDenialReason = Result.Message;
		}
		Receipt.AddPhaseTiming(TEXT("tool_execute"), TraceDurationMs);

		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);

		// P5 A-P5-6 A12 telemetry: emit a secondary receipt with a fixed
		// `Tool="mutation_lifecycle"` + `Classification="autonomous_persist"`
		// identity whenever the lifecycle wrapper actually ran (auto_save=true
		// path only; skipped-reason variants — user_opt_out, level_package_deferred,
		// engine_exit_requested — produce no persist and therefore no receipt).
		// Agents filter the diagnostic_trace.jsonl on Tool="mutation_lifecycle"
		// to audit autonomous saves without walking every per-tool entry.
		if (Result.Data.IsValid())
		{
			const TSharedPtr<FJsonObject>* LifecycleObj = nullptr;
			if (Result.Data->TryGetObjectField(TEXT("lifecycle"), LifecycleObj)
				&& LifecycleObj && LifecycleObj->IsValid())
			{
				bool bAutoSave = false;
				(*LifecycleObj)->TryGetBoolField(TEXT("auto_save"), bAutoSave);
				if (bAutoSave)
				{
					auto CountArrayField = [&LifecycleObj](const TCHAR* FieldName) -> int32
					{
						const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
						if ((*LifecycleObj)->TryGetArrayField(FieldName, Arr) && Arr)
						{
							return Arr->Num();
						}
						return 0;
					};

					const int32 SavedCount = CountArrayField(TEXT("saved"));
					const int32 FailedCount = CountArrayField(TEXT("failed"));

					int32 CompileOkCount = 0;
					int32 CompileFailCount = 0;
					{
						const TArray<TSharedPtr<FJsonValue>>* CompileArr = nullptr;
						if ((*LifecycleObj)->TryGetArrayField(TEXT("compile"), CompileArr) && CompileArr)
						{
							for (const TSharedPtr<FJsonValue>& Value : *CompileArr)
							{
								const TSharedPtr<FJsonObject>* Entry = nullptr;
								if (!Value->TryGetObject(Entry) || !Entry || !Entry->IsValid())
								{
									continue;
								}
								FString CompileResult;
								(*Entry)->TryGetStringField(TEXT("result"), CompileResult);
								if (CompileResult.Equals(TEXT("ok")))
								{
									++CompileOkCount;
								}
								else if (CompileResult.Equals(TEXT("failed")))
								{
									++CompileFailCount;
								}
							}
						}
					}

					int32 SCCheckedOutCount = 0;
					int32 SCWarningsCount = 0;
					{
						const TSharedPtr<FJsonObject>* SCObj = nullptr;
						if ((*LifecycleObj)->TryGetObjectField(TEXT("source_control"), SCObj)
							&& SCObj && SCObj->IsValid())
						{
							const TArray<TSharedPtr<FJsonValue>>* SCCheckedOutArr = nullptr;
							if ((*SCObj)->TryGetArrayField(TEXT("checked_out"), SCCheckedOutArr) && SCCheckedOutArr)
							{
								SCCheckedOutCount = SCCheckedOutArr->Num();
							}
							const TArray<TSharedPtr<FJsonValue>>* SCWarningsArr = nullptr;
							if ((*SCObj)->TryGetArrayField(TEXT("warnings"), SCWarningsArr) && SCWarningsArr)
							{
								SCWarningsCount = SCWarningsArr->Num();
							}
						}
					}

					int32 NewlyRegisteredCount = 0;
					{
						const TSharedPtr<FJsonObject>* RegistryObj = nullptr;
						if ((*LifecycleObj)->TryGetObjectField(TEXT("registry"), RegistryObj)
							&& RegistryObj && RegistryObj->IsValid())
						{
							const TArray<TSharedPtr<FJsonValue>>* NewlyArr = nullptr;
							if ((*RegistryObj)->TryGetArrayField(TEXT("newly_registered"), NewlyArr) && NewlyArr)
							{
								NewlyRegisteredCount = NewlyArr->Num();
							}
						}
					}

					FExecutionReceipt LifecycleReceipt;
					LifecycleReceipt.Tool = TEXT("mutation_lifecycle");
					LifecycleReceipt.Classification = TEXT("autonomous_persist");
					LifecycleReceipt.bSuccess = Result.bSuccess && FailedCount == 0;
					LifecycleReceipt.bPartialSuccess = Result.bSuccess && FailedCount > 0 && SavedCount > 0;
					LifecycleReceipt.TraceId = TraceId + TEXT("_lifecycle");
					LifecycleReceipt.DurationMs = TraceDurationMs;
					LifecycleReceipt.bWaitedOnGameThread = bWaitedOnGameThread;
					LifecycleReceipt.Status = LifecycleReceipt.bSuccess
						? TEXT("success")
						: (LifecycleReceipt.bPartialSuccess ? TEXT("partial_success") : TEXT("failed"));
					LifecycleReceipt.Summary = FString::Printf(
						TEXT("source_tool=%s saved=%d failed=%d compile_ok=%d compile_fail=%d sc_checked_out=%d sc_warnings=%d newly_registered=%d"),
						*ToolName,
						SavedCount, FailedCount, CompileOkCount, CompileFailCount,
						SCCheckedOutCount, SCWarningsCount, NewlyRegisteredCount);

					FOsvayderUEExecutionLog::Get().AddReceipt(LifecycleReceipt);
				}
			}
		}
	}

	return Result;
}

bool FMCPToolRegistry::HasTool(const FString& ToolName) const
{
	return Tools.Contains(ToolName);
}
