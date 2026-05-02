// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_BlueprintModify.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UnrealClaudeExecutionLog.h"
#include "BlueprintUtils.h"
#include "BlueprintGraphEditor.h"
#include "MCP/MCPParamValidator.h"
#include "MCP/MCPBlueprintLoadContext.h"
#include "UnrealClaudeModule.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Tunnel.h"
#include "K2Node_MacroInstance.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EditorUtilityBlueprint.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "EditorUtilitySubsystem.h"
#include "Editor.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Animation/AnimBlueprint.h"
#include "Components/SkeletalMeshComponent.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"

// Operation name constants
namespace BlueprintModifyOps
{
	static const FString Create = TEXT("create");
	static const FString AddVariable = TEXT("add_variable");
	static const FString RemoveVariable = TEXT("remove_variable");
	static const FString AddFunction = TEXT("add_function");
	static const FString RemoveFunction = TEXT("remove_function");
	static const FString AddNode = TEXT("add_node");
	static const FString AddNodes = TEXT("add_nodes");
	static const FString DeleteNode = TEXT("delete_node");
	static const FString ConnectPins = TEXT("connect_pins");
	static const FString DisconnectPins = TEXT("disconnect_pins");
	static const FString SetPinValue = TEXT("set_pin_value");
	static const FString ValidateBlueprint = TEXT("validate_blueprint");
	static const FString ReplaceNode = TEXT("replace_node");
	static const FString SetFunctionSignature = TEXT("set_function_signature");
	static const FString AddInterface = TEXT("add_interface");
	static const FString RemoveInterface = TEXT("remove_interface");
	static const FString AddDispatcher = TEXT("add_dispatcher");
	static const FString RemoveDispatcher = TEXT("remove_dispatcher");
	static const FString SetDispatcherSignature = TEXT("set_dispatcher_signature");
	static const FString ApplyBlueprintSpec = TEXT("apply_blueprint_spec");
	static const FString RunEditorUtility = TEXT("run_editor_utility");
	static const FString BindDispatcher = TEXT("bind_dispatcher");
	static const FString CreateMacro = TEXT("create_macro");
	static const FString RemoveMacro = TEXT("remove_macro");
	static const FString AddMacroInstance = TEXT("add_macro_instance");
	static const FString AddComponent = TEXT("add_component");
	static const FString RemoveComponent = TEXT("remove_component");
	static const FString RenameComponent = TEXT("rename_component");
	static const FString ReparentComponent = TEXT("reparent_component");
	static const FString SetComponentProperty = TEXT("set_component_property");
}

