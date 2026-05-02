// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeVoiceDictationService.h"

#include "UnrealClaudeConstants.h"
#include "UnrealClaudeModule.h"
#include "UnrealClaudeSettings.h"

#include "Async/Async.h"
#include "Audio.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString QuoteArg(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	FString JoinArgs(const TArray<FString>& Args)
	{
		return FString::Join(Args, TEXT(" "));
	}

	FString GetPluginBaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealClaude"));
		if (Plugin.IsValid())
		{
			return Plugin->GetBaseDir();
		}

		return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UnrealClaude"));
	}

	FString ResolveFirstLine(const FString& MultiLineValue)
	{
		TArray<FString> Lines;
		MultiLineValue.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			if (!Trimmed.IsEmpty())
			{
				return Trimmed;
			}
		}

		return FString();
	}

	FString BuildCompactFailureDetail(const TCHAR* Phase, const FString& Reason, const FString& Fallback)
	{
		const FString TrimmedReason = Reason.TrimStartAndEnd();
		if (TrimmedReason.IsEmpty())
		{
			return Fallback;
		}

		return TrimmedReason.EndsWith(TEXT("."))
			? FString::Printf(TEXT("%s failed: %s"), Phase, *TrimmedReason)
			: FString::Printf(TEXT("%s failed: %s."), Phase, *TrimmedReason);
	}

	FString ExtractCompactProcessReason(const FString& RawText)
	{
		const FString Trimmed = RawText.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return FString();
		}

		TSharedPtr<FJsonObject> ResultObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (FJsonSerializer::Deserialize(Reader, ResultObject) && ResultObject.IsValid())
		{
			static const TCHAR* CandidateFields[] = {
				TEXT("error"),
				TEXT("detail"),
				TEXT("message"),
				TEXT("stderr"),
				TEXT("stdout")
			};

			for (const TCHAR* FieldName : CandidateFields)
			{
				FString FieldValue;
				if (ResultObject->TryGetStringField(FieldName, FieldValue))
				{
					const FString FirstLine = ResolveFirstLine(FieldValue);
					if (!FirstLine.IsEmpty())
					{
						return FirstLine;
					}
				}
			}
		}

		return ResolveFirstLine(Trimmed);
	}

	bool TryDeserializeJsonObject(const FString& RawText, TSharedPtr<FJsonObject>& OutResultObject)
	{
		OutResultObject.Reset();

		const FString Trimmed = RawText.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		return FJsonSerializer::Deserialize(Reader, OutResultObject) && OutResultObject.IsValid();
	}

	bool TryLoadJsonObjectFromFile(const FString& JsonPath, TSharedPtr<FJsonObject>& OutResultObject)
	{
		OutResultObject.Reset();
		if (!IFileManager::Get().FileExists(*JsonPath))
		{
			return false;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *JsonPath))
		{
			return false;
		}

		return TryDeserializeJsonObject(JsonText, OutResultObject);
	}

	bool TryLoadTranscriptionResultObject(
		const FString& TranscriptJsonPath,
		const FString& StdOut,
		TSharedPtr<FJsonObject>& OutResultObject,
		FString& OutResultSource)
	{
		if (TryLoadJsonObjectFromFile(TranscriptJsonPath, OutResultObject))
		{
			OutResultSource = TEXT("result_json");
			return true;
		}

		if (TryDeserializeJsonObject(StdOut, OutResultObject))
		{
			OutResultSource = TEXT("stdout");
			return true;
		}

		OutResultSource.Reset();
		return false;
	}

	FString BuildCompactProcessFailureDetail(const TCHAR* Phase, const int32 ExitCode, const FString& StdOut, const FString& StdErr)
	{
		FString Reason = ExtractCompactProcessReason(StdErr);
		if (Reason.IsEmpty())
		{
			Reason = ExtractCompactProcessReason(StdOut);
		}

		return BuildCompactFailureDetail(
			Phase,
			Reason,
			FString::Printf(TEXT("%s failed with exit code %d. See Output Log for details."), Phase, ExitCode));
	}

	FString BuildCompactNonJsonDetail(const TCHAR* Phase, const FString& StdOut)
	{
		return BuildCompactFailureDetail(
			Phase,
			ExtractCompactProcessReason(StdOut),
			FString::Printf(TEXT("%s returned unreadable output. See Output Log for details."), Phase));
	}

	void LogDictationProcessFailure(const TCHAR* EventName, const int32 ExitCode, const FString& StdOut, const FString& StdErr)
	{
		UE_LOG(
			LogUnrealClaude,
			Warning,
			TEXT("%s exit=%d stdout=\"%s\" stderr=\"%s\""),
			EventName,
			ExitCode,
			*StdOut.TrimStartAndEnd(),
			*StdErr.TrimStartAndEnd());
	}

	FString FindPythonFromPathProbe()
	{
#if PLATFORM_WINDOWS
		const TCHAR* ResolverExe = TEXT("where");
		const TCHAR* ResolverArgs = TEXT("python");
#else
		const TCHAR* ResolverExe = TEXT("/bin/sh");
		const TCHAR* ResolverArgs = TEXT("-c 'which python3 2>/dev/null || which python 2>/dev/null'");
#endif

		FString StdOut;
		FString StdErr;
		int32 ExitCode = INDEX_NONE;
		if (FPlatformProcess::ExecProcess(ResolverExe, ResolverArgs, &ExitCode, &StdOut, &StdErr) && ExitCode == 0)
		{
			return ResolveFirstLine(StdOut);
		}

		return FString();
	}

	FString ResolvePythonExecutableCandidate()
	{
		const FString ExplicitOverride = FPlatformMisc::GetEnvironmentVariable(UnrealClaudeConstants::VoiceDictation::PythonEnvOverrideVar);
		if (!ExplicitOverride.IsEmpty() && IFileManager::Get().FileExists(*ExplicitOverride))
		{
			return ExplicitOverride;
		}

		const FString PathProbe = FindPythonFromPathProbe();
		if (!PathProbe.IsEmpty() && IFileManager::Get().FileExists(*PathProbe))
		{
			return PathProbe;
		}

#if PLATFORM_WINDOWS
		const TArray<FString> KnownCandidates = {
			TEXT("C:/Python313/python.exe"),
			TEXT("C:/Python312/python.exe"),
			TEXT("C:/Python311/python.exe"),
			TEXT("C:/Python310/python.exe")
		};

		for (const FString& Candidate : KnownCandidates)
		{
			if (IFileManager::Get().FileExists(*Candidate))
			{
				return Candidate;
			}
		}
#endif

		return FString();
	}

	FString DictationStateToString(const EUnrealClaudeVoiceDictationState State)
	{
		switch (State)
		{
		case EUnrealClaudeVoiceDictationState::Recording:
			return TEXT("recording");

		case EUnrealClaudeVoiceDictationState::PreparingRuntime:
			return TEXT("preparing_runtime");

		case EUnrealClaudeVoiceDictationState::Transcribing:
			return TEXT("transcribing");

		case EUnrealClaudeVoiceDictationState::Failed:
			return TEXT("failed");

		case EUnrealClaudeVoiceDictationState::Unavailable:
			return TEXT("unavailable");

		case EUnrealClaudeVoiceDictationState::Idle:
		default:
			return TEXT("idle");
		}
	}

	void DownmixToMono(const TArray<float>& InterleavedAudio, const int32 NumChannels, TArray<float>& OutMonoAudio)
	{
		OutMonoAudio.Reset();
		if (InterleavedAudio.Num() == 0 || NumChannels <= 0)
		{
			return;
		}

		const int32 NumFrames = InterleavedAudio.Num() / NumChannels;
		OutMonoAudio.Reserve(NumFrames);
		for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
		{
			float Sample = 0.0f;
			for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
			{
				Sample += InterleavedAudio[FrameIndex * NumChannels + ChannelIndex];
			}

			OutMonoAudio.Add(Sample / static_cast<float>(NumChannels));
		}
	}

	void ResampleMonoLinear(
		const TArray<float>& InMonoAudio,
		const int32 InSampleRate,
		const int32 OutSampleRate,
		TArray<float>& OutResampledAudio)
	{
		OutResampledAudio.Reset();
		if (InMonoAudio.Num() == 0 || InSampleRate <= 0 || OutSampleRate <= 0)
		{
			return;
		}

		if (InSampleRate == OutSampleRate || InMonoAudio.Num() == 1)
		{
			OutResampledAudio = InMonoAudio;
			return;
		}

		const double Ratio = static_cast<double>(OutSampleRate) / static_cast<double>(InSampleRate);
		const int32 TargetNumSamples = FMath::Max(1, FMath::RoundToInt(InMonoAudio.Num() * Ratio));
		OutResampledAudio.SetNumUninitialized(TargetNumSamples);

		for (int32 OutputIndex = 0; OutputIndex < TargetNumSamples; ++OutputIndex)
		{
			const double SourceIndex = static_cast<double>(OutputIndex) / Ratio;
			const int32 LeftIndex = FMath::Clamp(FMath::FloorToInt(SourceIndex), 0, InMonoAudio.Num() - 1);
			const int32 RightIndex = FMath::Min(LeftIndex + 1, InMonoAudio.Num() - 1);
			const float Alpha = static_cast<float>(SourceIndex - static_cast<double>(LeftIndex));
			OutResampledAudio[OutputIndex] = FMath::Lerp(InMonoAudio[LeftIndex], InMonoAudio[RightIndex], Alpha);
		}
	}

	void ConvertFloatToPcm16(const TArray<float>& InSamples, TArray<int16>& OutSamples)
	{
		OutSamples.Reset();
		OutSamples.Reserve(InSamples.Num());
		for (const float Sample : InSamples)
		{
			const float Clamped = FMath::Clamp(Sample, -1.0f, 1.0f);
			OutSamples.Add(static_cast<int16>(FMath::RoundToInt(Clamped * 32767.0f)));
		}
	}
}

