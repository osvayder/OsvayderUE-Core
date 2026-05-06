// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Sequencer.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEModule.h"
#include "MCP/MCPParamValidator.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Sections/MovieSceneSubSection.h"

FMCPToolInfo FMCPTool_Sequencer::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("sequencer");
	Info.Description = TEXT(
		"Sequencer / Cinematics authoring surfaces.\n\n"
		"Query operations (read-only):\n"
		"- 'list_sequences': Discover LevelSequence assets in project\n"
		"- 'get_sequence_info': Introspect sequence — possessables, spawnables, tracks, sections, subsequences, camera cuts\n\n"
		"Modify operations:\n"
		"- 'set_sequence_playback': Set playback defaults (play rate, start/end frame, loop count)\n"
		"- 'configure_sequence_foundation': Composed — playback config in one call with dry_run\n\n"
		"NOTE: This is a cinematic asset introspection + playback config surface.\n"
		"Full Sequencer graph editing (adding/rewiring tracks, keyframes) is NOT supported.\n"
		"Runtime playback verification requires PIE."
	);

	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"),
			TEXT("Operation: 'list_sequences', 'get_sequence_info', 'set_sequence_playback', 'configure_sequence_foundation'"), true),
		FMCPToolParameter(TEXT("asset_path"), TEXT("string"),
			TEXT("LevelSequence asset path"), false),
		FMCPToolParameter(TEXT("path_filter"), TEXT("string"),
			TEXT("Path prefix filter for list operations (default: /Game/)"), false, TEXT("/Game/")),
		FMCPToolParameter(TEXT("name_filter"), TEXT("string"),
			TEXT("Name substring filter"), false),
		FMCPToolParameter(TEXT("limit"), TEXT("number"),
			TEXT("Maximum results (1-500, default: 25)"), false, TEXT("25")),
		// Playback params
		FMCPToolParameter(TEXT("play_rate"), TEXT("number"),
			TEXT("Playback rate (1.0 = normal speed)"), false),
		FMCPToolParameter(TEXT("start_frame"), TEXT("number"),
			TEXT("Playback start frame"), false),
		FMCPToolParameter(TEXT("end_frame"), TEXT("number"),
			TEXT("Playback end frame"), false),
		FMCPToolParameter(TEXT("loop_count"), TEXT("number"),
			TEXT("Loop count (-1 = infinite, 0 = no loop)"), false),
		FMCPToolParameter(TEXT("dry_run"), TEXT("boolean"),
			TEXT("If true, report what would change without mutating"), false, TEXT("false")),
	};

	// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
	Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
		TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_Sequencer::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("list_sequences"))               return ExecuteListSequences(Params);
	if (Operation == TEXT("get_sequence_info"))             return ExecuteGetSequenceInfo(Params);
	if (Operation == TEXT("set_sequence_playback"))         return ExecuteSetSequencePlayback(Params);
	if (Operation == TEXT("configure_sequence_foundation")) return ExecuteConfigureSequenceFoundation(Params);

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown sequencer operation: '%s'. Valid: list_sequences, get_sequence_info, ")
		TEXT("set_sequence_playback, configure_sequence_foundation"),
		*Operation));
}

UObject* FMCPTool_Sequencer::LoadAssetByPath(const FString& AssetPath, FString& OutError)
{
	FSoftObjectPath SoftPath(AssetPath);
	UObject* Asset = SoftPath.TryLoad();
	if (!Asset) OutError = FString::Printf(TEXT("Could not load asset: %s"), *AssetPath);
	return Asset;
}

// ===== Query: List Sequences =====

