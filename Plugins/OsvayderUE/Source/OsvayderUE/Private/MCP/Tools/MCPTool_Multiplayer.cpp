// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Multiplayer.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEModule.h"
#include "OsvayderUEReportArtifacts.h"
#include "MCP/MCPParamValidator.h"
#include "BlueprintLoader.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Net/Core/PushModel/PushModel.h"
#include "UObject/CoreNetTypes.h"
#include "Editor.h"
#include "EngineUtils.h" // TActorIterator
#include "OnlineSubsystem.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/ActorComponent.h"

namespace
{
	constexpr int32 MaxArtifactPreviewItems = 8;

	void SetStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	}

	TArray<FString> ExtractStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const int32 MaxItems = MaxArtifactPreviewItems)
	{
		TArray<FString> Values;
		if (!Object.IsValid())
		{
			return Values;
		}

		const TArray<TSharedPtr<FJsonValue>>* JsonValues = nullptr;
		if (!Object->TryGetArrayField(FieldName, JsonValues) || !JsonValues)
		{
			return Values;
		}

		for (const TSharedPtr<FJsonValue>& JsonValue : *JsonValues)
		{
			FString StringValue;
			if (JsonValue.IsValid() && JsonValue->TryGetString(StringValue))
			{
				Values.Add(StringValue);
				if (MaxItems > 0 && Values.Num() >= MaxItems)
				{
					break;
				}
			}
		}

		return Values;
	}

	FString ExtractAssetName(const FString& AssetPath)
	{
		int32 DotIndex = INDEX_NONE;
		if (AssetPath.FindLastChar(TEXT('.'), DotIndex) && DotIndex + 1 < AssetPath.Len())
		{
			return AssetPath.Mid(DotIndex + 1);
		}

		int32 SlashIndex = INDEX_NONE;
		if (AssetPath.FindLastChar(TEXT('/'), SlashIndex) && SlashIndex + 1 < AssetPath.Len())
		{
			return AssetPath.Mid(SlashIndex + 1);
		}

		return AssetPath;
	}

	FString TitleFromOperation(const FString& Operation)
	{
		if (Operation == TEXT("multiplayer_audit"))
		{
			return TEXT("Multiplayer Audit");
		}
		if (Operation == TEXT("audit_object_references"))
		{
			return TEXT("Object Reference Audit");
		}
		if (Operation == TEXT("audit_persistence_placement"))
		{
			return TEXT("Persistence Placement Audit");
		}
		if (Operation == TEXT("audit_live_replication"))
		{
			return TEXT("Live Replication Audit");
		}
		return TEXT("Multiplayer Audit Report");
	}

	void AppendMarkdownSection(FString& Markdown, const FString& Title, const TArray<FString>& Items)
	{
		if (Items.Num() == 0)
		{
			return;
		}

		Markdown += FString::Printf(TEXT("## %s\n\n"), *Title);
		for (const FString& Item : Items)
		{
			Markdown += FString::Printf(TEXT("- %s\n"), *Item);
		}
		Markdown += TEXT("\n");
	}

	TArray<FString> BuildAuditEvidenceClasses(const FString& Operation, const TSharedPtr<FJsonObject>& Data)
	{
		TArray<FString> EvidenceClasses = {
			TEXT("replication_metadata"),
			TEXT("framework_role_classification")
		};

		if (Operation == TEXT("multiplayer_audit"))
		{
			EvidenceClasses.Add(TEXT("blueprint_metadata"));
		}
		else if (Operation == TEXT("audit_object_references"))
		{
			EvidenceClasses.Add(TEXT("object_reference_analysis"));
		}
		else if (Operation == TEXT("audit_persistence_placement"))
		{
			EvidenceClasses.Add(TEXT("persistence_placement_analysis"));
		}
		else if (Operation == TEXT("audit_live_replication"))
		{
			EvidenceClasses.Add(TEXT("runtime_observation"));
		}

		bool bIsRuntime = false;
		if (Data.IsValid() && Data->TryGetBoolField(TEXT("is_runtime"), bIsRuntime) && bIsRuntime)
		{
			EvidenceClasses.Add(TEXT("runtime_observation"));
		}

		return EvidenceClasses;
	}

	FOsvayderUEReportTruthSummary BuildAuditTruthSummary(
		const FString& Operation,
		const FString& BlueprintPath,
		const FString& ResultMessage,
		const TSharedPtr<FJsonObject>& Data)
	{
		FOsvayderUEReportTruthSummary TruthSummary;
		const FString OperationTitle = TitleFromOperation(Operation);
		if (!BlueprintPath.IsEmpty())
		{
			TruthSummary.Inspected.Add(FString::Printf(TEXT("%s executed for %s."), *OperationTitle, *BlueprintPath));
		}

		if (!ResultMessage.IsEmpty())
		{
			TruthSummary.Inspected.Add(ResultMessage);
		}

		FString FrameworkRole;
		if (Data.IsValid() && Data->TryGetStringField(TEXT("framework_role"), FrameworkRole) && !FrameworkRole.IsEmpty())
		{
			TruthSummary.Inspected.Add(FString::Printf(TEXT("Framework role classified as %s."), *FrameworkRole));
		}

		double WarningCount = 0.0;
		if (Data.IsValid() && Data->TryGetNumberField(TEXT("warning_count"), WarningCount))
		{
			TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed %.0f warning(s) in the audit result."), WarningCount));
		}

		if (Operation == TEXT("multiplayer_audit"))
		{
			double ReplicatedVariableCount = 0.0;
			double RpcCount = 0.0;
			if (Data.IsValid() && Data->TryGetNumberField(TEXT("replicated_variable_count"), ReplicatedVariableCount))
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed %.0f replicated variable(s) in the Blueprint metadata."), ReplicatedVariableCount));
			}
			if (Data.IsValid() && Data->TryGetNumberField(TEXT("rpc_count"), RpcCount))
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed %.0f RPC function(s) in the Blueprint metadata."), RpcCount));
			}
		}
		else if (Operation == TEXT("audit_object_references"))
		{
			double ObjectRefCount = 0.0;
			if (Data.IsValid() && Data->TryGetNumberField(TEXT("object_ref_count"), ObjectRefCount))
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed %.0f object/class/interface reference variable(s)."), ObjectRefCount));
			}
		}
		else if (Operation == TEXT("audit_persistence_placement"))
		{
			double TotalVariables = 0.0;
			double ReplicatedVariables = 0.0;
			if (Data.IsValid() && Data->TryGetNumberField(TEXT("total_variables"), TotalVariables))
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Placement review covered %.0f declared variable(s)."), TotalVariables));
			}
			if (Data.IsValid() && Data->TryGetNumberField(TEXT("replicated_variables"), ReplicatedVariables))
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed %.0f replicated variable(s) for persistence-placement review."), ReplicatedVariables));
			}
		}
		else if (Operation == TEXT("audit_live_replication"))
		{
			double AuditedActorCount = 0.0;
			double IssueCount = 0.0;
			bool bIsRuntime = false;
			if (Data.IsValid() && Data->TryGetNumberField(TEXT("audited_actor_count"), AuditedActorCount))
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed %.0f actor(s) in the live replication audit."), AuditedActorCount));
			}
			if (Data.IsValid() && Data->TryGetNumberField(TEXT("issue_count"), IssueCount))
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed %.0f live replication issue(s)."), IssueCount));
			}
			if (Data.IsValid() && Data->TryGetBoolField(TEXT("is_runtime"), bIsRuntime) && bIsRuntime)
			{
				TruthSummary.PracticallyVerified.Add(TEXT("The audit executed against a live runtime/PIE world instead of editor-only metadata."));
			}
		}

		TruthSummary.Limited.Add(TEXT("This report is derived from bounded multiplayer audit output, not from a full end-to-end gameplay run."));
		TruthSummary.NotVerified.Add(TEXT("Live multi-client session, travel, and replication correctness were not proven by this report alone."));
		return TruthSummary;
	}

	TSharedPtr<FJsonObject> BuildAuditExtraMetadata(
		const FString& Operation,
		const FString& BlueprintPath,
		const TSharedPtr<FJsonObject>& Data)
	{
		TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
		Metadata->SetStringField(TEXT("source_tool"), TEXT("multiplayer"));
		Metadata->SetStringField(TEXT("source_operation"), Operation);
		if (!BlueprintPath.IsEmpty())
		{
			Metadata->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		}

		if (Data.IsValid())
		{
			FString ClassName;
			if (Data->TryGetStringField(TEXT("class_name"), ClassName) && !ClassName.IsEmpty())
			{
				Metadata->SetStringField(TEXT("class_name"), ClassName);
			}

			FString FrameworkRole;
			if (Data->TryGetStringField(TEXT("framework_role"), FrameworkRole) && !FrameworkRole.IsEmpty())
			{
				Metadata->SetStringField(TEXT("framework_role"), FrameworkRole);
			}

			const TArray<FString> CountFields = {
				TEXT("warning_count"),
				TEXT("replicated_variable_count"),
				TEXT("rpc_count"),
				TEXT("object_ref_count"),
				TEXT("total_variables"),
				TEXT("replicated_variables"),
				TEXT("issue_count"),
				TEXT("audited_actor_count")
			};
			for (const FString& CountField : CountFields)
			{
				double CountValue = 0.0;
				if (Data->TryGetNumberField(CountField, CountValue))
				{
					Metadata->SetNumberField(CountField, CountValue);
				}
			}

			const TArray<FString> WarningPreview = ExtractStringArrayField(Data, TEXT("warnings"));
			if (WarningPreview.Num() > 0)
			{
				SetStringArrayField(Metadata, TEXT("warnings_preview"), WarningPreview);
			}

			const TArray<FString> InfoPreview = ExtractStringArrayField(Data, TEXT("info"));
			if (InfoPreview.Num() > 0)
			{
				SetStringArrayField(Metadata, TEXT("info_preview"), InfoPreview);
			}

			const TArray<FString> RecommendationPreview = ExtractStringArrayField(Data, TEXT("recommendations"));
			if (RecommendationPreview.Num() > 0)
			{
				SetStringArrayField(Metadata, TEXT("recommendations_preview"), RecommendationPreview);
			}

			const TSharedPtr<FJsonObject>* PlacementObject = nullptr;
			if (Data->TryGetObjectField(TEXT("placement"), PlacementObject) && PlacementObject && (*PlacementObject).IsValid())
			{
				TSharedPtr<FJsonObject> PlacementSummary = MakeShared<FJsonObject>();
				FString Visibility;
				FString TravelSurvival;
				FString Replication;
				if ((*PlacementObject)->TryGetStringField(TEXT("visibility"), Visibility))
				{
					PlacementSummary->SetStringField(TEXT("visibility"), Visibility);
				}
				if ((*PlacementObject)->TryGetStringField(TEXT("travel_survival"), TravelSurvival))
				{
					PlacementSummary->SetStringField(TEXT("travel_survival"), TravelSurvival);
				}
				if ((*PlacementObject)->TryGetStringField(TEXT("replication"), Replication))
				{
					PlacementSummary->SetStringField(TEXT("replication"), Replication);
				}
				if (PlacementSummary->Values.Num() > 0)
				{
					Metadata->SetObjectField(TEXT("placement_summary"), PlacementSummary);
				}
			}
		}

		return Metadata;
	}

	FString BuildAuditMarkdown(
		const FString& Operation,
		const FString& BlueprintPath,
		const FString& ResultMessage,
		const TSharedPtr<FJsonObject>& Data)
	{
		const FString Title = TitleFromOperation(Operation);
		FString Markdown = FString::Printf(TEXT("# %s\n\n"), *Title);
		Markdown += TEXT("## Summary\n\n");
		Markdown += FString::Printf(TEXT("- Source tool: `multiplayer`\n"));
		Markdown += FString::Printf(TEXT("- Operation: `%s`\n"), *Operation);
		if (!BlueprintPath.IsEmpty())
		{
			Markdown += FString::Printf(TEXT("- Blueprint: `%s`\n"), *BlueprintPath);
		}

		if (Data.IsValid())
		{
			FString ClassName;
			if (Data->TryGetStringField(TEXT("class_name"), ClassName) && !ClassName.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- Class: `%s`\n"), *ClassName);
			}

			FString FrameworkRole;
			if (Data->TryGetStringField(TEXT("framework_role"), FrameworkRole) && !FrameworkRole.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- Framework role: `%s`\n"), *FrameworkRole);
			}

			const TArray<FString> CountFields = {
				TEXT("warning_count"),
				TEXT("replicated_variable_count"),
				TEXT("rpc_count"),
				TEXT("object_ref_count"),
				TEXT("total_variables"),
				TEXT("replicated_variables"),
				TEXT("issue_count"),
				TEXT("audited_actor_count")
			};
			for (const FString& CountField : CountFields)
			{
				double CountValue = 0.0;
				if (Data->TryGetNumberField(CountField, CountValue))
				{
					Markdown += FString::Printf(TEXT("- %s: `%.0f`\n"), *CountField, CountValue);
				}
			}
		}

		Markdown += FString::Printf(TEXT("- Result: %s\n\n"), *ResultMessage);

		if (Data.IsValid())
		{
			const TArray<FString> WarningLines = ExtractStringArrayField(Data, TEXT("warnings"));
			AppendMarkdownSection(Markdown, TEXT("Warnings"), WarningLines);

			const TArray<FString> InfoLines = ExtractStringArrayField(Data, TEXT("info"));
			AppendMarkdownSection(Markdown, TEXT("Info"), InfoLines);

			const TArray<FString> RecommendationLines = ExtractStringArrayField(Data, TEXT("recommendations"));
			AppendMarkdownSection(Markdown, TEXT("Recommendations"), RecommendationLines);

			const TSharedPtr<FJsonObject>* PlacementObject = nullptr;
			if (Data->TryGetObjectField(TEXT("placement"), PlacementObject) && PlacementObject && (*PlacementObject).IsValid())
			{
				Markdown += TEXT("## Placement Summary\n\n");
				FString Visibility;
				FString TravelSurvival;
				FString Replication;
				if ((*PlacementObject)->TryGetStringField(TEXT("visibility"), Visibility) && !Visibility.IsEmpty())
				{
					Markdown += FString::Printf(TEXT("- visibility: `%s`\n"), *Visibility);
				}
				if ((*PlacementObject)->TryGetStringField(TEXT("travel_survival"), TravelSurvival) && !TravelSurvival.IsEmpty())
				{
					Markdown += FString::Printf(TEXT("- travel_survival: `%s`\n"), *TravelSurvival);
				}
				if ((*PlacementObject)->TryGetStringField(TEXT("replication"), Replication) && !Replication.IsEmpty())
				{
					Markdown += FString::Printf(TEXT("- replication: `%s`\n"), *Replication);
				}
				Markdown += TEXT("\n");
			}
		}

		Markdown += TEXT("## Truth Limits\n\n");
		Markdown += TEXT("- This artifact reflects bounded multiplayer audit output, not a full gameplay walkthrough.\n");
		Markdown += TEXT("- It does not prove end-to-end multi-client session or travel correctness by itself.\n");
		return Markdown;
	}

	FMCPToolResult MaybeAttachAuditReportArtifact(
		FMCPToolResult Result,
		const FString& Operation,
		const FString& BlueprintPath,
		const bool bExportReport,
		const FString& CustomReportName,
		const FString& CustomReportSlug)
	{
		if (!bExportReport || !Result.bSuccess || !Result.Data.IsValid())
		{
			return Result;
		}

		const FString AssetName = ExtractAssetName(BlueprintPath);
		FOsvayderUEReportExportRequest ExportRequest;
		ExportRequest.ReportName = !CustomReportName.TrimStartAndEnd().IsEmpty()
			? CustomReportName
			: FString::Printf(TEXT("%s - %s"), *TitleFromOperation(Operation), *AssetName);
		ExportRequest.ReportSlug = !CustomReportSlug.TrimStartAndEnd().IsEmpty()
			? CustomReportSlug
			: FString::Printf(TEXT("%s_%s"), *Operation, *AssetName);
		ExportRequest.Markdown = BuildAuditMarkdown(Operation, BlueprintPath, Result.Message, Result.Data);
		ExportRequest.SummaryText = Result.Message;
		ExportRequest.RunKind = Operation;
		ExportRequest.ExecutionMode = TEXT("read_only");
		ExportRequest.ToolNames = { TEXT("multiplayer") };
		ExportRequest.EvidenceClasses = BuildAuditEvidenceClasses(Operation, Result.Data);
		ExportRequest.TruthSummary = BuildAuditTruthSummary(Operation, BlueprintPath, Result.Message, Result.Data);
		ExportRequest.ExtraMetadata = BuildAuditExtraMetadata(Operation, BlueprintPath, Result.Data);

		FOsvayderUEReportExportResult ExportResult;
		if (!FOsvayderUEReportArtifacts::ExportReport(ExportRequest, ExportResult))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("%s Report export failed: %s"), *Result.Message, *ExportResult.ErrorMessage));
		}

		TSharedPtr<FJsonObject> ArtifactObject = MakeShared<FJsonObject>();
		ArtifactObject->SetStringField(TEXT("report_id"), ExportResult.ReportId);
		ArtifactObject->SetStringField(TEXT("markdown_path"), ExportResult.MarkdownPath);
		ArtifactObject->SetStringField(TEXT("summary_path"), ExportResult.SummaryPath);
		ArtifactObject->SetStringField(TEXT("export_status"), ExportResult.ExportStatus);
		ArtifactObject->SetBoolField(TEXT("roundtrip_exact"), ExportResult.bRoundTripExact);
		ArtifactObject->SetStringField(TEXT("status_tool"), TEXT("report_artifact_status"));
		ArtifactObject->SetStringField(TEXT("status_query_report_id"), ExportResult.ReportId);
		Result.Data->SetObjectField(TEXT("report_artifact"), ArtifactObject);

		Result.Message = FString::Printf(TEXT("%s | report=%s"), *Result.Message, *ExportResult.ReportId);
		return Result;
	}
}

