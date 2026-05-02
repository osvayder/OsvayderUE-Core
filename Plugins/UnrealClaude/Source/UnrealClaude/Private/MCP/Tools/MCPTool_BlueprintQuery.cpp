// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_BlueprintQuery.h"
#include "BlueprintUtils.h"
#include "BlueprintGraphEditor.h"
#include "MCP/MCPParamValidator.h"
#include "UnrealClaudeModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "Engine/DataTable.h"
#include "Engine/DataAsset.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "WidgetBlueprint.h"

FMCPToolResult FMCPTool_BlueprintQuery::Execute(const TSharedRef<FJsonObject>& Params)
{
	// Get operation type
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("list"))
	{
		return ExecuteList(Params);
	}
	else if (Operation == TEXT("inspect"))
	{
		return ExecuteInspect(Params);
	}
	else if (Operation == TEXT("get_graph"))
	{
		return ExecuteGetGraph(Params);
	}

	else if (Operation == TEXT("list_graphs"))
	{
		return ExecuteListGraphs(Params);
	}
	else if (Operation == TEXT("list_macros"))
	{
		return ExecuteListMacros(Params);
	}
	else if (Operation == TEXT("get_level_blueprint"))
	{
		return ExecuteGetLevelBlueprint(Params);
	}
	else if (Operation == TEXT("list_editor_utilities"))
	{
		return ExecuteListEditorUtilities(Params);
	}
	else if (Operation == TEXT("get_editor_utility_details"))
	{
		return ExecuteGetEditorUtilityDetails(Params);
	}
	else if (Operation == TEXT("get_class_defaults"))
	{
		return ExecuteGetClassDefaults(Params);
	}
	else if (Operation == TEXT("get_component_editable_properties"))
	{
		return ExecuteGetComponentEditableProperties(Params);
	}
	else if (Operation == TEXT("get_editable_properties"))
	{
		return ExecuteGetEditableProperties(Params);
	}
	else if (Operation == TEXT("get_data_asset_properties"))
	{
		return ExecuteGetDataAssetProperties(Params);
	}
	else if (Operation == TEXT("get_data_table_schema"))
	{
		return ExecuteGetDataTableSchema(Params);
	}
	else if (Operation == TEXT("get_data_table_rows"))
	{
		return ExecuteGetDataTableRows(Params);
	}
	else if (Operation == TEXT("get_widget_tree"))
	{
		return ExecuteGetWidgetTree(Params);
	}
	else if (Operation == TEXT("get_anim_blueprint_info"))
	{
		return ExecuteGetAnimBlueprintInfo(Params);
	}
	else if (Operation == TEXT("get_graph_nodes"))
	{
		return ExecuteGetGraphNodes(Params);
	}
	else if (Operation == TEXT("find_nodes"))
	{
		return ExecuteFindNodes(Params);
	}
	else if (Operation == TEXT("get_node_pins"))
	{
		return ExecuteGetNodePins(Params);
	}
	else if (Operation == TEXT("get_node"))
	{
		return ExecuteGetNode(Params);
	}
	else if (Operation == TEXT("get_node_connections"))
	{
		return ExecuteGetNodeConnections(Params);
	}
	else if (Operation == TEXT("can_connect_pins"))
	{
		return ExecuteCanConnectPins(Params);
	}
	else if (Operation == TEXT("list_interfaces"))
	{
		return ExecuteListInterfaces(Params);
	}
	else if (Operation == TEXT("list_dispatchers"))
	{
		return ExecuteListDispatchers(Params);
	}
	else if (Operation == TEXT("get_function_signature"))
	{
		return ExecuteGetFunctionSignature(Params);
	}
	else if (Operation == TEXT("list_components"))
	{
		return ExecuteListComponents(Params);
	}
	else if (Operation == TEXT("get_component_tree"))
	{
		return ExecuteGetComponentTree(Params);
	}
	else if (Operation == TEXT("get_component_details"))
	{
		return ExecuteGetComponentDetails(Params);
	}

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation: '%s'. Valid: list, inspect, get_graph, list_graphs, list_macros, get_level_blueprint, list_editor_utilities, get_editor_utility_details, get_class_defaults, get_editable_properties, get_component_editable_properties, get_data_asset_properties, get_data_table_schema, get_data_table_rows, get_widget_tree, get_anim_blueprint_info, get_graph_nodes, find_nodes, get_node_pins, get_node, get_node_connections, can_connect_pins, list_interfaces, list_dispatchers, get_function_signature, list_components, get_component_tree, get_component_details"), *Operation));
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteList(const TSharedRef<FJsonObject>& Params)
{
	// Extract filters
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString TypeFilter = ExtractOptionalString(Params, TEXT("type_filter"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);

	// Clamp limit
	Limit = FMath::Clamp(Limit, 1, 1000);

	// Validate path filter
	FString ValidationError;
	if (!PathFilter.IsEmpty() && !FMCPParamValidator::ValidateBlueprintPath(PathFilter, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Query AssetRegistry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Build filter
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
	}

	// Get assets
	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAssets(Filter, AssetDataList);

	// Process results
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 Count = 0;
	int32 TotalMatching = 0;

	for (const FAssetData& AssetData : AssetDataList)
	{
		// Get parent class name for filtering
		FString ParentClassName;
		FAssetDataTagMapSharedView::FFindTagResult ParentClassTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		if (ParentClassTag.IsSet())
		{
			ParentClassName = ParentClassTag.GetValue();
		}

		// Apply type filter
		if (!TypeFilter.IsEmpty())
		{
			if (!ParentClassName.Contains(TypeFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Apply name filter
		if (!NameFilter.IsEmpty())
		{
			if (!AssetData.AssetName.ToString().Contains(NameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TotalMatching++;

		// Check limit
		if (Count >= Limit)
		{
			continue;
		}

		// Get Blueprint type
		FString BlueprintType = TEXT("Normal");
		FAssetDataTagMapSharedView::FFindTagResult TypeTag = AssetData.TagsAndValues.FindTag(FName("BlueprintType"));
		if (TypeTag.IsSet())
		{
			BlueprintType = TypeTag.GetValue();
		}

		// Build result object
		TSharedPtr<FJsonObject> BPJson = MakeShared<FJsonObject>();
		BPJson->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		BPJson->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		BPJson->SetStringField(TEXT("blueprint_type"), BlueprintType);

		// Clean up parent class name (remove prefix)
		if (!ParentClassName.IsEmpty())
		{
			FString CleanParentName = ParentClassName;
			int32 LastDotIndex;
			if (CleanParentName.FindLastChar(TEXT('.'), LastDotIndex))
			{
				CleanParentName = CleanParentName.Mid(LastDotIndex + 1);
			}
			// Remove trailing '_C' from generated class names
			if (CleanParentName.EndsWith(TEXT("_C")))
			{
				CleanParentName = CleanParentName.LeftChop(2);
			}
			BPJson->SetStringField(TEXT("parent_class"), CleanParentName);
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(BPJson));
		Count++;
	}

	// Build response
	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetArrayField(TEXT("blueprints"), ResultsArray);
	ResponseData->SetNumberField(TEXT("count"), Count);
	ResponseData->SetNumberField(TEXT("total_matching"), TotalMatching);

	if (TotalMatching > Count)
	{
		ResponseData->SetBoolField(TEXT("truncated"), true);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d Blueprints (showing %d)"), TotalMatching, Count),
		ResponseData
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteInspect(const TSharedRef<FJsonObject>& Params)
{
	// Get Blueprint path
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	// Validate path
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintPath(BlueprintPath, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FMCPToolResult::Error(LoadError);
	}

	// Get options
	bool bIncludeVariables = ExtractOptionalBool(Params, TEXT("include_variables"), false);
	bool bIncludeFunctions = ExtractOptionalBool(Params, TEXT("include_functions"), false);
	bool bIncludeGraphs = ExtractOptionalBool(Params, TEXT("include_graphs"), false);

	// Serialize Blueprint info
	TSharedPtr<FJsonObject> BlueprintInfo = FBlueprintUtils::SerializeBlueprintInfo(
		Blueprint,
		bIncludeVariables,
		bIncludeFunctions,
		bIncludeGraphs
	);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Blueprint info for: %s"), *Blueprint->GetName()),
		BlueprintInfo
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetGraph(const TSharedRef<FJsonObject>& Params)
{
	// Get Blueprint path
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	// Validate path
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintPath(BlueprintPath, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FMCPToolResult::Error(LoadError);
	}

	// Get graph info
	TSharedPtr<FJsonObject> GraphInfo = FBlueprintUtils::GetGraphInfo(Blueprint);

	// Add Blueprint name for context
	GraphInfo->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	GraphInfo->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Graph info for: %s"), *Blueprint->GetName()),
		GraphInfo
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteListGraphs(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> GraphsArray;

	// Ubergraph pages (EventGraph, etc.)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
			GraphObj->SetStringField(TEXT("name"), Graph->GetName());
			GraphObj->SetStringField(TEXT("type"), TEXT("event_graph"));
			GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
		}
	}

	// Function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
			GraphObj->SetStringField(TEXT("name"), Graph->GetName());
			GraphObj->SetStringField(TEXT("type"), TEXT("function_graph"));
			GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
		}
	}

	// Macro graphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph)
		{
			TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
			GraphObj->SetStringField(TEXT("name"), Graph->GetName());
			GraphObj->SetStringField(TEXT("type"), TEXT("macro_graph"));
			GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
		}
	}

	ResponseData->SetArrayField(TEXT("graphs"), GraphsArray);
	ResponseData->SetNumberField(TEXT("count"), GraphsArray.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d graphs in %s"), GraphsArray.Num(), *Blueprint->GetName()),
		ResponseData
	);
}

// ===== list_macros: List macro graphs with entry/exit info =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteListMacros(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
		return Error.GetValue();

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> MacrosArray;

	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (!Graph) continue;

		TSharedPtr<FJsonObject> MacroObj = MakeShared<FJsonObject>();
		MacroObj->SetStringField(TEXT("name"), Graph->GetName());
		MacroObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

		// Find tunnel entry/exit nodes and count user-defined pins
		int32 InputCount = 0;
		int32 OutputCount = 0;
		bool bHasEntry = false;
		bool bHasExit = false;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
			{
				// Entry tunnel: outputs are macro inputs
				if (Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs)
				{
					bHasEntry = true;
					for (UEdGraphPin* Pin : Tunnel->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
							InputCount++;
					}
				}
				// Exit tunnel: inputs are macro outputs
				else if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs)
				{
					bHasExit = true;
					for (UEdGraphPin* Pin : Tunnel->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
							OutputCount++;
					}
				}
			}
		}

		MacroObj->SetBoolField(TEXT("has_entry"), bHasEntry);
		MacroObj->SetBoolField(TEXT("has_exit"), bHasExit);
		MacroObj->SetNumberField(TEXT("input_count"), InputCount);
		MacroObj->SetNumberField(TEXT("output_count"), OutputCount);

		MacrosArray.Add(MakeShared<FJsonValueObject>(MacroObj));
	}

	ResultData->SetArrayField(TEXT("macros"), MacrosArray);
	ResultData->SetNumberField(TEXT("macro_count"), MacrosArray.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("%d macro graph(s) in Blueprint"), MacrosArray.Num()),
		ResultData
	);
}

// ===== get_level_blueprint: Discover and inspect current level blueprint =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetLevelBlueprint(const TSharedRef<FJsonObject>& Params)
{
	FString LoadError;
	UBlueprint* LevelBP = FBlueprintUtils::LoadBlueprint(TEXT("level_blueprint"), LoadError);
	if (!LevelBP) return FMCPToolResult::Error(LoadError);

	TSharedPtr<FJsonObject> ResultData = FBlueprintUtils::SerializeBlueprintInfo(LevelBP, true, true, true);
	ResultData->SetStringField(TEXT("blueprint_path"), TEXT("level_blueprint"));
	ResultData->SetStringField(TEXT("blueprint_type"), TEXT("LevelScriptBlueprint"));
	ResultData->SetStringField(TEXT("hint"), TEXT("Use blueprint_path='level_blueprint' with any blueprint_query/blueprint_modify operation to target this level's blueprint."));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Level Blueprint: %s"), *LevelBP->GetName()),
		ResultData
	);
}

// ===== list_editor_utilities: Discover editor utility assets =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteListEditorUtilities(const TSharedRef<FJsonObject>& Params)
{
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	Limit = FMath::Clamp(Limit, 1, 200);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
	}

	// Search for EditorUtilityBlueprint and EditorUtilityWidgetBlueprint
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Blutility"), TEXT("EditorUtilityBlueprint")));
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Blutility"), TEXT("EditorUtilityWidgetBlueprint")));

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> UtilsArray;
	int32 Count = 0;

	for (const FAssetData& Asset : Assets)
	{
		if (Count >= Limit) break;

		TSharedPtr<FJsonObject> UtilObj = MakeShared<FJsonObject>();
		UtilObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		UtilObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		UtilObj->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());

		FString ParentClass;
		FAssetDataTagMapSharedView::FFindTagResult ParentTag = Asset.TagsAndValues.FindTag(FName("ParentClass"));
		if (ParentTag.IsSet())
		{
			ParentClass = ParentTag.GetValue();
			int32 DotIdx;
			if (ParentClass.FindLastChar(TEXT('.'), DotIdx))
				ParentClass = ParentClass.Mid(DotIdx + 1);
			if (ParentClass.EndsWith(TEXT("_C")))
				ParentClass = ParentClass.LeftChop(2);
			UtilObj->SetStringField(TEXT("parent_class"), ParentClass);
		}

		UtilsArray.Add(MakeShared<FJsonValueObject>(UtilObj));
		Count++;
	}

	ResultData->SetArrayField(TEXT("editor_utilities"), UtilsArray);
	ResultData->SetNumberField(TEXT("count"), Count);
	ResultData->SetNumberField(TEXT("total"), Assets.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d editor utility assets"), Count),
		ResultData
	);
}