FMCPToolResult FMCPTool_BlueprintModify::Execute(const TSharedRef<FJsonObject>& Params)
{
	// Get operation type
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	// Level 2: Variable/Function Operations
	if (Operation == BlueprintModifyOps::Create)
	{
		return ExecuteCreate(Params);
	}
	if (Operation == BlueprintModifyOps::AddVariable)
	{
		return ExecuteAddVariable(Params);
	}
	if (Operation == BlueprintModifyOps::RemoveVariable)
	{
		return ExecuteRemoveVariable(Params);
	}
	if (Operation == BlueprintModifyOps::AddFunction)
	{
		return ExecuteAddFunction(Params);
	}
	if (Operation == BlueprintModifyOps::RemoveFunction)
	{
		return ExecuteRemoveFunction(Params);
	}
	// Level 3: Node Operations
	if (Operation == BlueprintModifyOps::AddNode)
	{
		return ExecuteAddNode(Params);
	}
	if (Operation == BlueprintModifyOps::AddNodes)
	{
		return ExecuteAddNodes(Params);
	}
	if (Operation == BlueprintModifyOps::DeleteNode)
	{
		return ExecuteDeleteNode(Params);
	}
	// Level 4: Connection Operations
	if (Operation == BlueprintModifyOps::ConnectPins)
	{
		return ExecuteConnectPins(Params);
	}
	if (Operation == BlueprintModifyOps::DisconnectPins)
	{
		return ExecuteDisconnectPins(Params);
	}
	if (Operation == BlueprintModifyOps::SetPinValue)
	{
		return ExecuteSetPinValue(Params);
	}
	if (Operation == BlueprintModifyOps::ValidateBlueprint)
	{
		return ExecuteValidateBlueprint(Params);
	}
	if (Operation == BlueprintModifyOps::ReplaceNode)
	{
		return ExecuteReplaceNode(Params);
	}
	if (Operation == BlueprintModifyOps::SetFunctionSignature)
	{
		return ExecuteSetFunctionSignature(Params);
	}
	if (Operation == BlueprintModifyOps::AddInterface)
	{
		return ExecuteAddInterface(Params);
	}
	if (Operation == BlueprintModifyOps::RemoveInterface)
	{
		return ExecuteRemoveInterface(Params);
	}
	if (Operation == BlueprintModifyOps::AddDispatcher)
	{
		return ExecuteAddDispatcher(Params);
	}
	if (Operation == BlueprintModifyOps::RemoveDispatcher)
	{
		return ExecuteRemoveDispatcher(Params);
	}
	if (Operation == BlueprintModifyOps::SetDispatcherSignature)
	{
		return ExecuteSetDispatcherSignature(Params);
	}
	if (Operation == BlueprintModifyOps::ApplyBlueprintSpec)
	{
		return ExecuteApplyBlueprintSpec(Params);
	}
	static const FString SetClassDefault = TEXT("set_class_default");
	static const FString SetClassDefaults = TEXT("set_class_defaults");
	static const FString SetComponentPropertiesBatch = TEXT("set_component_properties_batch");
	if (Operation == SetClassDefault)
	{
		return ExecuteSetClassDefault(Params);
	}
	if (Operation == SetClassDefaults)
	{
		return ExecuteSetClassDefaults(Params);
	}
	if (Operation == SetComponentPropertiesBatch)
	{
		return ExecuteSetComponentPropertiesBatch(Params);
	}
	static const FString ModifyCollection = TEXT("modify_collection");
	static const FString ConfigureActorClass = TEXT("configure_actor_class");
	if (Operation == ModifyCollection)
	{
		return ExecuteModifyCollection(Params);
	}
	if (Operation == ConfigureActorClass)
	{
		return ExecuteConfigureActorClass(Params);
	}
	static const FString SetAnimBlueprint = TEXT("set_anim_blueprint");
	static const FString SetDataAssetProperties = TEXT("set_data_asset_properties");
	static const FString SetDataTableRow = TEXT("set_data_table_row");
	static const FString RemoveDataTableRow = TEXT("remove_data_table_row");
	static const FString AddWidget = TEXT("add_widget");
	static const FString RemoveWidget = TEXT("remove_widget");
	static const FString SetWidgetProperty = TEXT("set_widget_property");
	if (Operation == SetAnimBlueprint)
	{
		return ExecuteSetAnimBlueprint(Params);
	}
	if (Operation == SetDataAssetProperties)
	{
		return ExecuteSetDataAssetProperties(Params);
	}
	if (Operation == SetDataTableRow)
	{
		return ExecuteSetDataTableRow(Params);
	}
	if (Operation == RemoveDataTableRow)
	{
		return ExecuteRemoveDataTableRow(Params);
	}
	if (Operation == AddWidget)
	{
		return ExecuteAddWidget(Params);
	}
	if (Operation == RemoveWidget)
	{
		return ExecuteRemoveWidget(Params);
	}
	if (Operation == SetWidgetProperty)
	{
		return ExecuteSetWidgetProperty(Params);
	}
	if (Operation == BlueprintModifyOps::RunEditorUtility)
	{
		return ExecuteRunEditorUtility(Params);
	}
	if (Operation == BlueprintModifyOps::BindDispatcher)
	{
		return ExecuteBindDispatcher(Params);
	}
	if (Operation == BlueprintModifyOps::CreateMacro)
	{
		return ExecuteCreateMacro(Params);
	}
	if (Operation == BlueprintModifyOps::RemoveMacro)
	{
		return ExecuteRemoveMacro(Params);
	}
	if (Operation == BlueprintModifyOps::AddMacroInstance)
	{
		return ExecuteAddMacroInstance(Params);
	}
	if (Operation == BlueprintModifyOps::AddComponent)
	{
		return ExecuteAddComponent(Params);
	}
	if (Operation == BlueprintModifyOps::RemoveComponent)
	{
		return ExecuteRemoveComponent(Params);
	}
	if (Operation == BlueprintModifyOps::RenameComponent)
	{
		return ExecuteRenameComponent(Params);
	}
	if (Operation == BlueprintModifyOps::ReparentComponent)
	{
		return ExecuteReparentComponent(Params);
	}
	if (Operation == BlueprintModifyOps::SetComponentProperty)
	{
		return ExecuteSetComponentProperty(Params);
	}

	FMCPToolResult UnknownResult = FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation: '%s'. Valid: create, add_variable, remove_variable, add_function, remove_function, add_node, add_nodes, delete_node, connect_pins, disconnect_pins, set_pin_value, validate_blueprint, replace_node, set_function_signature, add_interface, remove_interface, add_dispatcher, remove_dispatcher, set_dispatcher_signature, apply_blueprint_spec, set_class_default, set_class_defaults, set_component_properties_batch, set_data_asset_properties, set_data_table_row, remove_data_table_row, add_widget, remove_widget, set_widget_property, set_anim_blueprint, create_macro, remove_macro, add_macro_instance, bind_dispatcher, run_editor_utility, add_component, remove_component, rename_component, reparent_component, set_component_property"),
		*Operation));

	// Log execution receipt for all operations
	FExecutionReceipt Receipt;
	Receipt.Tool = FString::Printf(TEXT("blueprint_modify:%s"), *Operation);
	Receipt.bSuccess = UnknownResult.bSuccess;
	Receipt.TargetType = TEXT("blueprint");
	FString BP;
	if (Params->TryGetStringField(TEXT("blueprint_path"), BP))
	{
		Receipt.Targets.Add(BP);
	}
	FUnrealClaudeExecutionLog::Get().AddReceipt(Receipt);

	return UnknownResult;
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteCreate(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	FString PackagePath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("package_path"), PackagePath, Error))
	{
		return Error.GetValue();
	}

	FString BlueprintName;
	if (!ExtractRequiredString(Params, TEXT("blueprint_name"), BlueprintName, Error))
	{
		return Error.GetValue();
	}

	FString ParentClassName;
	if (!ExtractRequiredString(Params, TEXT("parent_class"), ParentClassName, Error))
	{
		return Error.GetValue();
	}

	FString BlueprintTypeStr = ExtractOptionalString(Params, TEXT("blueprint_type"), TEXT("Normal"));

	// Validate package path
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintPath(PackagePath, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Validate Blueprint name
	if (!FMCPParamValidator::ValidateBlueprintVariableName(BlueprintName, ValidationError))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Invalid Blueprint name: %s"), *ValidationError));
	}

	// Find parent class
	FString ClassError;
	UClass* ParentClass = FBlueprintUtils::FindParentClass(ParentClassName, ClassError);
	if (!ParentClass)
	{
		return FMCPToolResult::Error(ClassError);
	}

	// Parse Blueprint type
	EBlueprintType BlueprintType = ParseBlueprintType(BlueprintTypeStr);

	// Create the Blueprint
	FString CreateError;
	UBlueprint* NewBlueprint = FBlueprintUtils::CreateBlueprint(
		PackagePath,
		BlueprintName,
		ParentClass,
		BlueprintType,
		CreateError
	);

	if (!NewBlueprint)
	{
		return FMCPToolResult::Error(CreateError);
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("blueprint_name"), NewBlueprint->GetName());
	ResultData->SetStringField(TEXT("blueprint_path"), NewBlueprint->GetPathName());
	ResultData->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	ResultData->SetStringField(TEXT("blueprint_type"), FBlueprintUtils::GetBlueprintTypeString(BlueprintType));
	ResultData->SetBoolField(TEXT("compiled"), true);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Created Blueprint: %s"), *NewBlueprint->GetPathName()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddVariable(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString VariableName;
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VariableName, Error))
	{
		return Error.GetValue();
	}

	FString VariableType;
	if (!ExtractRequiredString(Params, TEXT("variable_type"), VariableType, Error))
	{
		return Error.GetValue();
	}

	// Validate variable name
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintVariableName(VariableName, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Parse variable type
	FEdGraphPinType PinType;
	FString TypeError;
	if (!FBlueprintUtils::ParsePinType(VariableType, PinType, TypeError))
	{
		return FMCPToolResult::Error(TypeError);
	}

	// Add the variable
	FString AddError;
	if (!FBlueprintUtils::AddVariable(Context.Blueprint, VariableName, PinType, AddError))
	{
		return FMCPToolResult::Error(AddError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Variable added")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("variable_name"), VariableName);
	ResultData->SetStringField(TEXT("variable_type"), VariableType);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added variable '%s' (%s) to Blueprint"), *VariableName, *VariableType),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveVariable(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString VariableName;
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VariableName, Error))
	{
		return Error.GetValue();
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Remove the variable
	FString RemoveError;
	if (!FBlueprintUtils::RemoveVariable(Context.Blueprint, VariableName, RemoveError))
	{
		return FMCPToolResult::Error(RemoveError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Variable removed")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("variable_name"), VariableName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed variable '%s' from Blueprint"), *VariableName),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddFunction(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString FunctionName;
	if (!ExtractRequiredString(Params, TEXT("function_name"), FunctionName, Error))
	{
		return Error.GetValue();
	}

	// Validate function name
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintFunctionName(FunctionName, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Add the function
	FString AddError;
	if (!FBlueprintUtils::AddFunction(Context.Blueprint, FunctionName, AddError))
	{
		return FMCPToolResult::Error(AddError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Function added")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("function_name"), FunctionName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added function '%s' to Blueprint"), *FunctionName),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveFunction(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString FunctionName;
	if (!ExtractRequiredString(Params, TEXT("function_name"), FunctionName, Error))
	{
		return Error.GetValue();
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Remove the function
	FString RemoveError;
	if (!FBlueprintUtils::RemoveFunction(Context.Blueprint, FunctionName, RemoveError))
	{
		return FMCPToolResult::Error(RemoveError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Function removed")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("function_name"), FunctionName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed function '%s' from Blueprint"), *FunctionName),
		ResultData
	);
}

EBlueprintType FMCPTool_BlueprintModify::ParseBlueprintType(const FString& TypeString)
{
	FString LowerType = TypeString.ToLower();

	if (LowerType == TEXT("normal") || LowerType == TEXT("actor") || LowerType == TEXT("object"))
	{
		return BPTYPE_Normal;
	}
	if (LowerType == TEXT("functionlibrary") || LowerType == TEXT("function_library"))
	{
		return BPTYPE_FunctionLibrary;
	}
	if (LowerType == TEXT("interface"))
	{
		return BPTYPE_Interface;
	}
	if (LowerType == TEXT("macrolibrary") || LowerType == TEXT("macro_library") || LowerType == TEXT("macro"))
	{
		return BPTYPE_MacroLibrary;
	}

	// Default to normal
	return BPTYPE_Normal;
}

// ===== Level 3: Node Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddNode(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString NodeType;
	if (!ExtractRequiredString(Params, TEXT("node_type"), NodeType, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);
	int32 PosX = (int32)ExtractOptionalNumber(Params, TEXT("pos_x"), 0);
	int32 PosY = (int32)ExtractOptionalNumber(Params, TEXT("pos_y"), 0);

	// Get node params object
	TSharedPtr<FJsonObject> NodeParams;
	const TSharedPtr<FJsonObject>* NodeParamsPtr;
	if (Params->TryGetObjectField(TEXT("node_params"), NodeParamsPtr))
	{
		NodeParams = *NodeParamsPtr;
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Create the node
	FString NodeId;
	FString CreateError;
	UEdGraphNode* NewNode = FBlueprintUtils::CreateNode(Graph, NodeType, NodeParams, PosX, PosY, NodeId, CreateError);
	if (!NewNode)
	{
		return FMCPToolResult::Error(CreateError);
	}

	// Apply pin default values if provided
	if (NodeParams.IsValid())
	{
		const TSharedPtr<FJsonObject>* PinValuesPtr;
		if (NodeParams->TryGetObjectField(TEXT("pin_values"), PinValuesPtr))
		{
			for (const auto& PinValue : (*PinValuesPtr)->Values)
			{
				FString PinValueStr;
				if (PinValue.Value->TryGetString(PinValueStr))
				{
					FString PinError;
					FBlueprintUtils::SetPinDefaultValue(Graph, NodeId, PinValue.Key, PinValueStr, PinError);
				}
			}
		}
	}

	// Compile and finalize (skip on BPs with delegate graphs — UE 5.7 crash prevention)
	if (Context.Blueprint->DelegateSignatureGraphs.Num() > 0)
	{
		FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
	}
	else if (auto CompileError = Context.CompileAndFinalize(TEXT("Node created")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = FBlueprintUtils::SerializeNodeInfo(NewNode);
	ResultData->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
	ResultData->SetStringField(TEXT("graph_name"), Graph->GetName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Created node '%s' (type: %s)"), *NodeId, *NodeType),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddNodes(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	// Get nodes array
	const TArray<TSharedPtr<FJsonValue>>* NodesArray;
	if (!Params->TryGetArrayField(TEXT("nodes"), NodesArray))
	{
		return FMCPToolResult::Error(TEXT("'nodes' array is required"));
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Create all nodes using helper
	TArray<FString> CreatedNodeIds;
	TArray<TSharedPtr<FJsonValue>> CreatedNodes;
	TArray<TSharedPtr<FJsonValue>> NodeFailures;
	TArray<TSharedPtr<FJsonValue>> PinValueFailures;
	FString CreateError;
	bool bAllNodesCreated = CreateNodesFromSpec(Graph, *NodesArray, CreatedNodeIds, CreatedNodes, CreateError, NodeFailures, PinValueFailures);

	// Process connections — collect failures
	TArray<TSharedPtr<FJsonValue>> ConnectionFailures;
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray;
	if (Params->TryGetArrayField(TEXT("connections"), ConnectionsArray))
	{
		ProcessNodeConnections(Graph, *ConnectionsArray, CreatedNodeIds, ConnectionFailures);
	}

	// Compile — capture result but don't early-return
	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	bool bCompileSuccess = Context.CompileResult.bSuccess;
	if (bCompileSuccess)
	{
		FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
	}

	// Build result with full transparency — always includes mutation + compile data
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("graph_name"), Graph->GetName());
	ResultData->SetArrayField(TEXT("nodes"), CreatedNodes);
	ResultData->SetNumberField(TEXT("node_count"), CreatedNodes.Num());
	bool bHasAnyFailures = !bAllNodesCreated || NodeFailures.Num() > 0 || PinValueFailures.Num() > 0 || ConnectionFailures.Num() > 0 || !bCompileSuccess;
	ResultData->SetBoolField(TEXT("partial_success"), bHasAnyFailures);

	if (!CreateError.IsEmpty())
	{
		ResultData->SetStringField(TEXT("warnings"), CreateError);
	}
	if (NodeFailures.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("node_failures"), NodeFailures);
	}
	if (PinValueFailures.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("pin_value_failures"), PinValueFailures);
	}
	if (ConnectionFailures.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("connection_failures"), ConnectionFailures);
	}

	FString Msg = FString::Printf(TEXT("Created %d nodes"), CreatedNodes.Num());
	if (NodeFailures.Num() > 0) Msg += FString::Printf(TEXT(", %d node failures"), NodeFailures.Num());
	if (PinValueFailures.Num() > 0) Msg += FString::Printf(TEXT(", %d pin value failures"), PinValueFailures.Num());
	if (ConnectionFailures.Num() > 0) Msg += FString::Printf(TEXT(", %d connection failures"), ConnectionFailures.Num());
	if (!bCompileSuccess) Msg += TEXT(", compile failed");

	FMCPToolResult Result;
	Result.bSuccess = !bHasAnyFailures;
	Result.Message = Msg;
	Result.Data = ResultData;
	return Result;
}

bool FMCPTool_BlueprintModify::CreateNodesFromSpec(
	UEdGraph* Graph,
	const TArray<TSharedPtr<FJsonValue>>& NodesArray,
	TArray<FString>& OutCreatedNodeIds,
	TArray<TSharedPtr<FJsonValue>>& OutCreatedNodes,
	FString& OutError,
	TArray<TSharedPtr<FJsonValue>>& OutNodeFailures,
	TArray<TSharedPtr<FJsonValue>>& OutPinValueFailures)
{
	TArray<FString> Warnings;
	int32 FailedCount = 0;

	for (int32 i = 0; i < NodesArray.Num(); i++)
	{
		const TSharedPtr<FJsonObject>* NodeSpec;
		if (!NodesArray[i]->TryGetObject(NodeSpec))
		{
			{ TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>(); F->SetNumberField(TEXT("index"), i); F->SetStringField(TEXT("error"), TEXT("Not a valid object")); OutNodeFailures.Add(MakeShared<FJsonValueObject>(F)); }
			Warnings.Add(FString::Printf(TEXT("Node %d: not a valid object, skipped"), i));
			OutCreatedNodeIds.Add(FString());
			FailedCount++;
			continue;
		}

		FString NodeType = (*NodeSpec)->GetStringField(TEXT("type"));
		if (NodeType.IsEmpty())
		{
			{ TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>(); F->SetNumberField(TEXT("index"), i); F->SetStringField(TEXT("error"), TEXT("Missing 'type' field")); OutNodeFailures.Add(MakeShared<FJsonValueObject>(F)); }
			Warnings.Add(FString::Printf(TEXT("Node %d: missing 'type' field, skipped"), i));
			OutCreatedNodeIds.Add(FString());
			FailedCount++;
			continue;
		}

		int32 PosX = (int32)(*NodeSpec)->GetNumberField(TEXT("pos_x"));
		int32 PosY = (int32)(*NodeSpec)->GetNumberField(TEXT("pos_y"));

		// Get params (could be inline or nested)
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject>* ParamsPtr;
		if ((*NodeSpec)->TryGetObjectField(TEXT("params"), ParamsPtr))
		{
			NodeParams = *ParamsPtr;
		}
		else
		{
			if ((*NodeSpec)->HasField(TEXT("function")))
				NodeParams->SetStringField(TEXT("function"), (*NodeSpec)->GetStringField(TEXT("function")));
			if ((*NodeSpec)->HasField(TEXT("target_class")))
				NodeParams->SetStringField(TEXT("target_class"), (*NodeSpec)->GetStringField(TEXT("target_class")));
			if ((*NodeSpec)->HasField(TEXT("event")))
				NodeParams->SetStringField(TEXT("event"), (*NodeSpec)->GetStringField(TEXT("event")));
			if ((*NodeSpec)->HasField(TEXT("variable")))
				NodeParams->SetStringField(TEXT("variable"), (*NodeSpec)->GetStringField(TEXT("variable")));
			if ((*NodeSpec)->HasField(TEXT("num_outputs")))
				NodeParams->SetNumberField(TEXT("num_outputs"), (*NodeSpec)->GetNumberField(TEXT("num_outputs")));
		}

		// Create node
		FString NodeId;
		FString CreateError;
		UEdGraphNode* NewNode = FBlueprintUtils::CreateNode(Graph, NodeType, NodeParams, PosX, PosY, NodeId, CreateError);
		if (!NewNode)
		{
			{ TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>(); F->SetNumberField(TEXT("index"), i); F->SetStringField(TEXT("node_type"), NodeType); F->SetStringField(TEXT("error"), CreateError); OutNodeFailures.Add(MakeShared<FJsonValueObject>(F)); }
			Warnings.Add(FString::Printf(TEXT("Node %d (%s): %s"), i, *NodeType, *CreateError));
			OutCreatedNodeIds.Add(FString());
			FailedCount++;
			continue;
		}

		OutCreatedNodeIds.Add(NodeId);

		// Apply pin default values — report failures, don't skip
		const TSharedPtr<FJsonObject>* PinValuesPtr;
		if ((*NodeSpec)->TryGetObjectField(TEXT("pin_values"), PinValuesPtr))
		{
			for (const auto& PinValue : (*PinValuesPtr)->Values)
			{
				FString PinValueStr;
				if (PinValue.Value->TryGetString(PinValueStr))
				{
					FString PinError;
					if (!FBlueprintUtils::SetPinDefaultValue(Graph, NodeId, PinValue.Key, PinValueStr, PinError))
					{
						TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
						F->SetNumberField(TEXT("index"), i);
						F->SetStringField(TEXT("node_id"), NodeId);
						F->SetStringField(TEXT("pin_name"), PinValue.Key);
						F->SetStringField(TEXT("error"), PinError);
						OutPinValueFailures.Add(MakeShared<FJsonValueObject>(F));
						Warnings.Add(FString::Printf(TEXT("Node %d pin '%s': %s"), i, *PinValue.Key, *PinError));
					}
				}
			}
		}

		// Add to result
		TSharedPtr<FJsonObject> NodeInfo = FBlueprintUtils::SerializeNodeInfo(NewNode);
		NodeInfo->SetNumberField(TEXT("index"), i);
		OutCreatedNodes.Add(MakeShared<FJsonValueObject>(NodeInfo));
	}

	// Build error summary if any failures
	if (Warnings.Num() > 0)
	{
		OutError = FString::Printf(TEXT("%d warning(s): "), Warnings.Num());
		for (const FString& W : Warnings)
		{
			OutError += W + TEXT("; ");
		}
	}

	// Return false only if ALL nodes failed
	return FailedCount < NodesArray.Num();
}

void FMCPTool_BlueprintModify::ProcessNodeConnections(
	UEdGraph* Graph,
	const TArray<TSharedPtr<FJsonValue>>& ConnectionsArray,
	const TArray<FString>& CreatedNodeIds,
	TArray<TSharedPtr<FJsonValue>>& OutConnectionFailures)
{
	for (int32 i = 0; i < ConnectionsArray.Num(); i++)
	{
		const TSharedPtr<FJsonObject>* ConnSpec;
		if (!ConnectionsArray[i]->TryGetObject(ConnSpec))
		{
			continue;
		}

		// Get source - can be index or node_id
		FString SourceNodeId;
		if ((*ConnSpec)->HasTypedField<EJson::Number>(TEXT("from_node")))
		{
			int32 FromIndex = (int32)(*ConnSpec)->GetNumberField(TEXT("from_node"));
			if (FromIndex >= 0 && FromIndex < CreatedNodeIds.Num())
			{
				SourceNodeId = CreatedNodeIds[FromIndex];
			}
		}
		else if ((*ConnSpec)->HasTypedField<EJson::String>(TEXT("from_node")))
		{
			SourceNodeId = (*ConnSpec)->GetStringField(TEXT("from_node"));
		}

		// Get target - can be index or node_id
		FString TargetNodeId;
		if ((*ConnSpec)->HasTypedField<EJson::Number>(TEXT("to_node")))
		{
			int32 ToIndex = (int32)(*ConnSpec)->GetNumberField(TEXT("to_node"));
			if (ToIndex >= 0 && ToIndex < CreatedNodeIds.Num())
			{
				TargetNodeId = CreatedNodeIds[ToIndex];
			}
		}
		else if ((*ConnSpec)->HasTypedField<EJson::String>(TEXT("to_node")))
		{
			TargetNodeId = (*ConnSpec)->GetStringField(TEXT("to_node"));
		}

		FString SourcePin = (*ConnSpec)->GetStringField(TEXT("from_pin"));
		FString TargetPin = (*ConnSpec)->GetStringField(TEXT("to_pin"));

		if (!SourceNodeId.IsEmpty() && !TargetNodeId.IsEmpty())
		{
			FString ConnectError;
			if (!FBlueprintUtils::ConnectPins(Graph, SourceNodeId, SourcePin, TargetNodeId, TargetPin, ConnectError))
			{
				TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
				FailObj->SetNumberField(TEXT("index"), i);
				FailObj->SetStringField(TEXT("source"), SourceNodeId);
				FailObj->SetStringField(TEXT("target"), TargetNodeId);
				FailObj->SetStringField(TEXT("error"), ConnectError);
				OutConnectionFailures.Add(MakeShared<FJsonValueObject>(FailObj));
			}
		}
		else
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetNumberField(TEXT("index"), i);
			FailObj->SetStringField(TEXT("error"), TEXT("Invalid source or target node ID"));
			OutConnectionFailures.Add(MakeShared<FJsonValueObject>(FailObj));
		}
	}
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteDeleteNode(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString NodeId;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeId, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Delete the node
	FString DeleteError;
	if (!FBlueprintUtils::DeleteNode(Graph, NodeId, DeleteError))
	{
		return FMCPToolResult::Error(DeleteError);
	}

	if (Context.Blueprint->DelegateSignatureGraphs.Num() > 0)
		FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
	else if (auto CompileError = Context.CompileAndFinalize(TEXT("Node deleted")))
		return CompileError.GetValue();

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("node_id"), NodeId);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Deleted node '%s'"), *NodeId),
		ResultData
	);
}

// ===== Level 4: Connection Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteConnectPins(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString SourceNodeId;
	if (!ExtractRequiredString(Params, TEXT("source_node_id"), SourceNodeId, Error))
	{
		return Error.GetValue();
	}

	FString TargetNodeId;
	if (!ExtractRequiredString(Params, TEXT("target_node_id"), TargetNodeId, Error))
	{
		return Error.GetValue();
	}

	FString SourcePin = ExtractOptionalString(Params, TEXT("source_pin"), TEXT(""));
	FString TargetPin = ExtractOptionalString(Params, TEXT("target_pin"), TEXT(""));
	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Connect the pins
	FString ConnectError;
	if (!FBlueprintUtils::ConnectPins(Graph, SourceNodeId, SourcePin, TargetNodeId, TargetPin, ConnectError))
	{
		return FMCPToolResult::Error(ConnectError);
	}

	// Compile and finalize (skip on BPs with delegate graphs — UE 5.7 crash)
	if (Context.Blueprint->DelegateSignatureGraphs.Num() > 0)
	{
		FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
	}
	else if (auto CompileError = Context.CompileAndFinalize(TEXT("Pins connected")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("source_node_id"), SourceNodeId);
	ResultData->SetStringField(TEXT("source_pin"), SourcePin.IsEmpty() ? TEXT("(auto exec)") : SourcePin);
	ResultData->SetStringField(TEXT("target_node_id"), TargetNodeId);
	ResultData->SetStringField(TEXT("target_pin"), TargetPin.IsEmpty() ? TEXT("(auto exec)") : TargetPin);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Connected '%s' -> '%s'"), *SourceNodeId, *TargetNodeId),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteDisconnectPins(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString SourceNodeId;
	if (!ExtractRequiredString(Params, TEXT("source_node_id"), SourceNodeId, Error))
	{
		return Error.GetValue();
	}

	FString SourcePin;
	if (!ExtractRequiredString(Params, TEXT("source_pin"), SourcePin, Error))
	{
		return Error.GetValue();
	}

	FString TargetNodeId;
	if (!ExtractRequiredString(Params, TEXT("target_node_id"), TargetNodeId, Error))
	{
		return Error.GetValue();
	}

	FString TargetPin;
	if (!ExtractRequiredString(Params, TEXT("target_pin"), TargetPin, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Disconnect the pins
	FString DisconnectError;
	if (!FBlueprintUtils::DisconnectPins(Graph, SourceNodeId, SourcePin, TargetNodeId, TargetPin, DisconnectError))
	{
		return FMCPToolResult::Error(DisconnectError);
	}

	if (Context.Blueprint->DelegateSignatureGraphs.Num() > 0)
		FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
	else if (auto CompileError = Context.CompileAndFinalize(TEXT("Pins disconnected")))
		return CompileError.GetValue();

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("source_node_id"), SourceNodeId);
	ResultData->SetStringField(TEXT("source_pin"), SourcePin);
	ResultData->SetStringField(TEXT("target_node_id"), TargetNodeId);
	ResultData->SetStringField(TEXT("target_pin"), TargetPin);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Disconnected '%s.%s' from '%s.%s'"), *SourceNodeId, *SourcePin, *TargetNodeId, *TargetPin),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetPinValue(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString NodeId;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeId, Error))
	{
		return Error.GetValue();
	}

	FString PinName;
	if (!ExtractRequiredString(Params, TEXT("pin_name"), PinName, Error))
	{
		return Error.GetValue();
	}

	FString PinValue;
	if (!ExtractRequiredString(Params, TEXT("pin_value"), PinValue, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Set the pin value
	FString SetError;
	if (!FBlueprintUtils::SetPinDefaultValue(Graph, NodeId, PinName, PinValue, SetError))
	{
		return FMCPToolResult::Error(SetError);
	}

	if (Context.Blueprint->DelegateSignatureGraphs.Num() > 0)
		FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
	else if (auto CompileError = Context.CompileAndFinalize(TEXT("Pin value set")))
		return CompileError.GetValue();

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("node_id"), NodeId);
	ResultData->SetStringField(TEXT("pin_name"), PinName);
	ResultData->SetStringField(TEXT("pin_value"), PinValue);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set '%s.%s' = '%s'"), *NodeId, *PinName, *PinValue),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteValidateBlueprint(const TSharedRef<FJsonObject>& Params)
{
	// Use existing compile pipeline via FMCPBlueprintLoadContext
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Compile with detailed result capture
	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);

	// Build structured result using existing pipeline (includes error_count, warning_count, compile_messages)
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("blueprint_name"), Context.Blueprint->GetName());
	ResultData->SetNumberField(TEXT("graph_count"), Context.Blueprint->UbergraphPages.Num() + Context.Blueprint->FunctionGraphs.Num());

	// Add convenience fields
	bool bHasErrors = !Context.CompileResult.bSuccess;
	bool bHasWarnings = Context.CompileResult.WarningCount > 0;
	ResultData->SetBoolField(TEXT("has_errors"), bHasErrors);
	ResultData->SetBoolField(TEXT("has_warnings"), bHasWarnings);
	ResultData->SetBoolField(TEXT("up_to_date"), !bHasErrors);
	ResultData->SetStringField(TEXT("status"), bHasErrors ? TEXT("error") : (bHasWarnings ? TEXT("warning") : TEXT("ok")));
	ResultData->SetBoolField(TEXT("diagnostics_available"), true);

	FString StatusMsg = bHasErrors
		? FString::Printf(TEXT("Blueprint '%s' has %d error(s)"), *Context.Blueprint->GetName(), Context.CompileResult.ErrorCount)
		: (bHasWarnings
			? FString::Printf(TEXT("Blueprint '%s' compiled with %d warning(s)"), *Context.Blueprint->GetName(), Context.CompileResult.WarningCount)
			: FString::Printf(TEXT("Blueprint '%s' compiled successfully"), *Context.Blueprint->GetName()));

	// Always return structured data, even on error
	FMCPToolResult Result;
	Result.bSuccess = !bHasErrors;
	Result.Message = StatusMsg;
	Result.Data = ResultData;
	return Result;
}

// ===== Repair Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteReplaceNode(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString NodeId;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeId, Error)) return Error.GetValue();

	FString NewNodeType;
	if (!ExtractRequiredString(Params, TEXT("new_node_type"), NewNodeType, Error)) return Error.GetValue();

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);

	FString GraphError;
	UEdGraph* Graph = FBlueprintGraphEditor::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError);
	if (!Graph) return FMCPToolResult::Error(GraphError);

	// Find the old node
	UEdGraphNode* OldNode = FBlueprintGraphEditor::FindNodeById(Graph, NodeId);
	if (!OldNode) return FMCPToolResult::Error(FString::Printf(TEXT("Node not found: %s"), *NodeId));

	// Capture old node's connections before deletion
	struct FPinConnection
	{
		FString PinName;
		EEdGraphPinDirection Direction;
		FString ConnectedNodeId;
		FString ConnectedPinName;
	};
	TArray<FPinConnection> OldConnections;

	for (UEdGraphPin* Pin : OldNode->Pins)
	{
		if (!Pin) continue;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;

			FPinConnection Conn;
			Conn.PinName = Pin->PinName.ToString();
			Conn.Direction = Pin->Direction;
			Conn.ConnectedNodeId = FBlueprintGraphEditor::GetNodeId(LinkedPin->GetOwningNode());
			Conn.ConnectedPinName = LinkedPin->PinName.ToString();
			OldConnections.Add(Conn);
		}
	}

	// Capture position
	int32 PosX = OldNode->NodePosX;
	int32 PosY = OldNode->NodePosY;

	// Delete old node
	FString DeleteError;
	if (!FBlueprintGraphEditor::DeleteNode(Graph, NodeId, DeleteError))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to delete old node: %s"), *DeleteError));
	}

	// Create new node at same position
	const TSharedPtr<FJsonObject>* NewNodeParamsPtr = nullptr;
	TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
	if (!Params->TryGetObjectField(TEXT("new_node_params"), NewNodeParamsPtr))
	{
		NewNodeParamsPtr = &EmptyParams;
	}

	FString NewNodeId;
	FString CreateError;
	UEdGraphNode* NewNode = FBlueprintGraphEditor::CreateNode(
		Graph, NewNodeType, NewNodeParamsPtr->ToSharedRef(), PosX, PosY, NewNodeId, CreateError);

	if (!NewNode)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Old node deleted but replacement failed: %s"), *CreateError));
	}

	// Try to reconnect compatible pins
	TArray<TSharedPtr<FJsonValue>> ReconnectedArray;
	TArray<TSharedPtr<FJsonValue>> FailedArray;

	for (const FPinConnection& Conn : OldConnections)
	{
		// Find matching pin on new node by name
		UEdGraphPin* NewPin = FBlueprintGraphEditor::FindPinByName(NewNode, Conn.PinName, Conn.Direction);
		if (!NewPin)
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("pin_name"), Conn.PinName);
			FailObj->SetStringField(TEXT("connected_node_id"), Conn.ConnectedNodeId);
			FailObj->SetStringField(TEXT("connected_pin_name"), Conn.ConnectedPinName);
			FailObj->SetStringField(TEXT("reason"), TEXT("Pin not found on new node"));
			FailedArray.Add(MakeShared<FJsonValueObject>(FailObj));
			continue;
		}

		// Try to reconnect
		FString ConnError;
		bool bConnected = false;
		if (Conn.Direction == EGPD_Output)
		{
			bConnected = FBlueprintGraphEditor::ConnectPins(Graph, NewNodeId, Conn.PinName, Conn.ConnectedNodeId, Conn.ConnectedPinName, ConnError);
		}
		else
		{
			bConnected = FBlueprintGraphEditor::ConnectPins(Graph, Conn.ConnectedNodeId, Conn.ConnectedPinName, NewNodeId, Conn.PinName, ConnError);
		}

		if (bConnected)
		{
			TSharedPtr<FJsonObject> ReconnObj = MakeShared<FJsonObject>();
			ReconnObj->SetStringField(TEXT("pin_name"), Conn.PinName);
			ReconnObj->SetStringField(TEXT("connected_node_id"), Conn.ConnectedNodeId);
			ReconnObj->SetStringField(TEXT("connected_pin_name"), Conn.ConnectedPinName);
			ReconnectedArray.Add(MakeShared<FJsonValueObject>(ReconnObj));
		}
		else
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("pin_name"), Conn.PinName);
			FailObj->SetStringField(TEXT("connected_node_id"), Conn.ConnectedNodeId);
			FailObj->SetStringField(TEXT("connected_pin_name"), Conn.ConnectedPinName);
			FailObj->SetStringField(TEXT("reason"), ConnError);
			FailedArray.Add(MakeShared<FJsonValueObject>(FailObj));
		}
	}

	// Compile
	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();

	ResultData->SetStringField(TEXT("old_node_id"), NodeId);
	ResultData->SetStringField(TEXT("new_node_id"), NewNodeId);
	ResultData->SetStringField(TEXT("new_node_type"), NewNodeType);
	ResultData->SetNumberField(TEXT("pos_x"), PosX);
	ResultData->SetNumberField(TEXT("pos_y"), PosY);
	ResultData->SetArrayField(TEXT("reconnected"), ReconnectedArray);
	ResultData->SetArrayField(TEXT("failed_connections"), FailedArray);
	ResultData->SetNumberField(TEXT("reconnected_count"), ReconnectedArray.Num());
	ResultData->SetNumberField(TEXT("failed_count"), FailedArray.Num());

	bool bCompileFailed = !Context.CompileResult.bSuccess;
	bool bHasFailedConnections = FailedArray.Num() > 0;
	bool bPartialSuccess = bCompileFailed || bHasFailedConnections;
	ResultData->SetBoolField(TEXT("partial_success"), bPartialSuccess);

	FMCPToolResult Result;
	Result.bSuccess = !bCompileFailed;
	Result.Message = bCompileFailed
		? FString::Printf(TEXT("Replaced node '%s' with '%s' (%s) but compile failed (%d error(s)). Reconnected: %d, Failed: %d. Graph needs further repair."),
			*NodeId, *NewNodeId, *NewNodeType, Context.CompileResult.ErrorCount, ReconnectedArray.Num(), FailedArray.Num())
		: FString::Printf(TEXT("Replaced node '%s' with '%s' (%s). Reconnected: %d, Failed: %d"),
			*NodeId, *NewNodeId, *NewNodeType, ReconnectedArray.Num(), FailedArray.Num());
	Result.Data = ResultData;
	return Result;
}

