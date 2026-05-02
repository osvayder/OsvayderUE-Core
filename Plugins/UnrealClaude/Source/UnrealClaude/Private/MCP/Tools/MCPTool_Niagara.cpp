// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Niagara.h"
#include "UnrealClaudeExecutionLog.h"
#include "UnrealClaudeModule.h"
#include "MCP/MCPParamValidator.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

// Niagara headers
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraComponent.h"
#include "NiagaraTypes.h"
#include "NiagaraParameterStore.h"
#include "NiagaraEditorDataBase.h"

FMCPToolInfo FMCPTool_Niagara::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("niagara");
	Info.Description = TEXT(
		"Niagara / VFX authoring surfaces.\n\n"
		"Query operations (read-only):\n"
		"- 'list_systems': Discover Niagara System assets in project\n"
		"- 'get_system_info': Introspect system (emitter count, warm-up time, fixed tick delta, user parameters overview)\n"
		"- 'get_system_parameters': List user-exposed parameters with types and current values\n"
		"- 'list_emitters': Discover standalone Niagara Emitter assets\n"
		"- 'get_emitter_info': Introspect emitter (sim target, fixed bounds, determinism, properties)\n\n"
		"Modify operations:\n"
		"- 'set_system_parameter': Set a user parameter value on a Niagara System (by parameter name)\n\n"
		"NOTE: This is an asset/parameter authoring surface.\n"
		"It does NOT provide Niagara graph/module editing.\n"
		"For full VFX graph authoring, use the Niagara editor directly."
	);

	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"),
			TEXT("Operation: 'list_systems', 'get_system_info', 'get_system_parameters', "
				 "'list_emitters', 'get_emitter_info', 'set_system_parameter'"), true),
		FMCPToolParameter(TEXT("asset_path"), TEXT("string"),
			TEXT("Asset path for introspection/modification (e.g., /Game/VFX/NS_Fire)"), false),
		FMCPToolParameter(TEXT("path_filter"), TEXT("string"),
			TEXT("Path prefix filter for list operations (default: /Game/)"), false, TEXT("/Game/")),
		FMCPToolParameter(TEXT("name_filter"), TEXT("string"),
			TEXT("Name substring filter for list operations"), false),
		FMCPToolParameter(TEXT("limit"), TEXT("number"),
			TEXT("Maximum results (1-500, default: 25)"), false, TEXT("25")),
		// set_system_parameter params
		FMCPToolParameter(TEXT("parameter_name"), TEXT("string"),
			TEXT("Name of the user parameter to set"), false),
		FMCPToolParameter(TEXT("value"), TEXT("any"),
			TEXT("Value to set (number, boolean, string, or object for vectors/colors)"), false),
	};

	// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
	Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
		TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_Niagara::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("list_systems"))          return ExecuteListSystems(Params);
	if (Operation == TEXT("get_system_info"))       return ExecuteGetSystemInfo(Params);
	if (Operation == TEXT("get_system_parameters")) return ExecuteGetSystemParameters(Params);
	if (Operation == TEXT("list_emitters"))         return ExecuteListEmitters(Params);
	if (Operation == TEXT("get_emitter_info"))      return ExecuteGetEmitterInfo(Params);
	if (Operation == TEXT("set_system_parameter"))  return ExecuteSetSystemParameter(Params);

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown Niagara operation: '%s'. Valid: list_systems, get_system_info, get_system_parameters, ")
		TEXT("list_emitters, get_emitter_info, set_system_parameter"),
		*Operation));
}

// ===== Helpers =====

UObject* FMCPTool_Niagara::LoadAssetByPath(const FString& AssetPath, FString& OutError)
{
	FSoftObjectPath SoftPath(AssetPath);
	UObject* Asset = SoftPath.TryLoad();
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("Could not load asset: %s"), *AssetPath);
	}
	return Asset;
}

// ===== Query: List Systems =====

