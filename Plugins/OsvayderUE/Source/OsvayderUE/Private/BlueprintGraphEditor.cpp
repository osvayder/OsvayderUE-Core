// Copyright Natali Caggiano. All Rights Reserved.

#include "BlueprintGraphEditor.h"
#include "OsvayderUEModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_BaseMCDelegate.h"
#include "EdGraphSchema_K2.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "HAL/PlatformAtomics.h"

// Static member initialization
volatile int32 FBlueprintGraphEditor::NodeIdCounter = 0;
const FString FBlueprintGraphEditor::NodeIdPrefix = TEXT("MCP_ID:");

// ===== Graph Finding =====

UEdGraph* FBlueprintGraphEditor::FindGraph(
	UBlueprint* Blueprint,
	const FString& GraphName,
	bool bFunctionGraph,
	FString& OutError,
	bool bMacroGraph)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	// Get the appropriate graph array (UE 5.7 uses TObjectPtr)
	auto& Graphs = bMacroGraph ? Blueprint->MacroGraphs
		: (bFunctionGraph ? Blueprint->FunctionGraphs : Blueprint->UbergraphPages);

	FString GraphKind = bMacroGraph ? TEXT("macro") : (bFunctionGraph ? TEXT("function") : TEXT("event"));

	// If no name specified, return the first graph (default)
	if (GraphName.IsEmpty())
	{
		if (Graphs.Num() > 0 && Graphs[0])
		{
			return Graphs[0];
		}
		OutError = FString::Printf(TEXT("No %s graphs found"), *GraphKind);
		return nullptr;
	}

	// Search by name
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Build list of available graphs for error message
	TArray<FString> AvailableGraphs;
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph)
		{
			AvailableGraphs.Add(Graph->GetName());
		}
	}

	OutError = FString::Printf(TEXT("%s graph '%s' not found. Available: %s"),
		*GraphKind, *GraphName,
		*FString::Join(AvailableGraphs, TEXT(", ")));
	return nullptr;
}

// ===== Node Management =====

