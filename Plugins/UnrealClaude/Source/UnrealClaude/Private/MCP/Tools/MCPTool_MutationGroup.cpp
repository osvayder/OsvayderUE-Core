// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_MutationGroup.h"

#include "CppReflectionMutationSupport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr int32 MaxReasonCount = 12;
	constexpr int32 SchemaVersion = 1;

	const TCHAR* SchemaName = TEXT("unrealclaude_mutation_group");
	const TCHAR* ReceiptSchemaName = TEXT("unrealclaude_mutation_group_receipt");
	const TCHAR* SupportedMutationKind = TEXT("cpp_property_metadata_upsert");

	const TCHAR* StatePreviewed = TEXT("previewed_not_applied");
	const TCHAR* StateAborted = TEXT("aborted_no_apply");
	const TCHAR* StateApplied = TEXT("applied");
	const TCHAR* StateReverted = TEXT("reverted");

	const TCHAR* GateEligible = TEXT("eligible");
	const TCHAR* GateBlockedPreview = TEXT("blocked_preview");
	const TCHAR* GateBlockedState = TEXT("blocked_state");
	const TCHAR* GateBlockedSourceChanged = TEXT("blocked_source_changed_since_preview");
	const TCHAR* GateBlockedAppliedDrift = TEXT("blocked_source_changed_since_apply");

	struct FMutationItemSpec
	{
		FString MutationKind;
		FString HeaderPath;
		FString PropertyName;
		FString MetadataKey;
		FString MetadataValue;
		FString TargetId;
	};

	struct FCheckpointFileRecord
	{
		FString AbsolutePath;
		FString RelativePath;
		FString CheckpointPath;
		FString HashBeforeApply;
		FString HashAfterApply;
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

	FString SerializeJsonPretty(const TSharedPtr<FJsonObject>& JsonObject)
	{
		if (!JsonObject.IsValid())
		{
			return FString();
		}

		FString JsonText;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		Writer->Close();
		return JsonText;
	}

	bool SaveJsonPretty(const TSharedPtr<FJsonObject>& JsonObject, const FString& Path)
	{
		const FString Directory = FPaths::GetPath(Path);
		if (!Directory.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*Directory, true);
		}

		return FFileHelper::SaveStringToFile(
			SerializeJsonPretty(JsonObject),
			*Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool LoadJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
	{
		OutObject.Reset();

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *Path))
		{
			return false;
		}

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	FString ComputeContentHash(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	}

	bool LoadFileHash(const FString& AbsolutePath, FString& OutHash)
	{
		FString FileContent;
		if (!FFileHelper::LoadFileToString(FileContent, *AbsolutePath))
		{
			OutHash.Reset();
			return false;
		}

		OutHash = ComputeContentHash(FileContent);
		return true;
	}

	FString NormalizeAbsolutePath(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			return FString();
		}

		if (!FPaths::IsRelative(Path))
		{
			FPaths::NormalizeFilename(Path);
			return Path;
		}

		const FString AbsoluteProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FString Absolute = FPaths::ConvertRelativePathToFull(AbsoluteProjectDir, Path);
		FPaths::NormalizeFilename(Absolute);
		return Absolute;
	}

	FString MakeSafeSlug(const FString& InValue, const FString& Fallback)
	{
		FString Safe = InValue.TrimStartAndEnd();
		if (Safe.IsEmpty())
		{
			Safe = Fallback;
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

		bool bTrimmedUnderscore = false;
		Safe.TrimCharInline(TEXT('_'), &bTrimmedUnderscore);
		return Safe.IsEmpty() ? Fallback : Safe;
	}

	FString MakeUtcIsoNow()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	FString MakeGroupId(const FString& GroupSlug)
	{
		return FString::Printf(TEXT("%lld_%s"), FDateTime::UtcNow().GetTicks(), *GroupSlug);
	}

	FString GetMutationGroupsRoot()
	{
		const FString Root = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()),
			TEXT("UnrealClaude"),
			TEXT("mutation_groups"));
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	FString GetGroupDirectory(const FString& GroupId)
	{
		return FPaths::Combine(GetMutationGroupsRoot(), GroupId);
	}

	FString GetGroupManifestPath(const FString& GroupId)
	{
		return FPaths::Combine(GetGroupDirectory(GroupId), TEXT("group.json"));
	}

	FString GetGroupReceiptPath(const FString& GroupId, const FString& ReceiptName)
	{
		return FPaths::Combine(GetGroupDirectory(GroupId), ReceiptName);
	}

	FString GetCheckpointDirectory(const FString& GroupId)
	{
		return FPaths::Combine(GetGroupDirectory(GroupId), TEXT("checkpoint"));
	}

	FString GetCheckpointManifestPath(const FString& GroupId)
	{
		return FPaths::Combine(GetGroupDirectory(GroupId), TEXT("checkpoint_manifest.json"));
	}

	FString MakeProjectRelativePath(const FString& AbsolutePath)
	{
		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectDir);

		FString Normalized = AbsolutePath;
		FPaths::NormalizeFilename(Normalized);
		if (Normalized.StartsWith(ProjectDir))
		{
			return Normalized.RightChop(ProjectDir.Len()).TrimStartAndEnd();
		}

		return FPaths::GetCleanFilename(Normalized);
	}

	FString MakeCheckpointPathForFile(const FString& GroupId, const FString& AbsolutePath)
	{
		FString RelativePath = MakeProjectRelativePath(AbsolutePath);
		RelativePath.ReplaceInline(TEXT(":"), TEXT("_"));
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		const FString CheckpointPath = FPaths::Combine(GetCheckpointDirectory(GroupId), RelativePath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CheckpointPath), true);
		return CheckpointPath;
	}

	TSharedPtr<FJsonObject> MakeValidationGateObject(const FString& Outcome, const bool bEligible, const TArray<FString>& Reasons)
	{
		TSharedPtr<FJsonObject> GateObject = MakeShared<FJsonObject>();
		GateObject->SetStringField(TEXT("outcome"), Outcome);
		GateObject->SetBoolField(TEXT("eligible"), bEligible);
		SetJsonStringArrayField(GateObject, TEXT("reasons"), Reasons);
		return GateObject;
	}

	TSharedPtr<FJsonObject> MakeCheckpointFileJson(const FCheckpointFileRecord& Record)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("absolute_path"), Record.AbsolutePath);
		Object->SetStringField(TEXT("relative_path"), Record.RelativePath);
		Object->SetStringField(TEXT("checkpoint_path"), Record.CheckpointPath);
		Object->SetStringField(TEXT("hash_before_apply"), Record.HashBeforeApply);
		Object->SetStringField(TEXT("hash_after_apply"), Record.HashAfterApply);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeGroupPathsObject(const FString& GroupId)
	{
		TSharedPtr<FJsonObject> Paths = MakeShared<FJsonObject>();
		Paths->SetStringField(TEXT("group_directory"), GetGroupDirectory(GroupId));
		Paths->SetStringField(TEXT("manifest_path"), GetGroupManifestPath(GroupId));
		Paths->SetStringField(TEXT("checkpoint_directory"), GetCheckpointDirectory(GroupId));
		Paths->SetStringField(TEXT("checkpoint_manifest_path"), GetCheckpointManifestPath(GroupId));
		Paths->SetStringField(TEXT("preview_receipt_path"), GetGroupReceiptPath(GroupId, TEXT("preview_receipt.json")));
		Paths->SetStringField(TEXT("apply_receipt_path"), GetGroupReceiptPath(GroupId, TEXT("apply_receipt.json")));
		Paths->SetStringField(TEXT("abort_receipt_path"), GetGroupReceiptPath(GroupId, TEXT("abort_receipt.json")));
		Paths->SetStringField(TEXT("revert_receipt_path"), GetGroupReceiptPath(GroupId, TEXT("revert_receipt.json")));
		return Paths;
	}

	TArray<FString> GetStopRules()
	{
		TArray<FString> Rules;
		Rules.Add(TEXT("Do not apply when validation_gate.eligible is false."));
		Rules.Add(TEXT("Apply is blocked if any source hash changed after preview."));
		Rules.Add(TEXT("Abort is only available while the group is previewed and not applied."));
		Rules.Add(TEXT("Revert is only available after a successful apply with checkpoint metadata."));
		Rules.Add(TEXT("Revert is blocked if the current source file no longer matches the applied hash."));
		return Rules;
	}

	bool TryParseMutationSpecs(
		const TSharedRef<FJsonObject>& Params,
		TArray<FMutationItemSpec>& OutSpecs,
		TArray<FString>& OutErrors)
	{
		OutSpecs.Reset();
		OutErrors.Reset();

		const TArray<TSharedPtr<FJsonValue>>* MutationsArray = nullptr;
		if (!Params->TryGetArrayField(TEXT("mutations"), MutationsArray) || !MutationsArray)
		{
			OutErrors.Add(TEXT("mutations array is required for preview_group."));
			return false;
		}

		int32 MutationIndex = 0;
		for (const TSharedPtr<FJsonValue>& MutationValue : *MutationsArray)
		{
			const TSharedPtr<FJsonObject> MutationObject = MutationValue.IsValid() ? MutationValue->AsObject() : nullptr;
			if (!MutationObject.IsValid())
			{
				OutErrors.Add(FString::Printf(TEXT("mutations[%d] must be an object."), MutationIndex));
				++MutationIndex;
				continue;
			}

			FMutationItemSpec Spec;
			MutationObject->TryGetStringField(TEXT("mutation_kind"), Spec.MutationKind);
			MutationObject->TryGetStringField(TEXT("header_path"), Spec.HeaderPath);
			MutationObject->TryGetStringField(TEXT("property_name"), Spec.PropertyName);
			MutationObject->TryGetStringField(TEXT("metadata_key"), Spec.MetadataKey);
			MutationObject->TryGetStringField(TEXT("metadata_value"), Spec.MetadataValue);

			Spec.HeaderPath = NormalizeAbsolutePath(Spec.HeaderPath);
			Spec.PropertyName = Spec.PropertyName.TrimStartAndEnd();
			Spec.MetadataKey = Spec.MetadataKey.TrimStartAndEnd();

			if (Spec.MutationKind.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("mutations[%d].mutation_kind is required."), MutationIndex));
			}
			else if (!Spec.MutationKind.Equals(SupportedMutationKind, ESearchCase::IgnoreCase))
			{
				OutErrors.Add(FString::Printf(TEXT("mutations[%d].mutation_kind '%s' is unsupported in this slice. Supported: %s"), MutationIndex, *Spec.MutationKind, SupportedMutationKind));
			}

			if (Spec.HeaderPath.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("mutations[%d].header_path is required."), MutationIndex));
			}
			if (Spec.PropertyName.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("mutations[%d].property_name is required."), MutationIndex));
			}
			if (Spec.MetadataKey.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("mutations[%d].metadata_key is required."), MutationIndex));
			}

			FString CanonicalMetadataKey;
			FString KeyError;
			if (!Spec.MetadataKey.IsEmpty() && !TryNormalizeAllowedPropertyMetadataKey(Spec.MetadataKey, CanonicalMetadataKey, KeyError))
			{
				OutErrors.Add(FString::Printf(TEXT("mutations[%d].metadata_key is invalid: %s"), MutationIndex, *KeyError));
			}
			else if (!CanonicalMetadataKey.IsEmpty())
			{
				Spec.MetadataKey = CanonicalMetadataKey;
			}

			Spec.TargetId = FString::Printf(TEXT("%s|%s|%s"), *Spec.HeaderPath, *Spec.PropertyName, *Spec.MetadataKey);
			OutSpecs.Add(Spec);
			++MutationIndex;
		}

		return OutErrors.Num() == 0;
	}

	TSharedPtr<FJsonObject> BuildPreviewSummary(
		const int32 RequestedMutationCount,
		const int32 PreviewableMutationCount,
		const int32 BlockedMutationCount,
		const int32 WouldChangeMutationCount,
		const int32 NoOpMutationCount,
		const int32 UniqueTargetCount,
		const int32 UniqueFileCount)
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("requested_mutations"), RequestedMutationCount);
		Summary->SetNumberField(TEXT("previewable_mutations"), PreviewableMutationCount);
		Summary->SetNumberField(TEXT("blocked_mutations"), BlockedMutationCount);
		Summary->SetNumberField(TEXT("would_change_mutations"), WouldChangeMutationCount);
		Summary->SetNumberField(TEXT("no_op_mutations"), NoOpMutationCount);
		Summary->SetNumberField(TEXT("affected_target_count"), UniqueTargetCount);
		Summary->SetNumberField(TEXT("affected_file_count"), UniqueFileCount);
		return Summary;
	}

	void AddManifestCommonFields(
		const TSharedPtr<FJsonObject>& Manifest,
		const FString& GroupId,
		const FString& GroupName,
		const FString& GroupSlug)
	{
		TArray<FString> SupportedMutationKinds;
		SupportedMutationKinds.Add(FString(SupportedMutationKind));

		Manifest->SetStringField(TEXT("schema"), SchemaName);
		Manifest->SetNumberField(TEXT("schema_version"), SchemaVersion);
		Manifest->SetStringField(TEXT("mutation_group_id"), GroupId);
		Manifest->SetStringField(TEXT("group_name"), GroupName);
		Manifest->SetStringField(TEXT("group_slug"), GroupSlug);
		SetJsonStringArrayField(Manifest, TEXT("supported_mutation_kinds"), SupportedMutationKinds);
		SetJsonStringArrayField(Manifest, TEXT("stop_rules"), GetStopRules());
		Manifest->SetObjectField(TEXT("paths"), MakeGroupPathsObject(GroupId));
	}

	bool WriteReceipt(
		const FString& GroupId,
		const FString& ReceiptFilename,
		const FString& Operation,
		const FString& Outcome,
		const TSharedPtr<FJsonObject>& ResultData,
		FString& OutReceiptPath)
	{
		OutReceiptPath = GetGroupReceiptPath(GroupId, ReceiptFilename);
		TSharedPtr<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), ReceiptSchemaName);
		Receipt->SetNumberField(TEXT("schema_version"), SchemaVersion);
		Receipt->SetStringField(TEXT("mutation_group_id"), GroupId);
		Receipt->SetStringField(TEXT("operation"), Operation);
		Receipt->SetStringField(TEXT("operation_outcome"), Outcome);
		Receipt->SetStringField(TEXT("recorded_at_utc"), MakeUtcIsoNow());
		if (ResultData.IsValid())
		{
			Receipt->SetObjectField(TEXT("result"), ResultData);
		}
		return SaveJsonPretty(Receipt, OutReceiptPath);
	}

	TSharedPtr<FJsonObject> LoadGroupManifestOrNull(const FString& GroupId)
	{
		TSharedPtr<FJsonObject> Manifest;
		if (!LoadJsonObject(GetGroupManifestPath(GroupId), Manifest))
		{
			return nullptr;
		}
		return Manifest;
	}

	bool TryParseManifestMutations(
		const TSharedPtr<FJsonObject>& Manifest,
		TArray<FMutationItemSpec>& OutSpecs,
		TArray<TSharedPtr<FJsonObject>>& OutPreviewObjects,
		TArray<FString>& OutErrors)
	{
		OutSpecs.Reset();
		OutPreviewObjects.Reset();
		OutErrors.Reset();

		const TArray<TSharedPtr<FJsonValue>>* MutationArray = nullptr;
		if (!Manifest.IsValid() || !Manifest->TryGetArrayField(TEXT("mutations"), MutationArray) || !MutationArray)
		{
			OutErrors.Add(TEXT("group manifest is missing mutations."));
			return false;
		}

		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& MutationValue : *MutationArray)
		{
			const TSharedPtr<FJsonObject> MutationObject = MutationValue.IsValid() ? MutationValue->AsObject() : nullptr;
			if (!MutationObject.IsValid())
			{
				OutErrors.Add(FString::Printf(TEXT("group manifest mutations[%d] is invalid."), Index));
				++Index;
				continue;
			}

			FMutationItemSpec Spec;
			MutationObject->TryGetStringField(TEXT("mutation_kind"), Spec.MutationKind);
			MutationObject->TryGetStringField(TEXT("header_path"), Spec.HeaderPath);
			MutationObject->TryGetStringField(TEXT("property_name"), Spec.PropertyName);
			MutationObject->TryGetStringField(TEXT("metadata_key"), Spec.MetadataKey);
			MutationObject->TryGetStringField(TEXT("metadata_value"), Spec.MetadataValue);
			MutationObject->TryGetStringField(TEXT("target_id"), Spec.TargetId);

			const TSharedPtr<FJsonObject>* PreviewObjectPtr = nullptr;
			if (!MutationObject->TryGetObjectField(TEXT("preview"), PreviewObjectPtr) || !PreviewObjectPtr || !(*PreviewObjectPtr).IsValid())
			{
				OutErrors.Add(FString::Printf(TEXT("group manifest mutations[%d] is missing preview data."), Index));
				++Index;
				continue;
			}

			OutSpecs.Add(Spec);
			OutPreviewObjects.Add(*PreviewObjectPtr);
			++Index;
		}

		return OutErrors.Num() == 0;
	}

	FString GetManifestState(const TSharedPtr<FJsonObject>& Manifest)
	{
		FString State;
		if (Manifest.IsValid())
		{
			Manifest->TryGetStringField(TEXT("group_state"), State);
		}
		return State;
	}

	bool IsGateEligible(const TSharedPtr<FJsonObject>& Manifest)
	{
		const TSharedPtr<FJsonObject>* GateObject = nullptr;
		if (!Manifest.IsValid() || !Manifest->TryGetObjectField(TEXT("validation_gate"), GateObject) || !GateObject || !(*GateObject).IsValid())
		{
			return false;
		}

		bool bEligible = false;
		(*GateObject)->TryGetBoolField(TEXT("eligible"), bEligible);
		return bEligible;
	}

	TSharedPtr<FJsonObject> MakeAffectedTargetObject(const FMutationItemSpec& Spec)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("target_id"), Spec.TargetId);
		Object->SetStringField(TEXT("mutation_kind"), Spec.MutationKind);
		Object->SetStringField(TEXT("header_path"), Spec.HeaderPath);
		Object->SetStringField(TEXT("property_name"), Spec.PropertyName);
		Object->SetStringField(TEXT("metadata_key"), Spec.MetadataKey);
		return Object;
	}

	bool CopyFileToPath(const FString& SourcePath, const FString& DestinationPath)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationPath), true);
		return IFileManager::Get().Copy(*DestinationPath, *SourcePath, true, true) == COPY_OK;
	}

	bool RestoreCheckpointFiles(const TArray<FCheckpointFileRecord>& Files)
	{
		bool bAllRestored = true;
		for (const FCheckpointFileRecord& File : Files)
		{
			if (!CopyFileToPath(File.CheckpointPath, File.AbsolutePath))
			{
				bAllRestored = false;
			}
		}
		return bAllRestored;
	}

	bool TryReadCheckpointManifest(const FString& GroupId, TArray<FCheckpointFileRecord>& OutFiles)
	{
		OutFiles.Reset();

		TSharedPtr<FJsonObject> Manifest;
		if (!LoadJsonObject(GetCheckpointManifestPath(GroupId), Manifest) || !Manifest.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("files"), FilesArray) || !FilesArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& FileValue : *FilesArray)
		{
			const TSharedPtr<FJsonObject> FileObject = FileValue.IsValid() ? FileValue->AsObject() : nullptr;
			if (!FileObject.IsValid())
			{
				continue;
			}

			FCheckpointFileRecord Record;
			FileObject->TryGetStringField(TEXT("absolute_path"), Record.AbsolutePath);
			FileObject->TryGetStringField(TEXT("relative_path"), Record.RelativePath);
			FileObject->TryGetStringField(TEXT("checkpoint_path"), Record.CheckpointPath);
			FileObject->TryGetStringField(TEXT("hash_before_apply"), Record.HashBeforeApply);
			FileObject->TryGetStringField(TEXT("hash_after_apply"), Record.HashAfterApply);
			OutFiles.Add(Record);
		}

		return OutFiles.Num() > 0;
	}
}