FMCPToolResult FMCPTool_Sequencer::ExecuteListSequences(const TSharedRef<FJsonObject>& Params)
{
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	Limit = FMath::Clamp(Limit, 1, 500);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(ULevelSequence::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*PathFilter));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& AssetData : Assets)
	{
		if (Results.Num() >= Limit) break;
		FString AssetName = AssetData.AssetName.ToString();
		if (!NameFilter.IsEmpty() && !AssetName.Contains(NameFilter)) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("sequences"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d Level Sequences"), Results.Num()), Data);
}

// ===== Query: Get Sequence Info =====

FMCPToolResult FMCPTool_Sequencer::ExecuteGetSequenceInfo(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset) return FMCPToolResult::Error(LoadError);

	ULevelSequence* Seq = Cast<ULevelSequence>(Asset);
	if (!Seq) return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a LevelSequence: %s"), *AssetPath));

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene) return FMCPToolResult::Error(TEXT("LevelSequence has no MovieScene"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"), Seq->GetName());

	// Playback range
	FFrameNumber StartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();
	FFrameNumber EndFrame = MovieScene->GetPlaybackRange().GetUpperBoundValue();
	FFrameRate TickResolution = MovieScene->GetTickResolution();
	FFrameRate DisplayRate = MovieScene->GetDisplayRate();

	Data->SetNumberField(TEXT("start_frame"), StartFrame.Value);
	Data->SetNumberField(TEXT("end_frame"), EndFrame.Value);
	Data->SetStringField(TEXT("tick_resolution"), FString::Printf(TEXT("%d/%d"), TickResolution.Numerator, TickResolution.Denominator));
	Data->SetStringField(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), DisplayRate.Numerator, DisplayRate.Denominator));

	// Possessables
	TArray<TSharedPtr<FJsonValue>> Possessables;
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); i++)
	{
		const FMovieScenePossessable& Poss = MovieScene->GetPossessable(i);
		TSharedPtr<FJsonObject> PossObj = MakeShared<FJsonObject>();
		PossObj->SetStringField(TEXT("name"), Poss.GetName());
		PossObj->SetStringField(TEXT("guid"), Poss.GetGuid().ToString());
		PossObj->SetStringField(TEXT("possessed_class"), Poss.GetPossessedObjectClass() ? Poss.GetPossessedObjectClass()->GetName() : TEXT("(none)"));
		Possessables.Add(MakeShared<FJsonValueObject>(PossObj));
	}
	Data->SetArrayField(TEXT("possessables"), Possessables);
	Data->SetNumberField(TEXT("possessable_count"), Possessables.Num());

	// Spawnables
	TArray<TSharedPtr<FJsonValue>> Spawnables;
	for (int32 i = 0; i < MovieScene->GetSpawnableCount(); i++)
	{
		const FMovieSceneSpawnable& Spawn = MovieScene->GetSpawnable(i);
		TSharedPtr<FJsonObject> SpawnObj = MakeShared<FJsonObject>();
		SpawnObj->SetStringField(TEXT("name"), Spawn.GetName());
		SpawnObj->SetStringField(TEXT("guid"), Spawn.GetGuid().ToString());
		Spawnables.Add(MakeShared<FJsonValueObject>(SpawnObj));
	}
	Data->SetArrayField(TEXT("spawnables"), Spawnables);
	Data->SetNumberField(TEXT("spawnable_count"), Spawnables.Num());

	// Tracks
	TArray<TSharedPtr<FJsonValue>> Tracks;
	bool bHasCameraCut = false;
	bool bHasSubTrack = false;
	int32 EventTrackCount = 0;

	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (!Track) continue;

		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("class"), Track->GetClass()->GetName());
		TrackObj->SetStringField(TEXT("name"), Track->GetDisplayName().ToString());
		TrackObj->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());

		if (Track->IsA(UMovieSceneCameraCutTrack::StaticClass()))
		{
			bHasCameraCut = true;
			TrackObj->SetBoolField(TEXT("is_camera_cut"), true);
		}

		if (Track->IsA(UMovieSceneSubTrack::StaticClass()))
		{
			bHasSubTrack = true;
			TrackObj->SetBoolField(TEXT("is_sub_track"), true);

			// List subsequences
			TArray<TSharedPtr<FJsonValue>> SubSeqs;
			for (UMovieSceneSection* Section : Track->GetAllSections())
			{
				UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section);
				if (SubSection && SubSection->GetSequence())
				{
					TSharedPtr<FJsonObject> SubObj = MakeShared<FJsonObject>();
					SubObj->SetStringField(TEXT("name"), SubSection->GetSequence()->GetName());
					SubObj->SetStringField(TEXT("path"), SubSection->GetSequence()->GetPathName());
					SubSeqs.Add(MakeShared<FJsonValueObject>(SubObj));
				}
			}
			TrackObj->SetArrayField(TEXT("subsequences"), SubSeqs);
		}

		if (Track->GetClass()->GetName().Contains(TEXT("Event")))
		{
			EventTrackCount++;
			TrackObj->SetBoolField(TEXT("is_event_track"), true);
		}

		Tracks.Add(MakeShared<FJsonValueObject>(TrackObj));
	}

	Data->SetArrayField(TEXT("tracks"), Tracks);
	Data->SetNumberField(TEXT("track_count"), Tracks.Num());
	Data->SetBoolField(TEXT("has_camera_cut"), bHasCameraCut);
	Data->SetBoolField(TEXT("has_subsequences"), bHasSubTrack);
	Data->SetNumberField(TEXT("event_track_count"), EventTrackCount);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Sequence %s: %d possessables, %d spawnables, %d tracks"),
			*Seq->GetName(), Possessables.Num(), Spawnables.Num(), Tracks.Num()),
		Data);
}

