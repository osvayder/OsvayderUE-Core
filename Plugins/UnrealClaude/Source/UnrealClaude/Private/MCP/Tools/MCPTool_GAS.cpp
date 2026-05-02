// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_GAS.h"
#include "UnrealClaudeExecutionLog.h"
#include "UnrealClaudeModule.h"
#include "MCP/MCPParamValidator.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SavePackage.h"

// GAS headers
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "AttributeSet.h"
#include "GameplayTagContainer.h"
#include "AbilitySystemComponent.h"

FMCPToolInfo FMCPTool_GAS::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("gas");
	Info.Description = TEXT(
		"Gameplay Ability System (GAS) authoring surfaces.\n\n"
		"Query operations (read-only):\n"
		"- 'list_abilities': Discover Gameplay Ability blueprints in project\n"
		"- 'get_ability_info': Introspect ability (tags, cost GE ref, cooldown GE ref, instancing policy, net execution/replication)\n"
		"- 'list_effects': Discover Gameplay Effect assets/blueprints\n"
		"- 'get_effect_info': Introspect effect (duration policy, modifiers, gameplay tags, period, stacking)\n"
		"- 'list_attribute_sets': Discover AttributeSet C++ classes registered in engine\n"
		"- 'get_attribute_set_info': Introspect attribute set (attribute names and types)\n\n"
		"Modify operations:\n"
		"- 'set_effect_properties': Set GE properties via reflection (duration, period, modifiers, tags)\n"
		"- 'configure_gas_ability': Composed bundle — configure ability + optional cost/cooldown GE + tags in one call\n\n"
		"NOTE: This is an asset authoring surface. It creates/configures GAS assets.\n"
		"It does NOT grant abilities at runtime or hook up AbilitySystemComponents.\n"
		"Runtime integration requires gameplay code outside plugin scope."
	);

	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"),
			TEXT("Operation: 'list_abilities', 'get_ability_info', 'list_effects', 'get_effect_info', "
				 "'list_attribute_sets', 'get_attribute_set_info', 'set_effect_properties', 'configure_gas_ability', "
				 "'classify_gas_multiplayer', 'configure_gas_multiplayer_setup'"), true),
		FMCPToolParameter(TEXT("asset_path"), TEXT("string"),
			TEXT("Asset path for introspection/modification (e.g., /Game/GAS/GA_Dash)"), false),
		FMCPToolParameter(TEXT("path_filter"), TEXT("string"),
			TEXT("Path prefix filter for list operations (default: /Game/)"), false, TEXT("/Game/")),
		FMCPToolParameter(TEXT("name_filter"), TEXT("string"),
			TEXT("Name substring filter for list operations"), false),
		FMCPToolParameter(TEXT("limit"), TEXT("number"),
			TEXT("Maximum results (1-500, default: 25)"), false, TEXT("25")),
		FMCPToolParameter(TEXT("class_name"), TEXT("string"),
			TEXT("C++ class name for get_attribute_set_info"), false),
		// set_effect_properties params
		FMCPToolParameter(TEXT("properties"), TEXT("object"),
			TEXT("Properties to set on a GameplayEffect (key-value pairs via reflection)"), false),
		// configure_gas_ability params
		FMCPToolParameter(TEXT("ability_path"), TEXT("string"),
			TEXT("Ability blueprint path (for configure_gas_ability)"), false),
		FMCPToolParameter(TEXT("cost_effect_path"), TEXT("string"),
			TEXT("Cost GameplayEffect path to assign (for configure_gas_ability)"), false),
		FMCPToolParameter(TEXT("cooldown_effect_path"), TEXT("string"),
			TEXT("Cooldown GameplayEffect path to assign (for configure_gas_ability)"), false),
		FMCPToolParameter(TEXT("ability_tags"), TEXT("array"),
			TEXT("Array of gameplay tag strings to set as AbilityTags (for configure_gas_ability)"), false),
		FMCPToolParameter(TEXT("cancel_abilities_with_tags"), TEXT("array"),
			TEXT("Array of tags for CancelAbilitiesWithTag (for configure_gas_ability)"), false),
		FMCPToolParameter(TEXT("block_abilities_with_tags"), TEXT("array"),
			TEXT("Array of tags for BlockAbilitiesWithTag (for configure_gas_ability)"), false),
		FMCPToolParameter(TEXT("dry_run"), TEXT("boolean"),
			TEXT("If true, report what would change without mutating (for configure_gas_ability)"), false, TEXT("false")),
		// configure_gas_multiplayer_setup params
		FMCPToolParameter(TEXT("net_execution_policy"), TEXT("string"),
			TEXT("Net execution policy: LocalPredicted, LocalOnly, ServerInitiated, ServerOnly"), false),
		FMCPToolParameter(TEXT("cosmetic_vfx_path"), TEXT("string"),
			TEXT("Cosmetic VFX reference path (Niagara system) — classified as client-only cosmetic"), false),
	};

	// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
	Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
		TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_GAS::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("list_abilities"))       return ExecuteListAbilities(Params);
	if (Operation == TEXT("get_ability_info"))      return ExecuteGetAbilityInfo(Params);
	if (Operation == TEXT("list_effects"))          return ExecuteListEffects(Params);
	if (Operation == TEXT("get_effect_info"))       return ExecuteGetEffectInfo(Params);
	if (Operation == TEXT("list_attribute_sets"))   return ExecuteListAttributeSets(Params);
	if (Operation == TEXT("get_attribute_set_info"))return ExecuteGetAttributeSetInfo(Params);
	if (Operation == TEXT("set_effect_properties")) return ExecuteSetEffectProperties(Params);
	if (Operation == TEXT("configure_gas_ability")) return ExecuteConfigureGasAbility(Params);
	if (Operation == TEXT("classify_gas_multiplayer")) return ExecuteClassifyGasMultiplayer(Params);
	if (Operation == TEXT("configure_gas_multiplayer_setup")) return ExecuteConfigureGasMultiplayerSetup(Params);

	return FMCPToolResult::Error(FString::Printf(TEXT("Unknown GAS operation: %s"), *Operation));
}

// ===== Helpers =====

UObject* FMCPTool_GAS::LoadAssetByPath(const FString& AssetPath, FString& OutError)
{
	FSoftObjectPath SoftPath(AssetPath);
	UObject* Asset = SoftPath.TryLoad();
	if (!Asset)
	{
		// Try with _C suffix for Blueprint classes
		if (!AssetPath.EndsWith(TEXT("_C")))
		{
			FSoftObjectPath ClassPath(AssetPath + TEXT("_C"));
			Asset = ClassPath.TryLoad();
		}
	}
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("Could not load asset: %s"), *AssetPath);
	}
	return Asset;
}

TSharedPtr<FJsonObject> FMCPTool_GAS::SerializeGameplayTagContainer(const FGameplayTagContainer& Tags)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> TagArray;
	for (const FGameplayTag& Tag : Tags)
	{
		TagArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	Obj->SetArrayField(TEXT("tags"), TagArray);
	Obj->SetNumberField(TEXT("count"), Tags.Num());
	return Obj;
}

