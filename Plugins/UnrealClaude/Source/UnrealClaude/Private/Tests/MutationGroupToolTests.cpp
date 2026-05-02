// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_MutationGroup.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString GetSettingsHeaderPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT("Plugins"),
			TEXT("UnrealClaude"),
			TEXT("Source"),
			TEXT("UnrealClaude"),
			TEXT("Public"),
			TEXT("UnrealClaudeSettings.h")));
	}

	FString MakeUniqueSuffix(const FString& Prefix)
	{
		return FString::Printf(TEXT("%s_%lld"), *Prefix, FDateTime::UtcNow().GetTicks());
	}

	TSharedPtr<FJsonObject> MakePropertyMetadataMutation(
		const FString& HeaderPath,
		const FString& PropertyName,
		const FString& MetadataKey,
		const FString& MetadataValue)
	{
		TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
		Mutation->SetStringField(TEXT("mutation_kind"), TEXT("cpp_property_metadata_upsert"));
		Mutation->SetStringField(TEXT("header_path"), HeaderPath);
		Mutation->SetStringField(TEXT("property_name"), PropertyName);
		Mutation->SetStringField(TEXT("metadata_key"), MetadataKey);
		Mutation->SetStringField(TEXT("metadata_value"), MetadataValue);
		return Mutation;
	}

	TSharedRef<FJsonObject> MakePreviewParams(
		const FString& GroupSlug,
		const FString& HeaderPath,
		const FString& SpeedTooltip,
		const FString& ProfileTooltip)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), TEXT("preview_group"));
		Params->SetStringField(TEXT("group_name"), TEXT("Mutation Group Automation Probe"));
		Params->SetStringField(TEXT("group_slug"), GroupSlug);

		TArray<TSharedPtr<FJsonValue>> Mutations;
		Mutations.Add(MakeShared<FJsonValueObject>(MakePropertyMetadataMutation(
			HeaderPath,
			TEXT("DefaultCodexSpeedMode"),
			TEXT("ToolTip"),
			SpeedTooltip)));
		Mutations.Add(MakeShared<FJsonValueObject>(MakePropertyMetadataMutation(
			HeaderPath,
			TEXT("DefaultCodexProfile"),
			TEXT("ToolTip"),
			ProfileTooltip)));
		Params->SetArrayField(TEXT("mutations"), Mutations);
		return Params;
	}

	bool LoadFileText(const FString& Path, FString& OutText)
	{
		return FFileHelper::LoadFileToString(OutText, *Path);
	}

	bool SaveFileText(const FString& Path, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool LoadJsonObjectFromDisk(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
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

	bool TryGetNestedObject(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TSharedPtr<FJsonObject>& OutObject)
	{
		OutObject.Reset();
		if (!Object.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!Object->TryGetObjectField(FieldName, ObjectPtr) || !ObjectPtr || !(*ObjectPtr).IsValid())
		{
			return false;
		}

		OutObject = *ObjectPtr;
		return true;
	}

	FString GetStringFieldOrEmpty(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(FieldName, Value);
		}
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMutationGroup_RegistryToolsRegistered,
	"UnrealClaude.MutationGroup.Registry.ToolsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMutationGroup_RegistryToolsRegistered::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestTrue(TEXT("mutation_group should be registered"), Registry.HasTool(TEXT("mutation_group")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMutationGroup_PreviewCreatesDurableArtifact,
	"UnrealClaude.MutationGroup.PreviewCreatesDurableArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMutationGroup_PreviewCreatesDurableArtifact::RunTest(const FString& Parameters)
{
	FMCPTool_MutationGroup Tool;
	const FString HeaderPath = GetSettingsHeaderPath();

	FString OriginalHeaderContent;
	TestTrue(TEXT("should load settings header before preview"), LoadFileText(HeaderPath, OriginalHeaderContent));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	const FString PreviewSuffix = MakeUniqueSuffix(TEXT("preview"));
	const FMCPToolResult Result = Tool.Execute(MakePreviewParams(
		FString::Printf(TEXT("mutation_group_preview_%s"), *PreviewSuffix),
		HeaderPath,
		FString::Printf(TEXT("U5 preview speed tooltip %s"), *PreviewSuffix),
		FString::Printf(TEXT("U5 preview profile tooltip %s"), *PreviewSuffix)));

	TestTrue(TEXT("preview should succeed"), Result.bSuccess);
	TestTrue(TEXT("preview should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("preview should stay in previewed_not_applied state"), GetStringFieldOrEmpty(Result.Data, TEXT("group_state")), FString(TEXT("previewed_not_applied")));
	TestTrue(TEXT("preview should be apply eligible"), Result.Data->GetBoolField(TEXT("apply_eligible")));
	TestTrue(TEXT("preview should expose abort"), Result.Data->GetBoolField(TEXT("abort_available")));
	TestFalse(TEXT("preview should not expose revert"), Result.Data->GetBoolField(TEXT("revert_available")));
	TestFalse(TEXT("preview should not expose checkpoint"), Result.Data->GetBoolField(TEXT("checkpoint_available")));

	const FString PreviewReceiptPath = GetStringFieldOrEmpty(Result.Data, TEXT("preview_receipt_path"));
	const FString ManifestPath = GetStringFieldOrEmpty(Result.Data, TEXT("manifest_path"));
	TestTrue(TEXT("preview receipt should exist"), FPaths::FileExists(PreviewReceiptPath));
	TestTrue(TEXT("preview manifest should exist"), FPaths::FileExists(ManifestPath));

	TSharedPtr<FJsonObject> ManifestObject;
	TestTrue(TEXT("preview manifest should parse as json"), LoadJsonObjectFromDisk(ManifestPath, ManifestObject));
	if (ManifestObject.IsValid())
	{
		TestEqual(TEXT("durable manifest should stay previewed"), GetStringFieldOrEmpty(ManifestObject, TEXT("group_state")), FString(TEXT("previewed_not_applied")));
	}

	FString HeaderAfterPreview;
	TestTrue(TEXT("should reload header after preview"), LoadFileText(HeaderPath, HeaderAfterPreview));
	TestEqual(TEXT("preview should not modify the source header"), HeaderAfterPreview, OriginalHeaderContent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMutationGroup_AbortClosesWithoutApply,
	"UnrealClaude.MutationGroup.AbortClosesWithoutApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMutationGroup_AbortClosesWithoutApply::RunTest(const FString& Parameters)
{
	FMCPTool_MutationGroup Tool;
	const FString HeaderPath = GetSettingsHeaderPath();

	FString OriginalHeaderContent;
	TestTrue(TEXT("should load settings header before abort test"), LoadFileText(HeaderPath, OriginalHeaderContent));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	const FString AbortSuffix = MakeUniqueSuffix(TEXT("abort"));
	const FMCPToolResult PreviewResult = Tool.Execute(MakePreviewParams(
		FString::Printf(TEXT("mutation_group_abort_%s"), *AbortSuffix),
		HeaderPath,
		FString::Printf(TEXT("U5 abort speed tooltip %s"), *AbortSuffix),
		FString::Printf(TEXT("U5 abort profile tooltip %s"), *AbortSuffix)));

	TestTrue(TEXT("abort test preview should succeed"), PreviewResult.bSuccess);
	TestTrue(TEXT("abort test preview should return data"), PreviewResult.Data.IsValid());
	if (!PreviewResult.bSuccess || !PreviewResult.Data.IsValid())
	{
		return false;
	}

	const FString GroupId = GetStringFieldOrEmpty(PreviewResult.Data, TEXT("mutation_group_id"));
	TestFalse(TEXT("preview should expose group id"), GroupId.IsEmpty());
	if (GroupId.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> AbortParams = MakeShared<FJsonObject>();
	AbortParams->SetStringField(TEXT("operation"), TEXT("abort_group"));
	AbortParams->SetStringField(TEXT("mutation_group_id"), GroupId);

	const FMCPToolResult AbortResult = Tool.Execute(AbortParams);
	TestTrue(TEXT("abort should succeed"), AbortResult.bSuccess);
	TestTrue(TEXT("abort should return data"), AbortResult.Data.IsValid());
	if (!AbortResult.bSuccess || !AbortResult.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("abort should close the group"), GetStringFieldOrEmpty(AbortResult.Data, TEXT("group_state")), FString(TEXT("aborted_no_apply")));
	TestEqual(TEXT("abort outcome should be aborted"), GetStringFieldOrEmpty(AbortResult.Data, TEXT("operation_outcome")), FString(TEXT("aborted")));
	TestTrue(TEXT("abort should confirm no changes were applied"), AbortResult.Data->GetBoolField(TEXT("no_changes_applied")));

	const FString AbortReceiptPath = GetStringFieldOrEmpty(AbortResult.Data, TEXT("abort_receipt_path"));
	TestTrue(TEXT("abort receipt should exist"), FPaths::FileExists(AbortReceiptPath));

	FString HeaderAfterAbort;
	TestTrue(TEXT("should reload header after abort"), LoadFileText(HeaderPath, HeaderAfterAbort));
	TestEqual(TEXT("abort should leave the source header unchanged"), HeaderAfterAbort, OriginalHeaderContent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMutationGroup_ApplyAndRevertWithCheckpoint,
	"UnrealClaude.MutationGroup.ApplyAndRevertWithCheckpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMutationGroup_ApplyAndRevertWithCheckpoint::RunTest(const FString& Parameters)
{
	FMCPTool_MutationGroup Tool;
	const FString HeaderPath = GetSettingsHeaderPath();
	const FString ApplySuffix = MakeUniqueSuffix(TEXT("apply"));
	const FString SpeedTooltip = FString::Printf(TEXT("U5 apply speed tooltip %s"), *ApplySuffix);
	const FString ProfileTooltip = FString::Printf(TEXT("U5 apply profile tooltip %s"), *ApplySuffix);

	FString OriginalHeaderContent;
	TestTrue(TEXT("should load settings header before apply/revert"), LoadFileText(HeaderPath, OriginalHeaderContent));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	bool bOverallSuccess = true;

	const FMCPToolResult PreviewResult = Tool.Execute(MakePreviewParams(
		FString::Printf(TEXT("mutation_group_apply_%s"), *ApplySuffix),
		HeaderPath,
		SpeedTooltip,
		ProfileTooltip));

	bOverallSuccess &= TestTrue(TEXT("apply/revert preview should succeed"), PreviewResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("apply/revert preview should return data"), PreviewResult.Data.IsValid());
	if (!PreviewResult.bSuccess || !PreviewResult.Data.IsValid())
	{
		return false;
	}

	const FString GroupId = GetStringFieldOrEmpty(PreviewResult.Data, TEXT("mutation_group_id"));
	bOverallSuccess &= TestFalse(TEXT("preview should expose group id for apply/revert"), GroupId.IsEmpty());
	if (GroupId.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("operation"), TEXT("apply_group"));
	ApplyParams->SetStringField(TEXT("mutation_group_id"), GroupId);

	const FMCPToolResult ApplyResult = Tool.Execute(ApplyParams);
	bOverallSuccess &= TestTrue(TEXT("apply should succeed"), ApplyResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("apply should return data"), ApplyResult.Data.IsValid());
	if (!ApplyResult.bSuccess || !ApplyResult.Data.IsValid())
	{
		SaveFileText(HeaderPath, OriginalHeaderContent);
		return false;
	}

	bOverallSuccess &= TestEqual(TEXT("apply should move state to applied"), GetStringFieldOrEmpty(ApplyResult.Data, TEXT("group_state")), FString(TEXT("applied")));
	bOverallSuccess &= TestEqual(TEXT("apply outcome should be applied"), GetStringFieldOrEmpty(ApplyResult.Data, TEXT("operation_outcome")), FString(TEXT("applied")));
	bOverallSuccess &= TestTrue(TEXT("apply should expose revert"), ApplyResult.Data->GetBoolField(TEXT("revert_available")));
	bOverallSuccess &= TestTrue(TEXT("apply should expose checkpoint"), ApplyResult.Data->GetBoolField(TEXT("checkpoint_available")));

	const FString ApplyReceiptPath = GetStringFieldOrEmpty(ApplyResult.Data, TEXT("apply_receipt_path"));
	bOverallSuccess &= TestTrue(TEXT("apply receipt should exist"), FPaths::FileExists(ApplyReceiptPath));

	TSharedPtr<FJsonObject> PathsObject;
	bOverallSuccess &= TestTrue(TEXT("apply result should expose paths"), TryGetNestedObject(ApplyResult.Data, TEXT("paths"), PathsObject));
	if (PathsObject.IsValid())
	{
		const FString CheckpointManifestPath = GetStringFieldOrEmpty(PathsObject, TEXT("checkpoint_manifest_path"));
		bOverallSuccess &= TestTrue(TEXT("checkpoint manifest should exist"), FPaths::FileExists(CheckpointManifestPath));
	}

	FString HeaderAfterApply;
	bOverallSuccess &= TestTrue(TEXT("should load header after apply"), LoadFileText(HeaderPath, HeaderAfterApply));
	bOverallSuccess &= TestTrue(TEXT("applied header should contain updated speed tooltip"), HeaderAfterApply.Contains(SpeedTooltip));
	bOverallSuccess &= TestTrue(TEXT("applied header should contain inserted profile tooltip"), HeaderAfterApply.Contains(ProfileTooltip));

	TSharedRef<FJsonObject> RevertParams = MakeShared<FJsonObject>();
	RevertParams->SetStringField(TEXT("operation"), TEXT("revert_group"));
	RevertParams->SetStringField(TEXT("mutation_group_id"), GroupId);

	const FMCPToolResult RevertResult = Tool.Execute(RevertParams);
	bOverallSuccess &= TestTrue(TEXT("revert should succeed"), RevertResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("revert should return data"), RevertResult.Data.IsValid());
	if (RevertResult.Data.IsValid())
	{
		bOverallSuccess &= TestEqual(TEXT("revert should move state to reverted"), GetStringFieldOrEmpty(RevertResult.Data, TEXT("group_state")), FString(TEXT("reverted")));
		bOverallSuccess &= TestEqual(TEXT("revert outcome should be reverted"), GetStringFieldOrEmpty(RevertResult.Data, TEXT("operation_outcome")), FString(TEXT("reverted")));
		const FString RevertReceiptPath = GetStringFieldOrEmpty(RevertResult.Data, TEXT("revert_receipt_path"));
		bOverallSuccess &= TestTrue(TEXT("revert receipt should exist"), FPaths::FileExists(RevertReceiptPath));
	}

	FString HeaderAfterRevert;
	const bool bLoadedAfterRevert = LoadFileText(HeaderPath, HeaderAfterRevert);
	bOverallSuccess &= TestTrue(TEXT("should load header after revert"), bLoadedAfterRevert);
	if (bLoadedAfterRevert)
	{
		bOverallSuccess &= TestEqual(TEXT("revert should restore the original header content"), HeaderAfterRevert, OriginalHeaderContent);
	}

	if (HeaderAfterRevert != OriginalHeaderContent)
	{
		const bool bRestored = SaveFileText(HeaderPath, OriginalHeaderContent);
		TestTrue(TEXT("apply/revert safety restore should succeed"), bRestored);
		bOverallSuccess &= bRestored;
	}

	return bOverallSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMutationGroup_ApplyBlockedOnSourceDrift,
	"UnrealClaude.MutationGroup.ApplyBlockedOnSourceDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMutationGroup_ApplyBlockedOnSourceDrift::RunTest(const FString& Parameters)
{
	FMCPTool_MutationGroup Tool;
	const FString HeaderPath = GetSettingsHeaderPath();
	const FString DriftSuffix = MakeUniqueSuffix(TEXT("drift"));
	const FString SpeedTooltip = FString::Printf(TEXT("U5 drift speed tooltip %s"), *DriftSuffix);
	const FString ProfileTooltip = FString::Printf(TEXT("U5 drift profile tooltip %s"), *DriftSuffix);

	FString OriginalHeaderContent;
	TestTrue(TEXT("should load settings header before drift test"), LoadFileText(HeaderPath, OriginalHeaderContent));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	bool bOverallSuccess = true;

	const FMCPToolResult PreviewResult = Tool.Execute(MakePreviewParams(
		FString::Printf(TEXT("mutation_group_drift_%s"), *DriftSuffix),
		HeaderPath,
		SpeedTooltip,
		ProfileTooltip));

	bOverallSuccess &= TestTrue(TEXT("drift preview should succeed"), PreviewResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("drift preview should return data"), PreviewResult.Data.IsValid());
	if (!PreviewResult.bSuccess || !PreviewResult.Data.IsValid())
	{
		return false;
	}

	const FString GroupId = GetStringFieldOrEmpty(PreviewResult.Data, TEXT("mutation_group_id"));
	bOverallSuccess &= TestFalse(TEXT("drift preview should expose group id"), GroupId.IsEmpty());
	if (GroupId.IsEmpty())
	{
		return false;
	}

	const FString DriftMarker = FString::Printf(TEXT("\n// mutation_group drift marker %s\n"), *DriftSuffix);
	const bool bDriftWritten = SaveFileText(HeaderPath, OriginalHeaderContent + DriftMarker);
	bOverallSuccess &= TestTrue(TEXT("should write source drift marker"), bDriftWritten);
	if (!bDriftWritten)
	{
		return false;
	}

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("operation"), TEXT("apply_group"));
	ApplyParams->SetStringField(TEXT("mutation_group_id"), GroupId);

	const FMCPToolResult ApplyResult = Tool.Execute(ApplyParams);
	bOverallSuccess &= TestTrue(TEXT("drift-blocked apply should return structured success"), ApplyResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("drift-blocked apply should return data"), ApplyResult.Data.IsValid());
	if (ApplyResult.Data.IsValid())
	{
		bOverallSuccess &= TestEqual(TEXT("drift-blocked apply outcome should be apply_blocked"), GetStringFieldOrEmpty(ApplyResult.Data, TEXT("operation_outcome")), FString(TEXT("apply_blocked")));

		TSharedPtr<FJsonObject> ValidationGate;
		bOverallSuccess &= TestTrue(TEXT("drift-blocked apply should expose validation gate"), TryGetNestedObject(ApplyResult.Data, TEXT("validation_gate"), ValidationGate));
		if (ValidationGate.IsValid())
		{
			bOverallSuccess &= TestEqual(TEXT("drift-blocked apply gate should mention source drift"), GetStringFieldOrEmpty(ValidationGate, TEXT("outcome")), FString(TEXT("blocked_source_changed_since_preview")));
			bool bEligible = true;
			ValidationGate->TryGetBoolField(TEXT("eligible"), bEligible);
			bOverallSuccess &= TestFalse(TEXT("drift-blocked apply should not stay eligible"), bEligible);
		}
	}

	FString HeaderAfterBlockedApply;
	bOverallSuccess &= TestTrue(TEXT("should load header after blocked apply"), LoadFileText(HeaderPath, HeaderAfterBlockedApply));
	if (!HeaderAfterBlockedApply.IsEmpty())
	{
		bOverallSuccess &= TestFalse(TEXT("blocked apply should not insert speed tooltip"), HeaderAfterBlockedApply.Contains(SpeedTooltip));
		bOverallSuccess &= TestFalse(TEXT("blocked apply should not insert profile tooltip"), HeaderAfterBlockedApply.Contains(ProfileTooltip));
	}

	const bool bRestored = SaveFileText(HeaderPath, OriginalHeaderContent);
	TestTrue(TEXT("drift test should restore original header"), bRestored);
	bOverallSuccess &= bRestored;

	return bOverallSuccess;
}

#endif // WITH_DEV_AUTOMATION_TESTS