FMCPToolInfo FMCPTool_Multiplayer::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("multiplayer");
	Info.Description = TEXT(
		"Multiplayer authoring and introspection surfaces.\n\n"
		"Query operations (read-only):\n"
		"- 'get_replication_info': Inspect replicated properties, actor replication settings (bReplicates, NetUpdateFrequency, NetPriority, bAlwaysRelevant) on a Blueprint\n"
		"- 'get_rpc_info': List RPC functions with type (Server/Client/Multicast) and reliability\n"
		"- 'get_ownership_info': Inspect owner/authority semantics for a live actor in the level\n"
		"- 'get_framework_roles': Inspect GameMode/GameState/PlayerState/PlayerController/Pawn class setup for current world\n"
		"- 'get_multiplayer_config': Inspect world settings relevant to multiplayer (travel, net mode, etc.)\n"
		"- 'multiplayer_audit': Run diagnostic audit (v2: component replication, RepNotify caveats, condition mismatches)\n"
		"- 'get_subobject_replication': Subobject replication inspection + C++ guidance (registration, FastArray, object ref safety)\n"
		"- 'audit_object_references': Scan BP vars for object refs, classify multiplayer safety (safe/requires_care/local_only)\n"
		"- 'get_travel_contracts': Per-class travel survival matrix + session contracts. Errors on bad blueprint_path.\n"
		"- 'get_network_state': Live world net mode, PIE status, player/controller topology (runtime introspection)\n"
		"- 'get_live_actor_network': Batch live actor network snapshot (authority/role/owner/replicates for actors in level)\n"
		"- 'audit_persistence_placement': Classify BP data placement vs framework role, warn on fragile storage patterns\n"
		"- 'get_session_state': Live session state — OnlineSubsystem availability, current session status. Returns unavailable if subsystem not active.\n"
		"- 'get_travel_state': Current travel context — world type, net mode, is_in_seamless_travel, GameMode seamless config, current level, travel_available (true only in PIE/Game).\n"
		"- 'audit_live_replication': Compare configured replication intent vs live actor state — warns on intent-vs-observed mismatches.\n\n"
		"Modify operations:\n"
		"- 'set_replication_config': Set actor-level replication settings on a Blueprint CDO (bReplicates, NetUpdateFrequency, bAlwaysRelevant, NetPriority)\n"
		"- 'set_property_replication': Set Replicated/RepNotify/ReplicationCondition on a BP variable\n"
		"- 'configure_multiplayer_actor': Composed bundle — actor replication config + per-variable replication in one call, with dry_run\n\n"
		"Report export:\n"
		"- read-only audit operations can persist a Markdown report + normalized artifact sidecar by setting export_report=true\n\n"
		"NOTE: This is a metadata authoring surface. It configures replication intent on assets.\n"
		"It does NOT verify runtime replication correctness, prediction/reconciliation, or large-scale relevancy.\n"
		"Runtime verification requires PIE multiplayer testing."
	);

	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"),
			TEXT("Operation: 'get_replication_info', 'get_rpc_info', 'get_ownership_info', 'get_framework_roles', "
				 "'get_multiplayer_config', 'multiplayer_audit', 'get_component_replication', "
				 "'get_replication_graph', 'classify_multiplayer_actor', 'get_subobject_replication', "
				 "'audit_object_references', 'get_travel_contracts', 'get_network_state', "
				 "'get_live_actor_network', 'audit_persistence_placement', 'get_session_state', "
				 "'get_travel_state', 'audit_live_replication', 'set_replication_config', "
				 "'set_property_replication', 'configure_multiplayer_actor'"), true),
		FMCPToolParameter(TEXT("blueprint_path"), TEXT("string"),
			TEXT("Blueprint asset path (for replication/rpc/audit/config ops)"), false),
		FMCPToolParameter(TEXT("actor_name"), TEXT("string"),
			TEXT("Actor name in level (for get_ownership_info)"), false),
		// set_replication_config params
		FMCPToolParameter(TEXT("replicates"), TEXT("boolean"),
			TEXT("Set bReplicates on actor CDO"), false),
		FMCPToolParameter(TEXT("net_update_frequency"), TEXT("number"),
			TEXT("Set NetUpdateFrequency (updates per second)"), false),
		FMCPToolParameter(TEXT("min_net_update_frequency"), TEXT("number"),
			TEXT("Set MinNetUpdateFrequency"), false),
		FMCPToolParameter(TEXT("always_relevant"), TEXT("boolean"),
			TEXT("Set bAlwaysRelevant"), false),
		FMCPToolParameter(TEXT("net_priority"), TEXT("number"),
			TEXT("Set NetPriority"), false),
		// set_property_replication params
		FMCPToolParameter(TEXT("variable_name"), TEXT("string"),
			TEXT("BP variable name to configure replication on"), false),
		FMCPToolParameter(TEXT("replicated"), TEXT("boolean"),
			TEXT("Enable/disable replication on variable"), false),
		FMCPToolParameter(TEXT("rep_notify"), TEXT("boolean"),
			TEXT("Enable RepNotify on variable (creates OnRep_ function name)"), false),
		FMCPToolParameter(TEXT("rep_notify_func"), TEXT("string"),
			TEXT("Custom RepNotify function name (default: OnRep_<VarName>)"), false),
		FMCPToolParameter(TEXT("replication_condition"), TEXT("string"),
			TEXT("Replication condition: None, InitialOnly, OwnerOnly, SkipOwner, SimulatedOnly, AutonomousOnly, SimulatedOrPhysics, InitialOrOwner, Custom, Never"), false),
		// configure_multiplayer_actor params
		FMCPToolParameter(TEXT("variables"), TEXT("array"),
			TEXT("Array of {name, replicated, rep_notify, replication_condition} for configure_multiplayer_actor"), false),
		FMCPToolParameter(TEXT("dry_run"), TEXT("boolean"),
			TEXT("If true, report what would change without mutating"), false, TEXT("false")),
		FMCPToolParameter(TEXT("export_report"), TEXT("boolean"),
			TEXT("For read-only audit operations, if true, persist a Markdown report and normalized sidecar under Saved/OsvayderUE/Reports."), false, TEXT("false")),
		FMCPToolParameter(TEXT("report_name"), TEXT("string"),
			TEXT("Optional custom report name when export_report=true."), false),
		FMCPToolParameter(TEXT("report_slug"), TEXT("string"),
			TEXT("Optional custom slug for saved report filenames when export_report=true."), false),
	};

	// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
	Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
		TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_Multiplayer::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("get_replication_info"))      return ExecuteGetReplicationInfo(Params);
	if (Operation == TEXT("get_rpc_info"))              return ExecuteGetRpcInfo(Params);
	if (Operation == TEXT("get_ownership_info"))        return ExecuteGetOwnershipInfo(Params);
	if (Operation == TEXT("get_framework_roles"))       return ExecuteGetFrameworkRoles(Params);
	if (Operation == TEXT("get_multiplayer_config"))    return ExecuteGetMultiplayerConfig(Params);
	if (Operation == TEXT("multiplayer_audit"))         return ExecuteMultiplayerAudit(Params);
	if (Operation == TEXT("set_replication_config"))    return ExecuteSetReplicationConfig(Params);
	if (Operation == TEXT("set_property_replication"))  return ExecuteSetPropertyReplication(Params);
	if (Operation == TEXT("configure_multiplayer_actor")) return ExecuteConfigureMultiplayerActor(Params);
	if (Operation == TEXT("get_component_replication"))    return ExecuteGetComponentReplication(Params);
	if (Operation == TEXT("get_replication_graph"))        return ExecuteGetReplicationGraph(Params);
	if (Operation == TEXT("classify_multiplayer_actor"))   return ExecuteClassifyMultiplayerActor(Params);
	if (Operation == TEXT("get_subobject_replication"))    return ExecuteGetSubobjectReplication(Params);
	if (Operation == TEXT("audit_object_references"))      return ExecuteAuditObjectReferences(Params);
	if (Operation == TEXT("get_travel_contracts"))         return ExecuteGetTravelContracts(Params);
	if (Operation == TEXT("get_network_state"))            return ExecuteGetNetworkState(Params);
	if (Operation == TEXT("get_live_actor_network"))       return ExecuteGetLiveActorNetwork(Params);
	if (Operation == TEXT("audit_persistence_placement"))  return ExecuteAuditPersistencePlacement(Params);
	if (Operation == TEXT("get_session_state"))            return ExecuteGetSessionState(Params);
	if (Operation == TEXT("get_travel_state"))             return ExecuteGetTravelState(Params);
	if (Operation == TEXT("audit_live_replication"))       return ExecuteAuditLiveReplication(Params);

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown multiplayer operation: '%s'. Valid: get_replication_info, get_rpc_info, get_ownership_info, ")
		TEXT("get_framework_roles, get_multiplayer_config, multiplayer_audit, set_replication_config, ")
		TEXT("set_property_replication, configure_multiplayer_actor, get_component_replication, ")
		TEXT("get_replication_graph, classify_multiplayer_actor, get_subobject_replication, ")
		TEXT("audit_object_references, get_travel_contracts, get_network_state, get_live_actor_network, ")
		TEXT("audit_persistence_placement, get_session_state, get_travel_state, audit_live_replication"),
		*Operation));
}

// ===== Helpers =====

UBlueprint* FMCPTool_Multiplayer::LoadBlueprintByPath(const FString& Path, FString& OutError)
{
	FSoftObjectPath SoftPath(Path);
	UObject* Asset = SoftPath.TryLoad();
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("Could not load asset: %s"), *Path);
		return nullptr;
	}
	UBlueprint* BP = Cast<UBlueprint>(Asset);
	if (!BP)
	{
		OutError = FString::Printf(TEXT("Asset is not a Blueprint: %s"), *Path);
		return nullptr;
	}
	return BP;
}

FString FMCPTool_Multiplayer::ReplicationConditionToString(uint8 Condition)
{
	switch (static_cast<ELifetimeCondition>(Condition))
	{
	case COND_None: return TEXT("None");
	case COND_InitialOnly: return TEXT("InitialOnly");
	case COND_OwnerOnly: return TEXT("OwnerOnly");
	case COND_SkipOwner: return TEXT("SkipOwner");
	case COND_SimulatedOnly: return TEXT("SimulatedOnly");
	case COND_AutonomousOnly: return TEXT("AutonomousOnly");
	case COND_SimulatedOrPhysics: return TEXT("SimulatedOrPhysics");
	case COND_InitialOrOwner: return TEXT("InitialOrOwner");
	case COND_Custom: return TEXT("Custom");
	case COND_ReplayOrOwner: return TEXT("ReplayOrOwner");
	case COND_ReplayOnly: return TEXT("ReplayOnly");
	case COND_Never: return TEXT("Never");
	case COND_Dynamic: return TEXT("Dynamic");
	case COND_NetGroup: return TEXT("NetGroup");
	default: return TEXT("Unknown");
	}
}

FString FMCPTool_Multiplayer::RpcTypeToString(EFunctionFlags Flags)
{
	FString Type;
	if (Flags & FUNC_NetServer) Type = TEXT("Server");
	else if (Flags & FUNC_NetClient) Type = TEXT("Client");
	else if (Flags & FUNC_NetMulticast) Type = TEXT("NetMulticast");
	else Type = TEXT("Net");

	if (Flags & FUNC_NetReliable)
	{
		Type += TEXT(", Reliable");
	}
	else
	{
		Type += TEXT(", Unreliable");
	}
	return Type;
}

FString FMCPTool_Multiplayer::FrameworkRoleForClass(UClass* Class)
{
	if (!Class) return TEXT("Unknown");
	if (Class->IsChildOf(AGameModeBase::StaticClass())) return TEXT("GameMode (server-only, not replicated)");
	if (Class->IsChildOf(AGameStateBase::StaticClass())) return TEXT("GameState (replicated to all clients)");
	if (Class->IsChildOf(APlayerState::StaticClass())) return TEXT("PlayerState (replicated, per-player persistent across travel)");
	if (Class->IsChildOf(APlayerController::StaticClass())) return TEXT("PlayerController (replicated to owning client only)");
	if (Class->IsChildOf(ACharacter::StaticClass())) return TEXT("Character (replicated pawn with movement)");
	if (Class->IsChildOf(APawn::StaticClass())) return TEXT("Pawn (replicated, possessed by controller)");
	if (Class->IsChildOf(AActor::StaticClass())) return TEXT("Actor");
	return TEXT("Non-Actor");
}