FMCPToolInfo FMCPTool_MutationGroup::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("mutation_group");
	Info.Description = TEXT(
		"Grouped mutation safety/control foundation.\n\n"
		"This U5 slice provides one narrow grouped preview/apply/abort/revert path\n"
		"for plugin-owned changes with explicit checkpoints and stop rules.\n\n"
		"Current supported mutation kind:\n"
		"- cpp_property_metadata_upsert: grouped plugin-owned reflected UPROPERTY metadata upserts\n\n"
		"Supported operations:\n"
		"- preview_group\n"
		"- apply_group\n"
		"- abort_group\n"
		"- revert_group\n\n"
		"Truth boundary:\n"
		"- current slice is not broad autonomous repair\n"
		"- preview does not apply changes\n"
		"- abort closes out an unapplied preview only\n"
		"- revert is available only after a successful apply with checkpoint metadata\n"
		"- revert is blocked if the current source diverged after apply"
	);
	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"),
			TEXT("Operation: preview_group, apply_group, abort_group, or revert_group."), true),
		FMCPToolParameter(TEXT("group_name"), TEXT("string"),
			TEXT("Optional preview label for preview_group."), false),
		FMCPToolParameter(TEXT("group_slug"), TEXT("string"),
			TEXT("Optional stable slug for preview_group artifact paths."), false),
		FMCPToolParameter(TEXT("mutation_group_id"), TEXT("string"),
			TEXT("Required for apply_group, abort_group, and revert_group."), false),
		FMCPToolParameter(TEXT("mutations"), TEXT("array"),
			TEXT("Required for preview_group. Current supported mutation kind: cpp_property_metadata_upsert with header_path, property_name, metadata_key, metadata_value."), false)
	};
	// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
	Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
		TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));
	return Info;
}