// ===== get_editor_utility_details: Inspect editor utility asset =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetEditorUtilityDetails(const TSharedRef<FJsonObject>& Params)
{
	FString UtilityPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), UtilityPath, Error)) return Error.GetValue();

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(UtilityPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("name"), Blueprint->GetName());
	ResultData->SetStringField(TEXT("path"), Blueprint->GetPathName());
	ResultData->SetStringField(TEXT("class"), Blueprint->GetClass()->GetName());

	// Determine utility type
	bool bIsWidgetBP = Blueprint->GetClass()->GetName().Contains(TEXT("WidgetBlueprint"));
	bool bIsEditorUtility = Blueprint->GetClass()->GetName().Contains(TEXT("EditorUtility"));
	ResultData->SetBoolField(TEXT("is_editor_utility"), bIsEditorUtility);
	ResultData->SetBoolField(TEXT("is_widget_utility"), bIsWidgetBP);

	if (Blueprint->ParentClass)
	{
		ResultData->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetName());
		bool bIsRunnable = Blueprint->ParentClass->IsChildOf(AActor::StaticClass()) ||
			Blueprint->GetClass()->GetName().Contains(TEXT("EditorUtilityWidgetBlueprint"));
		ResultData->SetBoolField(TEXT("is_runnable"), bIsRunnable);
		ResultData->SetStringField(TEXT("run_hint"), bIsWidgetBP
			? TEXT("Use run_editor_utility to open this widget in the editor")
			: TEXT("EditorUtilityBlueprint (non-widget) — execution support is limited"));
	}

	// Graph/variable summary
	ResultData->SetNumberField(TEXT("graph_count"), Blueprint->UbergraphPages.Num() + Blueprint->FunctionGraphs.Num());
	ResultData->SetNumberField(TEXT("variable_count"), Blueprint->NewVariables.Num());
	ResultData->SetNumberField(TEXT("function_count"), Blueprint->FunctionGraphs.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Editor utility: %s (%s)"), *Blueprint->GetName(), bIsWidgetBP ? TEXT("Widget") : TEXT("Blueprint")),
		ResultData
	);
}

