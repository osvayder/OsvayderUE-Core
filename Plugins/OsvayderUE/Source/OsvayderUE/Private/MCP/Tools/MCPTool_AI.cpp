// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_AI.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEModule.h"
#include "MCP/MCPParamValidator.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

// AI headers
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"

FMCPToolInfo FMCPTool_AI::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("ai");
	Info.Description = TEXT(
		"AI / Behavior Tree / Blackboard / EQS authoring surfaces.\n\n"
		"Query operations (read-only):\n"
		"- 'list_behavior_trees': Discover BehaviorTree assets in project\n"
		"- 'get_behavior_tree_info': Introspect BT — root node class, blackboard reference, node counts (tasks/decorators/services/composites)\n"
		"- 'list_blackboards': Discover BlackboardData assets\n"
		"- 'get_blackboard_info': Introspect blackboard — keys with name, type, instance_synced flag, parent BB\n"
		"- 'list_eqs_queries': Discover EnvQuery assets\n"
		"- 'get_eqs_info': Introspect EQS — options with generator class and test summary\n\n"
		"Modify operations:\n"
		"- 'add_blackboard_key': Add a key to a BlackboardData asset (name + type)\n"
		"- 'remove_blackboard_key': Remove a key from a BlackboardData asset\n"
		"- 'configure_ai_foundation': Composed — add multiple Blackboard keys in one call with dry_run\n\n"
		"NOTE: This is an asset introspection and Blackboard authoring surface.\n"
		"BT graph mutation and EQS mutation are not supported in this version.\n"
		"Runtime AI behavior verification requires PIE with AI controllers active."
	);

	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"),
			TEXT("Operation: 'list_behavior_trees', 'get_behavior_tree_info', 'list_blackboards', "
				 "'get_blackboard_info', 'list_eqs_queries', 'get_eqs_info', "
				 "'add_blackboard_key', 'remove_blackboard_key', 'configure_ai_foundation'"), true),
		FMCPToolParameter(TEXT("asset_path"), TEXT("string"),
			TEXT("Asset path for introspection/modification"), false),
		FMCPToolParameter(TEXT("path_filter"), TEXT("string"),
			TEXT("Path prefix filter for list operations (default: /Game/)"), false, TEXT("/Game/")),
		FMCPToolParameter(TEXT("name_filter"), TEXT("string"),
			TEXT("Name substring filter for list operations"), false),
		FMCPToolParameter(TEXT("limit"), TEXT("number"),
			TEXT("Maximum results (1-500, default: 25)"), false, TEXT("25")),
		// Blackboard key params
		FMCPToolParameter(TEXT("key_name"), TEXT("string"),
			TEXT("Blackboard key name (for add/remove)"), false),
		FMCPToolParameter(TEXT("key_type"), TEXT("string"),
			TEXT("Blackboard key type: Bool, Int, Float, String, Name, Vector, Rotator, Enum, NativeEnum, Object, Class"), false),
		// configure_ai_foundation params
		FMCPToolParameter(TEXT("keys"), TEXT("array"),
			TEXT("Array of {name, type} objects for configure_ai_foundation"), false),
		FMCPToolParameter(TEXT("dry_run"), TEXT("boolean"),
			TEXT("If true, report what would change without mutating"), false, TEXT("false")),
	};

	// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
	Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
		TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_AI::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("list_behavior_trees"))     return ExecuteListBehaviorTrees(Params);
	if (Operation == TEXT("get_behavior_tree_info"))  return ExecuteGetBehaviorTreeInfo(Params);
	if (Operation == TEXT("list_blackboards"))        return ExecuteListBlackboards(Params);
	if (Operation == TEXT("get_blackboard_info"))     return ExecuteGetBlackboardInfo(Params);
	if (Operation == TEXT("list_eqs_queries"))        return ExecuteListEqsQueries(Params);
	if (Operation == TEXT("get_eqs_info"))            return ExecuteGetEqsInfo(Params);
	if (Operation == TEXT("add_blackboard_key"))      return ExecuteAddBlackboardKey(Params);
	if (Operation == TEXT("remove_blackboard_key"))   return ExecuteRemoveBlackboardKey(Params);
	if (Operation == TEXT("configure_ai_foundation")) return ExecuteConfigureAiFoundation(Params);

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown AI operation: '%s'. Valid: list_behavior_trees, get_behavior_tree_info, list_blackboards, ")
		TEXT("get_blackboard_info, list_eqs_queries, get_eqs_info, add_blackboard_key, remove_blackboard_key, ")
		TEXT("configure_ai_foundation"),
		*Operation));
}

