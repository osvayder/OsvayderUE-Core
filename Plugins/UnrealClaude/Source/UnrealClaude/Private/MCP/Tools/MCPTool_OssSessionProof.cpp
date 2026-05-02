// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_OssSessionProof.h"

#include "MCPTool_Multiplayer.h"
#include "Dom/JsonValue.h"
#include "Misc/ConfigCacheIni.h"
#include "UnrealClaudeReportArtifacts.h"

namespace
{
	constexpr int32 MaxEvidenceEntries = 12;

	struct FConfiguredOssBaseline
	{
		bool bHasDefaultPlatformService = false;
		FString DefaultPlatformService;
		bool bHasGameNetDriverDefinition = false;
		FString GameNetDriverClass;
		FString GameNetDriverFallbackClass;
		FString Classification;
	};

	void AddUniqueLimited(TArray<FString>& InOutValues, const FString& Value, const int32 MaxCount = INDEX_NONE)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || InOutValues.Contains(Trimmed))
		{
			return;
		}

		if (MaxCount != INDEX_NONE && InOutValues.Num() >= MaxCount)
		{
			return;
		}

		InOutValues.Add(Trimmed);
	}

	void SetJsonStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const TArray<FString>& Values)
	{
		if (!Object.IsValid())
		{
			return;
		}

		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	}

	bool GetBoolFieldOrDefault(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const bool bDefaultValue = false)
	{
		bool bValue = bDefaultValue;
		if (Object.IsValid())
		{
			Object->TryGetBoolField(FieldName, bValue);
		}
		return bValue;
	}

	FString GetStringFieldOrDefault(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& DefaultValue = FString())
	{
		FString Value = DefaultValue;
		if (Object.IsValid())
		{
			Object->TryGetStringField(FieldName, Value);
		}
		return Value;
	}

	bool ExtractTupleValue(const FString& SourceLine, const FString& Key, FString& OutValue)
	{
		OutValue.Reset();

		const FString SearchToken = Key + TEXT("=");
		int32 StartIndex = SourceLine.Find(SearchToken, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		if (StartIndex == INDEX_NONE)
		{
			return false;
		}

		StartIndex += SearchToken.Len();
		if (StartIndex >= SourceLine.Len())
		{
			return false;
		}

		if (SourceLine[StartIndex] == TEXT('"'))
		{
			const int32 ValueStart = StartIndex + 1;
			const int32 ValueEnd = SourceLine.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, ValueStart);
			if (ValueEnd == INDEX_NONE)
			{
				return false;
			}

			OutValue = SourceLine.Mid(ValueStart, ValueEnd - ValueStart).TrimStartAndEnd();
			return !OutValue.IsEmpty();
		}

		int32 ValueEnd = SourceLine.Find(TEXT(","), ESearchCase::IgnoreCase, ESearchDir::FromStart, StartIndex);
		if (ValueEnd == INDEX_NONE)
		{
			ValueEnd = SourceLine.Find(TEXT(")"), ESearchCase::IgnoreCase, ESearchDir::FromStart, StartIndex);
		}

		if (ValueEnd == INDEX_NONE)
		{
			OutValue = SourceLine.Mid(StartIndex).TrimStartAndEnd();
		}
		else
		{
			OutValue = SourceLine.Mid(StartIndex, ValueEnd - StartIndex).TrimStartAndEnd();
		}

		return !OutValue.IsEmpty();
	}

	FConfiguredOssBaseline ReadConfiguredBaseline()
	{
		FConfiguredOssBaseline Baseline;

		FString DefaultPlatformService;
		if (GConfig && GConfig->GetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), DefaultPlatformService, GEngineIni))
		{
			DefaultPlatformService = DefaultPlatformService.TrimStartAndEnd();
			if (!DefaultPlatformService.IsEmpty())
			{
				Baseline.bHasDefaultPlatformService = true;
				Baseline.DefaultPlatformService = DefaultPlatformService;
			}
		}

		TArray<FString> NetDriverDefinitions;
		if (GConfig)
		{
			GConfig->GetArray(TEXT("/Script/Engine.GameEngine"), TEXT("NetDriverDefinitions"), NetDriverDefinitions, GEngineIni);
		}

		for (const FString& DefinitionLine : NetDriverDefinitions)
		{
			FString DefName;
			if (!ExtractTupleValue(DefinitionLine, TEXT("DefName"), DefName))
			{
				continue;
			}

			if (!DefName.Equals(TEXT("GameNetDriver"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			Baseline.bHasGameNetDriverDefinition = true;
			ExtractTupleValue(DefinitionLine, TEXT("DriverClassName"), Baseline.GameNetDriverClass);
			ExtractTupleValue(DefinitionLine, TEXT("DriverClassNameFallback"), Baseline.GameNetDriverFallbackClass);
			break;
		}

		if (!Baseline.bHasDefaultPlatformService)
		{
			Baseline.Classification = TEXT("configured_baseline_unspecified");
		}
		else if (Baseline.DefaultPlatformService.Equals(TEXT("NULL"), ESearchCase::IgnoreCase))
		{
			Baseline.Classification = TEXT("configured_null_offline_baseline");
		}
		else
		{
			Baseline.Classification = TEXT("configured_named_service_baseline");
		}

		return Baseline;
	}

	TSharedPtr<FJsonObject> BuildConfiguredBaselineObject(const FConfiguredOssBaseline& Baseline)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("default_platform_service_configured"), Baseline.bHasDefaultPlatformService);
		Object->SetStringField(TEXT("default_platform_service"), Baseline.DefaultPlatformService);
		Object->SetBoolField(TEXT("game_net_driver_definition_present"), Baseline.bHasGameNetDriverDefinition);
		Object->SetStringField(TEXT("game_net_driver_class"), Baseline.GameNetDriverClass);
		Object->SetStringField(TEXT("game_net_driver_fallback_class"), Baseline.GameNetDriverFallbackClass);
		Object->SetStringField(TEXT("configured_baseline_classification"), Baseline.Classification);
		return Object;
	}

	FString DetermineObservedBaselineClassification(const TSharedPtr<FJsonObject>& NetworkState, const TSharedPtr<FJsonObject>& SessionState)
	{
		if (!GetBoolFieldOrDefault(NetworkState, TEXT("runtime_state_available"), false))
		{
			return TEXT("current_context_not_runtime_active");
		}

		if (!GetBoolFieldOrDefault(SessionState, TEXT("online_subsystem_available"), false))
		{
			return TEXT("runtime_online_subsystem_unavailable");
		}

		const FString OnlineSubsystemName = GetStringFieldOrDefault(SessionState, TEXT("online_subsystem_name"));
		if (OnlineSubsystemName.Equals(TEXT("NULL"), ESearchCase::IgnoreCase))
		{
			return TEXT("runtime_null_offline_baseline_observed");
		}

		return TEXT("runtime_named_service_baseline_observed");
	}

	FString DetermineActiveSessionClassification(const TSharedPtr<FJsonObject>& NetworkState, const TSharedPtr<FJsonObject>& SessionState)
	{
		if (!GetBoolFieldOrDefault(NetworkState, TEXT("runtime_state_available"), false))
		{
			return TEXT("runtime_not_active");
		}

		if (!GetBoolFieldOrDefault(SessionState, TEXT("online_subsystem_available"), false))
		{
			return TEXT("online_subsystem_unavailable");
		}

		if (!GetBoolFieldOrDefault(SessionState, TEXT("session_interface_available"), false))
		{
			return TEXT("session_interface_unavailable");
		}

		return GetBoolFieldOrDefault(SessionState, TEXT("game_session_active"), false)
			? TEXT("active_session_present")
			: TEXT("no_active_session");
	}

	FString DetermineProofResult(const FString& ActiveSessionClassification)
	{
		if (ActiveSessionClassification == TEXT("runtime_not_active"))
		{
			return TEXT("baseline_runtime_not_active");
		}
		if (ActiveSessionClassification == TEXT("online_subsystem_unavailable"))
		{
			return TEXT("baseline_online_subsystem_unavailable");
		}
		if (ActiveSessionClassification == TEXT("session_interface_unavailable"))
		{
			return TEXT("baseline_session_interface_unavailable");
		}
		if (ActiveSessionClassification == TEXT("active_session_present"))
		{
			return TEXT("baseline_active_session_present");
		}
		return TEXT("baseline_no_active_session_observed");
	}

	void DetermineNextStep(
		const FString& ActiveSessionClassification,
		FString& OutRecommendedNextStepClass,
		FString& OutRecommendedNextStepReason)
	{
		if (ActiveSessionClassification == TEXT("runtime_not_active"))
		{
			OutRecommendedNextStepClass = TEXT("start_runtime_context_if_session_behavior_matters");
			OutRecommendedNextStepReason = TEXT("The current context is not runtime active, so host/join/session behavior still needs a runtime proof lane if it matters.");
			return;
		}

		if (ActiveSessionClassification == TEXT("active_session_present"))
		{
			OutRecommendedNextStepClass = TEXT("host_join_travel_proof_next");
			OutRecommendedNextStepReason = TEXT("An active session is present in the current context, but host/join/travel/gameplay behavior remains unproven.");
			return;
		}

		OutRecommendedNextStepClass = TEXT("host_or_join_session_if_active_session_proof_matters");
		OutRecommendedNextStepReason = TEXT("The current context exposes baseline OSS/session facts, but no active game session was observed.");
	}

	FString BuildMarkdownReport(
		const FString& ProbeMode,
		const FString& ProofResult,
		const FString& ConfiguredBaselineClassification,
		const FString& ObservedBaselineClassification,
		const FString& ActiveSessionClassification,
		const FString& RecommendedNextStepClass,
		const FString& RecommendedNextStepReason,
		const TSharedPtr<FJsonObject>& ConfiguredBaseline,
		const TSharedPtr<FJsonObject>& NetworkState,
		const TSharedPtr<FJsonObject>& SessionState,
		const TSharedPtr<FJsonObject>& TravelState,
		const TArray<FString>& Limitations)
	{
		FString Markdown = TEXT("# OSS Session Baseline Proof\n\n");
		Markdown += TEXT("This report captures a bounded OSS/session baseline proof result with explicit separation between configured baseline, observed current context, and active-session state.\n\n");
		Markdown += TEXT("## Summary\n\n");
		Markdown += FString::Printf(TEXT("- Probe mode: `%s`\n"), *ProbeMode);
		Markdown += TEXT("- Execution mode: `read_only`\n");
		Markdown += FString::Printf(TEXT("- Proof result: `%s`\n"), *ProofResult);
		Markdown += FString::Printf(TEXT("- Configured baseline classification: `%s`\n"), *ConfiguredBaselineClassification);
		Markdown += FString::Printf(TEXT("- Observed baseline classification: `%s`\n"), *ObservedBaselineClassification);
		Markdown += FString::Printf(TEXT("- Active session classification: `%s`\n"), *ActiveSessionClassification);

		Markdown += TEXT("\n## Truth Boundary\n\n");
		Markdown += TEXT("- Configured/default OSS state is not active-session proof.\n");
		Markdown += TEXT("- Session interface availability is not active-session proof.\n");
		Markdown += TEXT("- A NULL/offline baseline is not treated as a bug without stronger evidence.\n");
		Markdown += TEXT("- This slice does not prove host/join/gameplay behavior.\n");

		if (ConfiguredBaseline.IsValid())
		{
			Markdown += TEXT("\n## Configured Baseline\n\n");
			Markdown += FString::Printf(TEXT("- DefaultPlatformService: `%s`\n"), *GetStringFieldOrDefault(ConfiguredBaseline, TEXT("default_platform_service"), TEXT("")));
			Markdown += FString::Printf(TEXT("- GameNetDriver class: `%s`\n"), *GetStringFieldOrDefault(ConfiguredBaseline, TEXT("game_net_driver_class"), TEXT("")));
			const FString FallbackClass = GetStringFieldOrDefault(ConfiguredBaseline, TEXT("game_net_driver_fallback_class"), TEXT(""));
			if (!FallbackClass.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- GameNetDriver fallback: `%s`\n"), *FallbackClass);
			}
		}

		Markdown += TEXT("\n## Observed Context\n\n");
		Markdown += FString::Printf(TEXT("- World type: `%s`\n"), *GetStringFieldOrDefault(NetworkState, TEXT("world_type"), TEXT("")));
		Markdown += FString::Printf(TEXT("- Net mode: `%s`\n"), *GetStringFieldOrDefault(NetworkState, TEXT("net_mode"), TEXT("")));
		Markdown += FString::Printf(TEXT("- Runtime state available: `%s`\n"), GetBoolFieldOrDefault(NetworkState, TEXT("runtime_state_available"), false) ? TEXT("true") : TEXT("false"));
		Markdown += FString::Printf(TEXT("- OnlineSubsystem available: `%s`\n"), GetBoolFieldOrDefault(SessionState, TEXT("online_subsystem_available"), false) ? TEXT("true") : TEXT("false"));
		Markdown += FString::Printf(TEXT("- OnlineSubsystem name: `%s`\n"), *GetStringFieldOrDefault(SessionState, TEXT("online_subsystem_name"), TEXT("unavailable")));
		Markdown += FString::Printf(TEXT("- Session interface available: `%s`\n"), GetBoolFieldOrDefault(SessionState, TEXT("session_interface_available"), false) ? TEXT("true") : TEXT("false"));
		Markdown += FString::Printf(TEXT("- Active game session: `%s`\n"), GetBoolFieldOrDefault(SessionState, TEXT("game_session_active"), false) ? TEXT("true") : TEXT("false"));
		Markdown += FString::Printf(TEXT("- Travel available: `%s`\n"), GetBoolFieldOrDefault(TravelState, TEXT("travel_available"), false) ? TEXT("true") : TEXT("false"));
		Markdown += FString::Printf(TEXT("- Current level: `%s`\n"), *GetStringFieldOrDefault(TravelState, TEXT("current_level"), TEXT("")));

		Markdown += TEXT("\n## Limitations\n\n");
		for (const FString& Limitation : Limitations)
		{
			Markdown += FString::Printf(TEXT("- %s\n"), *Limitation);
		}

		Markdown += TEXT("\n## Recommended Next Step\n\n");
		Markdown += FString::Printf(TEXT("- `%s`: %s\n"), *RecommendedNextStepClass, *RecommendedNextStepReason);
		return Markdown;
	}

	FUnrealClaudeReportTruthSummary BuildTruthSummary(
		const FConfiguredOssBaseline& ConfiguredBaseline,
		const TSharedPtr<FJsonObject>& NetworkState,
		const TSharedPtr<FJsonObject>& SessionState,
		const FString& ActiveSessionClassification,
		const TArray<FString>& Limitations)
	{
		FUnrealClaudeReportTruthSummary TruthSummary;

		if (ConfiguredBaseline.bHasDefaultPlatformService)
		{
			TruthSummary.PracticallyVerified.Add(FString::Printf(TEXT("Configured DefaultPlatformService was read as %s."), *ConfiguredBaseline.DefaultPlatformService));
		}
		else
		{
			TruthSummary.PracticallyVerified.Add(TEXT("Configured DefaultPlatformService was checked and no explicit value was found."));
		}

		TruthSummary.PracticallyVerified.Add(TEXT("Current network/session/travel state was queried through the live editor context."));
		TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed world type is %s and net mode is %s."),
			*GetStringFieldOrDefault(NetworkState, TEXT("world_type"), TEXT("unknown")),
			*GetStringFieldOrDefault(NetworkState, TEXT("net_mode"), TEXT("unknown"))));
		TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed OnlineSubsystem name is %s."),
			*GetStringFieldOrDefault(SessionState, TEXT("online_subsystem_name"), TEXT("unavailable"))));

		if (ActiveSessionClassification == TEXT("active_session_present"))
		{
			TruthSummary.PracticallyVerified.Add(TEXT("An active game session was observed in the current runtime context."));
		}
		else if (ActiveSessionClassification == TEXT("no_active_session"))
		{
			TruthSummary.PracticallyVerified.Add(TEXT("No active game session was observed in the current runtime context."));
		}

		TruthSummary.Limited.Add(TEXT("Configured/default OSS state is not active-session proof."));
		TruthSummary.Limited.Add(TEXT("Session interface availability is not active-session proof."));
		for (const FString& Limitation : Limitations)
		{
			AddUniqueLimited(TruthSummary.Limited, Limitation, MaxEvidenceEntries);
		}

		TruthSummary.NotVerified.Add(TEXT("Host/join/gameplay behavior still requires a separate runtime packet if those behaviors matter."));
		return TruthSummary;
	}

	TSharedPtr<FJsonObject> BuildExtraMetadata(
		const FConfiguredOssBaseline& ConfiguredBaseline,
		const FString& ProbeMode,
		const FString& ProofResult,
		const FString& ObservedBaselineClassification,
		const FString& ActiveSessionClassification)
	{
		TSharedPtr<FJsonObject> ExtraMetadata = MakeShared<FJsonObject>();
		ExtraMetadata->SetStringField(TEXT("source_operation"), TEXT("probe_oss_session_baseline"));
		ExtraMetadata->SetStringField(TEXT("probe_mode"), ProbeMode);
		ExtraMetadata->SetStringField(TEXT("proof_result"), ProofResult);
		ExtraMetadata->SetStringField(TEXT("configured_baseline_classification"), ConfiguredBaseline.Classification);
		ExtraMetadata->SetStringField(TEXT("observed_baseline_classification"), ObservedBaselineClassification);
		ExtraMetadata->SetStringField(TEXT("active_session_classification"), ActiveSessionClassification);
		ExtraMetadata->SetStringField(TEXT("configured_default_platform_service"), ConfiguredBaseline.DefaultPlatformService);
		return ExtraMetadata;
	}
}