FUnrealClaudeVoiceDictationService::FUnrealClaudeVoiceDictationService()
{
	Status.State = EUnrealClaudeVoiceDictationState::Idle;
}

FUnrealClaudeVoiceDictationService::~FUnrealClaudeVoiceDictationService()
{
	Shutdown();
}

void FUnrealClaudeVoiceDictationService::SetStatusChangedHandler(TFunction<void(const FUnrealClaudeVoiceDictationStatus&)> InHandler)
{
	OnStatusChanged = MoveTemp(InHandler);
	if (OnStatusChanged)
	{
		OnStatusChanged(Status);
	}
}

void FUnrealClaudeVoiceDictationService::SetTranscriptReadyHandler(TFunction<void(const FString&)> InHandler)
{
	OnTranscriptReady = MoveTemp(InHandler);
}

const FUnrealClaudeVoiceDictationStatus& FUnrealClaudeVoiceDictationService::GetStatus() const
{
	return Status;
}

void FUnrealClaudeVoiceDictationService::Tick()
{
	if (!bShutdown && Status.State == EUnrealClaudeVoiceDictationState::Recording)
	{
		DrainCapturedAudio();
	}
}

bool FUnrealClaudeVoiceDictationService::StartDictation()
{
	if (bShutdown)
	{
		return false;
	}

	if (Status.State == EUnrealClaudeVoiceDictationState::PreparingRuntime ||
		Status.State == EUnrealClaudeVoiceDictationState::Transcribing ||
		Status.State == EUnrealClaudeVoiceDictationState::Recording)
	{
		return false;
	}

	ResetCaptureState();

	const FString DebugFixturePath = GetDebugFixturePath();
	if (!DebugFixturePath.IsEmpty())
	{
		if (!IFileManager::Get().FileExists(*DebugFixturePath))
		{
			EmitStatus(
				EUnrealClaudeVoiceDictationState::Failed,
				FString::Printf(TEXT("Dictation debug fixture is missing: %s"), *DebugFixturePath));
			return false;
		}

		UE_LOG(LogUnrealClaude, Log, TEXT("VOICE_DICTATION_DEBUG_FIXTURE_ARMED audio=\"%s\""), *DebugFixturePath);
		EmitStatus(
			EUnrealClaudeVoiceDictationState::Recording,
			FString::Printf(TEXT("Debug fixture armed. Click Stop to transcribe %s."), *FPaths::GetCleanFilename(DebugFixturePath)));
		return true;
	}

	CaptureSynth = MakeUnique<Audio::FAudioCaptureSynth>();

	if (!CaptureSynth->GetDefaultCaptureDeviceInfo(CaptureDeviceInfo))
	{
		EmitStatus(EUnrealClaudeVoiceDictationState::Unavailable, TEXT("Dictation unavailable: no default microphone capture device."));
		CaptureSynth.Reset();
		return false;
	}

	CaptureSampleRate = FMath::Max(1, CaptureDeviceInfo.PreferredSampleRate);
	CaptureNumChannels = FMath::Max(1, CaptureDeviceInfo.InputChannels);

	if (!CaptureSynth->OpenDefaultStream())
	{
		EmitStatus(EUnrealClaudeVoiceDictationState::Unavailable, TEXT("Dictation unavailable: failed to open the default microphone stream."));
		CaptureSynth.Reset();
		return false;
	}

	if (!CaptureSynth->StartCapturing())
	{
		EmitStatus(EUnrealClaudeVoiceDictationState::Unavailable, TEXT("Dictation unavailable: failed to start microphone capture."));
		CaptureSynth->AbortCapturing();
		CaptureSynth.Reset();
		return false;
	}

	UE_LOG(
		LogUnrealClaude,
		Log,
		TEXT("VOICE_DICTATION_RECORDING_STARTED device=\"%s\" sample_rate=%d channels=%d"),
		*CaptureDeviceInfo.DeviceName,
		CaptureSampleRate,
		CaptureNumChannels);

	EmitStatus(EUnrealClaudeVoiceDictationState::Recording, BuildActiveDictationRecordingDetail());
	return true;
}