UEdGraphNode* FBlueprintGraphEditor::CreateNode(
	UEdGraph* Graph,
	const FString& NodeType,
	const TSharedPtr<FJsonObject>& NodeParams,
	int32 PosX,
	int32 PosY,
	FString& OutNodeId,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return nullptr;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!Blueprint)
	{
		OutError = TEXT("Could not find Blueprint for graph");
		return nullptr;
	}

	UEdGraphNode* NewNode = nullptr;
	FString Context;

	// Dispatch to appropriate creation function
	if (NodeType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
	{
		FString FunctionName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("function")) : TEXT("");
		FString TargetClass = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("target_class")) : TEXT("");
		Context = FunctionName;
		NewNode = CreateCallFunctionNode(Graph, FunctionName, TargetClass, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Branch"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("IfThenElse"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateBranchNode(Graph, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
	{
		FString EventName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("event")) : TEXT("");
		Context = EventName;
		NewNode = CreateEventNode(Graph, EventName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("GetVariable"), ESearchCase::IgnoreCase))
	{
		FString VariableName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("variable")) : TEXT("");
		Context = VariableName;
		NewNode = CreateVariableGetNode(Graph, Blueprint, VariableName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SetVariable"), ESearchCase::IgnoreCase))
	{
		FString VariableName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("variable")) : TEXT("");
		Context = VariableName;
		NewNode = CreateVariableSetNode(Graph, Blueprint, VariableName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase))
	{
		int32 NumOutputs = NodeParams.IsValid() ? (int32)NodeParams->GetNumberField(TEXT("num_outputs")) : 2;
		if (NumOutputs < 2) NumOutputs = 2;
		NewNode = CreateSequenceNode(Graph, NumOutputs, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Add"), ESearchCase::IgnoreCase) ||
			 NodeType.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase) ||
			 NodeType.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase) ||
			 NodeType.Equals(TEXT("Divide"), ESearchCase::IgnoreCase))
	{
		Context = NodeType;
		NewNode = CreateMathNode(Graph, NodeType, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("PrintString"), ESearchCase::IgnoreCase))
	{
		// Convenience alias for CallFunction with PrintString
		Context = TEXT("PrintString");
		NewNode = CreateCallFunctionNode(Graph, TEXT("PrintString"), TEXT("KismetSystemLibrary"), PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
	{
		FString EventName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("event_name")) : TEXT("");
		Context = EventName;
		NewNode = CreateCustomEventNode(Graph, EventName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("CallDelegate"), ESearchCase::IgnoreCase))
	{
		FString DispatcherName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("dispatcher")) : TEXT("");
		Context = DispatcherName;
		NewNode = CreateDelegateNode<UK2Node_CallDelegate>(Graph, Blueprint, DispatcherName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("AddDelegate"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("BindDelegate"), ESearchCase::IgnoreCase))
	{
		FString DispatcherName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("dispatcher")) : TEXT("");
		Context = DispatcherName;
		NewNode = CreateDelegateNode<UK2Node_AddDelegate>(Graph, Blueprint, DispatcherName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("RemoveDelegate"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("UnbindDelegate"), ESearchCase::IgnoreCase))
	{
		FString DispatcherName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("dispatcher")) : TEXT("");
		Context = DispatcherName;
		NewNode = CreateDelegateNode<UK2Node_RemoveDelegate>(Graph, Blueprint, DispatcherName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("ClearDelegate"), ESearchCase::IgnoreCase))
	{
		FString DispatcherName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("dispatcher")) : TEXT("");
		Context = DispatcherName;
		NewNode = CreateDelegateNode<UK2Node_ClearDelegate>(Graph, Blueprint, DispatcherName, PosX, PosY, OutError);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown node type: '%s'. Supported: CallFunction, Branch, Event, CustomEvent, VariableGet, VariableSet, Sequence, Add, Subtract, Multiply, Divide, PrintString, CallDelegate, AddDelegate, RemoveDelegate, ClearDelegate"), *NodeType);
		return nullptr;
	}

	if (NewNode)
	{
		// Generate and set node ID
		OutNodeId = GenerateNodeId(NodeType, Context, Graph);
		SetNodeId(NewNode, OutNodeId);

		// Mark blueprint as modified
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		UE_LOG(LogOsvayderUE, Log, TEXT("Created node '%s' (type: %s) at (%d, %d)"), *OutNodeId, *NodeType, PosX, PosY);
	}

	return NewNode;
}

bool FBlueprintGraphEditor::DeleteNode(UEdGraph* Graph, const FString& NodeId, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	UEdGraphNode* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node '%s' not found"), *NodeId);
		return false;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);

	// Break all connections first
	Node->BreakAllNodeLinks();

	// Remove from graph
	Graph->RemoveNode(Node);

	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	UE_LOG(LogOsvayderUE, Log, TEXT("Deleted node '%s'"), *NodeId);
	return true;
}

UEdGraphNode* FBlueprintGraphEditor::FindNodeById(UEdGraph* Graph, const FString& NodeId)
{
	if (!Graph || NodeId.IsEmpty())
	{
		return nullptr;
	}

	// Fast path: if NodeId starts with "guid_", search by NodeGuid directly
	if (NodeId.StartsWith(TEXT("guid_")))
	{
		FString GuidStr = NodeId.RightChop(5); // skip "guid_"
		FGuid TargetGuid;
		if (FGuid::Parse(GuidStr, TargetGuid))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == TargetGuid)
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	// Deterministic fallback path: det_{ClassName}_{Index}
	if (NodeId.StartsWith(TEXT("det_")))
	{
		FString Rest = NodeId.RightChop(4);
		int32 LastUnderscore;
		if (Rest.FindLastChar('_', LastUnderscore))
		{
			FString ClassName = Rest.Left(LastUnderscore);
			int32 TargetIndex = FCString::Atoi(*Rest.Mid(LastUnderscore + 1));
			int32 CurrentIndex = 0;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->GetClass()->GetName() == ClassName)
				{
					if (CurrentIndex == TargetIndex) return Node;
					CurrentIndex++;
				}
			}
		}
		return nullptr;
	}

	// Standard path: check MCP_ID in NodeComment, then fallback to guid match
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && GetNodeId(Node) == NodeId)
		{
			return Node;
		}
	}

	return nullptr;
}