// ===== Authoring Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetFunctionSignature(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString FunctionName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("function_name"), FunctionName, Error)) return Error.GetValue();

	// Find the function graph
	UEdGraph* FuncGraph = nullptr;
	for (UEdGraph* Graph : Context.Blueprint->FunctionGraphs)
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

	// Find entry and result nodes
	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : FuncGraph->Nodes)
	{
		if (!EntryNode) EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
	}
	if (!EntryNode)
	{
		return FMCPToolResult::Error(TEXT("Function entry node not found"));
	}

	// Parse parameters array: [{name, type, direction}]
	const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("parameters"), ParamsArray))
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: parameters (array of {name, type, direction})"));
	}

	// Type resolution — returns false if type is unresolvable
	auto ResolveType = [](const FString& TypeStr, FEdGraphPinType& OutPinType, FString& OutError) -> bool
	{
		if (TypeStr.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		else if (TypeStr.Equals(TEXT("int32"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("int"), ESearchCase::IgnoreCase))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		}
		else if (TypeStr.Equals(TEXT("float"), ESearchCase::IgnoreCase))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutPinType.PinSubCategory = TEXT("float");
		}
		else if (TypeStr.Equals(TEXT("double"), ESearchCase::IgnoreCase))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutPinType.PinSubCategory = TEXT("double");
		}
		else if (TypeStr.Equals(TEXT("FString"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("string"), ESearchCase::IgnoreCase))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		else if (TypeStr.Equals(TEXT("FName"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("name"), ESearchCase::IgnoreCase))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		}
		else if (TypeStr.Equals(TEXT("FVector"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		}
		else if (TypeStr.Equals(TEXT("FRotator"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		}
		else if (TypeStr.Equals(TEXT("FTransform"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Transform"), ESearchCase::IgnoreCase))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		}
		else
		{
			// Try as object type — delegate to ParsePinType for broad resolution
			FString ObjTypeStr = TypeStr;
			if (!ObjTypeStr.EndsWith(TEXT("*"))) ObjTypeStr += TEXT("*");
			FEdGraphPinType TempType;
			FString ParseError;
			if (FBlueprintUtils::ParsePinType(ObjTypeStr, TempType, ParseError))
			{
				OutPinType = TempType;
			}
			else
			{
				OutError = FString::Printf(TEXT("Cannot resolve type '%s': %s"), *TypeStr, *ParseError);
				return false;
			}
		}
		return true;
	};

	// Pre-validate all requested types before mutating anything
	struct FPendingParam
	{
		FString Name;
		FString TypeStr;
		FEdGraphPinType PinType;
		bool bIsOutput;
	};
	TArray<FPendingParam> PendingParams;
	TArray<TSharedPtr<FJsonValue>> TypeFailures;

	for (const TSharedPtr<FJsonValue>& ParamVal : *ParamsArray)
	{
		const TSharedPtr<FJsonObject>* ParamObj;
		if (!ParamVal->TryGetObject(ParamObj)) continue;

		FString ParamName, ParamType, Direction;
		(*ParamObj)->TryGetStringField(TEXT("name"), ParamName);
		(*ParamObj)->TryGetStringField(TEXT("type"), ParamType);
		(*ParamObj)->TryGetStringField(TEXT("direction"), Direction);

		if (ParamName.IsEmpty() || ParamType.IsEmpty()) continue;

		FEdGraphPinType PinType;
		FString TypeError;
		if (!ResolveType(ParamType, PinType, TypeError))
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("name"), ParamName);
			FailObj->SetStringField(TEXT("type"), ParamType);
			FailObj->SetStringField(TEXT("error"), TypeError);
			TypeFailures.Add(MakeShared<FJsonValueObject>(FailObj));
			continue;
		}

		FPendingParam Pending;
		Pending.Name = ParamName;
		Pending.TypeStr = ParamType;
		Pending.PinType = PinType;
		Pending.bIsOutput = Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase) || Direction.Equals(TEXT("return"), ESearchCase::IgnoreCase);
		PendingParams.Add(Pending);
	}

	// Hard error if any types failed to resolve
	if (TypeFailures.Num() > 0)
	{
		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("function_name"), FunctionName);
		ResultData->SetArrayField(TEXT("type_resolution_failures"), TypeFailures);
		ResultData->SetNumberField(TEXT("failure_count"), TypeFailures.Num());

		FMCPToolResult Result;
		Result.bSuccess = false;
		Result.Message = FString::Printf(TEXT("Cannot set signature: %d type(s) failed to resolve"), TypeFailures.Num());
		Result.Data = ResultData;
		return Result;
	}

	// Clear existing user-defined pins (SET semantics, not append)
	{
		// Collect names first to avoid modifying array during iteration
		TArray<FName> EntryPinNames;
		for (UEdGraphPin* Pin : EntryNode->Pins)
		{
			if (!Pin) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->PinName == TEXT("self")) continue;
			if (Pin->Direction == EGPD_Output)
			{
				EntryPinNames.Add(Pin->PinName);
			}
		}
		for (const FName& PinName : EntryPinNames)
		{
			EntryNode->RemoveUserDefinedPinByName(PinName);
		}

		if (ResultNode)
		{
			TArray<FName> ResultPinNames;
			for (UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (!Pin) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->Direction == EGPD_Input)
				{
					ResultPinNames.Add(Pin->PinName);
				}
			}
			for (const FName& PinName : ResultPinNames)
			{
				ResultNode->RemoveUserDefinedPinByName(PinName);
			}
		}
	}

	// Now add the validated parameters
	TArray<TSharedPtr<FJsonValue>> AddedParams;
	int32 InputCount = 0, OutputCount = 0;

	for (const FPendingParam& Pending : PendingParams)
	{
		if (Pending.bIsOutput)
		{
			// Create result node if needed
			if (!ResultNode)
			{
				ResultNode = NewObject<UK2Node_FunctionResult>(FuncGraph);
				ResultNode->CreateNewGuid();
				ResultNode->PostPlacedNewNode();
				ResultNode->AllocateDefaultPins();
				ResultNode->NodePosX = EntryNode->NodePosX + 600;
				ResultNode->NodePosY = EntryNode->NodePosY;
				FuncGraph->AddNode(ResultNode);
			}

			FName PinName(*Pending.Name);
			ResultNode->CreateUserDefinedPin(PinName, Pending.PinType, EGPD_Input);
			OutputCount++;
		}
		else
		{
			FName PinName(*Pending.Name);
			EntryNode->CreateUserDefinedPin(PinName, Pending.PinType, EGPD_Output);
			InputCount++;
		}

		TSharedPtr<FJsonObject> Added = MakeShared<FJsonObject>();
		Added->SetStringField(TEXT("name"), Pending.Name);
		Added->SetStringField(TEXT("type"), Pending.TypeStr);
		Added->SetStringField(TEXT("direction"), Pending.bIsOutput ? TEXT("output") : TEXT("input"));
		AddedParams.Add(MakeShared<FJsonValueObject>(Added));
	}

	// Compile
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Function signature set")))
	{
		// Still return data even on compile failure
		TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
		ResultData->SetStringField(TEXT("function_name"), FunctionName);
		ResultData->SetArrayField(TEXT("parameters"), AddedParams);
		ResultData->SetNumberField(TEXT("input_count"), InputCount);
		ResultData->SetNumberField(TEXT("output_count"), OutputCount);

		FMCPToolResult Result;
		Result.bSuccess = false;
		Result.Message = FString::Printf(TEXT("Set signature on '%s' (%d inputs, %d outputs) but compile failed"), *FunctionName, InputCount, OutputCount);
		Result.Data = ResultData;
		return Result;
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("function_name"), FunctionName);
	ResultData->SetArrayField(TEXT("parameters"), AddedParams);
	ResultData->SetNumberField(TEXT("input_count"), InputCount);
	ResultData->SetNumberField(TEXT("output_count"), OutputCount);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set function '%s' signature: %d inputs, %d outputs"), *FunctionName, InputCount, OutputCount),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddInterface(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString InterfacePath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("interface_path"), InterfacePath, Error)) return Error.GetValue();

	// Load the interface Blueprint
	UBlueprint* InterfaceBP = LoadObject<UBlueprint>(nullptr, *InterfacePath);
	if (!InterfaceBP)
	{
		// Try with /Script/ prefix for native interfaces
		UClass* InterfaceClass = FindObject<UClass>(nullptr, *InterfacePath);
		if (!InterfaceClass)
		{
			InterfaceClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *InterfacePath));
		}
		if (!InterfaceClass)
		{
			InterfaceClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/UMG.%s"), *InterfacePath));
		}
		if (InterfaceClass && InterfaceClass->IsChildOf(UInterface::StaticClass()))
		{
			// Native interface — add via ImplementedInterfaces
			FBlueprintEditorUtils::ImplementNewInterface(Context.Blueprint, FTopLevelAssetPath(InterfaceClass->GetPathName()));

			if (auto CompileError = Context.CompileAndFinalize(TEXT("Interface added")))
			{
				TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
				ResultData->SetStringField(TEXT("interface"), InterfacePath);
				FMCPToolResult Result;
				Result.bSuccess = false;
				Result.Message = FString::Printf(TEXT("Added interface '%s' but compile failed"), *InterfacePath);
				Result.Data = ResultData;
				return Result;
			}

			TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
			ResultData->SetStringField(TEXT("interface"), InterfacePath);
			ResultData->SetStringField(TEXT("interface_type"), TEXT("native"));

			return FMCPToolResult::Success(
				FString::Printf(TEXT("Implemented native interface '%s'"), *InterfacePath),
				ResultData
			);
		}

		return FMCPToolResult::Error(FString::Printf(TEXT("Interface not found: '%s'. Provide full path (e.g., '/Game/Interfaces/BPI_Interactable') or native class name"), *InterfacePath));
	}

	// Blueprint interface
	if (InterfaceBP->BlueprintType != BPTYPE_Interface)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("'%s' is not a Blueprint Interface"), *InterfacePath));
	}

	FBlueprintEditorUtils::ImplementNewInterface(Context.Blueprint, FTopLevelAssetPath(InterfaceBP->GeneratedClass->GetPathName()));

	if (auto CompileError = Context.CompileAndFinalize(TEXT("Interface added")))
	{
		TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
		ResultData->SetStringField(TEXT("interface"), InterfacePath);
		FMCPToolResult Result;
		Result.bSuccess = false;
		Result.Message = FString::Printf(TEXT("Added interface '%s' but compile failed"), *InterfacePath);
		Result.Data = ResultData;
		return Result;
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("interface"), InterfacePath);
	ResultData->SetStringField(TEXT("interface_type"), TEXT("blueprint"));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Implemented Blueprint interface '%s'"), *InterfacePath),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveInterface(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString InterfacePath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("interface_path"), InterfacePath, Error)) return Error.GetValue();

	// Find the interface in implemented list
	bool bFound = false;
	for (int32 i = Context.Blueprint->ImplementedInterfaces.Num() - 1; i >= 0; --i)
	{
		FBPInterfaceDescription& Desc = Context.Blueprint->ImplementedInterfaces[i];
		if (Desc.Interface && Desc.Interface->GetPathName().Contains(InterfacePath))
		{
			FBlueprintEditorUtils::RemoveInterface(Context.Blueprint, FTopLevelAssetPath(Desc.Interface->GetPathName()));
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Interface '%s' not found on this Blueprint"), *InterfacePath));
	}

	if (auto CompileError = Context.CompileAndFinalize(TEXT("Interface removed")))
	{
		TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
		ResultData->SetStringField(TEXT("interface"), InterfacePath);
		FMCPToolResult Result;
		Result.bSuccess = false;
		Result.Message = FString::Printf(TEXT("Removed interface '%s' but compile failed"), *InterfacePath);
		Result.Data = ResultData;
		return Result;
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("interface"), InterfacePath);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed interface '%s'"), *InterfacePath),
		ResultData
	);
}

// ===== Shared Nested Property Navigation =====

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

	Result.ErrorMessage = TEXT("Empty property path");
	return Result;
}

// ===== Class Defaults Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetClassDefault(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString PropertyName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("property_name"), PropertyName, Error)) return Error.GetValue();

	FString PropertyValue;
	if (!ExtractRequiredString(Params, TEXT("property_value"), PropertyValue, Error)) return Error.GetValue();

	UClass* GenClass = Context.Blueprint->GeneratedClass;
	if (!GenClass) return FMCPToolResult::Error(TEXT("Blueprint has no GeneratedClass — compile it first"));

	UObject* CDO = GenClass->GetDefaultObject();
	if (!CDO) return FMCPToolResult::Error(TEXT("Could not get Class Default Object"));

	FPropertyPathResult PathResult = NavigatePropertyPath(GenClass, CDO, PropertyName);
	if (!PathResult.bSuccess)
		return FMCPToolResult::Error(PathResult.ErrorMessage);

	FProperty* Prop = PathResult.Prop;
	void* ValuePtr = PathResult.ValuePtr;

	// Export old value
	FString OldValue;
	Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, CDO, PPF_None);

	// Import new value
	if (!Prop->ImportText_Direct(*PropertyValue, ValuePtr, CDO, PPF_None))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to parse value '%s' for property '%s' (type: %s)"), *PropertyValue, *PropertyName, *Prop->GetCPPType()));
	}

	// CDO changes don't require recompile — just mark dirty and propagate
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
	CDO->PostEditChange();

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("property_name"), PropertyName);
	ResultData->SetStringField(TEXT("property_type"), Prop->GetCPPType());
	ResultData->SetStringField(TEXT("old_value"), OldValue);
	ResultData->SetStringField(TEXT("new_value"), PropertyValue);
	bool bIsCollection = CastField<FArrayProperty>(Prop) || CastField<FSetProperty>(Prop) || CastField<FMapProperty>(Prop);
	if (bIsCollection) ResultData->SetStringField(TEXT("write_mode"), TEXT("replace_whole_value"));
	ResultData->SetStringField(TEXT("status"), TEXT("changed"));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set default '%s' = '%s' (was '%s')"), *PropertyName, *PropertyValue, *OldValue),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetClassDefaults(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	const TArray<TSharedPtr<FJsonValue>>* PropsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("properties"), PropsArray))
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: properties (array of {name, value})"));
	}

	bool bDryRun = ExtractOptionalBool(Params, TEXT("dry_run"), false);

	UClass* GenClass = Context.Blueprint->GeneratedClass;
	if (!GenClass) return FMCPToolResult::Error(TEXT("Blueprint has no GeneratedClass"));

	UObject* CDO = GenClass->GetDefaultObject();
	if (!CDO) return FMCPToolResult::Error(TEXT("Could not get Class Default Object"));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ChangedArr, FailedArr, UnchangedArr;

	for (const TSharedPtr<FJsonValue>& PropVal : *PropsArray)
	{
		const TSharedPtr<FJsonObject>* PropObj;
		if (!PropVal->TryGetObject(PropObj)) continue;

		FString Name, Value;
		(*PropObj)->TryGetStringField(TEXT("name"), Name);
		(*PropObj)->TryGetStringField(TEXT("value"), Value);
		if (Name.IsEmpty()) continue;

		// Navigate nested property paths (e.g., "Transform.Location.X")
		FPropertyPathResult PathResult = NavigatePropertyPath(GenClass, CDO, Name);
		if (!PathResult.bSuccess)
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("name"), Name);
			FailObj->SetStringField(TEXT("status"), bDryRun ? TEXT("would_fail") : TEXT("failed"));
			FailObj->SetStringField(TEXT("reason"), PathResult.ErrorMessage);
			FailedArr.Add(MakeShared<FJsonValueObject>(FailObj));
			continue;
		}

		FProperty* Prop = PathResult.Prop;
		void* ValuePtr = PathResult.ValuePtr;

		FString OldValue;
		Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, CDO, PPF_None);

		if (OldValue == Value)
		{
			TSharedPtr<FJsonObject> UncObj = MakeShared<FJsonObject>();
			UncObj->SetStringField(TEXT("name"), Name);
			UncObj->SetStringField(TEXT("status"), TEXT("unchanged"));
			UncObj->SetStringField(TEXT("value"), OldValue);
			UnchangedArr.Add(MakeShared<FJsonValueObject>(UncObj));
			continue;
		}

		if (bDryRun)
		{
			// Validate parseability: try import into a temp buffer to check if value is valid
			TArray<uint8> TempBuffer;
			TempBuffer.SetNumZeroed(Prop->GetSize());
			Prop->InitializeValue(TempBuffer.GetData());
			bool bParseable = Prop->ImportText_Direct(*Value, TempBuffer.GetData(), CDO, PPF_None) != nullptr;
			Prop->DestroyValue(TempBuffer.GetData());

			if (!bParseable)
			{
				TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
				FailObj->SetStringField(TEXT("name"), Name);
				FailObj->SetStringField(TEXT("status"), TEXT("would_fail"));
				FailObj->SetStringField(TEXT("reason"), FString::Printf(TEXT("cannot parse '%s' for type %s"), *Value, *Prop->GetCPPType()));
				FailedArr.Add(MakeShared<FJsonValueObject>(FailObj));
				continue;
			}

			TSharedPtr<FJsonObject> ChgObj = MakeShared<FJsonObject>();
			ChgObj->SetStringField(TEXT("name"), Name);
			ChgObj->SetStringField(TEXT("status"), TEXT("would_change"));
			ChgObj->SetStringField(TEXT("current_value"), OldValue);
			ChgObj->SetStringField(TEXT("desired_value"), Value);
			ChangedArr.Add(MakeShared<FJsonValueObject>(ChgObj));
			continue;
		}

		if (!Prop->ImportText_Direct(*Value, ValuePtr, CDO, PPF_None))
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("name"), Name);
			FailObj->SetStringField(TEXT("status"), TEXT("failed"));
			FailObj->SetStringField(TEXT("reason"), FString::Printf(TEXT("failed to parse '%s' for type %s"), *Value, *Prop->GetCPPType()));
			FailedArr.Add(MakeShared<FJsonValueObject>(FailObj));
			continue;
		}

		TSharedPtr<FJsonObject> ChgObj = MakeShared<FJsonObject>();
		ChgObj->SetStringField(TEXT("name"), Name);
		ChgObj->SetStringField(TEXT("status"), TEXT("changed"));
		ChgObj->SetStringField(TEXT("old_value"), OldValue);
		ChgObj->SetStringField(TEXT("new_value"), Value);
		ChangedArr.Add(MakeShared<FJsonValueObject>(ChgObj));
	}

	// CDO changes don't require recompile — just mark dirty and propagate
	if (!bDryRun)
	{
		FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
		CDO->PostEditChange();
	}

	bool bHasFailures = FailedArr.Num() > 0;
	ResultData->SetBoolField(TEXT("dry_run"), bDryRun);
	ResultData->SetArrayField(TEXT("changed"), ChangedArr);
	ResultData->SetArrayField(TEXT("unchanged"), UnchangedArr);
	ResultData->SetArrayField(TEXT("failed"), FailedArr);
	ResultData->SetNumberField(TEXT("changed_count"), ChangedArr.Num());
	ResultData->SetNumberField(TEXT("unchanged_count"), UnchangedArr.Num());
	ResultData->SetNumberField(TEXT("failed_count"), FailedArr.Num());
	if (bHasFailures) ResultData->SetBoolField(TEXT("partial_success"), true);

	FMCPToolResult Result;
	Result.bSuccess = !bHasFailures;
	Result.Message = FString::Printf(TEXT("%s class defaults: %d %s, %d unchanged, %d %s"),
		bDryRun ? TEXT("Preflight") : TEXT("Set"),
		ChangedArr.Num(), bDryRun ? TEXT("would_change") : TEXT("changed"),
		UnchangedArr.Num(),
		FailedArr.Num(), bDryRun ? TEXT("would_fail") : TEXT("failed"));
	Result.Data = ResultData;
	return Result;
}

