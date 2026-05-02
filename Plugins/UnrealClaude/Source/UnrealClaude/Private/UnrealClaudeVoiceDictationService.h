// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "HAL/ThreadSafeBool.h"
#include "AudioCaptureCore.h"
#include "UnrealClaudeVoiceDictationTypes.h"

class FUnrealClaudeVoiceDictationService : public TSharedFromThis<FUnrealClaudeVoiceDictationService>
{
public:
	FUnrealClaudeVoiceDictationService();
	~FUnrealClaudeVoiceDictationService();

	void SetStatusChangedHandler(TFunction<void(const FUnrealClaudeVoiceDictationStatus&)> InHandler);
	void SetTranscriptReadyHandler(TFunction<void(const FString&)> InHandler);

	const FUnrealClaudeVoiceDictationStatus& GetStatus() const;

	void Tick();
	bool StartDictation();
	void StopDictation();
	void Shutdown();

private:
	struct FRuntimePaths
	{
		FString VoiceRoot;
		FString RuntimeRoot;
		FString SitePackagesDir;
		FString ModelDir;
		FString ModelName;
		FString ModelUrl;
		FString LanguageTag;
		FString LanguageDisplayName;
		FString PrepareScriptPath;
		FString TranscribeScriptPath;
	};

	void DrainCapturedAudio();
	void ResetCaptureState();
	void EmitStatus(
		EUnrealClaudeVoiceDictationState NewState,
		const FString& Detail,
		const FString& AudioPath = FString(),
		const FString& TranscriptPath = FString(),
		const FString& Transcript = FString());
	bool FinalizeRecordingToWav(FString& OutAudioPath, FString& OutError);
	void BeginTranscription(const FString& AudioPath);
	FRuntimePaths GetRuntimePaths() const;
	bool ResolvePythonExecutable(FString& OutPythonPath, FString& OutError) const;
	bool EnsureRuntimeReady(const FString& PythonPath, const FRuntimePaths& Paths, FString& OutError) const;
	bool RunPythonHelper(
		const FString& PythonPath,
		const FString& ScriptPath,
		const TArray<FString>& Args,
		FString& OutStdOut,
		FString& OutStdErr,
		int32& OutExitCode) const;
	FString GetDebugFixturePath() const;
	FString BuildActiveDictationLabel() const;
	FString BuildActiveDictationRecordingDetail() const;
	FString BuildActiveDictationPreparationDetail() const;
	FString BuildActiveDictationTranscribingDetail() const;
	FString BuildActiveDictationInsertedDetail() const;
	FString AppendActiveDictationContext(const FString& Detail, const FRuntimePaths& Paths) const;

private:
	FThreadSafeBool bShutdown = false;
	FUnrealClaudeVoiceDictationStatus Status;
	TFunction<void(const FUnrealClaudeVoiceDictationStatus&)> OnStatusChanged;
	TFunction<void(const FString&)> OnTranscriptReady;
	TUniquePtr<Audio::FAudioCaptureSynth> CaptureSynth;
	Audio::FCaptureDeviceInfo CaptureDeviceInfo;
	TArray<float> CapturedInterleavedAudio;
	int32 CaptureSampleRate = 0;
	int32 CaptureNumChannels = 1;
};