FMCPToolResult FMCPTool_Niagara::ExecuteListSystems(const TSharedRef<FJsonObject>& Params)
{
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	Limit = FMath::Clamp(Limit, 1, 500);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*PathFilter));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& AssetData : Assets)
	{
		if (Results.Num() >= Limit) break;

		FString AssetName = AssetData.AssetName.ToString();
		if (!NameFilter.IsEmpty() && !AssetName.Contains(NameFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("systems"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d Niagara Systems"), Results.Num()), Data);
}

// ===== Query: Get System Info =====

FMCPToolResult FMCPTool_Niagara::ExecuteGetSystemInfo(const TSharedRef<FJsonObject>& Params)
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

	UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a NiagaraSystem: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"), System->GetName());

	// Emitter handles
	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();
	TArray<TSharedPtr<FJsonValue>> EmitterArray;
	for (const FNiagaraEmitterHandle& Handle : EmitterHandles)
	{
		TSharedPtr<FJsonObject> EmObj = MakeShared<FJsonObject>();
		EmObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EmObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EmObj->SetStringField(TEXT("unique_name"), Handle.GetUniqueInstanceName());

		// Get versioned emitter data
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (EmitterData)
		{
			FString SimTargetStr;
			switch (EmitterData->SimTarget)
			{
			case ENiagaraSimTarget::CPUSim: SimTargetStr = TEXT("CPU"); break;
			case ENiagaraSimTarget::GPUComputeSim: SimTargetStr = TEXT("GPU"); break;
			default: SimTargetStr = TEXT("Unknown"); break;
			}
			EmObj->SetStringField(TEXT("sim_target"), SimTargetStr);
			EmObj->SetBoolField(TEXT("local_space"), EmitterData->bLocalSpace);
			EmObj->SetBoolField(TEXT("determinism"), EmitterData->bDeterminism);
		}

		EmitterArray.Add(MakeShared<FJsonValueObject>(EmObj));
	}
	Data->SetArrayField(TEXT("emitters"), EmitterArray);
	Data->SetNumberField(TEXT("emitter_count"), EmitterArray.Num());

	// System settings
	Data->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	Data->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
	Data->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
	Data->SetBoolField(TEXT("fixed_bounds_valid"), System->GetFixedBounds().IsValid != 0);

	if (System->GetFixedBounds().IsValid != 0)
	{
		const FBox& Bounds = System->GetFixedBounds();
		TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
		BoundsObj->SetStringField(TEXT("min"), Bounds.Min.ToString());
		BoundsObj->SetStringField(TEXT("max"), Bounds.Max.ToString());
		Data->SetObjectField(TEXT("fixed_bounds"), BoundsObj);
	}

	// User parameter count
	const FNiagaraUserRedirectionParameterStore& UserParams = System->GetExposedParameters();
	Data->SetNumberField(TEXT("user_parameter_count"), UserParams.ReadParameterVariables().Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Niagara System %s: %d emitters, %d user params"),
			*System->GetName(), EmitterArray.Num(), UserParams.ReadParameterVariables().Num()),
		Data);
}

// ===== Query: Get System Parameters =====

FMCPToolResult FMCPTool_Niagara::ExecuteGetSystemParameters(const TSharedRef<FJsonObject>& Params)
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

	UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a NiagaraSystem: %s"), *AssetPath));
	}

	const FNiagaraUserRedirectionParameterStore& UserParams = System->GetExposedParameters();
	TArrayView<const FNiagaraVariableWithOffset> Variables = UserParams.ReadParameterVariables();

	TArray<TSharedPtr<FJsonValue>> ParamArray;
	for (const FNiagaraVariableWithOffset& VarWithOffset : Variables)
	{
		const FNiagaraVariable& Var = VarWithOffset;
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Var.GetName().ToString());
		ParamObj->SetStringField(TEXT("type"), Var.GetType().GetName());

		// Try to read value based on type
		const FNiagaraTypeDefinition& TypeDef = Var.GetType();
		if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
		{
			float Val = UserParams.GetParameterValue<float>(Var);
			ParamObj->SetNumberField(TEXT("value"), Val);
		}
		else if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
		{
			int32 Val = UserParams.GetParameterValue<int32>(Var);
			ParamObj->SetNumberField(TEXT("value"), Val);
		}
		else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
		{
			// Niagara stores bools as FNiagaraBool (int32)
			int32 Val = UserParams.GetParameterValue<int32>(Var);
			ParamObj->SetBoolField(TEXT("value"), Val != 0);
		}
		else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
		{
			FVector3f Val = UserParams.GetParameterValue<FVector3f>(Var);
			TSharedPtr<FJsonObject> VecObj = MakeShared<FJsonObject>();
			VecObj->SetNumberField(TEXT("x"), Val.X);
			VecObj->SetNumberField(TEXT("y"), Val.Y);
			VecObj->SetNumberField(TEXT("z"), Val.Z);
			ParamObj->SetObjectField(TEXT("value"), VecObj);
		}
		else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
		{
			FLinearColor Val = UserParams.GetParameterValue<FLinearColor>(Var);
			TSharedPtr<FJsonObject> ColObj = MakeShared<FJsonObject>();
			ColObj->SetNumberField(TEXT("r"), Val.R);
			ColObj->SetNumberField(TEXT("g"), Val.G);
			ColObj->SetNumberField(TEXT("b"), Val.B);
			ColObj->SetNumberField(TEXT("a"), Val.A);
			ParamObj->SetObjectField(TEXT("value"), ColObj);
		}
		else
		{
			ParamObj->SetStringField(TEXT("value"), TEXT("(complex type — use reflection or Niagara editor)"));
		}

		ParamArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("parameters"), ParamArray);
	Data->SetNumberField(TEXT("count"), ParamArray.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Niagara System %s: %d user parameters"), *System->GetName(), ParamArray.Num()), Data);
}

