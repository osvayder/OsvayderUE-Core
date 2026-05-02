// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "AgentBackendTypes.h"

class UNREALCLAUDE_API IAgentBackend
{
public:
	virtual ~IAgentBackend() = default;

	virtual bool ExecuteAsync(
		const FAgentRequestConfig& Config,
		FOnAgentResponse OnComplete,
		FOnAgentProgress OnProgress = FOnAgentProgress()
	) = 0;

	virtual bool ExecuteSync(const FAgentRequestConfig& Config, FString& OutResponse) = 0;
	virtual void Cancel() = 0;
	virtual bool IsExecuting() const = 0;
	virtual bool IsAvailable() const = 0;
	virtual EUnrealClaudeProviderBackend GetBackendType() const = 0;
	virtual FString GetBackendDisplayName() const = 0;
	virtual FAgentBackendCapabilities GetCapabilities() const = 0;
	virtual FAgentBackendStatus GetStatus() const = 0;
	virtual void ResetConversation() {}
};