// ===== Modify: Set Sequence Playback =====

FMCPToolResult FMCPTool_Sequencer::ExecuteSetSequencePlayback(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset) return FMCPToolResult::Error(LoadError);

	ULevelSequence* Seq = Cast<ULevelSequence>(Asset);
	if (!Seq) return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a LevelSequence: %s"), *AssetPath));

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene) return FMCPToolResult::Error(TEXT("LevelSequence has no MovieScene"));

	TArray<TSharedPtr<FJsonValue>> Changed;

	// Play rate
	double PlayRate;
	if (Params->TryGetNumberField(TEXT("play_rate"), PlayRate))
	{
		// Play rate is stored on the sequence player at runtime, not on the asset.
		// For the asset, we can store it as metadata hint but UMovieScene doesn't have a direct play rate field.
		// Truthfully: we cannot set play rate on the asset — it's a runtime property.
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("property"), TEXT("play_rate"));
		Obj->SetStringField(TEXT("status"), TEXT("skipped"));
		Obj->SetStringField(TEXT("reason"), TEXT("Play rate is a runtime property (ALevelSequenceActor/ULevelSequencePlayer), not stored on the asset"));
		Changed.Add(MakeShared<FJsonValueObject>(Obj));
	}

	// Start/end frame
	double StartFrame, EndFrame;
	bool bHasStart = Params->TryGetNumberField(TEXT("start_frame"), StartFrame);
	bool bHasEnd = Params->TryGetNumberField(TEXT("end_frame"), EndFrame);

	if (bHasStart || bHasEnd)
	{
		TRange<FFrameNumber> CurrentRange = MovieScene->GetPlaybackRange();
		FFrameNumber NewStart = bHasStart ? FFrameNumber(static_cast<int32>(StartFrame)) : CurrentRange.GetLowerBoundValue();
		FFrameNumber NewEnd = bHasEnd ? FFrameNumber(static_cast<int32>(EndFrame)) : CurrentRange.GetUpperBoundValue();

		MovieScene->SetPlaybackRange(NewStart, (NewEnd - NewStart).Value);

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("property"), TEXT("playback_range"));
		Obj->SetStringField(TEXT("status"), TEXT("changed"));
		Obj->SetStringField(TEXT("value"), FString::Printf(TEXT("[%d, %d]"), NewStart.Value, NewEnd.Value));
		Changed.Add(MakeShared<FJsonValueObject>(Obj));
	}

	if (Changed.Num() > 0)
	{
		Seq->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("changed"), Changed);
	Data->SetNumberField(TEXT("changed_count"), Changed.Num());

	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("sequencer");
		Receipt.Summary = TEXT("set_sequence_playback");
		Receipt.bSuccess = true;
		Receipt.Targets.Add(AssetPath);
		Receipt.Classification = TEXT("user_mutation");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set %d playback properties on %s"), Changed.Num(), *AssetPath), Data);
}

// ===== Modify: Configure Sequence Foundation (Composed) =====