// ===== Helpers =====

UObject* FMCPTool_AI::LoadAssetByPath(const FString& AssetPath, FString& OutError)
{
	FSoftObjectPath SoftPath(AssetPath);
	UObject* Asset = SoftPath.TryLoad();
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("Could not load asset: %s"), *AssetPath);
	}
	return Asset;
}

// ===== Query: List Behavior Trees =====

FMCPToolResult FMCPTool_AI::ExecuteListBehaviorTrees(const TSharedRef<FJsonObject>& Params)
{
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	Limit = FMath::Clamp(Limit, 1, 500);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBehaviorTree::StaticClass()->GetClassPathName());
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
	Data->SetArrayField(TEXT("behavior_trees"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d Behavior Trees"), Results.Num()), Data);
}

// ===== Query: Get Behavior Tree Info =====

FMCPToolResult FMCPTool_AI::ExecuteGetBehaviorTreeInfo(const TSharedRef<FJsonObject>& Params)
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

	UBehaviorTree* BT = Cast<UBehaviorTree>(Asset);
	if (!BT) return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a BehaviorTree: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"), BT->GetName());

	// Blackboard reference
	if (BT->BlackboardAsset)
	{
		Data->SetStringField(TEXT("blackboard_asset"), BT->BlackboardAsset->GetPathName());
	}
	else
	{
		Data->SetStringField(TEXT("blackboard_asset"), TEXT("(none)"));
	}

	// Root node
	if (BT->RootNode)
	{
		Data->SetStringField(TEXT("root_node_class"), BT->RootNode->GetClass()->GetName());
		Data->SetStringField(TEXT("root_node_name"), BT->RootNode->GetNodeName());
	}
	else
	{
		Data->SetStringField(TEXT("root_node_class"), TEXT("(empty tree)"));
	}

	// Count node types by walking the tree
	int32 TaskCount = 0, DecoratorCount = 0, ServiceCount = 0, CompositeCount = 0;

	TArray<UBTCompositeNode*> NodesToVisit;
	if (BT->RootNode)
	{
		NodesToVisit.Add(BT->RootNode);
	}

	while (NodesToVisit.Num() > 0)
	{
		UBTCompositeNode* Current = NodesToVisit.Pop();
		CompositeCount++;

		// Count services on this composite node
		ServiceCount += Current->Services.Num();

		// Visit children
		for (int32 i = 0; i < Current->Children.Num(); i++)
		{
			const FBTCompositeChild& Child = Current->Children[i];

			// Count decorators on this child branch
			DecoratorCount += Child.Decorators.Num();

			if (Child.ChildTask)
			{
				TaskCount++;
			}
			if (Child.ChildComposite)
			{
				NodesToVisit.Add(Child.ChildComposite);
			}
		}
	}

	TSharedPtr<FJsonObject> NodeCounts = MakeShared<FJsonObject>();
	NodeCounts->SetNumberField(TEXT("composites"), CompositeCount);
	NodeCounts->SetNumberField(TEXT("tasks"), TaskCount);
	NodeCounts->SetNumberField(TEXT("decorators"), DecoratorCount);
	NodeCounts->SetNumberField(TEXT("services"), ServiceCount);
	NodeCounts->SetNumberField(TEXT("total"), CompositeCount + TaskCount + DecoratorCount + ServiceCount);
	Data->SetObjectField(TEXT("node_counts"), NodeCounts);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("BT %s: %d tasks, %d decorators, %d services, %d composites"),
			*BT->GetName(), TaskCount, DecoratorCount, ServiceCount, CompositeCount),
		Data);
}

// ===== Query: List Blackboards =====

