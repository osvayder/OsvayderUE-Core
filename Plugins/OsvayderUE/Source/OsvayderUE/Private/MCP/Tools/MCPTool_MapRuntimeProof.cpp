// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_MapRuntimeProof.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"
#include "OsvayderUEReportArtifacts.h"

namespace
{
	constexpr int32 MaxEvidenceEntries = 12;
	constexpr int32 MaxActorClassRows = 24;
	constexpr int32 MaxActorSampleRows = 48;
	constexpr int32 MaxWorldPartitionBasis = 6;

	struct FCurrentMapPresence
	{
		bool bPresent = false;
		bool bMapFilePresent = false;
		bool bAssetRegistryPresent = false;
		FString ResolvedFilename;
	};

	struct FMapProbePaths
	{
		FString ProbeId;
		FString ProbeDirectory;
		FString ScriptPath;
		FString ResultPath;
		FString LogPath;
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

	FString NormalizePackagePath(const FString& InValue)
	{
		FString Candidate = InValue.TrimStartAndEnd();
		if (Candidate.IsEmpty())
		{
			return FString();
		}

		Candidate.ReplaceInline(TEXT("\""), TEXT(""));

		FSoftObjectPath SoftObjectPath(Candidate);
		if (!SoftObjectPath.GetLongPackageName().IsEmpty())
		{
			Candidate = SoftObjectPath.GetLongPackageName();
		}
		else if (Candidate.Contains(TEXT(".")))
		{
			FString LeftSide;
			if (Candidate.Split(TEXT("."), &LeftSide, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart))
			{
				Candidate = LeftSide;
			}
		}

		FText UnusedReason;
		if (!FPackageName::IsValidLongPackageName(Candidate, false, &UnusedReason))
		{
			return FString();
		}

		return Candidate;
	}

	FString MakeMapObjectPath(const FString& PackagePath)
	{
		if (PackagePath.IsEmpty())
		{
			return FString();
		}

		return FString::Printf(TEXT("%s.%s"), *PackagePath, *FPackageName::GetLongPackageAssetName(PackagePath));
	}

	FString MakeSafeLabel(const FString& InValue)
	{
		FString Safe = InValue.TrimStartAndEnd();
		if (Safe.IsEmpty())
		{
			Safe = TEXT("map_runtime_probe");
		}

		for (TCHAR& Character : Safe)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-')))
			{
				Character = TEXT('_');
			}
		}

		while (Safe.Contains(TEXT("__")))
		{
			Safe.ReplaceInline(TEXT("__"), TEXT("_"));
		}

		return Safe;
	}

	FString ToPythonLiteral(const FString& InValue)
	{
		FString Escaped = InValue;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
		Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
		return Escaped;
	}