// ===== Query: List Abilities =====

FMCPToolResult FMCPTool_GAS::ExecuteListAbilities(const TSharedRef<FJsonObject>& Params)
{
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	Limit = FMath::Clamp(Limit, 1, 500);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Search for Blueprint assets that are based on UGameplayAbility
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*PathFilter));
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& AssetData : Assets)
	{
		if (Results.Num() >= Limit) break;

		// Check if this BP is a GameplayAbility subclass
		FString ParentClassPath;
		FAssetTagValueRef ParentClassRef = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		if (ParentClassRef.IsSet())
		{
			ParentClassPath = ParentClassRef.GetValue();
		}
		else
		{
			// Try NativeParentClass
			FAssetTagValueRef NativeParentRef = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
			if (NativeParentRef.IsSet())
			{
				ParentClassPath = NativeParentRef.GetValue();
			}
		}

		// Check if it's a GameplayAbility derivative
		bool bIsAbility = ParentClassPath.Contains(TEXT("GameplayAbility"));
		if (!bIsAbility)
		{
			// Load and check class hierarchy
			UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
			if (BP && BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
			{
				bIsAbility = true;
			}
		}

		if (!bIsAbility) continue;

		// Name filter
		FString AssetName = AssetData.AssetName.ToString();
		if (!NameFilter.IsEmpty() && !AssetName.Contains(NameFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		Entry->SetStringField(TEXT("parent_class"), ParentClassPath);
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("abilities"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d Gameplay Ability blueprints"), Results.Num()), Data);
}

// ===== Query: Get Ability Info =====

FMCPToolResult FMCPTool_GAS::ExecuteGetAbilityInfo(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset)
	{
		return FMCPToolResult::Error(LoadError);
	}

	// Get the CDO of the ability class
	UGameplayAbility* AbilityCDO = nullptr;

	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			AbilityCDO = BP->GeneratedClass->GetDefaultObject<UGameplayAbility>();
		}
	}
	else if (UGameplayAbility* DirectAbility = Cast<UGameplayAbility>(Asset))
	{
		AbilityCDO = DirectAbility;
	}

	if (!AbilityCDO)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a GameplayAbility: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("class_name"), AbilityCDO->GetClass()->GetName());

	// Instancing policy — read via reflection to avoid deprecated enum warning
	{
		FString InstancingStr = TEXT("Unknown");
		FProperty* InstancingProp = AbilityCDO->GetClass()->FindPropertyByName(FName("InstancingPolicy"));
		if (InstancingProp)
		{
			FString EnumVal;
			InstancingProp->ExportText_Direct(EnumVal, InstancingProp->ContainerPtrToValuePtr<void>(AbilityCDO), nullptr, AbilityCDO, PPF_None);
			if (!EnumVal.IsEmpty())
			{
				InstancingStr = EnumVal;
			}
		}
		Data->SetStringField(TEXT("instancing_policy"), InstancingStr);
	}

	// Net execution policy
	FString NetExecStr;
	switch (AbilityCDO->GetNetExecutionPolicy())
	{
	case EGameplayAbilityNetExecutionPolicy::LocalPredicted:
		NetExecStr = TEXT("LocalPredicted"); break;
	case EGameplayAbilityNetExecutionPolicy::LocalOnly:
		NetExecStr = TEXT("LocalOnly"); break;
	case EGameplayAbilityNetExecutionPolicy::ServerInitiated:
		NetExecStr = TEXT("ServerInitiated"); break;
	case EGameplayAbilityNetExecutionPolicy::ServerOnly:
		NetExecStr = TEXT("ServerOnly"); break;
	default:
		NetExecStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("net_execution_policy"), NetExecStr);

	// Net security policy
	FString NetSecStr;
	switch (AbilityCDO->GetNetSecurityPolicy())
	{
	case EGameplayAbilityNetSecurityPolicy::ClientOrServer:
		NetSecStr = TEXT("ClientOrServer"); break;
	case EGameplayAbilityNetSecurityPolicy::ServerOnlyExecution:
		NetSecStr = TEXT("ServerOnlyExecution"); break;
	case EGameplayAbilityNetSecurityPolicy::ServerOnlyTermination:
		NetSecStr = TEXT("ServerOnlyTermination"); break;
	case EGameplayAbilityNetSecurityPolicy::ServerOnly:
		NetSecStr = TEXT("ServerOnly"); break;
	default:
		NetSecStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("net_security_policy"), NetSecStr);

	// Tags — use public GetAssetTags() for AbilityTags, and FProperty reflection for protected containers
	Data->SetObjectField(TEXT("ability_tags"), SerializeGameplayTagContainer(AbilityCDO->GetAssetTags()));

	// Read protected tag containers via FProperty reflection
	auto ReadTagContainerProperty = [&](const FString& PropName) -> TSharedPtr<FJsonObject>
	{
		FProperty* Prop = AbilityCDO->GetClass()->FindPropertyByName(FName(*PropName));
		if (Prop)
		{
			FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (StructProp && StructProp->Struct == FGameplayTagContainer::StaticStruct())
			{
				const FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(AbilityCDO);
				if (Container)
				{
					return SerializeGameplayTagContainer(*Container);
				}
			}
		}
		TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
		Empty->SetArrayField(TEXT("tags"), TArray<TSharedPtr<FJsonValue>>());
		Empty->SetNumberField(TEXT("count"), 0);
		return Empty;
	};

	Data->SetObjectField(TEXT("cancel_abilities_with_tag"), ReadTagContainerProperty(TEXT("CancelAbilitiesWithTag")));
	Data->SetObjectField(TEXT("block_abilities_with_tag"), ReadTagContainerProperty(TEXT("BlockAbilitiesWithTag")));
	Data->SetObjectField(TEXT("activation_owned_tags"), ReadTagContainerProperty(TEXT("ActivationOwnedTags")));
	Data->SetObjectField(TEXT("activation_required_tags"), ReadTagContainerProperty(TEXT("ActivationRequiredTags")));
	Data->SetObjectField(TEXT("activation_blocked_tags"), ReadTagContainerProperty(TEXT("ActivationBlockedTags")));
	Data->SetObjectField(TEXT("source_required_tags"), ReadTagContainerProperty(TEXT("SourceRequiredTags")));
	Data->SetObjectField(TEXT("source_blocked_tags"), ReadTagContainerProperty(TEXT("SourceBlockedTags")));
	Data->SetObjectField(TEXT("target_required_tags"), ReadTagContainerProperty(TEXT("TargetRequiredTags")));
	Data->SetObjectField(TEXT("target_blocked_tags"), ReadTagContainerProperty(TEXT("TargetBlockedTags")));

	// Cost and Cooldown GE references
	if (UGameplayEffect* CostGE = AbilityCDO->GetCostGameplayEffect())
	{
		Data->SetStringField(TEXT("cost_gameplay_effect"), CostGE->GetPathName());
	}
	else
	{
		Data->SetStringField(TEXT("cost_gameplay_effect"), TEXT("(none)"));
	}

	if (UGameplayEffect* CooldownGE = AbilityCDO->GetCooldownGameplayEffect())
	{
		Data->SetStringField(TEXT("cooldown_gameplay_effect"), CooldownGE->GetPathName());
	}
	else
	{
		Data->SetStringField(TEXT("cooldown_gameplay_effect"), TEXT("(none)"));
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Ability info for %s"), *AbilityCDO->GetClass()->GetName()), Data);
}

// ===== Query: List Effects =====

FMCPToolResult FMCPTool_GAS::ExecuteListEffects(const TSharedRef<FJsonObject>& Params)
{
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	Limit = FMath::Clamp(Limit, 1, 500);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// GameplayEffect is typically a UObject-based Blueprint (not Actor)
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*PathFilter));
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& AssetData : Assets)
	{
		if (Results.Num() >= Limit) break;

		// Check parent class tag
		FString ParentClassPath;
		FAssetTagValueRef ParentClassRef = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		if (ParentClassRef.IsSet())
		{
			ParentClassPath = ParentClassRef.GetValue();
		}
		else
		{
			FAssetTagValueRef NativeParentRef = AssetData.TagsAndValues.FindTag(FName("NativeParentClass"));
			if (NativeParentRef.IsSet())
			{
				ParentClassPath = NativeParentRef.GetValue();
			}
		}

		bool bIsEffect = ParentClassPath.Contains(TEXT("GameplayEffect"));
		if (!bIsEffect)
		{
			UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
			if (BP && BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
			{
				bIsEffect = true;
			}
		}

		if (!bIsEffect) continue;

		FString AssetName = AssetData.AssetName.ToString();
		if (!NameFilter.IsEmpty() && !AssetName.Contains(NameFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		Entry->SetStringField(TEXT("parent_class"), ParentClassPath);
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("effects"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d Gameplay Effect assets"), Results.Num()), Data);
}

// ===== Query: Get Effect Info =====

FMCPToolResult FMCPTool_GAS::ExecuteGetEffectInfo(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UGameplayEffect* EffectCDO = nullptr;

	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			EffectCDO = BP->GeneratedClass->GetDefaultObject<UGameplayEffect>();
		}
	}
	else if (UGameplayEffect* DirectEffect = Cast<UGameplayEffect>(Asset))
	{
		EffectCDO = DirectEffect;
	}

	if (!EffectCDO)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a GameplayEffect: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("class_name"), EffectCDO->GetClass()->GetName());

	// Duration policy
	FString DurationStr;
	switch (EffectCDO->DurationPolicy)
	{
	case EGameplayEffectDurationType::Instant:
		DurationStr = TEXT("Instant"); break;
	case EGameplayEffectDurationType::Infinite:
		DurationStr = TEXT("Infinite"); break;
	case EGameplayEffectDurationType::HasDuration:
		DurationStr = TEXT("HasDuration"); break;
	default:
		DurationStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("duration_policy"), DurationStr);

	// Period (FScalableFloat)
	{
		float PeriodValue = EffectCDO->Period.GetValue();
		Data->SetNumberField(TEXT("period"), PeriodValue);
	}

	// Modifiers
	TArray<TSharedPtr<FJsonValue>> ModArray;
	for (const FGameplayModifierInfo& Mod : EffectCDO->Modifiers)
	{
		TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
		ModObj->SetStringField(TEXT("attribute"), Mod.Attribute.GetName());

		FString ModOpStr;
		switch (Mod.ModifierOp)
		{
		case EGameplayModOp::Additive: ModOpStr = TEXT("Additive"); break;
		case EGameplayModOp::Multiplicitive: ModOpStr = TEXT("Multiplicative"); break;
		case EGameplayModOp::Division: ModOpStr = TEXT("Division"); break;
		case EGameplayModOp::Override: ModOpStr = TEXT("Override"); break;
		default: ModOpStr = TEXT("Unknown"); break;
		}
		ModObj->SetStringField(TEXT("modifier_op"), ModOpStr);

		// Try to get static magnitude
		float MagValue = 0.f;
		if (Mod.ModifierMagnitude.GetStaticMagnitudeIfPossible(1, MagValue))
		{
			ModObj->SetNumberField(TEXT("magnitude"), MagValue);
		}
		else
		{
			ModObj->SetStringField(TEXT("magnitude"), TEXT("(non-static)"));
		}

		ModArray.Add(MakeShared<FJsonValueObject>(ModObj));
	}
	Data->SetArrayField(TEXT("modifiers"), ModArray);
	Data->SetNumberField(TEXT("modifier_count"), ModArray.Num());

	// Stacking
	{
		TSharedPtr<FJsonObject> StackObj = MakeShared<FJsonObject>();
		FString StackTypeStr;
		// Read stacking via reflection to avoid deprecated field access
		FProperty* StackTypeProp = EffectCDO->GetClass()->FindPropertyByName(FName("StackingType"));
		if (StackTypeProp)
		{
			StackTypeProp->ExportText_Direct(StackTypeStr, StackTypeProp->ContainerPtrToValuePtr<void>(EffectCDO), nullptr, EffectCDO, PPF_None);
		}
		StackObj->SetStringField(TEXT("type"), StackTypeStr);

		FProperty* StackLimitProp = EffectCDO->GetClass()->FindPropertyByName(FName("StackLimitCount"));
		if (StackLimitProp)
		{
			FString LimitStr;
			StackLimitProp->ExportText_Direct(LimitStr, StackLimitProp->ContainerPtrToValuePtr<void>(EffectCDO), nullptr, EffectCDO, PPF_None);
			StackObj->SetStringField(TEXT("limit_count"), LimitStr);
		}
		Data->SetObjectField(TEXT("stacking"), StackObj);
	}

	// Tags on the effect
	Data->SetObjectField(TEXT("asset_tags"), SerializeGameplayTagContainer(EffectCDO->GetAssetTags()));

	// Granted tags
	Data->SetObjectField(TEXT("granted_tags"), SerializeGameplayTagContainer(EffectCDO->GetGrantedTags()));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Effect info for %s"), *EffectCDO->GetClass()->GetName()), Data);
}

// ===== Query: List Attribute Sets =====

FMCPToolResult FMCPTool_GAS::ExecuteListAttributeSets(const TSharedRef<FJsonObject>& Params)
{
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));

	TArray<TSharedPtr<FJsonValue>> Results;

	// Find all UAttributeSet subclasses via reflection
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->IsChildOf(UAttributeSet::StaticClass()) && Class != UAttributeSet::StaticClass())
		{
			FString ClassName = Class->GetName();
			if (!NameFilter.IsEmpty() && !ClassName.Contains(NameFilter))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), ClassName);
			Entry->SetStringField(TEXT("path"), Class->GetPathName());

			// Count attributes
			int32 AttrCount = 0;
			for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
			{
				if (FGameplayAttribute::IsGameplayAttributeDataProperty(*PropIt))
				{
					AttrCount++;
				}
			}
			Entry->SetNumberField(TEXT("attribute_count"), AttrCount);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("attribute_sets"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d AttributeSet classes"), Results.Num()), Data);
}