// ===== Collection Element Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteModifyCollection(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString PropertyName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("property_name"), PropertyName, Error)) return Error.GetValue();

	FString Action;
	if (!ExtractRequiredString(Params, TEXT("action"), Action, Error)) return Error.GetValue();
	Action = Action.ToLower();

	UClass* GenClass = Context.Blueprint->GeneratedClass;
	if (!GenClass) return FMCPToolResult::Error(TEXT("Blueprint has no GeneratedClass — compile it first"));

	UObject* CDO = GenClass->GetDefaultObject();
	if (!CDO) return FMCPToolResult::Error(TEXT("Could not get Class Default Object"));

	FPropertyPathResult PathResult = NavigatePropertyPath(GenClass, CDO, PropertyName);
	if (!PathResult.bSuccess) return FMCPToolResult::Error(PathResult.ErrorMessage);

	FProperty* Prop = PathResult.Prop;
	void* ValuePtr = PathResult.ValuePtr;

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("property_name"), PropertyName);
	ResultData->SetStringField(TEXT("action"), Action);

	// TArray operations
	if (FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper ArrHelper(ArrProp, ValuePtr);
		ResultData->SetStringField(TEXT("collection_type"), TEXT("TArray"));
		ResultData->SetNumberField(TEXT("count_before"), ArrHelper.Num());

		if (Action == TEXT("append"))
		{
			FString Value;
			if (!ExtractRequiredString(Params, TEXT("property_value"), Value, Error)) return Error.GetValue();
			int32 NewIdx = ArrHelper.AddValue();
			void* ElemPtr = ArrHelper.GetRawPtr(NewIdx);
			if (!ArrProp->Inner->ImportText_Direct(*Value, ElemPtr, CDO, PPF_None))
			{
				ArrHelper.RemoveValues(NewIdx, 1);
				return FMCPToolResult::Error(FString::Printf(TEXT("Failed to parse '%s' for element type %s"), *Value, *ArrProp->Inner->GetCPPType()));
			}
			ResultData->SetNumberField(TEXT("index"), NewIdx);
			ResultData->SetStringField(TEXT("value"), Value);
		}
		else if (Action == TEXT("insert"))
		{
			FString Value;
			if (!ExtractRequiredString(Params, TEXT("property_value"), Value, Error)) return Error.GetValue();
			int32 Index = ExtractOptionalNumber<int32>(Params, TEXT("index"), 0);
			if (Index < 0 || Index > ArrHelper.Num())
				return FMCPToolResult::Error(FString::Printf(TEXT("Index %d out of range [0..%d]"), Index, ArrHelper.Num()));
			ArrHelper.InsertValues(Index, 1);
			void* ElemPtr = ArrHelper.GetRawPtr(Index);
			if (!ArrProp->Inner->ImportText_Direct(*Value, ElemPtr, CDO, PPF_None))
			{
				ArrHelper.RemoveValues(Index, 1);
				return FMCPToolResult::Error(FString::Printf(TEXT("Failed to parse '%s' for element type %s"), *Value, *ArrProp->Inner->GetCPPType()));
			}
			ResultData->SetNumberField(TEXT("index"), Index);
			ResultData->SetStringField(TEXT("value"), Value);
		}
		else if (Action == TEXT("remove_at"))
		{
			int32 Index = ExtractOptionalNumber<int32>(Params, TEXT("index"), -1);
			if (Index < 0 || Index >= ArrHelper.Num())
				return FMCPToolResult::Error(FString::Printf(TEXT("Index %d out of range [0..%d)"), Index, ArrHelper.Num()));
			FString OldValue;
			ArrProp->Inner->ExportText_Direct(OldValue, ArrHelper.GetRawPtr(Index), ArrHelper.GetRawPtr(Index), CDO, PPF_None);
			ArrHelper.RemoveValues(Index, 1);
			ResultData->SetNumberField(TEXT("index"), Index);
			ResultData->SetStringField(TEXT("removed_value"), OldValue);
		}
		else if (Action == TEXT("set_at"))
		{
			FString Value;
			if (!ExtractRequiredString(Params, TEXT("property_value"), Value, Error)) return Error.GetValue();
			int32 Index = ExtractOptionalNumber<int32>(Params, TEXT("index"), -1);
			if (Index < 0 || Index >= ArrHelper.Num())
				return FMCPToolResult::Error(FString::Printf(TEXT("Index %d out of range [0..%d)"), Index, ArrHelper.Num()));
			void* ElemPtr = ArrHelper.GetRawPtr(Index);
			FString OldValue;
			ArrProp->Inner->ExportText_Direct(OldValue, ElemPtr, ElemPtr, CDO, PPF_None);
			if (!ArrProp->Inner->ImportText_Direct(*Value, ElemPtr, CDO, PPF_None))
				return FMCPToolResult::Error(FString::Printf(TEXT("Failed to parse '%s' for element type %s"), *Value, *ArrProp->Inner->GetCPPType()));
			ResultData->SetNumberField(TEXT("index"), Index);
			ResultData->SetStringField(TEXT("old_value"), OldValue);
			ResultData->SetStringField(TEXT("new_value"), Value);
		}
		else if (Action == TEXT("clear"))
		{
			ArrHelper.EmptyValues();
		}
		else
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Unknown TArray action '%s'. Valid: append, insert, remove_at, set_at, clear"), *Action));
		}

		ResultData->SetNumberField(TEXT("count_after"), ArrHelper.Num());
	}
	// TMap operations
	else if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		FScriptMapHelper MapHelper(MapProp, ValuePtr);
		ResultData->SetStringField(TEXT("collection_type"), TEXT("TMap"));
		ResultData->SetNumberField(TEXT("count_before"), MapHelper.Num());

		if (Action == TEXT("put"))
		{
			FString Key, Value;
			if (!ExtractRequiredString(Params, TEXT("key"), Key, Error)) return Error.GetValue();
			if (!ExtractRequiredString(Params, TEXT("property_value"), Value, Error)) return Error.GetValue();

			int32 NewIdx = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
			void* KeyPtr = MapHelper.GetKeyPtr(NewIdx);
			void* ValPtr = MapHelper.GetValuePtr(NewIdx);

			if (!MapProp->KeyProp->ImportText_Direct(*Key, KeyPtr, CDO, PPF_None))
			{
				MapHelper.RemoveAt(NewIdx);
				MapHelper.Rehash();
				return FMCPToolResult::Error(FString::Printf(TEXT("Failed to parse key '%s'"), *Key));
			}
			if (!MapProp->ValueProp->ImportText_Direct(*Value, ValPtr, CDO, PPF_None))
			{
				MapHelper.RemoveAt(NewIdx);
				MapHelper.Rehash();
				return FMCPToolResult::Error(FString::Printf(TEXT("Failed to parse value '%s'"), *Value));
			}
			MapHelper.Rehash();
			ResultData->SetStringField(TEXT("key"), Key);
			ResultData->SetStringField(TEXT("value"), Value);
		}
		else if (Action == TEXT("remove"))
		{
			FString Key;
			if (!ExtractRequiredString(Params, TEXT("key"), Key, Error)) return Error.GetValue();

			// Find and remove key by scanning entries
			bool bFound = false;
			for (int32 i = 0; i < MapHelper.GetMaxIndex(); i++)
			{
				if (!MapHelper.IsValidIndex(i)) continue;
				FString ExportedKey;
				MapProp->KeyProp->ExportText_Direct(ExportedKey, MapHelper.GetKeyPtr(i), MapHelper.GetKeyPtr(i), CDO, PPF_None);
				if (ExportedKey == Key)
				{
					MapHelper.RemoveAt(i);
					MapHelper.Rehash();
					bFound = true;
					break;
				}
			}
			if (!bFound) return FMCPToolResult::Error(FString::Printf(TEXT("Key '%s' not found in map"), *Key));
			ResultData->SetStringField(TEXT("removed_key"), Key);
		}
		else if (Action == TEXT("clear"))
		{
			MapHelper.EmptyValues();
		}
		else
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Unknown TMap action '%s'. Valid: put, remove, clear"), *Action));
		}

		ResultData->SetNumberField(TEXT("count_after"), MapHelper.Num());
	}
	// TSet operations
	else if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		FScriptSetHelper SetHelper(SetProp, ValuePtr);
		ResultData->SetStringField(TEXT("collection_type"), TEXT("TSet"));
		ResultData->SetNumberField(TEXT("count_before"), SetHelper.Num());

		if (Action == TEXT("add"))
		{
			FString Value;
			if (!ExtractRequiredString(Params, TEXT("property_value"), Value, Error)) return Error.GetValue();
			int32 NewIdx = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
			void* ElemPtr = SetHelper.GetElementPtr(NewIdx);
			if (!SetProp->ElementProp->ImportText_Direct(*Value, ElemPtr, CDO, PPF_None))
			{
				SetHelper.RemoveAt(NewIdx);
				SetHelper.Rehash();
				return FMCPToolResult::Error(FString::Printf(TEXT("Failed to parse '%s' for set element"), *Value));
			}
			SetHelper.Rehash();
			ResultData->SetStringField(TEXT("value"), Value);
		}
		else if (Action == TEXT("remove"))
		{
			FString Value;
			if (!ExtractRequiredString(Params, TEXT("property_value"), Value, Error)) return Error.GetValue();
			bool bFound = false;
			for (int32 i = 0; i < SetHelper.GetMaxIndex(); i++)
			{
				if (!SetHelper.IsValidIndex(i)) continue;
				FString ExportedVal;
				SetProp->ElementProp->ExportText_Direct(ExportedVal, SetHelper.GetElementPtr(i), SetHelper.GetElementPtr(i), CDO, PPF_None);
				if (ExportedVal == Value)
				{
					SetHelper.RemoveAt(i);
					SetHelper.Rehash();
					bFound = true;
					break;
				}
			}
			if (!bFound) return FMCPToolResult::Error(FString::Printf(TEXT("Value '%s' not found in set"), *Value));
			ResultData->SetStringField(TEXT("removed_value"), Value);
		}
		else if (Action == TEXT("clear"))
		{
			SetHelper.EmptyElements();
		}
		else
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Unknown TSet action '%s'. Valid: add, remove, clear"), *Action));
		}

		ResultData->SetNumberField(TEXT("count_after"), SetHelper.Num());
	}
	else
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Property '%s' (type: %s) is not a collection. Use set_class_default instead."), *PropertyName, *Prop->GetCPPType()));
	}

	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
	CDO->PostEditChange();

	ResultData->SetStringField(TEXT("status"), TEXT("changed"));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Collection '%s': %s completed"), *PropertyName, *Action),
		ResultData
	);
}

// ===== Animation Integration =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetAnimBlueprint(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString AnimBPPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("anim_blueprint_path"), AnimBPPath, Error)) return Error.GetValue();

	FString ComponentName = ExtractOptionalString(Params, TEXT("component_name"));

	// Load AnimBP
	UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AnimBPPath);
	if (!AnimBP)
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load AnimBlueprint: %s"), *AnimBPPath));

	if (!AnimBP->GeneratedClass)
		return FMCPToolResult::Error(TEXT("AnimBlueprint has no GeneratedClass — compile it first"));

	// Find SkeletalMeshComponent on the BP
	USimpleConstructionScript* SCS = Context.Blueprint->SimpleConstructionScript;
	if (!SCS)
		return FMCPToolResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));

	USCS_Node* MeshNode = nullptr;
	if (!ComponentName.IsEmpty())
	{
		MeshNode = SCS->FindSCSNode(FName(*ComponentName));
	}
	else
	{
		// Auto-find first SkeletalMeshComponent
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf(USkeletalMeshComponent::StaticClass()))
			{
				MeshNode = Node;
				break;
			}
		}
	}

	if (!MeshNode || !MeshNode->ComponentTemplate)
		return FMCPToolResult::Error(TEXT("No SkeletalMeshComponent found on Blueprint. Add one first or specify component_name."));

	USkeletalMeshComponent* MeshComp = Cast<USkeletalMeshComponent>(MeshNode->ComponentTemplate);
	if (!MeshComp)
		return FMCPToolResult::Error(TEXT("Component is not a SkeletalMeshComponent"));

	// Get old value
	FString OldAnimClass = MeshComp->AnimClass ? MeshComp->AnimClass->GetPathName() : TEXT("None");

	// Set the AnimClass
	MeshComp->SetAnimInstanceClass(AnimBP->GeneratedClass);

	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
	ResultData->SetStringField(TEXT("component_name"), MeshNode->GetVariableName().ToString());
	ResultData->SetStringField(TEXT("old_anim_class"), OldAnimClass);
	ResultData->SetStringField(TEXT("new_anim_class"), AnimBP->GeneratedClass->GetPathName());
	ResultData->SetStringField(TEXT("anim_blueprint"), AnimBP->GetPathName());
	ResultData->SetStringField(TEXT("status"), TEXT("assigned"));

	if (AnimBP->TargetSkeleton)
	{
		ResultData->SetStringField(TEXT("target_skeleton"), AnimBP->TargetSkeleton->GetName());
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Assigned AnimBP '%s' to component '%s'"), *AnimBP->GetName(), *MeshNode->GetVariableName().ToString()),
		ResultData
	);
}

// ===== Data Asset Property Setting =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetDataAssetProperties(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), AssetPath, Error)) return Error.GetValue();

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset) return FMCPToolResult::Error(FString::Printf(TEXT("Could not load asset: %s"), *AssetPath));
	if (!Asset->IsA<UDataAsset>()) return FMCPToolResult::Error(TEXT("Asset is not a DataAsset"));

	const TArray<TSharedPtr<FJsonValue>>* PropsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("properties"), PropsArray))
		return FMCPToolResult::Error(TEXT("Missing required parameter: properties"));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ChangedArr, UnchangedArr, FailedArr;

	for (const TSharedPtr<FJsonValue>& PropVal : *PropsArray)
	{
		const TSharedPtr<FJsonObject>* PropObj; if (!PropVal->TryGetObject(PropObj)) continue;
		FString Name, Value;
		(*PropObj)->TryGetStringField(TEXT("name"), Name);
		(*PropObj)->TryGetStringField(TEXT("value"), Value);
		if (Name.IsEmpty()) continue;

		// Navigate nested property paths
		FPropertyPathResult PathResult = NavigatePropertyPath(Asset->GetClass(), Asset, Name);
		if (!PathResult.bSuccess) { TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>(); F->SetStringField(TEXT("name"),Name); F->SetStringField(TEXT("status"),TEXT("failed")); F->SetStringField(TEXT("reason"),PathResult.ErrorMessage); FailedArr.Add(MakeShared<FJsonValueObject>(F)); continue; }

		FProperty* Prop = PathResult.Prop;
		void* ValuePtr = PathResult.ValuePtr;
		FString OldValue; Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, Asset, PPF_None);

		if (OldValue == Value) { TSharedPtr<FJsonObject> U = MakeShared<FJsonObject>(); U->SetStringField(TEXT("name"),Name); U->SetStringField(TEXT("status"),TEXT("unchanged")); UnchangedArr.Add(MakeShared<FJsonValueObject>(U)); continue; }

		if (!Prop->ImportText_Direct(*Value, ValuePtr, Asset, PPF_None)) { TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>(); F->SetStringField(TEXT("name"),Name); F->SetStringField(TEXT("status"),TEXT("failed")); F->SetStringField(TEXT("reason"),FString::Printf(TEXT("parse failed for %s"),*Prop->GetCPPType())); FailedArr.Add(MakeShared<FJsonValueObject>(F)); continue; }

		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>(); C->SetStringField(TEXT("name"),Name); C->SetStringField(TEXT("status"),TEXT("changed")); C->SetStringField(TEXT("old_value"),OldValue); C->SetStringField(TEXT("new_value"),Value);
		ChangedArr.Add(MakeShared<FJsonValueObject>(C));
	}

	Asset->MarkPackageDirty();
	Asset->PostEditChange();

	// Emit receipt
	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("blueprint_modify:set_data_asset_properties");
	Receipt.bSuccess = FailedArr.Num() == 0;
	Receipt.TargetType = TEXT("asset");
	Receipt.Targets.Add(AssetPath);
	Receipt.Classification = TEXT("user_mutation");
	Receipt.Status = Receipt.bSuccess ? TEXT("success") : TEXT("partial_success");
	Receipt.Summary = FString::Printf(TEXT("DataAsset: %d changed, %d failed"), ChangedArr.Num(), FailedArr.Num());
	FUnrealClaudeExecutionLog::Get().AddReceipt(Receipt);

	bool bHasFailures = FailedArr.Num() > 0;
	ResultData->SetArrayField(TEXT("changed"), ChangedArr);
	ResultData->SetArrayField(TEXT("unchanged"), UnchangedArr);
	ResultData->SetArrayField(TEXT("failed"), FailedArr);
	if (bHasFailures) ResultData->SetBoolField(TEXT("partial_success"), true);

	FMCPToolResult Result;
	Result.bSuccess = !bHasFailures;
	Result.Message = FString::Printf(TEXT("DataAsset: %d changed, %d unchanged, %d failed"), ChangedArr.Num(), UnchangedArr.Num(), FailedArr.Num());
	Result.Data = ResultData;
	return Result;
}