	FString JoinShortList(const TArray<FString>& Values)
	{
		if (Values.Num() == 0)
		{
			return TEXT("none");
		}

		FString Joined;
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			if (Index > 0)
			{
				Joined += TEXT("; ");
			}
			Joined += Values[Index];
		}
		return Joined;
	}

	FCurrentMapPresence DetermineCurrentMapPresence(const FString& PackagePath)
	{
		FCurrentMapPresence Presence;
		if (PackagePath.IsEmpty())
		{
			return Presence;
		}

		FString CandidateFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, CandidateFilename, FPackageName::GetMapPackageExtension())
			&& FPaths::FileExists(CandidateFilename))
		{
			Presence.bMapFilePresent = true;
			Presence.ResolvedFilename = CandidateFilename;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> AssetDatas;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), AssetDatas, true);
		Presence.bAssetRegistryPresent = AssetDatas.Num() > 0;
		Presence.bPresent = Presence.bMapFilePresent || Presence.bAssetRegistryPresent;
		return Presence;
	}

	FString GetCurrentEditorBinaryPath()
	{
		FString ExecutablePath = FPlatformProcess::ExecutablePath();
		FPaths::NormalizeFilename(ExecutablePath);

		const FString Filename = FPaths::GetCleanFilename(ExecutablePath);
		if (Filename.Equals(TEXT("UnrealEditor.exe"), ESearchCase::IgnoreCase))
		{
			const FString Candidate = FPaths::Combine(FPaths::GetPath(ExecutablePath), TEXT("UnrealEditor-Cmd.exe"));
			if (FPaths::FileExists(Candidate))
			{
				return Candidate;
			}
		}

		return ExecutablePath;
	}

	FMapProbePaths MakeProbePaths(const FString& MapPackage)
	{
		const FString MapLabel = MakeSafeLabel(FPackageName::GetLongPackageAssetName(MapPackage));
		const FString ProbeId = FString::Printf(TEXT("%lld_%s"), FDateTime::UtcNow().GetTicks(), *MapLabel);
		const FString ProbeRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("runtime_probes"), ProbeId);
		IFileManager::Get().MakeDirectory(*ProbeRoot, true);

		FMapProbePaths Paths;
		Paths.ProbeId = ProbeId;
		Paths.ProbeDirectory = ProbeRoot;
		Paths.ScriptPath = FPaths::Combine(ProbeRoot, ProbeId + TEXT(".py"));
		Paths.ResultPath = FPaths::Combine(ProbeRoot, ProbeId + TEXT(".json"));
		Paths.LogPath = FPaths::Combine(ProbeRoot, ProbeId + TEXT(".log"));
		return Paths;
	}

	FString BuildPythonProbeScript(
		const FString& MapPackage,
		const FString& ResultPath,
		const FString& ExternalActorsDir,
		const FString& ExternalObjectsDir)
	{
		const FString MapObjectPath = MakeMapObjectPath(MapPackage);

		FString Script;
		Script += TEXT("import collections\n");
		Script += TEXT("import json\n");
		Script += TEXT("import os\n");
		Script += TEXT("import unreal\n\n");
		Script += FString::Printf(TEXT("MAP_PATH = '%s'\n"), *ToPythonLiteral(MapPackage));
		Script += FString::Printf(TEXT("MAP_OBJECT_PATH = '%s'\n"), *ToPythonLiteral(MapObjectPath));
		Script += FString::Printf(TEXT("RESULT_PATH = '%s'\n"), *ToPythonLiteral(ResultPath));
		Script += FString::Printf(TEXT("EXTERNAL_ACTORS_DIR = '%s'\n"), *ToPythonLiteral(ExternalActorsDir));
		Script += FString::Printf(TEXT("EXTERNAL_OBJECTS_DIR = '%s'\n\n"), *ToPythonLiteral(ExternalObjectsDir));

		Script += TEXT("def collect_file_stats(root_dir):\n");
		Script += TEXT("    file_count = 0\n");
		Script += TEXT("    total_bytes = 0\n");
		Script += TEXT("    largest = []\n");
		Script += TEXT("    if not os.path.isdir(root_dir):\n");
		Script += TEXT("        return {\n");
		Script += TEXT("            'exists': False,\n");
		Script += TEXT("            'file_count': 0,\n");
		Script += TEXT("            'total_bytes': 0,\n");
		Script += TEXT("            'largest_files': [],\n");
		Script += TEXT("        }\n");
		Script += TEXT("    for current_root, _, files in os.walk(root_dir):\n");
		Script += TEXT("        for filename in files:\n");
		Script += TEXT("            path = os.path.join(current_root, filename)\n");
		Script += TEXT("            try:\n");
		Script += TEXT("                size = os.path.getsize(path)\n");
		Script += TEXT("            except OSError:\n");
		Script += TEXT("                continue\n");
		Script += TEXT("            file_count += 1\n");
		Script += TEXT("            total_bytes += size\n");
		Script += TEXT("            largest.append({'path': path, 'bytes': size})\n");
		Script += TEXT("    largest.sort(key=lambda item: item['bytes'], reverse=True)\n");
		Script += TEXT("    return {\n");
		Script += TEXT("        'exists': True,\n");
		Script += TEXT("        'file_count': file_count,\n");
		Script += TEXT("        'total_bytes': total_bytes,\n");
		Script += TEXT("        'largest_files': largest[:12],\n");
		Script += TEXT("    }\n\n");

		Script += TEXT("def safe_editor_property(obj, property_name, default=None):\n");
		Script += TEXT("    if obj is None:\n");
		Script += TEXT("        return default\n");
		Script += TEXT("    try:\n");
		Script += TEXT("        return obj.get_editor_property(property_name)\n");
		Script += TEXT("    except Exception:\n");
		Script += TEXT("        return default\n\n");

		Script += TEXT("result = {\n");
		Script += TEXT("    'probe_mode': 'headless_editor_load',\n");
		Script += TEXT("    'map_path': MAP_PATH,\n");
		Script += TEXT("    'map_object_path': MAP_OBJECT_PATH,\n");
		Script += TEXT("    'status': 'runtime_load_failed',\n");
		Script += TEXT("}\n\n");

		Script += TEXT("try:\n");
		Script += TEXT("    world = unreal.EditorLoadingAndSavingUtils.load_map(MAP_PATH)\n");
		Script += TEXT("    if not world:\n");
		Script += TEXT("        raise RuntimeError(f'Failed to load map {MAP_PATH}')\n");
		Script += TEXT("    editor_actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n");
		Script += TEXT("    actors = list(editor_actor_subsystem.get_all_level_actors())\n");
		Script += TEXT("    class_counts = collections.Counter()\n");
		Script += TEXT("    actor_sample = []\n");
		Script += TEXT("    for actor in actors:\n");
		Script += TEXT("        class_name = actor.get_class().get_name()\n");
		Script += TEXT("        class_counts[class_name] += 1\n");
		Script += FString::Printf(TEXT("        if len(actor_sample) < %d:\n"), MaxActorSampleRows);
		Script += TEXT("            actor_sample.append({'name': actor.get_name(), 'class': class_name})\n");
		Script += TEXT("    world_settings = world.get_world_settings()\n");
		Script += TEXT("    world_partition = safe_editor_property(world, 'world_partition', None)\n");
		Script += TEXT("    persistent_level = safe_editor_property(world, 'persistent_level', None)\n");
		Script += TEXT("    result.update({\n");
		Script += TEXT("        'status': 'runtime_load_succeeded',\n");
		Script += TEXT("        'loaded_world_name': world.get_name(),\n");
		Script += TEXT("        'persistent_level_name': persistent_level.get_name() if persistent_level else None,\n");
		Script += TEXT("        'actor_count': len(actors),\n");
		Script += TEXT("        'player_start_count': class_counts.get('PlayerStart', 0),\n");
		Script += TEXT("        'world_settings_class': world_settings.get_class().get_name() if world_settings else None,\n");
		Script += TEXT("        'world_partition_editor_property_present': bool(world_partition),\n");
		Script += TEXT("        'world_partition_class': world_partition.get_class().get_name() if world_partition else None,\n");
		Script += TEXT("        'top_actor_classes': [\n");
		Script += FString::Printf(TEXT("            {'class': class_name, 'count': count} for class_name, count in class_counts.most_common(%d)\n"), MaxActorClassRows);
		Script += TEXT("        ],\n");
		Script += TEXT("        'actor_sample': actor_sample,\n");
		Script += TEXT("        'external_actors': collect_file_stats(EXTERNAL_ACTORS_DIR),\n");
		Script += TEXT("        'external_objects': collect_file_stats(EXTERNAL_OBJECTS_DIR),\n");
		Script += TEXT("    })\n");
		Script += TEXT("except Exception as exc:\n");
		Script += TEXT("    result['error_message'] = str(exc)\n");
		Script += TEXT("finally:\n");
		Script += TEXT("    os.makedirs(os.path.dirname(RESULT_PATH), exist_ok=True)\n");
		Script += TEXT("    with open(RESULT_PATH, 'w', encoding='utf-8') as handle:\n");
		Script += TEXT("        json.dump(result, handle, indent=2)\n");
		Script += TEXT("    if result.get('status') == 'runtime_load_succeeded':\n");
		Script += TEXT("        unreal.log('MAP_RUNTIME_PROOF_OK')\n");
		Script += TEXT("    else:\n");
		Script += TEXT("        unreal.log_warning('MAP_RUNTIME_PROOF_FAILED')\n");
		Script += TEXT("    unreal.log('MAP_RUNTIME_PROOF_RESULT_WRITTEN')\n");

		return Script;
	}

	bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject)
	{
		OutObject.Reset();

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool WaitForFile(const FString& FilePath, const double TimeoutSeconds)
	{
		const double StartTime = FPlatformTime::Seconds();
		while ((FPlatformTime::Seconds() - StartTime) < TimeoutSeconds)
		{
			if (FPaths::FileExists(FilePath))
			{
				return true;
			}

			FPlatformProcess::Sleep(0.1f);
		}

		return FPaths::FileExists(FilePath);
	}

	bool RunHeadlessEditorProbe(
		const FString& ScriptPath,
		const FString& ProbeLogPath,
		const double TimeoutSeconds,
		int32& OutExitCode,
		TArray<FString>& OutExecutionNotes)
	{
		OutExitCode = INDEX_NONE;
		OutExecutionNotes.Reset();

		const FString EditorExecutable = GetCurrentEditorBinaryPath();
		const FString ProjectFilePath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		if (!FPaths::FileExists(EditorExecutable))
		{
			AddUniqueLimited(OutExecutionNotes, FString::Printf(TEXT("Editor executable was not found: %s"), *EditorExecutable), MaxEvidenceEntries);
			return false;
		}

		if (!FPaths::FileExists(ProjectFilePath))
		{
			AddUniqueLimited(OutExecutionNotes, FString::Printf(TEXT("Project file was not found: %s"), *ProjectFilePath), MaxEvidenceEntries);
			return false;
		}

		const FString Args = FString::Printf(
			TEXT("\"%s\" -EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities -run=pythonscript -script=\"%s\" -unattended -nop4 -nosplash -nullrhi -nosound -abslog=\"%s\" -stdout -FullStdOutLogOutput"),
			*ProjectFilePath,
			*ScriptPath,
			*ProbeLogPath);

		FProcHandle ProcessHandle = FPlatformProcess::CreateProc(
			*EditorExecutable,
			*Args,
			false,
			true,
			true,
			nullptr,
			0,
			*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
			nullptr);

		if (!ProcessHandle.IsValid())
		{
			AddUniqueLimited(OutExecutionNotes, TEXT("Failed to launch the headless editor probe process."), MaxEvidenceEntries);
			return false;
		}

		const double StartTime = FPlatformTime::Seconds();
		while (FPlatformProcess::IsProcRunning(ProcessHandle))
		{
			if ((FPlatformTime::Seconds() - StartTime) >= TimeoutSeconds)
			{
				FPlatformProcess::TerminateProc(ProcessHandle, true);
				FPlatformProcess::CloseProc(ProcessHandle);
				AddUniqueLimited(
					OutExecutionNotes,
					FString::Printf(TEXT("Headless editor probe timed out after %.1f seconds and was terminated."), TimeoutSeconds),
					MaxEvidenceEntries);
				return false;
			}

			FPlatformProcess::Sleep(0.1f);
		}

		if (!FPlatformProcess::GetProcReturnCode(ProcessHandle, &OutExitCode))
		{
			AddUniqueLimited(OutExecutionNotes, TEXT("Headless editor probe finished but its process return code was unavailable."), MaxEvidenceEntries);
		}

		FPlatformProcess::CloseProc(ProcessHandle);
		return true;
	}

	void SetPresenceObject(const TSharedPtr<FJsonObject>& Object, const FCurrentMapPresence& Presence)
	{
		if (!Object.IsValid())
		{
			return;
		}

		Object->SetBoolField(TEXT("present"), Presence.bPresent);
		Object->SetBoolField(TEXT("map_file_present"), Presence.bMapFilePresent);
		Object->SetBoolField(TEXT("asset_registry_present"), Presence.bAssetRegistryPresent);
		Object->SetStringField(TEXT("resolved_filename"), Presence.ResolvedFilename);
	}

	FString ClassifyWorldPartitionState(
		const TSharedPtr<FJsonObject>& RuntimeResult,
		const TArray<FString>& LogLines,
		const FString& MapPackage,
		TArray<FString>& OutBasis,
		TArray<FString>& OutLimitations)
	{
		OutBasis.Reset();
		OutLimitations.Reset();

		if (!RuntimeResult.IsValid())
		{
			AddUniqueLimited(OutLimitations, TEXT("No runtime result payload was available to classify world-partition state."), MaxEvidenceEntries);
			return TEXT("not_available");
		}

		FString RuntimeStatus;
		RuntimeResult->TryGetStringField(TEXT("status"), RuntimeStatus);
		if (RuntimeStatus != TEXT("runtime_load_succeeded"))
		{
			AddUniqueLimited(OutLimitations, TEXT("World-partition state is not applicable because the runtime load probe did not succeed."), MaxEvidenceEntries);
			return TEXT("not_applicable");
		}

		const FString MapAssetName = FPackageName::GetLongPackageAssetName(MapPackage);
		bool bSawPartitionInitialize = false;
		bool bSawPartitionStreaming = false;
		bool bSawOwningWorldPartitioned = false;
		for (const FString& Line : LogLines)
		{
			if (Line.Contains(TEXT("UWorldPartition::Initialize")) && (Line.Contains(MapPackage) || Line.Contains(MapAssetName)))
			{
				bSawPartitionInitialize = true;
				AddUniqueLimited(OutBasis, TEXT("runtime_log:UWorldPartition::Initialize"), MaxWorldPartitionBasis);
			}
			if (Line.Contains(TEXT("GenerateStreaming for '")) && Line.Contains(MapAssetName))
			{
				bSawPartitionStreaming = true;
				AddUniqueLimited(OutBasis, TEXT("runtime_log:GenerateStreaming"), MaxWorldPartitionBasis);
			}
			if (Line.Contains(TEXT("bIsOwningWorldPartitioned=1")) && Line.Contains(MapAssetName))
			{
				bSawOwningWorldPartitioned = true;
				AddUniqueLimited(OutBasis, TEXT("runtime_log:bIsOwningWorldPartitioned=1"), MaxWorldPartitionBasis);
			}
		}

		if (bSawPartitionInitialize || bSawPartitionStreaming || bSawOwningWorldPartitioned)
		{
			return TEXT("observed_in_runtime_log");
		}

		bool bEditorPropertyPresent = false;
		if (RuntimeResult->TryGetBoolField(TEXT("world_partition_editor_property_present"), bEditorPropertyPresent) && bEditorPropertyPresent)
		{
			AddUniqueLimited(OutBasis, TEXT("runtime_result:world_partition_editor_property_present"), MaxWorldPartitionBasis);
			return TEXT("observed_in_loaded_world");
		}

		const TArray<TSharedPtr<FJsonValue>>* ClassRows = nullptr;
		if (RuntimeResult->TryGetArrayField(TEXT("top_actor_classes"), ClassRows) && ClassRows)
		{
			for (const TSharedPtr<FJsonValue>& ClassValue : *ClassRows)
			{
				const TSharedPtr<FJsonObject> ClassObject = ClassValue.IsValid() ? ClassValue->AsObject() : nullptr;
				if (!ClassObject.IsValid())
				{
					continue;
				}

				FString ClassName;
				ClassObject->TryGetStringField(TEXT("class"), ClassName);
				if (ClassName == TEXT("WorldDataLayers") || ClassName == TEXT("WorldPartitionMiniMap"))
				{
					AddUniqueLimited(OutBasis, FString::Printf(TEXT("runtime_actor_class:%s"), *ClassName), MaxWorldPartitionBasis);
				}
			}
		}

		if (OutBasis.Num() > 0)
		{
			AddUniqueLimited(OutLimitations, TEXT("World-partition state is only hinted by runtime actor classes in this probe; no direct log/object proof was observed."), MaxEvidenceEntries);
			return TEXT("hinted_by_runtime_facts");
		}

		AddUniqueLimited(OutLimitations, TEXT("No strong world-partition observation was found in the bounded runtime proof lane."), MaxEvidenceEntries);
		return TEXT("not_observed");
	}

	FString BuildMarkdownReport(
		const FString& MapPackage,
		const FString& ProbeMode,
		const FString& ProofResult,
		const FString& RecommendedNextStepClass,
		const FString& RecommendedNextStepReason,
		const TSharedPtr<FJsonObject>& PresenceObject,
		const TSharedPtr<FJsonObject>& RuntimeFacts,
		const TSharedPtr<FJsonObject>& WorldPartitionObject,
		const TArray<FString>& Limitations,
		const TSharedPtr<FJsonObject>& ProbeArtifacts)
	{
		FString Markdown = TEXT("# Map Runtime Proof\n\n");
		Markdown += TEXT("This report captures a bounded map runtime-proof result with explicit separation between current package presence and runtime load proof.\n\n");
		Markdown += TEXT("## Summary\n\n");
		Markdown += FString::Printf(TEXT("- Target map: `%s`\n"), *MapPackage);
		Markdown += FString::Printf(TEXT("- Probe mode: `%s`\n"), *ProbeMode);
		Markdown += TEXT("- Execution mode: `read_only`\n");
		Markdown += FString::Printf(TEXT("- Proof result: `%s`\n"), *ProofResult);

		if (PresenceObject.IsValid())
		{
			Markdown += FString::Printf(TEXT("- Current package present: `%s`\n"), PresenceObject->GetBoolField(TEXT("present")) ? TEXT("true") : TEXT("false"));
			Markdown += FString::Printf(TEXT("- Current map file present: `%s`\n"), PresenceObject->GetBoolField(TEXT("map_file_present")) ? TEXT("true") : TEXT("false"));
		}

		if (WorldPartitionObject.IsValid())
		{
			FString WorldPartitionState;
			if (WorldPartitionObject->TryGetStringField(TEXT("classification"), WorldPartitionState) && !WorldPartitionState.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- World partition state: `%s`\n"), *WorldPartitionState);
			}
		}

		Markdown += TEXT("\n## Truth Boundary\n\n");
		Markdown += TEXT("- Current package presence is not runtime proof.\n");
		Markdown += TEXT("- Headless editor load proof is not gameplay, listen-server, travel, or session proof.\n");
		Markdown += TEXT("- World-partition ambiguity is surfaced as proof strength and limitations instead of being silently assumed.\n");

		if (RuntimeFacts.IsValid())
		{
			Markdown += TEXT("\n## Runtime Facts\n\n");

			FString LoadedWorldName;
			if (RuntimeFacts->TryGetStringField(TEXT("loaded_world_name"), LoadedWorldName) && !LoadedWorldName.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- Loaded world: `%s`\n"), *LoadedWorldName);
			}

			double ActorCount = 0.0;
			if (RuntimeFacts->TryGetNumberField(TEXT("actor_count"), ActorCount))
			{
				Markdown += FString::Printf(TEXT("- Actor count: `%.0f`\n"), ActorCount);
			}

			double PlayerStartCount = 0.0;
			if (RuntimeFacts->TryGetNumberField(TEXT("player_start_count"), PlayerStartCount))
			{
				Markdown += FString::Printf(TEXT("- PlayerStart count: `%.0f`\n"), PlayerStartCount);
			}

			FString WorldSettingsClass;
			if (RuntimeFacts->TryGetStringField(TEXT("world_settings_class"), WorldSettingsClass) && !WorldSettingsClass.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- World settings class: `%s`\n"), *WorldSettingsClass);
			}
		}

		if (ProbeArtifacts.IsValid())
		{
			Markdown += TEXT("\n## Probe Artifacts\n\n");
			FString ResultPath;
			FString ProbeLogPath;
			FString ScriptPath;
			if (ProbeArtifacts->TryGetStringField(TEXT("result_path"), ResultPath) && !ResultPath.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- Result path: `%s`\n"), *ResultPath);
			}
			if (ProbeArtifacts->TryGetStringField(TEXT("log_path"), ProbeLogPath) && !ProbeLogPath.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- Log path: `%s`\n"), *ProbeLogPath);
			}
			if (ProbeArtifacts->TryGetStringField(TEXT("script_path"), ScriptPath) && !ScriptPath.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- Script path: `%s`\n"), *ScriptPath);
			}
		}

		Markdown += TEXT("\n## Limitations\n\n");
		for (const FString& Limitation : Limitations)
		{
			Markdown += FString::Printf(TEXT("- %s\n"), *Limitation);
		}

		Markdown += TEXT("\n## Recommended Next Step\n\n");
		Markdown += FString::Printf(TEXT("- `%s`: %s\n"), *RecommendedNextStepClass, *RecommendedNextStepReason);
		return Markdown;
	}

	FOsvayderUEReportTruthSummary BuildTruthSummary(
		const FString& MapPackage,
		const FString& ProofResult,
		const FCurrentMapPresence& Presence,
		const TArray<FString>& Limitations)
	{
		FOsvayderUEReportTruthSummary TruthSummary;
		TruthSummary.PracticallyVerified.Add(FString::Printf(TEXT("Current package presence was checked for %s."), *MapPackage));

		if (Presence.bPresent)
		{
			TruthSummary.Inspected.Add(TEXT("The target map currently resolves through a map file and/or asset-registry presence."));
		}
		else
		{
			TruthSummary.Inspected.Add(TEXT("The target map does not currently resolve through a map file or asset-registry presence."));
		}

		if (ProofResult == TEXT("runtime_load_succeeded"))
		{
			TruthSummary.PracticallyVerified.Add(TEXT("A separate headless Unreal editor-load probe loaded the target map and returned bounded runtime facts."));
		}
		else if (ProofResult == TEXT("runtime_load_failed"))
		{
			TruthSummary.PracticallyVerified.Add(TEXT("A separate headless Unreal probe executed, but the target map did not produce a successful runtime load result."));
		}
		else
		{
			TruthSummary.Limited.Add(TEXT("The runtime probe was not attempted because the current package presence check was insufficient to justify a bounded load claim."));
		}

		TruthSummary.Limited.Add(TEXT("This slice proves bounded headless editor-load viability only; it does not prove gameplay, listen-server, travel, or session behavior."));
		for (const FString& Limitation : Limitations)
		{
			AddUniqueLimited(TruthSummary.Limited, Limitation, MaxEvidenceEntries);
		}

		TruthSummary.NotVerified.Add(TEXT("Gameplay/listen/session correctness still requires a separate runtime packet if those behaviors matter."));
		return TruthSummary;
	}

	TSharedPtr<FJsonObject> BuildExtraMetadata(
		const FString& MapPackage,
		const FString& ProbeMode,
		const FString& ProofResult,
		const FCurrentMapPresence& Presence,
		const TSharedPtr<FJsonObject>& WorldPartitionObject)
	{
		TSharedPtr<FJsonObject> ExtraMetadata = MakeShared<FJsonObject>();
		ExtraMetadata->SetStringField(TEXT("source_operation"), TEXT("probe_map_runtime_viability"));
		ExtraMetadata->SetStringField(TEXT("map_package"), MapPackage);
		ExtraMetadata->SetStringField(TEXT("probe_mode"), ProbeMode);
		ExtraMetadata->SetStringField(TEXT("proof_result"), ProofResult);
		ExtraMetadata->SetBoolField(TEXT("current_package_present"), Presence.bPresent);
		ExtraMetadata->SetBoolField(TEXT("current_map_file_present"), Presence.bMapFilePresent);
		ExtraMetadata->SetBoolField(TEXT("current_asset_registry_present"), Presence.bAssetRegistryPresent);
		if (WorldPartitionObject.IsValid())
		{
			ExtraMetadata->SetObjectField(TEXT("world_partition_state"), WorldPartitionObject);
		}
		return ExtraMetadata;
	}
}