// ===== Query: Get Attribute Set Info =====

FMCPToolResult FMCPTool_GAS::ExecuteGetAttributeSetInfo(const TSharedRef<FJsonObject>& Params)
{
	FString ClassName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("class_name"), ClassName, Error))
	{
		return Error.GetValue();
	}

	// Find the class
	UClass* FoundClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if ((*It)->IsChildOf(UAttributeSet::StaticClass()) && (*It)->GetName() == ClassName)
		{
			FoundClass = *It;
			break;
		}
	}

	if (!FoundClass)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("AttributeSet class not found: %s"), *ClassName));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), FoundClass->GetName());
	Data->SetStringField(TEXT("class_path"), FoundClass->GetPathName());
	Data->SetStringField(TEXT("parent_class"), FoundClass->GetSuperClass()->GetName());

	TArray<TSharedPtr<FJsonValue>> Attributes;
	for (TFieldIterator<FProperty> PropIt(FoundClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (FGameplayAttribute::IsGameplayAttributeDataProperty(Prop))
		{
			TSharedPtr<FJsonObject> AttrObj = MakeShared<FJsonObject>();
			AttrObj->SetStringField(TEXT("name"), Prop->GetName());
			AttrObj->SetStringField(TEXT("type"), Prop->GetCPPType());

			// Try to get default value from CDO
			UAttributeSet* CDO = FoundClass->GetDefaultObject<UAttributeSet>();
			if (CDO)
			{
				FGameplayAttribute GameplayAttr(Prop);
				float DefaultValue = GameplayAttr.GetNumericValue(CDO);
				AttrObj->SetNumberField(TEXT("default_value"), DefaultValue);
			}

			Attributes.Add(MakeShared<FJsonValueObject>(AttrObj));
		}
	}

	Data->SetArrayField(TEXT("attributes"), Attributes);
	Data->SetNumberField(TEXT("attribute_count"), Attributes.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("AttributeSet %s: %d attributes"), *ClassName, Attributes.Num()), Data);
}