// ===== Query: List Emitters =====

FMCPToolResult FMCPTool_Niagara::ExecuteListEmitters(const TSharedRef<FJsonObject>& Params)
{
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);
	Limit = FMath::Clamp(Limit, 1, 500);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraEmitter::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*PathFilter));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& AssetData : Assets)
	{
		if (Results.Num() >= Limit) break;

		FString AssetName = AssetData.AssetName.ToString();
		if (!NameFilter.IsEmpty() && !AssetName.Contains(NameFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("emitters"), Results);
	Data->SetNumberField(TEXT("count"), Results.Num());
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d Niagara Emitters"), Results.Num()), Data);
}

// ===== Query: Get Emitter Info =====

FMCPToolResult FMCPTool_Niagara::ExecuteGetEmitterInfo(const TSharedRef<FJsonObject>& Params)
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

	UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset);
	if (!Emitter)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a NiagaraEmitter: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("name"), Emitter->GetName());
	Data->SetStringField(TEXT("unique_name"), Emitter->GetUniqueEmitterName());

	// Get latest emitter data
	FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
	if (EmitterData)
	{
		FString SimTargetStr;
		switch (EmitterData->SimTarget)
		{
		case ENiagaraSimTarget::CPUSim: SimTargetStr = TEXT("CPU"); break;
		case ENiagaraSimTarget::GPUComputeSim: SimTargetStr = TEXT("GPU"); break;
		default: SimTargetStr = TEXT("Unknown"); break;
		}
		Data->SetStringField(TEXT("sim_target"), SimTargetStr);
		Data->SetBoolField(TEXT("local_space"), EmitterData->bLocalSpace);
		Data->SetBoolField(TEXT("determinism"), EmitterData->bDeterminism);
		Data->SetBoolField(TEXT("requires_persistent_ids"), EmitterData->bRequiresPersistentIDs);

		// Fixed bounds
		Data->SetBoolField(TEXT("fixed_bounds_valid"), EmitterData->FixedBounds.IsValid != 0);
		if (EmitterData->FixedBounds.IsValid != 0)
		{
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			BoundsObj->SetStringField(TEXT("min"), EmitterData->FixedBounds.Min.ToString());
			BoundsObj->SetStringField(TEXT("max"), EmitterData->FixedBounds.Max.ToString());
			Data->SetObjectField(TEXT("fixed_bounds"), BoundsObj);
		}
	}
	else
	{
		Data->SetStringField(TEXT("emitter_data"), TEXT("(no versioned data available)"));
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Emitter info for %s"), *Emitter->GetName()), Data);
}

// ===== Modify: Set System Parameter =====