// ===== Query: Get Replication Info =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetReplicationInfo(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		return FMCPToolResult::Error(TEXT("Blueprint has no generated class"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetStringField(TEXT("class_name"), GenClass->GetName());
	Data->SetStringField(TEXT("framework_role"), FrameworkRoleForClass(GenClass));

	// Actor-level replication settings from CDO
	bool bIsActor = GenClass->IsChildOf(AActor::StaticClass());
	if (bIsActor)
	{
		AActor* CDO = GenClass->GetDefaultObject<AActor>();
		if (CDO)
		{
			Data->SetBoolField(TEXT("replicates"), CDO->GetIsReplicated());
			Data->SetNumberField(TEXT("net_update_frequency"), CDO->GetNetUpdateFrequency());
			Data->SetNumberField(TEXT("min_net_update_frequency"), CDO->GetMinNetUpdateFrequency());
			Data->SetNumberField(TEXT("net_priority"), CDO->NetPriority);
			Data->SetBoolField(TEXT("always_relevant"), CDO->bAlwaysRelevant != 0);
			Data->SetBoolField(TEXT("only_relevant_to_owner"), CDO->bOnlyRelevantToOwner != 0);
			Data->SetBoolField(TEXT("net_use_owner_relevancy"), CDO->bNetUseOwnerRelevancy != 0);
		}
	}
	Data->SetBoolField(TEXT("is_actor"), bIsActor);

	// Replicated properties from BP variables
	TArray<TSharedPtr<FJsonValue>> ReplicatedProps;
	TArray<TSharedPtr<FJsonValue>> NonReplicatedProps;

	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		PropObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());

		bool bReplicated = (Var.PropertyFlags & CPF_Net) != 0;
		bool bRepNotify = (Var.PropertyFlags & CPF_RepNotify) != 0;

		PropObj->SetBoolField(TEXT("replicated"), bReplicated);
		PropObj->SetBoolField(TEXT("rep_notify"), bRepNotify);

		if (bRepNotify && !Var.RepNotifyFunc.IsNone())
		{
			PropObj->SetStringField(TEXT("rep_notify_func"), Var.RepNotifyFunc.ToString());
		}

		PropObj->SetStringField(TEXT("replication_condition"),
			ReplicationConditionToString(static_cast<uint8>(Var.ReplicationCondition)));

		if (bReplicated)
		{
			ReplicatedProps.Add(MakeShared<FJsonValueObject>(PropObj));
		}
		else
		{
			NonReplicatedProps.Add(MakeShared<FJsonValueObject>(PropObj));
		}
	}

	Data->SetArrayField(TEXT("replicated_properties"), ReplicatedProps);
	Data->SetNumberField(TEXT("replicated_count"), ReplicatedProps.Num());
	Data->SetArrayField(TEXT("non_replicated_properties"), NonReplicatedProps);
	Data->SetNumberField(TEXT("non_replicated_count"), NonReplicatedProps.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Replication info for %s: %d replicated, %d non-replicated properties"),
			*GenClass->GetName(), ReplicatedProps.Num(), NonReplicatedProps.Num()),
		Data);
}

// ===== Query: Get RPC Info =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetRpcInfo(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		return FMCPToolResult::Error(TEXT("Blueprint has no generated class"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetStringField(TEXT("class_name"), GenClass->GetName());

	TArray<TSharedPtr<FJsonValue>> RpcFunctions;

	for (TFieldIterator<UFunction> FuncIt(GenClass); FuncIt; ++FuncIt)
	{
		UFunction* Func = *FuncIt;
		EFunctionFlags Flags = Func->FunctionFlags;

		if (!(Flags & FUNC_Net)) continue;

		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), Func->GetName());
		FuncObj->SetStringField(TEXT("rpc_type"), RpcTypeToString(Flags));
		FuncObj->SetBoolField(TEXT("reliable"), (Flags & FUNC_NetReliable) != 0);
		FuncObj->SetBoolField(TEXT("server"), (Flags & FUNC_NetServer) != 0);
		FuncObj->SetBoolField(TEXT("client"), (Flags & FUNC_NetClient) != 0);
		FuncObj->SetBoolField(TEXT("multicast"), (Flags & FUNC_NetMulticast) != 0);

		// Ownership guidance
		if (Flags & FUNC_NetServer)
		{
			FuncObj->SetStringField(TEXT("ownership_note"), TEXT("Client->Server: must be called from owning client"));
		}
		else if (Flags & FUNC_NetClient)
		{
			FuncObj->SetStringField(TEXT("ownership_note"), TEXT("Server->OwningClient: server calls, only owning client receives"));
		}
		else if (Flags & FUNC_NetMulticast)
		{
			FuncObj->SetStringField(TEXT("ownership_note"), TEXT("Server->AllClients: server calls, all clients execute"));
		}

		// Check if declared in this class or inherited
		FuncObj->SetBoolField(TEXT("local"), Func->GetOwnerClass() == GenClass);

		RpcFunctions.Add(MakeShared<FJsonValueObject>(FuncObj));
	}

	Data->SetArrayField(TEXT("rpc_functions"), RpcFunctions);
	Data->SetNumberField(TEXT("rpc_count"), RpcFunctions.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("RPC info for %s: %d RPC functions"), *GenClass->GetName(), RpcFunctions.Num()),
		Data);
}

// ===== Query: Get Ownership Info =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetOwnershipInfo(const TSharedRef<FJsonObject>& Params)
{
	FString ActorName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("actor_name"), ActorName, Error))
	{
		return Error.GetValue();
	}

	UWorld* World = nullptr;
	TOptional<FMCPToolResult> ContextError = ValidateEditorContext(World);
	if (ContextError.IsSet())
	{
		return ContextError.GetValue();
	}

	AActor* Actor = FindActorByNameOrLabel(World, ActorName);
	if (!Actor)
	{
		return ActorNotFoundError(ActorName);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_name"), Actor->GetName());
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	Data->SetStringField(TEXT("framework_role"), FrameworkRoleForClass(Actor->GetClass()));
	Data->SetBoolField(TEXT("replicates"), Actor->GetIsReplicated());

	// Owner chain
	AActor* Owner = Actor->GetOwner();
	if (Owner)
	{
		Data->SetStringField(TEXT("owner"), Owner->GetName());
		Data->SetStringField(TEXT("owner_class"), Owner->GetClass()->GetName());
	}
	else
	{
		Data->SetStringField(TEXT("owner"), TEXT("(none)"));
	}

	// Authority
	Data->SetBoolField(TEXT("has_authority"), Actor->HasAuthority());

	// Role (only meaningful in networked context)
	FString RoleStr;
	switch (Actor->GetLocalRole())
	{
	case ROLE_Authority: RoleStr = TEXT("Authority"); break;
	case ROLE_AutonomousProxy: RoleStr = TEXT("AutonomousProxy"); break;
	case ROLE_SimulatedProxy: RoleStr = TEXT("SimulatedProxy"); break;
	default: RoleStr = TEXT("None"); break;
	}
	Data->SetStringField(TEXT("local_role"), RoleStr);

	FString RemoteRoleStr;
	switch (Actor->GetRemoteRole())
	{
	case ROLE_Authority: RemoteRoleStr = TEXT("Authority"); break;
	case ROLE_AutonomousProxy: RemoteRoleStr = TEXT("AutonomousProxy"); break;
	case ROLE_SimulatedProxy: RemoteRoleStr = TEXT("SimulatedProxy"); break;
	default: RemoteRoleStr = TEXT("None"); break;
	}
	Data->SetStringField(TEXT("remote_role"), RemoteRoleStr);

	// Multiplayer guidance
	TArray<TSharedPtr<FJsonValue>> Guidance;
	if (!Actor->GetIsReplicated())
	{
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Actor does not replicate — exists locally only")));
	}
	if (Actor->HasAuthority())
	{
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("This instance has authority — state changes here are authoritative")));
	}
	if (Actor->bOnlyRelevantToOwner)
	{
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Only relevant to owner — other clients will not see this actor")));
	}
	Data->SetArrayField(TEXT("guidance"), Guidance);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Ownership info for %s"), *Actor->GetName()), Data);
}

// ===== Query: Get Framework Roles =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetFrameworkRoles(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	TOptional<FMCPToolResult> ContextError = ValidateEditorContext(World);
	if (ContextError.IsSet())
	{
		return ContextError.GetValue();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// GameMode
	{
		TSharedPtr<FJsonObject> GM = MakeShared<FJsonObject>();
		AWorldSettings* WS = World->GetWorldSettings();
		if (WS)
		{
			UClass* GMClass = WS->DefaultGameMode;
			GM->SetStringField(TEXT("class"), GMClass ? GMClass->GetPathName() : TEXT("(not set)"));
		}
		GM->SetStringField(TEXT("role"), TEXT("server-only"));
		GM->SetStringField(TEXT("replication"), TEXT("NOT replicated — only exists on server"));
		GM->SetStringField(TEXT("purpose"), TEXT("Game rules, spawn logic, match state transitions. Server authority."));
		GM->SetStringField(TEXT("survives_travel"), TEXT("No — new GameMode created per level"));
		Data->SetObjectField(TEXT("game_mode"), GM);
	}

	// GameState
	{
		TSharedPtr<FJsonObject> GS = MakeShared<FJsonObject>();
		GS->SetStringField(TEXT("role"), TEXT("replicated to all"));
		GS->SetStringField(TEXT("replication"), TEXT("Replicated — all clients receive"));
		GS->SetStringField(TEXT("purpose"), TEXT("Match score, timer, player list, public game state. Authoritative on server."));
		GS->SetStringField(TEXT("survives_travel"), TEXT("No — new GameState per level, but PlayerStates can persist"));
		Data->SetObjectField(TEXT("game_state"), GS);
	}

	// PlayerState
	{
		TSharedPtr<FJsonObject> PS = MakeShared<FJsonObject>();
		PS->SetStringField(TEXT("role"), TEXT("replicated per-player"));
		PS->SetStringField(TEXT("replication"), TEXT("Replicated — all clients see all PlayerStates"));
		PS->SetStringField(TEXT("purpose"), TEXT("Player name, score, team, persistent per-player data. Best place for data that survives seamless travel."));
		PS->SetStringField(TEXT("survives_travel"), TEXT("Yes — persists across seamless travel"));
		Data->SetObjectField(TEXT("player_state"), PS);
	}

	// PlayerController
	{
		TSharedPtr<FJsonObject> PC = MakeShared<FJsonObject>();
		PC->SetStringField(TEXT("role"), TEXT("replicated to owning client only"));
		PC->SetStringField(TEXT("replication"), TEXT("Replicated — but only owning client receives. Other clients cannot see."));
		PC->SetStringField(TEXT("purpose"), TEXT("Input handling, UI, camera, client-side logic. Owns the connection."));
		PC->SetStringField(TEXT("survives_travel"), TEXT("Yes — persists across seamless travel"));
		PC->SetStringField(TEXT("ownership_note"), TEXT("Server RPCs from this controller are legal because it owns the connection"));
		Data->SetObjectField(TEXT("player_controller"), PC);
	}

	// Pawn / Character
	{
		TSharedPtr<FJsonObject> Pawn = MakeShared<FJsonObject>();
		Pawn->SetStringField(TEXT("role"), TEXT("replicated, possessed by controller"));
		Pawn->SetStringField(TEXT("replication"), TEXT("Replicated — all clients see. Autonomous proxy on owning client, simulated on others."));
		Pawn->SetStringField(TEXT("purpose"), TEXT("Physical representation in world. Movement, abilities, combat. Owned by possessing PlayerController."));
		Pawn->SetStringField(TEXT("survives_travel"), TEXT("No — destroyed on travel, re-possessed in new level"));
		Pawn->SetStringField(TEXT("ownership_note"), TEXT("Server RPCs legal from owning client. Client RPCs sent to owning client."));
		Data->SetObjectField(TEXT("pawn"), Pawn);
	}

	return FMCPToolResult::Success(TEXT("Multiplayer framework roles"), Data);
}

// ===== Query: Get Multiplayer Config =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetMultiplayerConfig(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	TOptional<FMCPToolResult> ContextError = ValidateEditorContext(World);
	if (ContextError.IsSet())
	{
		return ContextError.GetValue();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	AWorldSettings* WS = World->GetWorldSettings();
	if (WS)
	{
		// Game Mode
		UClass* GMClass = WS->DefaultGameMode;
		Data->SetStringField(TEXT("default_game_mode"),
			GMClass ? GMClass->GetPathName() : TEXT("(not set)"));

		// Travel
		Data->SetBoolField(TEXT("enable_world_composition"), WS->bEnableWorldComposition);
	}

	// Net mode
	FString NetModeStr;
	switch (World->GetNetMode())
	{
	case NM_Standalone: NetModeStr = TEXT("Standalone"); break;
	case NM_DedicatedServer: NetModeStr = TEXT("DedicatedServer"); break;
	case NM_ListenServer: NetModeStr = TEXT("ListenServer"); break;
	case NM_Client: NetModeStr = TEXT("Client"); break;
	default: NetModeStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("net_mode"), NetModeStr);

	// Session/Travel guidance
	TSharedPtr<FJsonObject> TravelGuide = MakeShared<FJsonObject>();
	TravelGuide->SetStringField(TEXT("server_travel"), TEXT("Server-initiated level change. All connected clients follow. Use: GetWorld()->ServerTravel(URL)"));
	TravelGuide->SetStringField(TEXT("client_travel"), TEXT("Client-initiated connection to another server. Use: PlayerController->ClientTravel(URL, TRAVEL_Absolute)"));
	TravelGuide->SetStringField(TEXT("seamless_travel"), TEXT("Smooth transition — PlayerController and PlayerState persist. Enable via GameMode::bUseSeamlessTravel"));
	TravelGuide->SetStringField(TEXT("non_seamless_travel"), TEXT("Hard transition — everything destroyed and recreated. All clients disconnect and reconnect."));
	TravelGuide->SetStringField(TEXT("what_survives_seamless"), TEXT("PlayerController, PlayerState, GameInstance. NOT: Pawns, GameMode, GameState, level actors."));
	Data->SetObjectField(TEXT("travel_guide"), TravelGuide);

	// Session guidance
	TSharedPtr<FJsonObject> SessionGuide = MakeShared<FJsonObject>();
	SessionGuide->SetStringField(TEXT("overview"), TEXT("Sessions managed via IOnlineSubsystem + IOnlineSession. Config in DefaultEngine.ini [OnlineSubsystem]."));
	SessionGuide->SetStringField(TEXT("create"), TEXT("IOnlineSession::CreateSession(LocalPlayer, SessionName, SessionSettings)"));
	SessionGuide->SetStringField(TEXT("find"), TEXT("IOnlineSession::FindSessions(LocalPlayer, SearchSettings)"));
	SessionGuide->SetStringField(TEXT("join"), TEXT("IOnlineSession::JoinSession(LocalPlayer, SessionName, SearchResult)"));
	SessionGuide->SetStringField(TEXT("destroy"), TEXT("IOnlineSession::DestroySession(SessionName)"));
	Data->SetObjectField(TEXT("session_guide"), SessionGuide);

	return FMCPToolResult::Success(TEXT("Multiplayer config and guidance"), Data);
}