// ===== Modify: Set Effect Properties =====

FMCPToolResult FMCPTool_GAS::ExecuteSetEffectProperties(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !(*PropsObj).IsValid())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: properties (object)"));
	}

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset)
	{
		return FMCPToolResult::Error(LoadError);
	}

	// Get the CDO
	UGameplayEffect* EffectCDO = nullptr;
	UBlueprint* BP = nullptr;

	if (UBlueprint* BPAsset = Cast<UBlueprint>(Asset))
	{
		BP = BPAsset;
		if (BPAsset->GeneratedClass && BPAsset->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			EffectCDO = BPAsset->GeneratedClass->GetDefaultObject<UGameplayEffect>();
		}
	}
	else if (UGameplayEffect* DirectEffect = Cast<UGameplayEffect>(Asset))
	{
		EffectCDO = DirectEffect;
	}

	if (!EffectCDO)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a GameplayEffect: %s"), *AssetPath));
	}

	// Set properties via reflection
	TArray<TSharedPtr<FJsonValue>> Changed;
	TArray<TSharedPtr<FJsonValue>> Failed;

	for (const auto& Pair : (*PropsObj)->Values)
	{
		const FString& PropName = Pair.Key;
		const TSharedPtr<FJsonValue>& Value = Pair.Value;

		FProperty* Prop = EffectCDO->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("property"), PropName);
			FailObj->SetStringField(TEXT("reason"), TEXT("property not found"));
			Failed.Add(MakeShared<FJsonValueObject>(FailObj));
			continue;
		}

		// Use ImportText for string-based setting
		FString ValueStr;
		if (Value->Type == EJson::String)
		{
			ValueStr = Value->AsString();
		}
		else if (Value->Type == EJson::Number)
		{
			ValueStr = FString::SanitizeFloat(Value->AsNumber());
		}
		else if (Value->Type == EJson::Boolean)
		{
			ValueStr = Value->AsBool() ? TEXT("True") : TEXT("False");
		}
		else
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("property"), PropName);
			FailObj->SetStringField(TEXT("reason"), TEXT("unsupported value type (use string/number/boolean)"));
			Failed.Add(MakeShared<FJsonValueObject>(FailObj));
			continue;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(EffectCDO);
		if (Prop->ImportText_Direct(*ValueStr, ValuePtr, EffectCDO, PPF_None) != nullptr)
		{
			TSharedPtr<FJsonObject> OkObj = MakeShared<FJsonObject>();
			OkObj->SetStringField(TEXT("property"), PropName);
			OkObj->SetStringField(TEXT("status"), TEXT("changed"));
			Changed.Add(MakeShared<FJsonValueObject>(OkObj));
		}
		else
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("property"), PropName);
			FailObj->SetStringField(TEXT("reason"), TEXT("ImportText failed"));
			Failed.Add(MakeShared<FJsonValueObject>(FailObj));
		}
	}

	// Mark modified and save
	if (Changed.Num() > 0)
	{
		EffectCDO->MarkPackageDirty();
		if (BP)
		{
			BP->MarkPackageDirty();
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("changed"), Changed);
	Data->SetArrayField(TEXT("failed"), Failed);
	Data->SetNumberField(TEXT("changed_count"), Changed.Num());
	Data->SetNumberField(TEXT("failed_count"), Failed.Num());

	// Determine truthful success status
	bool bAllFailed = Changed.Num() == 0 && Failed.Num() > 0;
	bool bPartialSuccess = Changed.Num() > 0 && Failed.Num() > 0;
	bool bFullSuccess = Changed.Num() > 0 && Failed.Num() == 0;

	if (bPartialSuccess)
	{
		Data->SetBoolField(TEXT("partial_success"), true);
	}

	// Emit receipt
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("gas");
		Receipt.Summary = TEXT("set_effect_properties");
		Receipt.bSuccess = !bAllFailed;
		Receipt.bPartialSuccess = bPartialSuccess;
		Receipt.Targets.Add(AssetPath);
		Receipt.Classification = TEXT("user_mutation");
		if (bAllFailed)
		{
			Receipt.ErrorOrDenialReason = FString::Printf(TEXT("All %d properties failed"), Failed.Num());
		}
		FUnrealClaudeExecutionLog::Get().AddReceipt(Receipt);
	}

	FString Message = FString::Printf(TEXT("Set %d properties on GE %s (%d failed)"), Changed.Num(), *AssetPath, Failed.Num());

	if (bAllFailed)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("All %d properties failed on GE %s"), Failed.Num(), *AssetPath));
	}

	FMCPToolResult Result = FMCPToolResult::Success(Message, Data);
	return Result;
}

// ===== Modify: Configure GAS Ability (Composed Bundle) =====