// ===== DataTable Mutation =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetDataTableRow(const TSharedRef<FJsonObject>& Params)
{
	FString TablePath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), TablePath, Error)) return Error.GetValue();

	FString RowName;
	if (!ExtractRequiredString(Params, TEXT("row_name"), RowName, Error)) return Error.GetValue();

	UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
	if (!Table) return FMCPToolResult::Error(FString::Printf(TEXT("Could not load DataTable: %s"), *TablePath));
	if (!Table->RowStruct) return FMCPToolResult::Error(TEXT("DataTable has no RowStruct"));

	// Find or add row
	uint8* RowData = Table->FindRowUnchecked(FName(*RowName));
	bool bNewRow = (RowData == nullptr);

	if (bNewRow)
	{
		// Add new row with default-constructed struct data
		FTableRowBase DefaultRow;
		Table->AddRow(FName(*RowName), DefaultRow);
		RowData = Table->FindRowUnchecked(FName(*RowName));
		if (!RowData) return FMCPToolResult::Error(TEXT("Failed to add row"));
	}

	// Set fields from properties array
	const TArray<TSharedPtr<FJsonValue>>* PropsArray = nullptr;
	TArray<TSharedPtr<FJsonValue>> ChangedArr, FailedArr;

	if (Params->TryGetArrayField(TEXT("properties"), PropsArray))
	{
		for (const TSharedPtr<FJsonValue>& PropVal : *PropsArray)
		{
			const TSharedPtr<FJsonObject>* PropObj; if (!PropVal->TryGetObject(PropObj)) continue;
			FString Name, Value;
			(*PropObj)->TryGetStringField(TEXT("name"), Name);
			(*PropObj)->TryGetStringField(TEXT("value"), Value);
			if (Name.IsEmpty()) continue;

			FProperty* Prop = Table->RowStruct->FindPropertyByName(FName(*Name));
			if (!Prop) { TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>(); F->SetStringField(TEXT("name"),Name); F->SetStringField(TEXT("status"),TEXT("failed")); F->SetStringField(TEXT("reason"),TEXT("column not found")); FailedArr.Add(MakeShared<FJsonValueObject>(F)); continue; }

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
			if (!Prop->ImportText_Direct(*Value, ValuePtr, nullptr, PPF_None)) { TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>(); F->SetStringField(TEXT("name"),Name); F->SetStringField(TEXT("status"),TEXT("failed")); F->SetStringField(TEXT("reason"),TEXT("parse failed")); FailedArr.Add(MakeShared<FJsonValueObject>(F)); continue; }

			TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>(); C->SetStringField(TEXT("name"),Name); C->SetStringField(TEXT("status"),TEXT("changed")); C->SetStringField(TEXT("new_value"),Value);
			ChangedArr.Add(MakeShared<FJsonValueObject>(C));
		}
	}

	Table->MarkPackageDirty();
	Table->HandleDataTableChanged(FName(*RowName));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("row_name"), RowName);
	bool bHasFailures = FailedArr.Num() > 0;
	ResultData->SetBoolField(TEXT("new_row"), bNewRow);
	ResultData->SetArrayField(TEXT("changed"), ChangedArr);
	ResultData->SetArrayField(TEXT("failed"), FailedArr);
	if (bHasFailures) ResultData->SetBoolField(TEXT("partial_success"), true);

	FMCPToolResult Result;
	Result.bSuccess = !bHasFailures;
	Result.Message = FString::Printf(TEXT("%s row '%s': %d fields set, %d failed"), bNewRow ? TEXT("Added") : TEXT("Updated"), *RowName, ChangedArr.Num(), FailedArr.Num());
	Result.Data = ResultData;
	return Result;
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveDataTableRow(const TSharedRef<FJsonObject>& Params)
{
	FString TablePath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), TablePath, Error)) return Error.GetValue();

	FString RowName;
	if (!ExtractRequiredString(Params, TEXT("row_name"), RowName, Error)) return Error.GetValue();

	UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
	if (!Table) return FMCPToolResult::Error(FString::Printf(TEXT("Could not load DataTable: %s"), *TablePath));

	if (!Table->FindRowUnchecked(FName(*RowName)))
		return FMCPToolResult::Error(FString::Printf(TEXT("Row '%s' not found"), *RowName));

	Table->RemoveRow(FName(*RowName));
	Table->MarkPackageDirty();

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("row_name"), RowName);
	ResultData->SetStringField(TEXT("status"), TEXT("removed"));

	return FMCPToolResult::Success(FString::Printf(TEXT("Removed row '%s'"), *RowName), ResultData);
}

// ===== Widget Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveWidget(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error)) return Error.GetValue();

	FString WidgetName;
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();

	UObject* LoadedObj = LoadObject<UObject>(nullptr, *BlueprintPath);
	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(LoadedObj);
	if (!WidgetBP) return FMCPToolResult::Error(TEXT("Not a WidgetBlueprint"));

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree) return FMCPToolResult::Error(TEXT("No WidgetTree"));

	UWidget* Widget = Tree->FindWidget(FName(*WidgetName));
	if (!Widget) return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));

	if (Widget == Tree->RootWidget)
		return FMCPToolResult::Error(TEXT("Cannot remove root widget"));

	// Remove from parent
	Widget->RemoveFromParent();
	Tree->RemoveWidget(Widget);
	WidgetBP->MarkPackageDirty();

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("widget_name"), WidgetName);
	ResultData->SetStringField(TEXT("status"), TEXT("removed"));

	return FMCPToolResult::Success(FString::Printf(TEXT("Removed widget '%s'"), *WidgetName), ResultData);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddWidget(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error)) return Error.GetValue();

	FString WidgetClass;
	if (!ExtractRequiredString(Params, TEXT("widget_class"), WidgetClass, Error)) return Error.GetValue();

	FString WidgetName = ExtractOptionalString(Params, TEXT("widget_name"));
	FString ParentWidget = ExtractOptionalString(Params, TEXT("parent_widget"));

	UObject* LoadedObj = LoadObject<UObject>(nullptr, *BlueprintPath);
	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(LoadedObj);
	if (!WidgetBP) return FMCPToolResult::Error(TEXT("Not a WidgetBlueprint"));

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree) return FMCPToolResult::Error(TEXT("No WidgetTree"));

	// Find widget class
	UClass* WClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/UMG.%s"), *WidgetClass));
	if (!WClass) WClass = FindObject<UClass>(nullptr, *WidgetClass);
	if (!WClass || !WClass->IsChildOf(UWidget::StaticClass()))
		return FMCPToolResult::Error(FString::Printf(TEXT("Widget class not found: %s"), *WidgetClass));

	// Create widget
	FName NewName = WidgetName.IsEmpty() ? FName(*WidgetClass) : FName(*WidgetName);
	UWidget* NewWidget = Tree->ConstructWidget<UWidget>(WClass, NewName);
	if (!NewWidget)
		return FMCPToolResult::Error(TEXT("Failed to construct widget"));

	// Add to parent or root
	if (!ParentWidget.IsEmpty())
	{
		UWidget* Parent = Tree->FindWidget(FName(*ParentWidget));
		UPanelWidget* Panel = Cast<UPanelWidget>(Parent);
		if (Panel)
		{
			Panel->AddChild(NewWidget);
		}
		else
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Parent '%s' not found or not a panel widget"), *ParentWidget));
		}
	}
	else if (!Tree->RootWidget)
	{
		Tree->RootWidget = NewWidget;
	}
	else
	{
		UPanelWidget* RootPanel = Cast<UPanelWidget>(Tree->RootWidget);
		if (RootPanel)
			RootPanel->AddChild(NewWidget);
		else
			return FMCPToolResult::Error(TEXT("Root widget is not a panel — cannot add child. Specify parent_widget explicitly."));
	}

	WidgetBP->MarkPackageDirty();

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("widget_name"), NewWidget->GetName());
	ResultData->SetStringField(TEXT("widget_class"), WClass->GetName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added widget '%s' (%s)"), *NewWidget->GetName(), *WClass->GetName()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetWidgetProperty(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error)) return Error.GetValue();

	FString WidgetName;
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();

	FString PropertyName;
	if (!ExtractRequiredString(Params, TEXT("property_name"), PropertyName, Error)) return Error.GetValue();

	FString PropertyValue;
	if (!ExtractRequiredString(Params, TEXT("property_value"), PropertyValue, Error)) return Error.GetValue();

	UObject* LoadedObj = LoadObject<UObject>(nullptr, *BlueprintPath);
	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(LoadedObj);
	if (!WidgetBP) return FMCPToolResult::Error(TEXT("Not a WidgetBlueprint"));

	UWidget* Widget = WidgetBP->WidgetTree ? WidgetBP->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
	if (!Widget) return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));

	FProperty* Prop = Widget->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop) return FMCPToolResult::Error(FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Widget->GetClass()->GetName()));

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
	FString OldValue; Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, Widget, PPF_None);

	if (!Prop->ImportText_Direct(*PropertyValue, ValuePtr, Widget, PPF_None))
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to parse '%s' for property '%s'"), *PropertyValue, *PropertyName));

	WidgetBP->MarkPackageDirty();

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("widget_name"), WidgetName);
	ResultData->SetStringField(TEXT("property_name"), PropertyName);
	ResultData->SetStringField(TEXT("old_value"), OldValue);
	ResultData->SetStringField(TEXT("new_value"), PropertyValue);
	ResultData->SetStringField(TEXT("status"), TEXT("changed"));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set '%s.%s' = '%s'"), *WidgetName, *PropertyName, *PropertyValue),
		ResultData
	);
}

// ===== Component Template Batch Properties =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetComponentPropertiesBatch(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString ComponentName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("component_name"), ComponentName, Error)) return Error.GetValue();

	const TArray<TSharedPtr<FJsonValue>>* PropsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("properties"), PropsArray))
		return FMCPToolResult::Error(TEXT("Missing required parameter: properties (array of {name, value})"));

	USimpleConstructionScript* SCS = Context.Blueprint->SimpleConstructionScript;
	if (!SCS) return FMCPToolResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));

	USCS_Node* FoundNode = SCS->FindSCSNode(FName(*ComponentName));
	if (!FoundNode || !FoundNode->ComponentTemplate)
		return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' not found"), *ComponentName));

	UActorComponent* Template = FoundNode->ComponentTemplate;

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ChangedArr, UnchangedArr, FailedArr;

	for (const TSharedPtr<FJsonValue>& PropVal : *PropsArray)
	{
		const TSharedPtr<FJsonObject>* PropObj;
		if (!PropVal->TryGetObject(PropObj)) continue;
		FString Name, Value;
		(*PropObj)->TryGetStringField(TEXT("name"), Name);
		(*PropObj)->TryGetStringField(TEXT("value"), Value);
		if (Name.IsEmpty()) continue;

		// Navigate nested property paths (e.g., "RelativeLocation.X")
		FPropertyPathResult PathResult = NavigatePropertyPath(Template->GetClass(), Template, Name);
		if (!PathResult.bSuccess)
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>(); F->SetStringField(TEXT("name"), Name); F->SetStringField(TEXT("status"), TEXT("failed")); F->SetStringField(TEXT("reason"), PathResult.ErrorMessage);
			FailedArr.Add(MakeShared<FJsonValueObject>(F)); continue;
		}

		FProperty* Prop = PathResult.Prop;
		void* ValuePtr = PathResult.ValuePtr;
		FString OldValue; Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, Template, PPF_None);

		if (OldValue == Value)
		{
			TSharedPtr<FJsonObject> U = MakeShared<FJsonObject>(); U->SetStringField(TEXT("name"), Name); U->SetStringField(TEXT("status"), TEXT("unchanged")); U->SetStringField(TEXT("value"), OldValue);
			UnchangedArr.Add(MakeShared<FJsonValueObject>(U)); continue;
		}

		if (!Prop->ImportText_Direct(*Value, ValuePtr, Template, PPF_None))
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>(); F->SetStringField(TEXT("name"), Name); F->SetStringField(TEXT("status"), TEXT("failed")); F->SetStringField(TEXT("reason"), FString::Printf(TEXT("parse failed for type %s"), *Prop->GetCPPType()));
			FailedArr.Add(MakeShared<FJsonValueObject>(F)); continue;
		}

		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>(); C->SetStringField(TEXT("name"), Name); C->SetStringField(TEXT("status"), TEXT("changed")); C->SetStringField(TEXT("old_value"), OldValue); C->SetStringField(TEXT("new_value"), Value);
		ChangedArr.Add(MakeShared<FJsonValueObject>(C));
	}

	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
	Template->PostEditChange();

	bool bHasFailures = FailedArr.Num() > 0;
	ResultData->SetStringField(TEXT("component_name"), ComponentName);
	ResultData->SetArrayField(TEXT("changed"), ChangedArr);
	ResultData->SetArrayField(TEXT("unchanged"), UnchangedArr);
	ResultData->SetArrayField(TEXT("failed"), FailedArr);
	ResultData->SetNumberField(TEXT("changed_count"), ChangedArr.Num());
	ResultData->SetNumberField(TEXT("unchanged_count"), UnchangedArr.Num());
	ResultData->SetNumberField(TEXT("failed_count"), FailedArr.Num());
	if (bHasFailures) ResultData->SetBoolField(TEXT("partial_success"), true);

	FMCPToolResult Result;
	Result.bSuccess = !bHasFailures;
	Result.Message = FString::Printf(TEXT("Component '%s': %d changed, %d unchanged, %d failed"), *ComponentName, ChangedArr.Num(), UnchangedArr.Num(), FailedArr.Num());
	Result.Data = ResultData;
	return Result;
}

// ===== Editor Utility Execution =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRunEditorUtility(const TSharedRef<FJsonObject>& Params)
{
	FString UtilityPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), UtilityPath, Error)) return Error.GetValue();

	// Load the utility asset
	UObject* LoadedAsset = LoadObject<UObject>(nullptr, *UtilityPath);
	if (!LoadedAsset)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load editor utility: %s"), *UtilityPath));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("utility_path"), UtilityPath);
	ResultData->SetStringField(TEXT("utility_name"), LoadedAsset->GetName());

	// Try to open as widget via UEditorUtilitySubsystem
	if (UEditorUtilitySubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>() : nullptr)
	{
		if (UEditorUtilityWidgetBlueprint* WidgetBP = Cast<UEditorUtilityWidgetBlueprint>(LoadedAsset))
		{
			Subsystem->SpawnAndRegisterTab(WidgetBP);
			ResultData->SetStringField(TEXT("action"), TEXT("opened_widget"));
			ResultData->SetStringField(TEXT("utility_type"), TEXT("EditorUtilityWidgetBlueprint"));

			FExecutionReceipt Receipt;
			Receipt.Tool = TEXT("blueprint_modify:run_editor_utility");
			Receipt.bSuccess = true;
			Receipt.TargetType = TEXT("editor_utility");
			Receipt.Targets.Add(UtilityPath);
			Receipt.Classification = TEXT("user_mutation");
			Receipt.Status = TEXT("success");
			Receipt.Summary = FString::Printf(TEXT("Opened editor utility widget: %s"), *LoadedAsset->GetName());
			FUnrealClaudeExecutionLog::Get().AddReceipt(Receipt);

			return FMCPToolResult::Success(
				FString::Printf(TEXT("Opened editor utility widget: %s"), *LoadedAsset->GetName()),
				ResultData
			);
		}

		// Non-widget EditorUtilityBlueprint — try RunEditorUtilityBlueprint
		if (UEditorUtilityBlueprint* UtilBP = Cast<UEditorUtilityBlueprint>(LoadedAsset))
		{
			Subsystem->TryRun(UtilBP);
			ResultData->SetStringField(TEXT("action"), TEXT("run_attempted"));
			ResultData->SetStringField(TEXT("utility_type"), TEXT("EditorUtilityBlueprint"));
			ResultData->SetStringField(TEXT("note"), TEXT("TryRun called — execution is best-effort, success cannot be confirmed programmatically"));

			FExecutionReceipt Receipt;
			Receipt.Tool = TEXT("blueprint_modify:run_editor_utility");
			Receipt.bSuccess = true;
			Receipt.TargetType = TEXT("editor_utility");
			Receipt.Targets.Add(UtilityPath);
			Receipt.Classification = TEXT("user_mutation");
			Receipt.Status = TEXT("success");
			Receipt.Summary = FString::Printf(TEXT("TryRun attempted on editor utility: %s (non-widget, best-effort)"), *LoadedAsset->GetName());
			FUnrealClaudeExecutionLog::Get().AddReceipt(Receipt);

			return FMCPToolResult::Success(
				FString::Printf(TEXT("TryRun attempted on editor utility: %s (non-widget, best-effort)"), *LoadedAsset->GetName()),
				ResultData
			);
		}
	}

	return FMCPToolResult::Error(TEXT("UEditorUtilitySubsystem not available or asset is not an editor utility"));
}