// ===== Property/Defaults introspection helper =====

static FString PropertyTypeToString(FProperty* Prop)
{
	if (!Prop) return TEXT("unknown");
	if (CastField<FBoolProperty>(Prop)) return TEXT("bool");
	if (CastField<FIntProperty>(Prop)) return TEXT("int32");
	if (CastField<FInt64Property>(Prop)) return TEXT("int64");
	if (CastField<FFloatProperty>(Prop)) return TEXT("float");
	if (CastField<FDoubleProperty>(Prop)) return TEXT("double");
	if (CastField<FStrProperty>(Prop)) return TEXT("FString");
	if (CastField<FNameProperty>(Prop)) return TEXT("FName");
	if (CastField<FTextProperty>(Prop)) return TEXT("FText");
	if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		if (ByteProp->Enum) return FString::Printf(TEXT("enum:%s"), *ByteProp->Enum->GetName());
		return TEXT("byte");
	}
	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		return FString::Printf(TEXT("enum:%s"), *EnumProp->GetEnum()->GetName());
	}
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		return FString::Printf(TEXT("struct:%s"), *StructProp->Struct->GetName());
	}
	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		return FString::Printf(TEXT("%s*"), *ObjProp->PropertyClass->GetName());
	}
	if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
	{
		return FString::Printf(TEXT("TSoftObjectPtr<%s>"), *SoftProp->PropertyClass->GetName());
	}
	if (FSoftClassProperty* SoftClsProp = CastField<FSoftClassProperty>(Prop))
	{
		return FString::Printf(TEXT("TSoftClassPtr<%s>"), *SoftClsProp->MetaClass->GetName());
	}
	if (FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
	{
		return FString::Printf(TEXT("TArray<%s>"), *PropertyTypeToString(ArrProp->Inner));
	}
	if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		return FString::Printf(TEXT("TSet<%s>"), *PropertyTypeToString(SetProp->ElementProp));
	}
	if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		return FString::Printf(TEXT("TMap<%s,%s>"), *PropertyTypeToString(MapProp->KeyProp), *PropertyTypeToString(MapProp->ValueProp));
	}
	return Prop->GetCPPType();
}

static TSharedPtr<FJsonObject> SerializePropertyValue(FProperty* Prop, void* ValuePtr, UObject* Container)
{
	TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
	PropObj->SetStringField(TEXT("name"), Prop->GetName());
	PropObj->SetStringField(TEXT("type"), PropertyTypeToString(Prop));
	PropObj->SetBoolField(TEXT("editable"), Prop->HasAnyPropertyFlags(CPF_Edit));

	// Export value as string
	FString ValueStr;
	Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, Container, PPF_None);
	PropObj->SetStringField(TEXT("value"), ValueStr);

	return PropObj;
}

// ===== Shared nested property path navigation =====

struct FPropertyPathResult
{
	FProperty* Prop = nullptr;
	void* ValuePtr = nullptr;
	FString ErrorMessage;
	bool bSuccess = false;
};

static FPropertyPathResult NavigatePropertyPath(UStruct* RootStruct, void* RootContainer, const FString& PropertyPath)
{
	FPropertyPathResult Result;

	// Try direct property first
	Result.Prop = RootStruct->FindPropertyByName(FName(*PropertyPath));
	if (Result.Prop)
	{
		Result.ValuePtr = Result.Prop->ContainerPtrToValuePtr<void>(RootContainer);
		Result.bSuccess = true;
		return Result;
	}

	// Navigate nested path via dot notation
	FString Remaining = PropertyPath;
	UStruct* CurrentStruct = RootStruct;
	void* CurrentContainer = RootContainer;

	while (!Remaining.IsEmpty())
	{
		FString Head, Tail;
		if (!Remaining.Split(TEXT("."), &Head, &Tail))
		{
			Head = Remaining;
			Tail.Empty();
		}

		FProperty* FieldProp = CurrentStruct->FindPropertyByName(FName(*Head));
		if (!FieldProp)
		{
			Result.ErrorMessage = FString::Printf(TEXT("Property '%s' not found (failed at '%s')"), *PropertyPath, *Head);
			return Result;
		}

		if (Tail.IsEmpty())
		{
			Result.Prop = FieldProp;
			Result.ValuePtr = FieldProp->ContainerPtrToValuePtr<void>(CurrentContainer);
			Result.bSuccess = true;
			return Result;
		}

		FStructProperty* StructProp = CastField<FStructProperty>(FieldProp);
		if (!StructProp)
		{
			Result.ErrorMessage = FString::Printf(TEXT("'%s' is not a struct, cannot navigate further"), *Head);
			return Result;
		}
		CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
		CurrentStruct = StructProp->Struct;
		Remaining = Tail;
	}

	Result.ErrorMessage = FString::Printf(TEXT("Empty property path"));
	return Result;
}

// Determine if a property is defined locally on this class or inherited
static FString GetPropertySource(FProperty* Prop, UClass* GenClass)
{
	if (!Prop || !GenClass) return TEXT("unknown");
	UClass* OwnerClass = Prop->GetOwnerClass();
	if (!OwnerClass) return TEXT("unknown");
	if (OwnerClass == GenClass) return TEXT("local");
	return FString::Printf(TEXT("inherited:%s"), *OwnerClass->GetName());
}

// ===== get_class_defaults: Read Blueprint CDO properties =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetClassDefaults(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
		return Error.GetValue();

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	UClass* GenClass = Blueprint->GeneratedClass;
	if (!GenClass) return FMCPToolResult::Error(TEXT("Blueprint has no GeneratedClass — compile it first"));

	UObject* CDO = GenClass->GetDefaultObject();
	if (!CDO) return FMCPToolResult::Error(TEXT("Could not get Class Default Object"));

	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 50);
	Limit = FMath::Clamp(Limit, 1, 200);
	bool bIncludeInherited = ExtractOptionalBool(Params, TEXT("include_inherited"), false);
	bool bExpandStructs = ExtractOptionalBool(Params, TEXT("expand_structs"), false);

	auto IterFlags = bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PropsArray;
	int32 Count = 0;

	for (TFieldIterator<FProperty> PropIt(GenClass, IterFlags); PropIt; ++PropIt)
	{
		if (Count >= Limit) break;
		FProperty* Prop = *PropIt;
		if (!Prop) continue;

		if (!NameFilter.IsEmpty() && !Prop->GetName().Contains(NameFilter, ESearchCase::IgnoreCase))
			continue;

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
		TSharedPtr<FJsonObject> PropObj = SerializePropertyValue(Prop, ValuePtr, CDO);
		PropObj->SetStringField(TEXT("source"), GetPropertySource(Prop, GenClass));

		// Expand struct children if requested
		if (bExpandStructs)
		{
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				TArray<TSharedPtr<FJsonValue>> ChildrenArr;
				void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(CDO);
				for (TFieldIterator<FProperty> ChildIt(StructProp->Struct); ChildIt; ++ChildIt)
				{
					FProperty* ChildProp = *ChildIt;
					if (!ChildProp) continue;
					void* ChildPtr = ChildProp->ContainerPtrToValuePtr<void>(StructPtr);
					TSharedPtr<FJsonObject> ChildObj = SerializePropertyValue(ChildProp, ChildPtr, CDO);
					ChildObj->SetStringField(TEXT("path"), FString::Printf(TEXT("%s.%s"), *Prop->GetName(), *ChildProp->GetName()));
					ChildrenArr.Add(MakeShared<FJsonValueObject>(ChildObj));
				}
				if (ChildrenArr.Num() > 0) PropObj->SetArrayField(TEXT("struct_children"), ChildrenArr);
			}
		}

		PropsArray.Add(MakeShared<FJsonValueObject>(PropObj));
		Count++;
	}

	ResultData->SetArrayField(TEXT("defaults"), PropsArray);
	ResultData->SetNumberField(TEXT("count"), Count);
	ResultData->SetStringField(TEXT("class_name"), GenClass->GetName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Class defaults: %d properties from %s"), Count, *GenClass->GetName()),
		ResultData
	);
}

