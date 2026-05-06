// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

enum class EOsvayderUEVoiceDictationState : uint8
{
	Idle,
	Recording,
	PreparingRuntime,
	Transcribing,
	Failed,
	Unavailable
};

struct FOsvayderUEVoiceDictationStatus
{
	EOsvayderUEVoiceDictationState State = EOsvayderUEVoiceDictationState::Idle;
	FString Detail;
	FString AudioPath;
	FString TranscriptPath;
	FString Transcript;
};