// ===== Pin & Connection Management =====

bool FBlueprintGraphEditor::ConnectPins(
	UEdGraph* Graph,
	const FString& SourceNodeId,
	const FString& SourcePinName,
	const FString& TargetNodeId,
	const FString& TargetPinName,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	// Find source node by MCP-generated ID
	UEdGraphNode* SourceNode = FindNodeById(Graph, SourceNodeId);
	if (!SourceNode)
	{
		OutError = FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId);
		return false;
	}

	// Find target node
	UEdGraphNode* TargetNode = FindNodeById(Graph, TargetNodeId);
	if (!TargetNode)
	{
		OutError = FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId);
		return false;
	}

	// Find pins - auto-detect exec pins if names are empty
	UEdGraphPin* SourcePin = nullptr;
	UEdGraphPin* TargetPin = nullptr;

	if (SourcePinName.IsEmpty())
	{
		// Auto-select first exec output
		SourcePin = GetExecPin(SourceNode, true);
		if (!SourcePin)
		{
			OutError = FString::Printf(TEXT("No exec output pin found on node '%s'"), *SourceNodeId);
			return false;
		}
	}
	else
	{
		SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_Output);
		if (!SourcePin)
		{
			// Try input direction for bidirectional data pins
			SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_Input);
		}
		if (!SourcePin)
		{
			OutError = FString::Printf(TEXT("Pin '%s' not found on source node '%s'"), *SourcePinName, *SourceNodeId);
			return false;
		}
	}

	if (TargetPinName.IsEmpty())
	{
		// Auto-select first exec input
		TargetPin = GetExecPin(TargetNode, false);
		if (!TargetPin)
		{
			OutError = FString::Printf(TEXT("No exec input pin found on node '%s'"), *TargetNodeId);
			return false;
		}
	}
	else
	{
		TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_Input);
		if (!TargetPin)
		{
			// Try output direction for bidirectional data pins
			TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_Output);
		}
		if (!TargetPin)
		{
			OutError = FString::Printf(TEXT("Pin '%s' not found on target node '%s'"), *TargetPinName, *TargetNodeId);
			return false;
		}
	}

	// Check if connection is valid
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			OutError = FString::Printf(TEXT("Cannot connect pins: %s"), *Response.Message.ToString());
			return false;
		}
	}

	// Make the connection
	SourcePin->MakeLinkTo(TargetPin);

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	UE_LOG(LogOsvayderUE, Log, TEXT("Connected '%s.%s' -> '%s.%s'"),
		*SourceNodeId, *SourcePin->PinName.ToString(),
		*TargetNodeId, *TargetPin->PinName.ToString());

	return true;
}

bool FBlueprintGraphEditor::DisconnectPins(
	UEdGraph* Graph,
	const FString& SourceNodeId,
	const FString& SourcePinName,
	const FString& TargetNodeId,
	const FString& TargetPinName,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	// Find nodes
	UEdGraphNode* SourceNode = FindNodeById(Graph, SourceNodeId);
	if (!SourceNode)
	{
		OutError = FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId);
		return false;
	}

	UEdGraphNode* TargetNode = FindNodeById(Graph, TargetNodeId);
	if (!TargetNode)
	{
		OutError = FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId);
		return false;
	}

	// Find pins
	UEdGraphPin* SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_MAX);
	if (!SourcePin)
	{
		OutError = FString::Printf(TEXT("Pin '%s' not found on source node '%s'"), *SourcePinName, *SourceNodeId);
		return false;
	}

	UEdGraphPin* TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_MAX);
	if (!TargetPin)
	{
		OutError = FString::Printf(TEXT("Pin '%s' not found on target node '%s'"), *TargetPinName, *TargetNodeId);
		return false;
	}

	// Break the link
	SourcePin->BreakLinkTo(TargetPin);

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	UE_LOG(LogOsvayderUE, Log, TEXT("Disconnected '%s.%s' from '%s.%s'"),
		*SourceNodeId, *SourcePinName,
		*TargetNodeId, *TargetPinName);

	return true;
}