// ===== Query: Multiplayer Audit =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteMultiplayerAudit(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		return FMCPToolResult::Error(TEXT("Blueprint has no generated class"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetStringField(TEXT("class_name"), GenClass->GetName());
	Data->SetStringField(TEXT("framework_role"), FrameworkRoleForClass(GenClass));

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Info;
	int32 ReplicatedVarCount = 0;
	int32 RpcCount = 0;

	bool bIsActor = GenClass->IsChildOf(AActor::StaticClass());

	// Check actor replication
	if (bIsActor)
	{
		AActor* CDO = GenClass->GetDefaultObject<AActor>();
		if (CDO && !CDO->GetIsReplicated())
		{
			// Check if any variables are marked replicated
			for (const FBPVariableDescription& Var : BP->NewVariables)
			{
				if (Var.PropertyFlags & CPF_Net)
				{
					Warnings.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Variable '%s' is marked Replicated but actor bReplicates=false — replication will not work"),
							*Var.VarName.ToString())));
				}
			}

			// Check if any RPCs exist
			for (TFieldIterator<UFunction> FuncIt(GenClass); FuncIt; ++FuncIt)
			{
				if ((*FuncIt)->FunctionFlags & FUNC_Net)
				{
					Warnings.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Function '%s' is an RPC but actor bReplicates=false — RPCs will not work"),
							*(*FuncIt)->GetName())));
					break;
				}
			}
		}
	}

	// Check BP variables
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		bool bReplicated = (Var.PropertyFlags & CPF_Net) != 0;
		bool bRepNotify = (Var.PropertyFlags & CPF_RepNotify) != 0;

		if (bReplicated) ReplicatedVarCount++;

		// RepNotify without handler
		if (bRepNotify && Var.RepNotifyFunc.IsNone())
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Variable '%s' has RepNotify enabled but no notify function specified"),
					*Var.VarName.ToString())));
		}

		// RepNotify without replication
		if (bRepNotify && !bReplicated)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Variable '%s' has RepNotify but is NOT marked Replicated — notify will never fire"),
					*Var.VarName.ToString())));
		}
	}

	// Count RPCs
	for (TFieldIterator<UFunction> FuncIt(GenClass); FuncIt; ++FuncIt)
	{
		if ((*FuncIt)->FunctionFlags & FUNC_Net)
		{
			RpcCount++;
		}
	}

	// Framework role guidance
	if (GenClass->IsChildOf(AGameModeBase::StaticClass()))
	{
		Info.Add(MakeShared<FJsonValueString>(TEXT("GameMode is server-only. Variables here are NOT replicated to clients. Use GameState for public data.")));
	}
	if (GenClass->IsChildOf(APlayerController::StaticClass()))
	{
		Info.Add(MakeShared<FJsonValueString>(TEXT("PlayerController is only replicated to owning client. Do not store data here that other players need.")));
	}

	// v2: Component replication checks
	if (bIsActor)
	{
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		AActor* CDO = GenClass->GetDefaultObject<AActor>();
		if (SCS && CDO)
		{
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (!Node || !Node->ComponentTemplate) continue;
				if (Node->ComponentTemplate->GetIsReplicated() && !CDO->GetIsReplicated())
				{
					Warnings.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Component '%s' is replicated but actor bReplicates=false — component replication will not work"),
							*Node->GetVariableName().ToString())));
				}
			}
		}
	}

	// v2: RepNotify caveats
	int32 RepNotifyCount = 0;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if ((Var.PropertyFlags & CPF_RepNotify) != 0) RepNotifyCount++;
	}
	if (RepNotifyCount > 0)
	{
		Info.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("RepNotify caveat: %d variables use RepNotify. Notify fires on clients when server value arrives — order is NOT guaranteed across multiple RepNotify vars in the same frame."),
				RepNotifyCount)));
		Info.Add(MakeShared<FJsonValueString>(TEXT("RepNotify caveat: The old value in OnRep_ is the previous replicated value, NOT the value before the server change.")));
	}

	// v2: Replication condition mismatches
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (!(Var.PropertyFlags & CPF_Net)) continue;

		// OwnerOnly on a GameState makes no sense (no single owner)
		if (Var.ReplicationCondition == COND_OwnerOnly && GenClass->IsChildOf(AGameStateBase::StaticClass()))
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Variable '%s' has OwnerOnly condition on GameState — GameState has no single owner, this condition will prevent replication"),
					*Var.VarName.ToString())));
		}

		// AutonomousOnly on a non-pawn
		if (Var.ReplicationCondition == COND_AutonomousOnly && !GenClass->IsChildOf(APawn::StaticClass()))
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Variable '%s' has AutonomousOnly condition on non-Pawn — only Pawns have autonomous proxy, this may not replicate as expected"),
					*Var.VarName.ToString())));
		}
	}

	Data->SetArrayField(TEXT("warnings"), Warnings);
	Data->SetArrayField(TEXT("info"), Info);
	Data->SetNumberField(TEXT("warning_count"), Warnings.Num());
	Data->SetNumberField(TEXT("replicated_variable_count"), ReplicatedVarCount);
	Data->SetNumberField(TEXT("rpc_count"), RpcCount);

	FMCPToolResult Result = FMCPToolResult::Success(
		FString::Printf(TEXT("Multiplayer audit for %s: %d warnings, %d replicated vars, %d RPCs"),
			*GenClass->GetName(), Warnings.Num(), ReplicatedVarCount, RpcCount),
		Data);

	return MaybeAttachAuditReportArtifact(
		Result,
		TEXT("multiplayer_audit"),
		BPPath,
		ExtractOptionalBool(Params, TEXT("export_report"), false),
		ExtractOptionalString(Params, TEXT("report_name")),
		ExtractOptionalString(Params, TEXT("report_slug")));
}

// ===== Modify: Set Replication Config =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteSetReplicationConfig(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass || !GenClass->IsChildOf(AActor::StaticClass()))
	{
		return FMCPToolResult::Error(TEXT("Blueprint must be an Actor subclass for replication config"));
	}

	AActor* CDO = GenClass->GetDefaultObject<AActor>();
	if (!CDO)
	{
		return FMCPToolResult::Error(TEXT("Could not get CDO"));
	}

	TArray<TSharedPtr<FJsonValue>> Changed;
	TArray<TSharedPtr<FJsonValue>> Failed;

	auto AddChanged = [&](const FString& Prop, const FString& From, const FString& To)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("property"), Prop);
		Obj->SetStringField(TEXT("from"), From);
		Obj->SetStringField(TEXT("to"), To);
		Obj->SetStringField(TEXT("status"), TEXT("changed"));
		Changed.Add(MakeShared<FJsonValueObject>(Obj));
	};

	// bReplicates
	bool bVal;
	if (Params->TryGetBoolField(TEXT("replicates"), bVal))
	{
		bool bOld = CDO->GetIsReplicated();
		if (bOld != bVal)
		{
			CDO->SetReplicates(bVal);
			AddChanged(TEXT("bReplicates"), bOld ? TEXT("true") : TEXT("false"), bVal ? TEXT("true") : TEXT("false"));
		}
	}

	// NetUpdateFrequency
	double NumVal;
	if (Params->TryGetNumberField(TEXT("net_update_frequency"), NumVal))
	{
		float OldVal = CDO->GetNetUpdateFrequency();
		if (!FMath::IsNearlyEqual(OldVal, static_cast<float>(NumVal)))
		{
			CDO->SetNetUpdateFrequency(static_cast<float>(NumVal));
			AddChanged(TEXT("NetUpdateFrequency"), FString::SanitizeFloat(OldVal), FString::SanitizeFloat(NumVal));
		}
	}

	// MinNetUpdateFrequency
	if (Params->TryGetNumberField(TEXT("min_net_update_frequency"), NumVal))
	{
		float OldVal = CDO->GetMinNetUpdateFrequency();
		if (!FMath::IsNearlyEqual(OldVal, static_cast<float>(NumVal)))
		{
			CDO->SetMinNetUpdateFrequency(static_cast<float>(NumVal));
			AddChanged(TEXT("MinNetUpdateFrequency"), FString::SanitizeFloat(OldVal), FString::SanitizeFloat(NumVal));
		}
	}

	// bAlwaysRelevant
	if (Params->TryGetBoolField(TEXT("always_relevant"), bVal))
	{
		bool bOld = CDO->bAlwaysRelevant != 0;
		if (bOld != bVal)
		{
			CDO->bAlwaysRelevant = bVal ? 1 : 0;
			AddChanged(TEXT("bAlwaysRelevant"), bOld ? TEXT("true") : TEXT("false"), bVal ? TEXT("true") : TEXT("false"));
		}
	}

	// NetPriority
	if (Params->TryGetNumberField(TEXT("net_priority"), NumVal))
	{
		float OldVal = CDO->NetPriority;
		if (!FMath::IsNearlyEqual(OldVal, static_cast<float>(NumVal)))
		{
			CDO->NetPriority = static_cast<float>(NumVal);
			AddChanged(TEXT("NetPriority"), FString::SanitizeFloat(OldVal), FString::SanitizeFloat(NumVal));
		}
	}

	if (Changed.Num() > 0)
	{
		CDO->MarkPackageDirty();
		BP->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetArrayField(TEXT("changed"), Changed);
	Data->SetNumberField(TEXT("changed_count"), Changed.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set %d replication config properties on %s"), Changed.Num(), *BPPath), Data);
}

// ===== Modify: Set Property Replication =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteSetPropertyReplication(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString VarName;
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VarName, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	// Find the variable
	FBPVariableDescription* FoundVar = nullptr;
	for (FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName.ToString() == VarName)
		{
			FoundVar = &Var;
			break;
		}
	}

	if (!FoundVar)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VarName));
	}

	TArray<TSharedPtr<FJsonValue>> Changed;

	// Replicated flag
	bool bSetReplicated;
	if (Params->TryGetBoolField(TEXT("replicated"), bSetReplicated))
	{
		bool bCurrentlyReplicated = (FoundVar->PropertyFlags & CPF_Net) != 0;
		if (bCurrentlyReplicated != bSetReplicated)
		{
			if (bSetReplicated)
			{
				FoundVar->PropertyFlags |= CPF_Net;
			}
			else
			{
				FoundVar->PropertyFlags &= ~CPF_Net;
				FoundVar->PropertyFlags &= ~CPF_RepNotify;
				FoundVar->RepNotifyFunc = NAME_None;
			}
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("field"), TEXT("replicated"));
			Obj->SetStringField(TEXT("status"), TEXT("changed"));
			Obj->SetBoolField(TEXT("value"), bSetReplicated);
			Changed.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	// RepNotify
	bool bSetRepNotify;
	if (Params->TryGetBoolField(TEXT("rep_notify"), bSetRepNotify))
	{
		bool bCurrentlyRepNotify = (FoundVar->PropertyFlags & CPF_RepNotify) != 0;
		if (bCurrentlyRepNotify != bSetRepNotify)
		{
			if (bSetRepNotify)
			{
				// Must be replicated first
				if (!(FoundVar->PropertyFlags & CPF_Net))
				{
					FoundVar->PropertyFlags |= CPF_Net;
				}
				FoundVar->PropertyFlags |= CPF_RepNotify;

				// Set notify function name
				FString NotifyFuncName = ExtractOptionalString(Params, TEXT("rep_notify_func"),
					FString::Printf(TEXT("OnRep_%s"), *VarName));
				FoundVar->RepNotifyFunc = FName(*NotifyFuncName);
			}
			else
			{
				FoundVar->PropertyFlags &= ~CPF_RepNotify;
				FoundVar->RepNotifyFunc = NAME_None;
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("field"), TEXT("rep_notify"));
			Obj->SetStringField(TEXT("status"), TEXT("changed"));
			Obj->SetBoolField(TEXT("value"), bSetRepNotify);
			if (bSetRepNotify)
			{
				Obj->SetStringField(TEXT("notify_func"), FoundVar->RepNotifyFunc.ToString());
			}
			Changed.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	// Replication condition
	FString CondStr = ExtractOptionalString(Params, TEXT("replication_condition"));
	if (!CondStr.IsEmpty())
	{
		ELifetimeCondition NewCond = COND_None;
		if (CondStr == TEXT("None")) NewCond = COND_None;
		else if (CondStr == TEXT("InitialOnly")) NewCond = COND_InitialOnly;
		else if (CondStr == TEXT("OwnerOnly")) NewCond = COND_OwnerOnly;
		else if (CondStr == TEXT("SkipOwner")) NewCond = COND_SkipOwner;
		else if (CondStr == TEXT("SimulatedOnly")) NewCond = COND_SimulatedOnly;
		else if (CondStr == TEXT("AutonomousOnly")) NewCond = COND_AutonomousOnly;
		else if (CondStr == TEXT("SimulatedOrPhysics")) NewCond = COND_SimulatedOrPhysics;
		else if (CondStr == TEXT("InitialOrOwner")) NewCond = COND_InitialOrOwner;
		else if (CondStr == TEXT("Custom")) NewCond = COND_Custom;
		else if (CondStr == TEXT("Never")) NewCond = COND_Never;
		else
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Unknown replication condition: %s"), *CondStr));
		}

		if (FoundVar->ReplicationCondition != NewCond)
		{
			FoundVar->ReplicationCondition = NewCond;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("field"), TEXT("replication_condition"));
			Obj->SetStringField(TEXT("status"), TEXT("changed"));
			Obj->SetStringField(TEXT("value"), CondStr);
			Changed.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	if (Changed.Num() > 0)
	{
		BP->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetStringField(TEXT("variable_name"), VarName);
	Data->SetArrayField(TEXT("changed"), Changed);
	Data->SetNumberField(TEXT("changed_count"), Changed.Num());

	// Emit receipt
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("multiplayer");
		Receipt.Summary = TEXT("set_property_replication");
		Receipt.bSuccess = true;
		Receipt.Targets.Add(BPPath);
		Receipt.Classification = TEXT("user_mutation");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set %d replication fields on %s.%s"), Changed.Num(), *BPPath, *VarName), Data);
}

// ===== Modify: Configure Multiplayer Actor (Composed Bundle) =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteConfigureMultiplayerActor(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	bool bDryRun = ExtractOptionalBool(Params, TEXT("dry_run"), false);

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass || !GenClass->IsChildOf(AActor::StaticClass()))
	{
		return FMCPToolResult::Error(TEXT("Blueprint must be an Actor subclass"));
	}

	AActor* CDO = GenClass->GetDefaultObject<AActor>();
	if (!CDO)
	{
		return FMCPToolResult::Error(TEXT("Could not get CDO"));
	}

	TArray<TSharedPtr<FJsonValue>> Actions;
	int32 UpdatedCount = 0;
	int32 UnchangedCount = 0;
	int32 FailedCount = 0;

	auto AddAction = [&](const FString& Name, const FString& Status, const FString& Detail = FString())
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("action"), Name);
		Obj->SetStringField(TEXT("status"), Status);
		if (!Detail.IsEmpty())
		{
			Obj->SetStringField(TEXT("detail"), Detail);
		}
		Actions.Add(MakeShared<FJsonValueObject>(Obj));

		if (Status == TEXT("updated")) UpdatedCount++;
		else if (Status == TEXT("unchanged") || Status == TEXT("would_be_unchanged")) UnchangedCount++;
		else if (Status == TEXT("failed")) FailedCount++;
		else if (Status == TEXT("would_update")) {} // dry_run, no count
	};

	// Actor-level replication config
	bool bVal;
	if (Params->TryGetBoolField(TEXT("replicates"), bVal))
	{
		bool bOld = CDO->GetIsReplicated();
		if (bOld != bVal)
		{
			if (!bDryRun) CDO->SetReplicates(bVal);
			AddAction(TEXT("set_replicates"), bDryRun ? TEXT("would_update") : TEXT("updated"),
				FString::Printf(TEXT("%s -> %s"), bOld ? TEXT("true") : TEXT("false"), bVal ? TEXT("true") : TEXT("false")));
		}
		else
		{
			AddAction(TEXT("set_replicates"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"));
		}
	}

	double NumVal;
	if (Params->TryGetNumberField(TEXT("net_update_frequency"), NumVal))
	{
		float OldVal = CDO->GetNetUpdateFrequency();
		if (!FMath::IsNearlyEqual(OldVal, static_cast<float>(NumVal)))
		{
			if (!bDryRun) CDO->SetNetUpdateFrequency(static_cast<float>(NumVal));
			AddAction(TEXT("set_net_update_frequency"), bDryRun ? TEXT("would_update") : TEXT("updated"),
				FString::Printf(TEXT("%s -> %s"), *FString::SanitizeFloat(OldVal), *FString::SanitizeFloat(NumVal)));
		}
		else
		{
			AddAction(TEXT("set_net_update_frequency"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"));
		}
	}

	if (Params->TryGetBoolField(TEXT("always_relevant"), bVal))
	{
		bool bOld = CDO->bAlwaysRelevant != 0;
		if (bOld != bVal)
		{
			if (!bDryRun) CDO->bAlwaysRelevant = bVal ? 1 : 0;
			AddAction(TEXT("set_always_relevant"), bDryRun ? TEXT("would_update") : TEXT("updated"),
				FString::Printf(TEXT("%s -> %s"), bOld ? TEXT("true") : TEXT("false"), bVal ? TEXT("true") : TEXT("false")));
		}
		else
		{
			AddAction(TEXT("set_always_relevant"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"));
		}
	}

	// Per-variable replication
	const TArray<TSharedPtr<FJsonValue>>* VarArray = nullptr;
	if (Params->TryGetArrayField(TEXT("variables"), VarArray) && VarArray)
	{
		for (const TSharedPtr<FJsonValue>& VarVal : *VarArray)
		{
			const TSharedPtr<FJsonObject>* VarObj = nullptr;
			if (!VarVal->TryGetObject(VarObj) || !VarObj || !(*VarObj).IsValid())
			{
				FailedCount++;
				continue;
			}

			FString VarName;
			if (!(*VarObj)->TryGetStringField(TEXT("name"), VarName) || VarName.IsEmpty())
			{
				AddAction(TEXT("set_variable_replication"), TEXT("failed"), TEXT("missing variable name"));
				continue;
			}

			// Find variable
			FBPVariableDescription* FoundVar = nullptr;
			for (FBPVariableDescription& Var : BP->NewVariables)
			{
				if (Var.VarName.ToString() == VarName)
				{
					FoundVar = &Var;
					break;
				}
			}

			if (!FoundVar)
			{
				AddAction(FString::Printf(TEXT("set_%s_replication"), *VarName), TEXT("failed"),
					FString::Printf(TEXT("Variable '%s' not found"), *VarName));
				continue;
			}

			bool bAnyChange = false;

			// Replicated
			bool bSetRep;
			if ((*VarObj)->TryGetBoolField(TEXT("replicated"), bSetRep))
			{
				bool bCurRep = (FoundVar->PropertyFlags & CPF_Net) != 0;
				if (bCurRep != bSetRep)
				{
					if (!bDryRun)
					{
						if (bSetRep)
							FoundVar->PropertyFlags |= CPF_Net;
						else
						{
							FoundVar->PropertyFlags &= ~CPF_Net;
							FoundVar->PropertyFlags &= ~CPF_RepNotify;
							FoundVar->RepNotifyFunc = NAME_None;
						}
					}
					bAnyChange = true;
				}
			}

			// RepNotify
			bool bSetNotify;
			if ((*VarObj)->TryGetBoolField(TEXT("rep_notify"), bSetNotify))
			{
				bool bCurNotify = (FoundVar->PropertyFlags & CPF_RepNotify) != 0;
				if (bCurNotify != bSetNotify)
				{
					if (!bDryRun)
					{
						if (bSetNotify)
						{
							FoundVar->PropertyFlags |= CPF_Net | CPF_RepNotify;
							if (FoundVar->RepNotifyFunc.IsNone())
							{
								FoundVar->RepNotifyFunc = FName(*FString::Printf(TEXT("OnRep_%s"), *VarName));
							}
						}
						else
						{
							FoundVar->PropertyFlags &= ~CPF_RepNotify;
							FoundVar->RepNotifyFunc = NAME_None;
						}
					}
					bAnyChange = true;
				}
			}

			// Replication condition
			FString CondStr;
			if ((*VarObj)->TryGetStringField(TEXT("replication_condition"), CondStr) && !CondStr.IsEmpty())
			{
				ELifetimeCondition NewCond = COND_None;
				bool bValidCond = true;
				if (CondStr == TEXT("None")) NewCond = COND_None;
				else if (CondStr == TEXT("InitialOnly")) NewCond = COND_InitialOnly;
				else if (CondStr == TEXT("OwnerOnly")) NewCond = COND_OwnerOnly;
				else if (CondStr == TEXT("SkipOwner")) NewCond = COND_SkipOwner;
				else if (CondStr == TEXT("SimulatedOnly")) NewCond = COND_SimulatedOnly;
				else if (CondStr == TEXT("AutonomousOnly")) NewCond = COND_AutonomousOnly;
				else if (CondStr == TEXT("Custom")) NewCond = COND_Custom;
				else if (CondStr == TEXT("Never")) NewCond = COND_Never;
				else bValidCond = false;

				if (bValidCond && FoundVar->ReplicationCondition != NewCond)
				{
					if (!bDryRun) FoundVar->ReplicationCondition = NewCond;
					bAnyChange = true;
				}
			}

			if (bAnyChange)
			{
				AddAction(FString::Printf(TEXT("set_%s_replication"), *VarName),
					bDryRun ? TEXT("would_update") : TEXT("updated"));
			}
			else
			{
				AddAction(FString::Printf(TEXT("set_%s_replication"), *VarName),
					bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"));
			}
		}
	}

	if (!bDryRun && UpdatedCount > 0)
	{
		CDO->MarkPackageDirty();
		BP->MarkPackageDirty();
	}

	bool bAllFailed = FailedCount > 0 && UpdatedCount == 0 && UnchangedCount == 0;
	bool bPartialSuccess = FailedCount > 0 && (UpdatedCount > 0 || UnchangedCount > 0);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetArrayField(TEXT("actions"), Actions);
	Data->SetNumberField(TEXT("action_count"), Actions.Num());
	Data->SetNumberField(TEXT("updated_count"), UpdatedCount);
	Data->SetNumberField(TEXT("unchanged_count"), UnchangedCount);
	Data->SetNumberField(TEXT("failed_count"), FailedCount);

	if (bPartialSuccess)
	{
		Data->SetBoolField(TEXT("partial_success"), true);
	}

	// Emit receipt
	if (!bDryRun)
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("multiplayer");
		Receipt.Summary = TEXT("configure_multiplayer_actor");
		Receipt.bSuccess = !bAllFailed;
		Receipt.bPartialSuccess = bPartialSuccess;
		Receipt.Targets.Add(BPPath);
		Receipt.Classification = TEXT("user_mutation");
		if (bAllFailed)
		{
			Receipt.ErrorOrDenialReason = FString::Printf(TEXT("All %d actions failed"), FailedCount);
		}
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
	}

	FString Message = FString::Printf(TEXT("%s configure_multiplayer_actor: %d actions (%d updated, %d unchanged, %d failed)"),
		bDryRun ? TEXT("[dry_run]") : TEXT("Applied"), Actions.Num(), UpdatedCount, UnchangedCount, FailedCount);

	if (bAllFailed && !bDryRun)
	{
		return FMCPToolResult::Error(Message);
	}

	return FMCPToolResult::Success(Message, Data);
}

// ===== Query: Get Component Replication =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetComponentReplication(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);

	TArray<TSharedPtr<FJsonValue>> Components;

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (SCS)
	{
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node || !Node->ComponentTemplate) continue;

			UActorComponent* Template = Node->ComponentTemplate;
			TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
			CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			CompObj->SetStringField(TEXT("class"), Template->GetClass()->GetName());
			CompObj->SetBoolField(TEXT("is_replicated"), Template->GetIsReplicated());

			// Check if it's a scene component with additional network properties
			if (USceneComponent* SceneComp = Cast<USceneComponent>(Template))
			{
				CompObj->SetBoolField(TEXT("is_scene_component"), true);
			}

			Components.Add(MakeShared<FJsonValueObject>(CompObj));
		}
	}

	Data->SetArrayField(TEXT("components"), Components);
	Data->SetNumberField(TEXT("component_count"), Components.Num());

	int32 ReplicatedCount = 0;
	for (const auto& Comp : Components)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Comp->TryGetObject(Obj) && Obj)
		{
			bool bRep = false;
			if ((*Obj)->TryGetBoolField(TEXT("is_replicated"), bRep) && bRep)
			{
				ReplicatedCount++;
			}
		}
	}
	Data->SetNumberField(TEXT("replicated_component_count"), ReplicatedCount);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Component replication for %s: %d components (%d replicated)"),
			*BPPath, Components.Num(), ReplicatedCount),
		Data);
}