FMCPToolResult FMCPTool_GAS::ExecuteConfigureGasAbility(const TSharedRef<FJsonObject>& Params)
{
	FString AbilityPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("ability_path"), AbilityPath, Error))
	{
		return Error.GetValue();
	}

	bool bDryRun = ExtractOptionalBool(Params, TEXT("dry_run"), false);

	FString LoadError;
	UObject* AbilityAsset = LoadAssetByPath(AbilityPath, LoadError);
	if (!AbilityAsset)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UGameplayAbility* AbilityCDO = nullptr;
	UBlueprint* AbilityBP = nullptr;

	if (UBlueprint* BP = Cast<UBlueprint>(AbilityAsset))
	{
		AbilityBP = BP;
		if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			AbilityCDO = BP->GeneratedClass->GetDefaultObject<UGameplayAbility>();
		}
	}

	if (!AbilityCDO)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a GameplayAbility: %s"), *AbilityPath));
	}

	TArray<TSharedPtr<FJsonValue>> Actions;

	auto AddAction = [&](const FString& Name, const FString& Status, const FString& Detail = FString())
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("action"), Name);
		Obj->SetStringField(TEXT("status"), Status);
		if (!Detail.IsEmpty())
		{
			Obj->SetStringField(TEXT("detail"), Detail);
		}
		Actions.Add(MakeShared<FJsonValueObject>(Obj));
	};

	// Helper to parse tag array from JSON
	auto ParseTagArray = [&](const FString& ParamName) -> TArray<FString>
	{
		TArray<FString> Tags;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(ParamName, Arr) && Arr)
		{
			for (const auto& Val : *Arr)
			{
				FString TagStr;
				if (Val->TryGetString(TagStr))
				{
					Tags.Add(TagStr);
				}
			}
		}
		return Tags;
	};

	// Helper to set tags on a container via FProperty reflection (tags are protected)
	auto SetTagContainerByName = [&](const FString& PropName, const TArray<FString>& TagStrings, const FString& ActionName) -> bool
	{
		FGameplayTagContainer NewTags;
		for (const FString& TagStr : TagStrings)
		{
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
			if (Tag.IsValid())
			{
				NewTags.AddTag(Tag);
			}
			else
			{
				AddAction(ActionName, TEXT("failed"), FString::Printf(TEXT("Invalid tag: %s"), *TagStr));
				return false;
			}
		}

		// Access via reflection
		FProperty* Prop = AbilityCDO->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			AddAction(ActionName, TEXT("failed"), FString::Printf(TEXT("Property %s not found"), *PropName));
			return false;
		}
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp || StructProp->Struct != FGameplayTagContainer::StaticStruct())
		{
			AddAction(ActionName, TEXT("failed"), FString::Printf(TEXT("Property %s is not FGameplayTagContainer"), *PropName));
			return false;
		}
		FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(AbilityCDO);
		if (!Container)
		{
			AddAction(ActionName, TEXT("failed"), TEXT("Could not get container pointer"));
			return false;
		}

		if (Container->HasAll(NewTags) && Container->Num() == NewTags.Num())
		{
			AddAction(ActionName, bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"));
			return true;
		}

		if (!bDryRun)
		{
			Container->Reset();
			Container->AppendTags(NewTags);
		}
		AddAction(ActionName, bDryRun ? TEXT("would_update") : TEXT("updated"),
			FString::Printf(TEXT("%d tags"), TagStrings.Num()));
		return true;
	};

	// 1. Ability Tags (protected — use reflection, same as other tag containers)
	TArray<FString> AbilityTags = ParseTagArray(TEXT("ability_tags"));
	if (AbilityTags.Num() > 0)
	{
		SetTagContainerByName(TEXT("AbilityTags"), AbilityTags, TEXT("set_ability_tags"));
	}

	// 2. Cancel Abilities With Tags (protected — use reflection)
	TArray<FString> CancelTags = ParseTagArray(TEXT("cancel_abilities_with_tags"));
	if (CancelTags.Num() > 0)
	{
		SetTagContainerByName(TEXT("CancelAbilitiesWithTag"), CancelTags, TEXT("set_cancel_abilities_with_tag"));
	}

	// 3. Block Abilities With Tags (protected — use reflection)
	TArray<FString> BlockTags = ParseTagArray(TEXT("block_abilities_with_tags"));
	if (BlockTags.Num() > 0)
	{
		SetTagContainerByName(TEXT("BlockAbilitiesWithTag"), BlockTags, TEXT("set_block_abilities_with_tag"));
	}

	// 4. Cost Effect
	FString CostPath = ExtractOptionalString(Params, TEXT("cost_effect_path"));
	if (!CostPath.IsEmpty())
	{
		FString CostLoadError;
		UObject* CostAsset = LoadAssetByPath(CostPath, CostLoadError);
		UGameplayEffect* CostCDO = nullptr;

		if (CostAsset)
		{
			if (UBlueprint* CostBP = Cast<UBlueprint>(CostAsset))
			{
				if (CostBP->GeneratedClass && CostBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
				{
					CostCDO = CostBP->GeneratedClass->GetDefaultObject<UGameplayEffect>();
				}
			}
		}

		if (CostCDO)
		{
			// Check CostGameplayEffectClass property
			UClass* CurrentCostClass = nullptr;
			FProperty* CostProp = AbilityCDO->GetClass()->FindPropertyByName(FName("CostGameplayEffectClass"));
			if (CostProp)
			{
				FClassProperty* ClassProp = CastField<FClassProperty>(CostProp);
				if (ClassProp)
				{
					UObject* CurrentVal = ClassProp->GetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO));
					CurrentCostClass = Cast<UClass>(CurrentVal);
				}
			}

			if (CurrentCostClass == CostCDO->GetClass())
			{
				AddAction(TEXT("set_cost_effect"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"), CostPath);
			}
			else if (!bDryRun)
			{
				// Set via reflection
				if (CostProp)
				{
					FClassProperty* ClassProp = CastField<FClassProperty>(CostProp);
					if (ClassProp)
					{
						ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO), CostCDO->GetClass());
						AddAction(TEXT("set_cost_effect"), TEXT("updated"), CostPath);
					}
					else
					{
						AddAction(TEXT("set_cost_effect"), TEXT("failed"), TEXT("CostGameplayEffectClass is not a class property"));
					}
				}
				else
				{
					AddAction(TEXT("set_cost_effect"), TEXT("failed"), TEXT("CostGameplayEffectClass property not found"));
				}
			}
			else
			{
				AddAction(TEXT("set_cost_effect"), TEXT("would_update"), CostPath);
			}
		}
		else
		{
			AddAction(TEXT("set_cost_effect"), TEXT("failed"), CostLoadError.IsEmpty() ? TEXT("Not a GameplayEffect") : CostLoadError);
		}
	}

	// 5. Cooldown Effect
	FString CooldownPath = ExtractOptionalString(Params, TEXT("cooldown_effect_path"));
	if (!CooldownPath.IsEmpty())
	{
		FString CooldownLoadError;
		UObject* CooldownAsset = LoadAssetByPath(CooldownPath, CooldownLoadError);
		UGameplayEffect* CooldownCDO = nullptr;

		if (CooldownAsset)
		{
			if (UBlueprint* CooldownBP = Cast<UBlueprint>(CooldownAsset))
			{
				if (CooldownBP->GeneratedClass && CooldownBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
				{
					CooldownCDO = CooldownBP->GeneratedClass->GetDefaultObject<UGameplayEffect>();
				}
			}
		}

		if (CooldownCDO)
		{
			UClass* CurrentCooldownClass = nullptr;
			FProperty* CooldownProp = AbilityCDO->GetClass()->FindPropertyByName(FName("CooldownGameplayEffectClass"));
			if (CooldownProp)
			{
				FClassProperty* ClassProp = CastField<FClassProperty>(CooldownProp);
				if (ClassProp)
				{
					UObject* CurrentVal = ClassProp->GetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO));
					CurrentCooldownClass = Cast<UClass>(CurrentVal);
				}
			}

			if (CurrentCooldownClass == CooldownCDO->GetClass())
			{
				AddAction(TEXT("set_cooldown_effect"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"), CooldownPath);
			}
			else if (!bDryRun)
			{
				if (CooldownProp)
				{
					FClassProperty* ClassProp = CastField<FClassProperty>(CooldownProp);
					if (ClassProp)
					{
						ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO), CooldownCDO->GetClass());
						AddAction(TEXT("set_cooldown_effect"), TEXT("updated"), CooldownPath);
					}
					else
					{
						AddAction(TEXT("set_cooldown_effect"), TEXT("failed"), TEXT("CooldownGameplayEffectClass is not a class property"));
					}
				}
				else
				{
					AddAction(TEXT("set_cooldown_effect"), TEXT("failed"), TEXT("CooldownGameplayEffectClass property not found"));
				}
			}
			else
			{
				AddAction(TEXT("set_cooldown_effect"), TEXT("would_update"), CooldownPath);
			}
		}
		else
		{
			AddAction(TEXT("set_cooldown_effect"), TEXT("failed"),
				CooldownLoadError.IsEmpty() ? TEXT("Not a GameplayEffect") : CooldownLoadError);
		}
	}

	// Mark dirty if not dry run and something changed
	if (!bDryRun)
	{
		bool bAnyChanged = false;
		for (const auto& ActionVal : Actions)
		{
			const TSharedPtr<FJsonObject>* ActionObj = nullptr;
			if (ActionVal->TryGetObject(ActionObj) && ActionObj)
			{
				FString Status;
				if ((*ActionObj)->TryGetStringField(TEXT("status"), Status) && Status == TEXT("updated"))
				{
					bAnyChanged = true;
					break;
				}
			}
		}

		if (bAnyChanged)
		{
			AbilityCDO->MarkPackageDirty();
			if (AbilityBP)
			{
				AbilityBP->MarkPackageDirty();
			}
		}
	}

	// Count action statuses for truthful top-level result
	int32 FailedCount = 0;
	int32 UpdatedCount = 0;
	int32 UnchangedCount = 0;
	int32 WouldCount = 0;
	for (const auto& ActionVal : Actions)
	{
		const TSharedPtr<FJsonObject>* ActionObj = nullptr;
		if (ActionVal->TryGetObject(ActionObj) && ActionObj)
		{
			FString Status;
			if ((*ActionObj)->TryGetStringField(TEXT("status"), Status))
			{
				if (Status == TEXT("failed")) FailedCount++;
				else if (Status == TEXT("updated")) UpdatedCount++;
				else if (Status == TEXT("unchanged")) UnchangedCount++;
				else if (Status.StartsWith(TEXT("would_"))) WouldCount++;
			}
		}
	}

	bool bAllFailed = FailedCount > 0 && UpdatedCount == 0 && UnchangedCount == 0 && WouldCount == 0;
	bool bPartialSuccess = FailedCount > 0 && (UpdatedCount > 0 || UnchangedCount > 0);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("ability_path"), AbilityPath);
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetArrayField(TEXT("actions"), Actions);
	Data->SetNumberField(TEXT("action_count"), Actions.Num());
	Data->SetNumberField(TEXT("updated_count"), UpdatedCount);
	Data->SetNumberField(TEXT("unchanged_count"), UnchangedCount);
	Data->SetNumberField(TEXT("failed_count"), FailedCount);

	if (bPartialSuccess)
	{
		Data->SetBoolField(TEXT("partial_success"), true);
	}

	// Emit receipt
	if (!bDryRun)
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("gas");
		Receipt.Summary = TEXT("configure_gas_ability");
		Receipt.bSuccess = !bAllFailed;
		Receipt.bPartialSuccess = bPartialSuccess;
		Receipt.Targets.Add(AbilityPath);
		Receipt.Classification = TEXT("user_mutation");
		if (bAllFailed)
		{
			Receipt.ErrorOrDenialReason = FString::Printf(TEXT("All %d actions failed"), FailedCount);
		}
		FUnrealClaudeExecutionLog::Get().AddReceipt(Receipt);
	}

	FString Message = FString::Printf(TEXT("%s configure_gas_ability: %d actions on %s (%d updated, %d unchanged, %d failed)"),
		bDryRun ? TEXT("[dry_run]") : TEXT("Applied"),
		Actions.Num(), *AbilityPath, UpdatedCount, UnchangedCount, FailedCount);

	if (bAllFailed && !bDryRun)
	{
		return FMCPToolResult::Error(Message);
	}

	FMCPToolResult Result = FMCPToolResult::Success(Message, Data);
	return Result;
}