void FUnrealClaudeVoiceDictationService::StopDictation()
{
	if (Status.State != EUnrealClaudeVoiceDictationState::Recording)
	{
		return;
	}

	DrainCapturedAudio();

	if (CaptureSynth.IsValid())
	{
		CaptureSynth->StopCapturing();
		CaptureSynth->AbortCapturing();
		CaptureSynth.Reset();
	}

	const FString DebugFixturePath = GetDebugFixturePath();
	if (!DebugFixturePath.IsEmpty())
	{
		if (!IFileManager::Get().FileExists(*DebugFixturePath))
		{
			EmitStatus(
				EUnrealClaudeVoiceDictationState::Failed,
				FString::Printf(TEXT("Dictation debug fixture is missing: %s"), *DebugFixturePath));
			return;
		}

		UE_LOG(LogUnrealClaude, Log, TEXT("VOICE_DICTATION_DEBUG_FIXTURE audio=\"%s\""), *DebugFixturePath);
		BeginTranscription(DebugFixturePath);
		return;
	}

	FString AudioPath;
	FString Error;
	if (!FinalizeRecordingToWav(AudioPath, Error))
	{
		EmitStatus(EUnrealClaudeVoiceDictationState::Failed, Error);
		return;
	}

	BeginTranscription(AudioPath);
}