FMCPToolResult FMCPTool_AI::ExecuteListBlackboards(const TSharedRef<FJsonObject>& Params)
{
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	Limit = FMath::Clamp(Limit, 1, 500);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlackboardData::StaticClass()->GetClassPathName());
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
	Data->SetArrayField(TEXT("blackboards"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d Blackboard assets"), Results.Num()), Data);
}

// ===== Query: Get Blackboard Info =====

FMCPToolResult FMCPTool_AI::ExecuteGetBlackboardInfo(const TSharedRef<FJsonObject>& Params)
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

	UBlackboardData* BB = Cast<UBlackboardData>(Asset);
	if (!BB) return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a BlackboardData: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"), BB->GetName());

	// Parent blackboard
	if (BB->Parent)
	{
		Data->SetStringField(TEXT("parent"), BB->Parent->GetPathName());
	}
	else
	{
		Data->SetStringField(TEXT("parent"), TEXT("(none)"));
	}

	// Keys
	TArray<TSharedPtr<FJsonValue>> Keys;
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
		KeyObj->SetStringField(TEXT("type"), Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("(null)"));
		KeyObj->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced);
		Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
	}
	Data->SetArrayField(TEXT("keys"), Keys);
	Data->SetNumberField(TEXT("key_count"), Keys.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Blackboard %s: %d keys"), *BB->GetName(), Keys.Num()), Data);
}

// ===== Query: List EQS Queries =====

FMCPToolResult FMCPTool_AI::ExecuteListEqsQueries(const TSharedRef<FJsonObject>& Params)
{
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	Limit = FMath::Clamp(Limit, 1, 500);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UEnvQuery::StaticClass()->GetClassPathName());
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
	Data->SetArrayField(TEXT("eqs_queries"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d EQS queries"), Results.Num()), Data);
}

// ===== Query: Get EQS Info =====

FMCPToolResult FMCPTool_AI::ExecuteGetEqsInfo(const TSharedRef<FJsonObject>& Params)
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

	UEnvQuery* EQ = Cast<UEnvQuery>(Asset);
	if (!EQ) return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not an EnvQuery: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"), EQ->GetName());

	TArray<TSharedPtr<FJsonValue>> Options;
	for (UEnvQueryOption* Option : EQ->GetOptions())
	{
		if (!Option) continue;

		TSharedPtr<FJsonObject> OptObj = MakeShared<FJsonObject>();

		// Generator
		if (Option->Generator)
		{
			OptObj->SetStringField(TEXT("generator_class"), Option->Generator->GetClass()->GetName());
			OptObj->SetStringField(TEXT("generator_name"), Option->Generator->GetName());
		}

		// Tests
		TArray<TSharedPtr<FJsonValue>> Tests;
		for (UEnvQueryTest* Test : Option->Tests)
		{
			if (!Test) continue;
			TSharedPtr<FJsonObject> TestObj = MakeShared<FJsonObject>();
			TestObj->SetStringField(TEXT("class"), Test->GetClass()->GetName());
			TestObj->SetStringField(TEXT("name"), Test->GetName());
			Tests.Add(MakeShared<FJsonValueObject>(TestObj));
		}
		OptObj->SetArrayField(TEXT("tests"), Tests);
		OptObj->SetNumberField(TEXT("test_count"), Tests.Num());

		Options.Add(MakeShared<FJsonValueObject>(OptObj));
	}

	Data->SetArrayField(TEXT("options"), Options);
	Data->SetNumberField(TEXT("option_count"), Options.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("EQS %s: %d options"), *EQ->GetName(), Options.Num()), Data);
}

// ===== Modify: Add Blackboard Key =====