// ===== Query: Classify GAS Multiplayer =====

FMCPToolResult FMCPTool_GAS::ExecuteClassifyGasMultiplayer(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UGameplayAbility* AbilityCDO = nullptr;
	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			AbilityCDO = BP->GeneratedClass->GetDefaultObject<UGameplayAbility>();
		}
	}

	if (!AbilityCDO)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a GameplayAbility: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("class_name"), AbilityCDO->GetClass()->GetName());

	// Net policies
	EGameplayAbilityNetExecutionPolicy::Type NetExec = AbilityCDO->GetNetExecutionPolicy();
	EGameplayAbilityNetSecurityPolicy::Type NetSec = AbilityCDO->GetNetSecurityPolicy();

	FString NetExecStr;
	switch (NetExec)
	{
	case EGameplayAbilityNetExecutionPolicy::LocalPredicted: NetExecStr = TEXT("LocalPredicted"); break;
	case EGameplayAbilityNetExecutionPolicy::LocalOnly: NetExecStr = TEXT("LocalOnly"); break;
	case EGameplayAbilityNetExecutionPolicy::ServerInitiated: NetExecStr = TEXT("ServerInitiated"); break;
	case EGameplayAbilityNetExecutionPolicy::ServerOnly: NetExecStr = TEXT("ServerOnly"); break;
	default: NetExecStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("net_execution_policy"), NetExecStr);

	FString NetSecStr;
	switch (NetSec)
	{
	case EGameplayAbilityNetSecurityPolicy::ClientOrServer: NetSecStr = TEXT("ClientOrServer"); break;
	case EGameplayAbilityNetSecurityPolicy::ServerOnlyExecution: NetSecStr = TEXT("ServerOnlyExecution"); break;
	case EGameplayAbilityNetSecurityPolicy::ServerOnlyTermination: NetSecStr = TEXT("ServerOnlyTermination"); break;
	case EGameplayAbilityNetSecurityPolicy::ServerOnly: NetSecStr = TEXT("ServerOnly"); break;
	default: NetSecStr = TEXT("Unknown"); break;
	}
	Data->SetStringField(TEXT("net_security_policy"), NetSecStr);

	// Multiplayer classification
	TArray<TSharedPtr<FJsonValue>> Classifications;
	TArray<TSharedPtr<FJsonValue>> Guidance;

	TSharedPtr<FJsonObject> AuthPath = MakeShared<FJsonObject>();
	switch (NetExec)
	{
	case EGameplayAbilityNetExecutionPolicy::ServerOnly:
		AuthPath->SetStringField(TEXT("type"), TEXT("server-authoritative"));
		AuthPath->SetStringField(TEXT("execution"), TEXT("Server only — client cannot execute. Safest for authoritative gameplay."));
		AuthPath->SetStringField(TEXT("use_case"), TEXT("Damage, healing, stat changes, ability granting"));
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("server-authoritative")));
		break;
	case EGameplayAbilityNetExecutionPolicy::ServerInitiated:
		AuthPath->SetStringField(TEXT("type"), TEXT("server-initiated"));
		AuthPath->SetStringField(TEXT("execution"), TEXT("Server initiates, can run on client if server tells it to."));
		AuthPath->SetStringField(TEXT("use_case"), TEXT("Server-driven gameplay events that need client-side feedback"));
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("server-initiated")));
		break;
	case EGameplayAbilityNetExecutionPolicy::LocalPredicted:
		AuthPath->SetStringField(TEXT("type"), TEXT("client-predicted"));
		AuthPath->SetStringField(TEXT("execution"), TEXT("Client predicts locally, server confirms/corrects. Best responsiveness."));
		AuthPath->SetStringField(TEXT("use_case"), TEXT("Movement abilities, dashes, jumps — anything that needs instant client feedback"));
		AuthPath->SetStringField(TEXT("warning"), TEXT("Prediction mismatch = rollback. Keep predicted state minimal."));
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("client-predicted")));
		break;
	case EGameplayAbilityNetExecutionPolicy::LocalOnly:
		AuthPath->SetStringField(TEXT("type"), TEXT("local-only"));
		AuthPath->SetStringField(TEXT("execution"), TEXT("Runs locally only. NOT replicated. NOT server-verified."));
		AuthPath->SetStringField(TEXT("use_case"), TEXT("Cosmetic-only abilities, UI effects, local feedback"));
		Classifications.Add(MakeShared<FJsonValueString>(TEXT("cosmetic-local-only")));
		break;
	default:
		AuthPath->SetStringField(TEXT("type"), TEXT("unknown"));
		break;
	}
	Data->SetObjectField(TEXT("authority_path"), AuthPath);

	// Cosmetic split guidance
	TSharedPtr<FJsonObject> CosmeticGuide = MakeShared<FJsonObject>();
	CosmeticGuide->SetStringField(TEXT("authoritative_effects"), TEXT("GameplayEffects (cost, cooldown, damage) — server-authoritative, replicated via GAS"));
	CosmeticGuide->SetStringField(TEXT("cosmetic_effects"), TEXT("VFX (Niagara), sounds, animations — client-local, NOT replicated. Trigger from RepNotify or GameplayCue."));
	CosmeticGuide->SetStringField(TEXT("bridge_pattern"), TEXT("Server applies GE (authoritative) → client detects via RepNotify/Cue → client spawns VFX (cosmetic)"));
	Data->SetObjectField(TEXT("cosmetic_split_guide"), CosmeticGuide);

	// Cost/cooldown
	UGameplayEffect* CostGE = AbilityCDO->GetCostGameplayEffect();
	UGameplayEffect* CooldownGE = AbilityCDO->GetCooldownGameplayEffect();
	Data->SetStringField(TEXT("cost_effect"), CostGE ? CostGE->GetPathName() : TEXT("(none)"));
	Data->SetStringField(TEXT("cooldown_effect"), CooldownGE ? CooldownGE->GetPathName() : TEXT("(none)"));

	if (CostGE)
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Cost GE is server-authoritative — server validates cost before ability executes")));
	if (CooldownGE)
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("Cooldown GE is server-authoritative — server enforces cooldown duration")));
	if (!CostGE && !CooldownGE)
		Guidance.Add(MakeShared<FJsonValueString>(TEXT("No cost/cooldown — ability can be spammed. Consider adding server-authoritative cost for multiplayer.")));

	Data->SetArrayField(TEXT("classifications"), Classifications);
	Data->SetArrayField(TEXT("guidance"), Guidance);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("GAS multiplayer classification for %s"), *AbilityCDO->GetClass()->GetName()), Data);
}