// ===== get_editable_properties: Schema for editable properties =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetEditableProperties(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
		return Error.GetValue();

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	UClass* GenClass = Blueprint->GeneratedClass;
	if (!GenClass) return FMCPToolResult::Error(TEXT("Blueprint has no GeneratedClass"));

	bool bIncludeInherited = ExtractOptionalBool(Params, TEXT("include_inherited"), false);
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 100);

	auto IterFlags = bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PropsArray;
	int32 Count = 0;

	for (TFieldIterator<FProperty> PropIt(GenClass, IterFlags); PropIt; ++PropIt)
	{
		if (Count >= Limit) break;
		FProperty* Prop = *PropIt;
		if (!Prop) continue;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

		if (!NameFilter.IsEmpty() && !Prop->GetName().Contains(NameFilter, ESearchCase::IgnoreCase))
			continue;

		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), PropertyTypeToString(Prop));
		PropObj->SetStringField(TEXT("category"), Prop->HasMetaData(TEXT("Category")) ? Prop->GetMetaData(TEXT("Category")) : TEXT(""));
		PropObj->SetBoolField(TEXT("blueprint_read_only"), Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
		PropObj->SetBoolField(TEXT("is_struct"), CastField<FStructProperty>(Prop) != nullptr);
		PropObj->SetBoolField(TEXT("is_array"), CastField<FArrayProperty>(Prop) != nullptr);
		PropObj->SetBoolField(TEXT("is_object_ref"), CastField<FObjectProperty>(Prop) != nullptr);
		PropObj->SetStringField(TEXT("source"), GetPropertySource(Prop, GenClass));

		// For enums, include valid values
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			UEnum* Enum = EnumProp->GetEnum();
			for (int32 i = 0; i < Enum->NumEnums() - 1; i++)
			{
				EnumValues.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(i)));
			}
			PropObj->SetArrayField(TEXT("enum_values"), EnumValues);
		}
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				TArray<TSharedPtr<FJsonValue>> EnumValues;
				for (int32 i = 0; i < ByteProp->Enum->NumEnums() - 1; i++)
				{
					EnumValues.Add(MakeShared<FJsonValueString>(ByteProp->Enum->GetNameStringByIndex(i)));
				}
				PropObj->SetArrayField(TEXT("enum_values"), EnumValues);
			}
		}

		// Struct children for deeper introspection
		if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			TArray<TSharedPtr<FJsonValue>> ChildrenArr;
			for (TFieldIterator<FProperty> ChildIt(StructProp->Struct); ChildIt; ++ChildIt)
			{
				FProperty* ChildProp = *ChildIt;
				if (!ChildProp) continue;
				TSharedPtr<FJsonObject> ChildObj = MakeShared<FJsonObject>();
				ChildObj->SetStringField(TEXT("name"), ChildProp->GetName());
				ChildObj->SetStringField(TEXT("type"), PropertyTypeToString(ChildProp));
				ChildObj->SetStringField(TEXT("path"), FString::Printf(TEXT("%s.%s"), *Prop->GetName(), *ChildProp->GetName()));
				ChildObj->SetBoolField(TEXT("editable"), ChildProp->HasAnyPropertyFlags(CPF_Edit));
				ChildrenArr.Add(MakeShared<FJsonValueObject>(ChildObj));
			}
			if (ChildrenArr.Num() > 0) PropObj->SetArrayField(TEXT("struct_fields"), ChildrenArr);
		}

		// Array/Set/Map inner type info
		if (FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
		{
			PropObj->SetStringField(TEXT("inner_type"), PropertyTypeToString(ArrProp->Inner));
		}
		else if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			PropObj->SetStringField(TEXT("key_type"), PropertyTypeToString(MapProp->KeyProp));
			PropObj->SetStringField(TEXT("value_type"), PropertyTypeToString(MapProp->ValueProp));
		}
		else if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			PropObj->SetStringField(TEXT("element_type"), PropertyTypeToString(SetProp->ElementProp));
		}

		PropsArray.Add(MakeShared<FJsonValueObject>(PropObj));
		Count++;
	}

	ResultData->SetArrayField(TEXT("properties"), PropsArray);
	ResultData->SetNumberField(TEXT("count"), Count);
	ResultData->SetStringField(TEXT("class_name"), GenClass->GetName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Editable properties: %d from %s"), Count, *GenClass->GetName()),
		ResultData
	);
}

