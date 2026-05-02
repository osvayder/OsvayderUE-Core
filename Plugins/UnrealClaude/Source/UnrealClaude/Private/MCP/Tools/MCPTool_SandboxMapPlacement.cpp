#include "MCPTool_SandboxMapPlacement.h"

#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "MCP/MCPSavePipeline.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClaudeModule.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
	FString MakeUtcStamp()
	{
		return FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	}

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

	TArray<FString> SnapshotDirtyPackageNames()
	{
		TArray<FString> Names;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (Package && Package->IsDirty())
			{
				Names.Add(Package->GetName());
			}
		}
		Names.Sort();
		return Names;
	}

	TArray<FString> PackageNamesFromResults(const TArray<UnrealClaude::SavePipeline::FLifecyclePhaseResult>& Results)
	{
		TArray<FString> Names;
		Names.Reserve(Results.Num());
		for (const UnrealClaude::SavePipeline::FLifecyclePhaseResult& Result : Results)
		{
			Names.Add(Result.PackageName);
		}
		Names.Sort();
		return Names;
	}

	TArray<TSharedPtr<FJsonValue>> PhaseResultsToJson(const TArray<UnrealClaude::SavePipeline::FLifecyclePhaseResult>& Results)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Results.Num());
		for (const UnrealClaude::SavePipeline::FLifecyclePhaseResult& Result : Results)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("package"), Result.PackageName);
			Row->SetStringField(TEXT("phase"), Result.Phase);
			Row->SetBoolField(TEXT("success"), Result.bSuccess);
			if (!Result.Error.IsEmpty())
			{
				Row->SetStringField(TEXT("error"), Result.Error);
			}
			if (!Result.SkippedReason.IsEmpty())
			{
				Row->SetStringField(TEXT("skipped_reason"), Result.SkippedReason);
			}
			JsonValues.Add(MakeShared<FJsonValueObject>(Row));
		}
		return JsonValues;
	}

	bool ArrayContainsString(const TArray<FString>& Values, const FString& Needle)
	{
		return Values.ContainsByPredicate([&Needle](const FString& Value)
		{
			return Value.Equals(Needle, ESearchCase::IgnoreCase);
		});
	}

	bool AreAllSavedPackagesSandboxed(const TArray<FString>& SavedPackages)
	{
		for (const FString& PackageName : SavedPackages)
		{
			FString PacketLabel;
			if (!FMCPTool_SandboxMapPlacement::TryExtractPacketLabelFromSandboxMapPath(PackageName, PacketLabel))
			{
				return false;
			}
		}
		return true;
	}

	FMCPToolResult MakeSandboxPlacementError(const FString& Message, const FString& Reason, const FString& MapPath)
	{
		FMCPToolResult Result = FMCPToolResult::Error(Message);
		if (!Result.Data.IsValid())
		{
			Result.Data = MakeShared<FJsonObject>();
		}
		Result.Data->SetStringField(TEXT("result_type"), TEXT("sandbox_map_placement_denied"));
		Result.Data->SetStringField(TEXT("schema_version"), TEXT("unrealclaude_sandbox_map_placement.v1"));
		Result.Data->SetStringField(TEXT("denial_reason"), Reason);
		Result.Data->SetStringField(TEXT("map_path"), MapPath);
		Result.Data->SetStringField(TEXT("sandbox_prefix"), FMCPTool_SandboxMapPlacement::GetPacketSandboxPrefix());
		Result.Data->SetBoolField(TEXT("production_map_mutation_prevented"), true);
		return Result;
	}

	TSharedPtr<FJsonObject> BuildSaveLifecycleJson(
		const FString& MapPath,
		const TArray<FString>& DirtyBeforeTool,
		const TArray<FString>& DirtyBeforePlacementSave,
		const TArray<FString>& DirtyAfterPlacementSave,
		const UnrealClaude::SavePipeline::FLifecycleOutcome& Outcome)
	{
		const TArray<FString> SavedPackages = PackageNamesFromResults(Outcome.Saved);
		const TArray<FString> FailedPackages = PackageNamesFromResults(Outcome.Failed);
		const TArray<FString> DeferredPackages = PackageNamesFromResults(Outcome.Deferred);

		TSharedPtr<FJsonObject> Lifecycle = MakeShared<FJsonObject>();
		Lifecycle->SetStringField(TEXT("schema_version"), TEXT("unrealclaude_sandbox_map_lifecycle.v1"));
		Lifecycle->SetBoolField(TEXT("auto_save"), true);
		Lifecycle->SetStringField(TEXT("save_policy"), TEXT("explicit_sandbox_map_save"));
		Lifecycle->SetStringField(TEXT("sandbox_prefix"), FMCPTool_SandboxMapPlacement::GetPacketSandboxPrefix());
		FString PacketLabel;
		if (FMCPTool_SandboxMapPlacement::TryExtractPacketLabelFromSandboxMapPath(MapPath, PacketLabel))
		{
			Lifecycle->SetStringField(TEXT("packet_label"), PacketLabel);
		}
		Lifecycle->SetStringField(TEXT("target_map_package"), MapPath);
		SetStringArrayField(Lifecycle, TEXT("dirty_before_tool"), DirtyBeforeTool);
		SetStringArrayField(Lifecycle, TEXT("dirty_before_placement_save"), DirtyBeforePlacementSave);
		SetStringArrayField(Lifecycle, TEXT("dirty_after_placement_save"), DirtyAfterPlacementSave);
		SetStringArrayField(Lifecycle, TEXT("saved_packages"), SavedPackages);
		SetStringArrayField(Lifecycle, TEXT("failed_packages"), FailedPackages);
		SetStringArrayField(Lifecycle, TEXT("deferred_packages"), DeferredPackages);
		TArray<FString> SaveTargets;
		SaveTargets.Add(MapPath);
		SetStringArrayField(Lifecycle, TEXT("save_targets"), SaveTargets);
		Lifecycle->SetArrayField(TEXT("saved"), PhaseResultsToJson(Outcome.Saved));
		Lifecycle->SetArrayField(TEXT("failed"), PhaseResultsToJson(Outcome.Failed));
		Lifecycle->SetArrayField(TEXT("deferred"), PhaseResultsToJson(Outcome.Deferred));
		Lifecycle->SetBoolField(TEXT("only_sandbox_packages_saved"), AreAllSavedPackagesSandboxed(SavedPackages));
		Lifecycle->SetBoolField(TEXT("target_map_saved"), ArrayContainsString(SavedPackages, MapPath));
		Lifecycle->SetBoolField(TEXT("unrelated_dirty_packages_saved"), false);
		return Lifecycle;
	}

	FString MakeProofArtifactPath(const FString& MapPath)
	{
		FString PacketLabel = TEXT("packet_sandbox");
		FString ExtractedPacketLabel;
		if (FMCPTool_SandboxMapPlacement::TryExtractPacketLabelFromSandboxMapPath(MapPath, ExtractedPacketLabel))
		{
			PacketLabel = ExtractedPacketLabel;
		}
		const FString ArtifactRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), PacketLabel + TEXT("_sandbox_map_placement"));
		IFileManager::Get().MakeDirectory(*ArtifactRoot, true);
		const FString MapLabel = FPackageName::GetLongPackageAssetName(MapPath);
		return FPaths::Combine(ArtifactRoot, FString::Printf(TEXT("%s_%s_%s_placement_proof.json"), *PacketLabel, *MakeUtcStamp(), *MapLabel));
	}

	bool WriteProofArtifact(const TSharedPtr<FJsonObject>& ResultData, FString& OutProofPath)
	{
		if (!ResultData.IsValid())
		{
			return false;
		}

		FString MapPath;
		ResultData->TryGetStringField(TEXT("map_path"), MapPath);
		OutProofPath = MakeProofArtifactPath(MapPath);
		ResultData->SetStringField(TEXT("proof_artifact_path"), OutProofPath);

		FString JsonString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
		if (!FJsonSerializer::Serialize(ResultData.ToSharedRef(), Writer))
		{
			return false;
		}

		return FFileHelper::SaveStringToFile(JsonString, *OutProofPath);
	}
}