// ===== Modify: Configure GAS Multiplayer Setup =====

FMCPToolResult FMCPTool_GAS::ExecuteConfigureGasMultiplayerSetup(const TSharedRef<FJsonObject>& Params)
{
	FString AbilityPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("ability_path"), AbilityPath, Error))
	{
		return Error.GetValue();
	}

	bool bDryRun = ExtractOptionalBool(Params, TEXT("dry_run"), false);

	FString LoadError;
	UObject* AbilityAsset = LoadAssetByPath(AbilityPath, LoadError);
	if (!AbilityAsset)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UGameplayAbility* AbilityCDO = nullptr;
	UBlueprint* AbilityBP = nullptr;

	if (UBlueprint* BP = Cast<UBlueprint>(AbilityAsset))
	{
		AbilityBP = BP;
		if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			AbilityCDO = BP->GeneratedClass->GetDefaultObject<UGameplayAbility>();
		}
	}

	if (!AbilityCDO)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a GameplayAbility: %s"), *AbilityPath));
	}

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
		else if (Status == TEXT("failed")) FailedCount++;
	};

	// 1. Net execution policy via reflection
	FString NetExecStr = ExtractOptionalString(Params, TEXT("net_execution_policy"));
	if (!NetExecStr.IsEmpty())
	{
		FProperty* NetExecProp = AbilityCDO->GetClass()->FindPropertyByName(FName("NetExecutionPolicy"));
		if (NetExecProp)
		{
			FString CurrentVal;
			NetExecProp->ExportText_Direct(CurrentVal, NetExecProp->ContainerPtrToValuePtr<void>(AbilityCDO), nullptr, AbilityCDO, PPF_None);
			if (CurrentVal == NetExecStr)
			{
				AddAction(TEXT("set_net_execution_policy"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"));
			}
			else if (!bDryRun)
			{
				void* ValPtr = NetExecProp->ContainerPtrToValuePtr<void>(AbilityCDO);
				if (NetExecProp->ImportText_Direct(*NetExecStr, ValPtr, AbilityCDO, PPF_None) != nullptr)
				{
					AddAction(TEXT("set_net_execution_policy"), TEXT("updated"), NetExecStr);
				}
				else
				{
					AddAction(TEXT("set_net_execution_policy"), TEXT("failed"), TEXT("ImportText failed"));
				}
			}
			else
			{
				AddAction(TEXT("set_net_execution_policy"), TEXT("would_update"), NetExecStr);
			}
		}
	}

	// 2. Cost effect (authoritative)
	FString CostPath = ExtractOptionalString(Params, TEXT("cost_effect_path"));
	if (!CostPath.IsEmpty())
	{
		FString CostErr;
		UObject* CostAsset = LoadAssetByPath(CostPath, CostErr);
		UGameplayEffect* CostCDO = nullptr;
		if (CostAsset)
		{
			if (UBlueprint* CostBP = Cast<UBlueprint>(CostAsset))
			{
				if (CostBP->GeneratedClass && CostBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
					CostCDO = CostBP->GeneratedClass->GetDefaultObject<UGameplayEffect>();
			}
		}

		if (CostCDO)
		{
			FProperty* CostProp = AbilityCDO->GetClass()->FindPropertyByName(FName("CostGameplayEffectClass"));
			FClassProperty* ClassProp = CostProp ? CastField<FClassProperty>(CostProp) : nullptr;
			if (ClassProp)
			{
				UObject* CurVal = ClassProp->GetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO));
				if (Cast<UClass>(CurVal) == CostCDO->GetClass())
				{
					AddAction(TEXT("set_cost_authoritative"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"), CostPath);
				}
				else if (!bDryRun)
				{
					ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO), CostCDO->GetClass());
					AddAction(TEXT("set_cost_authoritative"), TEXT("updated"), FString::Printf(TEXT("%s (server-authoritative)"), *CostPath));
				}
				else
				{
					AddAction(TEXT("set_cost_authoritative"), TEXT("would_update"), CostPath);
				}
			}
			else
			{
				AddAction(TEXT("set_cost_authoritative"), TEXT("failed"), TEXT("CostGameplayEffectClass property not found"));
			}
		}
		else
		{
			AddAction(TEXT("set_cost_authoritative"), TEXT("failed"), CostErr.IsEmpty() ? TEXT("Not a GameplayEffect") : CostErr);
		}
	}

	// 3. Cooldown effect (authoritative)
	FString CdPath = ExtractOptionalString(Params, TEXT("cooldown_effect_path"));
	if (!CdPath.IsEmpty())
	{
		FString CdErr;
		UObject* CdAsset = LoadAssetByPath(CdPath, CdErr);
		UGameplayEffect* CdCDO = nullptr;
		if (CdAsset)
		{
			if (UBlueprint* CdBP = Cast<UBlueprint>(CdAsset))
			{
				if (CdBP->GeneratedClass && CdBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
					CdCDO = CdBP->GeneratedClass->GetDefaultObject<UGameplayEffect>();
			}
		}

		if (CdCDO)
		{
			FProperty* CdProp = AbilityCDO->GetClass()->FindPropertyByName(FName("CooldownGameplayEffectClass"));
			FClassProperty* ClassProp = CdProp ? CastField<FClassProperty>(CdProp) : nullptr;
			if (ClassProp)
			{
				UObject* CurVal = ClassProp->GetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO));
				if (Cast<UClass>(CurVal) == CdCDO->GetClass())
				{
					AddAction(TEXT("set_cooldown_authoritative"), bDryRun ? TEXT("would_be_unchanged") : TEXT("unchanged"), CdPath);
				}
				else if (!bDryRun)
				{
					ClassProp->SetObjectPropertyValue(ClassProp->ContainerPtrToValuePtr<void>(AbilityCDO), CdCDO->GetClass());
					AddAction(TEXT("set_cooldown_authoritative"), TEXT("updated"), FString::Printf(TEXT("%s (server-authoritative)"), *CdPath));
				}
				else
				{
					AddAction(TEXT("set_cooldown_authoritative"), TEXT("would_update"), CdPath);
				}
			}
		}
		else
		{
			AddAction(TEXT("set_cooldown_authoritative"), TEXT("failed"), CdErr.IsEmpty() ? TEXT("Not a GameplayEffect") : CdErr);
		}
	}

	// 4. Cosmetic VFX reference
	FString VfxPath = ExtractOptionalString(Params, TEXT("cosmetic_vfx_path"));
	if (!VfxPath.IsEmpty())
	{
		FSoftObjectPath VfxSoftPath(VfxPath);
		UObject* VfxAsset = VfxSoftPath.TryLoad();
		if (VfxAsset)
		{
			AddAction(TEXT("classify_cosmetic_vfx"), bDryRun ? TEXT("would_update") : TEXT("updated"),
				FString::Printf(TEXT("%s — client-only cosmetic (NOT replicated, trigger via RepNotify/GameplayCue)"), *VfxPath));
		}
		else
		{
			AddAction(TEXT("classify_cosmetic_vfx"), TEXT("failed"),
				FString::Printf(TEXT("VFX asset not found: %s"), *VfxPath));
		}
	}

	if (!bDryRun && UpdatedCount > 0)
	{
		AbilityCDO->MarkPackageDirty();
		if (AbilityBP) AbilityBP->MarkPackageDirty();
	}

	bool bAllFailed = FailedCount > 0 && UpdatedCount == 0 && UnchangedCount == 0;
	bool bPartialSuccess = FailedCount > 0 && (UpdatedCount > 0 || UnchangedCount > 0);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("ability_path"), AbilityPath);
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetArrayField(TEXT("actions"), Actions);
	Data->SetNumberField(TEXT("action_count"), Actions.Num());
	Data->SetNumberField(TEXT("updated_count"), UpdatedCount);
	Data->SetNumberField(TEXT("unchanged_count"), UnchangedCount);
	Data->SetNumberField(TEXT("failed_count"), FailedCount);
	if (bPartialSuccess) Data->SetBoolField(TEXT("partial_success"), true);

	TSharedPtr<FJsonObject> SplitSummary = MakeShared<FJsonObject>();
	SplitSummary->SetStringField(TEXT("authoritative"), TEXT("Cost + Cooldown GEs — server-enforced via GAS replication"));
	SplitSummary->SetStringField(TEXT("cosmetic"), TEXT("VFX — client-only, trigger via RepNotify or GameplayCue"));
	SplitSummary->SetStringField(TEXT("bridge_pattern"), TEXT("Server: ApplyGE → client: OnRep_/Cue fires → client: SpawnVFX"));
	Data->SetObjectField(TEXT("multiplayer_split"), SplitSummary);

	if (!bDryRun)
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("gas");
		Receipt.Summary = TEXT("configure_gas_multiplayer_setup");
		Receipt.bSuccess = !bAllFailed;
		Receipt.bPartialSuccess = bPartialSuccess;
		Receipt.Targets.Add(AbilityPath);
		Receipt.Classification = TEXT("user_mutation");
		FUnrealClaudeExecutionLog::Get().AddReceipt(Receipt);
	}

	FString Message = FString::Printf(TEXT("%s configure_gas_multiplayer_setup: %d actions (%d updated, %d unchanged, %d failed)"),
		bDryRun ? TEXT("[dry_run]") : TEXT("Applied"), Actions.Num(), UpdatedCount, UnchangedCount, FailedCount);
	if (bAllFailed && !bDryRun) return FMCPToolResult::Error(Message);
	return FMCPToolResult::Success(Message, Data);
}