// ===== Query: Get Replication Graph =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetReplicationGraph(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		return FMCPToolResult::Error(TEXT("Blueprint has no generated class"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetStringField(TEXT("class_name"), GenClass->GetName());
	Data->SetStringField(TEXT("framework_role"), FrameworkRoleForClass(GenClass));

	// Actor settings
	bool bIsActor = GenClass->IsChildOf(AActor::StaticClass());
	TSharedPtr<FJsonObject> ActorSettings = MakeShared<FJsonObject>();
	if (bIsActor)
	{
		AActor* CDO = GenClass->GetDefaultObject<AActor>();
		if (CDO)
		{
			ActorSettings->SetBoolField(TEXT("replicates"), CDO->GetIsReplicated());
			ActorSettings->SetNumberField(TEXT("net_update_frequency"), CDO->GetNetUpdateFrequency());
			ActorSettings->SetNumberField(TEXT("net_priority"), CDO->NetPriority);
			ActorSettings->SetBoolField(TEXT("always_relevant"), CDO->bAlwaysRelevant != 0);
			ActorSettings->SetBoolField(TEXT("only_relevant_to_owner"), CDO->bOnlyRelevantToOwner != 0);
		}
	}
	Data->SetObjectField(TEXT("actor_settings"), ActorSettings);

	// Replicated variables
	TArray<TSharedPtr<FJsonValue>> RepVars;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (!(Var.PropertyFlags & CPF_Net)) continue;

		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
		VarObj->SetBoolField(TEXT("rep_notify"), (Var.PropertyFlags & CPF_RepNotify) != 0);
		if (Var.PropertyFlags & CPF_RepNotify)
		{
			VarObj->SetStringField(TEXT("notify_func"), Var.RepNotifyFunc.ToString());
		}
		VarObj->SetStringField(TEXT("condition"), ReplicationConditionToString(static_cast<uint8>(Var.ReplicationCondition)));
		RepVars.Add(MakeShared<FJsonValueObject>(VarObj));
	}
	Data->SetArrayField(TEXT("replicated_variables"), RepVars);

	// Replicated components
	TArray<TSharedPtr<FJsonValue>> RepComps;
	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (SCS)
	{
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node || !Node->ComponentTemplate) continue;
			if (!Node->ComponentTemplate->GetIsReplicated()) continue;

			TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
			CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			CompObj->SetStringField(TEXT("class"), Node->ComponentTemplate->GetClass()->GetName());
			RepComps.Add(MakeShared<FJsonValueObject>(CompObj));
		}
	}
	Data->SetArrayField(TEXT("replicated_components"), RepComps);

	// RPC functions
	TArray<TSharedPtr<FJsonValue>> Rpcs;
	for (TFieldIterator<UFunction> FuncIt(GenClass); FuncIt; ++FuncIt)
	{
		UFunction* Func = *FuncIt;
		if (!(Func->FunctionFlags & FUNC_Net)) continue;

		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), Func->GetName());
		FuncObj->SetStringField(TEXT("rpc_type"), RpcTypeToString(Func->FunctionFlags));
		FuncObj->SetBoolField(TEXT("local"), Func->GetOwnerClass() == GenClass);
		Rpcs.Add(MakeShared<FJsonValueObject>(FuncObj));
	}
	Data->SetArrayField(TEXT("rpc_functions"), Rpcs);

	// Summary counts
	Data->SetNumberField(TEXT("replicated_var_count"), RepVars.Num());
	Data->SetNumberField(TEXT("replicated_component_count"), RepComps.Num());
	Data->SetNumberField(TEXT("rpc_count"), Rpcs.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Replication graph for %s: %d vars, %d components, %d RPCs replicated"),
			*GenClass->GetName(), RepVars.Num(), RepComps.Num(), Rpcs.Num()),
		Data);
}