// ===== Data Surfaces =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetDataAssetProperties(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), AssetPath, Error)) return Error.GetValue();

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset) return FMCPToolResult::Error(FString::Printf(TEXT("Could not load asset: %s"), *AssetPath));

	bool bIsDataAsset = Asset->IsA<UDataAsset>();
	if (!bIsDataAsset) return FMCPToolResult::Error(FString::Printf(TEXT("Asset '%s' is not a DataAsset (class: %s)"), *AssetPath, *Asset->GetClass()->GetName()));

	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 50);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PropsArray;
	int32 Count = 0;

	for (TFieldIterator<FProperty> PropIt(Asset->GetClass(), EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
	{
		if (Count >= Limit) break;
		FProperty* Prop = *PropIt;
		if (!Prop) continue;
		if (!NameFilter.IsEmpty() && !Prop->GetName().Contains(NameFilter, ESearchCase::IgnoreCase)) continue;

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Asset);
		PropsArray.Add(MakeShared<FJsonValueObject>(SerializePropertyValue(Prop, ValuePtr, Asset)));
		Count++;
	}

	ResultData->SetStringField(TEXT("asset_path"), AssetPath);
	ResultData->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
	ResultData->SetBoolField(TEXT("is_primary_data_asset"), Asset->IsA<UPrimaryDataAsset>());
	ResultData->SetArrayField(TEXT("properties"), PropsArray);
	ResultData->SetNumberField(TEXT("count"), Count);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("DataAsset '%s': %d properties"), *Asset->GetName(), Count),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetDataTableSchema(const TSharedRef<FJsonObject>& Params)
{
	FString TablePath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), TablePath, Error)) return Error.GetValue();

	UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
	if (!Table) return FMCPToolResult::Error(FString::Printf(TEXT("Could not load DataTable: %s"), *TablePath));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("table_path"), TablePath);
	ResultData->SetStringField(TEXT("table_name"), Table->GetName());
	ResultData->SetStringField(TEXT("row_struct"), Table->RowStruct ? Table->RowStruct->GetName() : TEXT("Unknown"));
	ResultData->SetNumberField(TEXT("row_count"), Table->GetRowMap().Num());

	// Schema: list columns from row struct
	TArray<TSharedPtr<FJsonValue>> ColumnsArray;
	if (Table->RowStruct)
	{
		for (TFieldIterator<FProperty> PropIt(Table->RowStruct); PropIt; ++PropIt)
		{
			TSharedPtr<FJsonObject> ColObj = MakeShared<FJsonObject>();
			ColObj->SetStringField(TEXT("name"), PropIt->GetName());
			ColObj->SetStringField(TEXT("type"), PropertyTypeToString(*PropIt));
			ColumnsArray.Add(MakeShared<FJsonValueObject>(ColObj));
		}
	}
	ResultData->SetArrayField(TEXT("columns"), ColumnsArray);
	ResultData->SetNumberField(TEXT("column_count"), ColumnsArray.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("DataTable '%s': %d rows, %d columns"), *Table->GetName(), Table->GetRowMap().Num(), ColumnsArray.Num()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetDataTableRows(const TSharedRef<FJsonObject>& Params)
{
	FString TablePath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), TablePath, Error)) return Error.GetValue();

	UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
	if (!Table) return FMCPToolResult::Error(FString::Printf(TEXT("Could not load DataTable: %s"), *TablePath));

	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	FString RowFilter = ExtractOptionalString(Params, TEXT("name_filter"));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> RowsArray;
	int32 Count = 0;

	for (auto& Pair : Table->GetRowMap())
	{
		if (Count >= Limit) break;
		FString RowName = Pair.Key.ToString();
		if (!RowFilter.IsEmpty() && !RowName.Contains(RowFilter, ESearchCase::IgnoreCase)) continue;

		TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
		RowObj->SetStringField(TEXT("row_name"), RowName);

		// Serialize row fields
		if (Table->RowStruct)
		{
			uint8* RowData = Pair.Value;
			TArray<TSharedPtr<FJsonValue>> FieldsArray;
			for (TFieldIterator<FProperty> PropIt(Table->RowStruct); PropIt; ++PropIt)
			{
				void* ValuePtr = PropIt->ContainerPtrToValuePtr<void>(RowData);
				FieldsArray.Add(MakeShared<FJsonValueObject>(SerializePropertyValue(*PropIt, ValuePtr, nullptr)));
			}
			RowObj->SetArrayField(TEXT("fields"), FieldsArray);
		}

		RowsArray.Add(MakeShared<FJsonValueObject>(RowObj));
		Count++;
	}

	ResultData->SetStringField(TEXT("table_path"), TablePath);
	ResultData->SetArrayField(TEXT("rows"), RowsArray);
	ResultData->SetNumberField(TEXT("count"), Count);
	ResultData->SetNumberField(TEXT("total_rows"), Table->GetRowMap().Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("DataTable '%s': %d rows returned"), *Table->GetName(), Count),
		ResultData
	);
}

// ===== Widget Surfaces =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetWidgetTree(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error)) return Error.GetValue();

	UObject* LoadedObj = LoadObject<UObject>(nullptr, *BlueprintPath);
	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(LoadedObj);
	if (!WidgetBP) return FMCPToolResult::Error(FString::Printf(TEXT("Not a WidgetBlueprint: %s"), *BlueprintPath));

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree) return FMCPToolResult::Error(TEXT("WidgetBlueprint has no WidgetTree"));

	// Recursive serializer
	TFunction<TSharedPtr<FJsonObject>(UWidget*)> SerializeWidget = [&](UWidget* Widget) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Widget) return Obj;

		Obj->SetStringField(TEXT("name"), Widget->GetName());
		Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
		Obj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

		// Children
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			TArray<TSharedPtr<FJsonValue>> ChildrenArr;
			for (int32 i = 0; i < Panel->GetChildrenCount(); i++)
			{
				if (UWidget* Child = Panel->GetChildAt(i))
				{
					ChildrenArr.Add(MakeShared<FJsonValueObject>(SerializeWidget(Child)));
				}
			}
			if (ChildrenArr.Num() > 0)
			{
				Obj->SetArrayField(TEXT("children"), ChildrenArr);
			}
		}
		return Obj;
	};

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	ResultData->SetStringField(TEXT("widget_blueprint"), WidgetBP->GetName());

	UWidget* RootWidget = Tree->RootWidget;
	if (RootWidget)
	{
		ResultData->SetObjectField(TEXT("root"), SerializeWidget(RootWidget));
	}

	// Count all widgets
	TArray<UWidget*> AllWidgets;
	Tree->GetAllWidgets(AllWidgets);
	ResultData->SetNumberField(TEXT("widget_count"), AllWidgets.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Widget tree: %d widgets in %s"), AllWidgets.Num(), *WidgetBP->GetName()),
		ResultData
	);
}

// ===== get_anim_blueprint_info: AnimBP parent/skeleton/generated class =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetAnimBlueprintInfo(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
		return Error.GetValue();

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Blueprint);
	if (!AnimBP)
		return FMCPToolResult::Error(FString::Printf(TEXT("'%s' is not an AnimBlueprint"), *BlueprintPath));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("name"), AnimBP->GetName());
	ResultData->SetStringField(TEXT("path"), AnimBP->GetPathName());

	// Parent class
	if (AnimBP->ParentClass)
	{
		ResultData->SetStringField(TEXT("parent_class"), AnimBP->ParentClass->GetName());
		ResultData->SetStringField(TEXT("parent_class_path"), AnimBP->ParentClass->GetPathName());
		ResultData->SetBoolField(TEXT("is_custom_anim_instance"), !AnimBP->ParentClass->GetName().Equals(TEXT("AnimInstance")));
	}

	// Generated class
	if (AnimBP->GeneratedClass)
	{
		ResultData->SetStringField(TEXT("generated_class"), AnimBP->GeneratedClass->GetName());
		ResultData->SetStringField(TEXT("generated_class_path"), AnimBP->GeneratedClass->GetPathName());
	}

	// Target skeleton
	if (AnimBP->TargetSkeleton)
	{
		ResultData->SetStringField(TEXT("target_skeleton"), AnimBP->TargetSkeleton->GetName());
		ResultData->SetStringField(TEXT("target_skeleton_path"), AnimBP->TargetSkeleton->GetPathName());
		ResultData->SetNumberField(TEXT("bone_count"), AnimBP->TargetSkeleton->GetReferenceSkeleton().GetNum());
	}

	// Preview skeletal mesh
	USkeletalMesh* PreviewMesh = AnimBP->GetPreviewMesh();
	if (PreviewMesh)
	{
		ResultData->SetStringField(TEXT("preview_mesh"), PreviewMesh->GetName());
		ResultData->SetStringField(TEXT("preview_mesh_path"), PreviewMesh->GetPathName());
	}

	// State machine count
	int32 StateMachineCount = 0;
	for (UEdGraph* Graph : AnimBP->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Contains(TEXT("AnimGraph")))
			StateMachineCount++;
	}
	ResultData->SetNumberField(TEXT("function_graph_count"), AnimBP->FunctionGraphs.Num());
	ResultData->SetNumberField(TEXT("event_graph_count"), AnimBP->UbergraphPages.Num());

	// Compile status
	ResultData->SetStringField(TEXT("compile_status"),
		AnimBP->Status == BS_Error ? TEXT("Error") :
		AnimBP->Status == BS_UpToDate ? TEXT("UpToDate") :
		AnimBP->Status == BS_Dirty ? TEXT("Dirty") : TEXT("Unknown"));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("AnimBP '%s': parent=%s, skeleton=%s"),
			*AnimBP->GetName(),
			AnimBP->ParentClass ? *AnimBP->ParentClass->GetName() : TEXT("none"),
			AnimBP->TargetSkeleton ? *AnimBP->TargetSkeleton->GetName() : TEXT("none")),
		ResultData
	);
}