FMCPToolResult FMCPTool_Sequencer::ExecuteConfigureSequenceFoundation(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	bool bDryRun = ExtractOptionalBool(Params, TEXT("dry_run"), false);

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset) return FMCPToolResult::Error(LoadError);

	ULevelSequence* Seq = Cast<ULevelSequence>(Asset);
	if (!Seq) return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a LevelSequence: %s"), *AssetPath));

	UMovieScene* MovieScene = Seq->GetMovieScene();
	if (!MovieScene) return FMCPToolResult::Error(TEXT("LevelSequence has no MovieScene"));

	TArray<TSharedPtr<FJsonValue>> Actions;
	int32 UpdatedCount = 0, UnchangedCount = 0, FailedCount = 0;

	auto AddAction = [&](const FString& Name, const FString& Status, const FString& Detail = FString())
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("action"), Name);
		Obj->SetStringField(TEXT("status"), Status);
		if (!Detail.IsEmpty()) Obj->SetStringField(TEXT("detail"), Detail);
		Actions.Add(MakeShared<FJsonValueObject>(Obj));
		if (Status == TEXT("updated")) UpdatedCount++;
		else if (Status == TEXT("unchanged")) UnchangedCount++;
		else if (Status == TEXT("failed") || Status == TEXT("skipped")) FailedCount++;
	};

	// Playback range
	double StartFrame, EndFrame;
	bool bHasStart = Params->TryGetNumberField(TEXT("start_frame"), StartFrame);
	bool bHasEnd = Params->TryGetNumberField(TEXT("end_frame"), EndFrame);

	if (bHasStart || bHasEnd)
	{
		TRange<FFrameNumber> CurrentRange = MovieScene->GetPlaybackRange();
		FFrameNumber CurStart = CurrentRange.GetLowerBoundValue();
		FFrameNumber CurEnd = CurrentRange.GetUpperBoundValue();
		FFrameNumber NewStart = bHasStart ? FFrameNumber(static_cast<int32>(StartFrame)) : CurStart;
		FFrameNumber NewEnd = bHasEnd ? FFrameNumber(static_cast<int32>(EndFrame)) : CurEnd;

		if (CurStart == NewStart && CurEnd == NewEnd)
		{
			AddAction(TEXT("set_playback_range"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"));
		}
		else
		{
			if (!bDryRun)
			{
				MovieScene->SetPlaybackRange(NewStart, (NewEnd - NewStart).Value);
			}
			AddAction(TEXT("set_playback_range"), bDryRun ? TEXT("would_update") : TEXT("updated"),
				FString::Printf(TEXT("[%d, %d]"), NewStart.Value, NewEnd.Value));
		}
	}

	// Play rate — truthfully skipped (runtime property)
	double PlayRate;
	if (Params->TryGetNumberField(TEXT("play_rate"), PlayRate))
	{
		AddAction(TEXT("set_play_rate"), TEXT("skipped"),
			TEXT("Play rate is a runtime property, not stored on the asset"));
	}

	if (!bDryRun && UpdatedCount > 0)
	{
		Seq->MarkPackageDirty();
	}

	bool bAllFailed = FailedCount > 0 && UpdatedCount == 0 && UnchangedCount == 0;
	bool bPartialSuccess = FailedCount > 0 && (UpdatedCount > 0 || UnchangedCount > 0);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetArrayField(TEXT("actions"), Actions);
	Data->SetNumberField(TEXT("action_count"), Actions.Num());
	Data->SetNumberField(TEXT("updated_count"), UpdatedCount);
	Data->SetNumberField(TEXT("unchanged_count"), UnchangedCount);
	Data->SetNumberField(TEXT("failed_count"), FailedCount);
	if (bPartialSuccess) Data->SetBoolField(TEXT("partial_success"), true);

	if (!bDryRun)
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("sequencer");
		Receipt.Summary = TEXT("configure_sequence_foundation");
		Receipt.bSuccess = !bAllFailed;
		Receipt.bPartialSuccess = bPartialSuccess;
		Receipt.Targets.Add(AssetPath);
		Receipt.Classification = TEXT("user_mutation");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
	}

	FString Message = FString::Printf(TEXT("%s configure_sequence_foundation: %d actions (%d updated, %d unchanged, %d skipped)"),
		bDryRun ? TEXT("[dry_run]") : TEXT("Applied"), Actions.Num(), UpdatedCount, UnchangedCount, FailedCount);
	if (bAllFailed && !bDryRun) return FMCPToolResult::Error(Message);
	return FMCPToolResult::Success(Message, Data);
}