FMCPToolResult FMCPTool_AI::ExecuteAddBlackboardKey(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	FString KeyName;
	if (!ExtractRequiredString(Params, TEXT("key_name"), KeyName, Error))
	{
		return Error.GetValue();
	}

	FString KeyType;
	if (!ExtractRequiredString(Params, TEXT("key_type"), KeyType, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset) return FMCPToolResult::Error(LoadError);

	UBlackboardData* BB = Cast<UBlackboardData>(Asset);
	if (!BB) return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a BlackboardData: %s"), *AssetPath));

	// Check if key already exists
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName.ToString() == KeyName)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Key '%s' already exists in Blackboard"), *KeyName));
		}
	}

	// Resolve key type class
	FString KeyTypeClassName = FString::Printf(TEXT("BlackboardKeyType_%s"), *KeyType);
	UClass* KeyTypeClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/AIModule.%s"), *KeyTypeClassName));

	if (!KeyTypeClass)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Unknown Blackboard key type: '%s'. Valid types: Bool, Int, Float, String, Name, Vector, Rotator, Enum, NativeEnum, Object, Class"),
			*KeyType));
	}

	// Add the key
	FBlackboardEntry NewEntry;
	NewEntry.EntryName = FName(*KeyName);
	NewEntry.KeyType = NewObject<UBlackboardKeyType>(BB, KeyTypeClass);

	BB->Keys.Add(NewEntry);
	BB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("key_name"), KeyName);
	Data->SetStringField(TEXT("key_type"), KeyType);
	Data->SetStringField(TEXT("status"), TEXT("added"));

	// Emit receipt
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("ai");
		Receipt.Summary = TEXT("add_blackboard_key");
		Receipt.bSuccess = true;
		Receipt.Targets.Add(AssetPath);
		Receipt.Classification = TEXT("user_mutation");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added key '%s' (%s) to Blackboard %s"), *KeyName, *KeyType, *BB->GetName()), Data);
}

// ===== Modify: Remove Blackboard Key =====

FMCPToolResult FMCPTool_AI::ExecuteRemoveBlackboardKey(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	FString KeyName;
	if (!ExtractRequiredString(Params, TEXT("key_name"), KeyName, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset) return FMCPToolResult::Error(LoadError);

	UBlackboardData* BB = Cast<UBlackboardData>(Asset);
	if (!BB) return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a BlackboardData: %s"), *AssetPath));

	int32 RemoveIdx = INDEX_NONE;
	for (int32 i = 0; i < BB->Keys.Num(); i++)
	{
		if (BB->Keys[i].EntryName.ToString() == KeyName)
		{
			RemoveIdx = i;
			break;
		}
	}

	if (RemoveIdx == INDEX_NONE)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Key '%s' not found in Blackboard"), *KeyName));
	}

	BB->Keys.RemoveAt(RemoveIdx);
	BB->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("key_name"), KeyName);
	Data->SetStringField(TEXT("status"), TEXT("removed"));

	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("ai");
		Receipt.Summary = TEXT("remove_blackboard_key");
		Receipt.bSuccess = true;
		Receipt.Targets.Add(AssetPath);
		Receipt.Classification = TEXT("user_mutation");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed key '%s' from Blackboard %s"), *KeyName, *BB->GetName()), Data);
}

// ===== Modify: Configure AI Foundation (Composed Bundle) =====

FMCPToolResult FMCPTool_AI::ExecuteConfigureAiFoundation(const TSharedRef<FJsonObject>& Params)
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

	UBlackboardData* BB = Cast<UBlackboardData>(Asset);
	if (!BB) return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a BlackboardData: %s"), *AssetPath));

	const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("keys"), KeysArray) || !KeysArray)
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: keys (array of {name, type})"));
	}

	TArray<TSharedPtr<FJsonValue>> Actions;
	int32 UpdatedCount = 0, UnchangedCount = 0, FailedCount = 0;

	// Build existing key set
	TSet<FString> ExistingKeys;
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		ExistingKeys.Add(Entry.EntryName.ToString());
	}

	for (const TSharedPtr<FJsonValue>& KeyVal : *KeysArray)
	{
		const TSharedPtr<FJsonObject>* KeyObj = nullptr;
		if (!KeyVal->TryGetObject(KeyObj) || !KeyObj || !(*KeyObj).IsValid())
		{
			FailedCount++;
			continue;
		}

		FString KName, KType;
		if (!(*KeyObj)->TryGetStringField(TEXT("name"), KName) || KName.IsEmpty())
		{
			TSharedPtr<FJsonObject> Act = MakeShared<FJsonObject>();
			Act->SetStringField(TEXT("action"), TEXT("add_key"));
			Act->SetStringField(TEXT("status"), TEXT("failed"));
			Act->SetStringField(TEXT("detail"), TEXT("missing key name"));
			Actions.Add(MakeShared<FJsonValueObject>(Act));
			FailedCount++;
			continue;
		}

		if (!(*KeyObj)->TryGetStringField(TEXT("type"), KType) || KType.IsEmpty())
		{
			TSharedPtr<FJsonObject> Act = MakeShared<FJsonObject>();
			Act->SetStringField(TEXT("action"), FString::Printf(TEXT("add_%s"), *KName));
			Act->SetStringField(TEXT("status"), TEXT("failed"));
			Act->SetStringField(TEXT("detail"), TEXT("missing key type"));
			Actions.Add(MakeShared<FJsonValueObject>(Act));
			FailedCount++;
			continue;
		}

		// Check existing
		if (ExistingKeys.Contains(KName))
		{
			TSharedPtr<FJsonObject> Act = MakeShared<FJsonObject>();
			Act->SetStringField(TEXT("action"), FString::Printf(TEXT("add_%s"), *KName));
			Act->SetStringField(TEXT("status"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"));
			Actions.Add(MakeShared<FJsonValueObject>(Act));
			UnchangedCount++;
			continue;
		}

		// Resolve type
		FString KeyTypeClassName = FString::Printf(TEXT("BlackboardKeyType_%s"), *KType);
		UClass* KeyTypeClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/AIModule.%s"), *KeyTypeClassName));

		if (!KeyTypeClass)
		{
			TSharedPtr<FJsonObject> Act = MakeShared<FJsonObject>();
			Act->SetStringField(TEXT("action"), FString::Printf(TEXT("add_%s"), *KName));
			Act->SetStringField(TEXT("status"), TEXT("failed"));
			Act->SetStringField(TEXT("detail"), FString::Printf(TEXT("Unknown type: %s"), *KType));
			Actions.Add(MakeShared<FJsonValueObject>(Act));
			FailedCount++;
			continue;
		}

		if (!bDryRun)
		{
			FBlackboardEntry NewEntry;
			NewEntry.EntryName = FName(*KName);
			NewEntry.KeyType = NewObject<UBlackboardKeyType>(BB, KeyTypeClass);
			BB->Keys.Add(NewEntry);
			ExistingKeys.Add(KName);
		}

		TSharedPtr<FJsonObject> Act = MakeShared<FJsonObject>();
		Act->SetStringField(TEXT("action"), FString::Printf(TEXT("add_%s"), *KName));
		Act->SetStringField(TEXT("status"), bDryRun ? TEXT("would_create") : TEXT("created"));
		Act->SetStringField(TEXT("type"), KType);
		Actions.Add(MakeShared<FJsonValueObject>(Act));
		UpdatedCount++;
	}

	if (!bDryRun && UpdatedCount > 0)
	{
		BB->MarkPackageDirty();
	}

	bool bAllFailed = FailedCount > 0 && UpdatedCount == 0 && UnchangedCount == 0;
	bool bPartialSuccess = FailedCount > 0 && (UpdatedCount > 0 || UnchangedCount > 0);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetArrayField(TEXT("actions"), Actions);
	Data->SetNumberField(TEXT("action_count"), Actions.Num());
	Data->SetNumberField(TEXT("created_count"), UpdatedCount);
	Data->SetNumberField(TEXT("unchanged_count"), UnchangedCount);
	Data->SetNumberField(TEXT("failed_count"), FailedCount);
	if (bPartialSuccess) Data->SetBoolField(TEXT("partial_success"), true);

	if (!bDryRun)
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("ai");
		Receipt.Summary = TEXT("configure_ai_foundation");
		Receipt.bSuccess = !bAllFailed;
		Receipt.bPartialSuccess = bPartialSuccess;
		Receipt.Targets.Add(AssetPath);
		Receipt.Classification = TEXT("user_mutation");
		FOsvayderUEExecutionLog::Get().AddReceipt(Receipt);
	}

	FString Message = FString::Printf(TEXT("%s configure_ai_foundation: %d actions (%d created, %d unchanged, %d failed)"),
		bDryRun ? TEXT("[dry_run]") : TEXT("Applied"), Actions.Num(), UpdatedCount, UnchangedCount, FailedCount);

	if (bAllFailed && !bDryRun) return FMCPToolResult::Error(Message);
	return FMCPToolResult::Success(Message, Data);
}