// ===== get_component_editable_properties: Schema for component template properties =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetComponentEditableProperties(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
		return Error.GetValue();

	FString ComponentName;
	if (!ExtractRequiredString(Params, TEXT("component_name"), ComponentName, Error))
		return Error.GetValue();

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString SCSError;
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS) return FMCPToolResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));

	USCS_Node* FoundNode = SCS->FindSCSNode(FName(*ComponentName));
	if (!FoundNode || !FoundNode->ComponentTemplate)
		return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' not found"), *ComponentName));

	UActorComponent* Template = FoundNode->ComponentTemplate;
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 50);
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PropsArray;
	int32 Count = 0;

	for (TFieldIterator<FProperty> PropIt(Template->GetClass()); PropIt; ++PropIt)
	{
		if (Count >= Limit) break;
		FProperty* Prop = *PropIt;
		if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
		if (!NameFilter.IsEmpty() && !Prop->GetName().Contains(NameFilter, ESearchCase::IgnoreCase)) continue;

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);

		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), PropertyTypeToString(Prop));
		PropObj->SetBoolField(TEXT("is_struct"), CastField<FStructProperty>(Prop) != nullptr);
		PropObj->SetBoolField(TEXT("is_array"), CastField<FArrayProperty>(Prop) != nullptr);
		PropObj->SetBoolField(TEXT("is_object_ref"), CastField<FObjectProperty>(Prop) != nullptr);

		FString ValueStr;
		Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, Template, PPF_None);
		PropObj->SetStringField(TEXT("current_value"), ValueStr);

		PropsArray.Add(MakeShared<FJsonValueObject>(PropObj));
		Count++;
	}

	ResultData->SetStringField(TEXT("component_name"), ComponentName);
	ResultData->SetStringField(TEXT("component_class"), Template->GetClass()->GetName());
	ResultData->SetArrayField(TEXT("properties"), PropsArray);
	ResultData->SetNumberField(TEXT("count"), Count);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Component '%s': %d editable properties"), *ComponentName, Count),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetGraphNodes(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 100);
	Limit = FMath::Clamp(Limit, 1, 500);

	FString GraphError;
	UEdGraph* Graph = FBlueprintGraphEditor::FindGraph(Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph) return FMCPToolResult::Error(GraphError);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("graph_name"), Graph->GetName());
	ResponseData->SetStringField(TEXT("blueprint_path"), BlueprintPath);

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	int32 Count = 0;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || Count >= Limit) continue;
		NodesArray.Add(MakeShared<FJsonValueObject>(FBlueprintGraphEditor::SerializeNodeInfo(Node)));
		Count++;
	}

	ResponseData->SetArrayField(TEXT("nodes"), NodesArray);
	ResponseData->SetNumberField(TEXT("node_count"), Count);
	ResponseData->SetNumberField(TEXT("total_nodes"), Graph->Nodes.Num());
	if (Graph->Nodes.Num() > Count)
	{
		ResponseData->SetBoolField(TEXT("truncated"), true);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Graph '%s': %d nodes"), *Graph->GetName(), Count),
		ResponseData
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteFindNodes(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);
	FString ClassFilter = ExtractOptionalString(Params, TEXT("class_filter"));
	FString TitleFilter = ExtractOptionalString(Params, TEXT("title_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 50);
	Limit = FMath::Clamp(Limit, 1, 200);

	FString GraphError;
	UEdGraph* Graph = FBlueprintGraphEditor::FindGraph(Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph) return FMCPToolResult::Error(GraphError);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> MatchedNodes;
	int32 Count = 0;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || Count >= Limit) continue;

		// Apply class filter
		if (!ClassFilter.IsEmpty())
		{
			if (!Node->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Apply title filter
		if (!TitleFilter.IsEmpty())
		{
			FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			if (!Title.Contains(TitleFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		MatchedNodes.Add(MakeShared<FJsonValueObject>(FBlueprintGraphEditor::SerializeNodeInfo(Node)));
		Count++;
	}

	ResponseData->SetArrayField(TEXT("nodes"), MatchedNodes);
	ResponseData->SetNumberField(TEXT("count"), Count);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d matching nodes"), Count),
		ResponseData
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetNodePins(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString NodeId;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeId, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	FString GraphError;
	UEdGraph* Graph = FBlueprintGraphEditor::FindGraph(Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph) return FMCPToolResult::Error(GraphError);

	UEdGraphNode* Node = FBlueprintGraphEditor::FindNodeById(Graph, NodeId);
	if (!Node)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Node not found: %s"), *NodeId));
	}

	// Full node serialization already includes pins with connections
	TSharedPtr<FJsonObject> NodeInfo = FBlueprintGraphEditor::SerializeNodeInfo(Node);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Node '%s': %d pins"), *NodeId, Node->Pins.Num()),
		NodeInfo
	);
}

// ===== get_node: Full node info by ID =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetNode(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString NodeId;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeId, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	FString GraphError;
	UEdGraph* Graph = FBlueprintGraphEditor::FindGraph(Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph) return FMCPToolResult::Error(GraphError);

	UEdGraphNode* Node = FBlueprintGraphEditor::FindNodeById(Graph, NodeId);
	if (!Node)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Node not found: %s"), *NodeId));
	}

	TSharedPtr<FJsonObject> NodeInfo = FBlueprintGraphEditor::SerializeNodeInfo(Node);

	// Add graph context
	NodeInfo->SetStringField(TEXT("graph_name"), Graph->GetName());
	NodeInfo->SetBoolField(TEXT("has_compiler_message"), Node->bHasCompilerMessage);
	if (Node->bHasCompilerMessage)
	{
		NodeInfo->SetStringField(TEXT("error_msg"), Node->ErrorMsg);
		NodeInfo->SetStringField(TEXT("error_type"),
			Node->ErrorType == EMessageSeverity::Error ? TEXT("Error") :
			(Node->ErrorType == EMessageSeverity::Warning ? TEXT("Warning") : TEXT("Info")));
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Node '%s' in graph '%s': %d pins"), *NodeId, *Graph->GetName(), Node->Pins.Num()),
		NodeInfo
	);
}

// ===== get_node_connections: All connections for a node =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetNodeConnections(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString NodeId;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeId, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	FString GraphError;
	UEdGraph* Graph = FBlueprintGraphEditor::FindGraph(Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph) return FMCPToolResult::Error(GraphError);

	UEdGraphNode* Node = FBlueprintGraphEditor::FindNodeById(Graph, NodeId);
	if (!Node)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Node not found: %s"), *NodeId));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("node_id"), NodeId);
	ResultData->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

	TArray<TSharedPtr<FJsonValue>> InboundArray;
	TArray<TSharedPtr<FJsonValue>> OutboundArray;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;

			TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
			ConnObj->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
			ConnObj->SetStringField(TEXT("pin_type"), Pin->PinType.PinCategory.ToString());
			ConnObj->SetStringField(TEXT("connected_node_id"), FBlueprintGraphEditor::GetNodeId(LinkedPin->GetOwningNode()));
			ConnObj->SetStringField(TEXT("connected_node_title"), LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString());
			ConnObj->SetStringField(TEXT("connected_pin_name"), LinkedPin->PinName.ToString());

			if (Pin->Direction == EGPD_Input)
			{
				InboundArray.Add(MakeShared<FJsonValueObject>(ConnObj));
			}
			else
			{
				OutboundArray.Add(MakeShared<FJsonValueObject>(ConnObj));
			}
		}
	}

	ResultData->SetArrayField(TEXT("inbound"), InboundArray);
	ResultData->SetArrayField(TEXT("outbound"), OutboundArray);
	ResultData->SetNumberField(TEXT("inbound_count"), InboundArray.Num());
	ResultData->SetNumberField(TEXT("outbound_count"), OutboundArray.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Node '%s': %d inbound, %d outbound connections"), *NodeId, InboundArray.Num(), OutboundArray.Num()),
		ResultData
	);
}

