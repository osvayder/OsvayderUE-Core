// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

enum class EUnrealClaudeVoiceDictationState : uint8
{
	Idle,
	Recording,
	PreparingRuntime,
	Transcribing,
	Failed,
	Unavailable
};

struct FUnrealClaudeVoiceDictationStatus
{
	EUnrealClaudeVoiceDictationState State = EUnrealClaudeVoiceDictationState::Idle;
	FString Detail;
	FString AudioPath;
	FString TranscriptPath;
	FString Transcript;
};
