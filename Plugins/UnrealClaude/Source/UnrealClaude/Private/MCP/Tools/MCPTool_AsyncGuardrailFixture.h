// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Dev-only async guardrail fixture.
 *
 * This gives packet-level tests a deterministic tool that can either complete
 * without side effects or create one sandbox package. It is intentionally
 * registered only in dev automation builds.
 */
class FMCPTool_AsyncGuardrailFixture : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("async_guardrail_fixture");
		Info.Description = TEXT("Dev-only fixture for async cancellation, timeout, and late-mutation guardrail tests.");
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("Fixture operation: sleep_readonly or mutate_marker."), true),
			FMCPToolParameter(TEXT("delay_ms"), TEXT("number"),
				TEXT("Optional short delay inside the tool body. Keep small; it runs on the game thread."), false, TEXT("0")),
			FMCPToolParameter(TEXT("pre_execute_delay_ms"), TEXT("number"),
				TEXT("Dev-only async queue delay consumed before tool execution; used to test cancellation without blocking the game thread."), false, TEXT("0")),
			FMCPToolParameter(TEXT("package_path"), TEXT("string"),
				TEXT("Required for mutate_marker. Must be under /Game/__UnrealClaudeTestSandbox/."), false),
			FMCPToolParameter(TEXT("asset_name"), TEXT("string"),
				TEXT("Optional object name for mutate_marker; defaults to the package leaf."), false),
			FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
				TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false)
		};
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override
	{
		FString Operation;
		TOptional<FMCPToolResult> Error;
		if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
		{
			return Error.GetValue();
		}

		const int32 DelayMs = FMath::Max(0, ExtractOptionalNumber<int32>(Params, TEXT("delay_ms"), 0));
		if (DelayMs > 0)
		{
			FPlatformProcess::Sleep(static_cast<float>(DelayMs) / 1000.0f);
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("operation"), Operation);
		Data->SetNumberField(TEXT("delay_ms"), DelayMs);
		Data->SetNumberField(TEXT("pre_execute_delay_ms"), ExtractOptionalNumber<int32>(Params, TEXT("pre_execute_delay_ms"), 0));

		if (Operation == TEXT("sleep_readonly"))
		{
			Data->SetBoolField(TEXT("mutated"), false);
			return FMCPToolResult::Success(TEXT("Async guardrail fixture completed without mutation"), Data);
		}

		if (Operation != TEXT("mutate_marker"))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
		}

		FString PackagePath;
		if (!ExtractRequiredString(Params, TEXT("package_path"), PackagePath, Error))
		{
			return Error.GetValue();
		}

		if (!PackagePath.StartsWith(TEXT("/Game/__UnrealClaudeTestSandbox/"))
			|| PackagePath.Contains(TEXT(".."))
			|| PackagePath.Contains(TEXT("\\"))
			|| PackagePath.Contains(TEXT(".")))
		{
			return FMCPToolResult::Error(TEXT("Invalid package path: async guardrail fixture only writes under /Game/__UnrealClaudeTestSandbox/"));
		}

		FText PackageReason;
		if (!FPackageName::IsValidLongPackageName(PackagePath, false, &PackageReason))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Invalid package path: %s"), *PackageReason.ToString()));
		}

		FString AssetName = ExtractOptionalString(Params, TEXT("asset_name"), FPackageName::GetLongPackageAssetName(PackagePath));
		if (AssetName.IsEmpty()
			|| AssetName.Contains(TEXT("/"))
			|| AssetName.Contains(TEXT("\\"))
			|| AssetName.Contains(TEXT(".")))
		{
			return FMCPToolResult::Error(TEXT("Invalid asset name for async guardrail fixture marker"));
		}

		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *PackagePath));
		}

		if (FindObject<UObject>(Package, *AssetName))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Marker already exists: %s.%s"), *PackagePath, *AssetName));
		}

		UObject* Marker = NewObject<UObject>(Package, UObject::StaticClass(), FName(*AssetName), RF_Public | RF_Standalone);
		if (!Marker)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create marker object: %s.%s"), *PackagePath, *AssetName));
		}

		FAssetRegistryModule::AssetCreated(Marker);
		Package->MarkPackageDirty();

		Data->SetBoolField(TEXT("mutated"), true);
		Data->SetStringField(TEXT("marker_package"), PackagePath);
		Data->SetStringField(TEXT("marker_object_path"), FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName));
		return FMCPToolResult::Success(TEXT("Async guardrail fixture marker created"), Data);
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS
