// Copyright Natali Caggiano. All Rights Reserved.

#include "CodexCliRunner.h"
#include "AgentPromptContract.h"
#include "OsvayderUEConstants.h"
#include "OsvayderUEExecutionLog.h"
#include "OsvayderUEModule.h"
#include "OsvayderUESettings.h"
#include "OsvayderUEStorageMigration.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString FCodexCliRunner::CachedCodexJsPath;

namespace
{
	constexpr int32 CodexBrowserVerifyStartupTimeoutMs = 30000;
	constexpr int32 CodexPersistentAppServerStartupTimeoutMs = 30000;
	constexpr int32 CodexPersistentAppServerRequestTimeoutMs = 300000;
	constexpr float CodexBrowserVerifyPollIntervalSeconds = 0.05f;
	constexpr TCHAR CodexAppServerInitializeMethod[] = TEXT("initialize");
	constexpr TCHAR CodexAppServerInitializedNotification[] = TEXT("initialized");
	constexpr TCHAR CodexAppServerLoginStartMethod[] = TEXT("account/login/start");
	constexpr TCHAR CodexAppServerThreadStartMethod[] = TEXT("thread/start");
	constexpr TCHAR CodexAppServerThreadResumeMethod[] = TEXT("thread/resume");
	constexpr TCHAR CodexAppServerTurnStartMethod[] = TEXT("turn/start");
	constexpr TCHAR CodexAppServerTurnInterruptMethod[] = TEXT("turn/interrupt");
	constexpr TCHAR CodexPersistentServiceName[] = TEXT("osvayderue_editor");

	struct FCodexBrowserVerifySession
	{
		FCriticalSection Mutex;
		FProcHandle ProcessHandle;
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		void* StdInReadPipe = nullptr;
		void* StdInWritePipe = nullptr;
		FString LoginId;
		FString AuthUrl;
		FString CredentialHomePath;
		bool bPendingLogin = false;
	};

	struct FCodexEnvSetupOptions
	{
		bool bClearProxyEnv = false;
	};

	struct FCodexHomeResolution
	{
		FString ConfiguredHomePath;
		FString ResolutionSource = TEXT("default_home");
		FString DefaultHomePath;
		FString MachineStandardHome;
		FString MachineEnvOverrideName;
		TArray<FString> DetectedCandidateHomes;
		FString DetectedArtifactHome;
		FString ConfiguredArtifactPath;
		bool bArtifactPresentInConfiguredHome = false;
		bool bDefaultHomeHasCredentialArtifact = false;
	};

	FCodexBrowserVerifySession GCodexBrowserVerifySession;

#if WITH_DEV_AUTOMATION_TESTS
	bool GHasTestKnownMachineStandardHomesOverride = false;
	TArray<FString> GTestKnownMachineStandardHomes;
	bool GHasTestCodexLaunchOverride = false;
	FString GTestCodexExecutablePath;
	FString GTestCodexJsPath;
#endif

	// OpenAI Codex app-server uses newline-delimited JSON on stdio.
	bool TryExtractJsonlMessage(FString& InOutBuffer, FString& OutLine)
	{
		int32 NewlineIdx = INDEX_NONE;
		int32 CarriageIdx = INDEX_NONE;
		const bool bHasNewline = InOutBuffer.FindChar(TEXT('\n'), NewlineIdx);
		const bool bHasCarriage = InOutBuffer.FindChar(TEXT('\r'), CarriageIdx);
		if (!bHasNewline && !bHasCarriage)
		{
			return false;
		}

		int32 DelimiterIdx = INDEX_NONE;
		if (bHasNewline && bHasCarriage)
		{
			DelimiterIdx = FMath::Min(NewlineIdx, CarriageIdx);
		}
		else
		{
			DelimiterIdx = bHasNewline ? NewlineIdx : CarriageIdx;
		}

		OutLine = InOutBuffer.Left(DelimiterIdx);

		int32 ChopCount = DelimiterIdx + 1;
		while (ChopCount < InOutBuffer.Len())
		{
			const TCHAR DelimiterChar = InOutBuffer[ChopCount];
			if (DelimiterChar != TEXT('\n') && DelimiterChar != TEXT('\r'))
			{
				break;
			}
			++ChopCount;
		}

		InOutBuffer.RightChopInline(ChopCount);
		return true;
	}

	FString StripTerminalControlSequences(const FString& Input)
	{
		FString Sanitized;
		Sanitized.Reserve(Input.Len());

		for (int32 Index = 0; Index < Input.Len();)
		{
			const TCHAR Ch = Input[Index];
			if (Ch == 0x001B)
			{
				if (Index + 1 < Input.Len())
				{
					const TCHAR Next = Input[Index + 1];
					if (Next == TEXT('['))
					{
						Index += 2;
						while (Index < Input.Len())
						{
							const TCHAR SeqCh = Input[Index++];
							if (SeqCh >= 0x40 && SeqCh <= 0x7E)
							{
								break;
							}
						}
						continue;
					}
					if (Next == TEXT(']'))
					{
						Index += 2;
						while (Index < Input.Len())
						{
							const TCHAR SeqCh = Input[Index++];
							if (SeqCh == 0x0007)
							{
								break;
							}
						}
						continue;
					}
				}

				++Index;
				continue;
			}

			if (Ch < 0x20 && Ch != TEXT('\t'))
			{
				++Index;
				continue;
			}

			Sanitized.AppendChar(Ch);
			++Index;
		}

		return Sanitized;
	}

	FString ExtractJsonCandidate(const FString& Input)
	{
		const FString Sanitized = StripTerminalControlSequences(Input).TrimStartAndEnd();
		if (Sanitized.IsEmpty())
		{
			return Sanitized;
		}

		const int32 FirstBrace = Sanitized.Find(TEXT("{"));
		const int32 LastBrace = Sanitized.Find(TEXT("}"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (FirstBrace != INDEX_NONE && LastBrace != INDEX_NONE && LastBrace >= FirstBrace)
		{
			return Sanitized.Mid(FirstBrace, LastBrace - FirstBrace + 1);
		}

		return Sanitized;
	}

	FString ResolveHomeDirectory()
	{
#if PLATFORM_WINDOWS
		FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		if (!UserProfile.IsEmpty())
		{
			return UserProfile;
		}
#endif

		FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
		if (!Home.IsEmpty())
		{
			return Home;
		}

		return FString();
	}

	FString NormalizeConfiguredCodexHomePath(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			return FString();
		}

		if (FPaths::IsRelative(Path))
		{
			Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Path);
		}
		else
		{
			Path = FPaths::ConvertRelativePathToFull(Path);
		}

		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	FString GetDefaultCodexHomePath();

	bool TryFindCodexCredentialArtifactInHome(const FString& CodexHomeDir, FString& OutPath)
	{
		if (CodexHomeDir.IsEmpty())
		{
			return false;
		}

		const TArray<FString> CandidatePaths = {
			FPaths::Combine(CodexHomeDir, TEXT("auth.json")),
			FPaths::Combine(CodexHomeDir, TEXT("credentials.json"))
		};

		for (const FString& Path : CandidatePaths)
		{
			if (IFileManager::Get().FileExists(*Path))
			{
				OutPath = Path;
				return true;
			}
		}

		return false;
	}

	TArray<FString> GetKnownCodexCredentialArtifactPathsInHome(const FString& CodexHomeDir)
	{
		TArray<FString> ArtifactPaths;
		if (CodexHomeDir.IsEmpty())
		{
			return ArtifactPaths;
		}

		const TArray<FString> CandidatePaths = {
			FPaths::Combine(CodexHomeDir, TEXT("auth.json")),
			FPaths::Combine(CodexHomeDir, TEXT("credentials.json"))
		};

		for (const FString& Path : CandidatePaths)
		{
			if (IFileManager::Get().FileExists(*Path))
			{
				ArtifactPaths.Add(Path);
			}
		}

		return ArtifactPaths;
	}

	bool TryFindCodexCredentialArtifact(FString& OutPath)
	{
		return TryFindCodexCredentialArtifactInHome(GetDefaultCodexHomePath(), OutPath);
	}

	void AddUniqueCodexHomePath(TArray<FString>& InOutHomes, const FString& InPath)
	{
		const FString NormalizedPath = NormalizeConfiguredCodexHomePath(InPath);
		if (NormalizedPath.IsEmpty())
		{
			return;
		}

		for (const FString& ExistingHome : InOutHomes)
		{
			if (ExistingHome.Equals(NormalizedPath, ESearchCase::IgnoreCase))
			{
				return;
			}
		}

		InOutHomes.Add(NormalizedPath);
	}

	FString GetFallbackDefaultCodexHomePath()
	{
		const FString HomeDir = ResolveHomeDirectory();
		return HomeDir.IsEmpty()
			? FString()
			: NormalizeConfiguredCodexHomePath(FPaths::Combine(HomeDir, TEXT(".codex")));
	}

	FString GetPreferredProductEnvVar(const TCHAR* PreferredName, const TCHAR* LegacyName, FString* OutResolvedName = nullptr)
	{
		const FString PreferredValue = FPlatformMisc::GetEnvironmentVariable(PreferredName).TrimStartAndEnd();
		if (!PreferredValue.IsEmpty())
		{
			if (OutResolvedName)
			{
				*OutResolvedName = PreferredName;
			}
			return PreferredValue;
		}

		const FString LegacyValue = FPlatformMisc::GetEnvironmentVariable(LegacyName).TrimStartAndEnd();
		if (!LegacyValue.IsEmpty())
		{
			if (OutResolvedName)
			{
				*OutResolvedName = LegacyName;
			}
			return LegacyValue;
		}

		if (OutResolvedName)
		{
			OutResolvedName->Empty();
		}
		return FString();
	}

	FString GetMachineEnvOverrideCodexHomePath(FString* OutResolvedName = nullptr)
	{
		return NormalizeConfiguredCodexHomePath(GetPreferredProductEnvVar(
			TEXT("OSVAYDERUE_CODEX_HOME"),
			TEXT("OSVAYDERUE_CODEX_HOME"),
			OutResolvedName));
	}

	TArray<FString> GetKnownMachineStandardCodexHomeCandidates()
	{
		TArray<FString> Candidates;

#if WITH_DEV_AUTOMATION_TESTS
		if (GHasTestKnownMachineStandardHomesOverride)
		{
			for (const FString& Candidate : GTestKnownMachineStandardHomes)
			{
				AddUniqueCodexHomePath(Candidates, Candidate);
			}
			return Candidates;
		}
#endif

		const FString HomeDir = ResolveHomeDirectory();
		if (!HomeDir.IsEmpty())
		{
			AddUniqueCodexHomePath(Candidates, FPaths::Combine(HomeDir, TEXT(".codex-cli")));
		}

		return Candidates;
	}

	FCodexHomeResolution ResolveConfiguredSharedCodexHome()
	{
		FCodexHomeResolution Resolution;
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		const FString ExplicitOverride = Settings
			? NormalizeConfiguredCodexHomePath(Settings->GetConfiguredCodexHomeOverride())
			: FString();
		const FString MachineEnvOverride = GetMachineEnvOverrideCodexHomePath(&Resolution.MachineEnvOverrideName);

		Resolution.DefaultHomePath = GetFallbackDefaultCodexHomePath();
		if (!Resolution.DefaultHomePath.IsEmpty())
		{
			Resolution.bDefaultHomeHasCredentialArtifact =
				TryFindCodexCredentialArtifactInHome(Resolution.DefaultHomePath, Resolution.ConfiguredArtifactPath);
			Resolution.ConfiguredArtifactPath.Empty();
		}

		const TArray<FString> KnownCandidates = GetKnownMachineStandardCodexHomeCandidates();
		if (KnownCandidates.Num() > 0)
		{
			Resolution.MachineStandardHome = KnownCandidates[0];
		}

		for (const FString& CandidateHome : KnownCandidates)
		{
			FString CandidateArtifactPath;
			const bool bCandidateHasArtifact = TryFindCodexCredentialArtifactInHome(CandidateHome, CandidateArtifactPath);
			if (bCandidateHasArtifact || IFileManager::Get().DirectoryExists(*CandidateHome))
			{
				AddUniqueCodexHomePath(Resolution.DetectedCandidateHomes, CandidateHome);
			}

			if (Resolution.DetectedArtifactHome.IsEmpty() && bCandidateHasArtifact)
			{
				Resolution.DetectedArtifactHome = CandidateHome;
			}
		}

		if (!ExplicitOverride.IsEmpty())
		{
			Resolution.ConfiguredHomePath = ExplicitOverride;
			Resolution.ResolutionSource = TEXT("explicit_override");
		}
		else if (!MachineEnvOverride.IsEmpty())
		{
			Resolution.ConfiguredHomePath = MachineEnvOverride;
			Resolution.ResolutionSource = TEXT("machine_env_override");
		}
		else if (!Resolution.DetectedArtifactHome.IsEmpty())
		{
			Resolution.ConfiguredHomePath = Resolution.DetectedArtifactHome;
			Resolution.ResolutionSource = TEXT("machine_standard_candidate");
		}
		else
		{
			Resolution.ConfiguredHomePath = Resolution.DefaultHomePath;
			Resolution.ResolutionSource = TEXT("default_home");
		}

		Resolution.bArtifactPresentInConfiguredHome =
			TryFindCodexCredentialArtifactInHome(Resolution.ConfiguredHomePath, Resolution.ConfiguredArtifactPath);
		return Resolution;
	}

	FString GetDefaultCodexHomePath()
	{
		return ResolveConfiguredSharedCodexHome().ConfiguredHomePath;
	}

	FString BuildSharedCodexHomeResolutionNote(const FCodexHomeResolution& Resolution)
	{
		if (Resolution.ConfiguredHomePath.IsEmpty())
		{
			return FString();
		}

		if (Resolution.ResolutionSource.Equals(TEXT("explicit_override"), ESearchCase::IgnoreCase))
		{
			return FString::Printf(
				TEXT("Shared Codex home is pinned by DefaultCodexHomeOverride to %s."),
				*Resolution.ConfiguredHomePath);
		}

		if (Resolution.ResolutionSource.Equals(TEXT("machine_env_override"), ESearchCase::IgnoreCase))
		{
			FString EnvName = Resolution.MachineEnvOverrideName;
			if (EnvName.IsEmpty())
			{
				EnvName = TEXT("OSVAYDERUE_CODEX_HOME/OSVAYDERUE_CODEX_HOME");
			}
			return FString::Printf(
				TEXT("Shared Codex home is pinned by %s to %s. OSVAYDERUE_CODEX_HOME takes precedence over legacy OSVAYDERUE_CODEX_HOME when both are set."),
				*EnvName,
				*Resolution.ConfiguredHomePath);
		}

		if (Resolution.ResolutionSource.Equals(TEXT("machine_standard_candidate"), ESearchCase::IgnoreCase))
		{
			if (!Resolution.DefaultHomePath.IsEmpty() && !Resolution.bDefaultHomeHasCredentialArtifact)
			{
				return FString::Printf(
				TEXT("The default Codex home at %s did not contain a known file-backed credential artifact, so Osvayder UE selected the detected machine-standard Codex home at %s."),
					*Resolution.DefaultHomePath,
					*Resolution.ConfiguredHomePath);
			}

			return FString::Printf(
				TEXT("Osvayder UE selected the detected machine-standard Codex home at %s."),
				*Resolution.ConfiguredHomePath);
		}

		return FString();
	}

	FString GetIsolatedApiKeyEnvCodexHome()
	{
		return FPaths::Combine(OsvayderUEStorageMigration::GetPreferredSavedRoot(), TEXT("codex_api_key_env_home"));
	}

	FString GetIsolatedBrowserVerifyCodexHome()
	{
		return FPaths::Combine(OsvayderUEStorageMigration::GetPreferredSavedRoot(), TEXT("codex_browser_verify_home"));
	}

	FString GetPersistentCodexThreadStatePath()
	{
		return FPaths::Combine(OsvayderUEStorageMigration::GetPreferredSavedRoot(), TEXT("codex_persistent_thread.json"));
	}

	FString GetLegacyPersistentCodexThreadStatePath()
	{
		return FPaths::Combine(OsvayderUEStorageMigration::GetLegacySavedRoot(), TEXT("codex_persistent_thread.json"));
	}

	bool ValidatePersistentThreadStateFile(const FString& CandidatePath, FString& OutValidationError)
	{
		FString JsonString;
		if (!FFileHelper::LoadFileToString(JsonString, *CandidatePath))
		{
			OutValidationError = FString::Printf(TEXT("Could not read persisted Codex thread state: %s"), *CandidatePath);
			return false;
		}

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			OutValidationError = FString::Printf(TEXT("Persisted Codex thread state is not valid JSON: %s"), *CandidatePath);
			return false;
		}

		FString StoredThreadId;
		if (!RootObject->TryGetStringField(TEXT("thread_id"), StoredThreadId) || StoredThreadId.IsEmpty())
		{
			OutValidationError = FString::Printf(TEXT("Persisted Codex thread state is missing thread_id: %s"), *CandidatePath);
			return false;
		}

		return true;
	}

	void UpsertRootTomlStringSetting(FString& InOutContents, const FString& Key, const FString& Value)
	{
		const FString KeyPrefix = Key + TEXT(" =");
		TArray<FString> Lines;
		InOutContents.ParseIntoArrayLines(Lines, false);

		bool bReplaced = false;
		for (FString& Line : Lines)
		{
			FString TrimmedLeft = Line;
			TrimmedLeft.TrimStartInline();
			if (TrimmedLeft.StartsWith(KeyPrefix))
			{
				const FString Indent = Line.Left(Line.Len() - TrimmedLeft.Len());
				Line = Indent + FString::Printf(TEXT("%s = \"%s\""), *Key, *Value);
				bReplaced = true;
			}
		}

		if (!bReplaced)
		{
			if (Lines.Num() > 0 && !Lines.Last().IsEmpty())
			{
				Lines.Add(FString());
			}

			Lines.Add(FString::Printf(TEXT("%s = \"%s\""), *Key, *Value));
		}

		InOutContents = FString::Join(Lines, TEXT("\n"));
		if (!InOutContents.IsEmpty() && !InOutContents.EndsWith(TEXT("\n")))
		{
			InOutContents += TEXT("\n");
		}
	}

	bool PrepareManagedCodexHome(const FString& OutPath, const bool bDeleteAuthArtifacts, const bool bForceFileCredentialStore, FString& OutDiagnostic)
	{
		if (!IFileManager::Get().MakeDirectory(*OutPath, true))
		{
			OutDiagnostic += FString::Printf(TEXT("Failed to create managed Codex home at %s.\n"), *OutPath);
			return false;
		}

		const FString SourceCodexHome = GetDefaultCodexHomePath();
		const FString SourceConfigPath = SourceCodexHome.IsEmpty()
			? FString()
			: FPaths::Combine(SourceCodexHome, TEXT("config.toml"));
		const FString TargetConfigPath = FPaths::Combine(OutPath, TEXT("config.toml"));

		FString ConfigContents;
		if (!SourceConfigPath.IsEmpty() && IFileManager::Get().FileExists(*SourceConfigPath))
		{
			if (!FFileHelper::LoadFileToString(ConfigContents, *SourceConfigPath))
			{
				OutDiagnostic += FString::Printf(
					TEXT("Failed to read Codex config from %s while preparing %s.\n"),
					*SourceConfigPath,
					*OutPath);
				return false;
			}
		}

		if (bForceFileCredentialStore)
		{
			UpsertRootTomlStringSetting(ConfigContents, TEXT("cli_auth_credentials_store"), TEXT("file"));
		}

		if (!ConfigContents.IsEmpty())
		{
			if (!FFileHelper::SaveStringToFile(ConfigContents, *TargetConfigPath))
			{
				OutDiagnostic += FString::Printf(
					TEXT("Failed to write Codex config to %s while preparing %s.\n"),
					*TargetConfigPath,
					*OutPath);
				return false;
			}
		}
		else
		{
			IFileManager::Get().Delete(*TargetConfigPath, false, true, true);
		}

		if (bDeleteAuthArtifacts)
		{
			IFileManager::Get().Delete(*FPaths::Combine(OutPath, TEXT("auth.json")), false, true, true);
			IFileManager::Get().Delete(*FPaths::Combine(OutPath, TEXT("credentials.json")), false, true, true);
		}

		return true;
	}

	bool PrepareIsolatedApiKeyEnvCodexHome(FString& OutPath, FString& OutDiagnostic)
	{
		OutPath = GetIsolatedApiKeyEnvCodexHome();
		return PrepareManagedCodexHome(OutPath, true, false, OutDiagnostic);
	}

	bool PrepareIsolatedBrowserVerifyCodexHome(FString& OutPath, FString& OutDiagnostic)
	{
		OutPath = GetIsolatedBrowserVerifyCodexHome();
		return PrepareManagedCodexHome(OutPath, false, true, OutDiagnostic);
	}

	FString GetPluginDirectoryForAgentBackend()
	{
		FString EnginePluginPath = FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("OsvayderUE"));
		if (FPaths::DirectoryExists(EnginePluginPath))
		{
			return EnginePluginPath;
		}

		FString MarketplacePluginPath = FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("Marketplace"), TEXT("OsvayderUE"));
		if (FPaths::DirectoryExists(MarketplacePluginPath))
		{
			return MarketplacePluginPath;
		}

		FString ProjectPluginPath = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("OsvayderUE"));
		if (FPaths::DirectoryExists(ProjectPluginPath))
		{
			return ProjectPluginPath;
		}

		return FString();
	}

	FString QuoteCmdEnv(const TCHAR* Name, const FString& Value)
	{
		return FString::Printf(TEXT("set \"%s=%s\"&& "), Name, *Value);
	}

	FString BuildLocalhostNoProxyValue()
	{
		TArray<FString> Entries;

		auto AddEntryIfMissing = [&Entries](const FString& Entry)
		{
			const FString Trimmed = Entry.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				return;
			}

			for (const FString& Existing : Entries)
			{
				if (Existing.Equals(Trimmed, ESearchCase::IgnoreCase))
				{
					return;
				}
			}

			Entries.Add(Trimmed);
		};

		const FString ExistingNoProxy = FPlatformMisc::GetEnvironmentVariable(TEXT("NO_PROXY"));
		TArray<FString> ExistingEntries;
		ExistingNoProxy.ParseIntoArray(ExistingEntries, TEXT(","), true);
		for (const FString& Entry : ExistingEntries)
		{
			AddEntryIfMissing(Entry);
		}

		AddEntryIfMissing(TEXT("localhost"));
		AddEntryIfMissing(TEXT("127.0.0.1"));
		AddEntryIfMissing(TEXT("::1"));
		return FString::Join(Entries, TEXT(","));
	}

	bool CanLaunchExecutableForVersionProbe(const FString& CandidatePath)
	{
		if (CandidatePath.IsEmpty() || !IFileManager::Get().FileExists(*CandidatePath))
		{
			return false;
		}

		int32 ReturnCode = INDEX_NONE;
		FString StdOut;
		FString StdErr;
		FPlatformProcess::ExecProcess(*CandidatePath, TEXT("--version"), &ReturnCode, &StdOut, &StdErr);
		return ReturnCode == 0;
	}

	FString GetPreferredRipgrepExecutablePath()
	{
		static bool bResolved = false;
		static FString CachedExecutablePath;
		if (bResolved)
		{
			return CachedExecutablePath;
		}

		bResolved = true;

		TArray<FString> CandidateExecutablePaths;

		const FString ExplicitOverride = GetPreferredProductEnvVar(TEXT("OSVAYDERUE_RG_PATH"), TEXT("OSVAYDERUE_RG_PATH"));
		if (!ExplicitOverride.IsEmpty())
		{
			CandidateExecutablePaths.Add(ExplicitOverride);
		}

#if PLATFORM_WINDOWS
		const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		if (!LocalAppData.IsEmpty())
		{
			CandidateExecutablePaths.Add(FPaths::Combine(LocalAppData, TEXT("Programs/cursor/resources/app/node_modules/@vscode/ripgrep/bin/rg.exe")));
			CandidateExecutablePaths.Add(FPaths::Combine(LocalAppData, TEXT("Programs/cursor/resources/app/extensions/cursor-agent/dist/claude-agent-sdk/vendor/ripgrep/rg.exe")));
			CandidateExecutablePaths.Add(FPaths::Combine(LocalAppData, TEXT("Programs/Microsoft VS Code/resources/app/node_modules/@vscode/ripgrep/bin/rg.exe")));
		}
		CandidateExecutablePaths.Add(TEXT("C:/Program Files/Microsoft VS Code/resources/app/node_modules/@vscode/ripgrep/bin/rg.exe"));
#endif

		const FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
		TArray<FString> PathDirs;
#if PLATFORM_WINDOWS
		PathEnv.ParseIntoArray(PathDirs, TEXT(";"), true);
#else
		PathEnv.ParseIntoArray(PathDirs, TEXT(":"), true);
#endif
		for (const FString& Dir : PathDirs)
		{
			if (!Dir.TrimStartAndEnd().IsEmpty())
			{
				CandidateExecutablePaths.Add(FPaths::Combine(Dir, TEXT("rg.exe")));
			}
		}

		for (const FString& CandidatePath : CandidateExecutablePaths)
		{
			if (CanLaunchExecutableForVersionProbe(CandidatePath))
			{
				CachedExecutablePath = FPaths::ConvertRelativePathToFull(CandidatePath);
				FPaths::NormalizeFilename(CachedExecutablePath);
				break;
			}
		}

		return CachedExecutablePath;
	}

	bool EnsureRipgrepShim(FString& OutShimDirectory, FString& OutRealExecutablePath)
	{
		OutRealExecutablePath = GetPreferredRipgrepExecutablePath();
		if (OutRealExecutablePath.IsEmpty())
		{
			return false;
		}

		OutShimDirectory = FPaths::Combine(OsvayderUEStorageMigration::GetPreferredSavedRoot(), TEXT("LocalTools"), TEXT("bin"));
		if (!IFileManager::Get().MakeDirectory(*OutShimDirectory, true))
		{
			return false;
		}

		const FString WrapperScriptPath = FPaths::Combine(OutShimDirectory, TEXT("rg-wrapper.ps1"));
		const FString WrapperCmdPath = FPaths::Combine(OutShimDirectory, TEXT("rg.cmd"));

		const FString WrapperScript =
			TEXT("$realRg = if (-not [string]::IsNullOrWhiteSpace($env:OSVAYDERUE_REAL_RG)) { $env:OSVAYDERUE_REAL_RG } else { $env:OSVAYDERUE_REAL_RG }\n")
			TEXT("if ([string]::IsNullOrWhiteSpace($realRg) -or -not (Test-Path $realRg)) {\n")
			TEXT("    Write-Error 'OSVAYDERUE_REAL_RG / OSVAYDERUE_REAL_RG is not set to a valid executable path.'\n")
			TEXT("    exit 9009\n")
			TEXT("}\n")
			TEXT("$forward = New-Object System.Collections.Generic.List[string]\n")
			TEXT("foreach ($arg in $args) {\n")
			TEXT("    $hasPathSeparator = $arg.Contains('/') -or $arg.Contains('\\')\n")
			TEXT("    $hasWildcard = ($arg.IndexOf('*') -ge 0) -or ($arg.IndexOf('?') -ge 0)\n")
			TEXT("    if ($hasPathSeparator -and $hasWildcard) {\n")
			TEXT("        $expanded = @(Get-ChildItem -Path $arg -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })\n")
			TEXT("        if ($expanded.Count -gt 0) {\n")
			TEXT("            foreach ($match in $expanded) { [void]$forward.Add($match) }\n")
			TEXT("            continue\n")
			TEXT("        }\n")
			TEXT("    }\n")
			TEXT("    [void]$forward.Add($arg)\n")
			TEXT("}\n")
			TEXT("& $realRg @forward\n")
			TEXT("exit $LASTEXITCODE\n");

		const FString WrapperCmd =
			TEXT("@echo off\r\n")
			TEXT("powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0rg-wrapper.ps1\" %*\r\n")
			TEXT("exit /b %ERRORLEVEL%\r\n");

		return FFileHelper::SaveStringToFile(WrapperScript, *WrapperScriptPath)
			&& FFileHelper::SaveStringToFile(WrapperCmd, *WrapperCmdPath);
	}

	FString BuildStandardCodexEnvSetup(const FCodexEnvSetupOptions& Options = FCodexEnvSetupOptions())
	{
		FString EnvSetup;

		const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
		const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString TempPath = FPlatformMisc::GetEnvironmentVariable(TEXT("TEMP"));

		if (!UserProfile.IsEmpty())
		{
			EnvSetup += QuoteCmdEnv(TEXT("USERPROFILE"), UserProfile);
			EnvSetup += QuoteCmdEnv(TEXT("HOME"), UserProfile);
		}
		if (!AppData.IsEmpty())
		{
			EnvSetup += QuoteCmdEnv(TEXT("APPDATA"), AppData);
		}
		if (!LocalAppData.IsEmpty())
		{
			EnvSetup += QuoteCmdEnv(TEXT("LOCALAPPDATA"), LocalAppData);
		}
		if (!TempPath.IsEmpty())
		{
			EnvSetup += QuoteCmdEnv(TEXT("TEMP"), TempPath);
			EnvSetup += QuoteCmdEnv(TEXT("TMP"), TempPath);
		}

		const FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
		FString PreferredRipgrepPath;
		FString PreferredRipgrepShimDir;
		const bool bHasRipgrepShim = EnsureRipgrepShim(PreferredRipgrepShimDir, PreferredRipgrepPath);
		if (!PreferredRipgrepPath.IsEmpty())
		{
			const FString PreferredRipgrepDir = FPaths::GetPath(PreferredRipgrepPath);
			TArray<FString> PathEntries;
#if PLATFORM_WINDOWS
			const FString PathSeparator = TEXT(";");
#else
			const FString PathSeparator = TEXT(":");
#endif
			PathEnv.ParseIntoArray(PathEntries, *PathSeparator, true);

			bool bAlreadyFirst = false;
			if (PathEntries.Num() > 0)
			{
				FString FirstEntry = PathEntries[0];
				FPaths::NormalizeDirectoryName(FirstEntry);
				const FString ExpectedFirstEntry = bHasRipgrepShim ? PreferredRipgrepShimDir : PreferredRipgrepDir;
				bAlreadyFirst = FirstEntry.Equals(ExpectedFirstEntry, ESearchCase::IgnoreCase);
			}

			EnvSetup += QuoteCmdEnv(TEXT("OSVAYDERUE_REAL_RG"), PreferredRipgrepPath);
			EnvSetup += QuoteCmdEnv(TEXT("OSVAYDERUE_REAL_RG"), PreferredRipgrepPath);

			if (!bAlreadyFirst)
			{
				const FString PreferredPathEntry = bHasRipgrepShim ? PreferredRipgrepShimDir : PreferredRipgrepDir;
				const FString UpdatedPath = PathEnv.IsEmpty()
					? PreferredPathEntry
					: PreferredPathEntry + PathSeparator + PathEnv;
				EnvSetup += QuoteCmdEnv(TEXT("PATH"), UpdatedPath);
			}
		}

		if (Options.bClearProxyEnv)
		{
			EnvSetup += QuoteCmdEnv(TEXT("ALL_PROXY"), FString());
			EnvSetup += QuoteCmdEnv(TEXT("HTTP_PROXY"), FString());
			EnvSetup += QuoteCmdEnv(TEXT("HTTPS_PROXY"), FString());
			EnvSetup += QuoteCmdEnv(TEXT("NO_PROXY"), BuildLocalhostNoProxyValue());
		}

		return EnvSetup;
	}

	FString GetConfiguredCodexModel()
	{
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		return Settings ? Settings->GetConfiguredCodexModel() : FString();
	}

	FString GetExplicitCodexProfile()
	{
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		return Settings ? Settings->GetExplicitCodexProfile() : FString();
	}

	FString GetCodexProfileLabel()
	{
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		return Settings ? Settings->GetEffectiveCodexProfileLabel() : TEXT("default");
	}

	FString GetConfiguredCodexSpeedMode()
	{
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		return Settings ? Settings->GetConfiguredCodexSpeedModeName() : TEXT("standard");
	}

	FString GetConfiguredCodexWorkMode()
	{
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		return Settings ? Settings->GetConfiguredCodexWorkModeName() : TEXT("balanced");
	}

	FString GetConfiguredCodexReasoningEffort()
	{
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		return Settings ? Settings->GetConfiguredCodexReasoningEffortName() : TEXT("medium");
	}

	FString GetConfiguredCodexVerbosity()
	{
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		return Settings ? Settings->GetConfiguredCodexVerbosityName() : TEXT("medium");
	}

	FString ResolveCodexJsPathForDirectLaunch()
	{
#if PLATFORM_WINDOWS
		const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
		if (AppData.IsEmpty())
		{
			return FString();
		}

		const FString CodexJsPath = FPaths::Combine(AppData, TEXT("npm"), TEXT("node_modules"), TEXT("@openai"), TEXT("codex"), TEXT("bin"), TEXT("codex.js"));
		return IFileManager::Get().FileExists(*CodexJsPath) ? CodexJsPath : FString();
#else
		return FString();
#endif
	}

	void CloseBrowserVerifyResources(
		FProcHandle& InOutProcessHandle,
		void*& InOutReadPipe,
		void*& InOutWritePipe,
		void*& InOutStdInReadPipe,
		void*& InOutStdInWritePipe,
		const bool bTerminateProcess)
	{
		if (InOutStdInReadPipe || InOutStdInWritePipe)
		{
			FPlatformProcess::ClosePipe(InOutStdInReadPipe, InOutStdInWritePipe);
			InOutStdInReadPipe = nullptr;
			InOutStdInWritePipe = nullptr;
		}

		if (InOutReadPipe || InOutWritePipe)
		{
			FPlatformProcess::ClosePipe(InOutReadPipe, InOutWritePipe);
			InOutReadPipe = nullptr;
			InOutWritePipe = nullptr;
		}

		if (InOutProcessHandle.IsValid())
		{
			if (bTerminateProcess && FPlatformProcess::IsProcRunning(InOutProcessHandle))
			{
				FPlatformProcess::TerminateProc(InOutProcessHandle, true);
			}

			FPlatformProcess::CloseProc(InOutProcessHandle);
			InOutProcessHandle = FProcHandle();
		}
	}

	void CleanupGlobalBrowserVerifySession(const bool bTerminateProcess)
	{
		FScopeLock Lock(&GCodexBrowserVerifySession.Mutex);
		GCodexBrowserVerifySession.bPendingLogin = false;
		GCodexBrowserVerifySession.LoginId.Empty();
		GCodexBrowserVerifySession.AuthUrl.Empty();
		GCodexBrowserVerifySession.CredentialHomePath.Empty();
		CloseBrowserVerifyResources(
			GCodexBrowserVerifySession.ProcessHandle,
			GCodexBrowserVerifySession.ReadPipe,
			GCodexBrowserVerifySession.WritePipe,
			GCodexBrowserVerifySession.StdInReadPipe,
			GCodexBrowserVerifySession.StdInWritePipe,
			bTerminateProcess);
	}

	bool TryGetPendingBrowserVerifyLogin(FString& OutAuthUrl, FString& OutCredentialHomePath)
	{
		FScopeLock Lock(&GCodexBrowserVerifySession.Mutex);
		if (!GCodexBrowserVerifySession.bPendingLogin || !GCodexBrowserVerifySession.ProcessHandle.IsValid())
		{
			return false;
		}

		if (!FPlatformProcess::IsProcRunning(GCodexBrowserVerifySession.ProcessHandle))
		{
			GCodexBrowserVerifySession.bPendingLogin = false;
			GCodexBrowserVerifySession.LoginId.Empty();
			GCodexBrowserVerifySession.AuthUrl.Empty();
			GCodexBrowserVerifySession.CredentialHomePath.Empty();
			CloseBrowserVerifyResources(
				GCodexBrowserVerifySession.ProcessHandle,
				GCodexBrowserVerifySession.ReadPipe,
				GCodexBrowserVerifySession.WritePipe,
				GCodexBrowserVerifySession.StdInReadPipe,
				GCodexBrowserVerifySession.StdInWritePipe,
				false);
			return false;
		}

		OutAuthUrl = GCodexBrowserVerifySession.AuthUrl;
		OutCredentialHomePath = GCodexBrowserVerifySession.CredentialHomePath;
		return !OutAuthUrl.IsEmpty();
	}

	bool TryGetPendingBrowserVerifyAuthUrl(FString& OutAuthUrl)
	{
		FString IgnoredCredentialHomePath;
		return TryGetPendingBrowserVerifyLogin(OutAuthUrl, IgnoredCredentialHomePath);
	}

	bool HasPendingBrowserVerifyLogin()
	{
		FString IgnoredAuthUrl;
		return TryGetPendingBrowserVerifyAuthUrl(IgnoredAuthUrl);
	}

	bool TryLaunchBrowserUrl(const FString& AuthUrl, FString& OutDiagnostic)
	{
		if (AuthUrl.IsEmpty())
		{
			OutDiagnostic = TEXT("Codex Browser Verify did not return a browser URL.");
			return false;
		}

		FString LaunchError;
		FPlatformProcess::LaunchURL(*AuthUrl, nullptr, &LaunchError);
		if (!LaunchError.IsEmpty())
		{
			OutDiagnostic = FString::Printf(
				TEXT("Codex Browser Verify returned a browser URL, but Unreal failed to open it: %s"),
				*LaunchError);
			return false;
		}

		return true;
	}

	bool TryCreateBrowserVerifyPipes(
		void*& OutReadPipe,
		void*& OutWritePipe,
		void*& OutStdInReadPipe,
		void*& OutStdInWritePipe,
		FString& OutDiagnostic)
	{
		if (!FPlatformProcess::CreatePipe(OutReadPipe, OutWritePipe, false))
		{
			OutDiagnostic = TEXT("Failed to create Codex Browser Verify stdout pipe.");
			return false;
		}

		if (!FPlatformProcess::CreatePipe(OutStdInReadPipe, OutStdInWritePipe, true))
		{
			OutDiagnostic = TEXT("Failed to create Codex Browser Verify stdin pipe.");
			FPlatformProcess::ClosePipe(OutReadPipe, OutWritePipe);
			OutReadPipe = nullptr;
			OutWritePipe = nullptr;
			return false;
		}

		return true;
	}

	bool TryBuildCodexAppServerLaunchSpec(const FString& BrowserVerifyHome, const bool bClearProxyEnv, FString& OutExecutable, FString& OutArgs, FString& OutDiagnostic)
	{
		const FString CodexPath = FCodexCliRunner::GetCodexPath();
		if (CodexPath.IsEmpty())
		{
			OutDiagnostic = TEXT("Codex CLI path could not be resolved.");
			return false;
		}

#if PLATFORM_WINDOWS
		FCodexEnvSetupOptions EnvOptions;
		EnvOptions.bClearProxyEnv = bClearProxyEnv;
		FString EnvSetup = BuildStandardCodexEnvSetup(EnvOptions);
		if (!BrowserVerifyHome.IsEmpty())
		{
			EnvSetup += QuoteCmdEnv(TEXT("CODEX_HOME"), BrowserVerifyHome);
			EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), FString());
		}

		const FString CodexJsPath = ResolveCodexJsPathForDirectLaunch();
		if (!CodexJsPath.IsEmpty() && FPaths::GetCleanFilename(CodexPath).Equals(TEXT("node.exe"), ESearchCase::IgnoreCase))
		{
			OutExecutable = TEXT("cmd.exe");
			OutArgs = FString::Printf(TEXT("/c \"%s\"%s\" \"%s\" app-server --listen stdio:// 2>&1\""), *EnvSetup, *CodexPath, *CodexJsPath);
			return true;
		}

		FString DirectExecutable = CodexPath;
		if (FPaths::GetExtension(DirectExecutable, true).IsEmpty())
		{
			const FString ExeVariant = DirectExecutable + TEXT(".exe");
			if (IFileManager::Get().FileExists(*ExeVariant))
			{
				DirectExecutable = ExeVariant;
			}
		}

		if (!DirectExecutable.EndsWith(TEXT(".exe"), ESearchCase::IgnoreCase))
		{
			OutDiagnostic = FString::Printf(
				TEXT("Browser Verify requires a directly launchable Codex executable or node-backed codex.js path. Resolved path `%s` is not supported for app-server login."),
				*CodexPath);
			return false;
		}

		OutExecutable = TEXT("cmd.exe");
		OutArgs = FString::Printf(TEXT("/c \"%s\"%s\" app-server --listen stdio:// 2>&1\""), *EnvSetup, *DirectExecutable);
		return true;
#else
		OutExecutable = CodexPath;
		OutArgs = TEXT("app-server --listen stdio://");
		return true;