// ===== Delegate Binding Workflow =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteBindDispatcher(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString DispatcherName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("dispatcher_name"), DispatcherName, Error)) return Error.GetValue();

	FString EventName;
	if (!ExtractRequiredString(Params, TEXT("event_name"), EventName, Error)) return Error.GetValue();

	int32 PosX = (int32)ExtractOptionalNumber(Params, TEXT("pos_x"), 0);
	int32 PosY = (int32)ExtractOptionalNumber(Params, TEXT("pos_y"), 0);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CreatedNodes;

	// 1. Verify dispatcher exists
	bool bDispatcherFound = false;
	for (UEdGraph* G : Context.Blueprint->DelegateSignatureGraphs)
	{
		if (G && G->GetName() == DispatcherName) { bDispatcherFound = true; break; }
	}
	if (!bDispatcherFound)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Dispatcher '%s' not found. Create it first with add_dispatcher."), *DispatcherName));
	}

	// 2. Find or create CustomEvent as binding target
	FString GraphError;
	UEdGraph* EventGraph = FBlueprintGraphEditor::FindGraph(Context.Blueprint, TEXT(""), false, GraphError);
	if (!EventGraph) return FMCPToolResult::Error(GraphError);

	// Check if event already exists
	bool bEventExists = false;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
		{
			if (CE->CustomFunctionName.ToString() == EventName) { bEventExists = true; break; }
		}
	}

	FString EventNodeId;
	if (!bEventExists)
	{
		TSharedPtr<FJsonObject> EvtParams = MakeShared<FJsonObject>();
		EvtParams->SetStringField(TEXT("event_name"), EventName);
		FString CreateError;
		UEdGraphNode* EvtNode = FBlueprintGraphEditor::CreateNode(EventGraph, TEXT("CustomEvent"), EvtParams.ToSharedRef(), PosX, PosY, EventNodeId, CreateError);
		if (!EvtNode)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create event '%s': %s"), *EventName, *CreateError));
		}
		TSharedPtr<FJsonObject> EvtInfo = MakeShared<FJsonObject>();
		EvtInfo->SetStringField(TEXT("type"), TEXT("custom_event"));
		EvtInfo->SetStringField(TEXT("name"), EventName);
		EvtInfo->SetStringField(TEXT("node_id"), EventNodeId);
		EvtInfo->SetStringField(TEXT("status"), TEXT("created"));
		CreatedNodes.Add(MakeShared<FJsonValueObject>(EvtInfo));
	}

	// 3. Create AddDelegate (bind) node
	FString BindNodeId;
	FString BindError;
	TSharedPtr<FJsonObject> BindParams = MakeShared<FJsonObject>();
	BindParams->SetStringField(TEXT("dispatcher"), DispatcherName);
	UEdGraphNode* BindNode = FBlueprintGraphEditor::CreateNode(EventGraph, TEXT("AddDelegate"), BindParams.ToSharedRef(), PosX + 300, PosY, BindNodeId, BindError);
	if (!BindNode)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create bind node: %s"), *BindError));
	}
	TSharedPtr<FJsonObject> BindInfo = MakeShared<FJsonObject>();
	BindInfo->SetStringField(TEXT("type"), TEXT("add_delegate"));
	BindInfo->SetStringField(TEXT("dispatcher"), DispatcherName);
	BindInfo->SetStringField(TEXT("node_id"), BindNodeId);
	BindInfo->SetStringField(TEXT("status"), TEXT("created"));
	CreatedNodes.Add(MakeShared<FJsonValueObject>(BindInfo));

	// 4. Create CallDelegate node
	FString CallNodeId;
	FString CallError;
	TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
	CallParams->SetStringField(TEXT("dispatcher"), DispatcherName);
	UEdGraphNode* CallNode = FBlueprintGraphEditor::CreateNode(EventGraph, TEXT("CallDelegate"), CallParams.ToSharedRef(), PosX + 600, PosY, CallNodeId, CallError);
	if (!CallNode)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create call node: %s"), *CallError));
	}
	TSharedPtr<FJsonObject> CallInfo = MakeShared<FJsonObject>();
	CallInfo->SetStringField(TEXT("type"), TEXT("call_delegate"));
	CallInfo->SetStringField(TEXT("dispatcher"), DispatcherName);
	CallInfo->SetStringField(TEXT("node_id"), CallNodeId);
	CallInfo->SetStringField(TEXT("status"), TEXT("created"));
	CreatedNodes.Add(MakeShared<FJsonValueObject>(CallInfo));

	// 5. Wire: try to connect AddDelegate's "Delegate" pin to CustomEvent's output delegate
	TArray<TSharedPtr<FJsonValue>> Connections;
	{
		// Find CustomEvent node (may have been pre-existing)
		UEdGraphNode* EventNode = nullptr;
		for (UEdGraphNode* N : EventGraph->Nodes)
		{
			if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(N))
			{
				if (CE->CustomFunctionName.ToString() == EventName) { EventNode = N; break; }
			}
		}

		if (EventNode && BindNode)
		{
			// Connect exec: EventNode.then → BindNode.execute
			FString ExecError;
			{
				TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
				ConnObj->SetStringField(TEXT("from"), TEXT("CustomEvent.then"));
				ConnObj->SetStringField(TEXT("to"), TEXT("AddDelegate.execute"));
				if (FBlueprintGraphEditor::ConnectPins(EventGraph,
					FBlueprintGraphEditor::GetNodeId(EventNode), TEXT("then"),
					BindNodeId, TEXT("execute"), ExecError))
				{
					ConnObj->SetStringField(TEXT("status"), TEXT("connected"));
				}
				else
				{
					ConnObj->SetStringField(TEXT("status"), TEXT("failed"));
					ConnObj->SetStringField(TEXT("reason"), ExecError);
				}
				Connections.Add(MakeShared<FJsonValueObject>(ConnObj));
			}

			// Try to connect delegate pin: EventNode.OutputDelegate → BindNode.Delegate
			UEdGraphPin* DelegateOut = FBlueprintGraphEditor::FindPinByName(EventNode, TEXT("OutputDelegate"), EGPD_Output);
			UEdGraphPin* DelegateIn = FBlueprintGraphEditor::FindPinByName(BindNode, TEXT("Delegate"));
			if (DelegateOut && DelegateIn)
			{
				const UEdGraphSchema* Schema = EventGraph->GetSchema();
				if (Schema)
				{
					FPinConnectionResponse Response = Schema->CanCreateConnection(DelegateOut, DelegateIn);
					if (Response.Response != CONNECT_RESPONSE_DISALLOW)
					{
						Schema->TryCreateConnection(DelegateOut, DelegateIn);
						TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
						ConnObj->SetStringField(TEXT("from"), TEXT("CustomEvent.OutputDelegate"));
						ConnObj->SetStringField(TEXT("to"), TEXT("AddDelegate.Delegate"));
						ConnObj->SetStringField(TEXT("status"), TEXT("connected"));
						Connections.Add(MakeShared<FJsonValueObject>(ConnObj));
					}
					else
					{
						TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
						ConnObj->SetStringField(TEXT("from"), TEXT("CustomEvent.OutputDelegate"));
						ConnObj->SetStringField(TEXT("to"), TEXT("AddDelegate.Delegate"));
						ConnObj->SetStringField(TEXT("status"), TEXT("failed"));
						ConnObj->SetStringField(TEXT("reason"), Response.Message.ToString());
						Connections.Add(MakeShared<FJsonValueObject>(ConnObj));
					}
				}
			}
			else
			{
				TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
				ConnObj->SetStringField(TEXT("from"), TEXT("CustomEvent.OutputDelegate"));
				ConnObj->SetStringField(TEXT("to"), TEXT("AddDelegate.Delegate"));
				ConnObj->SetStringField(TEXT("status"), TEXT("skipped"));
				ConnObj->SetStringField(TEXT("reason"), !DelegateOut ? TEXT("OutputDelegate pin not found on CustomEvent") : TEXT("Delegate pin not found on AddDelegate node"));
				Connections.Add(MakeShared<FJsonValueObject>(ConnObj));
			}
		}
	}

	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	ResultData->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
	ResultData->SetStringField(TEXT("dispatcher"), DispatcherName);
	ResultData->SetStringField(TEXT("event_name"), EventName);
	ResultData->SetArrayField(TEXT("nodes"), CreatedNodes);
	ResultData->SetNumberField(TEXT("node_count"), CreatedNodes.Num());
	ResultData->SetArrayField(TEXT("connections"), Connections);
	ResultData->SetNumberField(TEXT("connection_count"), Connections.Num());
	ResultData->SetBoolField(TEXT("compiled"), false);
	ResultData->SetStringField(TEXT("compile_note"), TEXT("BP has delegate graphs — compile skipped. Save and reopen in editor."));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Dispatcher binding: %d nodes, %d connections for '%s' -> '%s'"), CreatedNodes.Num(), Connections.Num(), *DispatcherName, *EventName),
		ResultData
	);
}

// ===== Macro Graph Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteCreateMacro(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString MacroName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("macro_name"), MacroName, Error)) return Error.GetValue();

	// Check if macro already exists
	for (UEdGraph* Graph : Context.Blueprint->MacroGraphs)
	{
		if (Graph && Graph->GetName() == MacroName)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Macro '%s' already exists"), *MacroName));
		}
	}

	// Create macro graph
	UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
		Context.Blueprint, FName(*MacroName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!MacroGraph)
	{
		return FMCPToolResult::Error(TEXT("Failed to create macro graph"));
	}

	FBlueprintEditorUtils::AddMacroGraph(Context.Blueprint, MacroGraph, false, nullptr);

	// Ensure tunnel entry/exit nodes exist (AddMacroGraph should create them)
	bool bHasEntry = false, bHasExit = false;
	for (UEdGraphNode* Node : MacroGraph->Nodes)
	{
		if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
		{
			if (Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs) bHasEntry = true;
			if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs) bHasExit = true;
		}
	}

	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("macro_name"), MacroName);
	ResultData->SetBoolField(TEXT("has_entry"), bHasEntry);
	ResultData->SetBoolField(TEXT("has_exit"), bHasExit);
	ResultData->SetNumberField(TEXT("node_count"), MacroGraph->Nodes.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Created macro graph '%s'"), *MacroName),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveMacro(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString MacroName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("macro_name"), MacroName, Error)) return Error.GetValue();

	// Find macro graph
	UEdGraph* FoundGraph = nullptr;
	for (UEdGraph* Graph : Context.Blueprint->MacroGraphs)
	{
		if (Graph && Graph->GetName() == MacroName)
		{
			FoundGraph = Graph;
			break;
		}
	}
	if (!FoundGraph)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Macro '%s' not found"), *MacroName));
	}

	Context.Blueprint->MacroGraphs.Remove(FoundGraph);
	FBlueprintEditorUtils::RemoveGraph(Context.Blueprint, FoundGraph);

	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("macro_name"), MacroName);
	return FMCPToolResult::Success(FString::Printf(TEXT("Removed macro '%s'"), *MacroName), ResultData);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddMacroInstance(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	bool bMacroGraph = ExtractOptionalBool(Params, TEXT("is_macro_graph"), false);
	int32 PosX = (int32)ExtractOptionalNumber(Params, TEXT("pos_x"), 0);
	int32 PosY = (int32)ExtractOptionalNumber(Params, TEXT("pos_y"), 0);

	// Find target graph to place macro instance into
	FString GraphError;
	UEdGraph* TargetGraph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError, bMacroGraph);
	if (!TargetGraph) return FMCPToolResult::Error(GraphError);

	// Get macro source - defaults to same Blueprint
	FString MacroSourceBP = ExtractOptionalString(Params, TEXT("macro_source_blueprint"));
	FString MacroSourceName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("macro_source_name"), MacroSourceName, Error)) return Error.GetValue();

	// Find the source macro graph
	UBlueprint* SourceBP = Context.Blueprint;
	if (!MacroSourceBP.IsEmpty())
	{
		FString SourceLoadError;
		SourceBP = FBlueprintUtils::LoadBlueprint(MacroSourceBP, SourceLoadError);
		if (!SourceBP) return FMCPToolResult::Error(SourceLoadError);
	}

	UEdGraph* MacroGraph = nullptr;
	for (UEdGraph* Graph : SourceBP->MacroGraphs)
	{
		if (Graph && Graph->GetName() == MacroSourceName)
		{
			MacroGraph = Graph;
			break;
		}
	}
	if (!MacroGraph)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Macro '%s' not found in source Blueprint"), *MacroSourceName));
	}

	// Create macro instance node
	UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(TargetGraph);
	MacroNode->SetMacroGraph(MacroGraph);
	MacroNode->NodePosX = PosX;
	MacroNode->NodePosY = PosY;
	MacroNode->CreateNewGuid();
	MacroNode->PostPlacedNewNode();
	MacroNode->AllocateDefaultPins();
	TargetGraph->AddNode(MacroNode);

	// Generate and store MCP node ID
	FString NodeId = FBlueprintGraphEditor::GenerateNodeId(TEXT("MacroInstance"), MacroSourceName, TargetGraph);
	FBlueprintGraphEditor::SetNodeId(MacroNode, NodeId);

	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("node_id"), NodeId);
	ResultData->SetStringField(TEXT("macro_name"), MacroSourceName);
	ResultData->SetNumberField(TEXT("pin_count"), MacroNode->Pins.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added macro instance '%s' as node '%s'"), *MacroSourceName, *NodeId),
		ResultData
	);
}

// ===== Dispatcher Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddDispatcher(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString DispatcherName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("dispatcher_name"), DispatcherName, Error)) return Error.GetValue();

	// Check if already exists
	for (UEdGraph* Graph : Context.Blueprint->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetName() == DispatcherName)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Dispatcher '%s' already exists"), *DispatcherName));
		}
	}

	// Create delegate signature graph
	UEdGraph* SigGraph = FBlueprintEditorUtils::CreateNewGraph(Context.Blueprint, FName(*DispatcherName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!SigGraph)
	{
		return FMCPToolResult::Error(TEXT("Failed to create delegate signature graph"));
	}

	SigGraph->bAllowDeletion = false;
	Context.Blueprint->DelegateSignatureGraphs.Add(SigGraph);

	// Add function entry node for the delegate signature
	UK2Node_FunctionEntry* EntryNode = NewObject<UK2Node_FunctionEntry>(SigGraph);
	EntryNode->CreateNewGuid();
	EntryNode->PostPlacedNewNode();
	EntryNode->AllocateDefaultPins();
	SigGraph->AddNode(EntryNode);

	// Note: We do NOT create an MC_Delegate variable via AddMemberVariable because
	// UE's MarkBlueprintAsStructurallyModified crashes on MC_Delegate variables
	// with mismatched SubCategory names. The signature graph alone is sufficient
	// for the delegate to appear in the editor. The BP must be saved and reopened
	// or compiled manually in the editor for the delegate variable to fully conform.

	// Mark dirty only — do not compile here to avoid assertion crash on new delegate graphs
	// Use validate_blueprint separately after adding dispatcher to compile safely
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
	ResultData->SetBoolField(TEXT("compiled"), false);
	ResultData->SetStringField(TEXT("compile_note"), TEXT("Dispatcher added. Save the Blueprint in editor and reopen to conform. Do NOT call validate_blueprint — it crashes UE 5.7 on delegate graphs."));
	ResultData->SetStringField(TEXT("dispatcher_name"), DispatcherName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added event dispatcher '%s'"), *DispatcherName),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveDispatcher(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString DispatcherName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("dispatcher_name"), DispatcherName, Error)) return Error.GetValue();

	// Find and remove delegate signature graph
	UEdGraph* FoundGraph = nullptr;
	for (UEdGraph* Graph : Context.Blueprint->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetName().Contains(DispatcherName))
		{
			FoundGraph = Graph;
			break;
		}
	}
	if (!FoundGraph)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Dispatcher '%s' not found"), *DispatcherName));
	}

	Context.Blueprint->DelegateSignatureGraphs.Remove(FoundGraph);
	FBlueprintEditorUtils::RemoveGraph(Context.Blueprint, FoundGraph);
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
	ResultData->SetStringField(TEXT("dispatcher_name"), DispatcherName);
	return FMCPToolResult::Success(FString::Printf(TEXT("Removed dispatcher '%s'"), *DispatcherName), ResultData);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetDispatcherSignature(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	FString DispatcherName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("dispatcher_name"), DispatcherName, Error)) return Error.GetValue();

	// Find the delegate signature graph
	UEdGraph* SigGraph = nullptr;
	for (UEdGraph* Graph : Context.Blueprint->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetName().Contains(DispatcherName))
		{
			SigGraph = Graph;
			break;
		}
	}

	if (!SigGraph)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Dispatcher signature graph not found for '%s'. Create the dispatcher first with add_dispatcher."), *DispatcherName));
	}

	// Find entry node
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : SigGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}
	if (!EntryNode)
	{
		return FMCPToolResult::Error(TEXT("Dispatcher signature entry node not found"));
	}

	// Parse parameters
	const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("parameters"), ParamsArray))
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: parameters (array of {name, type})"));
	}

	// Clear existing user-defined pins
	TArray<FName> ExistingPinNames;
	for (UEdGraphPin* Pin : EntryNode->Pins)
	{
		if (!Pin) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == TEXT("self")) continue;
		if (Pin->Direction == EGPD_Output)
		{
			ExistingPinNames.Add(Pin->PinName);
		}
	}
	for (const FName& PinName : ExistingPinNames)
	{
		EntryNode->RemoveUserDefinedPinByName(PinName);
	}

	// Add new params
	TArray<TSharedPtr<FJsonValue>> AddedParams;
	int32 ParamCount = 0;

	for (const TSharedPtr<FJsonValue>& ParamVal : *ParamsArray)
	{
		const TSharedPtr<FJsonObject>* ParamObj;
		if (!ParamVal->TryGetObject(ParamObj)) continue;

		FString ParamName, ParamType;
		(*ParamObj)->TryGetStringField(TEXT("name"), ParamName);
		(*ParamObj)->TryGetStringField(TEXT("type"), ParamType);
		if (ParamName.IsEmpty() || ParamType.IsEmpty()) continue;

		FEdGraphPinType PinType; FString TypeError;
		if (!FBlueprintUtils::ParsePinType(ParamType, PinType, TypeError))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Type resolution failed for param '%s': %s"), *ParamName, *TypeError));
		}

		EntryNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output);
		ParamCount++;

		TSharedPtr<FJsonObject> Added = MakeShared<FJsonObject>();
		Added->SetStringField(TEXT("name"), ParamName);
		Added->SetStringField(TEXT("type"), ParamType);
		AddedParams.Add(MakeShared<FJsonValueObject>(Added));
	}

	// Mark dirty only — do not compile to avoid assertion crash on delegate variables
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
	ResultData->SetBoolField(TEXT("compiled"), false);
	ResultData->SetStringField(TEXT("compile_note"), TEXT("Dispatcher signature set. Save the Blueprint in editor and reopen to conform. Do NOT call validate_blueprint — it crashes UE 5.7 on delegate graphs."));
	ResultData->SetStringField(TEXT("dispatcher_name"), DispatcherName);
	ResultData->SetArrayField(TEXT("parameters"), AddedParams);
	ResultData->SetNumberField(TEXT("param_count"), ParamCount);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set dispatcher '%s' signature: %d params"), *DispatcherName, ParamCount),
		ResultData
	);
}