// ===== can_connect_pins: Dry-run connection check =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteCanConnectPins(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString SourceNodeId, SourcePinName, TargetNodeId, TargetPinName;
	if (!ExtractRequiredString(Params, TEXT("source_node_id"), SourceNodeId, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("source_pin_name"), SourcePinName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("target_node_id"), TargetNodeId, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("target_pin_name"), TargetPinName, Error)) return Error.GetValue();

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	FString GraphError;
	UEdGraph* Graph = FBlueprintGraphEditor::FindGraph(Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph) return FMCPToolResult::Error(GraphError);

	UEdGraphNode* SourceNode = FBlueprintGraphEditor::FindNodeById(Graph, SourceNodeId);
	if (!SourceNode) return FMCPToolResult::Error(FString::Printf(TEXT("Source node not found: %s"), *SourceNodeId));

	UEdGraphNode* TargetNode = FBlueprintGraphEditor::FindNodeById(Graph, TargetNodeId);
	if (!TargetNode) return FMCPToolResult::Error(FString::Printf(TEXT("Target node not found: %s"), *TargetNodeId));

	UEdGraphPin* SourcePin = FBlueprintGraphEditor::FindPinByName(SourceNode, SourcePinName);
	if (!SourcePin) return FMCPToolResult::Error(FString::Printf(TEXT("Source pin '%s' not found on node '%s'"), *SourcePinName, *SourceNodeId));

	UEdGraphPin* TargetPin = FBlueprintGraphEditor::FindPinByName(TargetNode, TargetPinName);
	if (!TargetPin) return FMCPToolResult::Error(FString::Printf(TEXT("Target pin '%s' not found on node '%s'"), *TargetPinName, *TargetNodeId));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("source_node_id"), SourceNodeId);
	ResultData->SetStringField(TEXT("source_pin"), SourcePinName);
	ResultData->SetStringField(TEXT("source_pin_type"), SourcePin->PinType.PinCategory.ToString());
	ResultData->SetStringField(TEXT("target_node_id"), TargetNodeId);
	ResultData->SetStringField(TEXT("target_pin"), TargetPinName);
	ResultData->SetStringField(TEXT("target_pin_type"), TargetPin->PinType.PinCategory.ToString());

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		ResultData->SetBoolField(TEXT("can_connect"), false);
		ResultData->SetStringField(TEXT("reason"), TEXT("No schema available for this graph"));
		return FMCPToolResult::Success(TEXT("Connection check failed: no schema"), ResultData);
	}

	FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
	bool bCanConnect = (Response.Response != CONNECT_RESPONSE_DISALLOW);

	ResultData->SetBoolField(TEXT("can_connect"), bCanConnect);
	ResultData->SetStringField(TEXT("response_type"),
		Response.Response == CONNECT_RESPONSE_MAKE ? TEXT("direct") :
		Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A ? TEXT("break_source_links") :
		Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B ? TEXT("break_target_links") :
		Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB ? TEXT("break_both_links") :
		Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE ? TEXT("with_conversion") :
		Response.Response == CONNECT_RESPONSE_MAKE_WITH_PROMOTION ? TEXT("with_promotion") :
		Response.Response == CONNECT_RESPONSE_DISALLOW ? TEXT("disallowed") :
		TEXT("unknown"));

	if (!bCanConnect)
	{
		ResultData->SetStringField(TEXT("reason"), Response.Message.ToString());
	}

	return FMCPToolResult::Success(
		bCanConnect
			? FString::Printf(TEXT("Connection allowed: %s.%s -> %s.%s"), *SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName)
			: FString::Printf(TEXT("Connection disallowed: %s"), *Response.Message.ToString()),
		ResultData
	);
}

// ===== list_dispatchers: List event dispatchers =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteListDispatchers(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
		return Error.GetValue();

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> DispArr;

	// Query from DelegateSignatureGraphs (the authoritative source for dispatchers)
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (!Graph) continue;

		TSharedPtr<FJsonObject> DispObj = MakeShared<FJsonObject>();
		DispObj->SetStringField(TEXT("name"), Graph->GetName());
		DispObj->SetStringField(TEXT("pin_category"), TEXT("mcdelegate"));
		DispObj->SetNumberField(TEXT("signature_node_count"), Graph->Nodes.Num());

		// Count user-defined params on entry node
		int32 ParamCount = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				for (UEdGraphPin* Pin : Entry->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && Pin->PinName != TEXT("self"))
						ParamCount++;
				}
				break;
			}
		}
		DispObj->SetNumberField(TEXT("param_count"), ParamCount);

		DispArr.Add(MakeShared<FJsonValueObject>(DispObj));
	}

	ResultData->SetArrayField(TEXT("dispatchers"), DispArr);
	ResultData->SetNumberField(TEXT("dispatcher_count"), DispArr.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("%d dispatcher(s) on Blueprint"), DispArr.Num()),
		ResultData
	);
}

// ===== list_interfaces: List implemented interfaces =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteListInterfaces(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> InterfacesArray;

	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (!Desc.Interface) continue;

		TSharedPtr<FJsonObject> IntObj = MakeShared<FJsonObject>();
		IntObj->SetStringField(TEXT("interface_name"), Desc.Interface->GetName());
		IntObj->SetStringField(TEXT("interface_path"), Desc.Interface->GetPathName());

		// List interface functions
		TArray<TSharedPtr<FJsonValue>> FuncsArray;
		for (UEdGraph* Graph : Desc.Graphs)
		{
			if (!Graph) continue;
			TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
			FuncObj->SetStringField(TEXT("name"), Graph->GetName());
			FuncObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			FuncsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
		}
		IntObj->SetArrayField(TEXT("functions"), FuncsArray);
		IntObj->SetNumberField(TEXT("function_count"), FuncsArray.Num());

		InterfacesArray.Add(MakeShared<FJsonValueObject>(IntObj));
	}

	ResultData->SetArrayField(TEXT("interfaces"), InterfacesArray);
	ResultData->SetNumberField(TEXT("interface_count"), InterfacesArray.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("%d interface(s) on Blueprint"), InterfacesArray.Num()),
		ResultData
	);
}