#endif
	}

	bool TryWriteJsonRpcRequest(
		void* StdInWritePipe,
		const int32 RequestId,
		const FString& Method,
		const TSharedRef<FJsonObject>& Params,
		FString& OutDiagnostic)
	{
		if (!StdInWritePipe)
		{
			OutDiagnostic = TEXT("Codex Browser Verify stdin pipe is not available.");
			return false;
		}

		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetNumberField(TEXT("id"), RequestId);
		Request->SetStringField(TEXT("method"), Method);
		Request->SetObjectField(TEXT("params"), Params);

		FString RequestLine;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&RequestLine);
		if (!FJsonSerializer::Serialize(Request, Writer))
		{
			OutDiagnostic = TEXT("Failed to serialize Codex Browser Verify JSON-RPC request.");
			return false;
		}

		RequestLine.AppendChar(TEXT('\n'));
		FTCHARToUTF8 Utf8Request(*RequestLine);
		int32 BytesWritten = 0;
		FPlatformProcess::WritePipe(
			StdInWritePipe,
			reinterpret_cast<const uint8*>(Utf8Request.Get()),
			Utf8Request.Length(),
			&BytesWritten);

		if (BytesWritten != Utf8Request.Length())
		{
			OutDiagnostic = FString::Printf(
				TEXT("Failed to send Codex Browser Verify request `%s` to app-server."),
				*Method);
			return false;
		}

		return true;
	}

	bool TryWriteJsonRpcNotification(
		void* StdInWritePipe,
		const FString& Method,
		const TSharedRef<FJsonObject>& Params,
		FString& OutDiagnostic)
	{
		if (!StdInWritePipe)
		{
			OutDiagnostic = TEXT("Codex app-server stdin pipe is not available.");
			return false;
		}

		TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
		Notification->SetStringField(TEXT("method"), Method);
		Notification->SetObjectField(TEXT("params"), Params);

		FString RequestLine;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&RequestLine);
		if (!FJsonSerializer::Serialize(Notification, Writer))
		{
			OutDiagnostic = TEXT("Failed to serialize Codex app-server JSON-RPC notification.");
			return false;
		}

		RequestLine.AppendChar(TEXT('\n'));
		FTCHARToUTF8 Utf8Request(*RequestLine);
		int32 BytesWritten = 0;
		FPlatformProcess::WritePipe(
			StdInWritePipe,
			reinterpret_cast<const uint8*>(Utf8Request.Get()),
			Utf8Request.Length(),
			&BytesWritten);

		if (BytesWritten != Utf8Request.Length())
		{
			OutDiagnostic = FString::Printf(
				TEXT("Failed to send Codex app-server notification `%s`."),
				*Method);
			return false;
		}

		return true;
	}

	bool TryReadNextJsonMessage(
		void* ReadPipe,
		FProcHandle& ProcessHandle,
		FString& InOutBuffer,
		const int32 TimeoutMs,
		TSharedPtr<FJsonObject>& OutMessage,
		FString& OutDiagnostic)
	{
		const double Deadline = FPlatformTime::Seconds() + (static_cast<double>(TimeoutMs) / 1000.0);
		bool bDidFinalDrainAfterExit = false;

		while (FPlatformTime::Seconds() < Deadline)
		{
			FString CompleteLine;
			if (TryExtractJsonlMessage(InOutBuffer, CompleteLine))
			{
				const FString JsonCandidate = ExtractJsonCandidate(CompleteLine);
				CompleteLine = JsonCandidate;
				CompleteLine.TrimStartAndEndInline();
				if (CompleteLine.IsEmpty())
				{
					continue;
				}

				TSharedPtr<FJsonObject> JsonMessage;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CompleteLine);
				if (!FJsonSerializer::Deserialize(Reader, JsonMessage) || !JsonMessage.IsValid())
				{
					UE_LOG(LogOsvayderUE, Warning, TEXT("Ignoring non-JSON Codex app-server output during Browser Verify: %s"), *CompleteLine);
					continue;
				}

				OutMessage = JsonMessage;
				return true;
			}

			const FString OutputChunk = FPlatformProcess::ReadPipe(ReadPipe);
			if (!OutputChunk.IsEmpty())
			{
				InOutBuffer += OutputChunk;
				continue;
			}

			if (ProcessHandle.IsValid() && !FPlatformProcess::IsProcRunning(ProcessHandle))
			{
				if (bDidFinalDrainAfterExit)
				{
					OutDiagnostic = TEXT("Codex app-server exited before Browser Verify returned the expected response.");
					return false;
				}

				bDidFinalDrainAfterExit = true;
			}

			FPlatformProcess::Sleep(CodexBrowserVerifyPollIntervalSeconds);
		}

		OutDiagnostic = TEXT("Timed out waiting for Codex Browser Verify app-server response.");
		return false;
	}

	bool TryWaitForJsonRpcResult(
		void* ReadPipe,
		FProcHandle& ProcessHandle,
		FString& InOutBuffer,
		const int32 RequestId,
		const int32 TimeoutMs,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutDiagnostic)
	{
		const double Deadline = FPlatformTime::Seconds() + (static_cast<double>(TimeoutMs) / 1000.0);
		while (FPlatformTime::Seconds() < Deadline)
		{
			TSharedPtr<FJsonObject> Message;
			const int32 RemainingMs = FMath::Max(1, static_cast<int32>((Deadline - FPlatformTime::Seconds()) * 1000.0));
			if (!TryReadNextJsonMessage(ReadPipe, ProcessHandle, InOutBuffer, RemainingMs, Message, OutDiagnostic))
			{
				return false;
			}

			double ResponseId = 0.0;
			if (!Message->TryGetNumberField(TEXT("id"), ResponseId) || FMath::RoundToInt(ResponseId) != RequestId)
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
			if (Message->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject && ErrorObject->IsValid())
			{
				FString ErrorMessage;
				if (!(*ErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage) || ErrorMessage.IsEmpty())
				{
					ErrorMessage = TEXT("unknown Codex app-server error");
				}

				OutDiagnostic = FString::Printf(TEXT("Codex Browser Verify app-server error: %s"), *ErrorMessage);
				return false;
			}

			const TSharedPtr<FJsonObject>* ResultObject = nullptr;
			if (Message->TryGetObjectField(TEXT("result"), ResultObject) && ResultObject && ResultObject->IsValid())
			{
				OutResult = *ResultObject;
				return true;
			}

			OutDiagnostic = TEXT("Codex Browser Verify app-server response did not contain an object result.");
			return false;
		}

		OutDiagnostic = TEXT("Timed out waiting for Codex Browser Verify result.");
		return false;
	}

	void StartBrowserVerifyMonitorThread()
	{
		Async(EAsyncExecution::ThreadPool, []()
		{
			FString PendingBuffer;
			for (;;)
			{
				FProcHandle ProcessHandle;
				void* ReadPipe = nullptr;
				FString ExpectedLoginId;
				FString ExpectedCredentialHome;
				{
					FScopeLock Lock(&GCodexBrowserVerifySession.Mutex);
					if (!GCodexBrowserVerifySession.bPendingLogin)
					{
						return;
					}

					ProcessHandle = GCodexBrowserVerifySession.ProcessHandle;
					ReadPipe = GCodexBrowserVerifySession.ReadPipe;
					ExpectedLoginId = GCodexBrowserVerifySession.LoginId;
					ExpectedCredentialHome = GCodexBrowserVerifySession.CredentialHomePath;
				}

				if (!ProcessHandle.IsValid() || !ReadPipe)
				{
					return;
				}

				const FString OutputChunk = FPlatformProcess::ReadPipe(ReadPipe);
				if (!OutputChunk.IsEmpty())
				{
					PendingBuffer += OutputChunk;

					FString CompleteLine;
					while (TryExtractJsonlMessage(PendingBuffer, CompleteLine))
					{
						CompleteLine = ExtractJsonCandidate(CompleteLine);
						CompleteLine.TrimStartAndEndInline();
						if (CompleteLine.IsEmpty())
						{
							continue;
						}

						TSharedPtr<FJsonObject> JsonMessage;
						const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CompleteLine);
						if (!FJsonSerializer::Deserialize(Reader, JsonMessage) || !JsonMessage.IsValid())
						{
							UE_LOG(LogOsvayderUE, Warning, TEXT("Ignoring non-JSON Codex Browser Verify monitor output: %s"), *CompleteLine);
							continue;
						}

						FString Method;
						const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
						if (!JsonMessage->TryGetStringField(TEXT("method"), Method) ||
							!JsonMessage->TryGetObjectField(TEXT("params"), ParamsObject) ||
							!ParamsObject ||
							!ParamsObject->IsValid() ||
							!Method.Contains(TEXT("login"), ESearchCase::IgnoreCase))
						{
							continue;
						}

						bool bSuccess = false;
						if (!(*ParamsObject)->TryGetBoolField(TEXT("success"), bSuccess))
						{
							continue;
						}

						FString LoginId;
						(*ParamsObject)->TryGetStringField(TEXT("loginId"), LoginId);
						if (!ExpectedLoginId.IsEmpty() && !LoginId.IsEmpty() && LoginId != ExpectedLoginId)
						{
							continue;
						}

						FString ErrorText;
						(*ParamsObject)->TryGetStringField(TEXT("error"), ErrorText);
						if (bSuccess)
						{
							UE_LOG(LogOsvayderUE, Log, TEXT("Codex Browser Verify completed for loginId %s."), *LoginId);
						}
						else
						{
							UE_LOG(LogOsvayderUE, Warning, TEXT("Codex Browser Verify failed for loginId %s: %s"), *LoginId, *ErrorText);
						}

						CleanupGlobalBrowserVerifySession(true);
						return;
					}
				}
				else if (!FPlatformProcess::IsProcRunning(ProcessHandle))
				{
					FString CredentialArtifactPath;
					const FString CredentialHome = ExpectedCredentialHome.IsEmpty()
						? GetDefaultCodexHomePath()
						: ExpectedCredentialHome;
					if (TryFindCodexCredentialArtifactInHome(CredentialHome, CredentialArtifactPath))
					{
						UE_LOG(LogOsvayderUE, Log, TEXT("Codex Browser Verify app-server exited. Credential artifact is now present at %s."), *CredentialArtifactPath);
					}
					else
					{
						UE_LOG(LogOsvayderUE, Log, TEXT("Codex Browser Verify app-server exited before any credential artifact was detected."));
					}

					CleanupGlobalBrowserVerifySession(false);
					return;
				}

				FPlatformProcess::Sleep(0.1f);
			}
		});
	}

	struct FCodexAuthContext
	{
		EOsvayderUECodexAuthMode Mode = EOsvayderUECodexAuthMode::Auto;
		FString ModeName = TEXT("auto");
		FString ApiKeyEnvVarName = UOsvayderUESettings::GetDefaultCodexApiKeyEnvVarName();
		FString ApiKeyEnvVarValue;
		FString InheritedOpenAiApiKey;
		FString CredentialArtifactPath;
		FString EffectivePath = TEXT("none_detected");
		FString OwnershipModel = TEXT("none");
		bool bHasNamedEnvVar = false;
		bool bHasInheritedOpenAiApiKey = false;
		bool bHasCredentialArtifact = false;
		bool bShouldForwardApiKeyToChild = false;
		bool bUseIsolatedCodexHome = false;
		FString IsolatedCodexHome;
		FString SharedCodexHome;
		bool bHasExplicitCodexHomeOverride = false;
		FCodexHomeResolution SharedHomeResolution;
	};

	struct FCodexSpeedModeState
	{
		FString RequestedMode = TEXT("standard");
		FString EffectiveMode = TEXT("standard");
		FString SupportLabel = TEXT("fast_mode_disabled_for_launch");
		bool bEnableFastModeFeature = false;
		bool bAppendFastServiceTierOverride = false;
	};

	FCodexEnvSetupOptions BuildEnvSetupOptionsForAuthContext(const FCodexAuthContext& AuthContext)
	{
		FCodexEnvSetupOptions Options;
		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		if (Settings && Settings->ShouldClearProxyEnvForCodexExec())
		{
			Options.bClearProxyEnv = true;
		}
		else if (AuthContext.Mode == EOsvayderUECodexAuthMode::BrowserVerify)
		{
			Options.bClearProxyEnv = Settings && Settings->ShouldClearProxyEnvForBrowserVerify();
		}

		return Options;
	}

	FCodexAuthContext ResolveCodexAuthContext()
	{
		FCodexAuthContext Context;

		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		if (Settings)
		{
			Context.Mode = Settings->GetCodexAuthMode();
			Context.ModeName = Settings->GetConfiguredCodexAuthModeName();
			Context.ApiKeyEnvVarName = Settings->GetConfiguredCodexApiKeyEnvVar();
			Context.bHasExplicitCodexHomeOverride = Settings->HasExplicitCodexHomeOverride();
		}

		Context.SharedHomeResolution = ResolveConfiguredSharedCodexHome();
		Context.SharedCodexHome = Context.SharedHomeResolution.ConfiguredHomePath;

		Context.InheritedOpenAiApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_API_KEY"));
		Context.bHasInheritedOpenAiApiKey = !Context.InheritedOpenAiApiKey.IsEmpty();

		if (Context.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
		{
			Context.ApiKeyEnvVarValue = FPlatformMisc::GetEnvironmentVariable(*Context.ApiKeyEnvVarName);
			if (Context.ApiKeyEnvVarValue.IsEmpty()
				&& Context.ApiKeyEnvVarName.Equals(TEXT("OSVAYDERUE_OPENAI_API_KEY"), ESearchCase::IgnoreCase))
			{
				Context.ApiKeyEnvVarValue = FPlatformMisc::GetEnvironmentVariable(TEXT("OSVAYDERUE_OPENAI_API_KEY"));
				if (!Context.ApiKeyEnvVarValue.IsEmpty())
				{
					Context.ApiKeyEnvVarName = TEXT("OSVAYDERUE_OPENAI_API_KEY");
				}
			}
			Context.bHasNamedEnvVar = !Context.ApiKeyEnvVarValue.IsEmpty();
			Context.bShouldForwardApiKeyToChild = Context.bHasNamedEnvVar;
			Context.bUseIsolatedCodexHome = true;
			Context.IsolatedCodexHome = GetIsolatedApiKeyEnvCodexHome();
			Context.EffectivePath = Context.bHasNamedEnvVar
				? TEXT("api_env_var_bridge")
				: TEXT("api_env_var_missing");
			Context.OwnershipModel = TEXT("plugin_launch_env_bridge");
			return Context;
		}

		if (Context.Mode == EOsvayderUECodexAuthMode::CliTerminal)
		{
			Context.bHasCredentialArtifact = TryFindCodexCredentialArtifactInHome(Context.SharedCodexHome, Context.CredentialArtifactPath);
			Context.EffectivePath = Context.bHasCredentialArtifact
				? TEXT("cli_terminal_artifact")
				: TEXT("cli_terminal_missing");
			Context.OwnershipModel = TEXT("external_terminal_cli_login");
			return Context;
		}

		if (Context.Mode == EOsvayderUECodexAuthMode::BrowserVerify)
		{
			Context.bUseIsolatedCodexHome = true;
			Context.IsolatedCodexHome = GetIsolatedBrowserVerifyCodexHome();
			Context.bHasCredentialArtifact = TryFindCodexCredentialArtifactInHome(Context.IsolatedCodexHome, Context.CredentialArtifactPath);
			const bool bBrowserLoginPending = HasPendingBrowserVerifyLogin();
			Context.EffectivePath = Context.bHasCredentialArtifact
				? TEXT("browser_verify_isolated_artifact")
				: (bBrowserLoginPending ? TEXT("browser_verify_isolated_login_started") : TEXT("browser_verify_isolated_pending_login"));
			Context.OwnershipModel = TEXT("plugin_isolated_browser_login");
			return Context;
		}

		Context.bHasCredentialArtifact = TryFindCodexCredentialArtifactInHome(Context.SharedCodexHome, Context.CredentialArtifactPath);

		if (Context.bHasInheritedOpenAiApiKey)
		{
			Context.EffectivePath = TEXT("process_env_openai_api_key");
			Context.OwnershipModel = TEXT("editor_process_env");
		}
		else if (Context.bHasCredentialArtifact)
		{
			Context.EffectivePath = TEXT("cli_managed_artifact");
			Context.OwnershipModel = TEXT("codex_cli_home");
		}

		return Context;
	}

	bool ResolveEffectiveCodexHomePathForLaunch(
		const FCodexAuthContext& AuthContext,
		const bool bPrepareManagedHome,
		FString& OutHomePath,
		FString& OutDiagnostic)
	{
		OutHomePath = GetDefaultCodexHomePath();

		if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
		{
			OutHomePath = GetIsolatedApiKeyEnvCodexHome();
			return !bPrepareManagedHome || PrepareIsolatedApiKeyEnvCodexHome(OutHomePath, OutDiagnostic);
		}

		if (AuthContext.Mode == EOsvayderUECodexAuthMode::BrowserVerify)
		{
			OutHomePath = GetIsolatedBrowserVerifyCodexHome();
			return !bPrepareManagedHome || PrepareIsolatedBrowserVerifyCodexHome(OutHomePath, OutDiagnostic);
		}

		return true;
	}

	FString ResolveEffectiveCodexHomePathForLaunchNoPrepare(const FCodexAuthContext& AuthContext)
	{
		FString HomePath;
		FString Diagnostic;
		ResolveEffectiveCodexHomePathForLaunch(AuthContext, false, HomePath, Diagnostic);
		return HomePath;
	}

	FString BuildCodexAuthDetailForDiagnostics(const FCodexAuthContext& AuthContext, const FString& EffectiveHomePath, const FString& CredentialArtifactPath)
	{
		if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
		{
			const bool bPreferredApiKeyAliasUsed = AuthContext.bHasNamedEnvVar
				&& AuthContext.ApiKeyEnvVarName.Equals(TEXT("OSVAYDERUE_OPENAI_API_KEY"), ESearchCase::IgnoreCase);
			if (bPreferredApiKeyAliasUsed && !FPlatformMisc::GetEnvironmentVariable(TEXT("OSVAYDERUE_OPENAI_API_KEY")).IsEmpty())
			{
				return TEXT("OSVAYDERUE_OPENAI_API_KEY is present and takes precedence over legacy OSVAYDERUE_OPENAI_API_KEY; probe has not validated the key.");
			}

			return AuthContext.bHasNamedEnvVar
				? FString::Printf(TEXT("%s is present; probe has not validated the key."), *AuthContext.ApiKeyEnvVarName)
				: FString::Printf(TEXT("%s is not set; explicit API auth cannot run."), *AuthContext.ApiKeyEnvVarName);
		}

		if (!CredentialArtifactPath.IsEmpty())
		{
			return FString::Printf(
				TEXT("Known Codex credential artifact is present at %s. Token validity remains unknown until Probe Backend Auth succeeds."),
				*CredentialArtifactPath);
		}

		if (AuthContext.bHasInheritedOpenAiApiKey && AuthContext.Mode == EOsvayderUECodexAuthMode::Auto)
		{
			return TEXT("OPENAI_API_KEY is present in the editor environment; probe has not validated it.");
		}

		if (EffectiveHomePath.IsEmpty())
		{
			return TEXT("No effective Codex home could be resolved.");
		}

		return FString::Printf(TEXT("No known Codex credential artifact exists under effective CODEX_HOME %s."), *EffectiveHomePath);
	}

	FString SanitizeSingleLineDiagnostic(FString Value)
	{
		Value.ReplaceInline(TEXT("\r"), TEXT(" "));
		Value.ReplaceInline(TEXT("\n"), TEXT(" | "));
		while (Value.Contains(TEXT("  ")))
		{
			Value.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		Value.TrimStartAndEndInline();
		return Value;
	}

	FString ResolveRequestedCodexSpeedModeLabel(const FString& RequestedModeOverride)
	{
		const FString TrimmedMode = RequestedModeOverride.TrimStartAndEnd();
		if (TrimmedMode.Equals(TEXT("fast"), ESearchCase::IgnoreCase))
		{
			return TEXT("fast");
		}

		if (TrimmedMode.Equals(TEXT("standard"), ESearchCase::IgnoreCase))
		{
			return TEXT("standard");
		}

		return GetConfiguredCodexSpeedMode();
	}

	bool AuthContextUsesApiKeyPricingForFastMode(const FCodexAuthContext& AuthContext)
	{
		if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
		{
			return true;
		}

		return AuthContext.EffectivePath.Equals(TEXT("process_env_openai_api_key"), ESearchCase::IgnoreCase) ||
			AuthContext.EffectivePath.StartsWith(TEXT("api_env_var"), ESearchCase::IgnoreCase);
	}

	FCodexSpeedModeState ResolveCodexSpeedModeState(const FString& RequestedModeOverride, const FCodexAuthContext& AuthContext)
	{
		FCodexSpeedModeState State;
		State.RequestedMode = ResolveRequestedCodexSpeedModeLabel(RequestedModeOverride);

		if (State.RequestedMode.Equals(TEXT("fast"), ESearchCase::IgnoreCase))
		{
			if (AuthContextUsesApiKeyPricingForFastMode(AuthContext))
			{
				State.EffectiveMode = TEXT("standard");
				State.SupportLabel = TEXT("api_key_standard_pricing_no_fast_credits");
			}
			else
			{
				State.EffectiveMode = TEXT("fast");
				State.SupportLabel = TEXT("launch_override_service_tier_fast");
				State.bEnableFastModeFeature = true;
				State.bAppendFastServiceTierOverride = true;
			}

			return State;
		}

		State.EffectiveMode = TEXT("standard");
		State.SupportLabel = TEXT("fast_mode_disabled_for_launch");
		return State;
	}

	FString BuildCodexSelectionSummary()
	{
		const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
		const FCodexSpeedModeState SpeedState = ResolveCodexSpeedModeState(GetConfiguredCodexSpeedMode(), AuthContext);
		const FString ProfileLabel = GetCodexProfileLabel();
		const FString ModelLabel = GetConfiguredCodexModel();
		const FString SpeedRequested = SpeedState.RequestedMode;
		const FString SpeedEffective = SpeedState.EffectiveMode;
		const FString WorkMode = GetConfiguredCodexWorkMode();
		const FString Reasoning = GetConfiguredCodexReasoningEffort();
		const FString Verbosity = GetConfiguredCodexVerbosity();

		FString Summary;
		if (ModelLabel.IsEmpty())
		{
			Summary = FString::Printf(TEXT("profile `%s`"), *ProfileLabel);
		}
		else
		{
			Summary = FString::Printf(TEXT("profile `%s`, model `%s`"), *ProfileLabel, *ModelLabel);
		}

		Summary += FString::Printf(
			TEXT(", speed requested `%s`, effective `%s`, work mode `%s`, reasoning `%s`, verbosity `%s`"),
			*SpeedRequested,
			*SpeedEffective,
			*WorkMode,
			*Reasoning,
			*Verbosity);
		Summary += FString::Printf(TEXT(", speed support `%s`"), *SpeedState.SupportLabel);
		return Summary;
	}

	bool TryAppendLiteralConfigOverride(FString& OutArgs, const FString& Key, const FString& Value, FString& OutDiagnostic)
	{
		if (Value.Contains(TEXT("'")))
		{
			OutDiagnostic += FString::Printf(
				TEXT("Cannot build Codex CLI config override for %s because the value contains an apostrophe.\n"),
				*Key);
			return false;
		}

		OutArgs += FString::Printf(TEXT("-c \"%s='%s'\" "), *Key, *Value);
		return true;
	}

	bool ConfigRequestsWorkspaceWriteTools(const FAgentRequestConfig& Config)
	{
		if (Config.ExecutionProfile != EAgentExecutionRunProfile::ConfiguredDefaultRuntime)
		{
			return false;
		}

		for (const FString& AllowedTool : Config.AllowedTools)
		{
			if (AllowedTool.Equals(TEXT("Write"), ESearchCase::IgnoreCase) ||
				AllowedTool.Equals(TEXT("Edit"), ESearchCase::IgnoreCase) ||
				AllowedTool.Equals(TEXT("Bash"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	bool TryConvertAllowedUnrealMcpToolPatternToRawToolName(const FString& AllowedTool, FString& OutRawToolName)
	{
		static const FString UnrealMcpPrefix = TEXT("mcp__osvayderue__");
		if (!AllowedTool.StartsWith(UnrealMcpPrefix, ESearchCase::IgnoreCase))
		{
			return false;
		}

		const FString RawToolName = AllowedTool.RightChop(UnrealMcpPrefix.Len()).TrimStartAndEnd();
		if (RawToolName.IsEmpty() || RawToolName == TEXT("*"))
		{
			return false;
		}

		OutRawToolName = RawToolName;
		return true;
	}

	TArray<FString> GetRequestedScopedUnrealMcpToolNames(const FAgentRequestConfig& Config)
	{
		TArray<FString> ToolNames;
		for (const FString& AllowedTool : Config.AllowedTools)
		{
			FString RawToolName;
			if (TryConvertAllowedUnrealMcpToolPatternToRawToolName(AllowedTool, RawToolName))
			{
				ToolNames.AddUnique(RawToolName);
			}
		}

		ToolNames.Sort([](const FString& A, const FString& B)
		{
			return A.Compare(B, ESearchCase::IgnoreCase) < 0;
		});

		return ToolNames;
	}

	bool ConfigRequestsUnrealMcpBridge(const FAgentRequestConfig& Config)
	{
		return Config.bEnableUnrealMcpBridge || GetRequestedScopedUnrealMcpToolNames(Config).Num() > 0;
	}

	FString BuildPersistentUnrealMcpLaunchSignature(const FAgentRequestConfig& Config)
	{
		if (!ConfigRequestsUnrealMcpBridge(Config))
		{
			return TEXT("disabled");
		}

		if (Config.bEnableUnrealMcpBridge)
		{
			return TEXT("full_bridge");
		}

		const TArray<FString> ScopedToolNames = GetRequestedScopedUnrealMcpToolNames(Config);
		return ScopedToolNames.Num() > 0
			? FString::Printf(TEXT("scoped:%s"), *FString::Join(ScopedToolNames, TEXT(",")))
			: TEXT("scoped:");
	}

	FString GetPersistentExecutionProfileName(const FAgentRequestConfig& Config)
	{
		return FString(AgentExecutionRunProfileToString(Config.ExecutionProfile));
	}

	FString GetPersistentSandboxWorkingDirectory(const FAgentRequestConfig& Config)
	{
		FString WorkingDir = Config.WorkingDirectory.IsEmpty()
			? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())
			: FPaths::ConvertRelativePathToFull(Config.WorkingDirectory);
		FPaths::NormalizeDirectoryName(WorkingDir);
		return WorkingDir;
	}

	FString GetPersistentThreadSandboxMode(const FAgentRequestConfig& Config)
	{
		if (Config.bSkipPermissions)
		{
			return TEXT("danger-full-access");
		}

		return ConfigRequestsWorkspaceWriteTools(Config)
			? TEXT("workspace-write")
			: TEXT("read-only");
	}

	TSharedRef<FJsonObject> BuildPersistentTurnSandboxPolicy(const FAgentRequestConfig& Config)
	{
		TSharedRef<FJsonObject> SandboxPolicy = MakeShared<FJsonObject>();
		if (Config.bSkipPermissions)
		{
			SandboxPolicy->SetStringField(TEXT("type"), TEXT("dangerFullAccess"));
			return SandboxPolicy;
		}

		if (ConfigRequestsWorkspaceWriteTools(Config))
		{
			SandboxPolicy->SetStringField(TEXT("type"), TEXT("workspaceWrite"));
			const FString WorkingDir = GetPersistentSandboxWorkingDirectory(Config);
			if (!WorkingDir.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> WritableRoots;
				WritableRoots.Add(MakeShared<FJsonValueString>(WorkingDir));
				SandboxPolicy->SetArrayField(TEXT("writableRoots"), WritableRoots);
			}
			return SandboxPolicy;
		}

		SandboxPolicy->SetStringField(TEXT("type"), TEXT("readOnly"));
		return SandboxPolicy;
	}

	bool ShouldForceWindowsUnelevatedWorkspaceWriteSandbox(const FAgentRequestConfig& Config)
	{
#if PLATFORM_WINDOWS
		return !Config.bSkipPermissions && ConfigRequestsWorkspaceWriteTools(Config);
#else
		return false;
#endif
	}

	bool TryBuildCodexLaunchOverrideArgs(
		const FAgentRequestConfig& Config,
		const FCodexAuthContext& AuthContext,
		FString& OutArgs,
		FString& OutDiagnostic)
	{
		const FCodexSpeedModeState SpeedState = ResolveCodexSpeedModeState(Config.CodexSpeedMode, AuthContext);
		OutArgs += SpeedState.bEnableFastModeFeature
			? TEXT("--enable fast_mode ")
			: TEXT("--disable fast_mode ");

		if (SpeedState.bAppendFastServiceTierOverride &&
			!TryAppendLiteralConfigOverride(OutArgs, TEXT("service_tier"), TEXT("fast"), OutDiagnostic))
		{
			return false;
		}

		const FString ReasoningEffort = Config.CodexReasoningEffort.TrimStartAndEnd();
		if (!ReasoningEffort.IsEmpty() && !ReasoningEffort.Equals(TEXT("model_default"), ESearchCase::IgnoreCase))
		{
			if (!TryAppendLiteralConfigOverride(OutArgs, TEXT("model_reasoning_effort"), ReasoningEffort, OutDiagnostic))
			{
				return false;
			}
		}

		const FString Verbosity = Config.CodexVerbosity.TrimStartAndEnd();
		if (!Verbosity.IsEmpty() && !Verbosity.Equals(TEXT("model_default"), ESearchCase::IgnoreCase))
		{
			if (!TryAppendLiteralConfigOverride(OutArgs, TEXT("model_verbosity"), Verbosity, OutDiagnostic))
			{
				return false;
			}
		}

		if (ShouldForceWindowsUnelevatedWorkspaceWriteSandbox(Config) &&
			!TryAppendLiteralConfigOverride(OutArgs, TEXT("windows.sandbox"), TEXT("unelevated"), OutDiagnostic))
		{
			return false;
		}

		return true;
	}

	bool TryBuildCodexMcpOverrideArgs(const FAgentRequestConfig& Config, FString& OutArgs, FString& OutDiagnostic)
	{
		const FString PluginDir = GetPluginDirectoryForAgentBackend();
		if (PluginDir.IsEmpty())
		{
			OutDiagnostic += TEXT("Could not resolve OsvayderUE plugin directory.\n");
			return false;
		}

		FString BridgePath = FPaths::Combine(PluginDir, TEXT("Resources"), TEXT("mcp-bridge"), TEXT("index.js"));
		FPaths::NormalizeFilename(BridgePath);
		BridgePath = FPaths::ConvertRelativePathToFull(BridgePath);
		BridgePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (!FPaths::FileExists(BridgePath))
		{
			OutDiagnostic += FString::Printf(TEXT("MCP bridge not found at %s\n"), *BridgePath);
			return false;
		}

		const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
		const int32 McpPort = Settings ? Settings->MCPServerPort : OsvayderUEConstants::MCPServer::DefaultPort;
		const FString McpUrl = FString::Printf(TEXT("http://localhost:%d"), McpPort);

		if (!TryAppendLiteralConfigOverride(OutArgs, TEXT("mcp_servers.osvayderue.command"), TEXT("node"), OutDiagnostic))
		{
			return false;
		}

		if (BridgePath.Contains(TEXT("'")))
		{
			OutDiagnostic += TEXT("Cannot build Codex CLI MCP bridge override because the bridge path contains an apostrophe.\n");
			return false;
		}

		OutArgs += FString::Printf(TEXT("-c \"mcp_servers.osvayderue.args=['%s']\" "), *BridgePath);

		if (!TryAppendLiteralConfigOverride(OutArgs, TEXT("mcp_servers.osvayderue.env.UNREAL_MCP_URL"), McpUrl, OutDiagnostic))
		{
			return false;
		}

		if (!Config.bEnableUnrealMcpBridge)
		{
			const TArray<FString> ScopedToolNames = GetRequestedScopedUnrealMcpToolNames(Config);
			if (ScopedToolNames.Num() > 0)
			{
				if (!TryAppendLiteralConfigOverride(
						OutArgs,
						TEXT("mcp_servers.osvayderue.env.UNREAL_MCP_ALLOWED_TOOLS"),
						FString::Join(ScopedToolNames, TEXT(",")),
						OutDiagnostic))
				{
					return false;
				}
			}
		}

		if (Settings && Settings->bEnableOsvayderEye)
		{
			if (!Settings->OsvayderEyeServerPath.FilePath.IsEmpty())
			{
				FString EyeServerPath = Settings->OsvayderEyeServerPath.FilePath;
				EyeServerPath.ReplaceInline(TEXT("\\"), TEXT("/"));
				if (!TryAppendLiteralConfigOverride(OutArgs, TEXT("mcp_servers.osvayderue.env.EYE_SERVER_PATH"), EyeServerPath, OutDiagnostic))
				{
					return false;
				}
			}

			if (!Settings->OsvayderEyePythonPath.FilePath.IsEmpty())
			{
				FString EyePythonPath = Settings->OsvayderEyePythonPath.FilePath;
				EyePythonPath.ReplaceInline(TEXT("\\"), TEXT("/"));
				if (!TryAppendLiteralConfigOverride(OutArgs, TEXT("mcp_servers.osvayderue.env.EYE_PYTHON_PATH"), EyePythonPath, OutDiagnostic))
				{
					return false;
				}
			}
		}

		return true;
	}

	bool TryGetStringFieldAny(const TSharedPtr<FJsonObject>& JsonObj, const TArray<FString>& FieldNames, FString& OutValue)
	{
		for (const FString& FieldName : FieldNames)
		{
			if (JsonObj->TryGetStringField(FieldName, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	FString SerializeJsonValue(const TSharedPtr<FJsonValue>& JsonValue)
	{
		FString Serialized;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
		FJsonSerializer::Serialize(JsonValue.ToSharedRef(), TEXT(""), Writer);
		Writer->Close();
		return Serialized;
	}

	FString GetCodexAuthDiagnosticsRootDir()
	{
		FString RootDir = FPaths::Combine(OsvayderUEStorageMigration::GetPreferredSavedRoot(), TEXT("CodexAuthDiagnostics"));
		FPaths::NormalizeDirectoryName(RootDir);
		IFileManager::Get().MakeDirectory(*RootDir, true);
		return RootDir;
	}

	FString WriteCodexAuthDiagnosticReceipt(
		const FString& Prefix,
		const FCodexCliRunner::FCodexAuthDiagnostics& Diagnostics,
		const FString& ResultState,
		const FString& Detail)
	{
		const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
		const FString SafePrefix = Prefix.IsEmpty() ? TEXT("codex_auth") : Prefix;
		const FString ReceiptPath = FPaths::Combine(GetCodexAuthDiagnosticsRootDir(), FString::Printf(TEXT("%s_%s.json"), *SafePrefix, *Timestamp));

		TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		RootObject->SetStringField(TEXT("result_state"), ResultState);
		RootObject->SetStringField(TEXT("detail"), SanitizeSingleLineDiagnostic(Detail));
		RootObject->SetStringField(TEXT("configured_codex_home"), Diagnostics.ConfiguredCodexHomePath);
		RootObject->SetStringField(TEXT("effective_codex_home"), Diagnostics.EffectiveCodexHomePath);
		RootObject->SetStringField(TEXT("home_resolution_source"), Diagnostics.CodexHomeResolutionSource);
		RootObject->SetStringField(TEXT("credential_artifact_path"), Diagnostics.CredentialArtifactPath);
		RootObject->SetStringField(TEXT("auth_mode"), Diagnostics.AuthMode);
		RootObject->SetStringField(TEXT("auth_state_before_probe"), Diagnostics.AuthState);
		RootObject->SetStringField(TEXT("effective_auth_path"), Diagnostics.EffectiveAuthEntryPath);
		RootObject->SetStringField(TEXT("ownership"), Diagnostics.AuthOwnershipModel);
		RootObject->SetStringField(TEXT("profile"), Diagnostics.ProfileLabel);
		RootObject->SetStringField(TEXT("model"), Diagnostics.ModelName);
		RootObject->SetStringField(TEXT("work_mode"), Diagnostics.WorkModeName);
		RootObject->SetStringField(TEXT("reasoning_effort"), Diagnostics.ReasoningEffortName);
		RootObject->SetStringField(TEXT("verbosity"), Diagnostics.VerbosityName);
		RootObject->SetBoolField(TEXT("persistent_app_server_enabled"), Diagnostics.bPersistentAppServerEnabled);
		RootObject->SetBoolField(TEXT("credential_artifact_present"), Diagnostics.bCredentialArtifactPresent);

		FString JsonString;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonString);
		if (FJsonSerializer::Serialize(RootObject, Writer))
		{
			FFileHelper::SaveStringToFile(JsonString, *ReceiptPath);
		}

		return ReceiptPath;
	}
}

FCodexCliRunner::FCodexCliRunner()
	: Thread(nullptr)
	, bIsExecuting(false)
	, ReadPipe(nullptr)
	, WritePipe(nullptr)
	, StdInReadPipe(nullptr)
	, StdInWritePipe(nullptr)
{
}

FCodexCliRunner::~FCodexCliRunner()
{
	StopTaskCounter.Set(1);

	if (Thread)
	{
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}

	CleanupHandles();
	CleanupPersistentAppServer(true);
}

FString FCodexCliRunner::GetBackendDisplayName() const
{
	return FString::Printf(TEXT("Codex CLI [%s]"), *GetCodexProfileLabel());
}

FAgentBackendCapabilities FCodexCliRunner::GetCapabilities() const
{
	FAgentBackendCapabilities Capabilities;
	Capabilities.Backend = EOsvayderUEProviderBackend::CodexCli;
	Capabilities.DisplayName = TEXT("Codex CLI");
	Capabilities.bSupportsStreamingEvents = true;
	Capabilities.bSupportsImages = true;
	Capabilities.bSupportsCancellation = true;
	Capabilities.bSupportsToolAllowList = false;
	Capabilities.bUsesStructuredOutput = true;
	Capabilities.bSupportsBrowserVerifyLogin = true;
	Capabilities.bSupportsProviderPersistentThreads = true;
	Capabilities.bSupportsReasoningEffortControl = true;
	Capabilities.bSupportsVerbosityControl = true;
	Capabilities.bSupportsSpeedModeControl = true;
	Capabilities.bSupportsProfileSelection = true;
	Capabilities.bSupportsExplicitAuthModeSelection = true;
	return Capabilities;
}

FAgentBackendStatus FCodexCliRunner::GetStatus() const
{
	FAgentBackendStatus Status;
	Status.Backend = EOsvayderUEProviderBackend::CodexCli;
	Status.DisplayName = GetBackendDisplayName();
	Status.Capabilities = GetCapabilities();
	Status.ExecutablePath = GetCodexPath();
	Status.bAvailable = !Status.ExecutablePath.IsEmpty();

	if (!Status.bAvailable)
	{
		Status.Readiness = EAgentBackendReadiness::NotAvailable;
		Status.AuthState = EAgentBackendAuthState::Unknown;
		Status.bReady = false;
		Status.Detail = FString::Printf(
			TEXT("Codex CLI not found. Install with: npm install -g @openai/codex. Current selection uses %s."),
			*BuildCodexSelectionSummary());
		Status.AuthDetail = TEXT("Authentication cannot be evaluated because executable is missing.");
		return Status;
	}

	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
	const FString SharedHomeResolutionNote = BuildSharedCodexHomeResolutionNote(AuthContext.SharedHomeResolution);
	const FString SharedHomeResolutionSuffix = SharedHomeResolutionNote.IsEmpty()
		? FString()
		: FString::Printf(TEXT(" %s"), *SharedHomeResolutionNote);

	if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
	{
		if (AuthContext.bHasNamedEnvVar)
		{
			Status.AuthState = EAgentBackendAuthState::Authenticated;
			Status.Readiness = EAgentBackendReadiness::Ready;
			Status.bReady = true;
			Status.AuthDetail = FString::Printf(
				TEXT("%s is present in the editor environment and will be forwarded to Codex as OPENAI_API_KEY. Child launches use an isolated CODEX_HOME without CLI auth artifacts."),
				*AuthContext.ApiKeyEnvVarName);
			Status.Detail = FString::Printf(
				TEXT("Codex CLI ready at %s using %s in explicit API mode via %s -> OPENAI_API_KEY with isolated CODEX_HOME."),
				*Status.ExecutablePath,
				*BuildCodexSelectionSummary(),
				*AuthContext.ApiKeyEnvVarName);
		}
		else
		{
			Status.AuthState = EAgentBackendAuthState::NotAuthenticated;
			Status.Readiness = EAgentBackendReadiness::AvailableNotAuthenticated;
			Status.bReady = false;
			Status.AuthDetail = FString::Printf(
				TEXT("%s is not set in the editor environment. This explicit API mode launches Codex with an isolated CODEX_HOME and does not fall back to CLI-managed Codex credentials."),
				*AuthContext.ApiKeyEnvVarName);
			Status.Detail = FString::Printf(
				TEXT("Codex CLI detected at %s using %s, but %s is not set. Set that env var before launching the editor or switch Codex auth mode back to auto."),
				*Status.ExecutablePath,
				*BuildCodexSelectionSummary(),
				*AuthContext.ApiKeyEnvVarName);
		}

		return Status;
	}

	if (AuthContext.Mode == EOsvayderUECodexAuthMode::CliTerminal)
	{
		if (AuthContext.bHasCredentialArtifact)
		{
			Status.AuthState = EAgentBackendAuthState::Unknown;
			Status.Readiness = EAgentBackendReadiness::AvailableAuthUnknown;
			Status.bReady = false;
			Status.AuthDetail = FString::Printf(
				TEXT("Codex credential artifact detected at %s. CLI Terminal mode assumes `codex login` is managed outside Unreal in Codex home at %s; token validity is not probed.%s"),
				*AuthContext.CredentialArtifactPath,
				*AuthContext.SharedCodexHome,
				*SharedHomeResolutionSuffix);
			Status.Detail = FString::Printf(
				TEXT("Codex CLI detected at %s using %s with external-terminal login artifacts present in Codex home at %s. This mode expects login to be managed outside the plugin.%s"),
				*Status.ExecutablePath,
				*BuildCodexSelectionSummary(),
				*AuthContext.SharedCodexHome,
				*SharedHomeResolutionSuffix);
		}
		else
		{
			Status.AuthState = EAgentBackendAuthState::NotAuthenticated;
			Status.Readiness = EAgentBackendReadiness::AvailableNotAuthenticated;
			Status.bReady = false;
			Status.AuthDetail = AuthContext.bHasInheritedOpenAiApiKey
				? FString::Printf(TEXT("OPENAI_API_KEY is present, but CLI Terminal mode ignores env-var auth and expects external `codex login` in Codex home at %s.%s"), *AuthContext.SharedCodexHome, *SharedHomeResolutionSuffix)
				: FString::Printf(TEXT("No known codex credential artifact detected. CLI Terminal mode expects external `codex login` in Codex home at %s.%s"), *AuthContext.SharedCodexHome, *SharedHomeResolutionSuffix);
			Status.Detail = FString::Printf(
				TEXT("Codex CLI detected at %s using %s, but CLI Terminal mode has no Codex login artifacts yet in Codex home at %s. Run `codex login` in a terminal or switch to Browser Verify to launch browser login from the plugin.%s"),
				*Status.ExecutablePath,
				*BuildCodexSelectionSummary(),
				*AuthContext.SharedCodexHome,
				*SharedHomeResolutionSuffix);
		}

		return Status;
	}

	if (AuthContext.Mode == EOsvayderUECodexAuthMode::BrowserVerify)
	{
		const bool bBrowserLoginPending = HasPendingBrowserVerifyLogin();
		if (AuthContext.bHasCredentialArtifact)
		{
			Status.AuthState = EAgentBackendAuthState::Unknown;
			Status.Readiness = EAgentBackendReadiness::AvailableAuthUnknown;
			Status.bReady = false;
			Status.AuthDetail = FString::Printf(
				TEXT("Codex credential artifact detected at %s. Browser Verify mode keeps credentials in isolated plugin-owned CODEX_HOME at %s; use the Browser Verify button if you want the plugin to relaunch the browser login flow."),
				*AuthContext.CredentialArtifactPath,
				*AuthContext.IsolatedCodexHome);
			Status.Detail = FString::Printf(
				TEXT("Codex CLI detected at %s using %s with Browser Verify selected. Existing plugin-owned browser-login artifacts are present under the isolated Browser Verify home; the plugin can request a fresh browser login URL from UI if needed."),
				*Status.ExecutablePath,
				*BuildCodexSelectionSummary());
		}
		else if (bBrowserLoginPending)
		{
			Status.AuthState = EAgentBackendAuthState::NotAuthenticated;
			Status.Readiness = EAgentBackendReadiness::AvailableNotAuthenticated;
			Status.bReady = false;
			Status.AuthDetail = FString::Printf(
				TEXT("Browser Verify already launched a Codex browser login URL. Finish the ChatGPT sign-in in your browser; credentials will be stored in isolated plugin-owned CODEX_HOME at %s."),
				*AuthContext.IsolatedCodexHome);
			Status.Detail = FString::Printf(
				TEXT("Codex CLI detected at %s using %s, and Browser Verify has already started a plugin-opened browser login flow. Complete the browser flow, then return to Unreal and retry."),
				*Status.ExecutablePath,
				*BuildCodexSelectionSummary());
		}
		else
		{
			Status.AuthState = EAgentBackendAuthState::NotAuthenticated;
			Status.Readiness = EAgentBackendReadiness::AvailableNotAuthenticated;
			Status.bReady = false;
			Status.AuthDetail = AuthContext.bHasInheritedOpenAiApiKey
				? FString::Printf(TEXT("OPENAI_API_KEY is present, but Browser Verify mode intentionally uses isolated plugin-owned browser login in CODEX_HOME at %s instead of env-var auth."), *AuthContext.IsolatedCodexHome)
				: FString::Printf(TEXT("No known codex credential artifact detected yet. Browser Verify mode expects the plugin to request a browser login URL from Codex and open it for you, storing the resulting session in isolated CODEX_HOME at %s."), *AuthContext.IsolatedCodexHome);
			Status.Detail = FString::Printf(
				TEXT("Codex CLI detected at %s using %s, but Browser Verify login has not completed yet. Use the Browser Verify button in the Osvayder UE widget or run `OsvayderUE.LaunchCodexBrowserVerify`."),
				*Status.ExecutablePath,
				*BuildCodexSelectionSummary());
		}

		return Status;
	}

	if (AuthContext.bHasInheritedOpenAiApiKey)
	{
		Status.AuthState = EAgentBackendAuthState::Authenticated;
		Status.Readiness = EAgentBackendReadiness::Ready;
		Status.bReady = true;
		Status.AuthDetail = TEXT("OPENAI_API_KEY is present in the environment.");
		Status.Detail = FString::Printf(
			TEXT("Codex CLI ready at %s using %s (auth via OPENAI_API_KEY)."),
			*Status.ExecutablePath,
			*BuildCodexSelectionSummary());
	}
	else if (AuthContext.bHasCredentialArtifact)
	{
		Status.AuthState = EAgentBackendAuthState::Unknown;
		Status.Readiness = EAgentBackendReadiness::AvailableAuthUnknown;
		Status.bReady = false;
		Status.AuthDetail = FString::Printf(
			TEXT("Codex credential artifact detected at %s (artifact presence only, token validity not probed; live runs use Codex home at %s).%s"),
			*AuthContext.CredentialArtifactPath,
			*AuthContext.SharedCodexHome,
			*SharedHomeResolutionSuffix);
		Status.Detail = FString::Printf(
			TEXT("Codex CLI detected at %s using %s with CLI-managed credential artifacts present in Codex home at %s. Auth readiness is unconfirmed until a live prompt succeeds.%s"),
			*Status.ExecutablePath,
			*BuildCodexSelectionSummary(),
			*AuthContext.SharedCodexHome,
			*SharedHomeResolutionSuffix);
	}
	else
	{
		Status.AuthState = EAgentBackendAuthState::NotAuthenticated;
		Status.Readiness = EAgentBackendReadiness::AvailableNotAuthenticated;
		Status.bReady = false;
		Status.AuthDetail = SharedHomeResolutionNote.IsEmpty()
			? TEXT("No OPENAI_API_KEY and no known codex credential artifact detected.")
			: FString::Printf(TEXT("No OPENAI_API_KEY and no known codex credential artifact detected.%s"), *SharedHomeResolutionSuffix);
		Status.Detail = FString::Printf(
			TEXT("Codex CLI detected at %s using %s, but credentials were not detected. Run `codex login` or set OPENAI_API_KEY.%s"),
			*Status.ExecutablePath,
			*BuildCodexSelectionSummary(),
			*SharedHomeResolutionSuffix);
	}

	return Status;
}

FString FCodexCliRunner::GetConfiguredAuthModeName()
{
	return ResolveCodexAuthContext().ModeName;
}

FString FCodexCliRunner::GetConfiguredApiKeyEnvVarName()
{
	return ResolveCodexAuthContext().ApiKeyEnvVarName;
}

FString FCodexCliRunner::GetConfiguredCodexHomePath()
{
	return ResolveConfiguredSharedCodexHome().ConfiguredHomePath;
}

FString FCodexCliRunner::GetConfiguredCodexHomeResolutionSource()
{
	return ResolveConfiguredSharedCodexHome().ResolutionSource;
}

FString FCodexCliRunner::GetMachineStandardCodexHomePath()
{
	return ResolveConfiguredSharedCodexHome().MachineStandardHome;
}

bool FCodexCliRunner::IsConfiguredCodexHomeArtifactPresent()
{
	return ResolveConfiguredSharedCodexHome().bArtifactPresentInConfiguredHome;
}

TArray<FString> FCodexCliRunner::GetDetectedCodexCandidateHomes()
{
	return ResolveConfiguredSharedCodexHome().DetectedCandidateHomes;
}

FString FCodexCliRunner::GetDetectedCodexArtifactHomePath()
{
	return ResolveConfiguredSharedCodexHome().DetectedArtifactHome;
}

bool FCodexCliRunner::HasExplicitCodexHomeOverride()
{
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	return Settings && Settings->HasExplicitCodexHomeOverride();
}

FString FCodexCliRunner::GetConfiguredSpeedModeName()
{
	return GetConfiguredCodexSpeedMode();
}

FString FCodexCliRunner::GetEffectiveSpeedModeName()
{
	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
	return ResolveCodexSpeedModeState(GetConfiguredCodexSpeedMode(), AuthContext).EffectiveMode;
}

FString FCodexCliRunner::GetSpeedModeSupportLabel()
{
	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
	return ResolveCodexSpeedModeState(GetConfiguredCodexSpeedMode(), AuthContext).SupportLabel;
}

FString FCodexCliRunner::GetEffectiveAuthEntryPath()
{
	return ResolveCodexAuthContext().EffectivePath;
}

FString FCodexCliRunner::GetEffectiveAuthOwnershipModel()
{
	return ResolveCodexAuthContext().OwnershipModel;
}

FString FCodexCliRunner::GetEffectiveCodexHomePathForLaunch()
{
	return ResolveEffectiveCodexHomePathForLaunchNoPrepare(ResolveCodexAuthContext());
}

FCodexCliRunner::FCodexAuthDiagnostics FCodexCliRunner::GetAuthDiagnostics()
{
	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
	const FString EffectiveHomePath = ResolveEffectiveCodexHomePathForLaunchNoPrepare(AuthContext);
	FString CredentialArtifactPath;
	const bool bHasCredentialArtifact = TryFindCodexCredentialArtifactInHome(EffectiveHomePath, CredentialArtifactPath);
	const FCodexSpeedModeState SpeedState = ResolveCodexSpeedModeState(GetConfiguredCodexSpeedMode(), AuthContext);

	FCodexAuthDiagnostics Diagnostics;
	Diagnostics.ConfiguredCodexHomePath = AuthContext.SharedCodexHome;
	Diagnostics.EffectiveCodexHomePath = EffectiveHomePath;
	Diagnostics.CodexHomeResolutionSource = AuthContext.SharedHomeResolution.ResolutionSource;
	Diagnostics.CredentialArtifactPath = CredentialArtifactPath;
	Diagnostics.AuthMode = AuthContext.ModeName;
	Diagnostics.EffectiveAuthEntryPath = AuthContext.EffectivePath;
	Diagnostics.AuthOwnershipModel = AuthContext.OwnershipModel;
	Diagnostics.ProfileLabel = GetCodexProfileLabel();
	Diagnostics.ModelName = GetConfiguredCodexModel();
	Diagnostics.RequestedSpeedMode = SpeedState.RequestedMode;
	Diagnostics.EffectiveSpeedMode = SpeedState.EffectiveMode;
	Diagnostics.SpeedSupportLabel = SpeedState.SupportLabel;
	Diagnostics.WorkModeName = GetConfiguredCodexWorkMode();
	Diagnostics.ReasoningEffortName = GetConfiguredCodexReasoningEffort();
	Diagnostics.VerbosityName = GetConfiguredCodexVerbosity();
	Diagnostics.ExecutablePath = GetCodexPath();
	Diagnostics.bExecutableAvailable = !Diagnostics.ExecutablePath.IsEmpty();
	Diagnostics.bCredentialArtifactPresent = bHasCredentialArtifact;
	Diagnostics.bPersistentAppServerEnabled = ShouldUsePersistentConversationTransport();

	if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
	{
		Diagnostics.AuthState = AuthContext.bHasNamedEnvVar ? TEXT("unknown_unprobed") : TEXT("missing");
	}
	else if (AuthContext.Mode == EOsvayderUECodexAuthMode::Auto && AuthContext.bHasInheritedOpenAiApiKey)
	{
		Diagnostics.AuthState = TEXT("unknown_unprobed");
	}
	else if (bHasCredentialArtifact)
	{
		Diagnostics.AuthState = TEXT("unknown_unprobed");
	}
	else
	{
		Diagnostics.AuthState = TEXT("missing");
	}

	Diagnostics.AuthDetailText = BuildCodexAuthDetailForDiagnostics(AuthContext, EffectiveHomePath, CredentialArtifactPath);
	return Diagnostics;
}

FString FCodexCliRunner::BuildAuthDiagnosticsCompactText(const FCodexAuthDiagnostics& Diagnostics)
{
	const FString ArtifactLabel = Diagnostics.CredentialArtifactPath.IsEmpty()
		? TEXT("none")
		: Diagnostics.CredentialArtifactPath;

	return FString::Printf(
		TEXT("Codex auth: state=%s | CODEX_HOME=%s | artifact=%s | profile=%s | model=%s | work=%s | app-server=%s"),
		*Diagnostics.AuthState,
		Diagnostics.EffectiveCodexHomePath.IsEmpty() ? TEXT("unresolved") : *Diagnostics.EffectiveCodexHomePath,
		*ArtifactLabel,
		*Diagnostics.ProfileLabel,
		Diagnostics.ModelName.IsEmpty() ? TEXT("default") : *Diagnostics.ModelName,
		*Diagnostics.WorkModeName,
		Diagnostics.bPersistentAppServerEnabled ? TEXT("enabled") : TEXT("disabled"));
}

FString FCodexCliRunner::BuildAuthDiagnosticsToolTip(const FCodexAuthDiagnostics& Diagnostics)
{
	FString ToolTip;
	ToolTip += FString::Printf(TEXT("auth_state = %s\n"), *Diagnostics.AuthState);
	ToolTip += FString::Printf(TEXT("auth_detail = %s\n"), *SanitizeSingleLineDiagnostic(Diagnostics.AuthDetailText));
	ToolTip += FString::Printf(TEXT("auth_mode = %s\n"), *Diagnostics.AuthMode);
	ToolTip += FString::Printf(TEXT("configured_codex_home = %s\n"), Diagnostics.ConfiguredCodexHomePath.IsEmpty() ? TEXT("unresolved") : *Diagnostics.ConfiguredCodexHomePath);
	ToolTip += FString::Printf(TEXT("effective_codex_home = %s\n"), Diagnostics.EffectiveCodexHomePath.IsEmpty() ? TEXT("unresolved") : *Diagnostics.EffectiveCodexHomePath);
	ToolTip += FString::Printf(TEXT("home_resolution_source = %s\n"), *Diagnostics.CodexHomeResolutionSource);
	ToolTip += FString::Printf(TEXT("credential_artifact_path = %s\n"), Diagnostics.CredentialArtifactPath.IsEmpty() ? TEXT("none") : *Diagnostics.CredentialArtifactPath);
	ToolTip += FString::Printf(TEXT("effective_auth_path = %s\n"), *Diagnostics.EffectiveAuthEntryPath);
	ToolTip += FString::Printf(TEXT("ownership = %s\n"), *Diagnostics.AuthOwnershipModel);
	ToolTip += FString::Printf(TEXT("profile = %s\n"), *Diagnostics.ProfileLabel);
	ToolTip += FString::Printf(TEXT("model = %s\n"), Diagnostics.ModelName.IsEmpty() ? TEXT("default") : *Diagnostics.ModelName);
	ToolTip += FString::Printf(TEXT("speed_requested = %s\n"), *Diagnostics.RequestedSpeedMode);
	ToolTip += FString::Printf(TEXT("speed_effective = %s\n"), *Diagnostics.EffectiveSpeedMode);
	ToolTip += FString::Printf(TEXT("speed_support = %s\n"), *Diagnostics.SpeedSupportLabel);
	ToolTip += FString::Printf(TEXT("work_mode = %s\n"), *Diagnostics.WorkModeName);
	ToolTip += FString::Printf(TEXT("reasoning = %s\n"), *Diagnostics.ReasoningEffortName);
	ToolTip += FString::Printf(TEXT("verbosity = %s\n"), *Diagnostics.VerbosityName);
	ToolTip += FString::Printf(TEXT("persistent_app_server = %s\n"), Diagnostics.bPersistentAppServerEnabled ? TEXT("enabled") : TEXT("disabled"));
	ToolTip += FString::Printf(TEXT("executable = %s"), Diagnostics.ExecutablePath.IsEmpty() ? TEXT("not_found") : *Diagnostics.ExecutablePath);
	if (!Diagnostics.ProbeDetailText.IsEmpty())
	{
		ToolTip += FString::Printf(TEXT("\nprobe_detail = %s"), *SanitizeSingleLineDiagnostic(Diagnostics.ProbeDetailText));
	}
	return ToolTip;
}

FString FCodexCliRunner::ClassifyAuthFailureMessage(const FString& Message)
{
	const FString Normalized = Message.ToLower();
	if (Normalized.IsEmpty())
	{
		return TEXT("unknown_unprobed");
	}

	if (Normalized.Contains(TEXT("access token could not be refreshed"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("refresh token was already used"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("refresh token"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("invalid_grant"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("expired"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("token refresh"), ESearchCase::CaseSensitive))
	{
		return TEXT("expired_or_refresh_failed");
	}

	if (Normalized.Contains(TEXT("not logged in"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("login required"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("no credential"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("missing credential"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("authentication required"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("not authenticated"), ESearchCase::CaseSensitive))
	{
		return TEXT("missing");
	}

	if (Normalized.Contains(TEXT("transport_broken"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("failed to launch"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("stdin pipe"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("stdout pipe"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("timed out waiting for codex"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("exited before"), ESearchCase::CaseSensitive))
	{
		return TEXT("transport_broken");
	}

	if (Normalized.Contains(TEXT("success"), ESearchCase::CaseSensitive) ||
		Normalized.Contains(TEXT("probe ok"), ESearchCase::CaseSensitive))
	{
		return TEXT("ok");
	}

	return TEXT("unknown_unprobed");
}

bool FCodexCliRunner::ShouldUsePersistentConversationTransport()
{
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	return Settings && Settings->ShouldUsePersistentCodexAppServer();
}

FString FCodexCliRunner::ClassifyPersistentTransportFailureMessage(const FString& Message)
{
	const FString Normalized = Message.ToLower();
	if (Normalized.IsEmpty())
	{
		return FString();
	}

	const bool bMentionsPersistentAppServer =
		Normalized.Contains(TEXT("persistent app-server"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("app-server session"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("backend-api/codex/responses"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("persistent transport"), ESearchCase::CaseSensitive);

	if (Normalized.Contains(TEXT("timed out waiting for codex persistent app-server"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("timed out waiting for codex persistent app-server json-rpc result"), ESearchCase::CaseSensitive))
	{
		return TEXT("persistent_transport_timeout");
	}

	if (bMentionsPersistentAppServer
		&& (Normalized.Contains(TEXT("wss://"), ESearchCase::CaseSensitive)
			|| Normalized.Contains(TEXT("websocket"), ESearchCase::CaseSensitive)
			|| Normalized.Contains(TEXT("os error 10061"), ESearchCase::CaseSensitive)
			|| Normalized.Contains(TEXT("connection refused"), ESearchCase::CaseSensitive)
			|| Normalized.Contains(TEXT("connection reset"), ESearchCase::CaseSensitive)
			|| Normalized.Contains(TEXT("response stream"), ESearchCase::CaseSensitive)))
	{
		return TEXT("persistent_transport_websocket_reset");
	}

	if (Normalized.Contains(TEXT("persistent transport was reset"), ESearchCase::CaseSensitive))
	{
		return TEXT("persistent_transport_reset");
	}

	if (Normalized.Contains(TEXT("persistent app-server exited unexpectedly"), ESearchCase::CaseSensitive))
	{
		return TEXT("persistent_transport_process_exit");
	}

	if (Normalized.Contains(TEXT("stdin pipe is not available"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("failed to write codex app-server request to stdin"), ESearchCase::CaseSensitive))
	{
		return TEXT("persistent_transport_write_failure");
	}

	if (Normalized.Contains(TEXT("codex persistent app-server error:"), ESearchCase::CaseSensitive)
		&& bMentionsPersistentAppServer)
	{
		return TEXT("persistent_transport_app_server_error");
	}

	return FString();
}

bool FCodexCliRunner::IsPersistentTransportFailureMessage(const FString& Message)
{
	return !ClassifyPersistentTransportFailureMessage(Message).IsEmpty();
}

bool FCodexCliRunner::CanLaunchBrowserVerify(FString& OutReason)
{
	if (!IsCodexAvailable())
	{
		OutReason = TEXT("Codex CLI is not available. Install it before using Browser Verify.");
		return false;
	}

	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	if (!Settings)
	{
		OutReason = TEXT("Osvayder UE settings are not available.");
		return false;
	}

	if (Settings->GetCodexAuthMode() != EOsvayderUECodexAuthMode::BrowserVerify)
	{
		OutReason = FString::Printf(
			TEXT("Current Codex auth mode is `%s`. Switch to `browser_verify` to enable plugin-opened browser login."),
			*Settings->GetConfiguredCodexAuthModeName());
		return false;
	}

	FString PendingAuthUrl;
	if (TryGetPendingBrowserVerifyAuthUrl(PendingAuthUrl))
	{
		OutReason = TEXT("Browser Verify already has a pending browser login URL. Click again to reopen it in your browser.");
		return true;
	}

	OutReason = TEXT("Launch Codex Browser Verify by requesting a browser login URL from `codex app-server` and opening it from the Osvayder UE UI.");
	return true;
}

bool FCodexCliRunner::OpenEffectiveCodexAuthFolder(FString& OutStatusMessage)
{
	const FCodexAuthDiagnostics Diagnostics = GetAuthDiagnostics();
	if (Diagnostics.EffectiveCodexHomePath.IsEmpty())
	{
		OutStatusMessage = TEXT("Effective Codex home could not be resolved.");
		return false;
	}

	if (!IFileManager::Get().DirectoryExists(*Diagnostics.EffectiveCodexHomePath) &&
		!IFileManager::Get().MakeDirectory(*Diagnostics.EffectiveCodexHomePath, true))
	{
		OutStatusMessage = FString::Printf(TEXT("Failed to create/open effective Codex home at %s."), *Diagnostics.EffectiveCodexHomePath);
		return false;
	}

	FPlatformProcess::ExploreFolder(*Diagnostics.EffectiveCodexHomePath);
	OutStatusMessage = FString::Printf(TEXT("Opened effective Codex auth folder: %s"), *Diagnostics.EffectiveCodexHomePath);
	return true;
}

bool FCodexCliRunner::LaunchCodexRelogin(FString& OutStatusMessage)
{
	if (!IsCodexAvailable())
	{
		OutStatusMessage = TEXT("Codex CLI is not available. Install it before relogin.");
		return false;
	}

	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
	if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
	{
		OutStatusMessage = FString::Printf(
			TEXT("Relogin Codex is not used for explicit API env-var mode. Set or rotate %s in the editor environment instead."),
			*AuthContext.ApiKeyEnvVarName);
		return false;
	}

	FString EffectiveHomePath;
	FString HomeDiagnostic;
	if (!ResolveEffectiveCodexHomePathForLaunch(AuthContext, true, EffectiveHomePath, HomeDiagnostic))
	{
		OutStatusMessage = HomeDiagnostic;
		return false;
	}

	FString PendingAuthUrl;
	FString PendingHomePath;
	if (TryGetPendingBrowserVerifyLogin(PendingAuthUrl, PendingHomePath))
	{
		FString LaunchDiagnostic;
		if (!TryLaunchBrowserUrl(PendingAuthUrl, LaunchDiagnostic))
		{
			OutStatusMessage = LaunchDiagnostic;
			return false;
		}

		OutStatusMessage = FString::Printf(
			TEXT("Reopened pending Codex relogin URL. Pending login is using CODEX_HOME `%s`."),
			PendingHomePath.IsEmpty() ? TEXT("unknown") : *PendingHomePath);
		return true;
	}

	FProcHandle AppServerHandle;
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	void* StdInReadPipe = nullptr;
	void* StdInWritePipe = nullptr;

	FString PipeDiagnostic;
	if (!TryCreateBrowserVerifyPipes(ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, PipeDiagnostic))
	{
		OutStatusMessage = PipeDiagnostic;
		return false;
	}

	FString Executable;
	FString Args;
	FString LaunchDiagnostic;
	if (!TryBuildCodexAppServerLaunchSpec(
		EffectiveHomePath,
		UOsvayderUESettings::Get() && UOsvayderUESettings::Get()->ShouldClearProxyEnvForBrowserVerify(),
		Executable,
		Args,
		LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, false);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	const FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	AppServerHandle = FPlatformProcess::CreateProc(
		*Executable,
		*Args,
		true,
		true,
		true,
		nullptr,
		0,
		*WorkingDir,
		WritePipe,
		StdInReadPipe);

	if (!AppServerHandle.IsValid())
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, false);
		OutStatusMessage = FString::Printf(TEXT("Failed to launch Codex app-server for relogin using `%s %s`."), *Executable, *Args);
		return false;
	}

	TSharedRef<FJsonObject> InitializeParams = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
	ClientInfo->SetStringField(TEXT("name"), TEXT("osvayderue_relogin"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("1.0"));
	InitializeParams->SetObjectField(TEXT("clientInfo"), ClientInfo);

	if (!TryWriteJsonRpcRequest(StdInWritePipe, 1, CodexAppServerInitializeMethod, InitializeParams, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	FString ResponseBuffer;
	TSharedPtr<FJsonObject> InitializeResult;
	if (!TryWaitForJsonRpcResult(ReadPipe, AppServerHandle, ResponseBuffer, 1, CodexBrowserVerifyStartupTimeoutMs, InitializeResult, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	TSharedRef<FJsonObject> LoginParams = MakeShared<FJsonObject>();
	LoginParams->SetStringField(TEXT("type"), TEXT("chatgpt"));
	if (!TryWriteJsonRpcRequest(StdInWritePipe, 2, CodexAppServerLoginStartMethod, LoginParams, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	TSharedPtr<FJsonObject> LoginResult;
	if (!TryWaitForJsonRpcResult(ReadPipe, AppServerHandle, ResponseBuffer, 2, CodexBrowserVerifyStartupTimeoutMs, LoginResult, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	FString AuthUrl;
	FString LoginId;
	if (!LoginResult.IsValid() ||
		!LoginResult->TryGetStringField(TEXT("authUrl"), AuthUrl) ||
		!LoginResult->TryGetStringField(TEXT("loginId"), LoginId) ||
		AuthUrl.IsEmpty() ||
		LoginId.IsEmpty())
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = TEXT("Codex relogin did not return the expected authUrl/loginId response.");
		return false;
	}

	if (!TryLaunchBrowserUrl(AuthUrl, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	{
		FScopeLock Lock(&GCodexBrowserVerifySession.Mutex);
		GCodexBrowserVerifySession.ProcessHandle = AppServerHandle;
		GCodexBrowserVerifySession.ReadPipe = ReadPipe;
		GCodexBrowserVerifySession.WritePipe = WritePipe;
		GCodexBrowserVerifySession.StdInReadPipe = StdInReadPipe;
		GCodexBrowserVerifySession.StdInWritePipe = StdInWritePipe;
		GCodexBrowserVerifySession.LoginId = LoginId;
		GCodexBrowserVerifySession.AuthUrl = AuthUrl;
		GCodexBrowserVerifySession.CredentialHomePath = EffectiveHomePath;
		GCodexBrowserVerifySession.bPendingLogin = true;
	}

	StartBrowserVerifyMonitorThread();

	OutStatusMessage = FString::Printf(
		TEXT("Opened Codex relogin in your browser. This relogin uses the same effective CODEX_HOME as plugin launches: `%s`."),
		*EffectiveHomePath);
	UE_LOG(LogOsvayderUE, Log, TEXT("%s"), *OutStatusMessage);
	return true;
}

bool FCodexCliRunner::BackupAndClearStaleAuthArtifacts(FString& OutStatusMessage)
{
	const FCodexAuthDiagnostics Diagnostics = GetAuthDiagnostics();
	if (Diagnostics.EffectiveCodexHomePath.IsEmpty())
	{
		OutStatusMessage = TEXT("Effective Codex home could not be resolved; no auth artifacts were changed.");
		return false;
	}

	const TArray<FString> ArtifactPaths = GetKnownCodexCredentialArtifactPathsInHome(Diagnostics.EffectiveCodexHomePath);
	if (ArtifactPaths.Num() == 0)
	{
		OutStatusMessage = FString::Printf(TEXT("No known Codex auth artifacts found under %s; nothing was cleared."), *Diagnostics.EffectiveCodexHomePath);
		return false;
	}

	const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString BackupDir = FPaths::Combine(GetCodexAuthDiagnosticsRootDir(), FString::Printf(TEXT("AuthBackup_%s"), *Timestamp));
	if (!IFileManager::Get().MakeDirectory(*BackupDir, true))
	{
		OutStatusMessage = FString::Printf(TEXT("Failed to create Codex auth backup directory at %s; no auth artifacts were changed."), *BackupDir);
		return false;
	}

	TArray<FString> BackupPaths;
	for (const FString& ArtifactPath : ArtifactPaths)
	{
		const FString BackupPath = FPaths::Combine(BackupDir, FPaths::GetCleanFilename(ArtifactPath));
		if (IFileManager::Get().Copy(*BackupPath, *ArtifactPath, true, true) != COPY_OK)
		{
			OutStatusMessage = FString::Printf(
				TEXT("Failed to back up %s to %s; no auth artifacts were deleted."),
				*ArtifactPath,
				*BackupPath);
			return false;
		}
		BackupPaths.Add(BackupPath);
	}

	for (const FString& ArtifactPath : ArtifactPaths)
	{
		if (!IFileManager::Get().Delete(*ArtifactPath, false, true, true))
		{
			OutStatusMessage = FString::Printf(
				TEXT("Backups were created under %s, but failed to delete %s. Check permissions before retrying."),
				*BackupDir,
				*ArtifactPath);
			return false;
		}
	}

	TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
	Manifest->SetStringField(TEXT("effective_codex_home"), Diagnostics.EffectiveCodexHomePath);
	Manifest->SetStringField(TEXT("auth_mode"), Diagnostics.AuthMode);
	TArray<TSharedPtr<FJsonValue>> ArtifactsJson;
	for (const FString& ArtifactPath : ArtifactPaths)
	{
		ArtifactsJson.Add(MakeShared<FJsonValueString>(ArtifactPath));
	}
	Manifest->SetArrayField(TEXT("cleared_artifacts"), ArtifactsJson);
	TArray<TSharedPtr<FJsonValue>> BackupsJson;
	for (const FString& BackupPath : BackupPaths)
	{
		BackupsJson.Add(MakeShared<FJsonValueString>(BackupPath));
	}
	Manifest->SetArrayField(TEXT("backup_paths"), BackupsJson);

	FString ManifestJson;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ManifestJson);
	if (FJsonSerializer::Serialize(Manifest, Writer))
	{
		FFileHelper::SaveStringToFile(ManifestJson, *FPaths::Combine(BackupDir, TEXT("manifest.json")));
	}

	OutStatusMessage = FString::Printf(
		TEXT("Backed up and cleared %d Codex auth artifact(s). Backup directory: %s. Effective CODEX_HOME: %s"),
		ArtifactPaths.Num(),
		*BackupDir,
		*Diagnostics.EffectiveCodexHomePath);
	return true;
}

bool FCodexCliRunner::ProbeBackendAuth(FString& OutStatusMessage)
{
	FCodexAuthDiagnostics Diagnostics = GetAuthDiagnostics();
	Diagnostics.bProbePerformed = true;

	auto FinishProbe = [&OutStatusMessage, &Diagnostics](const FString& ResultState, const FString& Detail, const bool bSuccess)
	{
		const FString ReceiptPath = WriteCodexAuthDiagnosticReceipt(TEXT("codex_auth_probe"), Diagnostics, ResultState, Detail);
		OutStatusMessage = FString::Printf(
			TEXT("Codex auth probe: state=%s; detail=%s; receipt=%s"),
			*ResultState,
			*SanitizeSingleLineDiagnostic(Detail),
			*ReceiptPath);
		return bSuccess;
	};

	if (!Diagnostics.bExecutableAvailable)
	{
		return FinishProbe(TEXT("unknown_unprobed"), TEXT("transport_broken: Codex CLI executable could not be resolved."), false);
	}

	if (Diagnostics.AuthState.Equals(TEXT("missing"), ESearchCase::IgnoreCase))
	{
		return FinishProbe(TEXT("missing"), Diagnostics.AuthDetailText, false);
	}

	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
	FString EffectiveHomePath;
	FString HomeDiagnostic;
	if (!ResolveEffectiveCodexHomePathForLaunch(AuthContext, true, EffectiveHomePath, HomeDiagnostic))
	{
		return FinishProbe(TEXT("unknown_unprobed"), FString::Printf(TEXT("transport_broken: %s"), *HomeDiagnostic), false);
	}

	FProcHandle AppServerHandle;
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	void* StdInReadPipe = nullptr;
	void* StdInWritePipe = nullptr;

	FString PipeDiagnostic;
	if (!TryCreateBrowserVerifyPipes(ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, PipeDiagnostic))
	{
		return FinishProbe(TEXT("unknown_unprobed"), FString::Printf(TEXT("transport_broken: %s"), *PipeDiagnostic), false);
	}

	FString Executable;
	FString Args;
	FString LaunchDiagnostic;
	{
		const FString CodexPath = GetCodexPath();
		FCodexEnvSetupOptions EnvOptions = BuildEnvSetupOptionsForAuthContext(AuthContext);
		FString EnvSetup = BuildStandardCodexEnvSetup(EnvOptions);
		if (!EffectiveHomePath.IsEmpty())
		{
			EnvSetup += QuoteCmdEnv(TEXT("CODEX_HOME"), EffectiveHomePath);
		}

		if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
		{
			EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), FString());
			if (AuthContext.bShouldForwardApiKeyToChild)
			{
				EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), AuthContext.ApiKeyEnvVarValue);
			}
		}
		else if (AuthContext.Mode == EOsvayderUECodexAuthMode::BrowserVerify ||
			AuthContext.Mode == EOsvayderUECodexAuthMode::CliTerminal)
		{
			EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), FString());
		}

#if PLATFORM_WINDOWS
		if (!CachedCodexJsPath.IsEmpty() && FPaths::GetCleanFilename(CodexPath).Equals(TEXT("node.exe"), ESearchCase::IgnoreCase))
		{
			Executable = TEXT("cmd.exe");
			Args = FString::Printf(TEXT("/c \"%s\"%s\" \"%s\" app-server --listen stdio:// 2>&1\""), *EnvSetup, *CodexPath, *CachedCodexJsPath);
		}
		else
		{
			FString DirectExecutable = CodexPath;
			if (FPaths::GetExtension(DirectExecutable, true).IsEmpty())
			{
				const FString ExeVariant = DirectExecutable + TEXT(".exe");
				if (IFileManager::Get().FileExists(*ExeVariant))
				{
					DirectExecutable = ExeVariant;
				}
			}

			if (!DirectExecutable.EndsWith(TEXT(".exe"), ESearchCase::IgnoreCase))
			{
				CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, false);
				return FinishProbe(TEXT("unknown_unprobed"), FString::Printf(TEXT("transport_broken: Resolved Codex path `%s` is not directly launchable."), *CodexPath), false);
			}

			Executable = TEXT("cmd.exe");
			Args = FString::Printf(TEXT("/c \"%s\"%s\" app-server --listen stdio:// 2>&1\""), *EnvSetup, *DirectExecutable);
		}
#else
		Executable = CodexPath;
		Args = TEXT("app-server --listen stdio://");
#endif
	}

	const FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	AppServerHandle = FPlatformProcess::CreateProc(
		*Executable,
		*Args,
		true,
		true,
		true,
		nullptr,
		0,
		*WorkingDir,
		WritePipe,
		StdInReadPipe);

	if (!AppServerHandle.IsValid())
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, false);
		return FinishProbe(TEXT("unknown_unprobed"), FString::Printf(TEXT("transport_broken: failed to launch `%s %s`."), *Executable, *Args), false);
	}

	TSharedRef<FJsonObject> InitializeParams = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
	ClientInfo->SetStringField(TEXT("name"), TEXT("osvayderue_auth_probe"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("1.0"));
	InitializeParams->SetObjectField(TEXT("clientInfo"), ClientInfo);

	FString ResponseBuffer;
	TSharedPtr<FJsonObject> InitializeResult;
	if (!TryWriteJsonRpcRequest(StdInWritePipe, 1, CodexAppServerInitializeMethod, InitializeParams, LaunchDiagnostic) ||
		!TryWaitForJsonRpcResult(ReadPipe, AppServerHandle, ResponseBuffer, 1, CodexBrowserVerifyStartupTimeoutMs, InitializeResult, LaunchDiagnostic))
	{
		const FString Classified = ClassifyAuthFailureMessage(LaunchDiagnostic);
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		return FinishProbe(
			Classified.Equals(TEXT("transport_broken"), ESearchCase::IgnoreCase) ? TEXT("unknown_unprobed") : Classified,
			Classified.Equals(TEXT("transport_broken"), ESearchCase::IgnoreCase) ? FString::Printf(TEXT("transport_broken: %s"), *LaunchDiagnostic) : LaunchDiagnostic,
			false);
	}

	if (!TryWriteJsonRpcNotification(StdInWritePipe, CodexAppServerInitializedNotification, MakeShared<FJsonObject>(), LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		return FinishProbe(TEXT("unknown_unprobed"), FString::Printf(TEXT("transport_broken: %s"), *LaunchDiagnostic), false);
	}

	TSharedRef<FJsonObject> ThreadParams = MakeShared<FJsonObject>();
	if (!Diagnostics.ModelName.IsEmpty())
	{
		ThreadParams->SetStringField(TEXT("model"), Diagnostics.ModelName);
	}
	ThreadParams->SetStringField(TEXT("cwd"), WorkingDir);
	ThreadParams->SetStringField(TEXT("approvalPolicy"), TEXT("never"));
	ThreadParams->SetStringField(TEXT("sandbox"), TEXT("read-only"));
	ThreadParams->SetStringField(TEXT("serviceName"), TEXT("osvayderue_auth_probe"));
	ThreadParams->SetBoolField(TEXT("ephemeral"), true);

	TSharedPtr<FJsonObject> ThreadResult;
	if (!TryWriteJsonRpcRequest(StdInWritePipe, 2, CodexAppServerThreadStartMethod, ThreadParams, LaunchDiagnostic) ||
		!TryWaitForJsonRpcResult(ReadPipe, AppServerHandle, ResponseBuffer, 2, CodexBrowserVerifyStartupTimeoutMs, ThreadResult, LaunchDiagnostic))
	{
		const FString Classified = ClassifyAuthFailureMessage(LaunchDiagnostic);
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		return FinishProbe(
			Classified.Equals(TEXT("transport_broken"), ESearchCase::IgnoreCase) ? TEXT("unknown_unprobed") : Classified,
			Classified.Equals(TEXT("transport_broken"), ESearchCase::IgnoreCase) ? FString::Printf(TEXT("transport_broken: %s"), *LaunchDiagnostic) : LaunchDiagnostic,
			false);
	}

	CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
	return FinishProbe(TEXT("ok"), FString::Printf(TEXT("Probe OK: app-server initialized and thread/start succeeded using CODEX_HOME %s."), *EffectiveHomePath), true);
}

#if WITH_DEV_AUTOMATION_TESTS
void FCodexCliRunner::SetTestKnownMachineStandardHomes(const TArray<FString>& InHomes)
{
	GHasTestKnownMachineStandardHomesOverride = true;
	GTestKnownMachineStandardHomes.Reset();
	for (const FString& HomePath : InHomes)
	{
		AddUniqueCodexHomePath(GTestKnownMachineStandardHomes, HomePath);
	}
}

void FCodexCliRunner::ClearTestKnownMachineStandardHomes()
{
	GHasTestKnownMachineStandardHomesOverride = false;
	GTestKnownMachineStandardHomes.Reset();
}

void FCodexCliRunner::SetTestCodexLaunchOverride(const FString& InExecutablePath, const FString& InCodexJsPath)
{
	GHasTestCodexLaunchOverride = true;
	GTestCodexExecutablePath = InExecutablePath;
	GTestCodexJsPath = InCodexJsPath;
	CachedCodexJsPath = InCodexJsPath;
}

void FCodexCliRunner::ClearTestCodexLaunchOverride()
{
	GHasTestCodexLaunchOverride = false;
	GTestCodexExecutablePath.Empty();
	GTestCodexJsPath.Empty();
	CachedCodexJsPath.Empty();
}

FString FCodexCliRunner::GetTestPersistentThreadSandboxMode(const FAgentRequestConfig& Config)
{
	return GetPersistentThreadSandboxMode(Config);
}

TSharedPtr<FJsonObject> FCodexCliRunner::MakeTestPersistentTurnSandboxPolicy(const FAgentRequestConfig& Config)
{
	return BuildPersistentTurnSandboxPolicy(Config);
}

bool FCodexCliRunner::DoesTestConfigRequestUnrealMcpBridge(const FAgentRequestConfig& Config)
{
	return ConfigRequestsUnrealMcpBridge(Config);
}

FString FCodexCliRunner::GetTestRequestedUnrealMcpToolFilterCsv(const FAgentRequestConfig& Config)
{
	return FString::Join(GetRequestedScopedUnrealMcpToolNames(Config), TEXT(","));
}

bool FCodexCliRunner::ShouldTestForceWindowsUnelevatedWorkspaceWriteSandbox(const FAgentRequestConfig& Config)
{
	return ShouldForceWindowsUnelevatedWorkspaceWriteSandbox(Config);
}

int32 FCodexCliRunner::GetTestPersistentRequestTimeoutMs(const FAgentRequestConfig& Config)
{
	return GetPersistentRequestTimeoutMs(Config);
}
#endif

bool FCodexCliRunner::LaunchBrowserVerifyLogin(FString& OutStatusMessage)
{
	FString LaunchReason;
	if (!CanLaunchBrowserVerify(LaunchReason))
	{
		OutStatusMessage = LaunchReason;
		return false;
	}

	const FString CodexPath = GetCodexPath();
	if (CodexPath.IsEmpty())
	{
		OutStatusMessage = TEXT("Codex CLI path could not be resolved.");
		return false;
	}

	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
	FString BrowserVerifyHome = AuthContext.IsolatedCodexHome;
	FString BrowserVerifyHomeDiagnostic;
	if (!PrepareIsolatedBrowserVerifyCodexHome(BrowserVerifyHome, BrowserVerifyHomeDiagnostic))
	{
		OutStatusMessage = BrowserVerifyHomeDiagnostic;
		return false;
	}

	FString PendingAuthUrl;
	if (TryGetPendingBrowserVerifyAuthUrl(PendingAuthUrl))
	{
		FString LaunchDiagnostic;
		if (!TryLaunchBrowserUrl(PendingAuthUrl, LaunchDiagnostic))
		{
			OutStatusMessage = LaunchDiagnostic;
			return false;
		}

		OutStatusMessage = TEXT("Reopened the pending Codex Browser Verify login URL in your browser. Finish the sign-in there, then return to Unreal.");
		UE_LOG(LogOsvayderUE, Log, TEXT("%s"), *OutStatusMessage);
		return true;
	}

	FProcHandle AppServerHandle;
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	void* StdInReadPipe = nullptr;
	void* StdInWritePipe = nullptr;

	FString PipeDiagnostic;
	if (!TryCreateBrowserVerifyPipes(ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, PipeDiagnostic))
	{
		OutStatusMessage = PipeDiagnostic;
		return false;
	}

	FString Executable;
	FString Args;
	FString LaunchDiagnostic;
	if (!TryBuildCodexAppServerLaunchSpec(
		BrowserVerifyHome,
		UOsvayderUESettings::Get() && UOsvayderUESettings::Get()->ShouldClearProxyEnvForBrowserVerify(),
		Executable,
		Args,
		LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, false);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	const FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	AppServerHandle = FPlatformProcess::CreateProc(
		*Executable,
		*Args,
		true,
		true,
		true,
		nullptr,
		0,
		*WorkingDir,
		WritePipe,
		StdInReadPipe);

	if (!AppServerHandle.IsValid())
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, false);
		OutStatusMessage = FString::Printf(TEXT("Failed to launch Codex app-server for Browser Verify using `%s %s`."), *Executable, *Args);
		return false;
	}

	TSharedRef<FJsonObject> InitializeParams = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
	ClientInfo->SetStringField(TEXT("name"), TEXT("osvayderue"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("1.0"));
	InitializeParams->SetObjectField(TEXT("clientInfo"), ClientInfo);

	if (!TryWriteJsonRpcRequest(StdInWritePipe, 1, CodexAppServerInitializeMethod, InitializeParams, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	FString ResponseBuffer;
	TSharedPtr<FJsonObject> InitializeResult;
	if (!TryWaitForJsonRpcResult(ReadPipe, AppServerHandle, ResponseBuffer, 1, CodexBrowserVerifyStartupTimeoutMs, InitializeResult, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	TSharedRef<FJsonObject> LoginParams = MakeShared<FJsonObject>();
	LoginParams->SetStringField(TEXT("type"), TEXT("chatgpt"));
	if (!TryWriteJsonRpcRequest(StdInWritePipe, 2, CodexAppServerLoginStartMethod, LoginParams, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	TSharedPtr<FJsonObject> LoginResult;
	if (!TryWaitForJsonRpcResult(ReadPipe, AppServerHandle, ResponseBuffer, 2, CodexBrowserVerifyStartupTimeoutMs, LoginResult, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	FString AuthUrl;
	FString LoginId;
	if (!LoginResult.IsValid() ||
		!LoginResult->TryGetStringField(TEXT("authUrl"), AuthUrl) ||
		!LoginResult->TryGetStringField(TEXT("loginId"), LoginId) ||
		AuthUrl.IsEmpty() ||
		LoginId.IsEmpty())
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = TEXT("Codex Browser Verify did not return the expected authUrl/loginId response.");
		return false;
	}

	if (!TryLaunchBrowserUrl(AuthUrl, LaunchDiagnostic))
	{
		CloseBrowserVerifyResources(AppServerHandle, ReadPipe, WritePipe, StdInReadPipe, StdInWritePipe, true);
		OutStatusMessage = LaunchDiagnostic;
		return false;
	}

	{
		FScopeLock Lock(&GCodexBrowserVerifySession.Mutex);
		GCodexBrowserVerifySession.ProcessHandle = AppServerHandle;
		GCodexBrowserVerifySession.ReadPipe = ReadPipe;
		GCodexBrowserVerifySession.WritePipe = WritePipe;
		GCodexBrowserVerifySession.StdInReadPipe = StdInReadPipe;
		GCodexBrowserVerifySession.StdInWritePipe = StdInWritePipe;
		GCodexBrowserVerifySession.LoginId = LoginId;
		GCodexBrowserVerifySession.AuthUrl = AuthUrl;
		GCodexBrowserVerifySession.CredentialHomePath = BrowserVerifyHome;
		GCodexBrowserVerifySession.bPendingLogin = true;
	}

	StartBrowserVerifyMonitorThread();

	OutStatusMessage = FString::Printf(
		TEXT("Opened Codex Browser Verify in your browser via `codex app-server`. Finish the ChatGPT sign-in there, then return to Unreal. Browser Verify uses isolated plugin-owned CODEX_HOME at `%s`."),
		*BrowserVerifyHome);
	UE_LOG(LogOsvayderUE, Log, TEXT("%s"), *OutStatusMessage);
	return true;
}

void FCodexCliRunner::CleanupHandles()
{
	if (ReadPipe || WritePipe)
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = nullptr;
		WritePipe = nullptr;
	}

	if (StdInReadPipe || StdInWritePipe)
	{
		FPlatformProcess::ClosePipe(StdInReadPipe, StdInWritePipe);
		StdInReadPipe = nullptr;
		StdInWritePipe = nullptr;
	}

	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::CloseProc(ProcessHandle);
	}
}

bool FCodexCliRunner::IsCodexAvailable()
{
	return !GetCodexPath().IsEmpty();
}

FString FCodexCliRunner::GetCodexPath()
{
	static FString CachedCodexPath;
	static bool bHasSearched = false;

#if WITH_DEV_AUTOMATION_TESTS
	if (GHasTestCodexLaunchOverride)
	{
		CachedCodexJsPath = GTestCodexJsPath;
		return GTestCodexExecutablePath;
	}
#endif

	if (bHasSearched && !CachedCodexPath.IsEmpty())
	{
		return CachedCodexPath;
	}

	bHasSearched = true;

#if PLATFORM_WINDOWS
	FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	if (!AppData.IsEmpty())
	{
		const FString CodexJsPath = FPaths::Combine(AppData, TEXT("npm"), TEXT("node_modules"), TEXT("@openai"), TEXT("codex"), TEXT("bin"), TEXT("codex.js"));
		if (IFileManager::Get().FileExists(*CodexJsPath))
		{
			TArray<FString> NodePaths;
			NodePaths.Add(TEXT("D:\\Program Files\\nodejs\\node.exe"));
			NodePaths.Add(TEXT("C:\\Program Files\\nodejs\\node.exe"));

			FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
			TArray<FString> PathDirs;
			PathEnv.ParseIntoArray(PathDirs, TEXT(";"), true);
			for (const FString& Dir : PathDirs)
			{
				NodePaths.Add(FPaths::Combine(Dir, TEXT("node.exe")));
			}

			for (const FString& NodePath : NodePaths)
			{
				if (IFileManager::Get().FileExists(*NodePath))
				{
					CachedCodexJsPath = CodexJsPath;
					CachedCodexPath = NodePath;
					return CachedCodexPath;
				}
			}
		}
	}

	TArray<FString> PossiblePaths;
	if (!AppData.IsEmpty())
	{
		PossiblePaths.Add(FPaths::Combine(AppData, TEXT("npm"), TEXT("codex.cmd")));
		PossiblePaths.Add(FPaths::Combine(AppData, TEXT("npm"), TEXT("codex")));
	}

	FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (!LocalAppData.IsEmpty())
	{
		PossiblePaths.Add(FPaths::Combine(LocalAppData, TEXT("npm"), TEXT("codex.cmd")));
	}

	FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	TArray<FString> PathDirs;
	PathEnv.ParseIntoArray(PathDirs, TEXT(";"), true);
	for (const FString& Dir : PathDirs)
	{
		PossiblePaths.Add(FPaths::Combine(Dir, TEXT("codex.cmd")));
		PossiblePaths.Add(FPaths::Combine(Dir, TEXT("codex.exe")));
		PossiblePaths.Add(FPaths::Combine(Dir, TEXT("codex")));
	}
#else
	TArray<FString> PossiblePaths;
	FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!Home.IsEmpty())
	{
		PossiblePaths.Add(FPaths::Combine(Home, TEXT(".local"), TEXT("bin"), TEXT("codex")));
	}
	PossiblePaths.Add(TEXT("/usr/local/bin/codex"));
	PossiblePaths.Add(TEXT("/usr/bin/codex"));

	FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	TArray<FString> PathDirs;
	PathEnv.ParseIntoArray(PathDirs, TEXT(":"), true);
	for (const FString& Dir : PathDirs)
	{
		PossiblePaths.Add(FPaths::Combine(Dir, TEXT("codex")));
	}
#endif

	for (const FString& Path : PossiblePaths)
	{
		if (IFileManager::Get().FileExists(*Path))
		{
			CachedCodexPath = Path;
			return CachedCodexPath;
		}
	}

	FString WhereOutput;
	FString WhereErrors;
	int32 ReturnCode = 0;

#if PLATFORM_WINDOWS
	const TCHAR* WhichCmd = TEXT("where");
	const TCHAR* WhichArgs = TEXT("codex");
#else
	const TCHAR* WhichCmd = TEXT("/bin/sh");
	const TCHAR* WhichArgs = TEXT("-c 'which codex 2>/dev/null'");
#endif

	if (FPlatformProcess::ExecProcess(WhichCmd, WhichArgs, &ReturnCode, &WhereOutput, &WhereErrors) && ReturnCode == 0)
	{
		WhereOutput.TrimStartAndEndInline();
		TArray<FString> Lines;
		WhereOutput.ParseIntoArrayLines(Lines);
		if (Lines.Num() > 0)
		{
			CachedCodexPath = Lines[0];
			return CachedCodexPath;
		}
	}

	return CachedCodexPath;
}

bool FCodexCliRunner::ExecuteAsync(
	const FAgentRequestConfig& Config,
	FOnAgentResponse OnComplete,
	FOnAgentProgress OnProgress)
{
	bool Expected = false;
	if (!bIsExecuting.CompareExchange(Expected, true))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("Codex CLI backend is already executing a request"));
		return false;
	}

	if (!IsCodexAvailable())
	{
		bIsExecuting = false;
		OnComplete.ExecuteIfBound(TEXT("Codex CLI not found. Install with: npm install -g @openai/codex"), false);
		return false;
	}

	if (Thread)
	{
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}

	CurrentConfig = Config;
	OnCompleteDelegate = OnComplete;
	OnProgressDelegate = OnProgress;

	Thread = FRunnableThread::Create(this, TEXT("CodexCliRunner"), 0, TPri_Normal);
	if (!Thread)
	{
		bIsExecuting = false;
		return false;
	}

	return true;
}

bool FCodexCliRunner::ExecuteSync(const FAgentRequestConfig& Config, FString& OutResponse)
{
	bool Expected = false;
	if (!bIsExecuting.CompareExchange(Expected, true))
	{
		OutResponse = TEXT("Codex CLI backend is already executing a request");
		return false;
	}

	CurrentConfig = Config;
	StopTaskCounter.Reset();
	JsonLineBuffer.Empty();
	AccumulatedResponseText.Empty();
	DiagnosticOutput.Empty();

	const FExecutionResult Result = ExecuteProcessBlocking();
	bIsExecuting = false;

	OutResponse = Result.ResponseText.IsEmpty() ? Result.DiagnosticText : Result.ResponseText;
	return Result.bSuccess;
}

void FCodexCliRunner::Cancel()
{
	StopTaskCounter.Set(1);
	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(ProcessHandle, true);
	}
	if (PersistentSession.ProcessHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(PersistentSession.ProcessHandle, true);
	}
}

bool FCodexCliRunner::Init()
{
	StopTaskCounter.Reset();
	JsonLineBuffer.Empty();
	AccumulatedResponseText.Empty();
	DiagnosticOutput.Empty();
	return true;
}

uint32 FCodexCliRunner::Run()
{
	ExecuteProcessAsync();
	return 0;
}

void FCodexCliRunner::Stop()
{
	StopTaskCounter.Increment();
}

void FCodexCliRunner::Exit()
{
	bIsExecuting = false;
}

void FCodexCliRunner::ResetConversation()
{
	PoisonPersistentTransport(true, true);
}

bool FCodexCliRunner::CreateProcessPipes()
{
	if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe, false))
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to create Codex stdout pipe"));
		return false;
	}

	if (!FPlatformProcess::CreatePipe(StdInReadPipe, StdInWritePipe, true))
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to create Codex stdin pipe"));
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = nullptr;
		WritePipe = nullptr;
		return false;
	}

	return true;
}

FString FCodexCliRunner::BuildCommandLine(const FAgentRequestConfig& Config)
{
	DiagnosticOutput.Empty();
	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();

	FString CommandLine;
	if (ConfigRequestsUnrealMcpBridge(Config) && !TryBuildCodexMcpOverrideArgs(Config, CommandLine, DiagnosticOutput))
	{
		return FString();
	}

	if (!TryBuildCodexLaunchOverrideArgs(Config, AuthContext, CommandLine, DiagnosticOutput))
	{
		return FString();
	}

	const bool bRequestsWorkspaceWriteTools = ConfigRequestsWorkspaceWriteTools(Config);
	if (Config.bSkipPermissions)
	{
		CommandLine += TEXT("--ask-for-approval never --sandbox danger-full-access ");
	}
	else if (bRequestsWorkspaceWriteTools)
	{
		CommandLine += TEXT("--ask-for-approval never --sandbox workspace-write ");
	}

	CommandLine += TEXT("exec --json --color never --skip-git-repo-check ");

	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	if (Settings && Settings->HasExplicitCodexProfile())
	{
		const FString ExplicitProfile = Settings->GetExplicitCodexProfile();
		if (ExplicitProfile.Contains(TEXT("\"")))
		{
			DiagnosticOutput += TEXT("Configured Codex profile cannot contain double quotes.\n");
			return FString();
		}

		CommandLine += FString::Printf(TEXT("-p \"%s\" "), *ExplicitProfile);
	}

	if (Settings && !Settings->GetConfiguredCodexModel().IsEmpty())
	{
		CommandLine += FString::Printf(TEXT("-m \"%s\" "), *Settings->GetConfiguredCodexModel());
	}

	for (const FString& ImagePath : Config.AttachedImagePaths)
	{
		FString FullImagePath = FPaths::ConvertRelativePathToFull(ImagePath);
		FullImagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		CommandLine += FString::Printf(TEXT("-i \"%s\" "), *FullImagePath);
	}

	FString WorkingDir = Config.WorkingDirectory;
	if (WorkingDir.IsEmpty())
	{
		WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}
	WorkingDir.ReplaceInline(TEXT("\\"), TEXT("/"));
	CommandLine += FString::Printf(TEXT("-C \"%s\" -"), *WorkingDir);

	return CommandLine;
}

FString FCodexCliRunner::BuildPromptPayload(const FAgentRequestConfig& Config) const
{
	if (Config.PromptContract.HasAnyContent())
	{
		return FAgentPromptMaterializer::MaterializeCodexPayload(Config.PromptContract, Config.Prompt);
	}

	FString Payload;
	if (!Config.SystemPrompt.IsEmpty())
	{
		Payload += TEXT("[SYSTEM CONTEXT]\n");
		Payload += Config.SystemPrompt;
		Payload += TEXT("\n[/SYSTEM CONTEXT]\n\n");
	}
	Payload += Config.Prompt;
	return Payload;
}

FString FCodexCliRunner::BuildPersistentTurnPayload(const FAgentRequestConfig& Config, const bool bIncludeSystemPrompt, const bool bIncludeBootstrapContext) const
{
	FString Payload;

	if (bIncludeSystemPrompt)
	{
		const FString SystemPrompt = Config.PromptContract.HasAnyContent()
			? FAgentPromptMaterializer::MaterializeCanonicalText(Config.PromptContract)
			: Config.SystemPrompt;
		if (!SystemPrompt.IsEmpty())
		{
			Payload += TEXT("[SYSTEM CONTEXT]\n");
			Payload += SystemPrompt;
			Payload += TEXT("\n[/SYSTEM CONTEXT]\n\n");
		}
	}

	if (bIncludeBootstrapContext && !Config.ConversationBootstrapText.IsEmpty())
	{
		Payload += TEXT("[RESTORED SESSION CONTEXT]\n");
		Payload += Config.ConversationBootstrapText;
		Payload += TEXT("\n[/RESTORED SESSION CONTEXT]\n\n");
	}

	Payload += Config.Prompt;
	return Payload;
}

void FCodexCliRunner::CleanupPersistentAppServer(const bool bTerminateProcess)
{
	if (PersistentSession.StdInReadPipe || PersistentSession.StdInWritePipe)
	{
		FPlatformProcess::ClosePipe(PersistentSession.StdInReadPipe, PersistentSession.StdInWritePipe);
		PersistentSession.StdInReadPipe = nullptr;
		PersistentSession.StdInWritePipe = nullptr;
	}

	if (PersistentSession.ReadPipe || PersistentSession.WritePipe)
	{
		FPlatformProcess::ClosePipe(PersistentSession.ReadPipe, PersistentSession.WritePipe);
		PersistentSession.ReadPipe = nullptr;
		PersistentSession.WritePipe = nullptr;
	}

	if (PersistentSession.ProcessHandle.IsValid())
	{
		if (bTerminateProcess && FPlatformProcess::IsProcRunning(PersistentSession.ProcessHandle))
		{
			FPlatformProcess::TerminateProc(PersistentSession.ProcessHandle, true);
		}

		FPlatformProcess::CloseProc(PersistentSession.ProcessHandle);
		PersistentSession.ProcessHandle = FProcHandle();
	}

	PersistentSession = FPersistentAppServerSession();
}

void FCodexCliRunner::ResetPersistentConversationState()
{
	PersistentSession.ThreadId.Empty();
	PersistentSession.ActiveTurnId.Empty();
	PersistentSession.bAwaitingBootstrapTurn = false;
}

FString FCodexCliRunner::GetPersistentThreadStatePath() const
{
	return GetPersistentCodexThreadStatePath();
}

void FCodexCliRunner::WritePersistentThreadStateSnapshot(const FString& StatePath) const
{
	const FString StateDir = FPaths::GetPath(StatePath);
	IFileManager::Get().MakeDirectory(*StateDir, true);

	if (PersistentSession.ThreadId.IsEmpty())
	{
		FString DeleteError;
		if (!OsvayderUEStorageMigration::DeleteManagedFileCopies(
			GetPersistentCodexThreadStatePath(),
			GetLegacyPersistentCodexThreadStatePath(),
			TEXT("codex_persistent_thread_state"),
			DeleteError))
		{
			UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *DeleteError);
		}
		return;
	}

	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("thread_id"), PersistentSession.ThreadId);
	RootObject->SetStringField(TEXT("home_path"), PersistentSession.HomePath);
	RootObject->SetStringField(TEXT("profile"), PersistentSession.ProfileLabel);
	RootObject->SetStringField(TEXT("model"), PersistentSession.ModelName);
	RootObject->SetStringField(TEXT("requested_speed_mode"), PersistentSession.RequestedSpeedModeName);
	RootObject->SetStringField(TEXT("effective_speed_mode"), PersistentSession.EffectiveSpeedModeName);
	RootObject->SetStringField(TEXT("work_mode"), PersistentSession.WorkModeName);
	RootObject->SetStringField(TEXT("reasoning_effort"), PersistentSession.ReasoningEffortName);
	RootObject->SetStringField(TEXT("verbosity"), PersistentSession.VerbosityName);
	RootObject->SetStringField(TEXT("execution_profile"), PersistentSession.ExecutionProfileName);

	FString JsonString;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonString);
	if (FJsonSerializer::Serialize(RootObject, Writer))
	{
		FFileHelper::SaveStringToFile(JsonString, *StatePath);
	}
}

bool FCodexCliRunner::TryLoadPersistedThreadState(FString& OutThreadId) const
{
	if (!PersistentSession.bPersistThreadState)
	{
		return false;
	}

	FString ResolveError;
	OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
	if (!OsvayderUEStorageMigration::ResolveManagedReadPath(
		GetPersistentThreadStatePath(),
		GetLegacyPersistentCodexThreadStatePath(),
		TEXT("codex_persistent_thread_state"),
		ValidatePersistentThreadStateFile,
		ManagedRead,
		ResolveError))
	{
		return false;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *ManagedRead.ResolvedPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return false;
	}

	FString StoredThreadId;
	FString StoredHomePath;
	FString StoredProfile;
	FString StoredModel;
	FString StoredRequestedSpeedMode;
	FString StoredEffectiveSpeedMode;
	FString StoredWorkMode;
	FString StoredReasoningEffort;
	FString StoredVerbosity;
	FString StoredExecutionProfile;
	if (!RootObject->TryGetStringField(TEXT("thread_id"), StoredThreadId) || StoredThreadId.IsEmpty())
	{
		return false;
	}

	RootObject->TryGetStringField(TEXT("home_path"), StoredHomePath);
	RootObject->TryGetStringField(TEXT("profile"), StoredProfile);
	RootObject->TryGetStringField(TEXT("model"), StoredModel);
	RootObject->TryGetStringField(TEXT("requested_speed_mode"), StoredRequestedSpeedMode);
	RootObject->TryGetStringField(TEXT("effective_speed_mode"), StoredEffectiveSpeedMode);
	RootObject->TryGetStringField(TEXT("work_mode"), StoredWorkMode);
	RootObject->TryGetStringField(TEXT("reasoning_effort"), StoredReasoningEffort);
	RootObject->TryGetStringField(TEXT("verbosity"), StoredVerbosity);
	RootObject->TryGetStringField(TEXT("execution_profile"), StoredExecutionProfile);

	if ((!PersistentSession.HomePath.IsEmpty() && StoredHomePath != PersistentSession.HomePath) ||
		StoredProfile != PersistentSession.ProfileLabel ||
		StoredModel != PersistentSession.ModelName ||
		StoredRequestedSpeedMode != PersistentSession.RequestedSpeedModeName ||
		StoredEffectiveSpeedMode != PersistentSession.EffectiveSpeedModeName ||
		StoredWorkMode != PersistentSession.WorkModeName ||
		StoredReasoningEffort != PersistentSession.ReasoningEffortName ||
		StoredVerbosity != PersistentSession.VerbosityName ||
		StoredExecutionProfile != PersistentSession.ExecutionProfileName)
	{
		return false;
	}

	OutThreadId = StoredThreadId;
	return true;
}

void FCodexCliRunner::PersistCurrentThreadState() const
{
	if (!PersistentSession.bPersistThreadState)
	{
		return;
	}

	WritePersistentThreadStateSnapshot(GetPersistentThreadStatePath());
}

bool FCodexCliRunner::ExportActiveThreadStateForRestartSurvival(FString& OutStatePath, FString& OutThreadId)
{
	OutStatePath = GetPersistentThreadStatePath();
	if (!PersistentSession.ThreadId.IsEmpty())
	{
		OutThreadId = PersistentSession.ThreadId;
		WritePersistentThreadStateSnapshot(OutStatePath);
		return IFileManager::Get().FileExists(*OutStatePath);
	}

	if (!TryLoadPersistedThreadState(OutThreadId))
	{
		return false;
	}

	return !OutThreadId.IsEmpty() && IFileManager::Get().FileExists(*OutStatePath);
}

FString FCodexCliRunner::GetActivePersistentThreadId() const
{
	return PersistentSession.ThreadId;
}

#if WITH_DEV_AUTOMATION_TESTS
void FCodexCliRunner::ClearTestInMemoryPersistentThreadId()
{
	ResetPersistentConversationState();
}
#endif

void FCodexCliRunner::ClearPersistedThreadState() const
{
	FString DeleteError;
	if (!OsvayderUEStorageMigration::DeleteManagedFileCopies(
		GetPersistentThreadStatePath(),
		GetLegacyPersistentCodexThreadStatePath(),
		TEXT("codex_persistent_thread_state"),
		DeleteError))
	{
		UE_LOG(LogOsvayderUE, Warning, TEXT("%s"), *DeleteError);
	}
}

int32 FCodexCliRunner::AllocatePersistentRequestId()
{
	return PersistentSession.NextRequestId++;
}

int32 FCodexCliRunner::GetPersistentRequestTimeoutMs(const FAgentRequestConfig& Config)
{
	const float DefaultTimeoutSeconds = static_cast<float>(CodexPersistentAppServerRequestTimeoutMs) / 1000.0f;
	const float RequestedTimeoutSeconds = (FMath::IsFinite(Config.TimeoutSeconds) && Config.TimeoutSeconds > 0.0f)
		? Config.TimeoutSeconds
		: DefaultTimeoutSeconds;
	return FMath::Max(1, FMath::RoundToInt(RequestedTimeoutSeconds * 1000.0f));
}

void FCodexCliRunner::PoisonPersistentTransport(const bool bTerminateProcess, const bool bClearPersistedThreadState)
{
	if (bClearPersistedThreadState)
	{
		ClearPersistedThreadState();
	}

	CleanupPersistentAppServer(bTerminateProcess);
}

void FCodexCliRunner::RecoverFromPersistentTransportFailure(FString& InOutDiagnostic, const bool bTerminateProcess, const bool bClearPersistedThreadState)
{
	PoisonPersistentTransport(bTerminateProcess, bClearPersistedThreadState);

	if (!InOutDiagnostic.IsEmpty() &&
		!InOutDiagnostic.Contains(TEXT("Persistent transport was reset"), ESearchCase::IgnoreCase))
	{
		InOutDiagnostic += TEXT(" Persistent transport was reset; the next request will start a fresh app-server session.");
	}
}

bool FCodexCliRunner::SendPersistentAppServerMessage(const TSharedRef<FJsonObject>& Message, FString& OutDiagnostic)
{
	if (!PersistentSession.StdInWritePipe)
	{
		OutDiagnostic = TEXT("Codex persistent app-server stdin pipe is not available.");
		RecoverFromPersistentTransportFailure(OutDiagnostic, true, true);
		return false;
	}

	FString RequestLine;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&RequestLine);
	if (!FJsonSerializer::Serialize(Message, Writer))
	{
		OutDiagnostic = TEXT("Failed to serialize Codex app-server JSON message.");
		return false;
	}

	RequestLine.AppendChar(TEXT('\n'));
	FTCHARToUTF8 Utf8Request(*RequestLine);
	int32 BytesWritten = 0;
	FPlatformProcess::WritePipe(
		PersistentSession.StdInWritePipe,
		reinterpret_cast<const uint8*>(Utf8Request.Get()),
		Utf8Request.Length(),
		&BytesWritten);

	if (BytesWritten != Utf8Request.Length())
	{
		OutDiagnostic = TEXT("Failed to write Codex app-server request to stdin.");
		RecoverFromPersistentTransportFailure(OutDiagnostic, true, true);
		return false;
	}

	return true;
}

bool FCodexCliRunner::SendPersistentAppServerRequest(const FString& Method, const TSharedRef<FJsonObject>& Params, const int32 RequestId, FString& OutDiagnostic)
{
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetNumberField(TEXT("id"), RequestId);
	Request->SetStringField(TEXT("method"), Method);
	Request->SetObjectField(TEXT("params"), Params);
	return SendPersistentAppServerMessage(Request, OutDiagnostic);
}

bool FCodexCliRunner::SendPersistentAppServerNotification(const FString& Method, const TSharedRef<FJsonObject>& Params, FString& OutDiagnostic)
{
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("method"), Method);
	Message->SetObjectField(TEXT("params"), Params);
	return SendPersistentAppServerMessage(Message, OutDiagnostic);
}

bool FCodexCliRunner::ReadNextPersistentAppServerMessage(const int32 TimeoutMs, TSharedPtr<FJsonObject>& OutMessage, FString& OutDiagnostic)
{
	const double Deadline = FPlatformTime::Seconds() + (static_cast<double>(TimeoutMs) / 1000.0);
	bool bDidFinalDrainAfterExit = false;

	while (FPlatformTime::Seconds() < Deadline)
	{
		FString CompleteLine;
		if (TryExtractJsonlMessage(PersistentSession.ReadBuffer, CompleteLine))
		{
			CompleteLine = ExtractJsonCandidate(CompleteLine);
			CompleteLine.TrimStartAndEndInline();
			if (CompleteLine.IsEmpty())
			{
				continue;
			}

			TSharedPtr<FJsonObject> JsonMessage;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CompleteLine);
			if (!FJsonSerializer::Deserialize(Reader, JsonMessage) || !JsonMessage.IsValid())
			{
				UE_LOG(LogOsvayderUE, Warning, TEXT("Ignoring non-JSON Codex persistent app-server output: %s"), *CompleteLine);
				continue;
			}

			OutMessage = JsonMessage;
			return true;
		}

		const FString OutputChunk = FPlatformProcess::ReadPipe(PersistentSession.ReadPipe);
		if (!OutputChunk.IsEmpty())
		{
			PersistentSession.ReadBuffer += OutputChunk;
			continue;
		}

		if (PersistentSession.ProcessHandle.IsValid() && !FPlatformProcess::IsProcRunning(PersistentSession.ProcessHandle))
		{
			if (bDidFinalDrainAfterExit)
			{
				const bool bCancelled = StopTaskCounter.GetValue() > 0;
				OutDiagnostic = bCancelled
					? TEXT("Codex turn was interrupted.")
					: TEXT("Codex persistent app-server exited unexpectedly.");
				RecoverFromPersistentTransportFailure(OutDiagnostic, true, !bCancelled);
				return false;
			}

			bDidFinalDrainAfterExit = true;
		}

		FPlatformProcess::Sleep(CodexBrowserVerifyPollIntervalSeconds);
	}

	OutDiagnostic = TEXT("Timed out waiting for Codex persistent app-server response.");
	RecoverFromPersistentTransportFailure(OutDiagnostic, true, true);
	return false;
}

bool FCodexCliRunner::WaitForPersistentAppServerResult(const int32 RequestId, const int32 TimeoutMs, TSharedPtr<FJsonObject>& OutResult, FString& OutDiagnostic)
{
	const double Deadline = FPlatformTime::Seconds() + (static_cast<double>(TimeoutMs) / 1000.0);
	while (FPlatformTime::Seconds() < Deadline)
	{
		TSharedPtr<FJsonObject> Message;
		const int32 RemainingMs = FMath::Max(1, static_cast<int32>((Deadline - FPlatformTime::Seconds()) * 1000.0));
		if (!ReadNextPersistentAppServerMessage(RemainingMs, Message, OutDiagnostic))
		{
			return false;
		}

		double ResponseId = 0.0;
		if (!Message->TryGetNumberField(TEXT("id"), ResponseId) || FMath::RoundToInt(ResponseId) != RequestId)
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (Message->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject && ErrorObject->IsValid())
		{
			FString ErrorMessage;
			if (!(*ErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage) || ErrorMessage.IsEmpty())
			{
				ErrorMessage = TEXT("unknown Codex app-server error");
			}

			OutDiagnostic = FString::Printf(TEXT("Codex persistent app-server error: %s"), *ErrorMessage);
			return false;
		}

		const TSharedPtr<FJsonObject>* ResultObject = nullptr;
		if (Message->TryGetObjectField(TEXT("result"), ResultObject) && ResultObject && ResultObject->IsValid())
		{
			OutResult = *ResultObject;
			return true;
		}

		OutDiagnostic = TEXT("Codex persistent app-server response did not contain an object result.");
		return false;
	}

	OutDiagnostic = TEXT("Timed out waiting for Codex persistent app-server JSON-RPC result.");
	RecoverFromPersistentTransportFailure(OutDiagnostic, true, true);
	return false;
}

bool FCodexCliRunner::EnsurePersistentAppServerReady(FString& OutDiagnostic)
{
	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
	const FCodexEnvSetupOptions EnvOptions = BuildEnvSetupOptionsForAuthContext(AuthContext);
	const FString DesiredProfile = GetCodexProfileLabel();
	const FString DesiredModel = GetConfiguredCodexModel();
	const FString DesiredRequestedSpeedMode = ResolveRequestedCodexSpeedModeLabel(CurrentConfig.CodexSpeedMode);
	const FCodexSpeedModeState DesiredSpeedState = ResolveCodexSpeedModeState(DesiredRequestedSpeedMode, AuthContext);
	const FString DesiredWorkMode = CurrentConfig.CodexWorkMode.TrimStartAndEnd().IsEmpty()
		? GetConfiguredCodexWorkMode()
		: CurrentConfig.CodexWorkMode.TrimStartAndEnd();
	const FString DesiredReasoningEffort = CurrentConfig.CodexReasoningEffort.TrimStartAndEnd().IsEmpty()
		? GetConfiguredCodexReasoningEffort()
		: CurrentConfig.CodexReasoningEffort.TrimStartAndEnd();
	const FString DesiredVerbosity = CurrentConfig.CodexVerbosity.TrimStartAndEnd().IsEmpty()
		? GetConfiguredCodexVerbosity()
		: CurrentConfig.CodexVerbosity.TrimStartAndEnd();

	FString DesiredHomePath;
	FString HomeDiagnostic;
	if (!ResolveEffectiveCodexHomePathForLaunch(AuthContext, true, DesiredHomePath, HomeDiagnostic))
	{
		OutDiagnostic = HomeDiagnostic;
		return false;
	}

	const bool bCanReusePersistentProcess =
		PersistentSession.ProcessHandle.IsValid() &&
		FPlatformProcess::IsProcRunning(PersistentSession.ProcessHandle) &&
		PersistentSession.bInitialized &&
		PersistentSession.ActiveTurnId.IsEmpty() &&
		PersistentSession.HomePath == DesiredHomePath &&
		PersistentSession.ProfileLabel == DesiredProfile &&
		PersistentSession.ModelName == DesiredModel &&
		PersistentSession.RequestedSpeedModeName == DesiredRequestedSpeedMode &&
		PersistentSession.EffectiveSpeedModeName == DesiredSpeedState.EffectiveMode &&
		PersistentSession.WorkModeName == DesiredWorkMode &&
		PersistentSession.ReasoningEffortName == DesiredReasoningEffort &&
		PersistentSession.VerbosityName == DesiredVerbosity &&
		PersistentSession.ExecutionProfileName == GetPersistentExecutionProfileName(CurrentConfig) &&
		PersistentSession.UnrealMcpLaunchSignature == BuildPersistentUnrealMcpLaunchSignature(CurrentConfig) &&
		PersistentSession.bPersistThreadState == (
			CurrentConfig.SessionPersistenceMode == EAgentSessionPersistenceMode::NormalProviderSession
			|| CurrentConfig.bUseRestartSurvivalProviderThreadState);

	if (bCanReusePersistentProcess)
	{
		return true;
	}

	CleanupPersistentAppServer(true);

	if (!FPlatformProcess::CreatePipe(PersistentSession.ReadPipe, PersistentSession.WritePipe, false))
	{
		OutDiagnostic = TEXT("Failed to create Codex persistent app-server stdout pipe.");
		return false;
	}

	if (!FPlatformProcess::CreatePipe(PersistentSession.StdInReadPipe, PersistentSession.StdInWritePipe, true))
	{
		OutDiagnostic = TEXT("Failed to create Codex persistent app-server stdin pipe.");
		CleanupPersistentAppServer(false);
		return false;
	}

	const FString CodexPath = GetCodexPath();
	if (CodexPath.IsEmpty())
	{
		OutDiagnostic = TEXT("Codex CLI path could not be resolved.");
		CleanupPersistentAppServer(false);
		return false;
	}

	FString EnvSetup = BuildStandardCodexEnvSetup(EnvOptions);
	if (!DesiredHomePath.IsEmpty())
	{
		EnvSetup += QuoteCmdEnv(TEXT("CODEX_HOME"), DesiredHomePath);
	}

	if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
	{
		EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), FString());
		if (AuthContext.bShouldForwardApiKeyToChild)
		{
			EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), AuthContext.ApiKeyEnvVarValue);
		}
	}
	else if (AuthContext.Mode == EOsvayderUECodexAuthMode::BrowserVerify ||
		AuthContext.Mode == EOsvayderUECodexAuthMode::CliTerminal)
	{
		EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), FString());
	}

	FString GlobalArgs;
	if (ConfigRequestsUnrealMcpBridge(CurrentConfig) && !TryBuildCodexMcpOverrideArgs(CurrentConfig, GlobalArgs, OutDiagnostic))
	{
		CleanupPersistentAppServer(false);
		return false;
	}

	if (!TryBuildCodexLaunchOverrideArgs(CurrentConfig, AuthContext, GlobalArgs, OutDiagnostic))
	{
		CleanupPersistentAppServer(false);
		return false;
	}

	if (const UOsvayderUESettings* Settings = UOsvayderUESettings::Get(); Settings && Settings->HasExplicitCodexProfile())
	{
		const FString ExplicitProfile = Settings->GetExplicitCodexProfile();
		if (ExplicitProfile.Contains(TEXT("\"")))
		{
			OutDiagnostic = TEXT("Configured Codex profile cannot contain double quotes.");
			CleanupPersistentAppServer(false);
			return false;
		}

		GlobalArgs += FString::Printf(TEXT("-p \"%s\" "), *ExplicitProfile);
	}

	FString Executable;
	FString Params;
#if PLATFORM_WINDOWS
	if (!CachedCodexJsPath.IsEmpty() && FPaths::GetCleanFilename(CodexPath).Equals(TEXT("node.exe"), ESearchCase::IgnoreCase))
	{
		Executable = TEXT("cmd.exe");
		Params = FString::Printf(
			TEXT("/c \"%s\"%s\" \"%s\" %sapp-server --listen stdio:// 2>&1\""),
			*EnvSetup,
			*CodexPath,
			*CachedCodexJsPath,
			*GlobalArgs);
	}
	else
	{
		FString DirectExecutable = CodexPath;
		if (FPaths::GetExtension(DirectExecutable, true).IsEmpty())
		{
			const FString ExeVariant = DirectExecutable + TEXT(".exe");
			if (IFileManager::Get().FileExists(*ExeVariant))
			{
				DirectExecutable = ExeVariant;
			}
		}

		if (!DirectExecutable.EndsWith(TEXT(".exe"), ESearchCase::IgnoreCase))
		{
			OutDiagnostic = FString::Printf(
				TEXT("Persistent Codex transport requires a directly launchable Codex executable or node-backed codex.js path. Resolved path `%s` is not supported."),
				*CodexPath);
			CleanupPersistentAppServer(false);
			return false;
		}

		Executable = TEXT("cmd.exe");
		Params = FString::Printf(
			TEXT("/c \"%s\"%s\" %sapp-server --listen stdio:// 2>&1\""),
			*EnvSetup,
			*DirectExecutable,
			*GlobalArgs);
	}
#else
	Executable = CodexPath;
	Params = FString::Printf(TEXT("%sapp-server --listen stdio://"), *GlobalArgs);
#endif

	PersistentSession.ProcessHandle = FPlatformProcess::CreateProc(
		*Executable,
		*Params,
		false,
		false,
		true,
		nullptr,
		0,
		*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
		PersistentSession.WritePipe,
		PersistentSession.StdInReadPipe);

	if (!PersistentSession.ProcessHandle.IsValid())
	{
		OutDiagnostic = FString::Printf(TEXT("Failed to launch Codex persistent app-server using `%s %s`."), *Executable, *Params);
		CleanupPersistentAppServer(false);
		return false;
	}

	PersistentSession.HomePath = DesiredHomePath;
	PersistentSession.ProfileLabel = DesiredProfile;
	PersistentSession.ModelName = DesiredModel;
	PersistentSession.RequestedSpeedModeName = DesiredRequestedSpeedMode;
	PersistentSession.EffectiveSpeedModeName = DesiredSpeedState.EffectiveMode;
	PersistentSession.WorkModeName = DesiredWorkMode;
	PersistentSession.ReasoningEffortName = DesiredReasoningEffort;
	PersistentSession.VerbosityName = DesiredVerbosity;
	PersistentSession.ExecutionProfileName = GetPersistentExecutionProfileName(CurrentConfig);
	PersistentSession.UnrealMcpLaunchSignature = BuildPersistentUnrealMcpLaunchSignature(CurrentConfig);
	PersistentSession.bPersistThreadState =
		CurrentConfig.SessionPersistenceMode == EAgentSessionPersistenceMode::NormalProviderSession
		|| CurrentConfig.bUseRestartSurvivalProviderThreadState;
	PersistentSession.NextRequestId = 1;
	PersistentSession.ReadBuffer.Empty();
	PersistentSession.ThreadId.Empty();
	PersistentSession.ActiveTurnId.Empty();
	PersistentSession.bAwaitingBootstrapTurn = false;

	const int32 InitializeRequestId = AllocatePersistentRequestId();
	TSharedRef<FJsonObject> InitializeParams = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
	ClientInfo->SetStringField(TEXT("name"), CodexPersistentServiceName);
	ClientInfo->SetStringField(TEXT("title"), TEXT("Osvayder UE Editor"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));
	InitializeParams->SetObjectField(TEXT("clientInfo"), ClientInfo);

	if (!SendPersistentAppServerRequest(CodexAppServerInitializeMethod, InitializeParams, InitializeRequestId, OutDiagnostic))
	{
		CleanupPersistentAppServer(true);
		return false;
	}

	TSharedPtr<FJsonObject> InitializeResult;
	if (!WaitForPersistentAppServerResult(InitializeRequestId, CodexPersistentAppServerStartupTimeoutMs, InitializeResult, OutDiagnostic))
	{
		CleanupPersistentAppServer(true);
		return false;
	}

	if (!SendPersistentAppServerNotification(CodexAppServerInitializedNotification, MakeShared<FJsonObject>(), OutDiagnostic))
	{
		CleanupPersistentAppServer(true);
		return false;
	}

	PersistentSession.bInitialized = true;
	return true;
}

bool FCodexCliRunner::StartOrResumePersistentThread(const FAgentRequestConfig& Config, const bool bPreferResume, FString& OutDiagnostic)
{
	const FString WorkingDir = GetPersistentSandboxWorkingDirectory(Config);

	FString Method = CodexAppServerThreadStartMethod;
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	FString RequestedThreadId;

	if (bPreferResume)
	{
		if (!TryLoadPersistedThreadState(RequestedThreadId))
		{
			return false;
		}

		Method = CodexAppServerThreadResumeMethod;
		Params->SetStringField(TEXT("threadId"), RequestedThreadId);
	}
	else
	{
		if (!PersistentSession.ModelName.IsEmpty())
		{
			Params->SetStringField(TEXT("model"), PersistentSession.ModelName);
		}
		if (!WorkingDir.IsEmpty())
		{
			Params->SetStringField(TEXT("cwd"), WorkingDir);
		}
		Params->SetStringField(TEXT("approvalPolicy"), TEXT("never"));
		Params->SetStringField(TEXT("sandbox"), GetPersistentThreadSandboxMode(Config));
		Params->SetStringField(TEXT("serviceName"), CodexPersistentServiceName);
		Params->SetBoolField(TEXT("ephemeral"), !PersistentSession.bPersistThreadState);
	}

	const int32 RequestId = AllocatePersistentRequestId();
	if (!SendPersistentAppServerRequest(Method, Params, RequestId, OutDiagnostic))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Result;
	if (!WaitForPersistentAppServerResult(RequestId, GetPersistentRequestTimeoutMs(Config), Result, OutDiagnostic))
	{
		if (bPreferResume)
		{
			ClearPersistedThreadState();
		}
		return false;
	}

	const TSharedPtr<FJsonObject>* ThreadObject = nullptr;
	if (!Result->TryGetObjectField(TEXT("thread"), ThreadObject) || !ThreadObject || !(*ThreadObject).IsValid())
	{
		OutDiagnostic = TEXT("Codex persistent app-server thread response did not include a thread object.");
		return false;
	}

	FString ThreadId;
	(*ThreadObject)->TryGetStringField(TEXT("id"), ThreadId);
	if (ThreadId.IsEmpty())
	{
		OutDiagnostic = TEXT("Codex persistent app-server thread response did not include a thread id.");
		return false;
	}

	PersistentSession.ThreadId = ThreadId;
	PersistentSession.ActiveTurnId.Empty();
	PersistentSession.bAwaitingBootstrapTurn = !bPreferResume;
	PersistCurrentThreadState();
	return true;
}

bool FCodexCliRunner::EnsurePersistentThreadReady(const FAgentRequestConfig& Config, FString& OutDiagnostic)
{
	if (!PersistentSession.ThreadId.IsEmpty())
	{
		return true;
	}

	if (PersistentSession.bPersistThreadState && StartOrResumePersistentThread(Config, true, OutDiagnostic))
	{
		return true;
	}

	const FString ResumeDiagnostic = OutDiagnostic;
	OutDiagnostic.Empty();

	if (StartOrResumePersistentThread(Config, false, OutDiagnostic))
	{
		return true;
	}

	if (PersistentSession.bPersistThreadState && !ResumeDiagnostic.IsEmpty())
	{
		OutDiagnostic = ResumeDiagnostic + TEXT("\n") + OutDiagnostic;
	}
	return false;
}

bool FCodexCliRunner::HandlePersistentAppServerNotification(
	const TSharedPtr<FJsonObject>& Message,
	bool& bOutTurnFinished,
	bool& bOutTurnSucceeded,
	FString& OutFinalTurnRawJson,
	FString& OutDiagnostic)
{
	bOutTurnFinished = false;
	bOutTurnSucceeded = false;

	FString Method;
	if (!Message.IsValid() || !Message->TryGetStringField(TEXT("method"), Method) || Method.IsEmpty())
	{
		return true;
	}

	const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
	if (!Message->TryGetObjectField(TEXT("params"), ParamsObject) || !ParamsObject || !(*ParamsObject).IsValid())
	{
		return true;
	}

	if (Method == TEXT("item/agentMessage/delta"))
	{
		FString DeltaText;
		if (!(*ParamsObject)->TryGetStringField(TEXT("delta"), DeltaText))
		{
			(*ParamsObject)->TryGetStringField(TEXT("text"), DeltaText);
		}

		if (!DeltaText.IsEmpty())
		{
			EmitTextEvent(DeltaText);
		}
		return true;
	}

	if (Method == TEXT("item/started") || Method == TEXT("item/completed"))
	{
		const bool bCompleted = Method == TEXT("item/completed");
		const TSharedPtr<FJsonObject>* ItemObject = nullptr;
		if (!(*ParamsObject)->TryGetObjectField(TEXT("item"), ItemObject) || !ItemObject || !(*ItemObject).IsValid())
		{
			return true;
		}

		FString ItemType;
		FString ItemId;
		(*ItemObject)->TryGetStringField(TEXT("type"), ItemType);
		(*ItemObject)->TryGetStringField(TEXT("id"), ItemId);

		if (ItemType == TEXT("commandExecution"))
		{
			FString Command;
			(*ItemObject)->TryGetStringField(TEXT("command"), Command);
			if (!bCompleted)
			{
				EmitToolUseEvent(ItemId, TEXT("command_execution"), Command, SerializeJsonValue(MakeShared<FJsonValueObject>(*ItemObject)));
			}
			else
			{
				FString Output;
				(*ItemObject)->TryGetStringField(TEXT("aggregatedOutput"), Output);
				double ExitCodeValue = -1.0;
				(*ItemObject)->TryGetNumberField(TEXT("exitCode"), ExitCodeValue);
				EmitToolResultEvent(ItemId, TEXT("command_execution"), Output, static_cast<int32>(ExitCodeValue), SerializeJsonValue(MakeShared<FJsonValueObject>(*ItemObject)));
			}
			return true;
		}

		if (ItemType == TEXT("mcpToolCall"))
		{
			FString Server;
			FString Tool;
			(*ItemObject)->TryGetStringField(TEXT("server"), Server);
			(*ItemObject)->TryGetStringField(TEXT("tool"), Tool);
			const FString ToolLabel = Server.IsEmpty() ? Tool : FString::Printf(TEXT("%s/%s"), *Server, *Tool);
			if (!bCompleted)
			{
				FString ToolInput;
				if (const TSharedPtr<FJsonValue>* ArgumentsValue = (*ItemObject)->Values.Find(TEXT("arguments")))
				{
					ToolInput = SerializeJsonValue(*ArgumentsValue);
				}
				EmitToolUseEvent(ItemId, ToolLabel, ToolInput, SerializeJsonValue(MakeShared<FJsonValueObject>(*ItemObject)));
			}
			else
			{
				FString ToolResult;
				if (const TSharedPtr<FJsonValue>* ResultValue = (*ItemObject)->Values.Find(TEXT("result")))
				{
					ToolResult = SerializeJsonValue(*ResultValue);
				}
				else if (const TSharedPtr<FJsonValue>* ErrorValue = (*ItemObject)->Values.Find(TEXT("error")))
				{
					ToolResult = SerializeJsonValue(*ErrorValue);
				}
				EmitToolResultEvent(ItemId, ToolLabel, ToolResult, 0, SerializeJsonValue(MakeShared<FJsonValueObject>(*ItemObject)));
			}
			return true;
		}

		if (ItemType == TEXT("dynamicToolCall"))
		{
			FString Tool;
			(*ItemObject)->TryGetStringField(TEXT("tool"), Tool);
			if (!bCompleted)
			{
				FString ToolInput;
				if (const TSharedPtr<FJsonValue>* ArgumentsValue = (*ItemObject)->Values.Find(TEXT("arguments")))
				{
					ToolInput = SerializeJsonValue(*ArgumentsValue);
				}
				EmitToolUseEvent(ItemId, Tool.IsEmpty() ? TEXT("dynamic_tool") : Tool, ToolInput, SerializeJsonValue(MakeShared<FJsonValueObject>(*ItemObject)));
			}
			else
			{
				const FString ToolResult = SerializeJsonValue(MakeShared<FJsonValueObject>(*ItemObject));
				EmitToolResultEvent(ItemId, Tool.IsEmpty() ? TEXT("dynamic_tool") : Tool, ToolResult, 0, ToolResult);
			}
			return true;
		}

		return true;
	}

	if (Method == TEXT("turn/completed"))
	{
		OutFinalTurnRawJson = SerializeJsonValue(MakeShared<FJsonValueObject>(Message));

		const TSharedPtr<FJsonObject>* TurnObject = nullptr;
		if (!(*ParamsObject)->TryGetObjectField(TEXT("turn"), TurnObject) || !TurnObject || !(*TurnObject).IsValid())
		{
			OutDiagnostic = TEXT("Codex persistent app-server turn/completed notification did not include a turn object.");
			return false;
		}

		FString TurnStatus;
		(*TurnObject)->TryGetStringField(TEXT("status"), TurnStatus);
		PersistentSession.ActiveTurnId.Empty();
		PersistentSession.bAwaitingBootstrapTurn = false;

		if (TurnStatus.Equals(TEXT("completed"), ESearchCase::IgnoreCase))
		{
			bOutTurnFinished = true;
			bOutTurnSucceeded = true;
			return true;
		}

		FString ErrorMessage = FString::Printf(TEXT("Codex turn finished with status `%s`."), *TurnStatus);
		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if ((*TurnObject)->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject && (*ErrorObject).IsValid())
		{
			FString ServerError;
			if ((*ErrorObject)->TryGetStringField(TEXT("message"), ServerError) && !ServerError.IsEmpty())
			{
				ErrorMessage = ServerError;
			}
		}

		OutDiagnostic = ErrorMessage;
		bOutTurnFinished = true;
		bOutTurnSucceeded = false;
		return true;
	}

	return true;
}

FCodexCliRunner::FExecutionResult FCodexCliRunner::ExecutePersistentTurnBlocking()
{
	FExecutionResult Result;
	FString Diagnostic;

	AccumulatedResponseText.Empty();
	DiagnosticOutput.Empty();

	if (!EnsurePersistentAppServerReady(Diagnostic))
	{
		Result.DiagnosticText = Diagnostic;
		return Result;
	}

	if (!EnsurePersistentThreadReady(CurrentConfig, Diagnostic))
	{
		Result.DiagnosticText = Diagnostic;
		return Result;
	}

	if (CurrentConfig.OnStreamEvent.IsBound())
	{
		FAgentRunEvent Event;
		Event.Type = EAgentRunEventType::SessionInit;
		Event.Backend = EOsvayderUEProviderBackend::CodexCli;
		Event.SessionId = PersistentSession.ThreadId;
		FOnAgentStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
		AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
		{
			EventDelegate.ExecuteIfBound(Event);
		});
	}

	TArray<TSharedPtr<FJsonValue>> InputItems;
	const FString TurnText = BuildPersistentTurnPayload(
		CurrentConfig,
		PersistentSession.bAwaitingBootstrapTurn,
		PersistentSession.bAwaitingBootstrapTurn && !CurrentConfig.ConversationBootstrapText.IsEmpty());

	if (!TurnText.IsEmpty())
	{
		TSharedRef<FJsonObject> TextItem = MakeShared<FJsonObject>();
		TextItem->SetStringField(TEXT("type"), TEXT("text"));
		TextItem->SetStringField(TEXT("text"), TurnText);
		InputItems.Add(MakeShared<FJsonValueObject>(TextItem));
	}

	for (const FString& ImagePath : CurrentConfig.AttachedImagePaths)
	{
		const FString FullImagePath = FPaths::ConvertRelativePathToFull(ImagePath);
		TSharedRef<FJsonObject> ImageItem = MakeShared<FJsonObject>();
		ImageItem->SetStringField(TEXT("type"), TEXT("localImage"));
		ImageItem->SetStringField(TEXT("path"), FullImagePath);
		InputItems.Add(MakeShared<FJsonValueObject>(ImageItem));
	}

	TSharedRef<FJsonObject> TurnParams = MakeShared<FJsonObject>();
	TurnParams->SetStringField(TEXT("threadId"), PersistentSession.ThreadId);
	TurnParams->SetArrayField(TEXT("input"), InputItems);

	const FString WorkingDir = GetPersistentSandboxWorkingDirectory(CurrentConfig);
	if (!WorkingDir.IsEmpty())
	{
		TurnParams->SetStringField(TEXT("cwd"), WorkingDir);
	}

	TurnParams->SetStringField(TEXT("approvalPolicy"), TEXT("never"));
	TurnParams->SetObjectField(TEXT("sandboxPolicy"), BuildPersistentTurnSandboxPolicy(CurrentConfig));
	if (!PersistentSession.ModelName.IsEmpty())
	{
		TurnParams->SetStringField(TEXT("model"), PersistentSession.ModelName);
	}

	const int32 TurnRequestId = AllocatePersistentRequestId();
	if (!SendPersistentAppServerRequest(CodexAppServerTurnStartMethod, TurnParams, TurnRequestId, Diagnostic))
	{
		Result.DiagnosticText = Diagnostic;
		return Result;
	}

	const int32 PersistentRequestTimeoutMs = GetPersistentRequestTimeoutMs(CurrentConfig);
	TSharedPtr<FJsonObject> TurnResult;
	if (!WaitForPersistentAppServerResult(TurnRequestId, PersistentRequestTimeoutMs, TurnResult, Diagnostic))
	{
		Result.DiagnosticText = Diagnostic;
		return Result;
	}

	const TSharedPtr<FJsonObject>* TurnObject = nullptr;
	if (!TurnResult->TryGetObjectField(TEXT("turn"), TurnObject) || !TurnObject || !(*TurnObject).IsValid())
	{
		Result.DiagnosticText = TEXT("Codex persistent app-server turn/start response did not include a turn object.");
		return Result;
	}

	(*TurnObject)->TryGetStringField(TEXT("id"), PersistentSession.ActiveTurnId);
	const double TurnStartTime = FPlatformTime::Seconds();

	bool bTurnFinished = false;
	bool bTurnSucceeded = false;
	FString FinalTurnRawJson;
	while (!StopTaskCounter.GetValue() && !bTurnFinished)
	{
		TSharedPtr<FJsonObject> Message;
		if (!ReadNextPersistentAppServerMessage(PersistentRequestTimeoutMs, Message, Diagnostic))
		{
			Result.DiagnosticText = Diagnostic;
			return Result;
		}

		if (!HandlePersistentAppServerNotification(Message, bTurnFinished, bTurnSucceeded, FinalTurnRawJson, Diagnostic))
		{
			Result.DiagnosticText = Diagnostic;
			return Result;
		}
	}

	if (StopTaskCounter.GetValue() && !bTurnFinished)
	{
		PoisonPersistentTransport(true, false);
		Result.DiagnosticText = TEXT("Codex turn was interrupted.");
		return Result;
	}

	if (!bTurnSucceeded)
	{
		Result.DiagnosticText = Diagnostic.IsEmpty() ? TEXT("Codex turn failed.") : Diagnostic;
		return Result;
	}

	const int32 DurationMs = static_cast<int32>((FPlatformTime::Seconds() - TurnStartTime) * 1000.0);
	EmitResultEvent(1, FinalTurnRawJson, DurationMs);
	PersistCurrentThreadState();

	Result.ResponseText = AccumulatedResponseText;
	Result.DiagnosticText = DiagnosticOutput.TrimStartAndEnd();
	Result.bSuccess = true;
	return Result;
}

bool FCodexCliRunner::LaunchProcess(const FString& FullCommand, const FString& WorkingDir)
{
	const FString CodexPath = GetCodexPath();
	const FCodexAuthContext AuthContext = ResolveCodexAuthContext();
	const FCodexEnvSetupOptions EnvOptions = BuildEnvSetupOptionsForAuthContext(AuthContext);
	FString EffectiveCodexHome;
	FString EffectiveHomeDiagnostic;
	if (!ResolveEffectiveCodexHomePathForLaunch(AuthContext, true, EffectiveCodexHome, EffectiveHomeDiagnostic))
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("%s"), *EffectiveHomeDiagnostic);
		return false;
	}
	FString EnvSetup = BuildStandardCodexEnvSetup(EnvOptions);
	if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
	{
		EnvSetup += QuoteCmdEnv(TEXT("CODEX_HOME"), EffectiveCodexHome);

		// Clear any inherited OPENAI_API_KEY first so the explicit env-var mode stays truthful.
		EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), FString());
		if (AuthContext.bShouldForwardApiKeyToChild)
		{
			EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), AuthContext.ApiKeyEnvVarValue);
		}
	}
	else if (AuthContext.Mode == EOsvayderUECodexAuthMode::BrowserVerify)
	{
		EnvSetup += QuoteCmdEnv(TEXT("CODEX_HOME"), EffectiveCodexHome);
		EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), FString());
	}
	else if (AuthContext.Mode == EOsvayderUECodexAuthMode::CliTerminal)
	{
		if (!EffectiveCodexHome.IsEmpty())
		{
			EnvSetup += QuoteCmdEnv(TEXT("CODEX_HOME"), EffectiveCodexHome);
		}

		// Keep CLI Terminal truthfully tied to externally managed CLI login.
		EnvSetup += QuoteCmdEnv(TEXT("OPENAI_API_KEY"), FString());
	}
	else if (!EffectiveCodexHome.IsEmpty())
	{
		EnvSetup += QuoteCmdEnv(TEXT("CODEX_HOME"), EffectiveCodexHome);
	}

	FString ActualExe = TEXT("cmd.exe");
	FString Params;

#if PLATFORM_WINDOWS
	if (!CachedCodexJsPath.IsEmpty())
	{
		Params = FString::Printf(
			TEXT("/c \"%s\"%s\" \"%s\" %s 2>&1\""),
			*EnvSetup,
			*CodexPath,
			*CachedCodexJsPath,
			*FullCommand);
	}
	else
	{
		Params = FString::Printf(
			TEXT("/c \"%s\"%s\" %s 2>&1\""),
			*EnvSetup,
			*CodexPath,
			*FullCommand);
	}
#else
	ActualExe = CodexPath;
	Params = FullCommand;
#endif

	ProcessHandle = FPlatformProcess::CreateProc(
		*ActualExe,
		*Params,
		false,
		false,
		true,
		nullptr,
		0,
		*WorkingDir,
		WritePipe,
		StdInReadPipe);

	if (!ProcessHandle.IsValid())
	{
		UE_LOG(LogOsvayderUE, Error, TEXT("Failed to launch Codex CLI process"));
		UE_LOG(LogOsvayderUE, Error, TEXT("Exe: %s"), *ActualExe);
		UE_LOG(LogOsvayderUE, Error, TEXT("Params: %s"), *Params);
		return false;
	}

	const FString WorkModeForLog = CurrentConfig.CodexWorkMode.IsEmpty()
		? GetConfiguredCodexWorkMode()
		: CurrentConfig.CodexWorkMode;
	const FString RequestedSpeedForLog = ResolveRequestedCodexSpeedModeLabel(CurrentConfig.CodexSpeedMode);
	const FCodexSpeedModeState SpeedStateForLog = ResolveCodexSpeedModeState(RequestedSpeedForLog, AuthContext);
	const FString ReasoningForLog = CurrentConfig.CodexReasoningEffort.IsEmpty()
		? GetConfiguredCodexReasoningEffort()
		: CurrentConfig.CodexReasoningEffort;
	const FString VerbosityForLog = CurrentConfig.CodexVerbosity.IsEmpty()
		? GetConfiguredCodexVerbosity()
		: CurrentConfig.CodexVerbosity;

	UE_LOG(
		LogOsvayderUE,
		Log,
		TEXT("Launching Codex CLI with auth_mode=%s effective_auth_path=%s ownership=%s profile=%s model=%s speed_requested=%s speed_effective=%s speed_support=%s work_mode=%s reasoning=%s verbosity=%s"),
		*AuthContext.ModeName,
		*AuthContext.EffectivePath,
		*AuthContext.OwnershipModel,
		*GetCodexProfileLabel(),
		*GetConfiguredCodexModel(),
		*RequestedSpeedForLog,
		*SpeedStateForLog.EffectiveMode,
		*SpeedStateForLog.SupportLabel,
		*WorkModeForLog,
		*ReasoningForLog,
		*VerbosityForLog);
	if (AuthContext.Mode == EOsvayderUECodexAuthMode::ApiKeyEnvVar)
	{
		UE_LOG(
			LogOsvayderUE,
			Log,
			TEXT("Codex env-var bridge source=%s present=%s isolated_codex_home=%s"),
			*AuthContext.ApiKeyEnvVarName,
			AuthContext.bHasNamedEnvVar ? TEXT("true") : TEXT("false"),
			*GetIsolatedApiKeyEnvCodexHome());
	}

	return true;
}

FString FCodexCliRunner::ReadProcessOutput()
{
	FString FullOutput;

	JsonLineBuffer.Empty();
	AccumulatedResponseText.Empty();
	DiagnosticOutput.Empty();

	while (!StopTaskCounter.GetValue())
	{
		FString OutputChunk = FPlatformProcess::ReadPipe(ReadPipe);
		if (!OutputChunk.IsEmpty())
		{
			FullOutput += OutputChunk;
			JsonLineBuffer += OutputChunk;

			int32 NewlineIdx = INDEX_NONE;
			while (JsonLineBuffer.FindChar(TEXT('\n'), NewlineIdx))
			{
				FString CompleteLine = JsonLineBuffer.Left(NewlineIdx);
				CompleteLine.TrimEndInline();
				JsonLineBuffer.RightChopInline(NewlineIdx + 1);

				if (!CompleteLine.IsEmpty())
				{
					ParseAndEmitJsonLine(CompleteLine);
				}
			}
		}

		if (!FPlatformProcess::IsProcRunning(ProcessHandle))
		{
			FString RemainingOutput = FPlatformProcess::ReadPipe(ReadPipe);
			while (!RemainingOutput.IsEmpty())
			{
				FullOutput += RemainingOutput;
				JsonLineBuffer += RemainingOutput;
				RemainingOutput = FPlatformProcess::ReadPipe(ReadPipe);
			}

			int32 FinalNewlineIdx = INDEX_NONE;
			while (JsonLineBuffer.FindChar(TEXT('\n'), FinalNewlineIdx))
			{
				FString CompleteLine = JsonLineBuffer.Left(FinalNewlineIdx);
				CompleteLine.TrimEndInline();
				JsonLineBuffer.RightChopInline(FinalNewlineIdx + 1);

				if (!CompleteLine.IsEmpty())
				{
					ParseAndEmitJsonLine(CompleteLine);
				}
			}

			JsonLineBuffer.TrimEndInline();
			if (!JsonLineBuffer.IsEmpty())
			{
				ParseAndEmitJsonLine(JsonLineBuffer);
				JsonLineBuffer.Empty();
			}

			break;
		}

		FPlatformProcess::Sleep(0.01f);
	}

	return FullOutput;
}

void FCodexCliRunner::ParseAndEmitJsonLine(const FString& JsonLine)
{
	const FString TrimmedLine = JsonLine.TrimStartAndEnd();
	if (!TrimmedLine.StartsWith(TEXT("{")))
	{
		DiagnosticOutput += TrimmedLine + TEXT("\n");
		return;
	}

	TSharedPtr<FJsonObject> JsonObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TrimmedLine);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		DiagnosticOutput += TrimmedLine + TEXT("\n");
		return;
	}

	FString Type;
	if (!JsonObj->TryGetStringField(TEXT("type"), Type))
	{
		return;
	}

	if (Type == TEXT("thread.started"))
	{
		FString ThreadId;
		JsonObj->TryGetStringField(TEXT("thread_id"), ThreadId);

		if (CurrentConfig.OnStreamEvent.IsBound())
		{
			FAgentRunEvent Event;
			Event.Type = EAgentRunEventType::SessionInit;
			Event.Backend = EOsvayderUEProviderBackend::CodexCli;
			Event.SessionId = ThreadId;
			Event.RawJson = TrimmedLine;
			FOnAgentStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
			AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
			{
				EventDelegate.ExecuteIfBound(Event);
			});
		}
		return;
	}

	if (Type == TEXT("item.started") || Type == TEXT("item.completed"))
	{
		const TSharedPtr<FJsonObject>* ItemObj = nullptr;
		if (!JsonObj->TryGetObjectField(TEXT("item"), ItemObj) || !ItemObj || !(*ItemObj).IsValid())
		{
			return;
		}

		FString ItemType;
		FString ItemId;
		(*ItemObj)->TryGetStringField(TEXT("type"), ItemType);
		(*ItemObj)->TryGetStringField(TEXT("id"), ItemId);

		const bool bStarted = Type == TEXT("item.started");

		if (ItemType == TEXT("agent_message"))
		{
			if (!bStarted)
			{
				FString Text;
				if ((*ItemObj)->TryGetStringField(TEXT("text"), Text))
				{
					EmitTextEvent(Text);
				}
			}
			return;
		}

		if (ItemType == TEXT("command_execution"))
		{
			FString Command;
			(*ItemObj)->TryGetStringField(TEXT("command"), Command);
			if (bStarted)
			{
				EmitToolUseEvent(ItemId, TEXT("command_execution"), Command, TrimmedLine);
			}
			else
			{
				FString AggregatedOutput;
				(*ItemObj)->TryGetStringField(TEXT("aggregated_output"), AggregatedOutput);
				double ExitCodeValue = -1.0;
				(*ItemObj)->TryGetNumberField(TEXT("exit_code"), ExitCodeValue);
				EmitToolResultEvent(ItemId, TEXT("command_execution"), AggregatedOutput, static_cast<int32>(ExitCodeValue), TrimmedLine);
			}
			return;
		}

		if (ItemType.Contains(TEXT("tool")))
		{
			FString ToolName;
			TryGetStringFieldAny(*ItemObj, { TEXT("title"), TEXT("name"), TEXT("tool_name") }, ToolName);
			if (ToolName.IsEmpty())
			{
				ToolName = ItemType;
			}

			FString ToolInput;
			const TSharedPtr<FJsonObject>* InputObj = nullptr;
			if ((*ItemObj)->TryGetObjectField(TEXT("input"), InputObj))
			{
				ToolInput = SerializeJsonValue(MakeShared<FJsonValueObject>(*InputObj));
			}
			else if (const TSharedPtr<FJsonValue>* ArgValue = (*ItemObj)->Values.Find(TEXT("arguments")))
			{
				ToolInput = SerializeJsonValue(*ArgValue);
			}

			if (bStarted)
			{
				EmitToolUseEvent(ItemId, ToolName, ToolInput, TrimmedLine);
			}
			else
			{
				FString ToolResult = TrimmedLine;
				if (const TSharedPtr<FJsonValue>* ResultValue = (*ItemObj)->Values.Find(TEXT("result")))
				{
					ToolResult = SerializeJsonValue(*ResultValue);
				}
				EmitToolResultEvent(ItemId, ToolName, ToolResult, 0, TrimmedLine);
			}
			return;
		}

		return;
	}

	if (Type == TEXT("turn.completed"))
	{
		EmitResultEvent(1, TrimmedLine);
		return;
	}
}

void FCodexCliRunner::EmitTextEvent(const FString& Text)
{
	AccumulatedResponseText += Text;

	if (OnProgressDelegate.IsBound())
	{
		FOnAgentProgress ProgressDelegate = OnProgressDelegate;
		AsyncTask(ENamedThreads::GameThread, [ProgressDelegate, Text]()
		{
			ProgressDelegate.ExecuteIfBound(Text);
		});
	}

	if (CurrentConfig.OnStreamEvent.IsBound())
	{
		FAgentRunEvent Event;
		Event.Type = EAgentRunEventType::TextContent;
		Event.Backend = EOsvayderUEProviderBackend::CodexCli;
		Event.Text = Text;
		FOnAgentStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
		AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
		{
			EventDelegate.ExecuteIfBound(Event);
		});
	}
}

void FCodexCliRunner::EmitToolUseEvent(const FString& ToolId, const FString& ToolName, const FString& ToolInput, const FString& RawJson)
{
	if (!CurrentConfig.OnStreamEvent.IsBound())
	{
		return;
	}

	FAgentRunEvent Event;
	Event.Type = EAgentRunEventType::ToolUse;
	Event.Backend = EOsvayderUEProviderBackend::CodexCli;
	Event.ToolCallId = ToolId;
	Event.ToolName = ToolName;
	Event.ToolInput = ToolInput;
	Event.RawJson = RawJson;

	FOnAgentStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
	AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
	{
		EventDelegate.ExecuteIfBound(Event);
	});
}