// ===== Composed Bundle: configure_actor_class =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteConfigureActorClass(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	bool bDryRun = ExtractOptionalBool(Params, TEXT("dry_run"), false);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CreatedArr, UpdatedArr, UnchangedArr, FailedArr, SkippedArr;
	int32 TotalOps = 0;
	int32 CompileCount = 0;
	int32 LowLevelCallsSaved = 0; // track how many individual calls this bundle replaces

	auto AddResult = [](TArray<TSharedPtr<FJsonValue>>& Arr, const FString& Kind, const FString& Name, const FString& Extra = TEXT(""), const FString& ExtraKey = TEXT("")) {
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("kind"), Kind); Obj->SetStringField(TEXT("name"), Name);
		if (!Extra.IsEmpty() && !ExtraKey.IsEmpty()) Obj->SetStringField(ExtraKey, Extra);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	};

	// In dry_run mode, created/updated results get a diff_status hint
	auto AddCreated = [&](const FString& Kind, const FString& Name) {
		if (bDryRun) AddResult(CreatedArr, Kind, Name, TEXT("would_create"), TEXT("diff_status"));
		else AddResult(CreatedArr, Kind, Name);
	};
	auto AddUpdated = [&](const FString& Kind, const FString& Name) {
		if (bDryRun) AddResult(UpdatedArr, Kind, Name, TEXT("would_update"), TEXT("diff_status"));
		else AddResult(UpdatedArr, Kind, Name);
	};
	auto AddUnchanged = [&](const FString& Kind, const FString& Name) { AddResult(UnchangedArr, Kind, Name); };
	auto AddFailed = [&](const FString& Kind, const FString& Name, const FString& Reason) { AddResult(FailedArr, Kind, Name, Reason, TEXT("error")); };
	auto AddSkipped = [&](const FString& Kind, const FString& Name, const FString& Reason) { AddResult(SkippedArr, Kind, Name, Reason, TEXT("reason")); };

	// ===== Phase 1: Variables (via apply_blueprint_spec sub-call patterns) =====
	const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("variables"), VarsArray))
	{
		for (const TSharedPtr<FJsonValue>& VarVal : *VarsArray)
		{
			TotalOps++;
			LowLevelCallsSaved++; // would be add_variable
			const TSharedPtr<FJsonObject>* VarObj; if (!VarVal->TryGetObject(VarObj)) continue;
			FString VarName, VarType;
			(*VarObj)->TryGetStringField(TEXT("name"), VarName);
			(*VarObj)->TryGetStringField(TEXT("type"), VarType);
			if (VarName.IsEmpty() || VarType.IsEmpty()) { AddFailed(TEXT("variable"), VarName, TEXT("missing name or type")); continue; }

			FEdGraphPinType PinType; FString TypeError;
			if (!FBlueprintUtils::ParsePinType(VarType, PinType, TypeError))
			{
				if (bDryRun) { AddResult(FailedArr, TEXT("variable"), VarName, TypeError, TEXT("error")); continue; }
				AddFailed(TEXT("variable"), VarName, TypeError); continue;
			}

			// Deep diff: check existence AND type match
			bool bExists = false;
			bool bTypeMatch = false;
			for (const FBPVariableDescription& V : Context.Blueprint->NewVariables)
			{
				if (V.VarName == FName(*VarName))
				{
					bExists = true;
					bTypeMatch = (V.VarType.PinCategory == PinType.PinCategory
						&& V.VarType.PinSubCategoryObject == PinType.PinSubCategoryObject
						&& V.VarType.ContainerType == PinType.ContainerType);
					break;
				}
			}

			if (bExists && bTypeMatch) { AddUnchanged(TEXT("variable"), VarName); continue; }
			if (bExists && !bTypeMatch) { AddSkipped(TEXT("variable"), VarName, TEXT("exists with different type")); continue; }
			if (bDryRun) { AddResult(CreatedArr, TEXT("variable"), VarName, TEXT("would_create"), TEXT("diff_status")); continue; }

			FString AddError;
			if (FBlueprintUtils::AddVariable(Context.Blueprint, VarName, PinType, AddError))
				AddCreated(TEXT("variable"), VarName);
			else
				AddFailed(TEXT("variable"), VarName, AddError);
		}
	}

	// ===== Phase 2: Components =====
	const TArray<TSharedPtr<FJsonValue>>* CompsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("components"), CompsArray))
	{
		USimpleConstructionScript* SCS = Context.Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			for (const auto& C : *CompsArray) { TotalOps++; LowLevelCallsSaved++; AddFailed(TEXT("component"), TEXT("?"), TEXT("Blueprint has no SCS")); }
		}
		else
		{
			for (const TSharedPtr<FJsonValue>& CompVal : *CompsArray)
			{
				TotalOps++;
				LowLevelCallsSaved++; // would be add_component
				const TSharedPtr<FJsonObject>* CompObj; if (!CompVal->TryGetObject(CompObj)) continue;
				FString CompName, CompClass;
				(*CompObj)->TryGetStringField(TEXT("name"), CompName);
				(*CompObj)->TryGetStringField(TEXT("class"), CompClass);
				if (CompName.IsEmpty() || CompClass.IsEmpty()) { AddFailed(TEXT("component"), CompName, TEXT("missing name or class")); continue; }

				// Deep diff: check existence AND class match
				USCS_Node* ExistingNode = SCS->FindSCSNode(FName(*CompName));
				if (ExistingNode)
				{
					// Verify class matches desired state
					bool bClassMatch = ExistingNode->ComponentClass && ExistingNode->ComponentClass->GetName().Contains(CompClass);
					if (bClassMatch)
						AddUnchanged(TEXT("component"), CompName);
					else
						AddSkipped(TEXT("component"), CompName, FString::Printf(TEXT("exists with class %s, desired %s"),
							ExistingNode->ComponentClass ? *ExistingNode->ComponentClass->GetName() : TEXT("null"), *CompClass));

					// Even if component exists, apply properties if specified
					const TArray<TSharedPtr<FJsonValue>>* CompProps = nullptr;
					if ((*CompObj)->TryGetArrayField(TEXT("properties"), CompProps) && CompProps->Num() > 0 && ExistingNode->ComponentTemplate)
					{
						for (const TSharedPtr<FJsonValue>& PropVal : *CompProps)
						{
							TotalOps++;
							LowLevelCallsSaved++; // would be set_component_property
							const TSharedPtr<FJsonObject>* PropObj; if (!PropVal->TryGetObject(PropObj)) continue;
							FString PropName, PropValue;
							(*PropObj)->TryGetStringField(TEXT("name"), PropName);
							(*PropObj)->TryGetStringField(TEXT("value"), PropValue);
							if (PropName.IsEmpty()) continue;

							if (bDryRun)
							{
								// Deep dry_run: check if property would actually change + validate parseability
								FPropertyPathResult PathResult = NavigatePropertyPath(ExistingNode->ComponentTemplate->GetClass(), ExistingNode->ComponentTemplate, PropName);
								if (!PathResult.bSuccess) { AddResult(FailedArr, TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName), PathResult.ErrorMessage, TEXT("error")); continue; }
								FString CurrentValue;
								PathResult.Prop->ExportText_Direct(CurrentValue, PathResult.ValuePtr, PathResult.ValuePtr, ExistingNode->ComponentTemplate, PPF_None);
								if (CurrentValue == PropValue)
								{
									AddUnchanged(TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName));
								}
								else
								{
									TArray<uint8> TmpBuf;
									TmpBuf.SetNumZeroed(PathResult.Prop->GetSize());
									PathResult.Prop->InitializeValue(TmpBuf.GetData());
									bool bCanParse = PathResult.Prop->ImportText_Direct(*PropValue, TmpBuf.GetData(), ExistingNode->ComponentTemplate, PPF_None) != nullptr;
									PathResult.Prop->DestroyValue(TmpBuf.GetData());
									if (bCanParse)
										AddResult(UpdatedArr, TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName), TEXT("would_update"), TEXT("diff_status"));
									else
										AddResult(FailedArr, TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName), FString::Printf(TEXT("cannot parse '%s' for type %s"), *PropValue, *PathResult.Prop->GetCPPType()), TEXT("error"));
								}
								continue;
							}

							FPropertyPathResult PathResult = NavigatePropertyPath(ExistingNode->ComponentTemplate->GetClass(), ExistingNode->ComponentTemplate, PropName);
							if (!PathResult.bSuccess) { AddFailed(TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName), PathResult.ErrorMessage); continue; }

							FString OldValue;
							PathResult.Prop->ExportText_Direct(OldValue, PathResult.ValuePtr, PathResult.ValuePtr, ExistingNode->ComponentTemplate, PPF_None);
							if (OldValue == PropValue) { AddUnchanged(TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName)); continue; }

							if (PathResult.Prop->ImportText_Direct(*PropValue, PathResult.ValuePtr, ExistingNode->ComponentTemplate, PPF_None))
								AddUpdated(TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName));
							else
								AddFailed(TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName), TEXT("parse failed"));
						}
					}
					continue;
				}

				if (bDryRun) { AddCreated(TEXT("component"), CompName); continue; }

				// Create component via add_component sub-call
				TSharedRef<FJsonObject> CompParams = MakeShared<FJsonObject>();
				CompParams->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
				CompParams->SetStringField(TEXT("operation"), TEXT("add_component"));
				CompParams->SetStringField(TEXT("component_name"), CompName);
				CompParams->SetStringField(TEXT("component_class"), CompClass);
				FString ParentComp;
				if ((*CompObj)->TryGetStringField(TEXT("parent"), ParentComp))
					CompParams->SetStringField(TEXT("parent_component"), ParentComp);

				FMCPToolResult CompResult = ExecuteAddComponent(CompParams);
				if (CompResult.bSuccess)
				{
					AddCreated(TEXT("component"), CompName);

					// Apply component properties if specified
					const TArray<TSharedPtr<FJsonValue>>* CompProps = nullptr;
					if ((*CompObj)->TryGetArrayField(TEXT("properties"), CompProps) && CompProps->Num() > 0)
					{
						USCS_Node* NewNode = SCS->FindSCSNode(FName(*CompName));
						if (NewNode && NewNode->ComponentTemplate)
						{
							for (const TSharedPtr<FJsonValue>& PropVal : *CompProps)
							{
								TotalOps++;
								LowLevelCallsSaved++;
								const TSharedPtr<FJsonObject>* PropObj; if (!PropVal->TryGetObject(PropObj)) continue;
								FString PropName, PropValue;
								(*PropObj)->TryGetStringField(TEXT("name"), PropName);
								(*PropObj)->TryGetStringField(TEXT("value"), PropValue);
								if (PropName.IsEmpty()) continue;

								FPropertyPathResult PathResult = NavigatePropertyPath(NewNode->ComponentTemplate->GetClass(), NewNode->ComponentTemplate, PropName);
								if (!PathResult.bSuccess) { AddFailed(TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName), PathResult.ErrorMessage); continue; }

								if (PathResult.Prop->ImportText_Direct(*PropValue, PathResult.ValuePtr, NewNode->ComponentTemplate, PPF_None))
									AddUpdated(TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName));
								else
									AddFailed(TEXT("component_property"), FString::Printf(TEXT("%s.%s"), *CompName, *PropName), TEXT("parse failed"));
							}
						}
					}
				}
				else
					AddFailed(TEXT("component"), CompName, CompResult.Message);
			}
		}
	}

	// ===== Phase 3: Class defaults =====
	const TArray<TSharedPtr<FJsonValue>>* DefaultsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("defaults"), DefaultsArray))
	{
		// Need compile first so CDO exists with variables
		if (!bDryRun && CreatedArr.Num() > 0)
		{
			if (Context.Blueprint->DelegateSignatureGraphs.Num() == 0)
			{
				CompileCount++;
				FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
			}
		}

		UClass* GenClass = Context.Blueprint->GeneratedClass;
		UObject* CDO = GenClass ? GenClass->GetDefaultObject() : nullptr;

		if (!GenClass || !CDO)
		{
			for (const auto& D : *DefaultsArray) { TotalOps++; LowLevelCallsSaved++; AddFailed(TEXT("default"), TEXT("?"), TEXT("No GeneratedClass/CDO")); }
		}
		else
		{
			for (const TSharedPtr<FJsonValue>& DefVal : *DefaultsArray)
			{
				TotalOps++;
				LowLevelCallsSaved++; // would be set_class_default
				const TSharedPtr<FJsonObject>* DefObj; if (!DefVal->TryGetObject(DefObj)) continue;
				FString PropName, PropValue;
				(*DefObj)->TryGetStringField(TEXT("name"), PropName);
				(*DefObj)->TryGetStringField(TEXT("value"), PropValue);
				if (PropName.IsEmpty()) continue;

				FPropertyPathResult PathResult = NavigatePropertyPath(GenClass, CDO, PropName);
				if (!PathResult.bSuccess) { AddFailed(TEXT("default"), PropName, PathResult.ErrorMessage); continue; }

				FString OldValue;
				PathResult.Prop->ExportText_Direct(OldValue, PathResult.ValuePtr, PathResult.ValuePtr, CDO, PPF_None);

				if (bDryRun)
				{
					// Deep dry_run: compare desired vs current value + validate parseability
					if (OldValue == PropValue)
					{
						AddUnchanged(TEXT("default"), PropName);
					}
					else
					{
						// Validate parseability before claiming would_update
						TArray<uint8> TempBuf;
						TempBuf.SetNumZeroed(PathResult.Prop->GetSize());
						PathResult.Prop->InitializeValue(TempBuf.GetData());
						bool bParseable = PathResult.Prop->ImportText_Direct(*PropValue, TempBuf.GetData(), CDO, PPF_None) != nullptr;
						PathResult.Prop->DestroyValue(TempBuf.GetData());
						if (bParseable)
							AddResult(UpdatedArr, TEXT("default"), PropName, TEXT("would_update"), TEXT("diff_status"));
						else
							AddFailed(TEXT("default"), PropName, FString::Printf(TEXT("cannot parse '%s' for type %s"), *PropValue, *PathResult.Prop->GetCPPType()));
					}
					continue;
				}

				if (OldValue == PropValue) { AddUnchanged(TEXT("default"), PropName); continue; }

				if (PathResult.Prop->ImportText_Direct(*PropValue, PathResult.ValuePtr, CDO, PPF_None))
					AddUpdated(TEXT("default"), PropName);
				else
					AddFailed(TEXT("default"), PropName, FString::Printf(TEXT("parse failed for type %s"), *PathResult.Prop->GetCPPType()));
			}
			if (!bDryRun) CDO->PostEditChange();
		}
	}

	// ===== Phase 4: AnimBP assignment =====
	FString AnimBPPath = ExtractOptionalString(Params, TEXT("anim_blueprint_path"));
	if (!AnimBPPath.IsEmpty())
	{
		TotalOps++;
		LowLevelCallsSaved++; // would be set_anim_blueprint
		if (bDryRun)
		{
			// Deep dry_run: check if AnimBP is already assigned
			USimpleConstructionScript* AnimSCS = Context.Blueprint->SimpleConstructionScript;
			bool bAlreadyAssigned = false;
			if (AnimSCS)
			{
				FString AnimCompName = ExtractOptionalString(Params, TEXT("anim_component_name"));
				for (USCS_Node* Node : AnimSCS->GetAllNodes())
				{
					if (!Node || !Node->ComponentClass || !Node->ComponentClass->IsChildOf(USkeletalMeshComponent::StaticClass())) continue;
					if (!AnimCompName.IsEmpty() && Node->GetVariableName().ToString() != AnimCompName) continue;
					USkeletalMeshComponent* MeshComp = Cast<USkeletalMeshComponent>(Node->ComponentTemplate);
					if (MeshComp && MeshComp->AnimClass)
					{
						UAnimBlueprint* DesiredABP = LoadObject<UAnimBlueprint>(nullptr, *AnimBPPath);
						if (DesiredABP && DesiredABP->GeneratedClass == MeshComp->AnimClass)
							bAlreadyAssigned = true;
					}
					break;
				}
			}
			if (bAlreadyAssigned)
				AddUnchanged(TEXT("anim_blueprint"), AnimBPPath);
			else
				AddResult(UpdatedArr, TEXT("anim_blueprint"), AnimBPPath, TEXT("would_update"), TEXT("diff_status"));
		}
		else
		{
			TSharedRef<FJsonObject> AnimParams = MakeShared<FJsonObject>();
			AnimParams->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
			AnimParams->SetStringField(TEXT("anim_blueprint_path"), AnimBPPath);
			FString CompName = ExtractOptionalString(Params, TEXT("anim_component_name"));
			if (!CompName.IsEmpty()) AnimParams->SetStringField(TEXT("component_name"), CompName);

			FMCPToolResult AnimResult = ExecuteSetAnimBlueprint(AnimParams);
			if (AnimResult.bSuccess)
				AddUpdated(TEXT("anim_blueprint"), AnimBPPath);
			else
				AddFailed(TEXT("anim_blueprint"), AnimBPPath, AnimResult.Message);
		}
	}

	// ===== Finalize: compile once =====
	bool bSkippedCompile = false;
	if (!bDryRun)
	{
		if (Context.Blueprint->DelegateSignatureGraphs.Num() > 0)
		{
			bSkippedCompile = true;
			FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
		}
		else if (CreatedArr.Num() > 0 || UpdatedArr.Num() > 0)
		{
			CompileCount++;
			FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
			FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
		}
	}

	bool bHasFailures = FailedArr.Num() > 0;
	ResultData->SetBoolField(TEXT("dry_run"), bDryRun);
	ResultData->SetArrayField(TEXT("created"), CreatedArr);
	ResultData->SetArrayField(TEXT("updated"), UpdatedArr);
	ResultData->SetArrayField(TEXT("unchanged"), UnchangedArr);
	ResultData->SetArrayField(TEXT("failed"), FailedArr);
	ResultData->SetArrayField(TEXT("skipped"), SkippedArr);
	ResultData->SetNumberField(TEXT("created_count"), CreatedArr.Num());
	ResultData->SetNumberField(TEXT("updated_count"), UpdatedArr.Num());
	ResultData->SetNumberField(TEXT("unchanged_count"), UnchangedArr.Num());
	ResultData->SetNumberField(TEXT("failed_count"), FailedArr.Num());
	ResultData->SetNumberField(TEXT("skipped_count"), SkippedArr.Num());
	ResultData->SetNumberField(TEXT("total_operations"), TotalOps);
	ResultData->SetNumberField(TEXT("compile_count"), CompileCount);
	ResultData->SetNumberField(TEXT("low_level_calls_saved"), LowLevelCallsSaved);
	if (bHasFailures) ResultData->SetBoolField(TEXT("partial_success"), true);
	if (bSkippedCompile) ResultData->SetStringField(TEXT("compile_note"), TEXT("Compile skipped: BP has delegate signature graphs"));

	// Emit receipt
	FExecutionReceipt Receipt;
	Receipt.Tool = TEXT("blueprint_modify:configure_actor_class");
	Receipt.bSuccess = !bHasFailures;
	Receipt.TargetType = TEXT("blueprint");
	Receipt.Targets.Add(Context.Blueprint->GetPathName());
	Receipt.Classification = TEXT("user_mutation");
	Receipt.Status = bHasFailures ? TEXT("partial_success") : TEXT("success");
	Receipt.Summary = FString::Printf(TEXT("configure_actor_class: %d created, %d updated, %d failed. %d low-level calls saved."),
		CreatedArr.Num(), UpdatedArr.Num(), FailedArr.Num(), LowLevelCallsSaved);
	FUnrealClaudeExecutionLog::Get().AddReceipt(Receipt);

	FMCPToolResult Result;
	Result.bSuccess = !bHasFailures;
	Result.Message = FString::Printf(TEXT("%s actor class: %d created, %d updated, %d unchanged, %d failed, %d skipped. Compiles: %d. Calls saved: %d"),
		bDryRun ? TEXT("Preflight") : TEXT("Configured"),
		CreatedArr.Num(), UpdatedArr.Num(), UnchangedArr.Num(), FailedArr.Num(), SkippedArr.Num(), CompileCount, LowLevelCallsSaved);
	Result.Data = ResultData;
	return Result;
}