void FUnrealClaudeVoiceDictationService::Shutdown()
{
	if (bShutdown)
	{
		return;
	}

	bShutdown = true;
	OnStatusChanged = nullptr;
	OnTranscriptReady = nullptr;

	if (CaptureSynth.IsValid())
	{
		CaptureSynth->AbortCapturing();
		CaptureSynth.Reset();
	}

	ResetCaptureState();
}

void FUnrealClaudeVoiceDictationService::DrainCapturedAudio()
{
	if (!CaptureSynth.IsValid())
	{
		return;
	}

	TArray<float> AudioChunk;
	while (CaptureSynth->GetAudioData(AudioChunk))
	{
		if (AudioChunk.Num() == 0)
		{
			break;
		}

		CapturedInterleavedAudio.Append(AudioChunk);
		AudioChunk.Reset();
	}
}

void FUnrealClaudeVoiceDictationService::ResetCaptureState()
{
	CapturedInterleavedAudio.Reset();
	CaptureSampleRate = 0;
	CaptureNumChannels = 1;
	CaptureDeviceInfo = Audio::FCaptureDeviceInfo();
}

void FUnrealClaudeVoiceDictationService::EmitStatus(
	const EUnrealClaudeVoiceDictationState NewState,
	const FString& Detail,
	const FString& AudioPath,
	const FString& TranscriptPath,
	const FString& Transcript)
{
	Status.State = NewState;
	Status.Detail = Detail;
	Status.AudioPath = AudioPath;
	Status.TranscriptPath = TranscriptPath;
	Status.Transcript = Transcript;

	UE_LOG(
		LogUnrealClaude,
		Log,
		TEXT("VOICE_DICTATION_STATE state=%s detail=\"%s\" audio=\"%s\" transcript_json=\"%s\""),
		*DictationStateToString(NewState),
		*Detail,
		*AudioPath,
		*TranscriptPath);

	if (OnStatusChanged)
	{
		OnStatusChanged(Status);
	}
}