void FCodexCliRunner::EmitToolResultEvent(const FString& ToolId, const FString& ToolName, const FString& ToolResult, int32 ExitCode, const FString& RawJson)
{
	if (!CurrentConfig.OnStreamEvent.IsBound())
	{
		return;
	}

	FAgentRunEvent Event;
	Event.Type = EAgentRunEventType::ToolResult;
	Event.Backend = EOsvayderUEProviderBackend::CodexCli;
	Event.ToolCallId = ToolId;
	Event.ToolName = ToolName;
	Event.ToolResultContent = ToolResult;
	Event.ExitCode = ExitCode;
	Event.bIsError = ExitCode > 0;
	Event.RawJson = RawJson;

	FOnAgentStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
	AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
	{
		EventDelegate.ExecuteIfBound(Event);
	});
}

void FCodexCliRunner::EmitResultEvent(int32 NumTurns, const FString& RawJson, const int32 DurationMs)
{
	if (!CurrentConfig.OnStreamEvent.IsBound())
	{
		return;
	}

	FAgentRunEvent Event;
	Event.Type = EAgentRunEventType::Result;
	Event.Backend = EOsvayderUEProviderBackend::CodexCli;
	Event.ResultText = AccumulatedResponseText;
	Event.NumTurns = NumTurns;
	Event.DurationMs = DurationMs;
	Event.RawJson = RawJson;

	FOnAgentStreamEvent EventDelegate = CurrentConfig.OnStreamEvent;
	AsyncTask(ENamedThreads::GameThread, [EventDelegate, Event]()
	{
		EventDelegate.ExecuteIfBound(Event);
	});
}