// ===== Composed Authoring =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteApplyBlueprintSpec(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params)) return LoadError.GetValue();

	bool bDryRun = ExtractOptionalBool(Params, TEXT("dry_run"), false);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CreatedArr, UpdatedArr, UnchangedArr, FailedArr, SkippedArr;
	int32 TotalOps = 0;
	int32 CompileCount = 0;
	bool bCreatedDispatchers = false; // track if dispatchers were created — skip final compile to avoid UE 5.7 crash

	auto AddResult = [](TArray<TSharedPtr<FJsonValue>>& Arr, const FString& Kind, const FString& Name, const FString& Extra = TEXT(""), const FString& ExtraKey = TEXT("")) {
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("kind"), Kind); Obj->SetStringField(TEXT("name"), Name);
		if (!Extra.IsEmpty() && !ExtraKey.IsEmpty()) Obj->SetStringField(ExtraKey, Extra);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	};

	// In dry_run mode, created/updated results get a diff_status hint
	auto AddCreated = [&](const FString& Kind, const FString& Name) {
		if (bDryRun) AddResult(CreatedArr, Kind, Name, TEXT("would_create"), TEXT("diff_status"));
		else AddResult(CreatedArr, Kind, Name);
	};
	auto AddUpdated = [&](const FString& Kind, const FString& Name) {
		if (bDryRun) AddResult(UpdatedArr, Kind, Name, TEXT("would_update"), TEXT("diff_status"));
		else AddResult(UpdatedArr, Kind, Name);
	};
	auto AddUnchanged = [&](const FString& Kind, const FString& Name) { AddResult(UnchangedArr, Kind, Name); };
	auto AddFailed = [&](const FString& Kind, const FString& Name, const FString& Reason) { AddResult(FailedArr, Kind, Name, Reason, TEXT("error")); };
	auto AddSkipped = [&](const FString& Kind, const FString& Name, const FString& Reason) { AddResult(SkippedArr, Kind, Name, Reason, TEXT("reason")); };

	// Variables
	const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("variables"), VarsArray))
	{
		for (const TSharedPtr<FJsonValue>& VarVal : *VarsArray)
		{
			TotalOps++;
			const TSharedPtr<FJsonObject>* VarObj; if (!VarVal->TryGetObject(VarObj)) continue;
			FString VarName, VarType;
			(*VarObj)->TryGetStringField(TEXT("name"), VarName);
			(*VarObj)->TryGetStringField(TEXT("type"), VarType);
			if (VarName.IsEmpty() || VarType.IsEmpty()) { AddFailed(TEXT("variable"), VarName, TEXT("missing name or type")); continue; }

			FEdGraphPinType PinType; FString TypeError;
			if (!FBlueprintUtils::ParsePinType(VarType, PinType, TypeError)) { AddFailed(TEXT("variable"), VarName, TypeError); continue; }

			// Check existence AND type match for truthful unchanged
			bool bExists = false;
			bool bTypeMatch = false;
			for (const FBPVariableDescription& V : Context.Blueprint->NewVariables)
			{
				if (V.VarName == FName(*VarName))
				{
					bExists = true;
					bTypeMatch = (V.VarType.PinCategory == PinType.PinCategory
						&& V.VarType.PinSubCategoryObject == PinType.PinSubCategoryObject
						&& V.VarType.ContainerType == PinType.ContainerType);
					break;
				}
			}

			if (bExists && bTypeMatch) { AddUnchanged(TEXT("variable"), VarName); continue; }
			if (bExists && !bTypeMatch) { AddSkipped(TEXT("variable"), VarName, TEXT("exists with different type")); continue; }
			if (bDryRun) { AddCreated(TEXT("variable"), VarName); continue; }

			FString AddError;
			if (FBlueprintUtils::AddVariable(Context.Blueprint, VarName, PinType, AddError))
				AddCreated(TEXT("variable"), VarName);
			else
				AddFailed(TEXT("variable"), VarName, AddError);
		}
	}

	// Functions
	const TArray<TSharedPtr<FJsonValue>>* FuncsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("functions"), FuncsArray))
	{
		for (const TSharedPtr<FJsonValue>& FuncVal : *FuncsArray)
		{
			TotalOps++;
			const TSharedPtr<FJsonObject>* FuncObj; if (!FuncVal->TryGetObject(FuncObj)) continue;
			FString FuncName;
			(*FuncObj)->TryGetStringField(TEXT("name"), FuncName);
			if (FuncName.IsEmpty()) { AddFailed(TEXT("function"), FuncName, TEXT("missing name")); continue; }

			bool bExists = false;
			for (UEdGraph* G : Context.Blueprint->FunctionGraphs) { if (G && G->GetName() == FuncName) { bExists = true; break; } }

			if (bExists)
			{
				const TArray<TSharedPtr<FJsonValue>>* SigArray = nullptr;
				if ((*FuncObj)->TryGetArrayField(TEXT("parameters"), SigArray) && SigArray->Num() > 0)
				{
					if (bDryRun) { AddUpdated(TEXT("function_signature"), FuncName); continue; }
					// Skip signature update if BP has delegate graphs — nested compile crashes UE 5.7
					if (Context.Blueprint->DelegateSignatureGraphs.Num() > 0)
					{
						AddSkipped(TEXT("function_signature"), FuncName, TEXT("compile unsafe: BP has delegate signature graphs"));
						continue;
					}
					TSharedRef<FJsonObject> SigParams = MakeShared<FJsonObject>();
					SigParams->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
					SigParams->SetStringField(TEXT("function_name"), FuncName);
					SigParams->SetArrayField(TEXT("parameters"), *const_cast<TArray<TSharedPtr<FJsonValue>>*>(SigArray));
					FMCPToolResult SigResult = ExecuteSetFunctionSignature(SigParams);
					CompileCount++;
					if (SigResult.bSuccess)
						AddUpdated(TEXT("function_signature"), FuncName);
					else
						AddFailed(TEXT("function_signature"), FuncName, SigResult.Message);
				}
				else
				{
					AddUnchanged(TEXT("function"), FuncName);
				}
				continue;
			}

			if (bDryRun) { AddCreated(TEXT("function"), FuncName); continue; }

			FString AddError;
			if (FBlueprintUtils::AddFunction(Context.Blueprint, FuncName, AddError))
			{
				AddCreated(TEXT("function"), FuncName);
				const TArray<TSharedPtr<FJsonValue>>* SigArray = nullptr;
				if ((*FuncObj)->TryGetArrayField(TEXT("parameters"), SigArray) && SigArray->Num() > 0)
				{
					// Skip signature if BP has delegate graphs — nested compile crashes UE 5.7
					if (Context.Blueprint->DelegateSignatureGraphs.Num() > 0)
					{
						AddSkipped(TEXT("function_signature"), FuncName, TEXT("compile unsafe: BP has delegate signature graphs"));
					}
					else
					{
						TSharedRef<FJsonObject> SigParams = MakeShared<FJsonObject>();
						SigParams->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
						SigParams->SetStringField(TEXT("function_name"), FuncName);
						SigParams->SetArrayField(TEXT("parameters"), *const_cast<TArray<TSharedPtr<FJsonValue>>*>(SigArray));
						FMCPToolResult SigResult = ExecuteSetFunctionSignature(SigParams);
						CompileCount++;
						if (!SigResult.bSuccess)
							AddFailed(TEXT("function_signature"), FuncName, SigResult.Message);
					}
				}
			}
			else
				AddFailed(TEXT("function"), FuncName, AddError);
		}
	}

	// Custom events (idempotent: check if event with same name exists)
	const TArray<TSharedPtr<FJsonValue>>* EventsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("custom_events"), EventsArray))
	{
		FString GraphError;
		UEdGraph* EventGraph = FBlueprintGraphEditor::FindGraph(Context.Blueprint, TEXT(""), false, GraphError);

		for (const TSharedPtr<FJsonValue>& EvtVal : *EventsArray)
		{
			TotalOps++;
			const TSharedPtr<FJsonObject>* EvtObj; if (!EvtVal->TryGetObject(EvtObj)) continue;
			FString EvtName;
			(*EvtObj)->TryGetStringField(TEXT("name"), EvtName);
			if (EvtName.IsEmpty()) { AddFailed(TEXT("custom_event"), EvtName, TEXT("missing name")); continue; }

			if (!EventGraph) { AddFailed(TEXT("custom_event"), EvtName, GraphError); continue; }

			// Check if CustomEvent with this name already exists
			bool bEventExists = false;
			for (UEdGraphNode* Node : EventGraph->Nodes)
			{
				if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
				{
					if (CE->CustomFunctionName.ToString() == EvtName) { bEventExists = true; break; }
				}
			}
			if (bEventExists) { AddUnchanged(TEXT("custom_event"), EvtName); continue; }
			if (bDryRun) { AddCreated(TEXT("custom_event"), EvtName); continue; }

			TSharedPtr<FJsonObject> EvtNodeParams = MakeShared<FJsonObject>();
			EvtNodeParams->SetStringField(TEXT("event_name"), EvtName);
			FString NodeId, CreateError;
			UEdGraphNode* Node = FBlueprintGraphEditor::CreateNode(EventGraph, TEXT("CustomEvent"), EvtNodeParams.ToSharedRef(), 0, 0, NodeId, CreateError);
			if (Node)
				AddCreated(TEXT("custom_event"), EvtName);
			else
				AddFailed(TEXT("custom_event"), EvtName, CreateError);
		}
	}

	// Interfaces (idempotent: check if already implemented)
	const TArray<TSharedPtr<FJsonValue>>* IntArray = nullptr;
	if (Params->TryGetArrayField(TEXT("interfaces"), IntArray))
	{
		for (const TSharedPtr<FJsonValue>& IntVal : *IntArray)
		{
			TotalOps++;
			FString IntPath;
			if (!IntVal->TryGetString(IntPath) || IntPath.IsEmpty()) continue;

			// Check if interface is already implemented
			bool bAlreadyImpl = false;
			for (const FBPInterfaceDescription& Desc : Context.Blueprint->ImplementedInterfaces)
			{
				if (Desc.Interface && Desc.Interface->GetPathName().Contains(IntPath)) { bAlreadyImpl = true; break; }
			}
			if (bAlreadyImpl) { AddUnchanged(TEXT("interface"), IntPath); continue; }
			if (bDryRun) { AddCreated(TEXT("interface"), IntPath); continue; }

			TSharedRef<FJsonObject> IntParams = MakeShared<FJsonObject>();
			IntParams->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
			IntParams->SetStringField(TEXT("interface_path"), IntPath);
			FMCPToolResult IntResult = ExecuteAddInterface(IntParams);
			CompileCount++;
			if (IntResult.bSuccess)
				AddCreated(TEXT("interface"), IntPath);
			else
				AddFailed(TEXT("interface"), IntPath, IntResult.Message);
		}
	}

	// Dispatchers (idempotent: skip if exists)
	const TArray<TSharedPtr<FJsonValue>>* DispArray = nullptr;
	if (Params->TryGetArrayField(TEXT("dispatchers"), DispArray))
	{
		for (const TSharedPtr<FJsonValue>& DispVal : *DispArray)
		{
			TotalOps++;
			const TSharedPtr<FJsonObject>* DispObj; if (!DispVal->TryGetObject(DispObj)) continue;
			FString DispName;
			(*DispObj)->TryGetStringField(TEXT("name"), DispName);
			if (DispName.IsEmpty()) { AddFailed(TEXT("dispatcher"), DispName, TEXT("missing name")); continue; }

			bool bExists = false;
			for (UEdGraph* G : Context.Blueprint->DelegateSignatureGraphs)
			{
				if (G && G->GetName().Contains(DispName)) { bExists = true; break; }
			}
			if (bExists) { AddUnchanged(TEXT("dispatcher"), DispName); continue; }
			if (bDryRun) { AddCreated(TEXT("dispatcher"), DispName); continue; }

			UEdGraph* DispSigGraph = FBlueprintEditorUtils::CreateNewGraph(Context.Blueprint, FName(*DispName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (DispSigGraph)
			{
				DispSigGraph->bAllowDeletion = false;
				Context.Blueprint->DelegateSignatureGraphs.Add(DispSigGraph);
				UK2Node_FunctionEntry* DispEntry = NewObject<UK2Node_FunctionEntry>(DispSigGraph);
				DispEntry->CreateNewGuid(); DispEntry->PostPlacedNewNode(); DispEntry->AllocateDefaultPins();
				DispSigGraph->AddNode(DispEntry);
			}
			AddCreated(TEXT("dispatcher"), DispName);
			bCreatedDispatchers = true;
		}
	}

	// Macros (idempotent: skip if exists)
	const TArray<TSharedPtr<FJsonValue>>* MacrosArray = nullptr;
	if (Params->TryGetArrayField(TEXT("macros"), MacrosArray))
	{
		for (const TSharedPtr<FJsonValue>& MacroVal : *MacrosArray)
		{
			TotalOps++;
			const TSharedPtr<FJsonObject>* MacroObj; if (!MacroVal->TryGetObject(MacroObj)) continue;
			FString MacroName;
			(*MacroObj)->TryGetStringField(TEXT("name"), MacroName);
			if (MacroName.IsEmpty()) { AddFailed(TEXT("macro"), MacroName, TEXT("missing name")); continue; }

			bool bExists = false;
			for (UEdGraph* G : Context.Blueprint->MacroGraphs)
			{
				if (G && G->GetName() == MacroName) { bExists = true; break; }
			}
			if (bExists) { AddUnchanged(TEXT("macro"), MacroName); continue; }
			if (bDryRun) { AddCreated(TEXT("macro"), MacroName); continue; }

			UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
				Context.Blueprint, FName(*MacroName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (MacroGraph)
			{
				FBlueprintEditorUtils::AddMacroGraph(Context.Blueprint, MacroGraph, false, nullptr);
				AddCreated(TEXT("macro"), MacroName);
			}
			else
				AddFailed(TEXT("macro"), MacroName, TEXT("graph creation failed"));
		}
	}

	// Compile and finalize (skip in dry_run; skip when dispatchers were created — UE 5.7 crashes on compile with delegate graphs)
	bool bSkippedCompile = false;
	if (!bDryRun)
	{
		if (bCreatedDispatchers || Context.Blueprint->DelegateSignatureGraphs.Num() > 0)
		{
			bSkippedCompile = true;
			FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
			ResultData->SetStringField(TEXT("compile_note"), TEXT("Compile skipped: dispatchers were created. UE 5.7 crashes on CompileBlueprint with delegate signature graphs. Save and reopen BP in editor."));
		}
		else
		{
			CompileCount++;
			Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
			FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);
			TSharedPtr<FJsonObject> CompileData = Context.BuildResultJson();
			for (const auto& Field : CompileData->Values)
			{
				ResultData->SetField(Field.Key, FJsonValue::Duplicate(Field.Value));
			}
		}
	}

	bool bCompileFailed = !bDryRun && !bSkippedCompile && !Context.CompileResult.bSuccess;
	bool bHasFailures = FailedArr.Num() > 0;

	ResultData->SetBoolField(TEXT("dry_run"), bDryRun);
	ResultData->SetArrayField(TEXT("created"), CreatedArr);
	ResultData->SetArrayField(TEXT("updated"), UpdatedArr);
	ResultData->SetArrayField(TEXT("unchanged"), UnchangedArr);
	ResultData->SetArrayField(TEXT("failed"), FailedArr);
	ResultData->SetArrayField(TEXT("skipped"), SkippedArr);
	ResultData->SetNumberField(TEXT("created_count"), CreatedArr.Num());
	ResultData->SetNumberField(TEXT("updated_count"), UpdatedArr.Num());
	ResultData->SetNumberField(TEXT("unchanged_count"), UnchangedArr.Num());
	ResultData->SetNumberField(TEXT("failed_count"), FailedArr.Num());
	ResultData->SetNumberField(TEXT("skipped_count"), SkippedArr.Num());
	ResultData->SetNumberField(TEXT("total_operations"), TotalOps);
	ResultData->SetNumberField(TEXT("compile_count"), CompileCount);
	ResultData->SetBoolField(TEXT("partial_success"), bHasFailures || bCompileFailed);

	FMCPToolResult Result;
	Result.bSuccess = !bCompileFailed && !bHasFailures;
	Result.Message = FString::Printf(TEXT("%s Blueprint spec: %d created, %d updated, %d unchanged, %d failed, %d skipped. Compiles: %d. %s"),
		bDryRun ? TEXT("Preflight") : TEXT("Applied"),
		CreatedArr.Num(), UpdatedArr.Num(), UnchangedArr.Num(), FailedArr.Num(), SkippedArr.Num(), CompileCount,
		bDryRun ? TEXT("(dry run — no mutations)") : (bCompileFailed ? TEXT("Final: FAILED") : TEXT("Final: ok")));
	Result.Data = ResultData;
	return Result;
}

// ===== Component Operations =====

static USimpleConstructionScript* GetSCSFromContext(FMCPBlueprintLoadContext& Context, FString& OutError)
{
	if (!Context.Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}
	USimpleConstructionScript* SCS = Context.Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		OutError = FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript"), *Context.Blueprint->GetName());
	}
	return SCS;
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddComponent(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString ComponentClass;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("component_class"), ComponentClass, Error))
	{
		return Error.GetValue();
	}

	FString ComponentName = ExtractOptionalString(Params, TEXT("component_name"));
	FString ParentComponent = ExtractOptionalString(Params, TEXT("parent_component"));

	FString SCSError;
	USimpleConstructionScript* SCS = GetSCSFromContext(Context, SCSError);
	if (!SCS) return FMCPToolResult::Error(SCSError);

	// Find component class
	UClass* CompClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *ComponentClass));
	if (!CompClass)
	{
		CompClass = FindObject<UClass>(nullptr, *ComponentClass);
	}
	if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Component class not found or not a component: %s"), *ComponentClass));
	}

	// Create SCS node
	FName NewName = ComponentName.IsEmpty() ? NAME_None : FName(*ComponentName);
	USCS_Node* NewNode = SCS->CreateNode(CompClass, NewName);
	if (!NewNode)
	{
		return FMCPToolResult::Error(TEXT("Failed to create SCS node"));
	}

	// Attach to parent or root
	if (!ParentComponent.IsEmpty())
	{
		USCS_Node* ParentNode = SCS->FindSCSNode(FName(*ParentComponent));
		if (ParentNode)
		{
			ParentNode->AddChildNode(NewNode);
		}
		else
		{
			SCS->AddNode(NewNode);
		}
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	// Compile
	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("component_name"), NewNode->GetVariableName().ToString());
	ResultData->SetStringField(TEXT("component_class"), CompClass->GetName());
	ResultData->SetBoolField(TEXT("is_root"), NewNode->IsRootNode());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added component '%s' (%s)"), *NewNode->GetVariableName().ToString(), *CompClass->GetName()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveComponent(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString ComponentName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("component_name"), ComponentName, Error))
	{
		return Error.GetValue();
	}

	FString SCSError;
	USimpleConstructionScript* SCS = GetSCSFromContext(Context, SCSError);
	if (!SCS) return FMCPToolResult::Error(SCSError);

	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' not found"), *ComponentName));
	}

	// Check if inherited
	if (Node->bIsParentComponentNative || Node->ParentComponentOwnerClassName != NAME_None)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Cannot remove inherited/native component '%s'"), *ComponentName));
	}

	SCS->RemoveNodeAndPromoteChildren(Node);

	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("removed_component"), ComponentName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed component '%s'"), *ComponentName),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRenameComponent(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString ComponentName, NewName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("component_name"), ComponentName, Error))
	{
		return Error.GetValue();
	}
	if (!ExtractRequiredString(Params, TEXT("new_name"), NewName, Error))
	{
		return Error.GetValue();
	}

	FString SCSError;
	USimpleConstructionScript* SCS = GetSCSFromContext(Context, SCSError);
	if (!SCS) return FMCPToolResult::Error(SCSError);

	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' not found"), *ComponentName));
	}

	FString OldName = Node->GetVariableName().ToString();
	Node->SetVariableName(FName(*NewName), true);

	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("old_name"), OldName);
	ResultData->SetStringField(TEXT("new_name"), Node->GetVariableName().ToString());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Renamed '%s' -> '%s'"), *OldName, *Node->GetVariableName().ToString()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteReparentComponent(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString ComponentName, NewParentName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("component_name"), ComponentName, Error))
	{
		return Error.GetValue();
	}
	if (!ExtractRequiredString(Params, TEXT("new_parent_component"), NewParentName, Error))
	{
		return Error.GetValue();
	}

	FString SCSError;
	USimpleConstructionScript* SCS = GetSCSFromContext(Context, SCSError);
	if (!SCS) return FMCPToolResult::Error(SCSError);

	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' not found"), *ComponentName));
	}

	USCS_Node* NewParent = SCS->FindSCSNode(FName(*NewParentName));
	if (!NewParent)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("New parent component '%s' not found"), *NewParentName));
	}

	// Remove from current parent
	USCS_Node* OldParent = SCS->FindParentNode(Node);
	if (OldParent)
	{
		OldParent->RemoveChildNode(Node, false);
	}
	else
	{
		SCS->RemoveNode(Node, false);
	}

	// Add to new parent
	NewParent->AddChildNode(Node, false);

	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("component_name"), ComponentName);
	ResultData->SetStringField(TEXT("new_parent"), NewParentName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Reparented '%s' under '%s'"), *ComponentName, *NewParentName),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetComponentProperty(const TSharedRef<FJsonObject>& Params)
{
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString ComponentName, PropertyName, PropertyValue;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("component_name"), ComponentName, Error))
	{
		return Error.GetValue();
	}
	if (!ExtractRequiredString(Params, TEXT("property_name"), PropertyName, Error))
	{
		return Error.GetValue();
	}
	if (!ExtractRequiredString(Params, TEXT("property_value"), PropertyValue, Error))
	{
		return Error.GetValue();
	}

	FString SCSError;
	USimpleConstructionScript* SCS = GetSCSFromContext(Context, SCSError);
	if (!SCS) return FMCPToolResult::Error(SCSError);

	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node || !Node->ComponentTemplate)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' not found or has no template"), *ComponentName));
	}

	// Use FProperty reflection to set value
	UObject* Template = Node->ComponentTemplate;
	FProperty* Prop = Template->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Property '%s' not found on component '%s' (%s)"), *PropertyName, *ComponentName, *Template->GetClass()->GetName()));
	}

	// Try to set from string
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);
	if (!Prop->ImportText_Direct(*PropertyValue, ValuePtr, Template, PPF_None))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to set property '%s' to '%s' (type: %s)"), *PropertyName, *PropertyValue, *Prop->GetCPPType()));
	}

	Context.CompileResult = FBlueprintLoader::CompileBlueprintWithResult(Context.Blueprint);
	FBlueprintUtils::MarkBlueprintDirty(Context.Blueprint);

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("component_name"), ComponentName);
	ResultData->SetStringField(TEXT("property_name"), PropertyName);
	ResultData->SetStringField(TEXT("property_value"), PropertyValue);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set '%s.%s' = '%s'"), *ComponentName, *PropertyName, *PropertyValue),
		ResultData
	);
}