bool FUnrealClaudeVoiceDictationService::FinalizeRecordingToWav(FString& OutAudioPath, FString& OutError)
{
	TArray<float> MonoAudio;
	DownmixToMono(CapturedInterleavedAudio, CaptureNumChannels, MonoAudio);

	TArray<float> ResampledAudio;
	ResampleMonoLinear(
		MonoAudio,
		FMath::Max(CaptureSampleRate, UnrealClaudeConstants::VoiceDictation::TargetSampleRate),
		UnrealClaudeConstants::VoiceDictation::TargetSampleRate,
		ResampledAudio);

	if (ResampledAudio.Num() < UnrealClaudeConstants::VoiceDictation::MinCapturedSamplesForTranscript)
	{
		OutError = TEXT("No usable audio captured. Try speaking for longer before stopping.");
		return false;
	}

	TArray<int16> Pcm16;
	ConvertFloatToPcm16(ResampledAudio, Pcm16);

	TArray<uint8> WaveData;
	SerializeWaveFile(
		WaveData,
		reinterpret_cast<const uint8*>(Pcm16.GetData()),
		Pcm16.Num() * sizeof(int16),
		1,
		UnrealClaudeConstants::VoiceDictation::TargetSampleRate);

	FRuntimePaths Paths = GetRuntimePaths();
	IFileManager::Get().MakeDirectory(*Paths.VoiceRoot, true);

	const FString AudioFileName = FString::Printf(TEXT("dictation_capture_%s.wav"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")));
	OutAudioPath = FPaths::Combine(Paths.VoiceRoot, AudioFileName);

	if (!FFileHelper::SaveArrayToFile(WaveData, *OutAudioPath))
	{
		OutError = FString::Printf(TEXT("Failed to write captured audio to %s"), *OutAudioPath);
		return false;
	}

	UE_LOG(
		LogUnrealClaude,
		Log,
		TEXT("VOICE_DICTATION_AUDIO_WRITTEN path=\"%s\" input_samples=%d output_samples=%d"),
		*OutAudioPath,
		CapturedInterleavedAudio.Num(),
		ResampledAudio.Num());

	return true;
}

void FUnrealClaudeVoiceDictationService::BeginTranscription(const FString& AudioPath)
{
	const TWeakPtr<FUnrealClaudeVoiceDictationService> WeakService = AsShared();

	AsyncTask(ENamedThreads::GameThread, [WeakService]()
	{
		if (const TSharedPtr<FUnrealClaudeVoiceDictationService> Pinned = WeakService.Pin())
		{
			Pinned->EmitStatus(EUnrealClaudeVoiceDictationState::PreparingRuntime, Pinned->BuildActiveDictationPreparationDetail());
		}
	});

	Async(EAsyncExecution::ThreadPool, [WeakService, AudioPath]()
	{
		const TSharedPtr<FUnrealClaudeVoiceDictationService> Pinned = WeakService.Pin();
		if (!Pinned.IsValid() || Pinned->bShutdown)
		{
			return;
		}

		FString PythonPath;
		FString Error;
		if (!Pinned->ResolvePythonExecutable(PythonPath, Error))
		{
			AsyncTask(ENamedThreads::GameThread, [WeakService, Error]()
			{
				if (const TSharedPtr<FUnrealClaudeVoiceDictationService> Inner = WeakService.Pin())
				{
					Inner->EmitStatus(EUnrealClaudeVoiceDictationState::Unavailable, Error);
				}
			});
			return;
		}

		const FRuntimePaths Paths = Pinned->GetRuntimePaths();
		if (!Pinned->EnsureRuntimeReady(PythonPath, Paths, Error))
		{
			AsyncTask(ENamedThreads::GameThread, [WeakService, Error]()
			{
				if (const TSharedPtr<FUnrealClaudeVoiceDictationService> Inner = WeakService.Pin())
				{
					Inner->EmitStatus(EUnrealClaudeVoiceDictationState::Unavailable, Error);
				}
			});
			return;
		}

		AsyncTask(ENamedThreads::GameThread, [WeakService]()
		{
			if (const TSharedPtr<FUnrealClaudeVoiceDictationService> Inner = WeakService.Pin())
			{
				Inner->EmitStatus(EUnrealClaudeVoiceDictationState::Transcribing, Inner->BuildActiveDictationTranscribingDetail());
			}
		});

		IFileManager::Get().MakeDirectory(*Paths.VoiceRoot, true);
		const FString TranscriptJsonPath = FPaths::Combine(
			Paths.VoiceRoot,
			FString::Printf(TEXT("dictation_result_%s.json"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"))));

		FString StdOut;
		FString StdErr;
		int32 ExitCode = INDEX_NONE;
		const TArray<FString> ScriptArgs = {
			QuoteArg(Paths.TranscribeScriptPath),
			TEXT("--site-packages"), QuoteArg(Paths.SitePackagesDir),
			TEXT("--model-dir"), QuoteArg(Paths.ModelDir),
			TEXT("--model-name"), QuoteArg(Paths.ModelName),
			TEXT("--language-tag"), QuoteArg(Paths.LanguageTag),
			TEXT("--language-display-name"), QuoteArg(Paths.LanguageDisplayName),
			TEXT("--audio-path"), QuoteArg(AudioPath),
			TEXT("--result-path"), QuoteArg(TranscriptJsonPath)
		};
		const bool bProcessRan = Pinned->RunPythonHelper(PythonPath, Paths.TranscribeScriptPath, ScriptArgs, StdOut, StdErr, ExitCode);

		if (!bProcessRan || ExitCode != 0)
		{
			LogDictationProcessFailure(TEXT("VOICE_DICTATION_TRANSCRIPTION_PROCESS_FAILED"), ExitCode, StdOut, StdErr);
		}

		TSharedPtr<FJsonObject> ResultObject;
		FString ResultSource;
		if (!TryLoadTranscriptionResultObject(TranscriptJsonPath, StdOut, ResultObject, ResultSource))
		{
			UE_LOG(
				LogUnrealClaude,
				Warning,
				TEXT("VOICE_DICTATION_TRANSCRIPTION_NON_JSON stdout=\"%s\" transcript_json=\"%s\""),
				*StdOut.TrimStartAndEnd(),
				*TranscriptJsonPath);
			Error = (!bProcessRan || ExitCode != 0)
				? BuildCompactProcessFailureDetail(TEXT("Dictation transcription"), ExitCode, StdOut, StdErr)
				: BuildCompactNonJsonDetail(TEXT("Dictation transcription"), StdOut);
			Error = Pinned->AppendActiveDictationContext(Error, Paths);
			AsyncTask(ENamedThreads::GameThread, [WeakService, Error, AudioPath, TranscriptJsonPath]()
			{
				if (const TSharedPtr<FUnrealClaudeVoiceDictationService> Inner = WeakService.Pin())
				{
					Inner->EmitStatus(EUnrealClaudeVoiceDictationState::Failed, Error, AudioPath, TranscriptJsonPath);
				}
			});
			return;
		}

		UE_LOG(
			LogUnrealClaude,
			Log,
			TEXT("VOICE_DICTATION_TRANSCRIPTION_RESULT source=%s exit=%d audio=\"%s\" transcript_json=\"%s\""),
			*ResultSource,
			ExitCode,
			*AudioPath,
			*TranscriptJsonPath);

		bool bOk = false;
		ResultObject->TryGetBoolField(TEXT("ok"), bOk);
		FString Transcript;
		ResultObject->TryGetStringField(TEXT("transcript"), Transcript);
		ResultObject->TryGetStringField(TEXT("error"), Error);

		if (!bOk || Transcript.TrimStartAndEnd().IsEmpty())
		{
			if (Error.IsEmpty())
			{
				Error = TEXT("No transcript was produced.");
			}
			Error = BuildCompactFailureDetail(
				TEXT("Dictation transcription"),
				Error,
				TEXT("Dictation transcription failed. See Output Log for details."));
			Error = Pinned->AppendActiveDictationContext(Error, Paths);
			UE_LOG(
				LogUnrealClaude,
				Warning,
				TEXT("VOICE_DICTATION_TRANSCRIPTION_REJECTED error=\"%s\" audio=\"%s\" transcript_json=\"%s\""),
				*Error,
				*AudioPath,
				*TranscriptJsonPath);

			AsyncTask(ENamedThreads::GameThread, [WeakService, Error, AudioPath, TranscriptJsonPath]()
			{
				if (const TSharedPtr<FUnrealClaudeVoiceDictationService> Inner = WeakService.Pin())
				{
					Inner->EmitStatus(EUnrealClaudeVoiceDictationState::Failed, Error, AudioPath, TranscriptJsonPath);
				}
			});
			return;
		}

		const FString CleanTranscript = Transcript.TrimStartAndEnd();
		AsyncTask(ENamedThreads::GameThread, [WeakService, CleanTranscript, AudioPath, TranscriptJsonPath]()
		{
			if (const TSharedPtr<FUnrealClaudeVoiceDictationService> Inner = WeakService.Pin())
			{
				if (Inner->OnTranscriptReady)
				{
					Inner->OnTranscriptReady(CleanTranscript);
				}

				UE_LOG(
					LogUnrealClaude,
					Log,
					TEXT("VOICE_DICTATION_TRANSCRIPT_READY transcript=\"%s\" audio=\"%s\" transcript_json=\"%s\""),
					*CleanTranscript,
					*AudioPath,
					*TranscriptJsonPath);

				Inner->EmitStatus(
					EUnrealClaudeVoiceDictationState::Idle,
					Inner->BuildActiveDictationInsertedDetail(),
					AudioPath,
					TranscriptJsonPath,
					CleanTranscript);
			}
		});
	});
}

FUnrealClaudeVoiceDictationService::FRuntimePaths FUnrealClaudeVoiceDictationService::GetRuntimePaths() const
{
	FRuntimePaths Paths;
	Paths.VoiceRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealClaude"), UnrealClaudeConstants::VoiceDictation::VoiceSubdirectory);
	Paths.RuntimeRoot = FPaths::Combine(Paths.VoiceRoot, TEXT("runtime"));
	Paths.SitePackagesDir = FPaths::Combine(Paths.RuntimeRoot, TEXT("site-packages"));
	const UUnrealClaudeSettings* Settings = UUnrealClaudeSettings::Get();
	Paths.ModelName = Settings ? Settings->GetEffectiveDictationModelName() : UnrealClaudeConstants::VoiceDictation::OfflineEnglishModelName;
	Paths.ModelUrl = Settings ? Settings->GetEffectiveDictationModelUrl() : UnrealClaudeConstants::VoiceDictation::OfflineEnglishModelUrl;
	Paths.LanguageTag = Settings ? Settings->GetEffectiveDictationLanguageTag() : TEXT("en-US");
	Paths.LanguageDisplayName = Settings ? Settings->GetEffectiveDictationLanguageDisplayName() : TEXT("English (US)");
	Paths.ModelDir = FPaths::Combine(Paths.RuntimeRoot, TEXT("models"), Paths.ModelName);

	const FString PluginBaseDir = GetPluginBaseDir();
	Paths.PrepareScriptPath = FPaths::Combine(PluginBaseDir, TEXT("Resources"), TEXT("voice"), TEXT("prepare_offline_runtime.py"));
	Paths.TranscribeScriptPath = FPaths::Combine(PluginBaseDir, TEXT("Resources"), TEXT("voice"), TEXT("transcribe_vosk.py"));
	return Paths;
}

bool FUnrealClaudeVoiceDictationService::ResolvePythonExecutable(FString& OutPythonPath, FString& OutError) const
{
	OutPythonPath = ResolvePythonExecutableCandidate();
	if (OutPythonPath.IsEmpty())
	{
		OutError = TEXT("Dictation unavailable: Python was not found. Install python.exe or set UNREALCLAUDE_DICTATION_PYTHON.");
		return false;
	}

	return true;
}

bool FUnrealClaudeVoiceDictationService::EnsureRuntimeReady(const FString& PythonPath, const FRuntimePaths& Paths, FString& OutError) const
{
	if (!IFileManager::Get().FileExists(*Paths.PrepareScriptPath))
	{
		OutError = FString::Printf(TEXT("Dictation runtime bootstrap script is missing: %s"), *Paths.PrepareScriptPath);
		return false;
	}

	if (!IFileManager::Get().FileExists(*Paths.TranscribeScriptPath))
	{
		OutError = FString::Printf(TEXT("Dictation transcription script is missing: %s"), *Paths.TranscribeScriptPath);
		return false;
	}

	IFileManager::Get().MakeDirectory(*Paths.RuntimeRoot, true);
	IFileManager::Get().MakeDirectory(*Paths.SitePackagesDir, true);

	FString StdOut;
	FString StdErr;
	int32 ExitCode = INDEX_NONE;
	const TArray<FString> ScriptArgs = {
		QuoteArg(Paths.PrepareScriptPath),
		TEXT("--runtime-root"), QuoteArg(Paths.RuntimeRoot),
		TEXT("--model-name"), QuoteArg(Paths.ModelName),
		TEXT("--model-url"), QuoteArg(Paths.ModelUrl),
		TEXT("--language-tag"), QuoteArg(Paths.LanguageTag),
		TEXT("--language-display-name"), QuoteArg(Paths.LanguageDisplayName)
	};

	if (!RunPythonHelper(PythonPath, Paths.PrepareScriptPath, ScriptArgs, StdOut, StdErr, ExitCode) || ExitCode != 0)
	{
		LogDictationProcessFailure(TEXT("VOICE_DICTATION_RUNTIME_PREPARATION_FAILED"), ExitCode, StdOut, StdErr);
		OutError = AppendActiveDictationContext(
			BuildCompactProcessFailureDetail(TEXT("Dictation runtime preparation"), ExitCode, StdOut, StdErr),
			Paths);
		return false;
	}

	TSharedPtr<FJsonObject> ResultObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StdOut);
	if (!FJsonSerializer::Deserialize(Reader, ResultObject) || !ResultObject.IsValid())
	{
		UE_LOG(
			LogUnrealClaude,
			Warning,
			TEXT("VOICE_DICTATION_RUNTIME_PREPARATION_NON_JSON stdout=\"%s\""),
			*StdOut.TrimStartAndEnd());
		OutError = AppendActiveDictationContext(
			BuildCompactNonJsonDetail(TEXT("Dictation runtime preparation"), StdOut),
			Paths);
		return false;
	}

	bool bOk = false;
	ResultObject->TryGetBoolField(TEXT("ok"), bOk);
	if (!bOk)
	{
		ResultObject->TryGetStringField(TEXT("error"), OutError);
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Dictation runtime preparation failed without a diagnostic.");
		}
		UE_LOG(
			LogUnrealClaude,
			Warning,
			TEXT("VOICE_DICTATION_RUNTIME_PREPARATION_REJECTED error=\"%s\""),
			*OutError);
		const FString RuntimeDetail = BuildCompactFailureDetail(
			TEXT("Dictation runtime preparation"),
			OutError,
			TEXT("Dictation runtime preparation failed. See Output Log for details."));
		OutError = AppendActiveDictationContext(RuntimeDetail, Paths);
		return false;
	}

	return true;
}

bool FUnrealClaudeVoiceDictationService::RunPythonHelper(
	const FString& PythonPath,
	const FString& ScriptPath,
	const TArray<FString>& Args,
	FString& OutStdOut,
	FString& OutStdErr,
	int32& OutExitCode) const
{
	if (!IFileManager::Get().FileExists(*PythonPath))
	{
		OutStdErr = FString::Printf(TEXT("Python executable does not exist: %s"), *PythonPath);
		OutExitCode = -1;
		return false;
	}

	if (!IFileManager::Get().FileExists(*ScriptPath))
	{
		OutStdErr = FString::Printf(TEXT("Python helper script does not exist: %s"), *ScriptPath);
		OutExitCode = -1;
		return false;
	}

	const FString ArgsLine = JoinArgs(Args);
	return FPlatformProcess::ExecProcess(
		*PythonPath,
		*ArgsLine,
		&OutExitCode,
		&OutStdOut,
		&OutStdErr,
		*FPaths::ProjectDir());
}

FString FUnrealClaudeVoiceDictationService::GetDebugFixturePath() const
{
	return FPlatformMisc::GetEnvironmentVariable(UnrealClaudeConstants::VoiceDictation::DebugFixtureEnvVar).TrimStartAndEnd();
}

FString FUnrealClaudeVoiceDictationService::BuildActiveDictationLabel() const
{
	const UUnrealClaudeSettings* Settings = UUnrealClaudeSettings::Get();
	const FString DisplayName = Settings ? Settings->GetEffectiveDictationLanguageDisplayName() : TEXT("English (US)");
	const FString LanguageTag = Settings ? Settings->GetEffectiveDictationLanguageTag() : TEXT("en-US");
	const FString ModelName = Settings ? Settings->GetEffectiveDictationModelName() : UnrealClaudeConstants::VoiceDictation::OfflineEnglishModelName;
	return FString::Printf(TEXT("%s (%s, %s)"), *DisplayName, *LanguageTag, *ModelName);
}

FString FUnrealClaudeVoiceDictationService::BuildActiveDictationRecordingDetail() const
{
	return FString::Printf(TEXT("Recording... Click Stop to transcribe. Active language: %s."), *BuildActiveDictationLabel());
}

FString FUnrealClaudeVoiceDictationService::BuildActiveDictationPreparationDetail() const
{
	return FString::Printf(TEXT("Preparing offline dictation runtime for %s..."), *BuildActiveDictationLabel());
}

FString FUnrealClaudeVoiceDictationService::BuildActiveDictationTranscribingDetail() const
{
	return FString::Printf(TEXT("Transcribing audio with %s..."), *BuildActiveDictationLabel());
}

FString FUnrealClaudeVoiceDictationService::BuildActiveDictationInsertedDetail() const
{
	return FString::Printf(TEXT("Transcript inserted. Edit before send. Active language: %s."), *BuildActiveDictationLabel());
}

FString FUnrealClaudeVoiceDictationService::AppendActiveDictationContext(const FString& Detail, const FRuntimePaths& Paths) const
{
	if (Detail.Contains(Paths.ModelName, ESearchCase::IgnoreCase) || Detail.Contains(Paths.LanguageTag, ESearchCase::IgnoreCase))
	{
		return Detail;
	}

	return FString::Printf(
		TEXT("%s Active language/model: %s (%s, %s)."),
		*Detail,
		*Paths.LanguageDisplayName,
		*Paths.LanguageTag,
		*Paths.ModelName);
}