bool FBlueprintGraphEditor::SetPinDefaultValue(
	UEdGraph* Graph,
	const FString& NodeId,
	const FString& PinName,
	const FString& Value,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	UEdGraphNode* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node '%s' not found"), *NodeId);
		return false;
	}

	UEdGraphPin* Pin = FindPinByName(Node, PinName, EGPD_Input);
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Input pin '%s' not found on node '%s'"), *PinName, *NodeId);
		return false;
	}

	// Set the default value
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		Schema->TrySetDefaultValue(*Pin, Value);
	}
	else
	{
		Pin->DefaultValue = Value;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	UE_LOG(LogOsvayderUE, Log, TEXT("Set pin '%s.%s' default value to '%s'"), *NodeId, *PinName, *Value);
	return true;
}

UEdGraphPin* FBlueprintGraphEditor::FindPinByName(
	UEdGraphNode* Node,
	const FString& PinName,
	EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			if (Direction == EGPD_MAX || Pin->Direction == Direction)
			{
				return Pin;
			}
		}
	}

	return nullptr;
}

UEdGraphPin* FBlueprintGraphEditor::GetExecPin(UEdGraphNode* Node, bool bOutput)
{
	if (!Node)
	{
		return nullptr;
	}

	EEdGraphPinDirection Direction = bOutput ? EGPD_Output : EGPD_Input;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return Pin;
		}
	}

	return nullptr;
}

// ===== Serialization =====

TSharedPtr<FJsonObject> FBlueprintGraphEditor::SerializeNodeInfo(UEdGraphNode* Node)
{
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();

	if (!Node)
	{
		return NodeObj;
	}

	NodeObj->SetStringField(TEXT("node_id"), GetNodeId(Node));
	NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
	NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
	if (!Node->NodeComment.IsEmpty() && !Node->NodeComment.StartsWith(NodeIdPrefix))
	{
		NodeObj->SetStringField(TEXT("comment"), Node->NodeComment);
	}

	// CallFunction metadata — resolved function owner
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		UFunction* Func = CallNode->GetTargetFunction();
		if (Func)
		{
			NodeObj->SetStringField(TEXT("function_name"), Func->GetName());
			if (UClass* OwnerClass = Func->GetOwnerClass())
			{
				NodeObj->SetStringField(TEXT("function_owner_class"), OwnerClass->GetName());
				NodeObj->SetStringField(TEXT("function_owner_path"), OwnerClass->GetPathName());
			}
		}
	}

	// Serialize pins
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));

			// Convert pin type to string representation
			FString TypeStr;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				TypeStr = TEXT("bool");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
			{
				TypeStr = TEXT("int32");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
			{
				TypeStr = (Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double) ? TEXT("double") : TEXT("float");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String)
			{
				TypeStr = TEXT("FString");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				TypeStr = TEXT("exec");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				if (UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					TypeStr = Struct->GetName();
				}
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
			{
				if (UClass* Class = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					TypeStr = Class->GetName() + TEXT("*");
				}
			}
			else
			{
				TypeStr = Pin->PinType.PinCategory.ToString();
			}

			PinObj->SetStringField(TEXT("type"), TypeStr);
			if (!Pin->DefaultValue.IsEmpty())
			{
				PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
			}
			// Serialize connection targets (not just count)
			if (Pin->LinkedTo.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> LinkedTargets;
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin && LinkedPin->GetOwningNode())
					{
						TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
						LinkObj->SetStringField(TEXT("node_id"), GetNodeId(LinkedPin->GetOwningNode()));
						LinkObj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
						LinkedTargets.Add(MakeShared<FJsonValueObject>(LinkObj));
					}
				}
				PinObj->SetArrayField(TEXT("connected_to"), LinkedTargets);
			}
			PinObj->SetNumberField(TEXT("connections"), Pin->LinkedTo.Num());
			Pins.Add(MakeShared<FJsonValueObject>(PinObj));
		}
	}
	NodeObj->SetArrayField(TEXT("pins"), Pins);

	return NodeObj;
}

