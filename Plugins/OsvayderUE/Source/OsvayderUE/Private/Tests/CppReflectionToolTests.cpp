// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_CppReflection.h"
#include "MCP/Tools/MCPTool_ReportArtifactStatus.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
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

	bool SaveUtf8WithoutBom(const FString& Path, const FString& Content)
	{
		return FFileHelper::SaveStringToFile(
			Content,
			*Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool SaveJsonObjectWithoutBom(const FString& Path, const TSharedPtr<FJsonObject>& JsonObject)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer))
		{
			return false;
		}

		return SaveUtf8WithoutBom(Path, JsonText);
	}

	int32 FindFirstLineContaining(const FString& Content, const FString& Needle)
	{
		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			if (Lines[Index].Contains(Needle))
			{
				return Index + 1;
			}
		}

		return INDEX_NONE;
	}

	TSharedPtr<FJsonObject> ReadSingleReportObject(const FString& ReportId, const int32 PreviewChars = 4000)
	{
		FMCPTool_ReportArtifactStatus StatusTool;
		TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("report_id"), ReportId);
		ParamsObject->SetBoolField(TEXT("latest_only"), false);
		ParamsObject->SetBoolField(TEXT("include_markdown_preview"), true);
		ParamsObject->SetNumberField(TEXT("preview_chars"), PreviewChars);

		const FMCPToolResult Result = StatusTool.Execute(ParamsObject);
		if (!Result.bSuccess || !Result.Data.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Reports = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("reports"), Reports) || !Reports || Reports->Num() != 1)
		{
			return nullptr;
		}

		return (*Reports)[0].IsValid() ? (*Reports)[0]->AsObject() : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_ListContracts,
	"OsvayderUE.CppReflection.ListContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCppReflection_ListContracts::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("operation"), TEXT("list_reflected_contracts"));
	ParamsObject->SetStringField(TEXT("module_scope"), TEXT("project_and_plugin"));
	ParamsObject->SetStringField(TEXT("module_filter"), TEXT("OsvayderUE"));
	ParamsObject->SetNumberField(TEXT("limit"), 20);

	const FMCPToolResult Result = Tool.Execute(ParamsObject);
	TestTrue(TEXT("cpp_reflection list should succeed"), Result.bSuccess);
	TestTrue(TEXT("cpp_reflection list should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Contracts = nullptr;
	TestTrue(TEXT("contracts array should exist"), Result.Data->TryGetArrayField(TEXT("contracts"), Contracts) && Contracts);
	TestTrue(TEXT("contracts array should not be empty"), Contracts && Contracts->Num() > 0);
	if (!Contracts || Contracts->Num() == 0)
	{
		return false;
	}

	bool bSawSettingsClass = false;
	bool bSawStruct = false;
	bool bSawEnum = false;
	for (const TSharedPtr<FJsonValue>& ContractValue : *Contracts)
	{
		const TSharedPtr<FJsonObject> ContractObject = ContractValue.IsValid() ? ContractValue->AsObject() : nullptr;
		if (!ContractObject.IsValid())
		{
			continue;
		}

		FString CppName;
		FString Kind;
		FString ModuleOrigin;
		ContractObject->TryGetStringField(TEXT("cpp_name"), CppName);
		ContractObject->TryGetStringField(TEXT("kind"), Kind);
		ContractObject->TryGetStringField(TEXT("module_origin"), ModuleOrigin);

		bSawSettingsClass = bSawSettingsClass || CppName == TEXT("UOsvayderUESettings");
		bSawStruct = bSawStruct || Kind == TEXT("struct");
		bSawEnum = bSawEnum || Kind == TEXT("enum");
		TestEqual(TEXT("module origin should stay inside plugin scope"), ModuleOrigin, FString(TEXT("plugin")));
	}

	TestTrue(TEXT("list should include UOsvayderUESettings"), bSawSettingsClass);
	TestTrue(TEXT("list should include at least one reflected struct"), bSawStruct);
	TestTrue(TEXT("list should include at least one reflected enum"), bSawEnum);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_GetContract,
	"OsvayderUE.CppReflection.GetContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCppReflection_GetContract::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("operation"), TEXT("get_reflected_contract"));
	ParamsObject->SetStringField(TEXT("symbol"), TEXT("UCharacterConfigDataAsset"));
	ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ParamsObject->SetBoolField(TEXT("include_properties"), true);
	ParamsObject->SetBoolField(TEXT("include_functions"), true);
	ParamsObject->SetBoolField(TEXT("include_metadata"), true);
	ParamsObject->SetNumberField(TEXT("member_limit"), 64);

	const FMCPToolResult Result = Tool.Execute(ParamsObject);
	TestTrue(TEXT("cpp_reflection get should succeed"), Result.bSuccess);
	TestTrue(TEXT("cpp_reflection get should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ContractObjectPtr = nullptr;
	TestTrue(TEXT("contract object should exist"), Result.Data->TryGetObjectField(TEXT("contract"), ContractObjectPtr) && ContractObjectPtr && (*ContractObjectPtr).IsValid());
	if (!ContractObjectPtr || !(*ContractObjectPtr).IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ContractObject = *ContractObjectPtr;

	FString Kind;
	FString CppName;
	ContractObject->TryGetStringField(TEXT("kind"), Kind);
	ContractObject->TryGetStringField(TEXT("cpp_name"), CppName);
	TestEqual(TEXT("contract kind should be class"), Kind, FString(TEXT("class")));
	TestEqual(TEXT("contract name should match"), CppName, FString(TEXT("UCharacterConfigDataAsset")));

	const TSharedPtr<FJsonObject>* SourceLocationPtr = nullptr;
	TestTrue(TEXT("source_location should exist"), ContractObject->TryGetObjectField(TEXT("source_location"), SourceLocationPtr) && SourceLocationPtr && (*SourceLocationPtr).IsValid());
	if (SourceLocationPtr && (*SourceLocationPtr).IsValid())
	{
		FString ResolvedHeaderPath;
		(*SourceLocationPtr)->TryGetStringField(TEXT("resolved_header_path"), ResolvedHeaderPath);
		TestTrue(TEXT("resolved header path should point at CharacterDataTypes.h"), ResolvedHeaderPath.EndsWith(TEXT("CharacterDataTypes.h")));
	}

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TestTrue(TEXT("properties should exist"), ContractObject->TryGetArrayField(TEXT("properties"), Properties) && Properties);
	TestTrue(TEXT("properties should not be empty"), Properties && Properties->Num() > 0);
	if (!Properties || Properties->Num() == 0)
	{
		return false;
	}

	bool bSawConfigId = false;
	bool bSawStatsTable = false;
	for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
	{
		const TSharedPtr<FJsonObject> PropertyObject = PropertyValue.IsValid() ? PropertyValue->AsObject() : nullptr;
		if (!PropertyObject.IsValid())
		{
			continue;
		}

		FString PropertyName;
		FString CppType;
		PropertyObject->TryGetStringField(TEXT("name"), PropertyName);
		PropertyObject->TryGetStringField(TEXT("cpp_type"), CppType);

		bSawConfigId = bSawConfigId || PropertyName == TEXT("ConfigId");
		bSawStatsTable = bSawStatsTable || (PropertyName == TEXT("StatsTable") && CppType.Contains(TEXT("TSoftObjectPtr")));
	}

	TestTrue(TEXT("contract should expose ConfigId property"), bSawConfigId);
	TestTrue(TEXT("contract should expose StatsTable property"), bSawStatsTable);

	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	TestTrue(TEXT("functions should exist"), ContractObject->TryGetArrayField(TEXT("functions"), Functions) && Functions);
	TestTrue(TEXT("functions should not be empty"), Functions && Functions->Num() > 0);
	if (!Functions || Functions->Num() == 0)
	{
		return false;
	}

	bool bSawGetStatsRow = false;
	for (const TSharedPtr<FJsonValue>& FunctionValue : *Functions)
	{
		const TSharedPtr<FJsonObject> FunctionObject = FunctionValue.IsValid() ? FunctionValue->AsObject() : nullptr;
		if (!FunctionObject.IsValid())
		{
			continue;
		}

		FString FunctionName;
		FString ReturnType;
		FunctionObject->TryGetStringField(TEXT("name"), FunctionName);
		FunctionObject->TryGetStringField(TEXT("return_type"), ReturnType);
		bSawGetStatsRow = bSawGetStatsRow || (FunctionName == TEXT("GetStatsRow") && ReturnType.Contains(TEXT("FCharacterStatsRow")));
	}

	TestTrue(TEXT("contract should expose GetStatsRow"), bSawGetStatsRow);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_GetEnumContract,
	"OsvayderUE.CppReflection.GetEnumContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCppReflection_GetEnumContract::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("operation"), TEXT("get_reflected_contract"));
	ParamsObject->SetStringField(TEXT("symbol"), TEXT("EOsvayderUEScopeMode"));
	ParamsObject->SetStringField(TEXT("symbol_kind"), TEXT("enum"));
	ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ParamsObject->SetNumberField(TEXT("member_limit"), 16);

	const FMCPToolResult Result = Tool.Execute(ParamsObject);
	TestTrue(TEXT("cpp_reflection enum get should succeed"), Result.bSuccess);
	TestTrue(TEXT("cpp_reflection enum get should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ContractObjectPtr = nullptr;
	TestTrue(TEXT("enum contract object should exist"), Result.Data->TryGetObjectField(TEXT("contract"), ContractObjectPtr) && ContractObjectPtr && (*ContractObjectPtr).IsValid());
	if (!ContractObjectPtr || !(*ContractObjectPtr).IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Enumerators = nullptr;
	TestTrue(TEXT("enumerators should exist"), (*ContractObjectPtr)->TryGetArrayField(TEXT("enumerators"), Enumerators) && Enumerators);
	TestTrue(TEXT("enumerators should not be empty"), Enumerators && Enumerators->Num() >= 2);
	if (!Enumerators || Enumerators->Num() < 2)
	{
		return false;
	}

	bool bSawPluginOnly = false;
	bool bSawPluginAndProject = false;
	for (const TSharedPtr<FJsonValue>& EnumeratorValue : *Enumerators)
	{
		const TSharedPtr<FJsonObject> EnumeratorObject = EnumeratorValue.IsValid() ? EnumeratorValue->AsObject() : nullptr;
		if (!EnumeratorObject.IsValid())
		{
			continue;
		}

		FString EnumeratorName;
		EnumeratorObject->TryGetStringField(TEXT("name"), EnumeratorName);
		bSawPluginOnly = bSawPluginOnly || EnumeratorName == TEXT("PluginOnly");
		bSawPluginAndProject = bSawPluginAndProject || EnumeratorName == TEXT("PluginAndProject");
	}

	TestTrue(TEXT("enum contract should expose PluginOnly"), bSawPluginOnly);
	TestTrue(TEXT("enum contract should expose PluginAndProject"), bSawPluginAndProject);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_PreviewPropertyMetadataMutation,
	"OsvayderUE.CppReflection.PreviewPropertyMetadataMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_PreviewReflectedPropertyDeclaration,
	"OsvayderUE.CppReflection.PreviewReflectedPropertyDeclaration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCppReflection_PreviewReflectedPropertyDeclaration::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
	ParamsObject->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ParamsObject->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	ParamsObject->SetStringField(TEXT("new_member_name"), TEXT("bP11Slice1PreviewFlag"));
	ParamsObject->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	ParamsObject->SetStringField(TEXT("category"), TEXT("Assistant"));
	ParamsObject->SetStringField(TEXT("default_value"), TEXT("false"));

	const FMCPToolResult Result = Tool.Execute(ParamsObject);
	TestTrue(TEXT("property declaration preview should succeed"), Result.bSuccess);
	TestTrue(TEXT("property declaration preview should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	FString PreviewSchemaVersion;
	bool bPreviewOnly = false;
	bool bApplyAvailableNow = true;
	Result.Data->TryGetStringField(TEXT("preview_schema_version"), PreviewSchemaVersion);
	Result.Data->TryGetBoolField(TEXT("preview_only"), bPreviewOnly);
	Result.Data->TryGetBoolField(TEXT("declaration_apply_available_now"), bApplyAvailableNow);
	TestEqual(TEXT("preview schema version should stay stable"), PreviewSchemaVersion, FString(TEXT("reflected_property_declaration_preview_v1")));
	TestTrue(TEXT("declaration preview should remain preview only"), bPreviewOnly);
	TestTrue(TEXT("declaration apply should now be available for the same bounded shape"), bApplyAvailableNow);

	const TSharedPtr<FJsonObject>* AnchorMemberObjectPtr = nullptr;
	TestTrue(TEXT("anchor member should exist"), Result.Data->TryGetObjectField(TEXT("anchor_member_before"), AnchorMemberObjectPtr) && AnchorMemberObjectPtr && (*AnchorMemberObjectPtr).IsValid());
	if (AnchorMemberObjectPtr && (*AnchorMemberObjectPtr).IsValid())
	{
		FString AnchorName;
		(*AnchorMemberObjectPtr)->TryGetStringField(TEXT("name"), AnchorName);
		TestEqual(TEXT("anchor member should stay DefaultCodexSpeedMode"), AnchorName, FString(TEXT("DefaultCodexSpeedMode")));
	}

	const TSharedPtr<FJsonObject>* ConstraintsObjectPtr = nullptr;
	TestTrue(TEXT("ownership scope constraints should exist"), Result.Data->TryGetObjectField(TEXT("ownership_scope_constraints"), ConstraintsObjectPtr) && ConstraintsObjectPtr && (*ConstraintsObjectPtr).IsValid());
	if (ConstraintsObjectPtr && (*ConstraintsObjectPtr).IsValid())
	{
		bool bRequiresPluginOwnedHeader = false;
		bool bRequiresReflectedUClass = false;
		(*ConstraintsObjectPtr)->TryGetBoolField(TEXT("requires_plugin_owned_header"), bRequiresPluginOwnedHeader);
		(*ConstraintsObjectPtr)->TryGetBoolField(TEXT("requires_reflected_uclass"), bRequiresReflectedUClass);
		TestTrue(TEXT("constraints should require plugin-owned header"), bRequiresPluginOwnedHeader);
		TestTrue(TEXT("constraints should require reflected UCLASS"), bRequiresReflectedUClass);
	}

	const TSharedPtr<FJsonObject>* FutureApplyObjectPtr = nullptr;
	TestTrue(TEXT("future apply expectations should exist"), Result.Data->TryGetObjectField(TEXT("future_apply_expectations"), FutureApplyObjectPtr) && FutureApplyObjectPtr && (*FutureApplyObjectPtr).IsValid());
	if (FutureApplyObjectPtr && (*FutureApplyObjectPtr).IsValid())
	{
		bool bApplyAvailableNowFromExpectations = false;
		bool bBoundedSingleShape = false;
		bool bRequiresFreshBuild = false;
		FString ApplyOperation;
		(*FutureApplyObjectPtr)->TryGetBoolField(TEXT("apply_available_now"), bApplyAvailableNowFromExpectations);
		(*FutureApplyObjectPtr)->TryGetBoolField(TEXT("apply_remains_bounded_to_one_bool_property_shape"), bBoundedSingleShape);
		(*FutureApplyObjectPtr)->TryGetBoolField(TEXT("requires_fresh_preflight_rebuild_after_apply"), bRequiresFreshBuild);
		(*FutureApplyObjectPtr)->TryGetStringField(TEXT("available_apply_operation"), ApplyOperation);
		TestTrue(TEXT("expectations should expose current bounded apply availability"), bApplyAvailableNowFromExpectations);
		TestTrue(TEXT("expectations should keep apply bounded to one bool property shape"), bBoundedSingleShape);
		TestTrue(TEXT("expectations should require fresh preflight rebuild after apply"), bRequiresFreshBuild);
		TestEqual(TEXT("expectations should point at apply_reflected_property_declaration"), ApplyOperation, FString(TEXT("apply_reflected_property_declaration")));
	}

	const TSharedPtr<FJsonObject>* PreviewObjectPtr = nullptr;
	TestTrue(TEXT("preview object should exist"), Result.Data->TryGetObjectField(TEXT("preview"), PreviewObjectPtr) && PreviewObjectPtr && (*PreviewObjectPtr).IsValid());
	if (!PreviewObjectPtr || !(*PreviewObjectPtr).IsValid())
	{
		return false;
	}

	FString SchemaVersion;
	FString DeclarationKind;
	FString GeneratedSnippet;
	FString GeneratedPropertyLine;
	FString PreviewExcerptAfter;
	(*PreviewObjectPtr)->TryGetStringField(TEXT("schema_version"), SchemaVersion);
	(*PreviewObjectPtr)->TryGetStringField(TEXT("declaration_kind"), DeclarationKind);
	(*PreviewObjectPtr)->TryGetStringField(TEXT("generated_snippet"), GeneratedSnippet);
	(*PreviewObjectPtr)->TryGetStringField(TEXT("generated_declaration"), GeneratedPropertyLine);
	(*PreviewObjectPtr)->TryGetStringField(TEXT("preview_excerpt_after"), PreviewExcerptAfter);

	TestEqual(TEXT("nested preview schema version should stay stable"), SchemaVersion, FString(TEXT("reflected_property_declaration_preview_v1")));
	TestEqual(TEXT("declaration kind should stay explicit"), DeclarationKind, FString(TEXT("reflected_uproperty_member_declaration")));
	TestTrue(TEXT("generated snippet should include preview bool property"), GeneratedSnippet.Contains(TEXT("bool bP11Slice1PreviewFlag = false;")));
	TestTrue(TEXT("generated snippet should include reflected category"), GeneratedSnippet.Contains(TEXT("Category = \"Assistant\"")));
	TestTrue(TEXT("generated declaration should include preview property name"), GeneratedPropertyLine.Contains(TEXT("bP11Slice1PreviewFlag")));
	TestTrue(TEXT("preview excerpt should show inserted property"), PreviewExcerptAfter.Contains(TEXT("bP11Slice1PreviewFlag")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_PreviewReflectedPropertyDeclarationBlockedShape,
	"OsvayderUE.CppReflection.PreviewReflectedPropertyDeclarationBlockedShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCppReflection_PreviewReflectedPropertyDeclarationBlockedShape::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
	ParamsObject->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ParamsObject->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	ParamsObject->SetStringField(TEXT("new_member_name"), TEXT("PreviewStringProperty"));
	ParamsObject->SetStringField(TEXT("property_cpp_type"), TEXT("FString"));
	ParamsObject->SetStringField(TEXT("category"), TEXT("Assistant"));

	const FMCPToolResult Result = Tool.Execute(ParamsObject);
	TestFalse(TEXT("unsupported declaration shape should fail"), Result.bSuccess);
	TestTrue(TEXT("unsupported declaration shape should explain bool-only boundary"), Result.Message.Contains(TEXT("supports only bool")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_ApplyReflectedPropertyDeclaration,
	"OsvayderUE.CppReflection.ApplyReflectedPropertyDeclaration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCppReflection_ApplyReflectedPropertyDeclaration::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;

	TSharedRef<FJsonObject> PreviewParams = MakeShared<FJsonObject>();
	PreviewParams->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
	PreviewParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	PreviewParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	PreviewParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	PreviewParams->SetStringField(TEXT("new_member_name"), TEXT("bP11Slice2AppliedFlag"));
	PreviewParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	PreviewParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	PreviewParams->SetStringField(TEXT("default_value"), TEXT("false"));

	const FMCPToolResult PreviewResult = Tool.Execute(PreviewParams);
	TestTrue(TEXT("declaration preview before apply should succeed"), PreviewResult.bSuccess);
	TestTrue(TEXT("declaration preview before apply should return data"), PreviewResult.Data.IsValid());
	if (!PreviewResult.bSuccess || !PreviewResult.Data.IsValid())
	{
		return false;
	}

	FString PreviewSchemaVersion;
	PreviewResult.Data->TryGetStringField(TEXT("preview_schema_version"), PreviewSchemaVersion);

	const TSharedPtr<FJsonObject>* PreviewObjectPtr = nullptr;
	TestTrue(TEXT("nested declaration preview object should exist"), PreviewResult.Data->TryGetObjectField(TEXT("preview"), PreviewObjectPtr) && PreviewObjectPtr && (*PreviewObjectPtr).IsValid());
	if (!PreviewObjectPtr || !(*PreviewObjectPtr).IsValid())
	{
		return false;
	}

	FString HeaderPath;
	FString ExpectedSourceHashBefore;
	(*PreviewObjectPtr)->TryGetStringField(TEXT("header_path"), HeaderPath);
	(*PreviewObjectPtr)->TryGetStringField(TEXT("source_hash_before"), ExpectedSourceHashBefore);
	TestTrue(TEXT("preview should expose concrete header path"), !HeaderPath.IsEmpty());
	TestTrue(TEXT("preview should expose source hash before"), !ExpectedSourceHashBefore.IsEmpty());
	if (HeaderPath.IsEmpty() || ExpectedSourceHashBefore.IsEmpty())
	{
		return false;
	}

	FString OriginalHeaderContent;
	TestTrue(TEXT("should load original header content before declaration apply"), FFileHelper::LoadFileToString(OriginalHeaderContent, *HeaderPath));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	bool bOverallSuccess = true;

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("operation"), TEXT("apply_reflected_property_declaration"));
	ApplyParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ApplyParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ApplyParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	ApplyParams->SetStringField(TEXT("new_member_name"), TEXT("bP11Slice2AppliedFlag"));
	ApplyParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	ApplyParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	ApplyParams->SetStringField(TEXT("default_value"), TEXT("false"));
	ApplyParams->SetStringField(TEXT("expected_preview_schema_version"), PreviewSchemaVersion);
	ApplyParams->SetStringField(TEXT("expected_source_hash_before"), ExpectedSourceHashBefore);

	const FMCPToolResult ApplyResult = Tool.Execute(ApplyParams);
	bOverallSuccess &= TestTrue(TEXT("declaration apply should succeed"), ApplyResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("declaration apply should return data"), ApplyResult.Data.IsValid());
	if (!ApplyResult.bSuccess || !ApplyResult.Data.IsValid())
	{
		const bool bRestoredEarly = FFileHelper::SaveStringToFile(
			OriginalHeaderContent,
			*HeaderPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TestTrue(TEXT("early cleanup should restore original header"), bRestoredEarly);
		return false;
	}

	FString ReceiptPath;
	ApplyResult.Data->TryGetStringField(TEXT("receipt_path"), ReceiptPath);
	bOverallSuccess &= TestTrue(TEXT("declaration apply should emit receipt path"), !ReceiptPath.IsEmpty());
	if (!ReceiptPath.IsEmpty())
	{
		bOverallSuccess &= TestTrue(TEXT("declaration apply receipt should exist"), FPaths::FileExists(ReceiptPath));
	}

	const TSharedPtr<FJsonObject>* ApplyObjectPtr = nullptr;
	bOverallSuccess &= TestTrue(TEXT("nested declaration apply object should exist"), ApplyResult.Data->TryGetObjectField(TEXT("apply"), ApplyObjectPtr) && ApplyObjectPtr && (*ApplyObjectPtr).IsValid());
	if (ApplyObjectPtr && (*ApplyObjectPtr).IsValid())
	{
		bool bApplied = false;
		bool bRestoreReady = false;
		FString CheckpointPath;
		FString GeneratedDeclaration;
		(*ApplyObjectPtr)->TryGetBoolField(TEXT("applied"), bApplied);
		(*ApplyObjectPtr)->TryGetBoolField(TEXT("restore_ready"), bRestoreReady);
		(*ApplyObjectPtr)->TryGetStringField(TEXT("checkpoint_path"), CheckpointPath);
		(*ApplyObjectPtr)->TryGetStringField(TEXT("generated_declaration"), GeneratedDeclaration);
		bOverallSuccess &= TestTrue(TEXT("declaration apply should report applied"), bApplied);
		bOverallSuccess &= TestTrue(TEXT("declaration apply should be restore-ready"), bRestoreReady);
		bOverallSuccess &= TestTrue(TEXT("declaration checkpoint path should exist"), !CheckpointPath.IsEmpty() && FPaths::FileExists(CheckpointPath));
		bOverallSuccess &= TestTrue(TEXT("generated declaration should contain the applied property"), GeneratedDeclaration.Contains(TEXT("bP11Slice2AppliedFlag")));
	}

	const TSharedPtr<FJsonObject>* ContinuityObjectPtr = nullptr;
	bOverallSuccess &= TestTrue(TEXT("preview continuity should exist"), ApplyResult.Data->TryGetObjectField(TEXT("preview_continuity"), ContinuityObjectPtr) && ContinuityObjectPtr && (*ContinuityObjectPtr).IsValid());
	if (ContinuityObjectPtr && (*ContinuityObjectPtr).IsValid())
	{
		bool bHashMatch = false;
		bool bSnippetMatch = false;
		(*ContinuityObjectPtr)->TryGetBoolField(TEXT("preview_hash_matches_apply_basis"), bHashMatch);
		(*ContinuityObjectPtr)->TryGetBoolField(TEXT("generated_snippet_matches_apply"), bSnippetMatch);
		bOverallSuccess &= TestTrue(TEXT("preview continuity should keep the same source hash basis"), bHashMatch);
		bOverallSuccess &= TestTrue(TEXT("preview continuity should keep the same snippet"), bSnippetMatch);
	}

	const TSharedPtr<FJsonObject>* HandshakePtr = nullptr;
	bOverallSuccess &= TestTrue(TEXT("declaration apply compile handshake should exist"), ApplyResult.Data->TryGetObjectField(TEXT("compile_handshake"), HandshakePtr) && HandshakePtr && (*HandshakePtr).IsValid());
	if (HandshakePtr && (*HandshakePtr).IsValid())
	{
		FString RecommendedAction;
		FString Status;
		bool bSupportsInProcessCompile = true;
		(*HandshakePtr)->TryGetStringField(TEXT("recommended_action"), RecommendedAction);
		(*HandshakePtr)->TryGetStringField(TEXT("status"), Status);
		(*HandshakePtr)->TryGetBoolField(TEXT("supports_in_process_reflected_compile"), bSupportsInProcessCompile);
		bOverallSuccess &= TestEqual(TEXT("declaration apply should require preflight launcher"), RecommendedAction, FString(TEXT("close_editor_and_run_preflight_launcher")));
		bOverallSuccess &= TestFalse(TEXT("declaration apply should not claim in-process reflected compile"), bSupportsInProcessCompile);
		bOverallSuccess &= TestTrue(TEXT("declaration apply handshake should keep rebuild pending"), Status == TEXT("rebuild_required") || Status == TEXT("build_already_stale"));
	}

	FString HeaderAfterApply;
	bOverallSuccess &= TestTrue(TEXT("should read header after declaration apply"), FFileHelper::LoadFileToString(HeaderAfterApply, *HeaderPath));
	if (!HeaderAfterApply.IsEmpty())
	{
		bOverallSuccess &= TestTrue(TEXT("applied header should contain generated declaration"), HeaderAfterApply.Contains(TEXT("bool bP11Slice2AppliedFlag = false;")));
		bOverallSuccess &= TestTrue(TEXT("applied header should contain generated UPROPERTY"), HeaderAfterApply.Contains(TEXT("UPROPERTY(EditAnywhere, Category = \"Assistant\")")));
	}

	TSharedRef<FJsonObject> RevertParams = MakeShared<FJsonObject>();
	RevertParams->SetStringField(TEXT("operation"), TEXT("revert_reflected_property_declaration"));
	RevertParams->SetStringField(TEXT("receipt_path"), ReceiptPath);

	const FMCPToolResult RevertResult = Tool.Execute(RevertParams);
	bOverallSuccess &= TestTrue(TEXT("declaration revert should succeed"), RevertResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("declaration revert should return data"), RevertResult.Data.IsValid());
	if (RevertResult.Data.IsValid())
	{
		FString RevertReceiptPath;
		RevertResult.Data->TryGetStringField(TEXT("revert_receipt_path"), RevertReceiptPath);
		bOverallSuccess &= TestTrue(TEXT("declaration revert should emit a revert receipt"), !RevertReceiptPath.IsEmpty() && FPaths::FileExists(RevertReceiptPath));

		const TSharedPtr<FJsonObject>* RevertObjectPtr = nullptr;
		bOverallSuccess &= TestTrue(TEXT("nested declaration revert object should exist"), RevertResult.Data->TryGetObjectField(TEXT("revert"), RevertObjectPtr) && RevertObjectPtr && (*RevertObjectPtr).IsValid());
		if (RevertObjectPtr && (*RevertObjectPtr).IsValid())
		{
			bool bReverted = false;
			FString SourceHashAfterRevert;
			(*RevertObjectPtr)->TryGetBoolField(TEXT("reverted"), bReverted);
			(*RevertObjectPtr)->TryGetStringField(TEXT("source_hash_after_revert"), SourceHashAfterRevert);
			bOverallSuccess &= TestTrue(TEXT("declaration revert should report reverted"), bReverted);
			bOverallSuccess &= TestEqual(TEXT("revert should restore the original source hash"), SourceHashAfterRevert, ExpectedSourceHashBefore);
		}
	}

	FString HeaderAfterRevert;
	bOverallSuccess &= TestTrue(TEXT("should read header after declaration revert"), FFileHelper::LoadFileToString(HeaderAfterRevert, *HeaderPath));
	if (!HeaderAfterRevert.IsEmpty())
	{
		bOverallSuccess &= TestEqual(TEXT("declaration revert should restore original header content"), HeaderAfterRevert, OriginalHeaderContent);
	}

	if (HeaderAfterRevert != OriginalHeaderContent)
	{
		const bool bRestored = FFileHelper::SaveStringToFile(
			OriginalHeaderContent,
			*HeaderPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TestTrue(TEXT("declaration apply test cleanup should restore original header"), bRestored);
		bOverallSuccess &= bRestored;
	}

	return bOverallSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_ApplyReflectedPropertyDeclarationBlockedShape,
	"OsvayderUE.CppReflection.ApplyReflectedPropertyDeclarationBlockedShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_BuildReflectedPropertyDeclarationEvidenceBundleSuccess,
	"OsvayderUE.CppReflection.BuildReflectedPropertyDeclarationEvidenceBundleSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_BuildReflectedPropertyDeclarationEvidenceBundleFailedBuild,
	"OsvayderUE.CppReflection.BuildReflectedPropertyDeclarationEvidenceBundleFailedBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_BuildReflectedPropertyDeclarationEvidenceBundleRevertCloseout,
	"OsvayderUE.CppReflection.BuildReflectedPropertyDeclarationEvidenceBundleRevertCloseout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_InspectReflectedPropertyDeclarationBuildFailure,
	"OsvayderUE.CppReflection.InspectReflectedPropertyDeclarationBuildFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCppReflection_InspectReflectedPropertyDeclarationBuildFailure::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;
	const FString PropertyName = TEXT("bitor");
	const FString GeneratedDeclarationLine = TEXT("bool bitor = false;");

	TSharedRef<FJsonObject> PreviewParams = MakeShared<FJsonObject>();
	PreviewParams->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
	PreviewParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	PreviewParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	PreviewParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	PreviewParams->SetStringField(TEXT("new_member_name"), PropertyName);
	PreviewParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	PreviewParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	PreviewParams->SetStringField(TEXT("default_value"), TEXT("false"));

	const FMCPToolResult PreviewResult = Tool.Execute(PreviewParams);
	TestTrue(TEXT("failed-build declaration preview should succeed"), PreviewResult.bSuccess);
	TestTrue(TEXT("failed-build declaration preview should return data"), PreviewResult.Data.IsValid());
	if (!PreviewResult.bSuccess || !PreviewResult.Data.IsValid())
	{
		return false;
	}

	FString PreviewSchemaVersion;
	PreviewResult.Data->TryGetStringField(TEXT("preview_schema_version"), PreviewSchemaVersion);
	const TSharedPtr<FJsonObject> PreviewObject = GetObjectFieldOrNull(PreviewResult.Data, TEXT("preview"));
	TestTrue(TEXT("failed-build preview object should exist"), PreviewObject.IsValid());
	if (!PreviewObject.IsValid())
	{
		return false;
	}

	FString HeaderPath;
	FString ExpectedSourceHashBefore;
	PreviewObject->TryGetStringField(TEXT("header_path"), HeaderPath);
	PreviewObject->TryGetStringField(TEXT("source_hash_before"), ExpectedSourceHashBefore);
	TestTrue(TEXT("failed-build preview should expose header_path"), !HeaderPath.IsEmpty());
	TestTrue(TEXT("failed-build preview should expose source_hash_before"), !ExpectedSourceHashBefore.IsEmpty());
	if (HeaderPath.IsEmpty() || ExpectedSourceHashBefore.IsEmpty())
	{
		return false;
	}

	FString OriginalHeaderContent;
	TestTrue(TEXT("should load original header before failed-build apply"), FFileHelper::LoadFileToString(OriginalHeaderContent, *HeaderPath));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	bool bOverallSuccess = true;
	FString ApplyReceiptPath;
	const auto RestoreOriginalHeader = [&]() -> bool
	{
		return SaveUtf8WithoutBom(HeaderPath, OriginalHeaderContent);
	};

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("operation"), TEXT("apply_reflected_property_declaration"));
	ApplyParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ApplyParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ApplyParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	ApplyParams->SetStringField(TEXT("new_member_name"), PropertyName);
	ApplyParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	ApplyParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	ApplyParams->SetStringField(TEXT("default_value"), TEXT("false"));
	ApplyParams->SetStringField(TEXT("expected_preview_schema_version"), PreviewSchemaVersion);
	ApplyParams->SetStringField(TEXT("expected_source_hash_before"), ExpectedSourceHashBefore);

	const FMCPToolResult ApplyResult = Tool.Execute(ApplyParams);
	bOverallSuccess &= TestTrue(TEXT("failed-build declaration apply should succeed"), ApplyResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("failed-build declaration apply should return data"), ApplyResult.Data.IsValid());
	if (!ApplyResult.bSuccess || !ApplyResult.Data.IsValid())
	{
		TestTrue(TEXT("failed-build early cleanup should restore original header"), RestoreOriginalHeader());
		return false;
	}

	ApplyResult.Data->TryGetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	bOverallSuccess &= TestTrue(TEXT("failed-build declaration apply should emit receipt path"), !ApplyReceiptPath.IsEmpty());
	if (ApplyReceiptPath.IsEmpty())
	{
		TestTrue(TEXT("failed-build cleanup should restore original header when receipt is missing"), RestoreOriginalHeader());
		return false;
	}

	const TSharedPtr<FJsonObject> ApplyObject = GetObjectFieldOrNull(ApplyResult.Data, TEXT("apply"));
	bOverallSuccess &= TestTrue(TEXT("failed-build apply object should exist"), ApplyObject.IsValid());
	if (!ApplyObject.IsValid())
	{
		TestTrue(TEXT("failed-build cleanup should restore original header when apply object is missing"), RestoreOriginalHeader());
		return false;
	}

	FString ExpectedAppliedSourceHash;
	FString CheckpointPath;
	FString GeneratedSnippet;
	double ReceiptInsertionLineNumber = 0.0;
	int32 ReceiptInsertionLine = INDEX_NONE;
	ApplyObject->TryGetStringField(TEXT("source_hash_after_apply"), ExpectedAppliedSourceHash);
	ApplyObject->TryGetStringField(TEXT("checkpoint_path"), CheckpointPath);
	ApplyObject->TryGetStringField(TEXT("generated_snippet"), GeneratedSnippet);
	if (ApplyObject->TryGetNumberField(TEXT("insertion_line"), ReceiptInsertionLineNumber))
	{
		ReceiptInsertionLine = static_cast<int32>(ReceiptInsertionLineNumber);
	}
	bOverallSuccess &= TestTrue(TEXT("failed-build apply should expose source_hash_after_apply"), !ExpectedAppliedSourceHash.IsEmpty());
	bOverallSuccess &= TestTrue(TEXT("failed-build apply should expose checkpoint_path"), !CheckpointPath.IsEmpty() && FPaths::FileExists(CheckpointPath));
	bOverallSuccess &= TestTrue(TEXT("failed-build apply should expose generated_snippet"), !GeneratedSnippet.IsEmpty());
	bOverallSuccess &= TestTrue(TEXT("failed-build apply should expose insertion_line"), ReceiptInsertionLine != INDEX_NONE);

	FString MutatedHeaderContent;
	bOverallSuccess &= TestTrue(TEXT("should read header after failed-build apply"), FFileHelper::LoadFileToString(MutatedHeaderContent, *HeaderPath));
	bOverallSuccess &= TestTrue(TEXT("failed-build applied header should contain invalid declaration"), MutatedHeaderContent.Contains(GeneratedDeclarationLine));
	if (MutatedHeaderContent.IsEmpty() || !MutatedHeaderContent.Contains(GeneratedDeclarationLine) || ReceiptInsertionLine == INDEX_NONE)
	{
		TestTrue(TEXT("failed-build cleanup should restore original header when mutated declaration is missing"), RestoreOriginalHeader());
		return false;
	}

	TArray<FString> GeneratedSnippetLines;
	GeneratedSnippet.ParseIntoArrayLines(GeneratedSnippetLines, true);
	const int32 GeneratedSnippetLineCount = GeneratedSnippetLines.Num() > 0 ? GeneratedSnippetLines.Num() : 1;
	const int32 MemberLineNumber = ReceiptInsertionLine + GeneratedSnippetLineCount - 1;
	bOverallSuccess &= TestTrue(TEXT("failed-build test should derive a valid generated declaration line from receipt context"), MemberLineNumber >= ReceiptInsertionLine);

	const FString AutomationDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("Automation"));
	IFileManager::Get().MakeDirectory(*AutomationDirectory, true);
	const FString StrongLogPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_failed_build_fixture.log"));
	const FString StrongLogContent = FString::Printf(
		TEXT("Using Visual Studio 2022 14.39.33523 toolchain.\n")
		TEXT("%s(%d,14): error C2059: syntax error: 'bitor'\n")
		TEXT("%s(%d,19): error C2143: syntax error: missing ';' before '='\n")
		TEXT("Result: Failed (OtherCompilationError)\n"),
		*HeaderPath,
		MemberLineNumber,
		*HeaderPath,
		MemberLineNumber);
	bOverallSuccess &= TestTrue(TEXT("failed-build test should write strong synthetic build log"), SaveUtf8WithoutBom(StrongLogPath, StrongLogContent));
	if (!FPaths::FileExists(StrongLogPath))
	{
		TestTrue(TEXT("failed-build cleanup should restore original header when strong synthetic log is missing"), RestoreOriginalHeader());
		return false;
	}

	const FString LineContextLogPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_failed_build_line_context_fixture.log"));
	const FString LineContextLogContent = FString::Printf(
		TEXT("Using Visual Studio 2022 14.39.33523 toolchain.\n")
		TEXT("%s(%d,7): error C2059: syntax error: |\n")
		TEXT("%s(%d,20): error C2238: unexpected token(s) preceding ';'\n")
		TEXT("Result: Failed (OtherCompilationError)\n"),
		*HeaderPath,
		MemberLineNumber,
		*HeaderPath,
		MemberLineNumber);
	bOverallSuccess &= TestTrue(TEXT("failed-build test should write line-context synthetic build log"), SaveUtf8WithoutBom(LineContextLogPath, LineContextLogContent));

	const int32 HeaderOnlyLineNumber = FMath::Max(1, MemberLineNumber - 3);
	const FString HeaderOnlyLogPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_failed_build_header_only_fixture.log"));
	const FString HeaderOnlyLogContent = FString::Printf(
		TEXT("Using Visual Studio 2022 14.39.33523 toolchain.\n")
		TEXT("%s(%d,7): error C2059: syntax error: |\n")
		TEXT("Result: Failed (OtherCompilationError)\n"),
		*HeaderPath,
		HeaderOnlyLineNumber);
	bOverallSuccess &= TestTrue(TEXT("failed-build test should write header-only synthetic build log"), SaveUtf8WithoutBom(HeaderOnlyLogPath, HeaderOnlyLogContent));
	if (!FPaths::FileExists(LineContextLogPath) || !FPaths::FileExists(HeaderOnlyLogPath))
	{
		TestTrue(TEXT("failed-build cleanup should restore original header when one of the synthetic logs is missing"), RestoreOriginalHeader());
		return false;
	}

	auto RunInspect = [&](const FString& BuildLogPath) -> FMCPToolResult
	{
		TSharedRef<FJsonObject> InspectParams = MakeShared<FJsonObject>();
		InspectParams->SetStringField(TEXT("operation"), TEXT("inspect_reflected_property_declaration_build_failure"));
		InspectParams->SetStringField(TEXT("receipt_path"), ApplyReceiptPath);
		InspectParams->SetStringField(TEXT("build_log_path"), BuildLogPath);
		return Tool.Execute(InspectParams);
	};

	const FMCPToolResult StrongInspectResult = RunInspect(StrongLogPath);
	bOverallSuccess &= TestTrue(TEXT("strong failed-build inspection should succeed"), StrongInspectResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("strong failed-build inspection should return data"), StrongInspectResult.Data.IsValid());
	if (StrongInspectResult.Data.IsValid())
	{
		FString DiagnosticSchemaVersion;
		FString FailureStateClassification;
		FString RevertStateClassification;
		FString ReflectionStateClassification;
		FString DiagnosticReceiptPath;
		StrongInspectResult.Data->TryGetStringField(TEXT("diagnostic_schema_version"), DiagnosticSchemaVersion);
		StrongInspectResult.Data->TryGetStringField(TEXT("failure_state_classification"), FailureStateClassification);
		StrongInspectResult.Data->TryGetStringField(TEXT("revert_state_classification"), RevertStateClassification);
		StrongInspectResult.Data->TryGetStringField(TEXT("reflection_state_classification"), ReflectionStateClassification);
		StrongInspectResult.Data->TryGetStringField(TEXT("diagnostic_receipt_path"), DiagnosticReceiptPath);
		bOverallSuccess &= TestEqual(TEXT("failed-build inspection schema version should stay stable"), DiagnosticSchemaVersion, FString(TEXT("reflected_property_declaration_build_failure_diagnostics_v1")));
		bOverallSuccess &= TestEqual(TEXT("failed-build inspection should classify mutation_written_but_build_failed"), FailureStateClassification, FString(TEXT("mutation_written_but_build_failed")));
		bOverallSuccess &= TestEqual(TEXT("failed-build inspection should classify revert_available"), RevertStateClassification, FString(TEXT("revert_available")));
		bOverallSuccess &= TestEqual(TEXT("failed-build inspection should keep reflection pending after build failure"), ReflectionStateClassification, FString(TEXT("reflection_not_yet_proven_due_to_build_failure")));
		bOverallSuccess &= TestTrue(TEXT("failed-build inspection should emit diagnostic receipt path"), !DiagnosticReceiptPath.IsEmpty() && FPaths::FileExists(DiagnosticReceiptPath));

		const TSharedPtr<FJsonObject> StrongArtifactObject = GetObjectFieldOrNull(StrongInspectResult.Data, TEXT("build_diagnostic_artifact"));
		bOverallSuccess &= TestTrue(TEXT("strong failed-build artifact object should exist"), StrongArtifactObject.IsValid());
		if (StrongArtifactObject.IsValid())
		{
			bool bArtifactExists = false;
			bool bBuildFailureObserved = false;
			int32 MatchingErrorCount = 0;
			FString LinkageConfidence;
			int32 MemberNameMatchCount = 0;
			int32 GeneratedDeclarationMatchCount = 0;
			StrongArtifactObject->TryGetBoolField(TEXT("artifact_exists"), bArtifactExists);
			StrongArtifactObject->TryGetBoolField(TEXT("build_failure_observed"), bBuildFailureObserved);
			StrongArtifactObject->TryGetNumberField(TEXT("matching_error_count"), MatchingErrorCount);
			StrongArtifactObject->TryGetNumberField(TEXT("member_name_match_count"), MemberNameMatchCount);
			StrongArtifactObject->TryGetNumberField(TEXT("generated_declaration_match_count"), GeneratedDeclarationMatchCount);
			StrongArtifactObject->TryGetStringField(TEXT("linkage_confidence"), LinkageConfidence);
			bOverallSuccess &= TestTrue(TEXT("strong failed-build artifact should exist"), bArtifactExists);
			bOverallSuccess &= TestTrue(TEXT("strong failed-build artifact should observe build failure"), bBuildFailureObserved);
			bOverallSuccess &= TestTrue(TEXT("strong failed-build artifact should contain receipt-linked errors"), MatchingErrorCount > 0);
			bOverallSuccess &= TestTrue(TEXT("strong failed-build artifact should keep member or declaration linkage explicit"), MemberNameMatchCount > 0 || GeneratedDeclarationMatchCount > 0);
			bOverallSuccess &= TestEqual(TEXT("strong failed-build artifact should earn the strongest linkage tier"), LinkageConfidence, FString(TEXT("receipt_snippet_or_member_linked")));
		}

		const TSharedPtr<FJsonObject> CurrentSourceState = GetObjectFieldOrNull(StrongInspectResult.Data, TEXT("current_source_state"));
		bOverallSuccess &= TestTrue(TEXT("current_source_state should exist"), CurrentSourceState.IsValid());
		if (CurrentSourceState.IsValid())
		{
			bool bMatchesExpectedAppliedHash = false;
			bool bRevertAvailableNow = false;
			FString CurrentSourceHash;
			CurrentSourceState->TryGetBoolField(TEXT("matches_expected_applied_hash"), bMatchesExpectedAppliedHash);
			CurrentSourceState->TryGetBoolField(TEXT("revert_available_now"), bRevertAvailableNow);
			CurrentSourceState->TryGetStringField(TEXT("current_source_hash"), CurrentSourceHash);
			bOverallSuccess &= TestTrue(TEXT("failed-build current source should still match applied hash"), bMatchesExpectedAppliedHash);
			bOverallSuccess &= TestTrue(TEXT("failed-build current source should keep revert available"), bRevertAvailableNow);
			bOverallSuccess &= TestEqual(TEXT("failed-build current source hash should match apply receipt hash"), CurrentSourceHash, ExpectedAppliedSourceHash);
		}

		const TSharedPtr<FJsonObject> CloseoutSequence = GetObjectFieldOrNull(StrongInspectResult.Data, TEXT("recommended_closeout_sequence"));
		bOverallSuccess &= TestTrue(TEXT("recommended_closeout_sequence should exist"), CloseoutSequence.IsValid());
		if (CloseoutSequence.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
			bOverallSuccess &= TestTrue(TEXT("recommended_closeout_sequence should include steps"), CloseoutSequence->TryGetArrayField(TEXT("recommended_steps"), Steps) && Steps);
			if (Steps && Steps->Num() > 1)
			{
				FString FirstStep;
				FString SecondStep;
				(*Steps)[0]->TryGetString(FirstStep);
				(*Steps)[1]->TryGetString(SecondStep);
				bOverallSuccess &= TestEqual(TEXT("failed-build closeout should begin with inspection"), FirstStep, FString(TEXT("inspect_reflected_property_declaration_build_failure")));
				bOverallSuccess &= TestEqual(TEXT("failed-build closeout should keep revert as next step when available"), SecondStep, FString(TEXT("revert_reflected_property_declaration")));
			}
		}
	}
	else
	{
		bOverallSuccess = false;
	}

	const FMCPToolResult LineContextInspectResult = RunInspect(LineContextLogPath);
	bOverallSuccess &= TestTrue(TEXT("line-context failed-build inspection should succeed"), LineContextInspectResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("line-context failed-build inspection should return data"), LineContextInspectResult.Data.IsValid());
	if (LineContextInspectResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> ArtifactObject = GetObjectFieldOrNull(LineContextInspectResult.Data, TEXT("build_diagnostic_artifact"));
		bOverallSuccess &= TestTrue(TEXT("line-context failed-build artifact object should exist"), ArtifactObject.IsValid());
		if (ArtifactObject.IsValid())
		{
			int32 HeaderPathMatchCount = 0;
			int32 LineContextMatchCount = 0;
			FString LinkageConfidence;
			ArtifactObject->TryGetNumberField(TEXT("header_path_match_count"), HeaderPathMatchCount);
			ArtifactObject->TryGetNumberField(TEXT("line_context_match_count"), LineContextMatchCount);
			ArtifactObject->TryGetStringField(TEXT("linkage_confidence"), LinkageConfidence);
			bOverallSuccess &= TestTrue(TEXT("line-context artifact should match header path"), HeaderPathMatchCount > 0);
			bOverallSuccess &= TestTrue(TEXT("line-context artifact should match receipt line window"), LineContextMatchCount > 0);
			bOverallSuccess &= TestEqual(TEXT("line-context artifact should earn the line-context tier"), LinkageConfidence, FString(TEXT("header_plus_line_context")));
		}
	}
	else
	{
		bOverallSuccess = false;
	}

	const FMCPToolResult HeaderOnlyInspectResult = RunInspect(HeaderOnlyLogPath);
	bOverallSuccess &= TestTrue(TEXT("header-only failed-build inspection should succeed"), HeaderOnlyInspectResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("header-only failed-build inspection should return data"), HeaderOnlyInspectResult.Data.IsValid());
	if (HeaderOnlyInspectResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> ArtifactObject = GetObjectFieldOrNull(HeaderOnlyInspectResult.Data, TEXT("build_diagnostic_artifact"));
		bOverallSuccess &= TestTrue(TEXT("header-only failed-build artifact object should exist"), ArtifactObject.IsValid());
		if (ArtifactObject.IsValid())
		{
			int32 HeaderPathMatchCount = 0;
			int32 LineContextMatchCount = 0;
			int32 MemberNameMatchCount = 0;
			FString LinkageConfidence;
			ArtifactObject->TryGetNumberField(TEXT("header_path_match_count"), HeaderPathMatchCount);
			ArtifactObject->TryGetNumberField(TEXT("line_context_match_count"), LineContextMatchCount);
			ArtifactObject->TryGetNumberField(TEXT("member_name_match_count"), MemberNameMatchCount);
			ArtifactObject->TryGetStringField(TEXT("linkage_confidence"), LinkageConfidence);
			bOverallSuccess &= TestTrue(TEXT("header-only artifact should still match header path"), HeaderPathMatchCount > 0);
			bOverallSuccess &= TestEqual(TEXT("header-only artifact should not claim line-context linkage"), LineContextMatchCount, 0);
			bOverallSuccess &= TestEqual(TEXT("header-only artifact should not claim member-name linkage"), MemberNameMatchCount, 0);
			bOverallSuccess &= TestEqual(TEXT("header-only artifact should downgrade honestly"), LinkageConfidence, FString(TEXT("header_match_only")));
		}
	}
	else
	{
		bOverallSuccess = false;
	}

	TSharedRef<FJsonObject> RevertParams = MakeShared<FJsonObject>();
	RevertParams->SetStringField(TEXT("operation"), TEXT("revert_reflected_property_declaration"));
	RevertParams->SetStringField(TEXT("receipt_path"), ApplyReceiptPath);

	const FMCPToolResult RevertResult = Tool.Execute(RevertParams);
	bOverallSuccess &= TestTrue(TEXT("failed-build declaration revert should succeed"), RevertResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("failed-build declaration revert should return data"), RevertResult.Data.IsValid());

	FString HeaderAfterRevert;
	bOverallSuccess &= TestTrue(TEXT("should read header after failed-build revert"), FFileHelper::LoadFileToString(HeaderAfterRevert, *HeaderPath));
	if (!HeaderAfterRevert.IsEmpty())
	{
		bOverallSuccess &= TestEqual(TEXT("failed-build revert should restore original header content"), HeaderAfterRevert, OriginalHeaderContent);
	}

	if (HeaderAfterRevert != OriginalHeaderContent)
	{
		const bool bRestored = RestoreOriginalHeader();
		TestTrue(TEXT("failed-build cleanup should restore original header"), bRestored);
		bOverallSuccess &= bRestored;
	}

	return bOverallSuccess;
}

bool FCppReflection_BuildReflectedPropertyDeclarationEvidenceBundleSuccess::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;
	const FString PropertyName = TEXT("bP13Slice1BundleSuccessFlag");

	TSharedRef<FJsonObject> PreviewParams = MakeShared<FJsonObject>();
	PreviewParams->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
	PreviewParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	PreviewParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	PreviewParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	PreviewParams->SetStringField(TEXT("new_member_name"), PropertyName);
	PreviewParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	PreviewParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	PreviewParams->SetStringField(TEXT("default_value"), TEXT("false"));

	const FMCPToolResult PreviewResult = Tool.Execute(PreviewParams);
	TestTrue(TEXT("success bundle preview should succeed"), PreviewResult.bSuccess);
	TestTrue(TEXT("success bundle preview should return data"), PreviewResult.Data.IsValid());
	if (!PreviewResult.bSuccess || !PreviewResult.Data.IsValid())
	{
		return false;
	}

	FString PreviewSchemaVersion;
	PreviewResult.Data->TryGetStringField(TEXT("preview_schema_version"), PreviewSchemaVersion);
	const TSharedPtr<FJsonObject> PreviewObject = GetObjectFieldOrNull(PreviewResult.Data, TEXT("preview"));
	TestTrue(TEXT("success bundle preview object should exist"), PreviewObject.IsValid());
	if (!PreviewObject.IsValid())
	{
		return false;
	}

	FString HeaderPath;
	FString ExpectedSourceHashBefore;
	PreviewObject->TryGetStringField(TEXT("header_path"), HeaderPath);
	PreviewObject->TryGetStringField(TEXT("source_hash_before"), ExpectedSourceHashBefore);
	TestTrue(TEXT("success bundle preview should expose header path"), !HeaderPath.IsEmpty());
	TestTrue(TEXT("success bundle preview should expose source hash before"), !ExpectedSourceHashBefore.IsEmpty());
	if (HeaderPath.IsEmpty() || ExpectedSourceHashBefore.IsEmpty())
	{
		return false;
	}

	FString OriginalHeaderContent;
	TestTrue(TEXT("success bundle test should read original header"), FFileHelper::LoadFileToString(OriginalHeaderContent, *HeaderPath));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	bool bOverallSuccess = true;
	FString ApplyReceiptPath;
	const auto RestoreOriginalHeader = [&]() -> bool
	{
		return SaveUtf8WithoutBom(HeaderPath, OriginalHeaderContent);
	};

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("operation"), TEXT("apply_reflected_property_declaration"));
	ApplyParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ApplyParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ApplyParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	ApplyParams->SetStringField(TEXT("new_member_name"), PropertyName);
	ApplyParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	ApplyParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	ApplyParams->SetStringField(TEXT("default_value"), TEXT("false"));
	ApplyParams->SetStringField(TEXT("expected_preview_schema_version"), PreviewSchemaVersion);
	ApplyParams->SetStringField(TEXT("expected_source_hash_before"), ExpectedSourceHashBefore);

	const FMCPToolResult ApplyResult = Tool.Execute(ApplyParams);
	bOverallSuccess &= TestTrue(TEXT("success bundle apply should succeed"), ApplyResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("success bundle apply should return data"), ApplyResult.Data.IsValid());
	if (!ApplyResult.bSuccess || !ApplyResult.Data.IsValid())
	{
		TestTrue(TEXT("success bundle cleanup should restore original header after failed apply"), RestoreOriginalHeader());
		return false;
	}

	ApplyResult.Data->TryGetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	bOverallSuccess &= TestTrue(TEXT("success bundle apply should emit receipt path"), !ApplyReceiptPath.IsEmpty() && FPaths::FileExists(ApplyReceiptPath));
	if (ApplyReceiptPath.IsEmpty())
	{
		TestTrue(TEXT("success bundle cleanup should restore original header when receipt is missing"), RestoreOriginalHeader());
		return false;
	}

	TSharedRef<FJsonObject> ReadbackParams = MakeShared<FJsonObject>();
	ReadbackParams->SetStringField(TEXT("operation"), TEXT("get_reflected_contract"));
	ReadbackParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ReadbackParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ReadbackParams->SetBoolField(TEXT("include_properties"), true);
	ReadbackParams->SetBoolField(TEXT("include_functions"), false);
	ReadbackParams->SetNumberField(TEXT("member_limit"), 128);

	const FMCPToolResult ReadbackResult = Tool.Execute(ReadbackParams);
	bOverallSuccess &= TestTrue(TEXT("success bundle readback baseline should succeed"), ReadbackResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("success bundle readback baseline should return data"), ReadbackResult.Data.IsValid());
	if (!ReadbackResult.bSuccess || !ReadbackResult.Data.IsValid())
	{
		TestTrue(TEXT("success bundle cleanup should restore original header after readback failure"), RestoreOriginalHeader());
		return false;
	}

	const TSharedPtr<FJsonObject> ReadbackContract = GetObjectFieldOrNull(ReadbackResult.Data, TEXT("contract"));
	bOverallSuccess &= TestTrue(TEXT("success bundle readback contract should exist"), ReadbackContract.IsValid());
	if (!ReadbackContract.IsValid())
	{
		TestTrue(TEXT("success bundle cleanup should restore original header when contract is missing"), RestoreOriginalHeader());
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ExistingProperties = nullptr;
	TArray<TSharedPtr<FJsonValue>> SyntheticProperties;
	bOverallSuccess &= TestTrue(TEXT("success bundle readback should expose properties"), ReadbackContract->TryGetArrayField(TEXT("properties"), ExistingProperties) && ExistingProperties);
	if (ExistingProperties)
	{
		SyntheticProperties = *ExistingProperties;
	}

	TSharedPtr<FJsonObject> SyntheticProperty = MakeShared<FJsonObject>();
	SyntheticProperty->SetStringField(TEXT("name"), PropertyName);
	SyntheticProperty->SetStringField(TEXT("cpp_type"), TEXT("bool"));
	SyntheticProperty->SetStringField(TEXT("declared_in"), TEXT("UOsvayderUESettings"));
	SyntheticProperty->SetStringField(TEXT("owner_kind"), TEXT("class"));
	SyntheticProperties.Add(MakeShared<FJsonValueObject>(SyntheticProperty));
	ReadbackContract->SetArrayField(TEXT("properties"), SyntheticProperties);

	const TSharedPtr<FJsonObject> MemberCounts = GetObjectFieldOrNull(ReadbackContract, TEXT("member_counts"));
	if (MemberCounts.IsValid())
	{
		MemberCounts->SetNumberField(TEXT("declared_properties"), SyntheticProperties.Num());
	}

	const FString AutomationDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("Automation"));
	IFileManager::Get().MakeDirectory(*AutomationDirectory, true);
	const FString PreviewResultPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_evidence_bundle_success_preview.json"));
	const FString ReadbackResultPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_evidence_bundle_success_readback.json"));
	bOverallSuccess &= TestTrue(TEXT("success bundle should persist preview result"), SaveJsonObjectWithoutBom(PreviewResultPath, PreviewResult.Data));
	bOverallSuccess &= TestTrue(TEXT("success bundle should persist synthetic readback result"), SaveJsonObjectWithoutBom(ReadbackResultPath, ReadbackResult.Data));

	TSharedRef<FJsonObject> BundleParams = MakeShared<FJsonObject>();
	BundleParams->SetStringField(TEXT("operation"), TEXT("build_reflected_property_declaration_evidence_bundle"));
	BundleParams->SetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	BundleParams->SetStringField(TEXT("preview_result_path"), PreviewResultPath);
	BundleParams->SetStringField(TEXT("reflection_readback_path"), ReadbackResultPath);
	BundleParams->SetBoolField(TEXT("export_report"), true);
	BundleParams->SetStringField(TEXT("report_name"), TEXT("P1.3 Slice 2 Success Bundle Automation"));
	BundleParams->SetStringField(TEXT("report_slug"), TEXT("p13_slice2_success_bundle_automation"));

	const FMCPToolResult BundleResult = Tool.Execute(BundleParams);
	bOverallSuccess &= TestTrue(TEXT("success bundle operation should succeed"), BundleResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("success bundle operation should return data"), BundleResult.Data.IsValid());
	if (BundleResult.Data.IsValid())
	{
		FString BundleSchemaVersion;
		FString BundlePath;
		BundleResult.Data->TryGetStringField(TEXT("bundle_schema_version"), BundleSchemaVersion);
		BundleResult.Data->TryGetStringField(TEXT("evidence_bundle_path"), BundlePath);
		bOverallSuccess &= TestEqual(TEXT("success bundle schema version should stay stable"), BundleSchemaVersion, FString(TEXT("reflected_property_declaration_evidence_bundle_v1")));
		bOverallSuccess &= TestTrue(TEXT("success bundle should emit a persisted artifact"), !BundlePath.IsEmpty() && FPaths::FileExists(BundlePath));

		const TArray<TSharedPtr<FJsonValue>>* StateLabels = nullptr;
		bOverallSuccess &= TestTrue(TEXT("success bundle should expose state labels"), BundleResult.Data->TryGetArrayField(TEXT("bundle_state_labels"), StateLabels) && StateLabels);
		if (StateLabels)
		{
			bool bSawSuccessLabel = false;
			for (const TSharedPtr<FJsonValue>& LabelValue : *StateLabels)
			{
				FString Label;
				if (LabelValue.IsValid() && LabelValue->TryGetString(Label) && Label == TEXT("success_path_authoring_cycle"))
				{
					bSawSuccessLabel = true;
				}
			}
			bOverallSuccess &= TestTrue(TEXT("success bundle should classify the success cycle"), bSawSuccessLabel);
		}

		const TSharedPtr<FJsonObject> ReflectionReadbackObject = GetObjectFieldOrNull(BundleResult.Data, TEXT("reflection_readback"));
		bOverallSuccess &= TestTrue(TEXT("success bundle should include reflection_readback"), ReflectionReadbackObject.IsValid());
		if (ReflectionReadbackObject.IsValid())
		{
			bool bPropertyPresent = false;
			FString ReadbackClassification;
			ReflectionReadbackObject->TryGetBoolField(TEXT("property_present"), bPropertyPresent);
			ReflectionReadbackObject->TryGetStringField(TEXT("readback_classification"), ReadbackClassification);
			bOverallSuccess &= TestTrue(TEXT("success bundle should mark the property as present"), bPropertyPresent);
			bOverallSuccess &= TestEqual(TEXT("success bundle should classify post-build presence"), ReadbackClassification, FString(TEXT("post_build_property_present")));
		}

		const TSharedPtr<FJsonObject> ReportArtifactObject = GetObjectFieldOrNull(BundleResult.Data, TEXT("report_artifact"));
		bOverallSuccess &= TestTrue(TEXT("success bundle should attach a report artifact"), ReportArtifactObject.IsValid());
		if (ReportArtifactObject.IsValid())
		{
			FString ReportId;
			ReportArtifactObject->TryGetStringField(TEXT("report_id"), ReportId);
			bOverallSuccess &= TestTrue(TEXT("success bundle report_id should not be empty"), !ReportId.IsEmpty());

			FString MarkdownPath;
			FString SummaryPath;
			ReportArtifactObject->TryGetStringField(TEXT("markdown_path"), MarkdownPath);
			ReportArtifactObject->TryGetStringField(TEXT("summary_path"), SummaryPath);
			bOverallSuccess &= TestTrue(TEXT("success bundle markdown export should exist"), !MarkdownPath.IsEmpty() && FPaths::FileExists(MarkdownPath));
			bOverallSuccess &= TestTrue(TEXT("success bundle summary export should exist"), !SummaryPath.IsEmpty() && FPaths::FileExists(SummaryPath));

			const TSharedPtr<FJsonObject> ReportObject = ReadSingleReportObject(ReportId);
			bOverallSuccess &= TestTrue(TEXT("success bundle status readback should return one report"), ReportObject.IsValid());
			if (ReportObject.IsValid())
			{
				FString RunKind;
				ReportObject->TryGetStringField(TEXT("run_kind"), RunKind);
				bOverallSuccess &= TestEqual(TEXT("success bundle report run_kind should stay bounded"), RunKind, FString(TEXT("cpp_reflected_declaration_evidence_bundle")));

				FString MarkdownPreview;
				ReportObject->TryGetStringField(TEXT("markdown_preview"), MarkdownPreview);
				bOverallSuccess &= TestTrue(TEXT("success bundle preview should keep truth boundary wording"), MarkdownPreview.Contains(TEXT("does not widen declaration families")));

				const TSharedPtr<FJsonObject> ExtraMetadata = GetObjectFieldOrNull(ReportObject, TEXT("extra_metadata"));
				bOverallSuccess &= TestTrue(TEXT("success bundle report should expose extra_metadata"), ExtraMetadata.IsValid());
				if (ExtraMetadata.IsValid())
				{
					FString PrimaryCycleState;
					FString ReadbackClassification;
					ExtraMetadata->TryGetStringField(TEXT("primary_cycle_state"), PrimaryCycleState);
					ExtraMetadata->TryGetStringField(TEXT("readback_classification"), ReadbackClassification);
					bOverallSuccess &= TestEqual(TEXT("success bundle report should preserve success state"), PrimaryCycleState, FString(TEXT("success_path_authoring_cycle")));
					bOverallSuccess &= TestEqual(TEXT("success bundle report should preserve post-build readback classification"), ReadbackClassification, FString(TEXT("post_build_property_present")));
				}
			}
		}
	}
	else
	{
		bOverallSuccess = false;
	}

	TSharedRef<FJsonObject> RevertParams = MakeShared<FJsonObject>();
	RevertParams->SetStringField(TEXT("operation"), TEXT("revert_reflected_property_declaration"));
	RevertParams->SetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	const FMCPToolResult RevertResult = Tool.Execute(RevertParams);
	bOverallSuccess &= TestTrue(TEXT("success bundle cleanup revert should succeed"), RevertResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("success bundle cleanup revert should return data"), RevertResult.Data.IsValid());

	FString HeaderAfterRevert;
	bOverallSuccess &= TestTrue(TEXT("success bundle test should read header after revert"), FFileHelper::LoadFileToString(HeaderAfterRevert, *HeaderPath));
	if (!HeaderAfterRevert.IsEmpty())
	{
		bOverallSuccess &= TestEqual(TEXT("success bundle revert should restore original header"), HeaderAfterRevert, OriginalHeaderContent);
	}

	if (HeaderAfterRevert != OriginalHeaderContent)
	{
		const bool bRestored = RestoreOriginalHeader();
		TestTrue(TEXT("success bundle cleanup should restore original header"), bRestored);
		bOverallSuccess &= bRestored;
	}

	return bOverallSuccess;
}

bool FCppReflection_BuildReflectedPropertyDeclarationEvidenceBundleFailedBuild::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;
	const FString PropertyName = TEXT("bitor");

	TSharedRef<FJsonObject> PreviewParams = MakeShared<FJsonObject>();
	PreviewParams->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
	PreviewParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	PreviewParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	PreviewParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	PreviewParams->SetStringField(TEXT("new_member_name"), PropertyName);
	PreviewParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	PreviewParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	PreviewParams->SetStringField(TEXT("default_value"), TEXT("false"));

	const FMCPToolResult PreviewResult = Tool.Execute(PreviewParams);
	TestTrue(TEXT("failed-build bundle preview should succeed"), PreviewResult.bSuccess);
	TestTrue(TEXT("failed-build bundle preview should return data"), PreviewResult.Data.IsValid());
	if (!PreviewResult.bSuccess || !PreviewResult.Data.IsValid())
	{
		return false;
	}

	FString PreviewSchemaVersion;
	PreviewResult.Data->TryGetStringField(TEXT("preview_schema_version"), PreviewSchemaVersion);
	const TSharedPtr<FJsonObject> PreviewObject = GetObjectFieldOrNull(PreviewResult.Data, TEXT("preview"));
	TestTrue(TEXT("failed-build bundle preview object should exist"), PreviewObject.IsValid());
	if (!PreviewObject.IsValid())
	{
		return false;
	}

	FString HeaderPath;
	FString ExpectedSourceHashBefore;
	PreviewObject->TryGetStringField(TEXT("header_path"), HeaderPath);
	PreviewObject->TryGetStringField(TEXT("source_hash_before"), ExpectedSourceHashBefore);
	TestTrue(TEXT("failed-build bundle preview should expose header path"), !HeaderPath.IsEmpty());
	TestTrue(TEXT("failed-build bundle preview should expose source hash before"), !ExpectedSourceHashBefore.IsEmpty());
	if (HeaderPath.IsEmpty() || ExpectedSourceHashBefore.IsEmpty())
	{
		return false;
	}

	FString OriginalHeaderContent;
	TestTrue(TEXT("failed-build bundle test should read original header"), FFileHelper::LoadFileToString(OriginalHeaderContent, *HeaderPath));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	bool bOverallSuccess = true;
	FString ApplyReceiptPath;
	const auto RestoreOriginalHeader = [&]() -> bool
	{
		return SaveUtf8WithoutBom(HeaderPath, OriginalHeaderContent);
	};

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("operation"), TEXT("apply_reflected_property_declaration"));
	ApplyParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ApplyParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ApplyParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	ApplyParams->SetStringField(TEXT("new_member_name"), PropertyName);
	ApplyParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	ApplyParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	ApplyParams->SetStringField(TEXT("default_value"), TEXT("false"));
	ApplyParams->SetStringField(TEXT("expected_preview_schema_version"), PreviewSchemaVersion);
	ApplyParams->SetStringField(TEXT("expected_source_hash_before"), ExpectedSourceHashBefore);

	const FMCPToolResult ApplyResult = Tool.Execute(ApplyParams);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle apply should succeed"), ApplyResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle apply should return data"), ApplyResult.Data.IsValid());
	if (!ApplyResult.bSuccess || !ApplyResult.Data.IsValid())
	{
		TestTrue(TEXT("failed-build bundle cleanup should restore original header after failed apply"), RestoreOriginalHeader());
		return false;
	}

	ApplyResult.Data->TryGetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle apply should emit receipt path"), !ApplyReceiptPath.IsEmpty() && FPaths::FileExists(ApplyReceiptPath));
	if (ApplyReceiptPath.IsEmpty())
	{
		TestTrue(TEXT("failed-build bundle cleanup should restore original header when receipt is missing"), RestoreOriginalHeader());
		return false;
	}

	const TSharedPtr<FJsonObject> ApplyObject = GetObjectFieldOrNull(ApplyResult.Data, TEXT("apply"));
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle apply object should exist"), ApplyObject.IsValid());
	if (!ApplyObject.IsValid())
	{
		TestTrue(TEXT("failed-build bundle cleanup should restore original header when apply object is missing"), RestoreOriginalHeader());
		return false;
	}

	FString GeneratedSnippet;
	double ReceiptInsertionLineNumber = 0.0;
	int32 ReceiptInsertionLine = INDEX_NONE;
	ApplyObject->TryGetStringField(TEXT("generated_snippet"), GeneratedSnippet);
	if (ApplyObject->TryGetNumberField(TEXT("insertion_line"), ReceiptInsertionLineNumber))
	{
		ReceiptInsertionLine = static_cast<int32>(ReceiptInsertionLineNumber);
	}
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle apply should expose generated snippet"), !GeneratedSnippet.IsEmpty());
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle apply should expose insertion line"), ReceiptInsertionLine != INDEX_NONE);
	if (GeneratedSnippet.IsEmpty() || ReceiptInsertionLine == INDEX_NONE)
	{
		TestTrue(TEXT("failed-build bundle cleanup should restore original header when receipt context is missing"), RestoreOriginalHeader());
		return false;
	}

	TArray<FString> GeneratedSnippetLines;
	GeneratedSnippet.ParseIntoArrayLines(GeneratedSnippetLines, true);
	const int32 GeneratedSnippetLineCount = GeneratedSnippetLines.Num() > 0 ? GeneratedSnippetLines.Num() : 1;
	const int32 MemberLineNumber = ReceiptInsertionLine + GeneratedSnippetLineCount - 1;

	const FString AutomationDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("Automation"));
	IFileManager::Get().MakeDirectory(*AutomationDirectory, true);
	const FString PreviewResultPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_evidence_bundle_failed_preview.json"));
	const FString FailedBuildLogPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_evidence_bundle_failed_fixture.log"));
	const FString FailedBuildResultPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_evidence_bundle_failed_result.json"));
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle should persist preview result"), SaveJsonObjectWithoutBom(PreviewResultPath, PreviewResult.Data));

	const FString FailedBuildLogContent = FString::Printf(
		TEXT("Using Visual Studio 2022 14.39.33523 toolchain.\n")
		TEXT("%s(%d,7): error C2059: syntax error: |\n")
		TEXT("%s(%d,20): error C2238: unexpected token(s) preceding ';'\n")
		TEXT("Result: Failed (OtherCompilationError)\n"),
		*HeaderPath,
		MemberLineNumber,
		*HeaderPath,
		MemberLineNumber);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle should write synthetic log"), SaveUtf8WithoutBom(FailedBuildLogPath, FailedBuildLogContent));

	TSharedRef<FJsonObject> InspectParams = MakeShared<FJsonObject>();
	InspectParams->SetStringField(TEXT("operation"), TEXT("inspect_reflected_property_declaration_build_failure"));
	InspectParams->SetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	InspectParams->SetStringField(TEXT("build_log_path"), FailedBuildLogPath);
	const FMCPToolResult InspectResult = Tool.Execute(InspectParams);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle inspection should succeed"), InspectResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle inspection should return data"), InspectResult.Data.IsValid());
	if (!InspectResult.bSuccess || !InspectResult.Data.IsValid())
	{
		TestTrue(TEXT("failed-build bundle cleanup should restore original header after inspection failure"), RestoreOriginalHeader());
		return false;
	}

	bOverallSuccess &= TestTrue(TEXT("failed-build bundle should persist inspection result"), SaveJsonObjectWithoutBom(FailedBuildResultPath, InspectResult.Data));

	TSharedRef<FJsonObject> BundleParams = MakeShared<FJsonObject>();
	BundleParams->SetStringField(TEXT("operation"), TEXT("build_reflected_property_declaration_evidence_bundle"));
	BundleParams->SetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	BundleParams->SetStringField(TEXT("preview_result_path"), PreviewResultPath);
	BundleParams->SetStringField(TEXT("failed_build_result_path"), FailedBuildResultPath);
	BundleParams->SetBoolField(TEXT("export_report"), true);
	BundleParams->SetStringField(TEXT("report_name"), TEXT("P1.3 Slice 2 Failed Bundle Automation"));
	BundleParams->SetStringField(TEXT("report_slug"), TEXT("p13_slice2_failed_bundle_automation"));

	const FMCPToolResult BundleResult = Tool.Execute(BundleParams);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle operation should succeed"), BundleResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle operation should return data"), BundleResult.Data.IsValid());
	if (BundleResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* StateLabels = nullptr;
		bOverallSuccess &= TestTrue(TEXT("failed-build bundle should expose state labels"), BundleResult.Data->TryGetArrayField(TEXT("bundle_state_labels"), StateLabels) && StateLabels);
		if (StateLabels)
		{
			bool bSawFailedBuildLabel = false;
			for (const TSharedPtr<FJsonValue>& LabelValue : *StateLabels)
			{
				FString Label;
				if (LabelValue.IsValid() && LabelValue->TryGetString(Label) && Label == TEXT("failed_build_cycle"))
				{
					bSawFailedBuildLabel = true;
				}
			}
			bOverallSuccess &= TestTrue(TEXT("failed-build bundle should classify the failed-build cycle"), bSawFailedBuildLabel);
		}

		const TSharedPtr<FJsonObject> FailedBuildBundle = GetObjectFieldOrNull(BundleResult.Data, TEXT("failed_build_diagnostic"));
		bOverallSuccess &= TestTrue(TEXT("failed-build bundle should include failed_build_diagnostic"), FailedBuildBundle.IsValid());
		if (FailedBuildBundle.IsValid())
		{
			FString LinkageConfidence;
			FailedBuildBundle->TryGetStringField(TEXT("diagnostic_linkage_confidence"), LinkageConfidence);
			bOverallSuccess &= TestEqual(TEXT("failed-build bundle should keep line-context linkage confidence"), LinkageConfidence, FString(TEXT("header_plus_line_context")));

			FString RecommendedCloseoutStatus;
			FailedBuildBundle->TryGetStringField(TEXT("recommended_closeout_status"), RecommendedCloseoutStatus);
			bOverallSuccess &= TestEqual(TEXT("failed-build bundle should keep the accepted closeout status"), RecommendedCloseoutStatus, FString(TEXT("revert_then_rebuild")));

			const TSharedPtr<FJsonObject> RecommendedCloseoutSequence = GetObjectFieldOrNull(FailedBuildBundle, TEXT("recommended_closeout_sequence"));
			bOverallSuccess &= TestTrue(TEXT("failed-build bundle should preserve the closeout sequence object"), RecommendedCloseoutSequence.IsValid());

			const TSharedPtr<FJsonObject> PresentationObject = GetObjectFieldOrNull(FailedBuildBundle, TEXT("presentation"));
			bOverallSuccess &= TestTrue(TEXT("failed-build bundle should include readable presentation"), PresentationObject.IsValid());
			if (PresentationObject.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* DisplaySamples = nullptr;
				bOverallSuccess &= TestTrue(TEXT("failed-build bundle should include readable diagnostic samples"), PresentationObject->TryGetArrayField(TEXT("matching_error_display_samples"), DisplaySamples) && DisplaySamples && DisplaySamples->Num() > 0);
			}
		}

		const TSharedPtr<FJsonObject> ReportArtifactObject = GetObjectFieldOrNull(BundleResult.Data, TEXT("report_artifact"));
		bOverallSuccess &= TestTrue(TEXT("failed-build bundle should attach a report artifact"), ReportArtifactObject.IsValid());
		if (ReportArtifactObject.IsValid())
		{
			FString ReportId;
			ReportArtifactObject->TryGetStringField(TEXT("report_id"), ReportId);
			bOverallSuccess &= TestTrue(TEXT("failed-build bundle report_id should not be empty"), !ReportId.IsEmpty());

			FString MarkdownPath;
			FString SummaryPath;
			ReportArtifactObject->TryGetStringField(TEXT("markdown_path"), MarkdownPath);
			ReportArtifactObject->TryGetStringField(TEXT("summary_path"), SummaryPath);
			bOverallSuccess &= TestTrue(TEXT("failed-build bundle markdown export should exist"), !MarkdownPath.IsEmpty() && FPaths::FileExists(MarkdownPath));
			bOverallSuccess &= TestTrue(TEXT("failed-build bundle summary export should exist"), !SummaryPath.IsEmpty() && FPaths::FileExists(SummaryPath));

			const TSharedPtr<FJsonObject> ReportObject = ReadSingleReportObject(ReportId, 2200);
			bOverallSuccess &= TestTrue(TEXT("failed-build bundle status readback should return one report"), ReportObject.IsValid());
			if (ReportObject.IsValid())
			{
				FString MarkdownPreview;
				ReportObject->TryGetStringField(TEXT("markdown_preview"), MarkdownPreview);
				const FString ExpectedErrorPreview = FString::Printf(
					TEXT("error C2059 at OsvayderUESettings.h(%d,7)"),
					MemberLineNumber);
				bOverallSuccess &= TestTrue(TEXT("failed-build bundle preview should preserve readable error display"), MarkdownPreview.Contains(ExpectedErrorPreview));
				bOverallSuccess &= TestTrue(TEXT("failed-build bundle preview should keep truth boundary wording"), MarkdownPreview.Contains(TEXT("does not widen declaration families")));

				const TSharedPtr<FJsonObject> ExtraMetadata = GetObjectFieldOrNull(ReportObject, TEXT("extra_metadata"));
				bOverallSuccess &= TestTrue(TEXT("failed-build bundle report should expose extra_metadata"), ExtraMetadata.IsValid());
				if (ExtraMetadata.IsValid())
				{
					FString PrimaryCycleState;
					FString DiagnosticLinkageConfidence;
					FString RecommendedCloseoutStatus;
					ExtraMetadata->TryGetStringField(TEXT("primary_cycle_state"), PrimaryCycleState);
					ExtraMetadata->TryGetStringField(TEXT("diagnostic_linkage_confidence"), DiagnosticLinkageConfidence);
					ExtraMetadata->TryGetStringField(TEXT("recommended_closeout_status"), RecommendedCloseoutStatus);
					bOverallSuccess &= TestEqual(TEXT("failed-build bundle report should preserve failed state"), PrimaryCycleState, FString(TEXT("failed_build_cycle")));
					bOverallSuccess &= TestEqual(TEXT("failed-build bundle report should preserve linkage confidence"), DiagnosticLinkageConfidence, FString(TEXT("header_plus_line_context")));
					bOverallSuccess &= TestEqual(TEXT("failed-build bundle report should preserve closeout status"), RecommendedCloseoutStatus, FString(TEXT("revert_then_rebuild")));
				}
			}
		}
	}
	else
	{
		bOverallSuccess = false;
	}

	TSharedRef<FJsonObject> RevertParams = MakeShared<FJsonObject>();
	RevertParams->SetStringField(TEXT("operation"), TEXT("revert_reflected_property_declaration"));
	RevertParams->SetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	const FMCPToolResult RevertResult = Tool.Execute(RevertParams);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle cleanup revert should succeed"), RevertResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle cleanup revert should return data"), RevertResult.Data.IsValid());

	FString HeaderAfterRevert;
	bOverallSuccess &= TestTrue(TEXT("failed-build bundle test should read header after revert"), FFileHelper::LoadFileToString(HeaderAfterRevert, *HeaderPath));
	if (!HeaderAfterRevert.IsEmpty())
	{
		bOverallSuccess &= TestEqual(TEXT("failed-build bundle revert should restore original header"), HeaderAfterRevert, OriginalHeaderContent);
	}

	if (HeaderAfterRevert != OriginalHeaderContent)
	{
		const bool bRestored = RestoreOriginalHeader();
		TestTrue(TEXT("failed-build bundle cleanup should restore original header"), bRestored);
		bOverallSuccess &= bRestored;
	}

	return bOverallSuccess;
}