FString FMCPTool_SandboxMapPlacement::GetSandboxRootPath()
{
	return TEXT("/Game/__UnrealClaudeTestSandbox/");
}

FString FMCPTool_SandboxMapPlacement::GetPacketSandboxPrefix()
{
	return GetSandboxRootPath() + TEXT("packet");
}

bool FMCPTool_SandboxMapPlacement::TryExtractPacketLabelFromSandboxMapPath(const FString& MapPath, FString& OutPacketLabel)
{
	OutPacketLabel.Reset();

	const FString Candidate = MapPath.TrimStartAndEnd();
	if (!Candidate.StartsWith(GetSandboxRootPath(), ESearchCase::IgnoreCase))
	{
		return false;
	}

	const FString RelativePath = Candidate.RightChop(GetSandboxRootPath().Len());
	int32 SlashIndex = INDEX_NONE;
	if (!RelativePath.FindChar(TEXT("/")[0], SlashIndex) || SlashIndex <= 0)
	{
		return false;
	}

	const FString FirstSegment = RelativePath.Left(SlashIndex);
	int32 UnderscoreIndex = INDEX_NONE;
	if (!FirstSegment.StartsWith(TEXT("packet"), ESearchCase::IgnoreCase)
		|| !FirstSegment.FindChar(TEXT("_")[0], UnderscoreIndex)
		|| UnderscoreIndex <= 6)
	{
		return false;
	}

	for (int32 Index = 6; Index < UnderscoreIndex; ++Index)
	{
		if (!FChar::IsDigit(FirstSegment[Index]))
		{
			return false;
		}
	}

	OutPacketLabel = FirstSegment.Left(UnderscoreIndex).ToLower();
	return true;
}