FMCPToolResult FMCPTool_MutationGroup::Execute(const TSharedRef<FJsonObject>& Params)
{
	const FString Operation = ExtractOptionalString(Params, TEXT("operation")).TrimStartAndEnd();
	if (Operation.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("operation is required. Supported: preview_group, apply_group, abort_group, revert_group"));
	}

	if (Operation == TEXT("preview_group"))
	{
		return ExecutePreviewGroup(Params);
	}
	if (Operation == TEXT("apply_group"))
	{
		return ExecuteApplyGroup(Params);
	}
	if (Operation == TEXT("abort_group"))
	{
		return ExecuteAbortGroup(Params);
	}
	if (Operation == TEXT("revert_group"))
	{
		return ExecuteRevertGroup(Params);
	}

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unsupported operation '%s'. Supported: preview_group, apply_group, abort_group, revert_group"),
		*Operation));
}

FMCPToolResult FMCPTool_MutationGroup::ExecutePreviewGroup(const TSharedRef<FJsonObject>& Params)
{
	TArray<FMutationItemSpec> Specs;
	TArray<FString> ParseErrors;
	if (!TryParseMutationSpecs(Params, Specs, ParseErrors))
	{
		return FMCPToolResult::Error(FString::Join(ParseErrors, TEXT(" | ")));
	}

	const FString GroupName = ExtractOptionalString(Params, TEXT("group_name"), TEXT("Mutation Group Preview")).TrimStartAndEnd();
	const FString GroupSlug = MakeSafeSlug(
		ExtractOptionalString(Params, TEXT("group_slug")),
		TEXT("mutation_group"));
	const FString GroupId = MakeGroupId(GroupSlug);
	IFileManager::Get().MakeDirectory(*GetGroupDirectory(GroupId), true);

	TArray<FString> ValidationReasons;
	TSet<FString> SeenTargets;
	TSet<FString> UniqueFiles;
	TArray<TSharedPtr<FJsonValue>> MutationValues;
	TArray<TSharedPtr<FJsonValue>> AffectedTargetValues;

	int32 PreviewableMutationCount = 0;
	int32 BlockedMutationCount = 0;
	int32 WouldChangeMutationCount = 0;
	int32 NoOpMutationCount = 0;

	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		const FMutationItemSpec& Spec = Specs[Index];
		TSharedPtr<FJsonObject> MutationObject = MakeShared<FJsonObject>();
		MutationObject->SetNumberField(TEXT("mutation_index"), Index);
		MutationObject->SetStringField(TEXT("mutation_kind"), Spec.MutationKind);
		MutationObject->SetStringField(TEXT("target_id"), Spec.TargetId);
		MutationObject->SetStringField(TEXT("header_path"), Spec.HeaderPath);
		MutationObject->SetStringField(TEXT("property_name"), Spec.PropertyName);
		MutationObject->SetStringField(TEXT("metadata_key"), Spec.MetadataKey);
		MutationObject->SetStringField(TEXT("metadata_value"), Spec.MetadataValue);

		bool bBlocked = false;
		if (SeenTargets.Contains(Spec.TargetId))
		{
			AddUniqueLimited(ValidationReasons, FString::Printf(TEXT("duplicate_target:%s"), *Spec.TargetId), MaxReasonCount);
			bBlocked = true;
		}
		else
		{
			SeenTargets.Add(Spec.TargetId);
		}

		const FCppReflectionPropertyMetadataMutationResult Preview = PreviewPluginPropertyMetadataMutation(
			Spec.HeaderPath,
			Spec.PropertyName,
			Spec.MetadataKey,
			Spec.MetadataValue);

		TSharedPtr<FJsonObject> PreviewObject = Preview.ToJson();
		MutationObject->SetObjectField(TEXT("preview"), PreviewObject);

		const bool bPreviewBlocked = !Preview.bSuccess || !Preview.bScopeAllowed || bBlocked;
		const bool bWouldChange = Preview.bWouldChangeFile && Preview.bSuccess && Preview.bScopeAllowed && !bBlocked;

		if (bPreviewBlocked)
		{
			++BlockedMutationCount;
			AddUniqueLimited(ValidationReasons, Preview.ErrorMessage, MaxReasonCount);
		}
		else
		{
			++PreviewableMutationCount;
			if (bWouldChange)
			{
				++WouldChangeMutationCount;
			}
			else
			{
				++NoOpMutationCount;
			}
			UniqueFiles.Add(Preview.HeaderPath);
			AffectedTargetValues.Add(MakeShared<FJsonValueObject>(MakeAffectedTargetObject(Spec)));
		}

		MutationObject->SetBoolField(TEXT("blocked"), bPreviewBlocked);
		MutationObject->SetBoolField(TEXT("would_change_file"), bWouldChange);
		MutationObject->SetBoolField(TEXT("apply_eligible"), !bPreviewBlocked);
		MutationValues.Add(MakeShared<FJsonValueObject>(MutationObject));
	}

	const bool bEligible = Specs.Num() > 0 && BlockedMutationCount == 0 && ParseErrors.Num() == 0;
	const FString GateOutcome = bEligible ? GateEligible : GateBlockedPreview;
	if (!bEligible && ValidationReasons.Num() == 0)
	{
		AddUniqueLimited(ValidationReasons, TEXT("preview_blocked"), MaxReasonCount);
	}

	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	AddManifestCommonFields(Manifest, GroupId, GroupName, GroupSlug);
	Manifest->SetStringField(TEXT("created_at_utc"), MakeUtcIsoNow());
	Manifest->SetStringField(TEXT("updated_at_utc"), MakeUtcIsoNow());
	Manifest->SetStringField(TEXT("group_state"), StatePreviewed);
	Manifest->SetStringField(TEXT("latest_operation"), TEXT("preview_group"));
	Manifest->SetStringField(TEXT("latest_operation_outcome"), bEligible ? TEXT("previewed") : TEXT("previewed_blocked"));
	Manifest->SetObjectField(TEXT("validation_gate"), MakeValidationGateObject(GateOutcome, bEligible, ValidationReasons));
	Manifest->SetObjectField(
		TEXT("preview_summary"),
		BuildPreviewSummary(
			Specs.Num(),
			PreviewableMutationCount,
			BlockedMutationCount,
			WouldChangeMutationCount,
			NoOpMutationCount,
			AffectedTargetValues.Num(),
			UniqueFiles.Num()));
	Manifest->SetBoolField(TEXT("apply_eligible"), bEligible);
	Manifest->SetBoolField(TEXT("abort_available"), true);
	Manifest->SetBoolField(TEXT("revert_available"), false);
	Manifest->SetBoolField(TEXT("checkpoint_available"), false);
	Manifest->SetArrayField(TEXT("affected_targets"), AffectedTargetValues);
	Manifest->SetArrayField(TEXT("mutations"), MutationValues);

	const FString ManifestPath = GetGroupManifestPath(GroupId);
	if (!SaveJsonPretty(Manifest, ManifestPath))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to write mutation group manifest to '%s'."), *ManifestPath));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>(*Manifest);
	FString PreviewReceiptPath;
	if (!WriteReceipt(GroupId, TEXT("preview_receipt.json"), TEXT("preview_group"), bEligible ? TEXT("previewed") : TEXT("previewed_blocked"), ResultData, PreviewReceiptPath))
	{
		return FMCPToolResult::Error(TEXT("Failed to write mutation group preview receipt."));
	}

	ResultData->SetStringField(TEXT("preview_receipt_path"), PreviewReceiptPath);
	ResultData->SetStringField(TEXT("manifest_path"), ManifestPath);

	Manifest->SetStringField(TEXT("latest_receipt_path"), PreviewReceiptPath);
	SaveJsonPretty(Manifest, ManifestPath);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("mutation_group preview -> %s (%s)"), bEligible ? TEXT("eligible") : TEXT("blocked"), *GroupId),
		ResultData);
}

