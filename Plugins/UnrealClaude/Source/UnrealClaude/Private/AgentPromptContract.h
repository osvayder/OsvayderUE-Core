// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentBackendTypes.h"

class FAgentPromptContractBuilder
{
public:
	static FAgentPromptContract Build(
		bool bIncludeEngineContext,
		bool bIncludeProjectContext,
		const FString& ProjectContextPrompt,
		const FString& CustomSystemPrompt);

	static bool AppendRoleContractContext(
		FAgentPromptContract& Contract,
		const FString& RoleId,
		const FString& RecipeId,
		int32 EvidenceSchemaVersion,
		FString* OutBlockerDetail = nullptr);
};

class FAgentPromptMaterializer
{
public:
	static FString MaterializeCanonicalText(const FAgentPromptContract& Contract);
	static FString MaterializeClaudeSystemPrompt(const FAgentPromptContract& Contract);
	/**
	 * Spec 621 §3 overload: if LanguageDisplayName is non-empty, appends one directive line so Claude replies in that language.
	 * Empty LanguageDisplayName reproduces MaterializeClaudeSystemPrompt(Contract) exactly.
	 */
	static FString MaterializeClaudeSystemPrompt(const FAgentPromptContract& Contract, const FString& LanguageDisplayName);
	static FString MaterializeCodexPayload(const FAgentPromptContract& Contract, const FString& UserPrompt);
};