FMCPToolResult FMCPTool_MapRuntimeProof::Execute(const TSharedRef<FJsonObject>& Params)
{
	const FString Operation = ExtractOptionalString(Params, TEXT("operation"), TEXT("probe_map_runtime_viability"));
	if (!Operation.Equals(TEXT("probe_map_runtime_viability"), ESearchCase::IgnoreCase))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Unsupported operation '%s'. Supported: probe_map_runtime_viability"), *Operation));
	}

	const FString RawMapPath = ExtractOptionalString(Params, TEXT("map_path")).TrimStartAndEnd();
	if (RawMapPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("map_path is required"));
	}

	const FString ProbeMode = ExtractOptionalString(Params, TEXT("probe_mode"), TEXT("headless_editor_load")).TrimStartAndEnd();
	if (!ProbeMode.Equals(TEXT("headless_editor_load"), ESearchCase::IgnoreCase))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Unsupported probe_mode '%s'. Supported: headless_editor_load"), *ProbeMode));
	}

	const FString MapPackage = NormalizePackagePath(RawMapPath);
	if (MapPackage.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("map_path must resolve to a valid long package name"));
	}

	const bool bExportReport = ExtractOptionalBool(Params, TEXT("export_report"), false);
	const FString CustomReportName = ExtractOptionalString(Params, TEXT("report_name"));
	const FString CustomReportSlug = ExtractOptionalString(Params, TEXT("report_slug"));
	const double TimeoutSeconds = FMath::Clamp(static_cast<double>(ExtractOptionalNumber<double>(Params, TEXT("timeout_seconds"), 120.0)), 10.0, 600.0);

	const FCurrentMapPresence Presence = DetermineCurrentMapPresence(MapPackage);
	TArray<FString> EvidenceBasis;
	TArray<FString> Limitations;
	AddUniqueLimited(EvidenceBasis, TEXT("current_state:map_file_presence"), MaxEvidenceEntries);
	AddUniqueLimited(EvidenceBasis, TEXT("current_state:asset_registry_presence"), MaxEvidenceEntries);
	AddUniqueLimited(Limitations, TEXT("Current package presence is not runtime proof."), MaxEvidenceEntries);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("probe_map_runtime_viability"));
	ResultData->SetStringField(TEXT("probe_mode"), ProbeMode);
	ResultData->SetStringField(TEXT("execution_mode"), TEXT("read_only"));
	ResultData->SetStringField(TEXT("map_package"), MapPackage);
	ResultData->SetStringField(TEXT("map_object_path"), MakeMapObjectPath(MapPackage));

	TSharedPtr<FJsonObject> PresenceObject = MakeShared<FJsonObject>();
	SetPresenceObject(PresenceObject, Presence);
	ResultData->SetObjectField(TEXT("current_package_presence"), PresenceObject);

	FString ProofResult = TEXT("runtime_probe_not_attempted");
	FString RecommendedNextStepClass = TEXT("current_state_review");
	FString RecommendedNextStepReason = TEXT("Inspect the current package state before making runtime claims about this map.");
	bool bRuntimeProbeAttempted = false;

	TSharedPtr<FJsonObject> RuntimeFactsObject;
	TSharedPtr<FJsonObject> WorldPartitionObject = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ProbeArtifactsObject = MakeShared<FJsonObject>();

	if (Presence.bPresent)
	{
		const FString RelativePackagePath = MapPackage.StartsWith(TEXT("/Game/")) ? MapPackage.RightChop(6) : FPackageName::GetLongPackageAssetName(MapPackage);
		const FString ExternalActorsDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("__ExternalActors__"), RelativePackagePath);
		const FString ExternalObjectsDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("__ExternalObjects__"), RelativePackagePath);
		const FMapProbePaths ProbePaths = MakeProbePaths(MapPackage);

		const FString ScriptBody = BuildPythonProbeScript(MapPackage, ProbePaths.ResultPath, ExternalActorsDir, ExternalObjectsDir);
		if (!FFileHelper::SaveStringToFile(ScriptBody, *ProbePaths.ScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddUniqueLimited(Limitations, TEXT("Failed to persist the generated runtime probe script."), MaxEvidenceEntries);
			ProofResult = TEXT("runtime_probe_generation_failed");
			RecommendedNextStepClass = TEXT("retry_runtime_probe");
			RecommendedNextStepReason = TEXT("The bounded runtime probe could not start because its generated script was not saved.");
		}
		else
		{
			int32 ProbeExitCode = INDEX_NONE;
			TArray<FString> ExecutionNotes;
			bRuntimeProbeAttempted = true;

			AddUniqueLimited(EvidenceBasis, TEXT("runtime_probe:headless_editor_load"), MaxEvidenceEntries);
			AddUniqueLimited(EvidenceBasis, TEXT("runtime_probe:child_editor_process"), MaxEvidenceEntries);

			const bool bLaunchCompleted = RunHeadlessEditorProbe(ProbePaths.ScriptPath, ProbePaths.LogPath, TimeoutSeconds, ProbeExitCode, ExecutionNotes);
			const bool bResultReady = WaitForFile(ProbePaths.ResultPath, 2.0);
			const bool bLogReady = WaitForFile(ProbePaths.LogPath, 1.0);

			ProbeArtifactsObject->SetStringField(TEXT("probe_id"), ProbePaths.ProbeId);
			ProbeArtifactsObject->SetStringField(TEXT("probe_directory"), ProbePaths.ProbeDirectory);
			ProbeArtifactsObject->SetStringField(TEXT("script_path"), ProbePaths.ScriptPath);
			ProbeArtifactsObject->SetStringField(TEXT("result_path"), ProbePaths.ResultPath);
			ProbeArtifactsObject->SetStringField(TEXT("log_path"), ProbePaths.LogPath);
			ProbeArtifactsObject->SetBoolField(TEXT("result_file_present"), bResultReady);
			ProbeArtifactsObject->SetBoolField(TEXT("log_file_present"), bLogReady);
			if (ProbeExitCode != INDEX_NONE)
			{
				ProbeArtifactsObject->SetNumberField(TEXT("process_exit_code"), ProbeExitCode);
			}
			SetJsonStringArrayField(ProbeArtifactsObject, TEXT("execution_notes"), ExecutionNotes);

			TSharedPtr<FJsonObject> RuntimeResult;
			if (bResultReady && LoadJsonObjectFromFile(ProbePaths.ResultPath, RuntimeResult))
			{
				RuntimeFactsObject = RuntimeResult;
				ResultData->SetObjectField(TEXT("runtime_facts"), RuntimeResult);

				FString RuntimeStatus;
				RuntimeResult->TryGetStringField(TEXT("status"), RuntimeStatus);
				if (RuntimeStatus == TEXT("runtime_load_succeeded"))
				{
					ProofResult = TEXT("runtime_load_succeeded");
					RecommendedNextStepClass = TEXT("gameplay_or_listen_probe_next");
					RecommendedNextStepReason = TEXT("The map proved bounded headless editor-load viability, but gameplay/listen/session behavior remains unproven.");
					AddUniqueLimited(Limitations, TEXT("Headless editor load proved the map could be loaded in a bounded child-editor lane, but it does not prove gameplay or session behavior."), MaxEvidenceEntries);
				}
				else
				{
					ProofResult = TEXT("runtime_load_failed");
					RecommendedNextStepClass = TEXT("review_runtime_probe_failure");
					RecommendedNextStepReason = TEXT("The bounded headless runtime probe ran, but the target map did not produce a successful load result.");

					FString ErrorMessage;
					RuntimeResult->TryGetStringField(TEXT("error_message"), ErrorMessage);
					if (!ErrorMessage.IsEmpty())
					{
						AddUniqueLimited(Limitations, FString::Printf(TEXT("Runtime load failure detail: %s"), *ErrorMessage), MaxEvidenceEntries);
					}
				}

				if (ProbeExitCode != INDEX_NONE && ProbeExitCode != 0)
				{
					AddUniqueLimited(Limitations, FString::Printf(TEXT("The child probe returned process exit code %d; runtime result classification relied on the written probe payload instead of exit code alone."), ProbeExitCode), MaxEvidenceEntries);
				}

				TArray<FString> LogLines;
				if (bLogReady)
				{
					FFileHelper::LoadFileToStringArray(LogLines, *ProbePaths.LogPath);
				}

				TArray<FString> WorldPartitionBasis;
				TArray<FString> WorldPartitionLimitations;
				const FString WorldPartitionState = ClassifyWorldPartitionState(RuntimeResult, LogLines, MapPackage, WorldPartitionBasis, WorldPartitionLimitations);
				WorldPartitionObject->SetStringField(TEXT("classification"), WorldPartitionState);
				WorldPartitionObject->SetStringField(
					TEXT("proof_strength"),
					(WorldPartitionState == TEXT("observed_in_runtime_log") || WorldPartitionState == TEXT("observed_in_loaded_world"))
						? TEXT("strong")
						: (WorldPartitionState == TEXT("hinted_by_runtime_facts") ? TEXT("weak") : TEXT("none")));
				SetJsonStringArrayField(WorldPartitionObject, TEXT("basis"), WorldPartitionBasis);
				SetJsonStringArrayField(WorldPartitionObject, TEXT("limitations"), WorldPartitionLimitations);
				for (const FString& Basis : WorldPartitionBasis)
				{
					AddUniqueLimited(EvidenceBasis, Basis, MaxEvidenceEntries);
				}
				for (const FString& Limitation : WorldPartitionLimitations)
				{
					AddUniqueLimited(Limitations, Limitation, MaxEvidenceEntries);
				}
			}
			else
			{
				ProofResult = bLaunchCompleted ? TEXT("runtime_probe_missing_result") : TEXT("runtime_probe_execution_failed");
				RecommendedNextStepClass = TEXT("retry_runtime_probe");
				RecommendedNextStepReason = TEXT("The bounded runtime probe did not produce a readable result payload.");
				AddUniqueLimited(Limitations, TEXT("The child probe did not produce a readable runtime result payload."), MaxEvidenceEntries);
			}
		}
	}
	else
	{
		ProofResult = TEXT("runtime_probe_not_attempted");
		RecommendedNextStepClass = TEXT("fix_target_map_input");
		RecommendedNextStepReason = TEXT("The target map does not currently resolve through file or asset-registry presence, so a runtime load claim would overreach.");
		AddUniqueLimited(Limitations, TEXT("The runtime probe was intentionally not attempted because the target map is not currently present in a bounded current-state check."), MaxEvidenceEntries);
		WorldPartitionObject->SetStringField(TEXT("classification"), TEXT("not_applicable"));
		WorldPartitionObject->SetStringField(TEXT("proof_strength"), TEXT("none"));
	}

	ResultData->SetBoolField(TEXT("runtime_probe_attempted"), bRuntimeProbeAttempted);
	ResultData->SetStringField(TEXT("proof_result"), ProofResult);
	ResultData->SetStringField(TEXT("recommended_next_step_class"), RecommendedNextStepClass);
	ResultData->SetStringField(TEXT("recommended_next_step_reason"), RecommendedNextStepReason);
	SetJsonStringArrayField(ResultData, TEXT("evidence_basis"), EvidenceBasis);
	SetJsonStringArrayField(ResultData, TEXT("limitations"), Limitations);
	ResultData->SetObjectField(TEXT("world_partition_state"), WorldPartitionObject);
	if (ProbeArtifactsObject->Values.Num() > 0)
	{
		ResultData->SetObjectField(TEXT("probe_artifacts"), ProbeArtifactsObject);
	}

	FString Message = FString::Printf(TEXT("map_runtime_proof for %s -> %s"), *MapPackage, *ProofResult);

	if (bExportReport)
	{
		FOsvayderUEReportExportRequest ExportRequest;
		ExportRequest.ReportName = !CustomReportName.TrimStartAndEnd().IsEmpty()
			? CustomReportName
			: FString::Printf(TEXT("Map Runtime Proof - %s"), *FPackageName::GetLongPackageAssetName(MapPackage));
		ExportRequest.ReportSlug = !CustomReportSlug.TrimStartAndEnd().IsEmpty()
			? CustomReportSlug
			: TEXT("map_runtime_proof");
		ExportRequest.Markdown = BuildMarkdownReport(
			MapPackage,
			ProbeMode,
			ProofResult,
			RecommendedNextStepClass,
			RecommendedNextStepReason,
			PresenceObject,
			RuntimeFactsObject,
			WorldPartitionObject,
			Limitations,
			ProbeArtifactsObject);
		ExportRequest.SummaryText = Message;
		ExportRequest.RunKind = TEXT("map_runtime_proof");
		ExportRequest.ExecutionMode = TEXT("read_only");
		ExportRequest.ToolNames = { TEXT("map_runtime_proof") };
		ExportRequest.ToolFamilies = { TEXT("runtime_proof") };
		ExportRequest.EvidenceClasses = { TEXT("current_package_presence"), TEXT("headless_editor_load_runtime_probe"), TEXT("runtime_log_observation") };
		ExportRequest.TruthSummary = BuildTruthSummary(MapPackage, ProofResult, Presence, Limitations);
		ExportRequest.ExtraMetadata = BuildExtraMetadata(MapPackage, ProbeMode, ProofResult, Presence, WorldPartitionObject);

		FOsvayderUEReportExportResult ExportResult;
		if (!FOsvayderUEReportArtifacts::ExportReport(ExportRequest, ExportResult))
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