FMCPToolResult FMCPTool_MutationGroup::ExecuteApplyGroup(const TSharedRef<FJsonObject>& Params)
{
	const FString GroupId = ExtractOptionalString(Params, TEXT("mutation_group_id")).TrimStartAndEnd();
	if (GroupId.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("mutation_group_id is required for apply_group."));
	}

	TSharedPtr<FJsonObject> Manifest = LoadGroupManifestOrNull(GroupId);
	if (!Manifest.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load mutation group '%s'."), *GroupId));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>(*Manifest);
	ResultData->SetStringField(TEXT("operation"), TEXT("apply_group"));

	TArray<FString> ValidationReasons;
	if (GetManifestState(Manifest) != StatePreviewed)
	{
		AddUniqueLimited(ValidationReasons, FString::Printf(TEXT("group_state=%s"), *GetManifestState(Manifest)), MaxReasonCount);
		ResultData->SetStringField(TEXT("operation_outcome"), TEXT("apply_blocked"));
		ResultData->SetObjectField(TEXT("validation_gate"), MakeValidationGateObject(GateBlockedState, false, ValidationReasons));

		FString ReceiptPath;
		WriteReceipt(GroupId, TEXT("apply_receipt.json"), TEXT("apply_group"), TEXT("apply_blocked"), ResultData, ReceiptPath);
		ResultData->SetStringField(TEXT("apply_receipt_path"), ReceiptPath);
		return FMCPToolResult::Success(
			FString::Printf(TEXT("mutation_group apply blocked -> %s"), *GroupId),
			ResultData);
	}

	if (!IsGateEligible(Manifest))
	{
		ValidationReasons = TArray<FString>{ TEXT("preview_validation_gate_not_eligible") };
		ResultData->SetStringField(TEXT("operation_outcome"), TEXT("apply_blocked"));
		ResultData->SetObjectField(TEXT("validation_gate"), MakeValidationGateObject(GateBlockedPreview, false, ValidationReasons));

		FString ReceiptPath;
		WriteReceipt(GroupId, TEXT("apply_receipt.json"), TEXT("apply_group"), TEXT("apply_blocked"), ResultData, ReceiptPath);
		ResultData->SetStringField(TEXT("apply_receipt_path"), ReceiptPath);
		return FMCPToolResult::Success(
			FString::Printf(TEXT("mutation_group apply blocked -> %s"), *GroupId),
			ResultData);
	}

	TArray<FMutationItemSpec> Specs;
	TArray<TSharedPtr<FJsonObject>> PreviewObjects;
	TArray<FString> ParseErrors;
	if (!TryParseManifestMutations(Manifest, Specs, PreviewObjects, ParseErrors))
	{
		return FMCPToolResult::Error(FString::Join(ParseErrors, TEXT(" | ")));
	}

	TMap<FString, FString> ExpectedFileHashes;
	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		FString SourceHashBefore;
		PreviewObjects[Index]->TryGetStringField(TEXT("source_hash_before"), SourceHashBefore);
		if (!SourceHashBefore.IsEmpty())
		{
			ExpectedFileHashes.FindOrAdd(Specs[Index].HeaderPath) = SourceHashBefore;
		}
	}

	for (const TPair<FString, FString>& Pair : ExpectedFileHashes)
	{
		FString CurrentHash;
		if (!LoadFileHash(Pair.Key, CurrentHash) || CurrentHash != Pair.Value)
		{
			AddUniqueLimited(ValidationReasons, FString::Printf(TEXT("source_changed:%s"), *Pair.Key), MaxReasonCount);
		}
	}

	if (ValidationReasons.Num() > 0)
	{
		ResultData->SetStringField(TEXT("operation_outcome"), TEXT("apply_blocked"));
		ResultData->SetObjectField(TEXT("validation_gate"), MakeValidationGateObject(GateBlockedSourceChanged, false, ValidationReasons));

		FString ReceiptPath;
		WriteReceipt(GroupId, TEXT("apply_receipt.json"), TEXT("apply_group"), TEXT("apply_blocked"), ResultData, ReceiptPath);
		ResultData->SetStringField(TEXT("apply_receipt_path"), ReceiptPath);
		return FMCPToolResult::Success(
			FString::Printf(TEXT("mutation_group apply blocked -> %s"), *GroupId),
			ResultData);
	}

	TArray<FCheckpointFileRecord> CheckpointFiles;
	for (const TPair<FString, FString>& Pair : ExpectedFileHashes)
	{
		FCheckpointFileRecord Record;
		Record.AbsolutePath = Pair.Key;
		Record.RelativePath = MakeProjectRelativePath(Pair.Key);
		Record.CheckpointPath = MakeCheckpointPathForFile(GroupId, Pair.Key);
		Record.HashBeforeApply = Pair.Value;

		if (!CopyFileToPath(Pair.Key, Record.CheckpointPath))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create checkpoint for '%s'."), *Pair.Key));
		}

		CheckpointFiles.Add(Record);
	}

	TArray<TSharedPtr<FJsonValue>> AppliedMutationValues;
	AppliedMutationValues.Reserve(Specs.Num());

	int32 AppliedMutationCount = 0;
	int32 NoOpMutationCount = 0;
	TArray<FString> ApplyErrors;
	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		const FMutationItemSpec& Spec = Specs[Index];
		const FCppReflectionPropertyMetadataMutationResult ApplyResult = ApplyPluginPropertyMetadataMutation(
			Spec.HeaderPath,
			Spec.PropertyName,
			Spec.MetadataKey,
			Spec.MetadataValue);

		TSharedPtr<FJsonObject> MutationObject = MakeShared<FJsonObject>();
		MutationObject->SetNumberField(TEXT("mutation_index"), Index);
		MutationObject->SetStringField(TEXT("mutation_kind"), Spec.MutationKind);
		MutationObject->SetStringField(TEXT("target_id"), Spec.TargetId);
		MutationObject->SetStringField(TEXT("header_path"), Spec.HeaderPath);
		MutationObject->SetStringField(TEXT("property_name"), Spec.PropertyName);
		MutationObject->SetStringField(TEXT("metadata_key"), Spec.MetadataKey);
		MutationObject->SetStringField(TEXT("metadata_value"), Spec.MetadataValue);
		MutationObject->SetObjectField(TEXT("apply"), ApplyResult.ToJson());
		AppliedMutationValues.Add(MakeShared<FJsonValueObject>(MutationObject));

		if (!ApplyResult.bSuccess)
		{
			AddUniqueLimited(ApplyErrors, ApplyResult.ErrorMessage, MaxReasonCount);
			break;
		}

		if (ApplyResult.bApplied)
		{
			++AppliedMutationCount;
		}
		else
		{
			++NoOpMutationCount;
		}
	}

	if (ApplyErrors.Num() > 0)
	{
		const bool bRestored = RestoreCheckpointFiles(CheckpointFiles);
		ResultData->SetStringField(TEXT("operation_outcome"), TEXT("apply_failed_rolled_back"));
		ResultData->SetObjectField(TEXT("validation_gate"), MakeValidationGateObject(TEXT("apply_failed"), false, ApplyErrors));
		ResultData->SetBoolField(TEXT("checkpoint_restore_performed"), bRestored);
		ResultData->SetArrayField(TEXT("mutations"), AppliedMutationValues);

		FString ReceiptPath;
		WriteReceipt(GroupId, TEXT("apply_receipt.json"), TEXT("apply_group"), TEXT("apply_failed_rolled_back"), ResultData, ReceiptPath);
		ResultData->SetStringField(TEXT("apply_receipt_path"), ReceiptPath);
		return FMCPToolResult::Success(
			FString::Printf(TEXT("mutation_group apply failed and restored checkpoint -> %s"), *GroupId),
			ResultData);
	}

	for (FCheckpointFileRecord& Record : CheckpointFiles)
	{
		LoadFileHash(Record.AbsolutePath, Record.HashAfterApply);
	}

	TSharedPtr<FJsonObject> CheckpointManifest = MakeShared<FJsonObject>();
	CheckpointManifest->SetStringField(TEXT("schema"), SchemaName);
	CheckpointManifest->SetNumberField(TEXT("schema_version"), SchemaVersion);
	CheckpointManifest->SetStringField(TEXT("mutation_group_id"), GroupId);
	CheckpointManifest->SetStringField(TEXT("created_at_utc"), MakeUtcIsoNow());

	TArray<TSharedPtr<FJsonValue>> CheckpointValues;
	for (const FCheckpointFileRecord& Record : CheckpointFiles)
	{
		CheckpointValues.Add(MakeShared<FJsonValueObject>(MakeCheckpointFileJson(Record)));
	}
	CheckpointManifest->SetArrayField(TEXT("files"), CheckpointValues);

	const FString CheckpointManifestPath = GetCheckpointManifestPath(GroupId);
	if (!SaveJsonPretty(CheckpointManifest, CheckpointManifestPath))
	{
		return FMCPToolResult::Error(TEXT("Failed to write mutation group checkpoint manifest."));
	}

	Manifest->SetStringField(TEXT("updated_at_utc"), MakeUtcIsoNow());
	Manifest->SetStringField(TEXT("group_state"), StateApplied);
	Manifest->SetStringField(TEXT("latest_operation"), TEXT("apply_group"));
	Manifest->SetStringField(TEXT("latest_operation_outcome"), TEXT("applied"));
	Manifest->SetBoolField(TEXT("abort_available"), false);
	Manifest->SetBoolField(TEXT("revert_available"), CheckpointFiles.Num() > 0);
	Manifest->SetBoolField(TEXT("checkpoint_available"), CheckpointFiles.Num() > 0);
	Manifest->SetArrayField(TEXT("last_apply_mutations"), AppliedMutationValues);
	Manifest->SetObjectField(TEXT("checkpoint"), CheckpointManifest);

	if (!SaveJsonPretty(Manifest, GetGroupManifestPath(GroupId)))
	{
		return FMCPToolResult::Error(TEXT("Failed to update mutation group manifest after apply."));
	}

	ResultData = MakeShared<FJsonObject>(*Manifest);
	ResultData->SetStringField(TEXT("operation"), TEXT("apply_group"));
	ResultData->SetStringField(TEXT("operation_outcome"), TEXT("applied"));

	TSharedPtr<FJsonObject> ApplySummary = MakeShared<FJsonObject>();
	ApplySummary->SetNumberField(TEXT("requested_mutations"), Specs.Num());
	ApplySummary->SetNumberField(TEXT("applied_mutations"), AppliedMutationCount);
	ApplySummary->SetNumberField(TEXT("no_op_mutations"), NoOpMutationCount);
	ApplySummary->SetNumberField(TEXT("checkpoint_file_count"), CheckpointFiles.Num());
	ResultData->SetObjectField(TEXT("apply_summary"), ApplySummary);

	FString ReceiptPath;
	if (!WriteReceipt(GroupId, TEXT("apply_receipt.json"), TEXT("apply_group"), TEXT("applied"), ResultData, ReceiptPath))
	{
		return FMCPToolResult::Error(TEXT("Failed to write mutation group apply receipt."));
	}
	ResultData->SetStringField(TEXT("apply_receipt_path"), ReceiptPath);

	Manifest->SetStringField(TEXT("latest_receipt_path"), ReceiptPath);
	SaveJsonPretty(Manifest, GetGroupManifestPath(GroupId));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("mutation_group apply -> applied (%s)"), *GroupId),
		ResultData);
}

FMCPToolResult FMCPTool_MutationGroup::ExecuteAbortGroup(const TSharedRef<FJsonObject>& Params)
{
	const FString GroupId = ExtractOptionalString(Params, TEXT("mutation_group_id")).TrimStartAndEnd();
	if (GroupId.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("mutation_group_id is required for abort_group."));
	}

	TSharedPtr<FJsonObject> Manifest = LoadGroupManifestOrNull(GroupId);
	if (!Manifest.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load mutation group '%s'."), *GroupId));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>(*Manifest);
	ResultData->SetStringField(TEXT("operation"), TEXT("abort_group"));

	TArray<FString> Reasons;
	if (GetManifestState(Manifest) != StatePreviewed)
	{
		AddUniqueLimited(Reasons, FString::Printf(TEXT("group_state=%s"), *GetManifestState(Manifest)), MaxReasonCount);
		ResultData->SetStringField(TEXT("operation_outcome"), TEXT("abort_blocked"));
		ResultData->SetObjectField(TEXT("validation_gate"), MakeValidationGateObject(GateBlockedState, false, Reasons));

		FString ReceiptPath;
		WriteReceipt(GroupId, TEXT("abort_receipt.json"), TEXT("abort_group"), TEXT("abort_blocked"), ResultData, ReceiptPath);
		ResultData->SetStringField(TEXT("abort_receipt_path"), ReceiptPath);
		return FMCPToolResult::Success(
			FString::Printf(TEXT("mutation_group abort blocked -> %s"), *GroupId),
			ResultData);
	}

	Manifest->SetStringField(TEXT("updated_at_utc"), MakeUtcIsoNow());
	Manifest->SetStringField(TEXT("group_state"), StateAborted);
	Manifest->SetStringField(TEXT("latest_operation"), TEXT("abort_group"));
	Manifest->SetStringField(TEXT("latest_operation_outcome"), TEXT("aborted"));
	Manifest->SetBoolField(TEXT("apply_eligible"), false);
	Manifest->SetBoolField(TEXT("abort_available"), false);
	Manifest->SetBoolField(TEXT("revert_available"), false);
	Manifest->SetBoolField(TEXT("checkpoint_available"), false);

	if (!SaveJsonPretty(Manifest, GetGroupManifestPath(GroupId)))
	{
		return FMCPToolResult::Error(TEXT("Failed to update mutation group manifest after abort."));
	}

	ResultData = MakeShared<FJsonObject>(*Manifest);
	ResultData->SetStringField(TEXT("operation"), TEXT("abort_group"));
	ResultData->SetStringField(TEXT("operation_outcome"), TEXT("aborted"));
	ResultData->SetBoolField(TEXT("no_changes_applied"), true);

	FString ReceiptPath;
	if (!WriteReceipt(GroupId, TEXT("abort_receipt.json"), TEXT("abort_group"), TEXT("aborted"), ResultData, ReceiptPath))
	{
		return FMCPToolResult::Error(TEXT("Failed to write mutation group abort receipt."));
	}
	ResultData->SetStringField(TEXT("abort_receipt_path"), ReceiptPath);

	Manifest->SetStringField(TEXT("latest_receipt_path"), ReceiptPath);
	SaveJsonPretty(Manifest, GetGroupManifestPath(GroupId));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("mutation_group abort -> closed without apply (%s)"), *GroupId),
		ResultData);
}

FMCPToolResult FMCPTool_MutationGroup::ExecuteRevertGroup(const TSharedRef<FJsonObject>& Params)
{
	const FString GroupId = ExtractOptionalString(Params, TEXT("mutation_group_id")).TrimStartAndEnd();
	if (GroupId.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("mutation_group_id is required for revert_group."));
	}

	TSharedPtr<FJsonObject> Manifest = LoadGroupManifestOrNull(GroupId);
	if (!Manifest.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load mutation group '%s'."), *GroupId));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>(*Manifest);
	ResultData->SetStringField(TEXT("operation"), TEXT("revert_group"));

	TArray<FString> ValidationReasons;
	if (GetManifestState(Manifest) != StateApplied)
	{
		AddUniqueLimited(ValidationReasons, FString::Printf(TEXT("group_state=%s"), *GetManifestState(Manifest)), MaxReasonCount);
		ResultData->SetStringField(TEXT("operation_outcome"), TEXT("revert_blocked"));
		ResultData->SetObjectField(TEXT("validation_gate"), MakeValidationGateObject(GateBlockedState, false, ValidationReasons));

		FString ReceiptPath;
		WriteReceipt(GroupId, TEXT("revert_receipt.json"), TEXT("revert_group"), TEXT("revert_blocked"), ResultData, ReceiptPath);
		ResultData->SetStringField(TEXT("revert_receipt_path"), ReceiptPath);
		return FMCPToolResult::Success(
			FString::Printf(TEXT("mutation_group revert blocked -> %s"), *GroupId),
			ResultData);
	}

	TArray<FCheckpointFileRecord> CheckpointFiles;
	if (!TryReadCheckpointManifest(GroupId, CheckpointFiles))
	{
		return FMCPToolResult::Error(TEXT("Checkpoint metadata is missing for revert_group."));
	}

	for (const FCheckpointFileRecord& Record : CheckpointFiles)
	{
		FString CurrentHash;
		if (!LoadFileHash(Record.AbsolutePath, CurrentHash) || CurrentHash != Record.HashAfterApply)
		{
			AddUniqueLimited(ValidationReasons, FString::Printf(TEXT("source_changed:%s"), *Record.AbsolutePath), MaxReasonCount);
		}
	}

	if (ValidationReasons.Num() > 0)
	{
		ResultData->SetStringField(TEXT("operation_outcome"), TEXT("revert_blocked"));
		ResultData->SetObjectField(TEXT("validation_gate"), MakeValidationGateObject(GateBlockedAppliedDrift, false, ValidationReasons));

		FString ReceiptPath;
		WriteReceipt(GroupId, TEXT("revert_receipt.json"), TEXT("revert_group"), TEXT("revert_blocked"), ResultData, ReceiptPath);
		ResultData->SetStringField(TEXT("revert_receipt_path"), ReceiptPath);
		return FMCPToolResult::Success(
			FString::Printf(TEXT("mutation_group revert blocked -> %s"), *GroupId),
			ResultData);
	}

	if (!RestoreCheckpointFiles(CheckpointFiles))
	{
		return FMCPToolResult::Error(TEXT("Failed to restore one or more checkpoint files during revert_group."));
	}

	Manifest->SetStringField(TEXT("updated_at_utc"), MakeUtcIsoNow());
	Manifest->SetStringField(TEXT("group_state"), StateReverted);
	Manifest->SetStringField(TEXT("latest_operation"), TEXT("revert_group"));
	Manifest->SetStringField(TEXT("latest_operation_outcome"), TEXT("reverted"));
	Manifest->SetBoolField(TEXT("abort_available"), false);
	Manifest->SetBoolField(TEXT("revert_available"), false);
	Manifest->SetBoolField(TEXT("checkpoint_available"), true);

	if (!SaveJsonPretty(Manifest, GetGroupManifestPath(GroupId)))
	{
		return FMCPToolResult::Error(TEXT("Failed to update mutation group manifest after revert."));
	}

	ResultData = MakeShared<FJsonObject>(*Manifest);
	ResultData->SetStringField(TEXT("operation"), TEXT("revert_group"));
	ResultData->SetStringField(TEXT("operation_outcome"), TEXT("reverted"));

	TSharedPtr<FJsonObject> RevertSummary = MakeShared<FJsonObject>();
	RevertSummary->SetNumberField(TEXT("restored_file_count"), CheckpointFiles.Num());
	ResultData->SetObjectField(TEXT("revert_summary"), RevertSummary);

	FString ReceiptPath;
	if (!WriteReceipt(GroupId, TEXT("revert_receipt.json"), TEXT("revert_group"), TEXT("reverted"), ResultData, ReceiptPath))
	{
		return FMCPToolResult::Error(TEXT("Failed to write mutation group revert receipt."));
	}
	ResultData->SetStringField(TEXT("revert_receipt_path"), ReceiptPath);

	Manifest->SetStringField(TEXT("latest_receipt_path"), ReceiptPath);
	SaveJsonPretty(Manifest, GetGroupManifestPath(GroupId));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("mutation_group revert -> restored checkpoint (%s)"), *GroupId),
		ResultData);
}