// ===== Query: Classify Multiplayer Actor =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteClassifyMultiplayerActor(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		return FMCPToolResult::Error(TEXT("Blueprint has no generated class"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetStringField(TEXT("class_name"), GenClass->GetName());

	FString Role = FrameworkRoleForClass(GenClass);
	Data->SetStringField(TEXT("framework_role"), Role);

	// Classification
	TArray<TSharedPtr<FJsonValue>> Classifications;
	TArray<TSharedPtr<FJsonValue>> Guidance;

	bool bIsActor = GenClass->IsChildOf(AActor::StaticClass());
	bool bReplicates = false;
	bool bOwnerOnly = false;

	if (bIsActor)
	{
		AActor* CDO = GenClass->GetDefaultObject<AActor>();
		if (CDO)
		{
			bReplicates = CDO->GetIsReplicated();
			bOwnerOnly = CDO->bOnlyRelevantToOwner != 0;
		}
	}

	// Count replicated state
	int32 RepVarCount = 0;
	int32 RepNotifyCount = 0;
	int32 OwnerOnlyVarCount = 0;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.PropertyFlags & CPF_Net)
		{
			RepVarCount++;
			if (Var.PropertyFlags & CPF_RepNotify) RepNotifyCount++;
			if (Var.ReplicationCondition == COND_OwnerOnly) OwnerOnlyVarCount++;
		}
	}

	int32 ServerRpcCount = 0, ClientRpcCount = 0, MulticastRpcCount = 0;
	for (TFieldIterator<UFunction> FuncIt(GenClass); FuncIt; ++FuncIt)
	{
		EFunctionFlags Flags = (*FuncIt)->FunctionFlags;
		if (!(Flags & FUNC_Net)) continue;
		if (Flags & FUNC_NetServer) ServerRpcCount++;
		else if (Flags & FUNC_NetClient) ClientRpcCount++;
		else if (Flags & FUNC_NetMulticast) MulticastRpcCount++;
	}

	// Determine classification
	if (GenClass->IsChildOf(AGameModeBase::StaticClass()))
	{
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("server-authority-only")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("GameMode exists only on server. All state here is authoritative. Cannot be read by clients.")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Use GameState for any data clients need to see.")));
	}
	else if (GenClass->IsChildOf(AGameStateBase::StaticClass()))
	{
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("server-authoritative-replicated-to-all")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("GameState is replicated to all clients. Good for: match score, timer, player list.")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Mutations should happen on server only — clients see replicated values.")));
	}
	else if (GenClass->IsChildOf(APlayerState::StaticClass()))
	{
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("server-authoritative-replicated-per-player")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("PlayerState is replicated and survives seamless travel. Best for: player name, score, team, persistent stats.")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("All clients can see all PlayerStates — do not store private data here.")));
	}
	else if (GenClass->IsChildOf(APlayerController::StaticClass()))
	{
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("server-authoritative-owner-only")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("PlayerController is replicated to owning client only. Owns the network connection.")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Server RPCs from this controller are legal. Other clients cannot see it.")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Good for: input, UI state, client-specific settings.")));
	}
	else if (GenClass->IsChildOf(ACharacter::StaticClass()) || GenClass->IsChildOf(APawn::StaticClass()))
	{
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("server-authoritative-replicated-possessed")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Pawn/Character is replicated. Owning client = autonomous proxy (full prediction). Others = simulated proxy.")));
		if (ServerRpcCount > 0)
		{
			Guidance.Add(MakeShared<FJsonValueString>(TEXT("Server RPCs are legal from owning client via possessing PlayerController.")));
		}
		if (RepVarCount > 0 && RepNotifyCount == 0)
		{
			Guidance.Add(MakeShared<FJsonValueString>(TEXT("Consider RepNotify on gameplay-critical replicated vars for client-side response to state changes.")));
		}
	}
	else if (bReplicates)
	{
		if (bOwnerOnly)
		{
			Classifications.Add(MakeShared<FJsonValueString>(TEXT("server-authoritative-owner-only")));
		}
		else
		{
			Classifications.Add(MakeShared<FJsonValueString>(TEXT("server-authoritative-replicated")));
		}
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Replicated actor. Server is authoritative. State changes on server replicate to clients.")));
	}
	else
	{
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("local-only")));
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Actor does not replicate. Exists independently on each machine. Good for: cosmetic effects, local UI actors.")));
		if (RepVarCount > 0)
		{
			Guidance.Add(MakeShared<FJsonValueString>(TEXT("WARNING: Has replicated variables but bReplicates=false — replication will not work.")));
		}
	}

	// Cosmetic vs authoritative split guidance
	if (bReplicates && RepVarCount > 0)
	{
		TSharedPtr<FJsonObject> Split = MakeShared<FJsonObject>();
		Split->SetStringField(TEXT("authoritative_state"), FString::Printf(TEXT("%d replicated variables (mutate on server only)"), RepVarCount));
		if (OwnerOnlyVarCount > 0)
		{
			Split->SetStringField(TEXT("owner_only_state"), FString::Printf(TEXT("%d owner-only variables (visible only to owning client)"), OwnerOnlyVarCount));
		}
		Split->SetStringField(TEXT("cosmetic_state"), TEXT("Non-replicated variables are cosmetic/local — safe for VFX, sounds, UI feedback"));
		Data->SetObjectField(TEXT("authority_cosmetic_split"), Split);
	}

	Data->SetArrayField(TEXT("classifications"), Classifications);
	Data->SetArrayField(TEXT("guidance"), Guidance);

	// Stats
	TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
	Stats->SetNumberField(TEXT("replicated_vars"), RepVarCount);
	Stats->SetNumberField(TEXT("rep_notify_vars"), RepNotifyCount);
	Stats->SetNumberField(TEXT("owner_only_vars"), OwnerOnlyVarCount);
	Stats->SetNumberField(TEXT("server_rpcs"), ServerRpcCount);
	Stats->SetNumberField(TEXT("client_rpcs"), ClientRpcCount);
	Stats->SetNumberField(TEXT("multicast_rpcs"), MulticastRpcCount);
	Data->SetObjectField(TEXT("stats"), Stats);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Classification for %s: %s"), *GenClass->GetName(),
			Classifications.Num() > 0 ? *Classifications[0]->AsString() : TEXT("unknown")),
		Data);
}

// ===== Query: Get Subobject Replication =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetSubobjectReplication(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass || !GenClass->IsChildOf(AActor::StaticClass()))
	{
		return FMCPToolResult::Error(TEXT("Blueprint must be an Actor subclass"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetStringField(TEXT("class_name"), GenClass->GetName());

	// Check actor-level subobject replication mode
	AActor* CDO = GenClass->GetDefaultObject<AActor>();
	bool bActorReplicates = CDO ? CDO->GetIsReplicated() : false;
	Data->SetBoolField(TEXT("actor_replicates"), bActorReplicates);

	// Inspect SCS components for subobject replication
	TArray<TSharedPtr<FJsonValue>> Components;
	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (SCS)
	{
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node || !Node->ComponentTemplate) continue;

			UActorComponent* Template = Node->ComponentTemplate;
			TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
			CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			CompObj->SetStringField(TEXT("class"), Template->GetClass()->GetName());
			CompObj->SetBoolField(TEXT("is_replicated"), Template->GetIsReplicated());
			CompObj->SetBoolField(TEXT("uses_registered_subobject_list"), Template->IsUsingRegisteredSubObjectList());

			// Subobject replication guidance
			TArray<TSharedPtr<FJsonValue>> CompGuidance;
			if (Template->GetIsReplicated() && !bActorReplicates)
			{
				CompGuidance.Add(MakeShared<FJsonValueString>(TEXT("WARNING: Component replicates but actor does not — subobject replication will not work")));
			}
			if (Template->IsUsingRegisteredSubObjectList())
			{
				CompGuidance.Add(MakeShared<FJsonValueString>(TEXT("Uses registered subobject list — subobjects must be explicitly registered via AddReplicatedSubObject()")));
			}
			else if (Template->GetIsReplicated())
			{
				CompGuidance.Add(MakeShared<FJsonValueString>(TEXT("Uses virtual ReplicateSubobjects() — C++ override needed for custom subobject replication")));
			}
			CompObj->SetArrayField(TEXT("guidance"), CompGuidance);

			Components.Add(MakeShared<FJsonValueObject>(CompObj));
		}
	}
	Data->SetArrayField(TEXT("components"), Components);

	// General subobject replication guidance
	TSharedPtr<FJsonObject> SubobjectGuide = MakeShared<FJsonObject>();
	SubobjectGuide->SetStringField(TEXT("what_are_subobjects"), TEXT("UObjects owned by an actor (components, instanced UObjects) that replicate alongside the actor"));
	SubobjectGuide->SetStringField(TEXT("registration_pattern"), TEXT("Owner calls AddReplicatedSubObject(SubObj, Condition) in ReadyForReplication() or BeginPlay()"));
	SubobjectGuide->SetStringField(TEXT("c_plus_plus_note"), TEXT("Subobject registration is C++ API. Blueprint cannot directly register subobjects — use C++ base class."));
	SubobjectGuide->SetStringField(TEXT("fast_array_note"), TEXT("FFastArraySerializer (e.g., FGameplayAbilitySpec arrays in GAS) replicates array elements efficiently with per-element callbacks. Detection requires C++ class inspection — not directly visible from BP variables."));
	SubobjectGuide->SetStringField(TEXT("object_reference_safety"), TEXT("Replicated UObject* references must point to stably-named/replicated objects. Pointers to local-only or transient objects will be null on clients."));
	Data->SetObjectField(TEXT("subobject_guide"), SubobjectGuide);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Subobject replication for %s: %d components inspected"), *GenClass->GetName(), Components.Num()),
		Data);
}

// ===== Query: Audit Object References =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteAuditObjectReferences(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		return FMCPToolResult::Error(TEXT("Blueprint has no generated class"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetStringField(TEXT("class_name"), GenClass->GetName());

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> ObjectRefVars;

	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		FString PinCategory = Var.VarType.PinCategory.ToString();
		bool bIsObjectRef = PinCategory == TEXT("object") || PinCategory == TEXT("class") ||
			PinCategory == TEXT("softobject") || PinCategory == TEXT("softclass") ||
			PinCategory == TEXT("interface");
		bool bReplicated = (Var.PropertyFlags & CPF_Net) != 0;

		if (!bIsObjectRef) continue;

		TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
		RefObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		RefObj->SetStringField(TEXT("pin_category"), PinCategory);
		RefObj->SetStringField(TEXT("pin_subcategory"), Var.VarType.PinSubCategoryObject.IsValid()
			? Var.VarType.PinSubCategoryObject->GetName() : TEXT("(none)"));
		RefObj->SetBoolField(TEXT("replicated"), bReplicated);

		// Classify safety
		FString Safety;
		TArray<TSharedPtr<FJsonValue>> RefGuidance;

		if (PinCategory == TEXT("softobject") || PinCategory == TEXT("softclass"))
		{
			Safety = TEXT("safe");
			RefGuidance.Add(MakeShared<FJsonValueString>(TEXT("Soft references are path-based — safe for replication (resolved on each machine independently)")));
		}
		else if (bReplicated)
		{
			Safety = TEXT("requires_care");
			RefGuidance.Add(MakeShared<FJsonValueString>(TEXT("Replicated hard UObject reference — target must be a replicated/stably-named object. Local-only objects will be null on clients.")));
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Variable '%s' is a replicated object reference (%s) — ensure target is replicated or stably-named"),
					*Var.VarName.ToString(), *PinCategory)));
		}
		else
		{
			Safety = TEXT("local_only");
			RefGuidance.Add(MakeShared<FJsonValueString>(TEXT("Non-replicated object reference — local only, safe for cosmetic/client-side usage")));
		}

		RefObj->SetStringField(TEXT("multiplayer_safety"), Safety);
		RefObj->SetArrayField(TEXT("guidance"), RefGuidance);
		ObjectRefVars.Add(MakeShared<FJsonValueObject>(RefObj));
	}

	Data->SetArrayField(TEXT("object_reference_variables"), ObjectRefVars);
	Data->SetNumberField(TEXT("object_ref_count"), ObjectRefVars.Num());
	Data->SetArrayField(TEXT("warnings"), Warnings);
	Data->SetNumberField(TEXT("warning_count"), Warnings.Num());

	// General object reference guidance
	TSharedPtr<FJsonObject> RefGuide = MakeShared<FJsonObject>();
	RefGuide->SetStringField(TEXT("safe_patterns"), TEXT("Soft references (TSoftObjectPtr/TSoftClassPtr), replicated actors, stably-named subobjects, class references"));
	RefGuide->SetStringField(TEXT("unsafe_patterns"), TEXT("Hard UObject* to local-only actors, transient objects, dynamically spawned non-replicated objects"));
	RefGuide->SetStringField(TEXT("guidance"), TEXT("For multiplayer: prefer soft references or ensure targets replicate. Hard refs to local objects will be null on remote machines."));
	Data->SetObjectField(TEXT("reference_safety_guide"), RefGuide);

	FMCPToolResult Result = FMCPToolResult::Success(
		FString::Printf(TEXT("Object reference audit for %s: %d refs found, %d warnings"), *GenClass->GetName(), ObjectRefVars.Num(), Warnings.Num()),
		Data);

	return MaybeAttachAuditReportArtifact(
		Result,
		TEXT("audit_object_references"),
		BPPath,
		ExtractOptionalBool(Params, TEXT("export_report"), false),
		ExtractOptionalString(Params, TEXT("report_name")),
		ExtractOptionalString(Params, TEXT("report_slug")));
}

// ===== Query: Get Travel Contracts =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetTravelContracts(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath = ExtractOptionalString(Params, TEXT("blueprint_path"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// Per-class travel survival matrix
	TArray<TSharedPtr<FJsonValue>> Contracts;

	auto AddContract = [&](const FString& ClassName, const FString& SeamlessSurvival, const FString& NonSeamlessSurvival,
		const FString& ServerOnly, const FString& ReplicatedTo, const FString& PersistenceAdvice)
	{
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("class"), ClassName);
		C->SetStringField(TEXT("seamless_travel"), SeamlessSurvival);
		C->SetStringField(TEXT("non_seamless_travel"), NonSeamlessSurvival);
		C->SetStringField(TEXT("server_only"), ServerOnly);
		C->SetStringField(TEXT("replicated_to"), ReplicatedTo);
		C->SetStringField(TEXT("persistence_advice"), PersistenceAdvice);
		Contracts.Add(MakeShared<FJsonValueObject>(C));
	};

	AddContract(TEXT("GameInstance"),
		TEXT("SURVIVES — persists across all travel types"),
		TEXT("SURVIVES — persists across all travel types"),
		TEXT("No — exists on both server and clients"),
		TEXT("Not replicated (local per-machine)"),
		TEXT("Best place for: matchmaking state, player preferences, cross-level progression tracking. Reset manually if needed."));

	AddContract(TEXT("GameMode"),
		TEXT("DESTROYED — new GameMode created in destination level"),
		TEXT("DESTROYED — new GameMode created in destination level"),
		TEXT("Yes — server only, never on clients"),
		TEXT("Not replicated"),
		TEXT("Do NOT store persistent data here. Use GameState for replicated match data, GameInstance for server-local persistence."));

	AddContract(TEXT("GameState"),
		TEXT("DESTROYED — new GameState created in destination level"),
		TEXT("DESTROYED — new GameState created in destination level"),
		TEXT("No — exists on server, replicated to all clients"),
		TEXT("All clients"),
		TEXT("Current-level match data only. For cross-level data: copy to GameInstance before travel, restore after."));

	AddContract(TEXT("PlayerController"),
		TEXT("SURVIVES — same controller persists"),
		TEXT("DESTROYED — new controller created on reconnect"),
		TEXT("No — exists on server + owning client"),
		TEXT("Owning client only"),
		TEXT("Survives seamless travel. Good for: input settings, UI state. NOT visible to other players."));

	AddContract(TEXT("PlayerState"),
		TEXT("SURVIVES — persists and re-associates with controller"),
		TEXT("DESTROYED — new PlayerState on reconnect"),
		TEXT("No — exists on server, replicated to all"),
		TEXT("All clients"),
		TEXT("BEST place for per-player data that must survive seamless travel AND be visible to all: name, score, team, stats."));

	AddContract(TEXT("Pawn / Character"),
		TEXT("DESTROYED — new pawn spawned and possessed in destination"),
		TEXT("DESTROYED — new pawn spawned on reconnect"),
		TEXT("No — replicated"),
		TEXT("All clients (autonomous proxy on owner, simulated on others)"),
		TEXT("Never store persistent data on Pawn. It is destroyed on every travel. Use PlayerState for persistent per-player data."));

	AddContract(TEXT("Level Actors"),
		TEXT("DESTROYED — belong to the old level"),
		TEXT("DESTROYED — belong to the old level"),
		TEXT("Depends on actor"),
		TEXT("Depends on actor bReplicates"),
		TEXT("Level actors are destroyed when their level unloads. For persistent world objects: use GameState or a dedicated save system."));

	Data->SetArrayField(TEXT("travel_contracts"), Contracts);

	// If a specific BP was provided, classify it — error on bad path
	if (!BPPath.IsEmpty())
	{
		FString LoadError;
		UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
		if (!BP)
		{
			return FMCPToolResult::Error(LoadError);
		}
		if (!BP->GeneratedClass)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Blueprint has no generated class: %s"), *BPPath));
		}
		{
			UClass* GenClass = BP->GeneratedClass;
			TSharedPtr<FJsonObject> BPContract = MakeShared<FJsonObject>();
			BPContract->SetStringField(TEXT("blueprint_path"), BPPath);
			BPContract->SetStringField(TEXT("class"), GenClass->GetName());
			BPContract->SetStringField(TEXT("framework_role"), FrameworkRoleForClass(GenClass));

			if (GenClass->IsChildOf(AGameModeBase::StaticClass()))
			{
				BPContract->SetStringField(TEXT("travel_survival"), TEXT("DESTROYED on all travel types"));
				BPContract->SetStringField(TEXT("advice"), TEXT("Do NOT store persistent data. Use GameState or GameInstance."));
			}
			else if (GenClass->IsChildOf(AGameStateBase::StaticClass()))
			{
				BPContract->SetStringField(TEXT("travel_survival"), TEXT("DESTROYED on all travel types"));
				BPContract->SetStringField(TEXT("advice"), TEXT("Copy data to GameInstance before travel if needed post-travel."));
			}
			else if (GenClass->IsChildOf(APlayerState::StaticClass()))
			{
				BPContract->SetStringField(TEXT("travel_survival"), TEXT("SURVIVES seamless, DESTROYED on non-seamless"));
				BPContract->SetStringField(TEXT("advice"), TEXT("Best place for per-player persistent data visible to all clients."));
			}
			else if (GenClass->IsChildOf(APlayerController::StaticClass()))
			{
				BPContract->SetStringField(TEXT("travel_survival"), TEXT("SURVIVES seamless, DESTROYED on non-seamless"));
				BPContract->SetStringField(TEXT("advice"), TEXT("Good for input/UI state. Not visible to other clients."));
			}
			else if (GenClass->IsChildOf(APawn::StaticClass()))
			{
				BPContract->SetStringField(TEXT("travel_survival"), TEXT("DESTROYED on all travel types"));
				BPContract->SetStringField(TEXT("advice"), TEXT("Never store persistent data on Pawn. Use PlayerState."));
			}
			else
			{
				BPContract->SetStringField(TEXT("travel_survival"), TEXT("DESTROYED (level actor)"));
				BPContract->SetStringField(TEXT("advice"), TEXT("Level actors are destroyed when their level unloads."));
			}
			Data->SetObjectField(TEXT("blueprint_contract"), BPContract);
		}
	}

	// Session contracts
	TSharedPtr<FJsonObject> SessionContracts = MakeShared<FJsonObject>();
	SessionContracts->SetStringField(TEXT("overview"), TEXT("Sessions managed via IOnlineSubsystem→IOnlineSession. Config in DefaultEngine.ini [OnlineSubsystem]."));
	SessionContracts->SetStringField(TEXT("lifecycle"), TEXT("Create → Start → (players join/leave) → End → Destroy"));
	SessionContracts->SetStringField(TEXT("server_travel_effect"), TEXT("ServerTravel preserves the session. All connected clients follow to new level."));
	SessionContracts->SetStringField(TEXT("non_seamless_effect"), TEXT("Non-seamless travel disconnects all clients. Session may need to be re-created or clients must rejoin."));
	SessionContracts->SetStringField(TEXT("seamless_effect"), TEXT("Seamless travel keeps clients connected. Session persists. PlayerController + PlayerState survive."));
	SessionContracts->SetStringField(TEXT("dedicated_server_note"), TEXT("Dedicated server: GameMode is the session host. Clients connect via IP/session search. Travel initiated by server."));
	SessionContracts->SetStringField(TEXT("listen_server_note"), TEXT("Listen server: one player hosts. If host disconnects, session is lost. Consider host migration strategy."));
	Data->SetObjectField(TEXT("session_contracts"), SessionContracts);

	return FMCPToolResult::Success(TEXT("Travel and session contracts"), Data);
}

// ===== Query: Get Network State (Live Runtime) =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetNetworkState(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	TOptional<FMCPToolResult> ContextError = ValidateEditorContext(World);
	if (ContextError.IsSet())
	{
		return ContextError.GetValue();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// Net mode
	FString NetModeStr;
	ENetMode NetMode = World->GetNetMode();
	switch (NetMode)
	{
	case NM_Standalone: NetModeStr = TEXT("Standalone"); break;
	case NM_DedicatedServer: NetModeStr = TEXT("DedicatedServer"); break;
	case NM_ListenServer: NetModeStr = TEXT("ListenServer"); break;
	case NM_Client: NetModeStr = TEXT("Client"); break;
	default: NetModeStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("net_mode"), NetModeStr);
	Data->SetBoolField(TEXT("is_networked"), NetMode != NM_Standalone);
	Data->SetBoolField(TEXT("is_server"), NetMode == NM_DedicatedServer || NetMode == NM_ListenServer);
	Data->SetBoolField(TEXT("is_client"), NetMode == NM_Client);

	// World type
	FString WorldTypeStr;
	switch (World->WorldType)
	{
	case EWorldType::Editor: WorldTypeStr = TEXT("Editor"); break;
	case EWorldType::PIE: WorldTypeStr = TEXT("PIE"); break;
	case EWorldType::Game: WorldTypeStr = TEXT("Game"); break;
	case EWorldType::EditorPreview: WorldTypeStr = TEXT("EditorPreview"); break;
	default: WorldTypeStr = TEXT("Other"); break;
	}
	Data->SetStringField(TEXT("world_type"), WorldTypeStr);
	Data->SetBoolField(TEXT("is_pie"), World->WorldType == EWorldType::PIE);
	Data->SetBoolField(TEXT("is_editor"), World->WorldType == EWorldType::Editor);

	// Runtime availability
	bool bRuntimeAvailable = (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
	Data->SetBoolField(TEXT("runtime_state_available"), bRuntimeAvailable);
	if (!bRuntimeAvailable)
	{
		Data->SetStringField(TEXT("runtime_note"), TEXT("Editor world — runtime networking state not active. Start PIE to get live network data."));
	}

	// Player topology (best effort — works in PIE, minimal in editor)
	int32 PlayerControllerCount = 0;
	int32 PlayerStateCount = 0;
	int32 PawnCount = 0;
	int32 ReplicatedActorCount = 0;
	int32 TotalActorCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		TotalActorCount++;
		if (Actor->GetIsReplicated()) ReplicatedActorCount++;
		if (Actor->IsA(APlayerController::StaticClass())) PlayerControllerCount++;
		if (Actor->IsA(APlayerState::StaticClass())) PlayerStateCount++;
		if (Actor->IsA(APawn::StaticClass())) PawnCount++;
	}

	TSharedPtr<FJsonObject> Topology = MakeShared<FJsonObject>();
	Topology->SetNumberField(TEXT("player_controllers"), PlayerControllerCount);
	Topology->SetNumberField(TEXT("player_states"), PlayerStateCount);
	Topology->SetNumberField(TEXT("pawns"), PawnCount);
	Topology->SetNumberField(TEXT("replicated_actors"), ReplicatedActorCount);
	Topology->SetNumberField(TEXT("total_actors"), TotalActorCount);
	Data->SetObjectField(TEXT("topology"), Topology);

	// GameMode (server only)
	AGameModeBase* GM = World->GetAuthGameMode();
	if (GM)
	{
		Data->SetStringField(TEXT("game_mode_class"), GM->GetClass()->GetName());
		Data->SetBoolField(TEXT("game_mode_present"), true);
	}
	else
	{
		Data->SetBoolField(TEXT("game_mode_present"), false);
		if (NetMode == NM_Client)
		{
			Data->SetStringField(TEXT("game_mode_note"), TEXT("GameMode is server-only — not present on clients"));
		}
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Network state: %s, world=%s, %d replicated actors, %d PCs"),
			*NetModeStr, *WorldTypeStr, ReplicatedActorCount, PlayerControllerCount),
		Data);
}

// ===== Query: Get Live Actor Network =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetLiveActorNetwork(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	TOptional<FMCPToolResult> ContextError = ValidateEditorContext(World);
	if (ContextError.IsSet())
	{
		return ContextError.GetValue();
	}

	FString ClassFilter = ExtractOptionalString(Params, TEXT("class_filter"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	bool bReplicatedOnly = ExtractOptionalBool(Params, TEXT("replicated_only"), false);
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 50);
	Limit = FMath::Clamp(Limit, 1, 500);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// World context
	FString NetModeStr;
	switch (World->GetNetMode())
	{
	case NM_Standalone: NetModeStr = TEXT("Standalone"); break;
	case NM_DedicatedServer: NetModeStr = TEXT("DedicatedServer"); break;
	case NM_ListenServer: NetModeStr = TEXT("ListenServer"); break;
	case NM_Client: NetModeStr = TEXT("Client"); break;
	default: NetModeStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("net_mode"), NetModeStr);
	Data->SetBoolField(TEXT("is_runtime"), World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);

	TArray<TSharedPtr<FJsonValue>> Actors;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (Actors.Num() >= Limit) break;

		AActor* Actor = *It;

		if (bReplicatedOnly && !Actor->GetIsReplicated()) continue;

		FString ClassName = Actor->GetClass()->GetName();
		FString ActorName = Actor->GetName();

		if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter)) continue;
		if (!NameFilter.IsEmpty() && !ActorName.Contains(NameFilter) && !Actor->GetActorLabel().Contains(NameFilter)) continue;

		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), ActorName);
		ActorObj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		ActorObj->SetStringField(TEXT("class"), ClassName);
		ActorObj->SetStringField(TEXT("framework_role"), FrameworkRoleForClass(Actor->GetClass()));
		ActorObj->SetBoolField(TEXT("replicates"), Actor->GetIsReplicated());
		ActorObj->SetBoolField(TEXT("has_authority"), Actor->HasAuthority());

		// Role
		FString LocalRoleStr;
		switch (Actor->GetLocalRole())
		{
		case ROLE_Authority: LocalRoleStr = TEXT("Authority"); break;
		case ROLE_AutonomousProxy: LocalRoleStr = TEXT("AutonomousProxy"); break;
		case ROLE_SimulatedProxy: LocalRoleStr = TEXT("SimulatedProxy"); break;
		default: LocalRoleStr = TEXT("None"); break;
		}
		ActorObj->SetStringField(TEXT("local_role"), LocalRoleStr);

		FString RemoteRoleStr;
		switch (Actor->GetRemoteRole())
		{
		case ROLE_Authority: RemoteRoleStr = TEXT("Authority"); break;
		case ROLE_AutonomousProxy: RemoteRoleStr = TEXT("AutonomousProxy"); break;
		case ROLE_SimulatedProxy: RemoteRoleStr = TEXT("SimulatedProxy"); break;
		default: RemoteRoleStr = TEXT("None"); break;
		}
		ActorObj->SetStringField(TEXT("remote_role"), RemoteRoleStr);

		// Owner
		AActor* Owner = Actor->GetOwner();
		ActorObj->SetStringField(TEXT("owner"), Owner ? Owner->GetName() : TEXT("(none)"));

		ActorObj->SetBoolField(TEXT("always_relevant"), Actor->bAlwaysRelevant != 0);
		ActorObj->SetBoolField(TEXT("only_relevant_to_owner"), Actor->bOnlyRelevantToOwner != 0);

		Actors.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	Data->SetArrayField(TEXT("actors"), Actors);
	Data->SetNumberField(TEXT("count"), Actors.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Live actor network: %d actors in %s world"), Actors.Num(), *NetModeStr),
		Data);
}

// ===== Query: Audit Persistence Placement =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteAuditPersistencePlacement(const TSharedRef<FJsonObject>& Params)
{
	FString BPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BPPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByPath(BPPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		return FMCPToolResult::Error(TEXT("Blueprint has no generated class"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BPPath);
	Data->SetStringField(TEXT("class_name"), GenClass->GetName());

	FString Role = FrameworkRoleForClass(GenClass);
	Data->SetStringField(TEXT("framework_role"), Role);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Recommendations;

	// Gather variable stats
	int32 ReplicatedCount = 0;
	int32 RepNotifyCount = 0;
	int32 TotalVars = BP->NewVariables.Num();
	TArray<FString> ReplicatedVarNames;

	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.PropertyFlags & CPF_Net)
		{
			ReplicatedCount++;
			ReplicatedVarNames.Add(Var.VarName.ToString());
			if (Var.PropertyFlags & CPF_RepNotify) RepNotifyCount++;
		}
	}

	Data->SetNumberField(TEXT("total_variables"), TotalVars);
	Data->SetNumberField(TEXT("replicated_variables"), ReplicatedCount);

	// Classify placement based on framework role
	TSharedPtr<FJsonObject> Placement = MakeShared<FJsonObject>();

	if (GenClass->IsChildOf(AGameModeBase::StaticClass()))
	{
		Placement->SetStringField(TEXT("visibility"), TEXT("server-only"));
		Placement->SetStringField(TEXT("travel_survival"), TEXT("DESTROYED on all travel"));
		Placement->SetStringField(TEXT("replication"), TEXT("NOT replicated"));

		if (ReplicatedCount > 0)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("GameMode has %d replicated variables but GameMode is server-only and NOT replicated. Clients will never see these values. Move to GameState."),
					ReplicatedCount)));
		}
		if (TotalVars > 0)
		{
			Recommendations.Add(MakeShared<FJsonValueString>(TEXT("GameMode data is destroyed on travel and invisible to clients. For persistent or shared data, use PlayerState (per-player) or GameState (match-wide).")));
		}
	}
	else if (GenClass->IsChildOf(AGameStateBase::StaticClass()))
	{
		Placement->SetStringField(TEXT("visibility"), TEXT("all clients"));
		Placement->SetStringField(TEXT("travel_survival"), TEXT("DESTROYED on all travel"));
		Placement->SetStringField(TEXT("replication"), TEXT("Replicated to all"));

		if (TotalVars > 0 && ReplicatedCount == 0)
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("GameState has variables but none are replicated. Clients won't see the data. Mark variables as Replicated.")));
		}
		Recommendations.Add(MakeShared<FJsonValueString>(TEXT("GameState is good for: match score, timer, game phase. Data is lost on travel — copy to GameInstance if needed across levels.")));
	}
	else if (GenClass->IsChildOf(APlayerState::StaticClass()))
	{
		Placement->SetStringField(TEXT("visibility"), TEXT("all clients"));
		Placement->SetStringField(TEXT("travel_survival"), TEXT("SURVIVES seamless travel"));
		Placement->SetStringField(TEXT("replication"), TEXT("Replicated to all"));

		Recommendations.Add(MakeShared<FJsonValueString>(TEXT("PlayerState is the BEST place for per-player persistent data visible to all: name, score, team, stats. Survives seamless travel.")));
		if (TotalVars > 0 && ReplicatedCount == 0)
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("PlayerState has variables but none are replicated. Other clients won't see player data.")));
		}
	}
	else if (GenClass->IsChildOf(APlayerController::StaticClass()))
	{
		Placement->SetStringField(TEXT("visibility"), TEXT("owning client only"));
		Placement->SetStringField(TEXT("travel_survival"), TEXT("SURVIVES seamless travel"));
		Placement->SetStringField(TEXT("replication"), TEXT("Owning client only"));

		if (ReplicatedCount > 0)
		{
			Recommendations.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("%d replicated variables — visible only to owning client. If other players need this data, move to PlayerState."),
					ReplicatedCount)));
		}
		Recommendations.Add(MakeShared<FJsonValueString>(TEXT("PlayerController is good for: input config, UI state, client-specific settings. NOT for data other players need.")));
	}
	else if (GenClass->IsChildOf(APawn::StaticClass()))
	{
		Placement->SetStringField(TEXT("visibility"), TEXT("all clients"));
		Placement->SetStringField(TEXT("travel_survival"), TEXT("DESTROYED on all travel"));
		Placement->SetStringField(TEXT("replication"), TEXT("Replicated to all"));

		if (ReplicatedCount > 0)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Pawn has %d replicated variables. Pawn is DESTROYED on travel. If this data must persist (stats, inventory), move to PlayerState."),
					ReplicatedCount)));
		}
		Recommendations.Add(MakeShared<FJsonValueString>(TEXT("Pawn is good for: health, current weapon, movement state. NOT for persistent data — use PlayerState.")));
	}
	else
	{
		bool bIsActor = GenClass->IsChildOf(AActor::StaticClass());
		Placement->SetStringField(TEXT("visibility"), bIsActor ? TEXT("depends on bReplicates") : TEXT("N/A"));
		Placement->SetStringField(TEXT("travel_survival"), TEXT("DESTROYED (level actor)"));
		Placement->SetStringField(TEXT("replication"), bIsActor ? TEXT("depends on bReplicates") : TEXT("N/A"));

		if (bIsActor && ReplicatedCount > 0)
		{
			Recommendations.Add(MakeShared<FJsonValueString>(TEXT("Level actor with replicated data — destroyed on travel. For persistent state, use GameState/PlayerState/GameInstance.")));
		}
	}

	Data->SetObjectField(TEXT("placement"), Placement);
	Data->SetArrayField(TEXT("warnings"), Warnings);
	Data->SetArrayField(TEXT("recommendations"), Recommendations);
	Data->SetNumberField(TEXT("warning_count"), Warnings.Num());

	FMCPToolResult Result = FMCPToolResult::Success(
		FString::Printf(TEXT("Persistence placement audit for %s: %s, %d warnings"),
			*GenClass->GetName(), *Role, Warnings.Num()),
		Data);

	return MaybeAttachAuditReportArtifact(
		Result,
		TEXT("audit_persistence_placement"),
		BPPath,
		ExtractOptionalBool(Params, TEXT("export_report"), false),
		ExtractOptionalString(Params, TEXT("report_name")),
		ExtractOptionalString(Params, TEXT("report_slug")));
}

