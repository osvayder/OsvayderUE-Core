// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "IAgentBackend.h"

using FOnOsvayderResponse = FOnAgentResponse;
using FOnOsvayderProgress = FOnAgentProgress;
using EOsvayderStreamEventType = EAgentRunEventType;
using FOsvayderStreamEvent = FAgentRunEvent;
using FOnOsvayderStreamEvent = FOnAgentStreamEvent;
using FOsvayderRequestConfig = FAgentRequestConfig;

class OSVAYDERUE_API IOsvayderRunner : public IAgentBackend
{
};