bool FMCPTool_SandboxMapPlacement::NormalizeAndValidateSandboxMapPath(
	const FString& InPath,
	FString& OutMapPath,
	FString& OutError,
	FString& OutReason)
{
	FString Candidate = InPath.TrimStartAndEnd();
	OutMapPath.Reset();
	OutError.Reset();
	OutReason.Reset();

	if (Candidate.IsEmpty())
	{
		OutError = TEXT("Sandbox map path cannot be empty.");
		OutReason = TEXT("empty_map_path");
		return false;
	}

	if (Candidate.Contains(TEXT("\\")) || Candidate.Contains(TEXT(":")))
	{
		OutError = TEXT("Sandbox map path must be a long package path, not a raw disk path.");
		OutReason = TEXT("raw_disk_path");
		return false;
	}

	if (Candidate.Contains(TEXT("..")))
	{
		OutError = TEXT("Sandbox map path cannot contain path traversal sequences.");
		OutReason = TEXT("path_traversal");
		return false;
	}

	if (Candidate.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase) || Candidate.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Sandbox map path must omit disk file extensions and use a long package path.");
		OutReason = TEXT("raw_disk_path");
		return false;
	}

	if (Candidate.Contains(TEXT(".")))
	{
		OutError = TEXT("Sandbox map path must be a package path, not an object path.");
		OutReason = TEXT("object_path_not_allowed");
		return false;
	}

	FText InvalidReason;
	if (!FPackageName::IsValidLongPackageName(Candidate, false, &InvalidReason))
	{
		OutError = FString::Printf(TEXT("Invalid sandbox map package path '%s': %s"), *Candidate, *InvalidReason.ToString());
		OutReason = TEXT("invalid_package_path");
		return false;
	}

	FString PacketLabel;
	if (!TryExtractPacketLabelFromSandboxMapPath(Candidate, PacketLabel))
	{
		OutError = FString::Printf(TEXT("Map placement is restricted to sandbox paths under %spacket<digits>_*"), *GetSandboxRootPath());
		OutReason = TEXT("non_sandbox_map_path");
		return false;
	}

	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(Candidate, Filename, FPackageName::GetMapPackageExtension()))
	{
		OutError = FString::Printf(TEXT("Could not resolve sandbox map package path '%s' to a map filename."), *Candidate);
		OutReason = TEXT("unresolvable_package_path");
		return false;
	}

	OutMapPath = Candidate;
	return true;
}

FMCPToolResult FMCPTool_SandboxMapPlacement::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString RawMapPath;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("map_path"), RawMapPath, ParamError))
	{
		return ParamError.GetValue();
	}

	FString MapPath;
	FString ValidationError;
	FString DenialReason;
	if (!NormalizeAndValidateSandboxMapPath(RawMapPath, MapPath, ValidationError, DenialReason))
	{
		return MakeSandboxPlacementError(ValidationError, DenialReason, RawMapPath);
	}

	const bool bReplaceExisting = ExtractOptionalBool(Params, TEXT("replace_existing"), false);
	const bool bExportProof = ExtractOptionalBool(Params, TEXT("export_proof"), true);

	FString MapFilename = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (FPaths::FileExists(MapFilename) && !bReplaceExisting)
	{
		return MakeSandboxPlacementError(
			FString::Printf(TEXT("Sandbox map already exists at '%s'. Pass replace_existing=true to overwrite within the sandbox."), *MapPath),
			TEXT("sandbox_map_already_exists"),
			MapPath);
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(MapFilename), true);

	FString ActorName = ExtractOptionalString(Params, TEXT("actor_name"), TEXT("Packet691_SandboxActor"));
	if (ActorName.IsEmpty())
	{
		ActorName = TEXT("Packet691_SandboxActor");
	}
	if (!ValidateActorNameParam(ActorName, ParamError))
	{
		return ParamError.GetValue();
	}

	FString ActorClassPath = ExtractOptionalString(Params, TEXT("actor_class"), TEXT("/Script/Engine.StaticMeshActor"));
	if (ActorClassPath.IsEmpty())
	{
		ActorClassPath = TEXT("/Script/Engine.StaticMeshActor");
	}

	UClass* ActorClass = LoadActorClass(ActorClassPath, ParamError);
	if (!ActorClass)
	{
		return ParamError.GetValue();
	}

	const TArray<FString> DirtyBeforeTool = SnapshotDirtyPackageNames();

	UWorld* NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(false);
	if (!NewWorld)
	{
		return FMCPToolResult::Error(TEXT("Failed to create a new blank sandbox map."));
	}

	if (!FEditorFileUtils::SaveMap(NewWorld, MapFilename))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create initial sandbox map at '%s'."), *MapPath));
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FMCPToolResult::Error(TEXT("Sandbox map was created but no editor world is available for actor placement."));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = FName(*ActorName);
	SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	const FVector Location = ExtractVectorParam(Params, TEXT("location"), FVector(100.0, 0.0, 120.0));
	const FRotator Rotation = ExtractRotatorParam(Params, TEXT("rotation"), FRotator::ZeroRotator);
	const FVector Scale = ExtractScaleParam(Params, TEXT("scale"), FVector::OneVector);
	const FTransform SpawnTransform(Rotation, Location, Scale);

	AActor* SpawnedActor = World->SpawnActor<AActor>(ActorClass, SpawnTransform, SpawnParams);
	if (!SpawnedActor)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to place actor of class '%s' in sandbox map '%s'."), *ActorClassPath, *MapPath));
	}

	SpawnedActor->SetActorLabel(ActorName);
	MarkActorDirty(SpawnedActor);
	MarkWorldDirty(World);

	const TArray<FString> DirtyBeforePlacementSave = SnapshotDirtyPackageNames();

	UnrealClaude::SavePipeline::FSaveSpec SaveSpec;
	SaveSpec.Packages.Add(World->GetOutermost());
	SaveSpec.bIsExplicitToolCall = true;
	const UnrealClaude::SavePipeline::FLifecycleOutcome SaveOutcome = UnrealClaude::SavePipeline::Run(SaveSpec);

	const TArray<FString> SavedPackages = PackageNamesFromResults(SaveOutcome.Saved);
	if (!ArrayContainsString(SavedPackages, MapPath))
	{
		FString ErrorText = TEXT("Explicit sandbox map lifecycle save did not report the target map as saved.");
		if (SaveOutcome.Failed.Num() > 0 && !SaveOutcome.Failed[0].Error.IsEmpty())
		{
			ErrorText += FString::Printf(TEXT(" First failure: %s"), *SaveOutcome.Failed[0].Error);
		}
		return FMCPToolResult::Error(ErrorText);
	}

	const TArray<FString> DirtyAfterPlacementSave = SnapshotDirtyPackageNames();

	bool bReadbackFound = false;
	int32 MatchingActorCount = 0;
	FString ReadbackActorClass;
	FString ReadbackActorLabel;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		if (Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase)
			|| Actor->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase))
		{
			bReadbackFound = true;
			++MatchingActorCount;
			ReadbackActorClass = Actor->GetClass()->GetName();
			ReadbackActorLabel = Actor->GetActorLabel();
		}
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("result_type"), TEXT("sandbox_map_placement"));
	ResultData->SetStringField(TEXT("schema_version"), TEXT("unrealclaude_sandbox_map_placement.v1"));
	ResultData->SetStringField(TEXT("map_path"), MapPath);
	ResultData->SetStringField(TEXT("map_filename"), MapFilename);
	ResultData->SetStringField(TEXT("sandbox_root"), GetSandboxRootPath());
	ResultData->SetStringField(TEXT("sandbox_prefix"), GetPacketSandboxPrefix());
	FString PacketLabel;
	if (TryExtractPacketLabelFromSandboxMapPath(MapPath, PacketLabel))
	{
		ResultData->SetStringField(TEXT("packet_label"), PacketLabel);
	}
	ResultData->SetStringField(TEXT("actor_name"), SpawnedActor->GetName());
	ResultData->SetStringField(TEXT("actor_label"), SpawnedActor->GetActorLabel());
	ResultData->SetStringField(TEXT("actor_class"), SpawnedActor->GetClass()->GetName());
	ResultData->SetBoolField(TEXT("created_or_replaced_map"), true);
	ResultData->SetBoolField(TEXT("production_map_mutation_prevented"), true);
	ResultData->SetBoolField(TEXT("readback_actor_found"), bReadbackFound);
	ResultData->SetNumberField(TEXT("readback_matching_actor_count"), MatchingActorCount);
	ResultData->SetStringField(TEXT("readback_actor_class"), ReadbackActorClass);
	ResultData->SetStringField(TEXT("readback_actor_label"), ReadbackActorLabel);
	ResultData->SetObjectField(TEXT("sandbox_lifecycle"), BuildSaveLifecycleJson(MapPath, DirtyBeforeTool, DirtyBeforePlacementSave, DirtyAfterPlacementSave, SaveOutcome));

	if (bExportProof)
	{
		FString ProofPath;
		const bool bProofWritten = WriteProofArtifact(ResultData, ProofPath);
		ResultData->SetBoolField(TEXT("proof_artifact_written"), bProofWritten);
		ResultData->SetStringField(TEXT("proof_artifact_path"), ProofPath);
	}
	else
	{
		ResultData->SetBoolField(TEXT("proof_artifact_written"), false);
	}

	if (!bReadbackFound)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Placed actor '%s' but readback did not find it in sandbox map '%s'."), *ActorName, *MapPath));
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Created sandbox map '%s', placed actor '%s', saved and read back successfully."), *MapPath, *ActorName),
		ResultData);
}