FCodexCliRunner::FExecutionResult FCodexCliRunner::ExecuteProcessBlocking()
{
	const bool bUsePersistentConversationTransport =
		!CurrentConfig.bForceDisablePersistentConversationTransport &&
		ShouldUsePersistentConversationTransport();
	if (bUsePersistentConversationTransport)
	{
		return ExecutePersistentTurnBlocking();
	}

	FExecutionResult Result;

	const FString CodexPath = GetCodexPath();
	if (CodexPath.IsEmpty())
	{
		Result.DiagnosticText = TEXT("Codex CLI not found. Install with: npm install -g @openai/codex");
		return Result;
	}

	FString WorkingDir = CurrentConfig.WorkingDirectory;
	if (WorkingDir.IsEmpty())
	{
		WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}

	if (!CreateProcessPipes())
	{
		Result.DiagnosticText = TEXT("Failed to create pipe for Codex process");
		return Result;
	}

	const FString CommandLine = BuildCommandLine(CurrentConfig);
	if (CommandLine.IsEmpty())
	{
		CleanupHandles();
		Result.DiagnosticText = DiagnosticOutput.IsEmpty()
			? TEXT("Failed to prepare Codex CLI command line")
			: DiagnosticOutput;
		return Result;
	}

	if (!LaunchProcess(CommandLine, WorkingDir))
	{
		CleanupHandles();
		Result.DiagnosticText = FString::Printf(TEXT("Failed to start Codex process from %s"), *CodexPath);
		return Result;
	}

	if (StdInWritePipe)
	{
		const FString Payload = BuildPromptPayload(CurrentConfig);
		FTCHARToUTF8 Utf8Payload(*Payload);
		int32 BytesWritten = 0;
		FPlatformProcess::WritePipe(StdInWritePipe, reinterpret_cast<const uint8*>(Utf8Payload.Get()), Utf8Payload.Length(), &BytesWritten);
		FPlatformProcess::ClosePipe(StdInReadPipe, StdInWritePipe);
		StdInReadPipe = nullptr;
		StdInWritePipe = nullptr;
	}

	ReadProcessOutput();

	int32 ExitCode = 0;
	FPlatformProcess::GetProcReturnCode(ProcessHandle, &ExitCode);

	if (ReadPipe || WritePipe)
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = nullptr;
		WritePipe = nullptr;
	}
	FPlatformProcess::CloseProc(ProcessHandle);

	Result.ResponseText = AccumulatedResponseText;
	Result.DiagnosticText = DiagnosticOutput.TrimStartAndEnd();
	Result.bSuccess = (ExitCode == 0) && !StopTaskCounter.GetValue();

	if (!Result.bSuccess && Result.ResponseText.IsEmpty())
	{
		Result.ResponseText = Result.DiagnosticText.IsEmpty()
			? FString::Printf(TEXT("Codex CLI exited with code %d"), ExitCode)
			: Result.DiagnosticText;
	}

	return Result;
}

void FCodexCliRunner::ExecuteProcessAsync()
{
	const FExecutionResult Result = ExecuteProcessBlocking();
	if (Result.bSuccess)
	{
		ReportCompletion(Result.ResponseText, true);
	}
	else
	{
		ReportError(Result.ResponseText.IsEmpty() ? Result.DiagnosticText : Result.ResponseText);
	}
}

void FCodexCliRunner::ReportError(const FString& ErrorMessage)
{
	FOnAgentResponse CompleteCopy = OnCompleteDelegate;
	const FString Message = ErrorMessage;
	AsyncTask(ENamedThreads::GameThread, [CompleteCopy, Message]()
	{
		CompleteCopy.ExecuteIfBound(Message, false);
	});
}

void FCodexCliRunner::ReportCompletion(const FString& Output, bool bSuccess)
{
	FOnAgentResponse CompleteCopy = OnCompleteDelegate;
	const FString FinalOutput = Output;
	AsyncTask(ENamedThreads::GameThread, [CompleteCopy, FinalOutput, bSuccess]()
	{
		CompleteCopy.ExecuteIfBound(FinalOutput, bSuccess);
	});
}