// ===== Node ID System =====

FString FBlueprintGraphEditor::GenerateNodeId(const FString& NodeType, const FString& Context, UEdGraph* Graph)
{
	FString BaseId;
	if (Context.IsEmpty())
	{
		BaseId = NodeType;
	}
	else
	{
		BaseId = FString::Printf(TEXT("%s_%s"), *NodeType, *Context);
	}

	// Ensure uniqueness with atomic increment for thread safety
	int32 Counter = FPlatformAtomics::InterlockedIncrement(&NodeIdCounter);
	FString NodeId = FString::Printf(TEXT("%s_%d"), *BaseId, Counter);

	// Verify uniqueness in graph
	if (Graph)
	{
		while (FindNodeById(Graph, NodeId) != nullptr)
		{
			Counter = FPlatformAtomics::InterlockedIncrement(&NodeIdCounter);
			NodeId = FString::Printf(TEXT("%s_%d"), *BaseId, Counter);
		}
	}

	return NodeId;
}

void FBlueprintGraphEditor::SetNodeId(UEdGraphNode* Node, const FString& NodeId)
{
	if (Node)
	{
		// Store ID in node comment (visible in editor, persisted)
		Node->NodeComment = NodeIdPrefix + NodeId;
	}
}

FString FBlueprintGraphEditor::GetNodeId(UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}

	// Priority 1: MCP-assigned ID in NodeComment (backward compatible)
	if (Node->NodeComment.StartsWith(NodeIdPrefix))
	{
		return Node->NodeComment.RightChop(NodeIdPrefix.Len());
	}

	// Priority 2: NodeGuid for existing nodes (stable across saves/loads)
	if (Node->NodeGuid.IsValid())
	{
		return TEXT("guid_") + Node->NodeGuid.ToString(EGuidFormats::Short);
	}

	// Priority 3: Deterministic fallback — class_name_index
	UEdGraph* Graph = Node->GetGraph();
	if (Graph)
	{
		int32 Index = 0;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N == Node) break;
			if (N && N->GetClass() == Node->GetClass()) Index++;
		}
		return FString::Printf(TEXT("det_%s_%d"), *Node->GetClass()->GetName(), Index);
	}

	return FString();
}

// ===== Private Node Creation Helpers =====

