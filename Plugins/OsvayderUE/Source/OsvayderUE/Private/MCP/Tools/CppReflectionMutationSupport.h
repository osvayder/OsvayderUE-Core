// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FCppReflectionPropertyMetadataMutationResult
{
	bool bSuccess = false;
	bool bApplied = false;
	bool bWouldChangeFile = false;
	bool bScopeAllowed = false;
	bool bHadMetaBlock = false;
	bool bHadExistingKey = false;
	FString ErrorMessage;
	FString NormalizedMetadataKey;
	FString MetadataValue;
	FString ExistingMetadataValue;
	FString ChangeKind;
	FString HeaderPath;
	FString ScopeMatchedRoot;
	FString ScopeDenialReason;
	int32 MacroStartLine = 0;
	int32 MacroEndLine = 0;
	int32 DeclarationLine = 0;
	FString DeclarationText;
	FString MacroBefore;
	FString MacroAfter;
	FString SourceHashBefore;
	FString SourceHashAfter;

	TSharedPtr<FJsonObject> ToJson() const;
};

struct FCppReflectionPropertyDeclarationPreviewResult
{
	bool bSuccess = false;
	bool bScopeAllowed = false;
	bool bWouldChangeFile = false;
	FString ErrorMessage;
	FString SchemaVersion;
	FString DeclarationKind;
	FString PreviewKind;
	FString HeaderPath;
	FString ScopeMatchedRoot;
	FString ScopeDenialReason;
	FString AnchorPropertyName;
	int32 AnchorMacroStartLine = 0;
	int32 AnchorDeclarationLine = 0;
	FString AnchorDeclarationText;
	int32 InsertionLine = 0;
	FString PropertyName;
	FString PropertyCppType;
	FString Category;
	FString DefaultValueLiteral;
	FString GeneratedMacroLine;
	FString GeneratedDeclarationLine;
	FString GeneratedSnippet;
	FString PreviewExcerptAfter;
	FString SourceHashBefore;
	FString SourceHashAfterPreview;

	TSharedPtr<FJsonObject> ToJson() const;
};

struct FCppReflectionPropertyDeclarationApplyResult
{
	bool bSuccess = false;
	bool bApplied = false;
	bool bScopeAllowed = false;
	bool bWouldChangeFile = false;
	bool bRestoreReady = false;
	FString ErrorMessage;
	FString SchemaVersion;
	FString PreviewSchemaVersion;
	FString DeclarationKind;
	FString ApplyKind;
	FString HeaderPath;
	FString ScopeMatchedRoot;
	FString ScopeDenialReason;
	FString AnchorPropertyName;
	int32 AnchorMacroStartLine = 0;
	int32 AnchorDeclarationLine = 0;
	FString AnchorDeclarationText;
	int32 InsertionLine = 0;
	FString PropertyName;
	FString PropertyCppType;
	FString Category;
	FString DefaultValueLiteral;
	FString GeneratedMacroLine;
	FString GeneratedDeclarationLine;
	FString GeneratedSnippet;
	FString ExpectedSourceHashBefore;
	FString SourceHashBeforeApply;
	FString SourceHashAfterPreview;
	FString SourceHashAfterApply;
	FString CheckpointPath;

	TSharedPtr<FJsonObject> ToJson() const;
};

struct FCppReflectionPropertyDeclarationRevertResult
{
	bool bSuccess = false;
	bool bReverted = false;
	bool bScopeAllowed = false;
	FString ErrorMessage;
	FString SchemaVersion;
	FString DeclarationKind;
	FString RevertKind;
	FString HeaderPath;
	FString ScopeMatchedRoot;
	FString ScopeDenialReason;
	FString PropertyName;
	FString CheckpointPath;
	FString ExpectedAppliedSourceHash;
	FString ExpectedRestoredSourceHash;
	FString SourceHashBeforeRevert;
	FString SourceHashAfterRevert;

	TSharedPtr<FJsonObject> ToJson() const;
};

bool TryNormalizeAllowedPropertyMetadataKey(const FString& RequestedKey, FString& OutCanonicalKey, FString& OutError);

FCppReflectionPropertyDeclarationPreviewResult PreviewPluginReflectedPropertyDeclaration(
	const FString& HeaderPath,
	const FString& AnchorPropertyName,
	const FString& NewPropertyName,
	const FString& PropertyCppType,
	const FString& Category,
	const FString& DefaultValueLiteral);

FCppReflectionPropertyDeclarationApplyResult ApplyPluginReflectedPropertyDeclaration(
	const FString& HeaderPath,
	const FString& AnchorPropertyName,
	const FString& NewPropertyName,
	const FString& PropertyCppType,
	const FString& Category,
	const FString& DefaultValueLiteral,
	const FString& ExpectedSourceHashBefore,
	const FString& CheckpointPath);

FCppReflectionPropertyDeclarationRevertResult RevertPluginReflectedPropertyDeclaration(
	const FString& HeaderPath,
	const FString& PropertyName,
	const FString& CheckpointPath,
	const FString& ExpectedAppliedSourceHash,
	const FString& ExpectedRestoredSourceHash);

FCppReflectionPropertyMetadataMutationResult PreviewPluginPropertyMetadataMutation(
	const FString& HeaderPath,
	const FString& PropertyName,
	const FString& MetadataKey,
	const FString& MetadataValue);

FCppReflectionPropertyMetadataMutationResult ApplyPluginPropertyMetadataMutation(
	const FString& HeaderPath,
	const FString& PropertyName,
	const FString& MetadataKey,
	const FString& MetadataValue);