FMCPToolResult FMCPTool_OssSessionProof::Execute(const TSharedRef<FJsonObject>& Params)
{
	const FString Operation = ExtractOptionalString(Params, TEXT("operation"), TEXT("probe_oss_session_baseline"));
	if (!Operation.Equals(TEXT("probe_oss_session_baseline"), ESearchCase::IgnoreCase))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Unsupported operation '%s'. Supported: probe_oss_session_baseline"), *Operation));
	}

	const FString ProbeMode = ExtractOptionalString(Params, TEXT("probe_mode"), TEXT("current_context_snapshot")).TrimStartAndEnd();
	if (!ProbeMode.Equals(TEXT("current_context_snapshot"), ESearchCase::IgnoreCase))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Unsupported probe_mode '%s'. Supported: current_context_snapshot"), *ProbeMode));
	}

	const bool bExportReport = ExtractOptionalBool(Params, TEXT("export_report"), false);
	const FString CustomReportName = ExtractOptionalString(Params, TEXT("report_name"));
	const FString CustomReportSlug = ExtractOptionalString(Params, TEXT("report_slug"));

	FMCPTool_Multiplayer MultiplayerTool;

	TSharedRef<FJsonObject> NetworkParams = MakeShared<FJsonObject>();
	NetworkParams->SetStringField(TEXT("operation"), TEXT("get_network_state"));
	const FMCPToolResult NetworkResult = MultiplayerTool.Execute(NetworkParams);
	if (!NetworkResult.bSuccess || !NetworkResult.Data.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to query multiplayer.get_network_state: %s"), *NetworkResult.Message));
	}

	TSharedRef<FJsonObject> SessionParams = MakeShared<FJsonObject>();
	SessionParams->SetStringField(TEXT("operation"), TEXT("get_session_state"));
	const FMCPToolResult SessionResult = MultiplayerTool.Execute(SessionParams);
	if (!SessionResult.bSuccess || !SessionResult.Data.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to query multiplayer.get_session_state: %s"), *SessionResult.Message));
	}

	TSharedRef<FJsonObject> TravelParams = MakeShared<FJsonObject>();
	TravelParams->SetStringField(TEXT("operation"), TEXT("get_travel_state"));
	const FMCPToolResult TravelResult = MultiplayerTool.Execute(TravelParams);
	if (!TravelResult.bSuccess || !TravelResult.Data.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to query multiplayer.get_travel_state: %s"), *TravelResult.Message));
	}

	const FConfiguredOssBaseline ConfiguredBaseline = ReadConfiguredBaseline();
	const TSharedPtr<FJsonObject> ConfiguredBaselineObject = BuildConfiguredBaselineObject(ConfiguredBaseline);
	const TSharedPtr<FJsonObject> NetworkStateObject = NetworkResult.Data;
	const TSharedPtr<FJsonObject> SessionStateObject = SessionResult.Data;
	const TSharedPtr<FJsonObject> TravelStateObject = TravelResult.Data;

	TArray<FString> EvidenceBasis;
	TArray<FString> Limitations;
	AddUniqueLimited(EvidenceBasis, TEXT("config:default_platform_service"), MaxEvidenceEntries);
	AddUniqueLimited(EvidenceBasis, TEXT("config:game_net_driver_definition"), MaxEvidenceEntries);
	AddUniqueLimited(EvidenceBasis, TEXT("runtime:get_network_state"), MaxEvidenceEntries);
	AddUniqueLimited(EvidenceBasis, TEXT("runtime:get_session_state"), MaxEvidenceEntries);
	AddUniqueLimited(EvidenceBasis, TEXT("runtime:get_travel_state"), MaxEvidenceEntries);

	const FString ObservedBaselineClassification = DetermineObservedBaselineClassification(NetworkStateObject, SessionStateObject);
	const FString ActiveSessionClassification = DetermineActiveSessionClassification(NetworkStateObject, SessionStateObject);
	const FString ProofResult = DetermineProofResult(ActiveSessionClassification);

	const bool bRuntimeStateAvailable = GetBoolFieldOrDefault(NetworkStateObject, TEXT("runtime_state_available"), false);
	const bool bOnlineSubsystemAvailable = GetBoolFieldOrDefault(SessionStateObject, TEXT("online_subsystem_available"), false);
	const bool bSessionInterfaceAvailable = GetBoolFieldOrDefault(SessionStateObject, TEXT("session_interface_available"), false);
	const bool bGameSessionActive = GetBoolFieldOrDefault(SessionStateObject, TEXT("game_session_active"), false);
	const FString OnlineSubsystemName = GetStringFieldOrDefault(SessionStateObject, TEXT("online_subsystem_name"), TEXT("unavailable"));

	AddUniqueLimited(Limitations, TEXT("Configured/default OSS state is not active-session proof."), MaxEvidenceEntries);
	AddUniqueLimited(Limitations, TEXT("Session interface availability is not active-session proof."), MaxEvidenceEntries);
	if (OnlineSubsystemName.Equals(TEXT("NULL"), ESearchCase::IgnoreCase) || ConfiguredBaseline.DefaultPlatformService.Equals(TEXT("NULL"), ESearchCase::IgnoreCase))
	{
		AddUniqueLimited(Limitations, TEXT("A NULL/offline baseline is a valid observed/configured baseline in this slice; it is not treated as a bug without stronger evidence."), MaxEvidenceEntries);
	}

	if (!bRuntimeStateAvailable)
	{
		AddUniqueLimited(Limitations, TEXT("The current context is not runtime active, so active-session behavior is not proven in this slice."), MaxEvidenceEntries);
	}
	else if (!bOnlineSubsystemAvailable)
	{
		AddUniqueLimited(Limitations, TEXT("Runtime is active, but no OnlineSubsystem was observed in the current context."), MaxEvidenceEntries);
	}
	else if (!bSessionInterfaceAvailable)
	{
		AddUniqueLimited(Limitations, TEXT("Runtime is active and an OnlineSubsystem is loaded, but no session interface was observed in the current context."), MaxEvidenceEntries);
	}
	else if (!bGameSessionActive)
	{
		AddUniqueLimited(Limitations, TEXT("No active game session was observed in the current runtime context. Host/join behavior remains unproven."), MaxEvidenceEntries);
	}

	FString RecommendedNextStepClass;
	FString RecommendedNextStepReason;
	DetermineNextStep(ActiveSessionClassification, RecommendedNextStepClass, RecommendedNextStepReason);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("probe_oss_session_baseline"));
	ResultData->SetStringField(TEXT("probe_mode"), ProbeMode);
	ResultData->SetStringField(TEXT("execution_mode"), TEXT("read_only"));
	ResultData->SetObjectField(TEXT("configured_baseline"), ConfiguredBaselineObject);
	ResultData->SetObjectField(TEXT("observed_network_state"), NetworkStateObject);
	ResultData->SetObjectField(TEXT("observed_session_state"), SessionStateObject);
	ResultData->SetObjectField(TEXT("observed_travel_state"), TravelStateObject);
	ResultData->SetBoolField(TEXT("runtime_state_available"), bRuntimeStateAvailable);
	ResultData->SetStringField(TEXT("configured_baseline_classification"), ConfiguredBaseline.Classification);
	ResultData->SetStringField(TEXT("observed_baseline_classification"), ObservedBaselineClassification);
	ResultData->SetStringField(TEXT("active_session_classification"), ActiveSessionClassification);
	ResultData->SetStringField(TEXT("proof_result"), ProofResult);
	ResultData->SetStringField(TEXT("recommended_next_step_class"), RecommendedNextStepClass);
	ResultData->SetStringField(TEXT("recommended_next_step_reason"), RecommendedNextStepReason);
	SetJsonStringArrayField(ResultData, TEXT("evidence_basis"), EvidenceBasis);
	SetJsonStringArrayField(ResultData, TEXT("limitations"), Limitations);

	FString Message = FString::Printf(TEXT("oss_session_proof -> %s"), *ProofResult);

	if (bExportReport)
	{
		FUnrealClaudeReportExportRequest ExportRequest;
		ExportRequest.ReportName = !CustomReportName.TrimStartAndEnd().IsEmpty()
			? CustomReportName
			: TEXT("OSS Session Baseline Proof");
		ExportRequest.ReportSlug = !CustomReportSlug.TrimStartAndEnd().IsEmpty()
			? CustomReportSlug
			: TEXT("oss_session_proof");
		ExportRequest.Markdown = BuildMarkdownReport(
			ProbeMode,
			ProofResult,
			ConfiguredBaseline.Classification,
			ObservedBaselineClassification,
			ActiveSessionClassification,
			RecommendedNextStepClass,
			RecommendedNextStepReason,
			ConfiguredBaselineObject,
			NetworkStateObject,
			SessionStateObject,
			TravelStateObject,
			Limitations);
		ExportRequest.SummaryText = Message;
		ExportRequest.RunKind = TEXT("oss_session_proof");
		ExportRequest.ExecutionMode = TEXT("read_only");
		ExportRequest.ToolNames = { TEXT("oss_session_proof") };
		ExportRequest.ToolFamilies = { TEXT("runtime_proof"), TEXT("oss_session_proof") };
		ExportRequest.EvidenceClasses = { TEXT("configured_oss_baseline"), TEXT("runtime_session_state"), TEXT("runtime_context_observation") };
		ExportRequest.TruthSummary = BuildTruthSummary(ConfiguredBaseline, NetworkStateObject, SessionStateObject, ActiveSessionClassification, Limitations);
		ExportRequest.ExtraMetadata = BuildExtraMetadata(ConfiguredBaseline, ProbeMode, ProofResult, ObservedBaselineClassification, ActiveSessionClassification);

		FUnrealClaudeReportExportResult ExportResult;
		if (!FUnrealClaudeReportArtifacts::ExportReport(ExportRequest, ExportResult))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("%s | report export failed: %s"), *Message, *ExportResult.ErrorMessage));
		}

		TSharedPtr<FJsonObject> ArtifactObject = MakeShared<FJsonObject>();
		ArtifactObject->SetStringField(TEXT("report_id"), ExportResult.ReportId);
		ArtifactObject->SetStringField(TEXT("markdown_path"), ExportResult.MarkdownPath);
		ArtifactObject->SetStringField(TEXT("summary_path"), ExportResult.SummaryPath);
		ArtifactObject->SetStringField(TEXT("status_tool"), TEXT("report_artifact_status"));
		ResultData->SetObjectField(TEXT("report_artifact"), ArtifactObject);
		Message = FString::Printf(TEXT("%s | report=%s"), *Message, *ExportResult.ReportId);
	}

	return FMCPToolResult::Success(Message, ResultData);
}