// ===== Query: Get Session State (Live Runtime) =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetSessionState(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	TOptional<FMCPToolResult> ContextError = ValidateEditorContext(World);
	if (ContextError.IsSet())
	{
		return ContextError.GetValue();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// World context
	FString WorldTypeStr;
	switch (World->WorldType)
	{
	case EWorldType::Editor: WorldTypeStr = TEXT("Editor"); break;
	case EWorldType::PIE: WorldTypeStr = TEXT("PIE"); break;
	case EWorldType::Game: WorldTypeStr = TEXT("Game"); break;
	default: WorldTypeStr = TEXT("Other"); break;
	}
	Data->SetStringField(TEXT("world_type"), WorldTypeStr);

	FString NetModeStr;
	switch (World->GetNetMode())
	{
	case NM_Standalone: NetModeStr = TEXT("Standalone"); break;
	case NM_DedicatedServer: NetModeStr = TEXT("DedicatedServer"); break;
	case NM_ListenServer: NetModeStr = TEXT("ListenServer"); break;
	case NM_Client: NetModeStr = TEXT("Client"); break;
	default: NetModeStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("net_mode"), NetModeStr);

	// OnlineSubsystem availability
	FString OSSName = TEXT("unavailable");
	IOnlineSubsystem* OSS = IOnlineSubsystem::Get();
	if (OSS)
	{
		OSSName = OSS->GetSubsystemName().ToString();
		Data->SetBoolField(TEXT("online_subsystem_available"), true);
		Data->SetStringField(TEXT("online_subsystem_name"), OSSName);

		// Session interface
		IOnlineSessionPtr SessionInterface = OSS->GetSessionInterface();
		if (SessionInterface.IsValid())
		{
			Data->SetBoolField(TEXT("session_interface_available"), true);

			// Check for active game session
			FNamedOnlineSession* GameSession = SessionInterface->GetNamedSession(FName(TEXT("Game")));
			if (GameSession)
			{
				Data->SetBoolField(TEXT("game_session_active"), true);

				FString SessionStateStr;
				switch (GameSession->SessionState)
				{
				case EOnlineSessionState::NoSession: SessionStateStr = TEXT("NoSession"); break;
				case EOnlineSessionState::Creating: SessionStateStr = TEXT("Creating"); break;
				case EOnlineSessionState::Pending: SessionStateStr = TEXT("Pending"); break;
				case EOnlineSessionState::Starting: SessionStateStr = TEXT("Starting"); break;
				case EOnlineSessionState::InProgress: SessionStateStr = TEXT("InProgress"); break;
				case EOnlineSessionState::Ending: SessionStateStr = TEXT("Ending"); break;
				case EOnlineSessionState::Ended: SessionStateStr = TEXT("Ended"); break;
				case EOnlineSessionState::Destroying: SessionStateStr = TEXT("Destroying"); break;
				default: SessionStateStr = TEXT("Unknown"); break;
				}
				Data->SetStringField(TEXT("session_state"), SessionStateStr);
				Data->SetNumberField(TEXT("max_players"),
					GameSession->SessionSettings.NumPublicConnections + GameSession->SessionSettings.NumPrivateConnections);
				Data->SetBoolField(TEXT("is_lan"), GameSession->SessionSettings.bIsLANMatch);
				Data->SetBoolField(TEXT("is_dedicated"), GameSession->SessionSettings.bIsDedicated);
			}
			else
			{
				Data->SetBoolField(TEXT("game_session_active"), false);
				Data->SetStringField(TEXT("session_note"), TEXT("No active game session. Sessions created via IOnlineSession::CreateSession() at runtime."));
			}
		}
		else
		{
			Data->SetBoolField(TEXT("session_interface_available"), false);
			Data->SetStringField(TEXT("session_note"), TEXT("OnlineSubsystem loaded but session interface unavailable."));
		}
	}
	else
	{
		Data->SetBoolField(TEXT("online_subsystem_available"), false);
		Data->SetBoolField(TEXT("session_interface_available"), false);
		Data->SetBoolField(TEXT("game_session_active"), false);
		Data->SetStringField(TEXT("session_note"), TEXT("OnlineSubsystem not available. Configure in DefaultEngine.ini [OnlineSubsystem]. Common: NULL (LAN), Steam, EOS."));
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Session state: %s, net=%s, OSS=%s"),
			*WorldTypeStr, *NetModeStr, *OSSName),
		Data);
}

// ===== Query: Get Travel State =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteGetTravelState(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	TOptional<FMCPToolResult> ContextError = ValidateEditorContext(World);
	if (ContextError.IsSet())
	{
		return ContextError.GetValue();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// World context
	FString WorldTypeStr;
	switch (World->WorldType)
	{
	case EWorldType::Editor: WorldTypeStr = TEXT("Editor"); break;
	case EWorldType::PIE: WorldTypeStr = TEXT("PIE"); break;
	case EWorldType::Game: WorldTypeStr = TEXT("Game"); break;
	default: WorldTypeStr = TEXT("Other"); break;
	}
	Data->SetStringField(TEXT("world_type"), WorldTypeStr);

	FString NetModeStr;
	switch (World->GetNetMode())
	{
	case NM_Standalone: NetModeStr = TEXT("Standalone"); break;
	case NM_DedicatedServer: NetModeStr = TEXT("DedicatedServer"); break;
	case NM_ListenServer: NetModeStr = TEXT("ListenServer"); break;
	case NM_Client: NetModeStr = TEXT("Client"); break;
	default: NetModeStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("net_mode"), NetModeStr);

	// Travel state
	bool bIsInSeamlessTravel = World->IsInSeamlessTravel();
	Data->SetBoolField(TEXT("is_in_seamless_travel"), bIsInSeamlessTravel);

	// GameMode seamless travel config (server only)
	AGameModeBase* GM = World->GetAuthGameMode();
	if (GM)
	{
		Data->SetBoolField(TEXT("game_mode_present"), true);
		Data->SetBoolField(TEXT("uses_seamless_travel"), GM->bUseSeamlessTravel);
		Data->SetStringField(TEXT("game_mode_class"), GM->GetClass()->GetName());
	}
	else
	{
		Data->SetBoolField(TEXT("game_mode_present"), false);
		if (World->GetNetMode() == NM_Client)
		{
			Data->SetStringField(TEXT("game_mode_note"), TEXT("GameMode is server-only — not present on clients. Seamless travel config is server-side."));
		}
	}

	// Current level
	Data->SetStringField(TEXT("current_level"), World->GetMapName());

	// Runtime availability for travel
	bool bRuntimeAvailable = (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
	Data->SetBoolField(TEXT("travel_available"), bRuntimeAvailable);
	if (!bRuntimeAvailable)
	{
		Data->SetStringField(TEXT("travel_note"), TEXT("Editor world — travel not active. Start PIE to use ServerTravel/ClientTravel."));
	}

	// Travel guidance
	TSharedPtr<FJsonObject> TravelGuide = MakeShared<FJsonObject>();
	TravelGuide->SetStringField(TEXT("server_travel"), TEXT("GetWorld()->ServerTravel(URL) — all connected clients follow to new level"));
	TravelGuide->SetStringField(TEXT("client_travel"), TEXT("PlayerController->ClientTravel(URL, TRAVEL_Absolute) — client connects to new server"));
	TravelGuide->SetStringField(TEXT("seamless_enable"), TEXT("GameMode->bUseSeamlessTravel = true — PlayerController + PlayerState survive"));
	TravelGuide->SetStringField(TEXT("transition_map"), TEXT("Set GameMode->TransitionMapURL for loading screen during seamless travel"));
	Data->SetObjectField(TEXT("travel_guide"), TravelGuide);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Travel state: %s, net=%s, seamless_traveling=%s, level=%s"),
			*WorldTypeStr, *NetModeStr,
			bIsInSeamlessTravel ? TEXT("true") : TEXT("false"),
			*World->GetMapName()),
		Data);
}

// ===== Query: Audit Live Replication =====

FMCPToolResult FMCPTool_Multiplayer::ExecuteAuditLiveReplication(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	TOptional<FMCPToolResult> ContextError = ValidateEditorContext(World);
	if (ContextError.IsSet())
	{
		return ContextError.GetValue();
	}

	FString ClassFilter = ExtractOptionalString(Params, TEXT("class_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 100);
	Limit = FMath::Clamp(Limit, 1, 500);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	bool bIsRuntime = (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
	Data->SetBoolField(TEXT("is_runtime"), bIsRuntime);

	FString WorldTypeStr;
	switch (World->WorldType)
	{
	case EWorldType::Editor: WorldTypeStr = TEXT("Editor"); break;
	case EWorldType::PIE: WorldTypeStr = TEXT("PIE"); break;
	case EWorldType::Game: WorldTypeStr = TEXT("Game"); break;
	default: WorldTypeStr = TEXT("Other"); break;
	}
	Data->SetStringField(TEXT("world_type"), WorldTypeStr);

	if (!bIsRuntime)
	{
		Data->SetStringField(TEXT("audit_scope"), TEXT("editor-static"));
		Data->SetStringField(TEXT("audit_note"), TEXT("Editor world — audit compares configured intent from CDO/BP against editor actor state. Runtime replication behavior requires PIE."));
	}
	else
	{
		Data->SetStringField(TEXT("audit_scope"), TEXT("runtime-live"));
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> AuditedActors;
	int32 AuditedCount = 0;
	int32 IssueCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (AuditedCount >= Limit) break;

		AActor* Actor = *It;
		FString ClassName = Actor->GetClass()->GetName();

		if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter)) continue;

		AuditedCount++;

		// Get CDO for intent comparison
		AActor* CDO = Actor->GetClass()->GetDefaultObject<AActor>();
		if (!CDO) continue;

		bool bIntendedReplicated = CDO->GetIsReplicated();
		bool bActuallyReplicated = Actor->GetIsReplicated();

		// Intent vs observed comparison
		if (bIntendedReplicated != bActuallyReplicated)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("actor"), Actor->GetName());
			Issue->SetStringField(TEXT("class"), ClassName);
			Issue->SetStringField(TEXT("issue"), TEXT("replication_intent_mismatch"));
			Issue->SetStringField(TEXT("detail"),
				FString::Printf(TEXT("CDO bReplicates=%s but live actor bReplicates=%s"),
					bIntendedReplicated ? TEXT("true") : TEXT("false"),
					bActuallyReplicated ? TEXT("true") : TEXT("false")));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			IssueCount++;
		}

		// Owner-only relevance without owner
		if (Actor->bOnlyRelevantToOwner && !Actor->GetOwner())
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("actor"), Actor->GetName());
			Issue->SetStringField(TEXT("class"), ClassName);
			Issue->SetStringField(TEXT("issue"), TEXT("owner_only_no_owner"));
			Issue->SetStringField(TEXT("detail"), TEXT("bOnlyRelevantToOwner=true but no owner set — actor will not replicate to any client"));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			IssueCount++;
		}

		// Replicated actor with no authority (unusual in editor, meaningful in PIE)
		if (bActuallyReplicated && !Actor->HasAuthority() && bIsRuntime)
		{
			// This is normal for clients but worth noting
			TSharedPtr<FJsonObject> ActorEntry = MakeShared<FJsonObject>();
			ActorEntry->SetStringField(TEXT("name"), Actor->GetName());
			ActorEntry->SetStringField(TEXT("class"), ClassName);
			ActorEntry->SetStringField(TEXT("role"), TEXT("non-authority replica"));
			AuditedActors.Add(MakeShared<FJsonValueObject>(ActorEntry));
		}

		// Framework role placement check
		FString Role = FrameworkRoleForClass(Actor->GetClass());
		if (Role.Contains(TEXT("GameMode")) && World->GetNetMode() == NM_Client)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("actor"), Actor->GetName());
			Issue->SetStringField(TEXT("class"), ClassName);
			Issue->SetStringField(TEXT("issue"), TEXT("game_mode_on_client"));
			Issue->SetStringField(TEXT("detail"), TEXT("GameMode actor present on client — this should only exist on server"));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			IssueCount++;
		}
	}

	Data->SetArrayField(TEXT("issues"), Issues);
	Data->SetNumberField(TEXT("issue_count"), IssueCount);
	Data->SetNumberField(TEXT("audited_actor_count"), AuditedCount);
	Data->SetArrayField(TEXT("non_authority_replicas"), AuditedActors);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Live replication audit: %d actors audited, %d issues found (%s)"),
			AuditedCount, IssueCount, *WorldTypeStr),
		Data);
}
