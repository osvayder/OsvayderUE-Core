// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "IAgentBackend.h"

using FOnClaudeResponse = FOnAgentResponse;
using FOnClaudeProgress = FOnAgentProgress;
using EClaudeStreamEventType = EAgentRunEventType;
using FClaudeStreamEvent = FAgentRunEvent;
using FOnClaudeStreamEvent = FOnAgentStreamEvent;
using FClaudeRequestConfig = FAgentRequestConfig;

class UNREALCLAUDE_API IClaudeRunner : public IAgentBackend
{
};