bool FCppReflection_BuildReflectedPropertyDeclarationEvidenceBundleRevertCloseout::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;
	const FString PropertyName = TEXT("bP13Slice1BundleRevertFlag");

	TSharedRef<FJsonObject> PreviewParams = MakeShared<FJsonObject>();
	PreviewParams->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
	PreviewParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	PreviewParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	PreviewParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	PreviewParams->SetStringField(TEXT("new_member_name"), PropertyName);
	PreviewParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	PreviewParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	PreviewParams->SetStringField(TEXT("default_value"), TEXT("false"));

	const FMCPToolResult PreviewResult = Tool.Execute(PreviewParams);
	TestTrue(TEXT("revert bundle preview should succeed"), PreviewResult.bSuccess);
	TestTrue(TEXT("revert bundle preview should return data"), PreviewResult.Data.IsValid());
	if (!PreviewResult.bSuccess || !PreviewResult.Data.IsValid())
	{
		return false;
	}

	FString PreviewSchemaVersion;
	PreviewResult.Data->TryGetStringField(TEXT("preview_schema_version"), PreviewSchemaVersion);
	const TSharedPtr<FJsonObject> PreviewObject = GetObjectFieldOrNull(PreviewResult.Data, TEXT("preview"));
	TestTrue(TEXT("revert bundle preview object should exist"), PreviewObject.IsValid());
	if (!PreviewObject.IsValid())
	{
		return false;
	}

	FString HeaderPath;
	FString ExpectedSourceHashBefore;
	PreviewObject->TryGetStringField(TEXT("header_path"), HeaderPath);
	PreviewObject->TryGetStringField(TEXT("source_hash_before"), ExpectedSourceHashBefore);
	TestTrue(TEXT("revert bundle preview should expose header path"), !HeaderPath.IsEmpty());
	TestTrue(TEXT("revert bundle preview should expose source hash before"), !ExpectedSourceHashBefore.IsEmpty());
	if (HeaderPath.IsEmpty() || ExpectedSourceHashBefore.IsEmpty())
	{
		return false;
	}

	FString OriginalHeaderContent;
	TestTrue(TEXT("revert bundle test should read original header"), FFileHelper::LoadFileToString(OriginalHeaderContent, *HeaderPath));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	bool bOverallSuccess = true;
	FString ApplyReceiptPath;
	const auto RestoreOriginalHeader = [&]() -> bool
	{
		return SaveUtf8WithoutBom(HeaderPath, OriginalHeaderContent);
	};

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("operation"), TEXT("apply_reflected_property_declaration"));
	ApplyParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ApplyParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ApplyParams->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	ApplyParams->SetStringField(TEXT("new_member_name"), PropertyName);
	ApplyParams->SetStringField(TEXT("property_cpp_type"), TEXT("bool"));
	ApplyParams->SetStringField(TEXT("category"), TEXT("Assistant"));
	ApplyParams->SetStringField(TEXT("default_value"), TEXT("false"));
	ApplyParams->SetStringField(TEXT("expected_preview_schema_version"), PreviewSchemaVersion);
	ApplyParams->SetStringField(TEXT("expected_source_hash_before"), ExpectedSourceHashBefore);

	const FMCPToolResult ApplyResult = Tool.Execute(ApplyParams);
	bOverallSuccess &= TestTrue(TEXT("revert bundle apply should succeed"), ApplyResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("revert bundle apply should return data"), ApplyResult.Data.IsValid());
	if (!ApplyResult.bSuccess || !ApplyResult.Data.IsValid())
	{
		TestTrue(TEXT("revert bundle cleanup should restore original header after failed apply"), RestoreOriginalHeader());
		return false;
	}

	ApplyResult.Data->TryGetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	bOverallSuccess &= TestTrue(TEXT("revert bundle apply should emit receipt path"), !ApplyReceiptPath.IsEmpty() && FPaths::FileExists(ApplyReceiptPath));
	if (ApplyReceiptPath.IsEmpty())
	{
		TestTrue(TEXT("revert bundle cleanup should restore original header when receipt is missing"), RestoreOriginalHeader());
		return false;
	}

	TSharedRef<FJsonObject> RevertParams = MakeShared<FJsonObject>();
	RevertParams->SetStringField(TEXT("operation"), TEXT("revert_reflected_property_declaration"));
	RevertParams->SetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	const FMCPToolResult RevertResult = Tool.Execute(RevertParams);
	bOverallSuccess &= TestTrue(TEXT("revert bundle revert should succeed"), RevertResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("revert bundle revert should return data"), RevertResult.Data.IsValid());
	if (!RevertResult.bSuccess || !RevertResult.Data.IsValid())
	{
		TestTrue(TEXT("revert bundle cleanup should restore original header after revert failure"), RestoreOriginalHeader());
		return false;
	}

	TSharedRef<FJsonObject> ReadbackParams = MakeShared<FJsonObject>();
	ReadbackParams->SetStringField(TEXT("operation"), TEXT("get_reflected_contract"));
	ReadbackParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ReadbackParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ReadbackParams->SetBoolField(TEXT("include_properties"), true);
	ReadbackParams->SetBoolField(TEXT("include_functions"), false);
	ReadbackParams->SetNumberField(TEXT("member_limit"), 128);
	const FMCPToolResult ReadbackResult = Tool.Execute(ReadbackParams);
	bOverallSuccess &= TestTrue(TEXT("revert bundle readback should succeed"), ReadbackResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("revert bundle readback should return data"), ReadbackResult.Data.IsValid());
	if (!ReadbackResult.bSuccess || !ReadbackResult.Data.IsValid())
	{
		TestTrue(TEXT("revert bundle cleanup should restore original header after readback failure"), RestoreOriginalHeader());
		return false;
	}

	const FString AutomationDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("Automation"));
	IFileManager::Get().MakeDirectory(*AutomationDirectory, true);
	const FString PreviewResultPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_evidence_bundle_revert_preview.json"));
	const FString RevertResultPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_evidence_bundle_revert_result.json"));
	const FString ReadbackResultPath = FPaths::Combine(AutomationDirectory, TEXT("cpp_reflection_evidence_bundle_revert_readback.json"));
	bOverallSuccess &= TestTrue(TEXT("revert bundle should persist preview result"), SaveJsonObjectWithoutBom(PreviewResultPath, PreviewResult.Data));
	bOverallSuccess &= TestTrue(TEXT("revert bundle should persist revert result"), SaveJsonObjectWithoutBom(RevertResultPath, RevertResult.Data));
	bOverallSuccess &= TestTrue(TEXT("revert bundle should persist readback result"), SaveJsonObjectWithoutBom(ReadbackResultPath, ReadbackResult.Data));

	TSharedRef<FJsonObject> BundleParams = MakeShared<FJsonObject>();
	BundleParams->SetStringField(TEXT("operation"), TEXT("build_reflected_property_declaration_evidence_bundle"));
	BundleParams->SetStringField(TEXT("receipt_path"), ApplyReceiptPath);
	BundleParams->SetStringField(TEXT("preview_result_path"), PreviewResultPath);
	BundleParams->SetStringField(TEXT("revert_result_path"), RevertResultPath);
	BundleParams->SetStringField(TEXT("reflection_readback_path"), ReadbackResultPath);
	BundleParams->SetBoolField(TEXT("export_report"), true);
	BundleParams->SetStringField(TEXT("report_name"), TEXT("P1.3 Slice 2 Revert Bundle Automation"));
	BundleParams->SetStringField(TEXT("report_slug"), TEXT("p13_slice2_revert_bundle_automation"));

	const FMCPToolResult BundleResult = Tool.Execute(BundleParams);
	bOverallSuccess &= TestTrue(TEXT("revert bundle operation should succeed"), BundleResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("revert bundle operation should return data"), BundleResult.Data.IsValid());
	if (BundleResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* StateLabels = nullptr;
		bOverallSuccess &= TestTrue(TEXT("revert bundle should expose state labels"), BundleResult.Data->TryGetArrayField(TEXT("bundle_state_labels"), StateLabels) && StateLabels);
		if (StateLabels)
		{
			bool bSawRevertLabel = false;
			for (const TSharedPtr<FJsonValue>& LabelValue : *StateLabels)
			{
				FString Label;
				if (LabelValue.IsValid() && LabelValue->TryGetString(Label) && Label == TEXT("revert_closeout_cycle"))
				{
					bSawRevertLabel = true;
				}
			}
			bOverallSuccess &= TestTrue(TEXT("revert bundle should classify the closeout cycle"), bSawRevertLabel);
		}

		const TSharedPtr<FJsonObject> ReflectionReadbackObject = GetObjectFieldOrNull(BundleResult.Data, TEXT("reflection_readback"));
		bOverallSuccess &= TestTrue(TEXT("revert bundle should include reflection_readback"), ReflectionReadbackObject.IsValid());
		if (ReflectionReadbackObject.IsValid())
		{
			bool bPropertyPresent = true;
			FString ReadbackClassification;
			ReflectionReadbackObject->TryGetBoolField(TEXT("property_present"), bPropertyPresent);
			ReflectionReadbackObject->TryGetStringField(TEXT("readback_classification"), ReadbackClassification);
			bOverallSuccess &= TestFalse(TEXT("revert bundle should show the property absent after closeout"), bPropertyPresent);
			bOverallSuccess &= TestEqual(TEXT("revert bundle should classify post-revert absence"), ReadbackClassification, FString(TEXT("post_revert_property_absent")));
		}

		const TSharedPtr<FJsonObject> CompileHandshakeObject = GetObjectFieldOrNull(BundleResult.Data, TEXT("compile_handshake"));
		bOverallSuccess &= TestTrue(TEXT("revert bundle should include compile_handshake"), CompileHandshakeObject.IsValid());
		if (CompileHandshakeObject.IsValid())
		{
			const TSharedPtr<FJsonObject> RevertHandshakeObject = GetObjectFieldOrNull(CompileHandshakeObject, TEXT("revert"));
			bOverallSuccess &= TestTrue(TEXT("revert bundle should include revert handshake"), RevertHandshakeObject.IsValid());
		}

		const TSharedPtr<FJsonObject> ReportArtifactObject = GetObjectFieldOrNull(BundleResult.Data, TEXT("report_artifact"));
		bOverallSuccess &= TestTrue(TEXT("revert bundle should attach a report artifact"), ReportArtifactObject.IsValid());
		if (ReportArtifactObject.IsValid())
		{
			FString ReportId;
			ReportArtifactObject->TryGetStringField(TEXT("report_id"), ReportId);
			bOverallSuccess &= TestTrue(TEXT("revert bundle report_id should not be empty"), !ReportId.IsEmpty());

			FString MarkdownPath;
			FString SummaryPath;
			ReportArtifactObject->TryGetStringField(TEXT("markdown_path"), MarkdownPath);
			ReportArtifactObject->TryGetStringField(TEXT("summary_path"), SummaryPath);
			bOverallSuccess &= TestTrue(TEXT("revert bundle markdown export should exist"), !MarkdownPath.IsEmpty() && FPaths::FileExists(MarkdownPath));
			bOverallSuccess &= TestTrue(TEXT("revert bundle summary export should exist"), !SummaryPath.IsEmpty() && FPaths::FileExists(SummaryPath));

			const TSharedPtr<FJsonObject> ReportObject = ReadSingleReportObject(ReportId);
			bOverallSuccess &= TestTrue(TEXT("revert bundle status readback should return one report"), ReportObject.IsValid());
			if (ReportObject.IsValid())
			{
				FString MarkdownPreview;
				ReportObject->TryGetStringField(TEXT("markdown_preview"), MarkdownPreview);
				bOverallSuccess &= TestTrue(TEXT("revert bundle preview should keep truth boundary wording"), MarkdownPreview.Contains(TEXT("does not widen declaration families")));

				const TSharedPtr<FJsonObject> ExtraMetadata = GetObjectFieldOrNull(ReportObject, TEXT("extra_metadata"));
				bOverallSuccess &= TestTrue(TEXT("revert bundle report should expose extra_metadata"), ExtraMetadata.IsValid());
				if (ExtraMetadata.IsValid())
				{
					FString PrimaryCycleState;
					FString ReadbackClassification;
					FString RevertHandshakeStatus;
					ExtraMetadata->TryGetStringField(TEXT("primary_cycle_state"), PrimaryCycleState);
					ExtraMetadata->TryGetStringField(TEXT("readback_classification"), ReadbackClassification);
					ExtraMetadata->TryGetStringField(TEXT("compile_handshake_revert_status"), RevertHandshakeStatus);
					bOverallSuccess &= TestEqual(TEXT("revert bundle report should preserve revert state"), PrimaryCycleState, FString(TEXT("revert_closeout_cycle")));
					bOverallSuccess &= TestEqual(TEXT("revert bundle report should preserve post-revert readback classification"), ReadbackClassification, FString(TEXT("post_revert_property_absent")));
					bOverallSuccess &= TestEqual(TEXT("revert bundle report should preserve revert handshake status"), RevertHandshakeStatus, FString(TEXT("rebuild_required")));
				}
			}
		}
	}
	else
	{
		bOverallSuccess = false;
	}

	FString HeaderAfterRevert;
	bOverallSuccess &= TestTrue(TEXT("revert bundle test should read header after revert"), FFileHelper::LoadFileToString(HeaderAfterRevert, *HeaderPath));
	if (!HeaderAfterRevert.IsEmpty())
	{
		bOverallSuccess &= TestEqual(TEXT("revert bundle should restore original header"), HeaderAfterRevert, OriginalHeaderContent);
	}

	if (HeaderAfterRevert != OriginalHeaderContent)
	{
		const bool bRestored = RestoreOriginalHeader();
		TestTrue(TEXT("revert bundle cleanup should restore original header"), bRestored);
		bOverallSuccess &= bRestored;
	}

	return bOverallSuccess;
}