// ===== get_function_signature: Function inputs/outputs =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetFunctionSignature(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString FunctionName;
	if (!ExtractRequiredString(Params, TEXT("function_name"), FunctionName, Error)) return Error.GetValue();

	// Find the function graph
	UEdGraph* FuncGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			FuncGraph = Graph;
			break;
		}
	}
	if (!FuncGraph)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Function '%s' not found"), *FunctionName));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("function_name"), FunctionName);
	ResultData->SetNumberField(TEXT("node_count"), FuncGraph->Nodes.Num());

	TArray<TSharedPtr<FJsonValue>> InputsArray;
	TArray<TSharedPtr<FJsonValue>> OutputsArray;

	// Find entry and result nodes
	for (UEdGraphNode* Node : FuncGraph->Nodes)
	{
		if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
		{
			for (UEdGraphPin* Pin : Entry->Pins)
			{
				if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->PinName == TEXT("self")) continue;

				TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
				ParamObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				ParamObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
				if (Pin->PinType.PinSubCategoryObject.IsValid())
				{
					ParamObj->SetStringField(TEXT("sub_type"), Pin->PinType.PinSubCategoryObject->GetName());
				}
				ParamObj->SetStringField(TEXT("direction"), TEXT("input"));
				if (!Pin->DefaultValue.IsEmpty())
				{
					ParamObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
				}
				InputsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
			}
		}
		else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
		{
			for (UEdGraphPin* Pin : Result->Pins)
			{
				if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

				TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
				ParamObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
				ParamObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
				if (Pin->PinType.PinSubCategoryObject.IsValid())
				{
					ParamObj->SetStringField(TEXT("sub_type"), Pin->PinType.PinSubCategoryObject->GetName());
				}
				ParamObj->SetStringField(TEXT("direction"), TEXT("output"));
				OutputsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
			}
		}
	}

	ResultData->SetArrayField(TEXT("inputs"), InputsArray);
	ResultData->SetArrayField(TEXT("outputs"), OutputsArray);
	ResultData->SetNumberField(TEXT("input_count"), InputsArray.Num());
	ResultData->SetNumberField(TEXT("output_count"), OutputsArray.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Function '%s': %d inputs, %d outputs"), *FunctionName, InputsArray.Num(), OutputsArray.Num()),
		ResultData
	);
}

// ===== Helper: Serialize SCS Node =====

static TSharedPtr<FJsonObject> SerializeSCSNode(USCS_Node* Node, UBlueprint* Blueprint)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Node) return Obj;

	FString CompName = Node->GetVariableName().ToString();
	Obj->SetStringField(TEXT("component_name"), CompName);
	Obj->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown"));
	Obj->SetBoolField(TEXT("is_root"), Node->IsRootNode());

	// Determine if inherited
	bool bInherited = Node->bIsParentComponentNative || Node->ParentComponentOwnerClassName != NAME_None;
	Obj->SetBoolField(TEXT("is_inherited"), bInherited);

	// Parent component
	if (Node->ParentComponentOrVariableName != NAME_None)
	{
		Obj->SetStringField(TEXT("parent_component"), Node->ParentComponentOrVariableName.ToString());
	}

	// Attach socket
	if (Node->AttachToName != NAME_None)
	{
		Obj->SetStringField(TEXT("attach_socket"), Node->AttachToName.ToString());
	}

	return Obj;
}

static TSharedPtr<FJsonObject> SerializeSCSNodeTree(USCS_Node* Node, UBlueprint* Blueprint)
{
	TSharedPtr<FJsonObject> Obj = SerializeSCSNode(Node, Blueprint);

	TArray<TSharedPtr<FJsonValue>> ChildrenArr;
	for (USCS_Node* Child : Node->GetChildNodes())
	{
		if (Child)
		{
			ChildrenArr.Add(MakeShared<FJsonValueObject>(SerializeSCSNodeTree(Child, Blueprint)));
		}
	}
	if (ChildrenArr.Num() > 0)
	{
		Obj->SetArrayField(TEXT("children"), ChildrenArr);
	}

	return Obj;
}

static USimpleConstructionScript* GetBlueprintSCS(UBlueprint* Blueprint, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		OutError = FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript (not an Actor Blueprint?)"), *Blueprint->GetName());
	}
	return SCS;
}

// ===== Component Query Operations =====

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteListComponents(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString SCSError;
	USimpleConstructionScript* SCS = GetBlueprintSCS(Blueprint, SCSError);
	if (!SCS) return FMCPToolResult::Error(SCSError);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ComponentsArray;

	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node)
		{
			ComponentsArray.Add(MakeShared<FJsonValueObject>(SerializeSCSNode(Node, Blueprint)));
		}
	}

	ResultData->SetArrayField(TEXT("components"), ComponentsArray);
	ResultData->SetNumberField(TEXT("count"), ComponentsArray.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d components in %s"), ComponentsArray.Num(), *Blueprint->GetName()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetComponentTree(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString SCSError;
	USimpleConstructionScript* SCS = GetBlueprintSCS(Blueprint, SCSError);
	if (!SCS) return FMCPToolResult::Error(SCSError);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> RootNodes;

	for (USCS_Node* RootNode : SCS->GetRootNodes())
	{
		if (RootNode)
		{
			RootNodes.Add(MakeShared<FJsonValueObject>(SerializeSCSNodeTree(RootNode, Blueprint)));
		}
	}

	ResultData->SetArrayField(TEXT("tree"), RootNodes);
	ResultData->SetNumberField(TEXT("total_components"), SCS->GetAllNodes().Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Component tree for %s: %d total components"), *Blueprint->GetName(), SCS->GetAllNodes().Num()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetComponentDetails(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractAndValidate(Params, TEXT("blueprint_path"), FMCPParamValidator::ValidateBlueprintPath, BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString ComponentName;
	if (!ExtractRequiredString(Params, TEXT("component_name"), ComponentName, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint) return FMCPToolResult::Error(LoadError);

	FString SCSError;
	USimpleConstructionScript* SCS = GetBlueprintSCS(Blueprint, SCSError);
	if (!SCS) return FMCPToolResult::Error(SCSError);

	USCS_Node* FoundNode = SCS->FindSCSNode(FName(*ComponentName));
	if (!FoundNode)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' not found"), *ComponentName));
	}

	TSharedPtr<FJsonObject> ResultData = SerializeSCSNode(FoundNode, Blueprint);

	// Add transform info for scene components
	if (FoundNode->ComponentTemplate)
	{
		USceneComponent* SceneComp = Cast<USceneComponent>(FoundNode->ComponentTemplate);
		if (SceneComp)
		{
			TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();
			FVector Loc = SceneComp->GetRelativeLocation();
			FRotator Rot = SceneComp->GetRelativeRotation();
			FVector Scale = SceneComp->GetRelativeScale3D();

			TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
			LocObj->SetNumberField(TEXT("x"), Loc.X); LocObj->SetNumberField(TEXT("y"), Loc.Y); LocObj->SetNumberField(TEXT("z"), Loc.Z);
			TransformObj->SetObjectField(TEXT("location"), LocObj);

			TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
			RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch); RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw); RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
			TransformObj->SetObjectField(TEXT("rotation"), RotObj);

			TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
			ScaleObj->SetNumberField(TEXT("x"), Scale.X); ScaleObj->SetNumberField(TEXT("y"), Scale.Y); ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
			TransformObj->SetObjectField(TEXT("scale"), ScaleObj);

			ResultData->SetObjectField(TEXT("relative_transform"), TransformObj);
		}

		// Basic properties
		TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
		UActorComponent* Template = FoundNode->ComponentTemplate;

		// Mobility for scene components
		if (SceneComp)
		{
			FString MobilityStr;
			switch (SceneComp->Mobility)
			{
				case EComponentMobility::Static: MobilityStr = TEXT("Static"); break;
				case EComponentMobility::Stationary: MobilityStr = TEXT("Stationary"); break;
				case EComponentMobility::Movable: MobilityStr = TEXT("Movable"); break;
				default: MobilityStr = TEXT("Unknown"); break;
			}
			PropsObj->SetStringField(TEXT("mobility"), MobilityStr);
			PropsObj->SetBoolField(TEXT("visible"), SceneComp->GetVisibleFlag());
		}

		PropsObj->SetBoolField(TEXT("auto_activate"), Template->bAutoActivate);
		ResultData->SetObjectField(TEXT("properties"), PropsObj);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Component details: %s"), *ComponentName),
		ResultData
	);
}