FMCPToolResult FMCPTool_Niagara::ExecuteSetSystemParameter(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}

	FString ParamName;
	if (!ExtractRequiredString(Params, TEXT("parameter_name"), ParamName, Error))
	{
		return Error.GetValue();
	}

	FString LoadError;
	UObject* Asset = LoadAssetByPath(AssetPath, LoadError);
	if (!Asset)
	{
		return FMCPToolResult::Error(LoadError);
	}

	UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Asset is not a NiagaraSystem: %s"), *AssetPath));
	}

	FNiagaraUserRedirectionParameterStore& UserParams = System->GetExposedParameters();
	TArrayView<const FNiagaraVariableWithOffset> Variables = UserParams.ReadParameterVariables();

	// Find the parameter
	const FNiagaraVariableWithOffset* FoundVar = nullptr;
	for (const FNiagaraVariableWithOffset& VarWithOffset : Variables)
	{
		if (VarWithOffset.GetName().ToString() == ParamName ||
			VarWithOffset.GetName().ToString().EndsWith(ParamName))
		{
			FoundVar = &VarWithOffset;
			break;
		}
	}

	if (!FoundVar)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Parameter '%s' not found on NiagaraSystem %s. Use get_system_parameters to discover available parameters."),
			*ParamName, *AssetPath));
	}

	// Build an FNiagaraVariable from the found FNiagaraVariableWithOffset
	FNiagaraVariable NiagaraVar(*FoundVar);

	const FNiagaraTypeDefinition& TypeDef = FoundVar->GetType();
	bool bSet = false;
	FString SetValueStr;

	if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
	{
		double Val = 0.0;
		if (Params->TryGetNumberField(TEXT("value"), Val))
		{
			UserParams.SetParameterValue(static_cast<float>(Val), NiagaraVar);
			bSet = true;
			SetValueStr = FString::SanitizeFloat(Val);
		}
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
	{
		double Val = 0.0;
		if (Params->TryGetNumberField(TEXT("value"), Val))
		{
			UserParams.SetParameterValue(static_cast<int32>(Val), NiagaraVar);
			bSet = true;
			SetValueStr = FString::FromInt(static_cast<int32>(Val));
		}
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
	{
		bool Val = false;
		if (Params->TryGetBoolField(TEXT("value"), Val))
		{
			UserParams.SetParameterValue(Val ? 1 : 0, NiagaraVar);
			bSet = true;
			SetValueStr = Val ? TEXT("true") : TEXT("false");
		}
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
	{
		const TSharedPtr<FJsonObject>* VecObj = nullptr;
		if (Params->TryGetObjectField(TEXT("value"), VecObj) && VecObj && (*VecObj).IsValid())
		{
			FVector3f Val;
			Val.X = static_cast<float>((*VecObj)->GetNumberField(TEXT("x")));
			Val.Y = static_cast<float>((*VecObj)->GetNumberField(TEXT("y")));
			Val.Z = static_cast<float>((*VecObj)->GetNumberField(TEXT("z")));
			UserParams.SetParameterValue(Val, NiagaraVar);
			bSet = true;
			SetValueStr = FString::Printf(TEXT("(%f, %f, %f)"), Val.X, Val.Y, Val.Z);
		}
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
	{
		const TSharedPtr<FJsonObject>* ColObj = nullptr;
		if (Params->TryGetObjectField(TEXT("value"), ColObj) && ColObj && (*ColObj).IsValid())
		{
			FLinearColor Val;
			Val.R = static_cast<float>((*ColObj)->GetNumberField(TEXT("r")));
			Val.G = static_cast<float>((*ColObj)->GetNumberField(TEXT("g")));
			Val.B = static_cast<float>((*ColObj)->GetNumberField(TEXT("b")));
			Val.A = (*ColObj)->HasField(TEXT("a")) ? static_cast<float>((*ColObj)->GetNumberField(TEXT("a"))) : 1.0f;
			UserParams.SetParameterValue(Val, NiagaraVar);
			bSet = true;
			SetValueStr = Val.ToString();
		}
	}

	if (!bSet)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Could not set parameter '%s' (type: %s). Provide a compatible value."),
			*ParamName, *TypeDef.GetName()));
	}

	System->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("parameter_name"), FoundVar->GetName().ToString());
	Data->SetStringField(TEXT("parameter_type"), TypeDef.GetName());
	Data->SetStringField(TEXT("value_set"), SetValueStr);
	Data->SetStringField(TEXT("status"), TEXT("changed"));

	// Emit receipt
	{
		FExecutionReceipt Receipt;
		Receipt.Tool = TEXT("niagara");
		Receipt.Summary = TEXT("set_system_parameter");
		Receipt.bSuccess = true;
		Receipt.Targets.Add(AssetPath);
		Receipt.Classification = TEXT("user_mutation");
		FUnrealClaudeExecutionLog::Get().AddReceipt(Receipt);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set %s = %s on %s"), *ParamName, *SetValueStr, *AssetPath), Data);
}