UEdGraphNode* FBlueprintGraphEditor::CreateCallFunctionNode(
	UEdGraph* Graph,
	const FString& FunctionName,
	const FString& TargetClass,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (FunctionName.IsEmpty())
	{
		OutError = TEXT("Function name is required");
		return nullptr;
	}

	// Find the function
	UFunction* Function = nullptr;
	UClass* FunctionOwner = nullptr;

	// Common library classes searched in order
	auto GetCommonLibraries = []() -> TArray<UClass*>
	{
		TArray<UClass*> Libs;
		Libs.Add(UKismetSystemLibrary::StaticClass());
		Libs.Add(UKismetMathLibrary::StaticClass());
		// GameplayStatics — most commonly used for actor/world queries
		if (UClass* GS = FindObject<UClass>(nullptr, TEXT("/Script/Engine.GameplayStatics"))) Libs.Add(GS);
		// Additional common libraries
		if (UClass* KA = FindObject<UClass>(nullptr, TEXT("/Script/Engine.KismetArrayLibrary"))) Libs.Add(KA);
		if (UClass* KS = FindObject<UClass>(nullptr, TEXT("/Script/Engine.KismetStringLibrary"))) Libs.Add(KS);
		if (UClass* KT = FindObject<UClass>(nullptr, TEXT("/Script/Engine.KismetTextLibrary"))) Libs.Add(KT);
		if (UClass* GA = FindObject<UClass>(nullptr, TEXT("/Script/Engine.GameplayStatics"))) {} // already added
		if (UClass* HL = FindObject<UClass>(nullptr, TEXT("/Script/HeadMountedDisplay.HeadMountedDisplayFunctionLibrary"))) Libs.Add(HL);
		return Libs;
	};

	// Try to find class by name
	if (!TargetClass.IsEmpty())
	{
		FunctionOwner = FindObject<UClass>(nullptr, *TargetClass);
		if (!FunctionOwner)
		{
			// Try short name resolution against common modules
			TArray<FString> ModulePrefixes = {
				TEXT("/Script/Engine."), TEXT("/Script/CoreUObject."),
				TEXT("/Script/UMG."), TEXT("/Script/AIModule."),
				TEXT("/Script/NavigationSystem."), TEXT("/Script/PhysicsCore.")
			};
			for (const FString& Prefix : ModulePrefixes)
			{
				FunctionOwner = FindObject<UClass>(nullptr, *(Prefix + TargetClass));
				if (FunctionOwner) break;
			}
		}
		// Also try common short names
		if (!FunctionOwner)
		{
			if (TargetClass.Equals(TEXT("KismetSystemLibrary"), ESearchCase::IgnoreCase))
				FunctionOwner = UKismetSystemLibrary::StaticClass();
			else if (TargetClass.Equals(TEXT("KismetMathLibrary"), ESearchCase::IgnoreCase))
				FunctionOwner = UKismetMathLibrary::StaticClass();
			else if (TargetClass.Equals(TEXT("GameplayStatics"), ESearchCase::IgnoreCase))
				FunctionOwner = FindObject<UClass>(nullptr, TEXT("/Script/Engine.GameplayStatics"));
		}
	}
	else
	{
		// Default to KismetSystemLibrary for common functions
		FunctionOwner = UKismetSystemLibrary::StaticClass();
	}

	if (FunctionOwner)
	{
		Function = FunctionOwner->FindFunctionByName(FName(*FunctionName));
	}

	// If not found in specified class, search all common libraries
	if (!Function)
	{
		TArray<UClass*> CommonLibs = GetCommonLibraries();
		for (UClass* Lib : CommonLibs)
		{
			if (Lib)
			{
				Function = Lib->FindFunctionByName(FName(*FunctionName));
				if (Function)
				{
					FunctionOwner = Lib;
					break;
				}
			}
		}
	}

	// Last resort: search Blueprint's parent class hierarchy
	if (!Function)
	{
		UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
		if (BP && BP->ParentClass)
		{
			Function = BP->ParentClass->FindFunctionByName(FName(*FunctionName));
			if (Function) FunctionOwner = BP->ParentClass;
		}
	}

	if (!Function)
	{
		OutError = FString::Printf(TEXT("Function '%s' not found. Searched: specified class, KismetSystemLibrary, KismetMathLibrary, GameplayStatics, KismetArrayLibrary, KismetStringLibrary, parent class hierarchy. Specify target_class if the function belongs to a specific class."), *FunctionName);
		return nullptr;
	}

	// Create the node
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* CallNode = NodeCreator.CreateNode();
	CallNode->SetFromFunction(Function);
	CallNode->NodePosX = PosX;
	CallNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return CallNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateBranchNode(
	UEdGraph* Graph,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	FGraphNodeCreator<UK2Node_IfThenElse> NodeCreator(*Graph);
	UK2Node_IfThenElse* BranchNode = NodeCreator.CreateNode();
	BranchNode->NodePosX = PosX;
	BranchNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return BranchNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateEventNode(
	UEdGraph* Graph,
	const FString& EventName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (EventName.IsEmpty())
	{
		OutError = TEXT("Event name is required");
		return nullptr;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!Blueprint)
	{
		OutError = TEXT("Could not find Blueprint for graph");
		return nullptr;
	}

	// Find the event function
	UFunction* EventFunc = nullptr;

	// Check common events
	if (EventName.Equals(TEXT("BeginPlay"), ESearchCase::IgnoreCase))
	{
		EventFunc = AActor::StaticClass()->FindFunctionByName(FName("ReceiveBeginPlay"));
	}
	else if (EventName.Equals(TEXT("Tick"), ESearchCase::IgnoreCase))
	{
		EventFunc = AActor::StaticClass()->FindFunctionByName(FName("ReceiveTick"));
	}
	else if (EventName.Equals(TEXT("EndPlay"), ESearchCase::IgnoreCase))
	{
		EventFunc = AActor::StaticClass()->FindFunctionByName(FName("ReceiveEndPlay"));
	}
	else
	{
		// Try to find in parent class
		if (Blueprint->ParentClass)
		{
			EventFunc = Blueprint->ParentClass->FindFunctionByName(FName(*EventName));
		}
	}

	if (!EventFunc)
	{
		OutError = FString::Printf(TEXT("Event '%s' not found. Common events: BeginPlay, Tick, EndPlay"), *EventName);
		return nullptr;
	}

	// Create the event node
	FGraphNodeCreator<UK2Node_Event> NodeCreator(*Graph);
	UK2Node_Event* EventNode = NodeCreator.CreateNode();
	EventNode->EventReference.SetFromField<UFunction>(EventFunc, false);
	EventNode->bOverrideFunction = true;
	EventNode->NodePosX = PosX;
	EventNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return EventNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateCustomEventNode(
	UEdGraph* Graph,
	const FString& EventName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (EventName.IsEmpty())
	{
		OutError = TEXT("Custom event name is required (pass event_name in node_params)");
		return nullptr;
	}

	// Create custom event node
	FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*Graph);
	UK2Node_CustomEvent* EventNode = NodeCreator.CreateNode();
	EventNode->CustomFunctionName = FName(*EventName);
	EventNode->NodePosX = PosX;
	EventNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return EventNode;
}

// ===== Delegate Node Creation (template for Call/Add/Remove/Clear) =====

template<typename TNodeClass>
UEdGraphNode* FBlueprintGraphEditor::CreateDelegateNode(
	UEdGraph* Graph,
	UBlueprint* Blueprint,
	const FString& DispatcherName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (DispatcherName.IsEmpty())
	{
		OutError = TEXT("Dispatcher name is required (pass 'dispatcher' in node_params)");
		return nullptr;
	}

	if (!Blueprint)
	{
		OutError = TEXT("Blueprint not found for graph");
		return nullptr;
	}

	// Find the dispatcher property on the Blueprint's generated class
	FName DispFName(*DispatcherName);
	FMulticastDelegateProperty* DelegateProp = nullptr;

	if (UClass* GenClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass)
	{
		for (TFieldIterator<FMulticastDelegateProperty> It(GenClass); It; ++It)
		{
			if (It->GetFName() == DispFName)
			{
				DelegateProp = *It;
				break;
			}
		}
	}

	// Also check delegate signature graphs by name as fallback
	if (!DelegateProp)
	{
		bool bGraphExists = false;
		for (UEdGraph* G : Blueprint->DelegateSignatureGraphs)
		{
			if (G && G->GetName() == DispatcherName) { bGraphExists = true; break; }
		}
		if (!bGraphExists)
		{
			OutError = FString::Printf(TEXT("Dispatcher '%s' not found on Blueprint. Create it first with add_dispatcher."), *DispatcherName);
			return nullptr;
		}
	}

	// Create the delegate node
	FGraphNodeCreator<TNodeClass> NodeCreator(*Graph);
	TNodeClass* DelegateNode = NodeCreator.CreateNode();
	DelegateNode->NodePosX = PosX;
	DelegateNode->NodePosY = PosY;

	// Set the delegate reference — UK2Node_BaseMCDelegate exposes SetFromProperty
	if (DelegateProp)
	{
		DelegateNode->SetFromProperty(DelegateProp, false, DelegateProp->GetOwnerClass());
	}
	else
	{
		// Fallback: set delegate FName directly via member variable reference
		FMemberReference& DelegateRef = DelegateNode->DelegateReference;
		DelegateRef.SetSelfMember(DispFName);
	}

	NodeCreator.Finalize();

	return DelegateNode;
}

// Explicit template instantiations
template UEdGraphNode* FBlueprintGraphEditor::CreateDelegateNode<UK2Node_CallDelegate>(UEdGraph*, UBlueprint*, const FString&, int32, int32, FString&);
template UEdGraphNode* FBlueprintGraphEditor::CreateDelegateNode<UK2Node_AddDelegate>(UEdGraph*, UBlueprint*, const FString&, int32, int32, FString&);
template UEdGraphNode* FBlueprintGraphEditor::CreateDelegateNode<UK2Node_RemoveDelegate>(UEdGraph*, UBlueprint*, const FString&, int32, int32, FString&);
template UEdGraphNode* FBlueprintGraphEditor::CreateDelegateNode<UK2Node_ClearDelegate>(UEdGraph*, UBlueprint*, const FString&, int32, int32, FString&);

UEdGraphNode* FBlueprintGraphEditor::CreateVariableGetNode(
	UEdGraph* Graph,
	UBlueprint* Blueprint,
	const FString& VariableName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Variable name is required");
		return nullptr;
	}

	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	// Verify variable exists
	FName VarName(*VariableName);
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VariableName);
		return nullptr;
	}

	// Create the node
	FGraphNodeCreator<UK2Node_VariableGet> NodeCreator(*Graph);
	UK2Node_VariableGet* GetNode = NodeCreator.CreateNode();
	GetNode->VariableReference.SetSelfMember(VarName);
	GetNode->NodePosX = PosX;
	GetNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return GetNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateVariableSetNode(
	UEdGraph* Graph,
	UBlueprint* Blueprint,
	const FString& VariableName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Variable name is required");
		return nullptr;
	}

	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	// Verify variable exists
	FName VarName(*VariableName);
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VariableName);
		return nullptr;
	}

	// Create the node
	FGraphNodeCreator<UK2Node_VariableSet> NodeCreator(*Graph);
	UK2Node_VariableSet* SetNode = NodeCreator.CreateNode();
	SetNode->VariableReference.SetSelfMember(VarName);
	SetNode->NodePosX = PosX;
	SetNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return SetNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateSequenceNode(
	UEdGraph* Graph,
	int32 NumOutputs,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	FGraphNodeCreator<UK2Node_ExecutionSequence> NodeCreator(*Graph);
	UK2Node_ExecutionSequence* SeqNode = NodeCreator.CreateNode();
	SeqNode->NodePosX = PosX;
	SeqNode->NodePosY = PosY;
	NodeCreator.Finalize();

	// Add additional output pins if needed (default is 2)
	while (SeqNode->Pins.Num() < NumOutputs + 1) // +1 for input exec
	{
		SeqNode->AddInputPin();
	}

	return SeqNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateMathNode(
	UEdGraph* Graph,
	const FString& MathOp,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	// Find the appropriate math function
	FName FunctionName;
	if (MathOp.Equals(TEXT("Add"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Add_FloatFloat");
	}
	else if (MathOp.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Subtract_FloatFloat");
	}
	else if (MathOp.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Multiply_FloatFloat");
	}
	else if (MathOp.Equals(TEXT("Divide"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Divide_FloatFloat");
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown math operation: '%s'. Supported: Add, Subtract, Multiply, Divide"), *MathOp);
		return nullptr;
	}

	UFunction* MathFunc = UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName);
	if (!MathFunc)
	{
		OutError = FString::Printf(TEXT("Math function '%s' not found"), *FunctionName.ToString());
		return nullptr;
	}

	// Create a call function node for the math operation
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* MathNode = NodeCreator.CreateNode();
	MathNode->SetFromFunction(MathFunc);
	MathNode->NodePosX = PosX;
	MathNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return MathNode;
}