bool FCppReflection_ApplyReflectedPropertyDeclarationBlockedShape::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("operation"), TEXT("apply_reflected_property_declaration"));
	ParamsObject->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ParamsObject->SetStringField(TEXT("anchor_member_name"), TEXT("DefaultCodexSpeedMode"));
	ParamsObject->SetStringField(TEXT("new_member_name"), TEXT("ApplyStringProperty"));
	ParamsObject->SetStringField(TEXT("property_cpp_type"), TEXT("FString"));
	ParamsObject->SetStringField(TEXT("category"), TEXT("Assistant"));
	ParamsObject->SetStringField(TEXT("default_value"), TEXT("false"));
	ParamsObject->SetStringField(TEXT("expected_preview_schema_version"), TEXT("reflected_property_declaration_preview_v1"));
	ParamsObject->SetStringField(TEXT("expected_source_hash_before"), TEXT("ignored_for_blocked_shape"));

	const FMCPToolResult Result = Tool.Execute(ParamsObject);
	TestFalse(TEXT("unsupported declaration apply shape should fail"), Result.bSuccess);
	TestTrue(TEXT("unsupported declaration apply shape should explain bool-only boundary"), Result.Message.Contains(TEXT("supports only bool")));
	return true;
}

bool FCppReflection_PreviewPropertyMetadataMutation::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("operation"), TEXT("preview_property_metadata_mutation"));
	ParamsObject->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ParamsObject->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ParamsObject->SetStringField(TEXT("member_name"), TEXT("DefaultCodexSpeedMode"));
	ParamsObject->SetStringField(TEXT("metadata_key"), TEXT("ToolTip"));
	ParamsObject->SetStringField(TEXT("metadata_value"), TEXT("CF1 slice 2 preview tooltip"));

	const FMCPToolResult Result = Tool.Execute(ParamsObject);
	TestTrue(TEXT("cpp_reflection preview should succeed"), Result.bSuccess);
	TestTrue(TEXT("cpp_reflection preview should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ContractObjectPtr = nullptr;
	TestTrue(TEXT("contract should exist"), Result.Data->TryGetObjectField(TEXT("contract"), ContractObjectPtr) && ContractObjectPtr && (*ContractObjectPtr).IsValid());

	const TSharedPtr<FJsonObject>* PreviewObjectPtr = nullptr;
	TestTrue(TEXT("preview object should exist"), Result.Data->TryGetObjectField(TEXT("preview"), PreviewObjectPtr) && PreviewObjectPtr && (*PreviewObjectPtr).IsValid());
	if (!PreviewObjectPtr || !(*PreviewObjectPtr).IsValid())
	{
		return false;
	}

	FString ChangeKind;
	FString MacroAfter;
	FString HeaderPath;
	(*PreviewObjectPtr)->TryGetStringField(TEXT("change_kind"), ChangeKind);
	(*PreviewObjectPtr)->TryGetStringField(TEXT("macro_after"), MacroAfter);
	(*PreviewObjectPtr)->TryGetStringField(TEXT("header_path"), HeaderPath);

	TestFalse(TEXT("change_kind should not be empty"), ChangeKind.IsEmpty());
	TestTrue(TEXT("preview macro should contain test tooltip"), MacroAfter.Contains(TEXT("CF1 slice 2 preview tooltip")));
	TestTrue(TEXT("preview header should point at OsvayderUESettings.h"), HeaderPath.EndsWith(TEXT("OsvayderUESettings.h")));

	const TSharedPtr<FJsonObject>* ExpectedMetadataPtr = nullptr;
	TestTrue(TEXT("expected metadata after rebuild should exist"), Result.Data->TryGetObjectField(TEXT("expected_metadata_after_rebuild"), ExpectedMetadataPtr) && ExpectedMetadataPtr && (*ExpectedMetadataPtr).IsValid());
	if (ExpectedMetadataPtr && (*ExpectedMetadataPtr).IsValid())
	{
		FString TooltipValue;
		(*ExpectedMetadataPtr)->TryGetStringField(TEXT("ToolTip"), TooltipValue);
		TestEqual(TEXT("expected metadata should reflect requested tooltip"), TooltipValue, FString(TEXT("CF1 slice 2 preview tooltip")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_ApplyPropertyMetadataMutation,
	"OsvayderUE.CppReflection.ApplyPropertyMetadataMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCppReflection_ApplyPropertyMetadataMutation::RunTest(const FString& Parameters)
{
	FMCPTool_CppReflection Tool;
	const FString TestTooltip = TEXT("CF1 slice 2 apply tooltip");

	TSharedRef<FJsonObject> PreviewParams = MakeShared<FJsonObject>();
	PreviewParams->SetStringField(TEXT("operation"), TEXT("preview_property_metadata_mutation"));
	PreviewParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	PreviewParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	PreviewParams->SetStringField(TEXT("member_name"), TEXT("DefaultCodexSpeedMode"));
	PreviewParams->SetStringField(TEXT("metadata_key"), TEXT("ToolTip"));
	PreviewParams->SetStringField(TEXT("metadata_value"), TestTooltip);

	const FMCPToolResult PreviewResult = Tool.Execute(PreviewParams);
	TestTrue(TEXT("preview before apply should succeed"), PreviewResult.bSuccess);
	TestTrue(TEXT("preview before apply should return data"), PreviewResult.Data.IsValid());
	if (!PreviewResult.bSuccess || !PreviewResult.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* PreviewObjectPtr = nullptr;
	TestTrue(TEXT("preview object should exist"), PreviewResult.Data->TryGetObjectField(TEXT("preview"), PreviewObjectPtr) && PreviewObjectPtr && (*PreviewObjectPtr).IsValid());
	if (!PreviewObjectPtr || !(*PreviewObjectPtr).IsValid())
	{
		return false;
	}

	FString HeaderPath;
	(*PreviewObjectPtr)->TryGetStringField(TEXT("header_path"), HeaderPath);
	TestTrue(TEXT("preview should expose a concrete header path"), !HeaderPath.IsEmpty());
	if (HeaderPath.IsEmpty())
	{
		return false;
	}

	FString OriginalHeaderContent;
	TestTrue(TEXT("should load original header content"), FFileHelper::LoadFileToString(OriginalHeaderContent, *HeaderPath));
	if (OriginalHeaderContent.IsEmpty())
	{
		return false;
	}

	bool bOverallSuccess = true;

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("operation"), TEXT("apply_property_metadata_mutation"));
	ApplyParams->SetStringField(TEXT("symbol"), TEXT("UOsvayderUESettings"));
	ApplyParams->SetStringField(TEXT("module_scope"), TEXT("plugin_only"));
	ApplyParams->SetStringField(TEXT("member_name"), TEXT("DefaultCodexSpeedMode"));
	ApplyParams->SetStringField(TEXT("metadata_key"), TEXT("ToolTip"));
	ApplyParams->SetStringField(TEXT("metadata_value"), TestTooltip);

	const FMCPToolResult ApplyResult = Tool.Execute(ApplyParams);
	bOverallSuccess &= TestTrue(TEXT("apply should succeed"), ApplyResult.bSuccess);
	bOverallSuccess &= TestTrue(TEXT("apply should return data"), ApplyResult.Data.IsValid());

	if (ApplyResult.Data.IsValid())
	{
		FString ReceiptPath;
		ApplyResult.Data->TryGetStringField(TEXT("receipt_path"), ReceiptPath);
		bOverallSuccess &= TestTrue(TEXT("apply should emit a receipt path"), !ReceiptPath.IsEmpty());
		if (!ReceiptPath.IsEmpty())
		{
			bOverallSuccess &= TestTrue(TEXT("apply receipt should exist on disk"), FPaths::FileExists(ReceiptPath));
		}

		const TSharedPtr<FJsonObject>* ApplyObjectPtr = nullptr;
		bOverallSuccess &= TestTrue(TEXT("apply object should exist"), ApplyResult.Data->TryGetObjectField(TEXT("apply"), ApplyObjectPtr) && ApplyObjectPtr && (*ApplyObjectPtr).IsValid());
		if (ApplyObjectPtr && (*ApplyObjectPtr).IsValid())
		{
			bool bApplied = false;
			FString ChangeKind;
			(*ApplyObjectPtr)->TryGetBoolField(TEXT("applied"), bApplied);
			(*ApplyObjectPtr)->TryGetStringField(TEXT("change_kind"), ChangeKind);
			bOverallSuccess &= TestTrue(TEXT("apply should mutate the header for the unique tooltip"), bApplied);
			bOverallSuccess &= TestTrue(TEXT("apply change kind should not be empty"), !ChangeKind.IsEmpty());
		}

		const TSharedPtr<FJsonObject>* HandshakePtr = nullptr;
		bOverallSuccess &= TestTrue(TEXT("compile handshake should exist"), ApplyResult.Data->TryGetObjectField(TEXT("compile_handshake"), HandshakePtr) && HandshakePtr && (*HandshakePtr).IsValid());
		if (HandshakePtr && (*HandshakePtr).IsValid())
		{
			FString RecommendedAction;
			bool bSupportsInProcessCompile = true;
			(*HandshakePtr)->TryGetStringField(TEXT("recommended_action"), RecommendedAction);
			(*HandshakePtr)->TryGetBoolField(TEXT("supports_in_process_reflected_compile"), bSupportsInProcessCompile);
			bOverallSuccess &= TestEqual(TEXT("apply handshake should require preflight launcher"), RecommendedAction, FString(TEXT("close_editor_and_run_preflight_launcher")));
			bOverallSuccess &= TestFalse(TEXT("apply handshake should not claim in-process reflected compile"), bSupportsInProcessCompile);
		}
	}

	FString MutatedHeaderContent;
	bOverallSuccess &= TestTrue(TEXT("should read header after apply"), FFileHelper::LoadFileToString(MutatedHeaderContent, *HeaderPath));
	bOverallSuccess &= TestTrue(TEXT("mutated header should contain the unique tooltip"), MutatedHeaderContent.Contains(TestTooltip));

	const bool bRestored = FFileHelper::SaveStringToFile(
		OriginalHeaderContent,
		*HeaderPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TestTrue(TEXT("header should restore after apply test"), bRestored);

	if (bRestored)
	{
		FString RestoredHeaderContent;
		bOverallSuccess &= TestTrue(TEXT("should read header after restore"), FFileHelper::LoadFileToString(RestoredHeaderContent, *HeaderPath));
		bOverallSuccess &= TestFalse(TEXT("restored header should not contain the unique tooltip"), RestoredHeaderContent.Contains(TestTooltip));
	}
	else
	{
		bOverallSuccess = false;
	}

	return bOverallSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflection_RegistryToolsRegistered,
	"OsvayderUE.CppReflection.Registry.ToolsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCppReflection_RegistryToolsRegistered::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	TestTrue(TEXT("cpp_reflection should be registered"), Registry.HasTool(TEXT("cpp_reflection")));
	return true;
}

#endif
