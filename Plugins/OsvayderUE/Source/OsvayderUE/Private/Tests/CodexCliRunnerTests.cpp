// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "CodexCliRunner.h"
#include "MCP/Tools/MCPTool_PluginSettings.h"
#include "OsvayderUESettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString NormalizeDirectoryPathForTest(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return FString();
		}

		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	TSharedPtr<FJsonObject> GetObjectFieldOrNull(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* NestedObject = nullptr;
		if (!Object->TryGetObjectField(FieldName, NestedObject) || !NestedObject || !(*NestedObject).IsValid())
		{
			return nullptr;
		}

		return *NestedObject;
	}

	bool JsonStringArrayContains(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& ExpectedValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}

		const FString NormalizedExpectedValue = NormalizeDirectoryPathForTest(ExpectedValue);
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString ActualValue;
			if (Value.IsValid() &&
				Value->TryGetString(ActualValue) &&
				NormalizeDirectoryPathForTest(ActualValue).Equals(NormalizedExpectedValue, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	bool WriteCodexArtifact(const FString& CodexHomePath)
	{
		const FString NormalizedHomePath = NormalizeDirectoryPathForTest(CodexHomePath);
		if (!IFileManager::Get().MakeDirectory(*NormalizedHomePath, true))
		{
			return false;
		}

		const FString ArtifactPath = FPaths::Combine(NormalizedHomePath, TEXT("auth.json"));
		return FFileHelper::SaveStringToFile(TEXT("{\"access_token\":\"test\"}\n"), *ArtifactPath);
	}

	FString MakeFreshTestRoot(const FString& TestName)
	{
		FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("OsvayderUEAutomation"), TEXT("CodexHomeResolution"), TestName);
		FPaths::NormalizeDirectoryName(Root);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	FAgentRequestConfig MakeWorkspaceWriteDefaultConfigForPersistentSandbox()
	{
		FAgentRequestConfig Config;
		Config.WorkingDirectory = FPaths::ProjectDir();
		Config.ExecutionProfile = EAgentExecutionRunProfile::ConfiguredDefaultRuntime;
		Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NotPersisted;
		Config.bSkipPermissions = false;
		Config.AllowedTools = {
			TEXT("Read"),
			TEXT("Write"),
			TEXT("Edit"),
			TEXT("Grep"),
			TEXT("Glob"),
			TEXT("Bash"),
			TEXT("mcp__osvayderue__restart_survival")
		};
		return Config;
	}

	FAgentRequestConfig MakeExplicitExpertConfigForPersistentSandbox()
	{
		FAgentRequestConfig Config;
		Config.WorkingDirectory = FPaths::ProjectDir();
		Config.ExecutionProfile = EAgentExecutionRunProfile::ExplicitExpertOptIn;
		Config.SessionPersistenceMode = EAgentSessionPersistenceMode::NormalProviderSession;
		Config.bSkipPermissions = true;
		Config.AllowedTools = {
			TEXT("Read"),
			TEXT("Write"),
			TEXT("Edit"),
			TEXT("Grep"),
			TEXT("Glob"),
			TEXT("Bash")
		};
		return Config;
	}

	struct FScopedEnvironmentVariable
	{
		FString Name;
		FString OriginalValue;

		FScopedEnvironmentVariable(const TCHAR* InName, const FString& NewValue)
			: Name(InName)
			, OriginalValue(FPlatformMisc::GetEnvironmentVariable(InName))
		{
			FPlatformMisc::SetEnvironmentVar(*Name, *NewValue);
		}

		~FScopedEnvironmentVariable()
		{
			FPlatformMisc::SetEnvironmentVar(*Name, *OriginalValue);
		}
	};

	struct FScopedCodexHomeOverride
	{
		UOsvayderUESettings* Settings = nullptr;
		FString OriginalHomeOverride;

		explicit FScopedCodexHomeOverride(const FString& NewHomeOverride)
		{
			Settings = UOsvayderUESettings::GetMutable();
			if (Settings)
			{
				OriginalHomeOverride = Settings->DefaultCodexHomeOverride;
				Settings->DefaultCodexHomeOverride = NewHomeOverride;
			}
		}

		~FScopedCodexHomeOverride()
		{
			if (Settings)
			{
				Settings->DefaultCodexHomeOverride = OriginalHomeOverride;
			}
		}
	};

	struct FScopedMachineStandardCandidateOverride
	{
		explicit FScopedMachineStandardCandidateOverride(const TArray<FString>& CandidateHomes)
		{
			FCodexCliRunner::SetTestKnownMachineStandardHomes(CandidateHomes);
		}

		~FScopedMachineStandardCandidateOverride()
		{
			FCodexCliRunner::ClearTestKnownMachineStandardHomes();
		}
	};

	struct FScopedCodexLaunchOverride
	{
		FScopedCodexLaunchOverride(const FString& InExecutablePath, const FString& InCodexJsPath)
		{
			FCodexCliRunner::SetTestCodexLaunchOverride(InExecutablePath, InCodexJsPath);
		}

		~FScopedCodexLaunchOverride()
		{
			FCodexCliRunner::ClearTestCodexLaunchOverride();
		}
	};

	struct FScopedPersistentAppServerOverride
	{
		UOsvayderUESettings* Settings = nullptr;
		bool OriginalValue = true;

		explicit FScopedPersistentAppServerOverride(const bool bNewValue)
		{
			Settings = UOsvayderUESettings::GetMutable();
			if (Settings)
			{
				OriginalValue = Settings->bCodexUsePersistentAppServer;
				Settings->bCodexUsePersistentAppServer = bNewValue;
			}
		}

		~FScopedPersistentAppServerOverride()
		{
			if (Settings)
			{
				Settings->bCodexUsePersistentAppServer = OriginalValue;
			}
		}
	};

	struct FScopedCodexAuthModeOverride
	{
		UOsvayderUESettings* Settings = nullptr;
		EOsvayderUECodexAuthMode OriginalValue = EOsvayderUECodexAuthMode::Auto;

		explicit FScopedCodexAuthModeOverride(const EOsvayderUECodexAuthMode NewValue)
		{
			Settings = UOsvayderUESettings::GetMutable();
			if (Settings)
			{
				OriginalValue = Settings->DefaultCodexAuthMode;
				Settings->DefaultCodexAuthMode = NewValue;
			}
		}

		~FScopedCodexAuthModeOverride()
		{
			if (Settings)
			{
				Settings->DefaultCodexAuthMode = OriginalValue;
			}
		}
	};

	struct FScopedCodexApiKeyEnvVarOverride
	{
		UOsvayderUESettings* Settings = nullptr;
		FString OriginalValue;

		explicit FScopedCodexApiKeyEnvVarOverride(const FString& NewValue)
		{
			Settings = UOsvayderUESettings::GetMutable();
			if (Settings)
			{
				OriginalValue = Settings->DefaultCodexApiKeyEnvVar;
				Settings->DefaultCodexApiKeyEnvVar = NewValue;
			}
		}

		~FScopedCodexApiKeyEnvVarOverride()
		{
			if (Settings)
			{
				Settings->DefaultCodexApiKeyEnvVar = OriginalValue;
			}
		}
	};

	FString ResolveNodeExecutableForTest()
	{
#if PLATFORM_WINDOWS
		TArray<FString> CandidatePaths;
		CandidatePaths.Add(TEXT("D:\\Program Files\\nodejs\\node.exe"));
		CandidatePaths.Add(TEXT("C:\\Program Files\\nodejs\\node.exe"));

		FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
		TArray<FString> PathDirs;
		PathEnv.ParseIntoArray(PathDirs, TEXT(";"), true);
		for (const FString& Dir : PathDirs)
		{
			CandidatePaths.Add(FPaths::Combine(Dir, TEXT("node.exe")));
		}

		for (const FString& CandidatePath : CandidatePaths)
		{
			if (IFileManager::Get().FileExists(*CandidatePath))
			{
				return CandidatePath;
			}
		}

		FString WhereOutput;
		FString WhereErrors;
		int32 ReturnCode = 0;
		if (FPlatformProcess::ExecProcess(TEXT("where"), TEXT("node"), &ReturnCode, &WhereOutput, &WhereErrors) && ReturnCode == 0)
		{
			TArray<FString> Lines;
			WhereOutput.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				const FString TrimmedLine = Line.TrimStartAndEnd();
				if (!TrimmedLine.IsEmpty() && IFileManager::Get().FileExists(*TrimmedLine))
				{
					return TrimmedLine;
				}
			}
		}
#else
		const TArray<FString> CandidatePaths = {
			TEXT("/usr/local/bin/node"),
			TEXT("/usr/bin/node")
		};
		for (const FString& CandidatePath : CandidatePaths)
		{
			if (IFileManager::Get().FileExists(*CandidatePath))
			{
				return CandidatePath;
			}
		}
#endif

		return FString();
	}

	FString WriteFakeCodexAppServerScript(const FString& TestRoot)
	{
		const FString ScriptPath = FPaths::Combine(TestRoot, TEXT("fake_codex_app_server.js"));
		const FString ScriptText =
			TEXT("const fs = require('fs');\n")
			TEXT("const readline = require('readline');\n")
			TEXT("const scenario = process.env.OSVAYDERUE_FAKE_APP_SERVER_SCENARIO || 'always_success';\n")
			TEXT("const markerPath = process.env.OSVAYDERUE_FAKE_APP_SERVER_MARKER || '';\n")
			TEXT("const counterPath = process.env.OSVAYDERUE_FAKE_APP_SERVER_COUNTER || '';\n")
			TEXT("const allowedToolsLogPath = process.env.OSVAYDERUE_FAKE_APP_SERVER_ALLOWED_TOOLS_LOG || '';\n")
			TEXT("const codexHomeLogPath = process.env.OSVAYDERUE_FAKE_APP_SERVER_CODEX_HOME_LOG || '';\n")
			TEXT("function readAllowedToolsFromArgs(argv) {\n")
			TEXT("  for (let index = 0; index < argv.length; index += 1) {\n")
			TEXT("    const currentArg = argv[index] || '';\n")
			TEXT("    const configArg = currentArg === '-c' && index + 1 < argv.length ? argv[index + 1] : currentArg;\n")
			TEXT("    const match = configArg.match(/mcp_servers\\.osvayderue\\.env\\.UNREAL_MCP_ALLOWED_TOOLS='([^']*)'/);\n")
			TEXT("    if (match && match[1] !== undefined) { return match[1]; }\n")
			TEXT("  }\n")
			TEXT("  return '';\n")
			TEXT("}\n")
			TEXT("const allowedTools = process.env.UNREAL_MCP_ALLOWED_TOOLS || readAllowedToolsFromArgs(process.argv) || '';\n")
			TEXT("function readLaunchIndex(path) {\n")
			TEXT("  if (!path) { return 0; }\n")
			TEXT("  try {\n")
			TEXT("    const raw = fs.readFileSync(path, 'utf8').trim();\n")
			TEXT("    const parsed = parseInt(raw, 10);\n")
			TEXT("    return Number.isFinite(parsed) ? parsed : 0;\n")
			TEXT("  } catch (error) {\n")
			TEXT("    return 0;\n")
			TEXT("  }\n")
			TEXT("}\n")
			TEXT("let launchIndex = readLaunchIndex(counterPath) + 1;\n")
			TEXT("if (counterPath) { fs.writeFileSync(counterPath, String(launchIndex)); }\n")
			TEXT("if (allowedToolsLogPath) { fs.appendFileSync(allowedToolsLogPath, `${launchIndex}:${allowedTools}\\n`); }\n")
			TEXT("if (codexHomeLogPath) { fs.appendFileSync(codexHomeLogPath, `${launchIndex}:${process.env.CODEX_HOME || ''}\\n`); }\n")
			TEXT("let turnCounter = 0;\n")
			TEXT("let hasHungTurn = false;\n")
			TEXT("let lastThreadMethod = 'thread/start';\n")
			TEXT("let lastThreadId = '';\n")
			TEXT("function send(message) { process.stdout.write(JSON.stringify(message) + '\\n'); }\n")
			TEXT("const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });\n")
			TEXT("rl.on('line', (line) => {\n")
			TEXT("  let message;\n")
			TEXT("  try { message = JSON.parse(line); } catch (error) { return; }\n")
			TEXT("  if (message.method === 'initialize' && message.id !== undefined) {\n")
			TEXT("    send({ id: message.id, result: { protocolVersion: '1.0' } });\n")
			TEXT("    return;\n")
			TEXT("  }\n")
			TEXT("  if (message.method === 'initialized') { return; }\n")
			TEXT("  if ((message.method === 'thread/start' || message.method === 'thread/resume') && message.id !== undefined) {\n")
			TEXT("    const requestedThreadId = message.params && message.params.threadId ? message.params.threadId : `thread-${launchIndex}`;\n")
			TEXT("    lastThreadMethod = message.method;\n")
			TEXT("    lastThreadId = requestedThreadId;\n")
			TEXT("    send({ id: message.id, result: { thread: { id: requestedThreadId } } });\n")
			TEXT("    return;\n")
			TEXT("  }\n")
			TEXT("  if (message.method === 'turn/start' && message.id !== undefined) {\n")
			TEXT("    turnCounter += 1;\n")
			TEXT("    const turnId = `turn-${launchIndex}-${turnCounter}`;\n")
			TEXT("    send({ id: message.id, result: { turn: { id: turnId } } });\n")
			TEXT("    if (hasHungTurn) { return; }\n")
			TEXT("    if (scenario === 'turn_timeout_once_then_success' && !markerPath) {\n")
			TEXT("      hasHungTurn = true;\n")
			TEXT("      return;\n")
			TEXT("    }\n")
			TEXT("    if (scenario === 'turn_timeout_once_then_success' && !fs.existsSync(markerPath)) {\n")
			TEXT("      fs.writeFileSync(markerPath, 'timed_out');\n")
			TEXT("      hasHungTurn = true;\n")
			TEXT("      return;\n")
			TEXT("    }\n")
			TEXT("    send({ method: 'item/agentMessage/delta', params: { delta: `launch=${launchIndex} threadMethod=${lastThreadMethod} threadId=${lastThreadId || `thread-${launchIndex}`} turn=${turnCounter} tools=${allowedTools}` } });\n")
			TEXT("    send({ method: 'turn/completed', params: { turn: { id: turnId, status: 'completed' } } });\n")
			TEXT("    return;\n")
			TEXT("  }\n")
			TEXT("  if (message.method === 'turn/interrupt' && message.id !== undefined) {\n")
			TEXT("    send({ id: message.id, result: {} });\n")
			TEXT("  }\n")
			TEXT("});\n");

		if (!FFileHelper::SaveStringToFile(ScriptText, *ScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return FString();
		}

		return ScriptPath;
	}

	FAgentRequestConfig MakeLivePersistentTransportConfig(const float TimeoutSeconds = 1.0f)
	{
		FAgentRequestConfig Config = MakeWorkspaceWriteDefaultConfigForPersistentSandbox();
		Config.Prompt = TEXT("Persistent transport recovery probe");
		Config.TimeoutSeconds = TimeoutSeconds;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_MachineStandardCandidateResolutionPrecedence,
	"OsvayderUE.CodexCliRunner.HomeResolution.MachineStandardCandidatePrecedence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_MachineStandardCandidateResolutionPrecedence::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("MachineStandardCandidatePrecedence"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CandidateHome = FPaths::Combine(TestRoot, TEXT("MachineStandardHome"), TEXT(".codex-cli"));

	TestTrue(TEXT("candidate artifact should be created"), WriteCodexArtifact(CandidateHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedCodexHomeOverride ExplicitOverride(TEXT(""));
	TArray<FString> CandidateHomes;
	CandidateHomes.Add(CandidateHome);
	FScopedMachineStandardCandidateOverride CandidateOverride(CandidateHomes);

	const FString NormalizedCandidateHome = NormalizeDirectoryPathForTest(CandidateHome);
	TestEqual(TEXT("shared codex home should resolve to the detected machine-standard candidate"),
		NormalizeDirectoryPathForTest(FCodexCliRunner::GetConfiguredCodexHomePath()),
		NormalizedCandidateHome);
	TestEqual(TEXT("resolution source should surface machine_standard_candidate"),
		FCodexCliRunner::GetConfiguredCodexHomeResolutionSource(),
		FString(TEXT("machine_standard_candidate")));
	TestEqual(TEXT("machine-standard readback should surface the known candidate"),
		NormalizeDirectoryPathForTest(FCodexCliRunner::GetMachineStandardCodexHomePath()),
		NormalizedCandidateHome);
	TestTrue(TEXT("configured home should report a credential artifact"),
		FCodexCliRunner::IsConfiguredCodexHomeArtifactPresent());
	TestEqual(TEXT("detected artifact home should point to the candidate"),
		NormalizeDirectoryPathForTest(FCodexCliRunner::GetDetectedCodexArtifactHomePath()),
		NormalizedCandidateHome);

	const TArray<FString> DetectedCandidates = FCodexCliRunner::GetDetectedCodexCandidateHomes();
	TestEqual(TEXT("exactly one candidate home should be detected"), DetectedCandidates.Num(), 1);
	if (DetectedCandidates.Num() == 1)
	{
		TestEqual(TEXT("detected candidate list should surface the candidate home"),
			NormalizeDirectoryPathForTest(DetectedCandidates[0]),
			NormalizedCandidateHome);
	}

	if (FCodexCliRunner::IsCodexAvailable())
	{
		FCodexCliRunner Runner;
		const FAgentBackendStatus Status = Runner.GetStatus();
		TestTrue(TEXT("status detail should explain machine-standard candidate selection"),
			Status.Detail.Contains(TEXT("machine-standard"), ESearchCase::IgnoreCase));
		TestTrue(TEXT("status auth detail should explain machine-standard candidate selection"),
			Status.AuthDetail.Contains(TEXT("machine-standard"), ESearchCase::IgnoreCase));
		TestTrue(TEXT("status detail should use Osvayder branding for machine-standard selection"),
			Status.Detail.Contains(TEXT("Osvayder UE"), ESearchCase::IgnoreCase));
		TestFalse(TEXT("status detail should not use stale OsvayderUE branding for machine-standard selection"),
			Status.Detail.Contains(TEXT("OsvayderUE selected the detected machine-standard"), ESearchCase::IgnoreCase));
		TestTrue(TEXT("status auth detail should use Osvayder branding for machine-standard selection"),
			Status.AuthDetail.Contains(TEXT("Osvayder UE"), ESearchCase::IgnoreCase));
		TestFalse(TEXT("status auth detail should not use stale OsvayderUE branding for machine-standard selection"),
			Status.AuthDetail.Contains(TEXT("OsvayderUE selected the detected machine-standard"), ESearchCase::IgnoreCase));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_ExplicitOverrideWinsHomeResolution,
	"OsvayderUE.CodexCliRunner.HomeResolution.ExplicitOverrideWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_ExplicitOverrideWinsHomeResolution::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("ExplicitOverrideWins"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString ExplicitHome = FPaths::Combine(TestRoot, TEXT("ExplicitHome"));
	const FString EnvHome = FPaths::Combine(TestRoot, TEXT("EnvHome"));
	const FString CandidateHome = FPaths::Combine(TestRoot, TEXT("MachineStandardHome"), TEXT(".codex-cli"));

	TestTrue(TEXT("explicit artifact should be created"), WriteCodexArtifact(ExplicitHome));
	TestTrue(TEXT("env override artifact should be created"), WriteCodexArtifact(EnvHome));
	TestTrue(TEXT("candidate artifact should be created"), WriteCodexArtifact(CandidateHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), EnvHome);
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedCodexHomeOverride ExplicitOverride(ExplicitHome);
	TArray<FString> CandidateHomes;
	CandidateHomes.Add(CandidateHome);
	FScopedMachineStandardCandidateOverride CandidateOverride(CandidateHomes);

	TestEqual(TEXT("explicit override should win over env and candidate homes"),
		NormalizeDirectoryPathForTest(FCodexCliRunner::GetConfiguredCodexHomePath()),
		NormalizeDirectoryPathForTest(ExplicitHome));
	TestEqual(TEXT("resolution source should surface explicit_override"),
		FCodexCliRunner::GetConfiguredCodexHomeResolutionSource(),
		FString(TEXT("explicit_override")));
	TestTrue(TEXT("configured explicit home should report an artifact"),
		FCodexCliRunner::IsConfiguredCodexHomeArtifactPresent());
	TestEqual(TEXT("candidate detection should still remember the machine-standard artifact home"),
		NormalizeDirectoryPathForTest(FCodexCliRunner::GetDetectedCodexArtifactHomePath()),
		NormalizeDirectoryPathForTest(CandidateHome));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_MachineEnvOverrideWinsHomeResolution,
	"OsvayderUE.CodexCliRunner.HomeResolution.MachineEnvOverrideWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_MachineEnvOverrideWinsHomeResolution::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("MachineEnvOverrideWins"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString EnvHome = FPaths::Combine(TestRoot, TEXT("EnvHome"));
	const FString CandidateHome = FPaths::Combine(TestRoot, TEXT("MachineStandardHome"), TEXT(".codex-cli"));

	TestTrue(TEXT("env override artifact should be created"), WriteCodexArtifact(EnvHome));
	TestTrue(TEXT("candidate artifact should be created"), WriteCodexArtifact(CandidateHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), EnvHome);
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedCodexHomeOverride ExplicitOverride(TEXT(""));
	TArray<FString> CandidateHomes;
	CandidateHomes.Add(CandidateHome);
	FScopedMachineStandardCandidateOverride CandidateOverride(CandidateHomes);

	TestEqual(TEXT("machine env override should win over the machine-standard candidate"),
		NormalizeDirectoryPathForTest(FCodexCliRunner::GetConfiguredCodexHomePath()),
		NormalizeDirectoryPathForTest(EnvHome));
	TestEqual(TEXT("resolution source should surface machine_env_override"),
		FCodexCliRunner::GetConfiguredCodexHomeResolutionSource(),
		FString(TEXT("machine_env_override")));
	TestTrue(TEXT("configured env home should report an artifact"),
		FCodexCliRunner::IsConfiguredCodexHomeArtifactPresent());
	TestEqual(TEXT("candidate detection should still surface the machine-standard artifact home"),
		NormalizeDirectoryPathForTest(FCodexCliRunner::GetDetectedCodexArtifactHomePath()),
		NormalizeDirectoryPathForTest(CandidateHome));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_OsvayderMachineEnvOverrideWinsHomeResolution,
	"OsvayderUE.CodexCliRunner.HomeResolution.OsvayderMachineEnvOverrideWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_OsvayderMachineEnvOverrideWinsHomeResolution::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("OsvayderMachineEnvOverrideWins"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString PreferredEnvHome = FPaths::Combine(TestRoot, TEXT("PreferredEnvHome"));
	const FString LegacyEnvHome = FPaths::Combine(TestRoot, TEXT("LegacyEnvHome"));
	const FString CandidateHome = FPaths::Combine(TestRoot, TEXT("MachineStandardHome"), TEXT(".codex-cli"));

	TestTrue(TEXT("preferred env override artifact should be created"), WriteCodexArtifact(PreferredEnvHome));
	TestTrue(TEXT("legacy env override artifact should be created"), WriteCodexArtifact(LegacyEnvHome));
	TestTrue(TEXT("candidate artifact should be created"), WriteCodexArtifact(CandidateHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), LegacyEnvHome);
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), PreferredEnvHome);
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedCodexHomeOverride ExplicitOverride(TEXT(""));
	TArray<FString> CandidateHomes;
	CandidateHomes.Add(CandidateHome);
	FScopedMachineStandardCandidateOverride CandidateOverride(CandidateHomes);

	TestEqual(TEXT("preferred Osvayder env override should win over legacy env and candidate homes"),
		NormalizeDirectoryPathForTest(FCodexCliRunner::GetConfiguredCodexHomePath()),
		NormalizeDirectoryPathForTest(PreferredEnvHome));
	TestEqual(TEXT("resolution source should surface machine_env_override"),
		FCodexCliRunner::GetConfiguredCodexHomeResolutionSource(),
		FString(TEXT("machine_env_override")));
	TestTrue(TEXT("configured preferred env home should report an artifact"),
		FCodexCliRunner::IsConfiguredCodexHomeArtifactPresent());

	if (FCodexCliRunner::IsCodexAvailable())
	{
		FCodexCliRunner Runner;
		const FAgentBackendStatus Status = Runner.GetStatus();
		TestTrue(TEXT("status detail should explain preferred env override precedence"),
			Status.Detail.Contains(TEXT("OSVAYDERUE_CODEX_HOME"), ESearchCase::IgnoreCase));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_PluginSettingsReadbackSurfacesHomeResolutionTruth,
	"OsvayderUE.CodexCliRunner.HomeResolution.PluginSettingsReadbackTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_PluginSettingsReadbackSurfacesHomeResolutionTruth::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("PluginSettingsReadbackTruth"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CandidateHome = FPaths::Combine(TestRoot, TEXT("MachineStandardHome"), TEXT(".codex-cli"));

	TestTrue(TEXT("candidate artifact should be created"), WriteCodexArtifact(CandidateHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedCodexHomeOverride ExplicitOverride(TEXT(""));
	TArray<FString> CandidateHomes;
	CandidateHomes.Add(CandidateHome);
	FScopedMachineStandardCandidateOverride CandidateOverride(CandidateHomes);

	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> BackendObject = GetObjectFieldOrNull(Result.Data, TEXT("assistant_backend"));
	TestTrue(TEXT("assistant_backend should exist"), BackendObject.IsValid());
	if (!BackendObject.IsValid())
	{
		return false;
	}

	FString CodexHomePath;
	FString ResolutionSource;
	FString MachineStandardHome;
	FString DetectedArtifactHome;
	bool bArtifactPresentInConfiguredHome = false;
	TestTrue(TEXT("codex_home_path should exist"), BackendObject->TryGetStringField(TEXT("codex_home_path"), CodexHomePath));
	TestTrue(TEXT("codex_home_resolution_source should exist"), BackendObject->TryGetStringField(TEXT("codex_home_resolution_source"), ResolutionSource));
	TestTrue(TEXT("codex_machine_standard_home should exist"), BackendObject->TryGetStringField(TEXT("codex_machine_standard_home"), MachineStandardHome));
	TestTrue(TEXT("codex_artifact_present_in_configured_home should exist"), BackendObject->TryGetBoolField(TEXT("codex_artifact_present_in_configured_home"), bArtifactPresentInConfiguredHome));
	TestTrue(TEXT("codex_detected_artifact_home should exist"), BackendObject->TryGetStringField(TEXT("codex_detected_artifact_home"), DetectedArtifactHome));
	TestEqual(TEXT("plugin_settings should report the resolved machine-standard home"),
		NormalizeDirectoryPathForTest(CodexHomePath),
		NormalizeDirectoryPathForTest(CandidateHome));
	TestEqual(TEXT("plugin_settings should report machine_standard_candidate resolution"),
		ResolutionSource,
		FString(TEXT("machine_standard_candidate")));
	TestEqual(TEXT("plugin_settings should report the known machine-standard home"),
		NormalizeDirectoryPathForTest(MachineStandardHome),
		NormalizeDirectoryPathForTest(CandidateHome));
	TestTrue(TEXT("plugin_settings should report artifact presence in the configured home"), bArtifactPresentInConfiguredHome);
	TestEqual(TEXT("plugin_settings should surface the detected artifact home"),
		NormalizeDirectoryPathForTest(DetectedArtifactHome),
		NormalizeDirectoryPathForTest(CandidateHome));
	TestTrue(TEXT("plugin_settings should expose the detected candidate in the array readback"),
		JsonStringArrayContains(BackendObject, TEXT("codex_detected_candidate_homes"), CandidateHome));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_ProjectSessionIsolationRemainsProjectLocal,
	"OsvayderUE.CodexCliRunner.HomeResolution.ProjectSessionIsolationRemainsProjectLocal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_ProjectLocalAuthModesUseOsvayderStorageRoot,
	"OsvayderUE.CodexCliRunner.AuthDiagnostics.ProjectLocalAuthModesUseOsvayderStorageRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_ProjectSessionIsolationRemainsProjectLocal::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("ProjectSessionIsolationRemainsProjectLocal"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CandidateHome = FPaths::Combine(TestRoot, TEXT("MachineStandardHome"), TEXT(".codex-cli"));

	TestTrue(TEXT("candidate artifact should be created"), WriteCodexArtifact(CandidateHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedCodexHomeOverride ExplicitOverride(TEXT(""));
	TArray<FString> CandidateHomes;
	CandidateHomes.Add(CandidateHome);
	FScopedMachineStandardCandidateOverride CandidateOverride(CandidateHomes);

	FMCPTool_PluginSettings Tool;
	const FMCPToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
	TestTrue(TEXT("plugin_settings should succeed"), Result.bSuccess);
	TestTrue(TEXT("plugin_settings should return data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> BackendObject = GetObjectFieldOrNull(Result.Data, TEXT("assistant_backend"));
	const TSharedPtr<FJsonObject> SessionObject = GetObjectFieldOrNull(BackendObject, TEXT("session"));
	TestTrue(TEXT("assistant_backend should exist"), BackendObject.IsValid());
	TestTrue(TEXT("session should exist"), SessionObject.IsValid());
	if (!BackendObject.IsValid() || !SessionObject.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> CurrentProviderSession = GetObjectFieldOrNull(SessionObject, TEXT("current_provider"));
	const TSharedPtr<FJsonObject> OtherProviderSession = GetObjectFieldOrNull(SessionObject, TEXT("other_provider"));
	const TSharedPtr<FJsonObject> LegacySharedSession = GetObjectFieldOrNull(SessionObject, TEXT("legacy_shared"));
	const TSharedPtr<FJsonObject> ExplicitExpertProviderSession = GetObjectFieldOrNull(SessionObject, TEXT("explicit_expert_opt_in_provider"));
	TestTrue(TEXT("current_provider should exist"), CurrentProviderSession.IsValid());
	TestTrue(TEXT("other_provider should exist"), OtherProviderSession.IsValid());
	TestTrue(TEXT("legacy_shared should exist"), LegacySharedSession.IsValid());
	TestTrue(TEXT("explicit_expert_opt_in_provider should exist"), ExplicitExpertProviderSession.IsValid());
	if (!CurrentProviderSession.IsValid() || !OtherProviderSession.IsValid() || !LegacySharedSession.IsValid() || !ExplicitExpertProviderSession.IsValid())
	{
		return false;
	}

	FString CodexHomePath;
	FString CurrentProviderPath;
	FString OtherProviderPath;
	FString LegacySharedPath;
	FString ExplicitExpertProviderPath;
	bool bCurrentProviderIsLegacyShared = true;
	bool bOtherProviderIsLegacyShared = true;
	bool bLegacySharedIsLegacyShared = false;
	bool bExplicitExpertProviderIsLegacyShared = true;
	TestTrue(TEXT("codex_home_path should exist"), BackendObject->TryGetStringField(TEXT("codex_home_path"), CodexHomePath));
	TestTrue(TEXT("current_provider.path should exist"), CurrentProviderSession->TryGetStringField(TEXT("path"), CurrentProviderPath));
	TestTrue(TEXT("other_provider.path should exist"), OtherProviderSession->TryGetStringField(TEXT("path"), OtherProviderPath));
	TestTrue(TEXT("legacy_shared.path should exist"), LegacySharedSession->TryGetStringField(TEXT("path"), LegacySharedPath));
	TestTrue(TEXT("explicit_expert_opt_in_provider.path should exist"), ExplicitExpertProviderSession->TryGetStringField(TEXT("path"), ExplicitExpertProviderPath));
	TestTrue(TEXT("current_provider.legacy_shared_file should exist"), CurrentProviderSession->TryGetBoolField(TEXT("legacy_shared_file"), bCurrentProviderIsLegacyShared));
	TestTrue(TEXT("other_provider.legacy_shared_file should exist"), OtherProviderSession->TryGetBoolField(TEXT("legacy_shared_file"), bOtherProviderIsLegacyShared));
	TestTrue(TEXT("legacy_shared.legacy_shared_file should exist"), LegacySharedSession->TryGetBoolField(TEXT("legacy_shared_file"), bLegacySharedIsLegacyShared));
	TestTrue(TEXT("explicit_expert_opt_in_provider.legacy_shared_file should exist"), ExplicitExpertProviderSession->TryGetBoolField(TEXT("legacy_shared_file"), bExplicitExpertProviderIsLegacyShared));

	const FString ProjectSavedRoot = NormalizeDirectoryPathForTest(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE")));
	const FString LegacyProjectSavedRoot = NormalizeDirectoryPathForTest(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE")));
	const FString NormalizedCodexHomePath = NormalizeDirectoryPathForTest(CodexHomePath);
	TestTrue(TEXT("shared machine auth home should stay outside the project-local Saved/OsvayderUE root"),
		!NormalizedCodexHomePath.StartsWith(ProjectSavedRoot, ESearchCase::IgnoreCase));
	TestTrue(TEXT("current provider session path should stay empty or project-local"),
		CurrentProviderPath.IsEmpty() || NormalizeDirectoryPathForTest(CurrentProviderPath).StartsWith(ProjectSavedRoot, ESearchCase::IgnoreCase));
	TestTrue(TEXT("other provider session path should stay project-local"),
		NormalizeDirectoryPathForTest(OtherProviderPath).StartsWith(ProjectSavedRoot, ESearchCase::IgnoreCase));
	const FString NormalizedLegacySharedPath = NormalizeDirectoryPathForTest(LegacySharedPath);
	TestTrue(TEXT("legacy shared session path should stay project-local"),
		NormalizedLegacySharedPath.StartsWith(ProjectSavedRoot, ESearchCase::IgnoreCase)
		|| NormalizedLegacySharedPath.StartsWith(LegacyProjectSavedRoot, ESearchCase::IgnoreCase));
	TestTrue(TEXT("explicit expert provider session path should stay project-local"),
		NormalizeDirectoryPathForTest(ExplicitExpertProviderPath).StartsWith(ProjectSavedRoot, ESearchCase::IgnoreCase));
	TestFalse(TEXT("current provider session should not be marked as legacy shared"), bCurrentProviderIsLegacyShared);
	TestFalse(TEXT("other provider session should not be marked as legacy shared"), bOtherProviderIsLegacyShared);
	TestTrue(TEXT("legacy shared session metadata should stay marked as legacy shared"), bLegacySharedIsLegacyShared);
	TestFalse(TEXT("explicit expert provider session should not be marked as legacy shared"), bExplicitExpertProviderIsLegacyShared);
	return true;
}

bool FCodexCliRunner_ProjectLocalAuthModesUseOsvayderStorageRoot::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("ProjectLocalAuthModesUseOsvayderStorageRoot"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString ExpectedSavedRoot = NormalizeDirectoryPathForTest(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE")));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), TEXT("sk-test"));
	FScopedCodexHomeOverride ExplicitOverride(TEXT(""));

	{
		FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::ApiKeyEnvVar);
		const FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
		const FString ExpectedHome = NormalizeDirectoryPathForTest(FPaths::Combine(ExpectedSavedRoot, TEXT("codex_api_key_env_home")));
		TestEqual(TEXT("api-key env mode should use the preferred project-local home"), NormalizeDirectoryPathForTest(Diagnostics.EffectiveCodexHomePath), ExpectedHome);
	}

	{
		FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::BrowserVerify);
		const FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
		const FString ExpectedHome = NormalizeDirectoryPathForTest(FPaths::Combine(ExpectedSavedRoot, TEXT("codex_browser_verify_home")));
		TestEqual(TEXT("browser verify mode should use the preferred project-local home"), NormalizeDirectoryPathForTest(Diagnostics.EffectiveCodexHomePath), ExpectedHome);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_ApiKeyEnvVarPreferredAliasWins,
	"OsvayderUE.CodexCliRunner.AuthDiagnostics.ApiKeyEnvVarPreferredAliasWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_ApiKeyEnvVarPreferredAliasWins::RunTest(const FString& Parameters)
{
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedEnvironmentVariable LegacyApiKeyOverride(TEXT("OSVAYDERUE_OPENAI_API_KEY"), TEXT("sk-legacy"));
	FScopedEnvironmentVariable PreferredApiKeyOverride(TEXT("OSVAYDERUE_OPENAI_API_KEY"), TEXT("sk-preferred"));
	FScopedCodexApiKeyEnvVarOverride ApiKeyEnvVarOverride(UOsvayderUESettings::GetDefaultCodexApiKeyEnvVarName());
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::ApiKeyEnvVar);

	const FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
	TestEqual(TEXT("default configured API-key env var should be the preferred Osvayder alias"),
		FCodexCliRunner::GetConfiguredApiKeyEnvVarName(),
		FString(TEXT("OSVAYDERUE_OPENAI_API_KEY")));
	TestEqual(TEXT("preferred API-key alias should be detected as explicit API auth"),
		Diagnostics.EffectiveAuthEntryPath,
		FString(TEXT("api_env_var_bridge")));
	TestTrue(TEXT("diagnostics should name the preferred API-key env alias"),
		Diagnostics.AuthDetailText.Contains(TEXT("OSVAYDERUE_OPENAI_API_KEY"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("diagnostics should explain preferred API-key alias precedence over legacy"),
		Diagnostics.AuthDetailText.Contains(TEXT("OSVAYDERUE_OPENAI_API_KEY"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_ApiKeyEnvVarLegacyAliasFallback,
	"OsvayderUE.CodexCliRunner.AuthDiagnostics.ApiKeyEnvVarLegacyAliasFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_ApiKeyEnvVarLegacyAliasFallback::RunTest(const FString& Parameters)
{
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedEnvironmentVariable PreferredApiKeyOverride(TEXT("OSVAYDERUE_OPENAI_API_KEY"), FString());
	FScopedEnvironmentVariable LegacyApiKeyOverride(TEXT("OSVAYDERUE_OPENAI_API_KEY"), TEXT("sk-legacy"));
	FScopedCodexApiKeyEnvVarOverride ApiKeyEnvVarOverride(UOsvayderUESettings::GetDefaultCodexApiKeyEnvVarName());
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::ApiKeyEnvVar);

	const FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
	TestEqual(TEXT("legacy API-key alias should remain a fallback for default Osvayder config"),
		Diagnostics.EffectiveAuthEntryPath,
		FString(TEXT("api_env_var_bridge")));
	TestTrue(TEXT("diagnostics should name the legacy API-key fallback when used"),
		Diagnostics.AuthDetailText.Contains(TEXT("OSVAYDERUE_OPENAI_API_KEY"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_AuthDiagnostics_HomeResolutionDisplay,
	"OsvayderUE.CodexCliRunner.AuthDiagnostics.HomeResolutionDisplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_AuthDiagnostics_HomeResolutionDisplay::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("AuthDiagnosticsHomeResolutionDisplay"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CodexHome = FPaths::Combine(TestRoot, TEXT("CodexHome"));
	TestTrue(TEXT("codex artifact should be created"), WriteCodexArtifact(CodexHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedCodexHomeOverride CodexHomeOverride(CodexHome);
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::Auto);
	FScopedPersistentAppServerOverride PersistentTransportOverride(true);

	const FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
	TestEqual(TEXT("diagnostics should surface configured home"),
		NormalizeDirectoryPathForTest(Diagnostics.ConfiguredCodexHomePath),
		NormalizeDirectoryPathForTest(CodexHome));
	TestEqual(TEXT("diagnostics should surface effective launch home"),
		NormalizeDirectoryPathForTest(Diagnostics.EffectiveCodexHomePath),
		NormalizeDirectoryPathForTest(CodexHome));
	TestEqual(TEXT("diagnostics should classify artifact-only auth as unknown_unprobed"),
		Diagnostics.AuthState,
		FString(TEXT("unknown_unprobed")));
	TestTrue(TEXT("diagnostics should report artifact presence"), Diagnostics.bCredentialArtifactPresent);
	TestTrue(TEXT("diagnostics should include the artifact path"), Diagnostics.CredentialArtifactPath.EndsWith(TEXT("auth.json")));
	TestTrue(TEXT("diagnostics should report persistent app-server enabled"), Diagnostics.bPersistentAppServerEnabled);
	TestFalse(TEXT("diagnostics should surface profile label"), Diagnostics.ProfileLabel.IsEmpty());
	TestFalse(TEXT("diagnostics should surface work mode"), Diagnostics.WorkModeName.IsEmpty());

	const FString CompactText = FCodexCliRunner::BuildAuthDiagnosticsCompactText(Diagnostics);
	TestTrue(TEXT("compact text should show auth state"), CompactText.Contains(TEXT("state=unknown_unprobed")));
	TestTrue(TEXT("compact text should show CODEX_HOME"), CompactText.Contains(TEXT("CODEX_HOME=")));
	TestTrue(TEXT("compact text should show persistent transport"), CompactText.Contains(TEXT("app-server=enabled")));

	const FString ToolTipText = FCodexCliRunner::BuildAuthDiagnosticsToolTip(Diagnostics);
	TestTrue(TEXT("tooltip should show credential artifact path"), ToolTipText.Contains(TEXT("credential_artifact_path")));
	TestTrue(TEXT("tooltip should show effective home"), ToolTipText.Contains(TEXT("effective_codex_home")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_AuthDiagnostics_MissingArtifactStatus,
	"OsvayderUE.CodexCliRunner.AuthDiagnostics.MissingArtifactStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_AuthDiagnostics_MissingArtifactStatus::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("AuthDiagnosticsMissingArtifactStatus"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CodexHome = FPaths::Combine(TestRoot, TEXT("EmptyCodexHome"));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedCodexHomeOverride CodexHomeOverride(CodexHome);
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::Auto);

	const FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
	TestEqual(TEXT("missing artifact should classify as missing"), Diagnostics.AuthState, FString(TEXT("missing")));
	TestFalse(TEXT("missing artifact should not report credential presence"), Diagnostics.bCredentialArtifactPresent);
	TestTrue(TEXT("missing detail should mention no known Codex credential artifact"),
		Diagnostics.AuthDetailText.Contains(TEXT("No known Codex credential artifact"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_AuthDiagnostics_RefreshFailureClassifier,
	"OsvayderUE.CodexCliRunner.AuthDiagnostics.RefreshFailureClassifier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_AuthDiagnostics_RefreshFailureClassifier::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("refresh-token reuse should classify as expired_or_refresh_failed"),
		FCodexCliRunner::ClassifyAuthFailureMessage(TEXT("access token could not be refreshed because your refresh token was already used")),
		FString(TEXT("expired_or_refresh_failed")));
	TestEqual(
		TEXT("login required should classify as missing"),
		FCodexCliRunner::ClassifyAuthFailureMessage(TEXT("Authentication required: login required")),
		FString(TEXT("missing")));
	TestEqual(
		TEXT("launch failure should classify separately from auth"),
		FCodexCliRunner::ClassifyAuthFailureMessage(TEXT("Failed to launch Codex app-server")),
		FString(TEXT("transport_broken")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_AuthDiagnostics_BackupBeforeClear,
	"OsvayderUE.CodexCliRunner.AuthDiagnostics.BackupBeforeClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_AuthDiagnostics_BackupBeforeClear::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("AuthDiagnosticsBackupBeforeClear"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CodexHome = FPaths::Combine(TestRoot, TEXT("CodexHome"));
	TestTrue(TEXT("codex home should be created"), IFileManager::Get().MakeDirectory(*CodexHome, true));

	const FString AuthPath = FPaths::Combine(CodexHome, TEXT("auth.json"));
	const FString CredentialsPath = FPaths::Combine(CodexHome, TEXT("credentials.json"));
	TestTrue(TEXT("auth.json should be created"), FFileHelper::SaveStringToFile(TEXT("{\"access_token\":\"redacted-test\"}\n"), *AuthPath));
	TestTrue(TEXT("credentials.json should be created"), FFileHelper::SaveStringToFile(TEXT("{\"refresh_token\":\"redacted-test\"}\n"), *CredentialsPath));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedCodexHomeOverride CodexHomeOverride(CodexHome);
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::Auto);

	FString StatusMessage;
	TestTrue(TEXT("backup/clear action should succeed"), FCodexCliRunner::BackupAndClearStaleAuthArtifacts(StatusMessage));
	TestFalse(TEXT("auth.json should be removed after backup"), IFileManager::Get().FileExists(*AuthPath));
	TestFalse(TEXT("credentials.json should be removed after backup"), IFileManager::Get().FileExists(*CredentialsPath));

	FString BackupSuffix;
	TestTrue(TEXT("status should expose backup directory"), StatusMessage.Split(TEXT("Backup directory: "), nullptr, &BackupSuffix));
	FString BackupDir;
	TestTrue(TEXT("status should delimit backup directory"), BackupSuffix.Split(TEXT(". Effective"), &BackupDir, nullptr));
	BackupDir.TrimStartAndEndInline();
	const FString ExpectedDiagnosticsRoot = NormalizeDirectoryPathForTest(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("CodexAuthDiagnostics")));
	TestTrue(TEXT("auth diagnostics backups should write under the preferred OsvayderUE root"),
		NormalizeDirectoryPathForTest(BackupDir).StartsWith(ExpectedDiagnosticsRoot, ESearchCase::IgnoreCase));
	TestTrue(TEXT("backup auth.json should exist"), IFileManager::Get().FileExists(*FPaths::Combine(BackupDir, TEXT("auth.json"))));
	TestTrue(TEXT("backup credentials.json should exist"), IFileManager::Get().FileExists(*FPaths::Combine(BackupDir, TEXT("credentials.json"))));
	TestTrue(TEXT("backup manifest should exist"), IFileManager::Get().FileExists(*FPaths::Combine(BackupDir, TEXT("manifest.json"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_AuthDiagnostics_ProbeLaunchUsesEffectiveHome,
	"OsvayderUE.CodexCliRunner.AuthDiagnostics.ProbeLaunchUsesEffectiveHome",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_AuthDiagnostics_ProbeLaunchUsesEffectiveHome::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("AuthDiagnosticsProbeLaunchUsesEffectiveHome"));
	const FString NodeExecutablePath = ResolveNodeExecutableForTest();
	TestFalse(TEXT("node executable path should not be empty"), NodeExecutablePath.IsEmpty());
	if (NodeExecutablePath.IsEmpty())
	{
		return false;
	}

	const FString ScriptPath = WriteFakeCodexAppServerScript(TestRoot);
	TestFalse(TEXT("fake app-server script path should not be empty"), ScriptPath.IsEmpty());
	if (ScriptPath.IsEmpty())
	{
		return false;
	}

	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CodexHome = FPaths::Combine(TestRoot, TEXT("CodexHome"));
	const FString CodexHomeLogPath = FPaths::Combine(TestRoot, TEXT("codex_home_log.txt"));
	TestTrue(TEXT("fake codex auth artifact should be created"), WriteCodexArtifact(CodexHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedEnvironmentVariable ScenarioOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_SCENARIO"), TEXT("always_success"));
	FScopedEnvironmentVariable CodexHomeLogOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_CODEX_HOME_LOG"), CodexHomeLogPath);
	FScopedCodexHomeOverride CodexHomeOverride(CodexHome);
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::Auto);
	FScopedCodexLaunchOverride LaunchOverride(NodeExecutablePath, ScriptPath);

	const FCodexCliRunner::FCodexAuthDiagnostics Diagnostics = FCodexCliRunner::GetAuthDiagnostics();
	TestEqual(TEXT("diagnostic effective home should be the explicit test home"),
		NormalizeDirectoryPathForTest(Diagnostics.EffectiveCodexHomePath),
		NormalizeDirectoryPathForTest(CodexHome));

	FString StatusMessage;
	TestTrue(TEXT("probe should succeed against fake app-server"), FCodexCliRunner::ProbeBackendAuth(StatusMessage));
	TestTrue(TEXT("probe message should report ok"), StatusMessage.Contains(TEXT("state=ok")));
	TestTrue(TEXT("fake app-server should log CODEX_HOME"), IFileManager::Get().FileExists(*CodexHomeLogPath));

	FString CodexHomeLog;
	TestTrue(TEXT("CODEX_HOME log should be readable"), FFileHelper::LoadFileToString(CodexHomeLog, *CodexHomeLogPath));
	TArray<FString> LogLines;
	CodexHomeLog.ParseIntoArrayLines(LogLines);
	TestTrue(TEXT("CODEX_HOME log should have at least one line"), LogLines.Num() > 0);
	if (LogLines.Num() > 0)
	{
		FString LoggedIndex;
		FString LoggedHome;
		TestTrue(TEXT("CODEX_HOME log should include launch index and home"), LogLines[0].Split(TEXT(":"), &LoggedIndex, &LoggedHome));
		TestEqual(TEXT("probe app-server launch should use the diagnostic effective home"),
			NormalizeDirectoryPathForTest(LoggedHome),
			NormalizeDirectoryPathForTest(Diagnostics.EffectiveCodexHomePath));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_AuthDiagnostics_StatusTextMapping,
	"OsvayderUE.CodexCliRunner.AuthDiagnostics.StatusTextMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_AuthDiagnostics_StatusTextMapping::RunTest(const FString& Parameters)
{
	FCodexCliRunner::FCodexAuthDiagnostics Diagnostics;
	Diagnostics.AuthState = TEXT("expired_or_refresh_failed");
	Diagnostics.EffectiveCodexHomePath = TEXT("C:/Temp/CodexHome");
	Diagnostics.CredentialArtifactPath = TEXT("C:/Temp/CodexHome/auth.json");
	Diagnostics.ProfileLabel = TEXT("default");
	Diagnostics.ModelName = TEXT("gpt-test");
	Diagnostics.WorkModeName = TEXT("max");
	Diagnostics.bPersistentAppServerEnabled = true;

	const FString CompactText = FCodexCliRunner::BuildAuthDiagnosticsCompactText(Diagnostics);
	TestTrue(TEXT("compact status should include expired/refresh state"), CompactText.Contains(TEXT("state=expired_or_refresh_failed")));
	TestTrue(TEXT("compact status should include CODEX_HOME"), CompactText.Contains(TEXT("CODEX_HOME=C:/Temp/CodexHome")));
	TestTrue(TEXT("compact status should include model"), CompactText.Contains(TEXT("model=gpt-test")));

	const FString ToolTipText = FCodexCliRunner::BuildAuthDiagnosticsToolTip(Diagnostics);
	TestTrue(TEXT("tooltip should include auth state"), ToolTipText.Contains(TEXT("auth_state = expired_or_refresh_failed")));
	TestTrue(TEXT("tooltip should include artifact path"), ToolTipText.Contains(TEXT("credential_artifact_path = C:/Temp/CodexHome/auth.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_PersistentTransport_WorkspaceWriteSandboxPolicyTruth,
	"OsvayderUE.CodexCliRunner.PersistentTransport.WorkspaceWriteSandboxPolicyTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_PersistentTransport_WorkspaceWriteSandboxPolicyTruth::RunTest(const FString& Parameters)
{
	const FAgentRequestConfig Config = MakeWorkspaceWriteDefaultConfigForPersistentSandbox();

	TestEqual(
		TEXT("workspace-write default persistent thread sandbox should stay workspace-write"),
		FCodexCliRunner::GetTestPersistentThreadSandboxMode(Config),
		FString(TEXT("workspace-write")));

	const TSharedPtr<FJsonObject> SandboxPolicy = FCodexCliRunner::MakeTestPersistentTurnSandboxPolicy(Config);
	TestTrue(TEXT("workspace-write default sandbox policy should be created"), SandboxPolicy.IsValid());
	if (!SandboxPolicy.IsValid())
	{
		return false;
	}

	FString SandboxType;
	TestTrue(TEXT("workspace-write default sandbox policy should expose type"), SandboxPolicy->TryGetStringField(TEXT("type"), SandboxType));
	TestEqual(TEXT("workspace-write default sandbox policy type should stay workspaceWrite"), SandboxType, FString(TEXT("workspaceWrite")));
	TestTrue(
		TEXT("workspace-write default sandbox policy should scope writable roots to the working directory"),
		JsonStringArrayContains(SandboxPolicy, TEXT("writableRoots"), FPaths::ProjectDir()));

#if PLATFORM_WINDOWS
	TestTrue(
		TEXT("workspace-write default should force unelevated Windows sandbox backend for persistent app-server"),
		FCodexCliRunner::ShouldTestForceWindowsUnelevatedWorkspaceWriteSandbox(Config));
#endif

	TestTrue(
		TEXT("workspace-write default should request the narrow restart-survival Unreal MCP bridge"),
		FCodexCliRunner::DoesTestConfigRequestUnrealMcpBridge(Config));
	TestEqual(
		TEXT("workspace-write default should scope the Unreal MCP bridge to restart_survival only"),
		FCodexCliRunner::GetTestRequestedUnrealMcpToolFilterCsv(Config),
		FString(TEXT("restart_survival")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_PersistentTransport_ExplicitExpertSandboxPolicyTruth,
	"OsvayderUE.CodexCliRunner.PersistentTransport.ExplicitExpertSandboxPolicyTruth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_PersistentTransport_ExplicitExpertSandboxPolicyTruth::RunTest(const FString& Parameters)
{
	const FAgentRequestConfig Config = MakeExplicitExpertConfigForPersistentSandbox();

	TestEqual(
		TEXT("explicit expert persistent thread sandbox should stay danger-full-access"),
		FCodexCliRunner::GetTestPersistentThreadSandboxMode(Config),
		FString(TEXT("danger-full-access")));

	const TSharedPtr<FJsonObject> SandboxPolicy = FCodexCliRunner::MakeTestPersistentTurnSandboxPolicy(Config);
	TestTrue(TEXT("explicit expert sandbox policy should be created"), SandboxPolicy.IsValid());
	if (!SandboxPolicy.IsValid())
	{
		return false;
	}

	FString SandboxType;
	TestTrue(TEXT("explicit expert sandbox policy should expose type"), SandboxPolicy->TryGetStringField(TEXT("type"), SandboxType));
	TestEqual(TEXT("explicit expert sandbox policy type should stay dangerFullAccess"), SandboxType, FString(TEXT("dangerFullAccess")));
	TestFalse(TEXT("explicit expert sandbox policy should not expose workspace writable roots"), SandboxPolicy->HasField(TEXT("writableRoots")));

#if PLATFORM_WINDOWS
	TestFalse(
		TEXT("explicit expert should not force unelevated Windows sandbox backend"),
		FCodexCliRunner::ShouldTestForceWindowsUnelevatedWorkspaceWriteSandbox(Config));
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_PersistentTransport_TimeoutRecoveryStartsFreshTransport,
	"OsvayderUE.CodexCliRunner.PersistentTransport.TimeoutRecoveryStartsFreshTransport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_PersistentTransport_ExportRestartSurvivalStateFallsBackToPersistedSnapshot,
	"OsvayderUE.CodexCliRunner.PersistentTransport.ExportRestartSurvivalStateFallsBackToPersistedSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_PersistentTransport_ResetConversationClearsLegacySnapshotFallback,
	"OsvayderUE.CodexCliRunner.PersistentTransport.ResetConversationClearsLegacySnapshotFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_PersistentTransport_TimeoutRecoveryStartsFreshTransport::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("PersistentTransportTimeoutRecoveryStartsFreshTransport"));
	const FString NodeExecutablePath = ResolveNodeExecutableForTest();
	TestFalse(TEXT("node executable path should not be empty"), NodeExecutablePath.IsEmpty());
	if (NodeExecutablePath.IsEmpty())
	{
		return false;
	}

	const FString ScriptPath = WriteFakeCodexAppServerScript(TestRoot);
	TestFalse(TEXT("fake app-server script path should not be empty"), ScriptPath.IsEmpty());
	if (ScriptPath.IsEmpty())
	{
		return false;
	}

	const FString CodexHome = FPaths::Combine(TestRoot, TEXT("CodexHome"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString MarkerPath = FPaths::Combine(TestRoot, TEXT("timeout_marker.txt"));
	const FString CounterPath = FPaths::Combine(TestRoot, TEXT("launch_counter.txt"));
	TestTrue(TEXT("fake codex auth artifact should be created"), WriteCodexArtifact(CodexHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedEnvironmentVariable ScenarioOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_SCENARIO"), TEXT("turn_timeout_once_then_success"));
	FScopedEnvironmentVariable MarkerOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_MARKER"), MarkerPath);
	FScopedEnvironmentVariable CounterOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_COUNTER"), CounterPath);
	FScopedCodexHomeOverride CodexHomeOverride(CodexHome);
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::Auto);
	FScopedPersistentAppServerOverride PersistentTransportOverride(true);
	FScopedCodexLaunchOverride LaunchOverride(NodeExecutablePath, ScriptPath);

	FAgentRequestConfig Config = MakeLivePersistentTransportConfig(0.2f);
	TestEqual(TEXT("persistent request timeout should honor config seconds"), FCodexCliRunner::GetTestPersistentRequestTimeoutMs(Config), 200);

	FCodexCliRunner Runner;
	FString FirstResponse;
	TestFalse(TEXT("first persistent run should time out"), Runner.ExecuteSync(Config, FirstResponse));
	TestTrue(TEXT("timeout diagnostic should be surfaced"), FirstResponse.Contains(TEXT("Timed out waiting for Codex persistent app-server"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("timeout diagnostic should explain transport reset"), FirstResponse.Contains(TEXT("Persistent transport was reset"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("timeout marker should be written by the fake app-server"), IFileManager::Get().FileExists(*MarkerPath));

	FString SecondResponse;
	TestTrue(TEXT("second persistent run should recover on a fresh process"), Runner.ExecuteSync(Config, SecondResponse));
	TestTrue(TEXT("second response should come from a new fake app-server launch"), SecondResponse.Contains(TEXT("launch=2"), ESearchCase::IgnoreCase));
	return true;
}

bool FCodexCliRunner_PersistentTransport_ExportRestartSurvivalStateFallsBackToPersistedSnapshot::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("PersistentTransportExportRestartSurvivalStateFallsBackToPersistedSnapshot"));
	const FString NodeExecutablePath = ResolveNodeExecutableForTest();
	TestFalse(TEXT("node executable path should not be empty"), NodeExecutablePath.IsEmpty());
	if (NodeExecutablePath.IsEmpty())
	{
		return false;
	}

	const FString ScriptPath = WriteFakeCodexAppServerScript(TestRoot);
	TestFalse(TEXT("fake app-server script path should not be empty"), ScriptPath.IsEmpty());
	if (ScriptPath.IsEmpty())
	{
		return false;
	}

	const FString CodexHome = FPaths::Combine(TestRoot, TEXT("CodexHome"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	TestTrue(TEXT("fake codex auth artifact should be created"), WriteCodexArtifact(CodexHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedEnvironmentVariable ScenarioOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_SCENARIO"), TEXT("always_success"));
	FScopedEnvironmentVariable MarkerOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_MARKER"), FString());
	FScopedEnvironmentVariable CounterOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_COUNTER"), FString());
	FScopedCodexHomeOverride CodexHomeOverride(CodexHome);
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::Auto);
	FScopedPersistentAppServerOverride PersistentTransportOverride(true);
	FScopedCodexLaunchOverride LaunchOverride(NodeExecutablePath, ScriptPath);

	FAgentRequestConfig Config = MakeLivePersistentTransportConfig(1.0f);
	Config.bUseRestartSurvivalProviderThreadState = true;
	FCodexCliRunner Runner;

	FString FirstResponse;
	TestTrue(TEXT("persistent run should succeed before restart-survival export fallback"), Runner.ExecuteSync(Config, FirstResponse));
	TestFalse(TEXT("active thread id should be available after a successful persistent run"), Runner.GetActivePersistentThreadId().IsEmpty());

	Runner.ClearTestInMemoryPersistentThreadId();
	TestTrue(TEXT("test helper should clear the in-memory thread id"), Runner.GetActivePersistentThreadId().IsEmpty());

	FString ExportedStatePath;
	FString ExportedThreadId;
	TestTrue(
		TEXT("restart-survival export should fall back to the persisted thread snapshot when in-memory state was cleared"),
		Runner.ExportActiveThreadStateForRestartSurvival(ExportedStatePath, ExportedThreadId));
	TestFalse(TEXT("exported thread id should come from the persisted snapshot"), ExportedThreadId.IsEmpty());
	TestTrue(TEXT("exported state path should still exist on disk"), IFileManager::Get().FileExists(*ExportedStatePath));
	return true;
}

bool FCodexCliRunner_PersistentTransport_ResetConversationClearsLegacySnapshotFallback::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("PersistentTransportResetConversationClearsLegacySnapshotFallback"));
	const FString NodeExecutablePath = ResolveNodeExecutableForTest();
	TestFalse(TEXT("node executable path should not be empty"), NodeExecutablePath.IsEmpty());
	if (NodeExecutablePath.IsEmpty())
	{
		return false;
	}

	const FString ScriptPath = WriteFakeCodexAppServerScript(TestRoot);
	TestFalse(TEXT("fake app-server script path should not be empty"), ScriptPath.IsEmpty());
	if (ScriptPath.IsEmpty())
	{
		return false;
	}

	const FString CodexHome = FPaths::Combine(TestRoot, TEXT("CodexHome"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CounterPath = FPaths::Combine(TestRoot, TEXT("launch_counter.txt"));
	TestTrue(TEXT("fake codex auth artifact should be created"), WriteCodexArtifact(CodexHome));

	const FString PreferredStatePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("codex_persistent_thread.json"));
	const FString LegacyStatePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OsvayderUE"), TEXT("codex_persistent_thread.json"));
	IFileManager::Get().Delete(*PreferredStatePath, false, true, true);
	IFileManager::Get().Delete(*LegacyStatePath, false, true, true);

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedEnvironmentVariable ScenarioOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_SCENARIO"), TEXT("always_success"));
	FScopedEnvironmentVariable MarkerOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_MARKER"), FString());
	FScopedEnvironmentVariable CounterOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_COUNTER"), CounterPath);
	FScopedCodexHomeOverride CodexHomeOverride(CodexHome);
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::Auto);
	FScopedPersistentAppServerOverride PersistentTransportOverride(true);
	FScopedCodexLaunchOverride LaunchOverride(NodeExecutablePath, ScriptPath);

	FAgentRequestConfig Config = MakeLivePersistentTransportConfig(1.0f);
	Config.bUseRestartSurvivalProviderThreadState = true;

	FCodexCliRunner Runner;
	FString FirstResponse;
	TestTrue(TEXT("first persistent run should succeed"), Runner.ExecuteSync(Config, FirstResponse));
	TestTrue(TEXT("first run should start a fresh thread"), FirstResponse.Contains(TEXT("threadMethod=thread/start"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("first run should use the first fake thread id"), FirstResponse.Contains(TEXT("threadId=thread-1"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("preferred persistent thread snapshot should exist after first run"), IFileManager::Get().FileExists(*PreferredStatePath));

	FString SnapshotJson;
	TestTrue(TEXT("preferred persistent thread snapshot should be readable"), FFileHelper::LoadFileToString(SnapshotJson, *PreferredStatePath));
	TestTrue(TEXT("legacy-only persistent thread snapshot should save"), FFileHelper::SaveStringToFile(SnapshotJson, *LegacyStatePath));
	TestTrue(TEXT("preferred snapshot should be removable to seed legacy-only state"), IFileManager::Get().Delete(*PreferredStatePath, false, true, true));
	TestFalse(TEXT("preferred snapshot should be absent before reset"), IFileManager::Get().FileExists(*PreferredStatePath));
	TestTrue(TEXT("legacy snapshot should exist before reset"), IFileManager::Get().FileExists(*LegacyStatePath));

	Runner.ResetConversation();
	TestFalse(TEXT("preferred snapshot should be absent after reset"), IFileManager::Get().FileExists(*PreferredStatePath));
	TestFalse(TEXT("legacy snapshot should be absent after reset"), IFileManager::Get().FileExists(*LegacyStatePath));

	FString SecondResponse;
	TestTrue(TEXT("second persistent run should succeed after reset"), Runner.ExecuteSync(Config, SecondResponse));
	TestTrue(TEXT("second response should come from a fresh app-server launch"), SecondResponse.Contains(TEXT("launch=2"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("second run should start a fresh thread instead of resuming legacy state"), SecondResponse.Contains(TEXT("threadMethod=thread/start"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("second run should allocate a fresh fake thread id"), SecondResponse.Contains(TEXT("threadId=thread-2"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("second run should not resume the legacy thread id"), SecondResponse.Contains(TEXT("threadMethod=thread/resume"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_PersistentTransport_ResetConversationRestartsProcess,
	"OsvayderUE.CodexCliRunner.PersistentTransport.ResetConversationRestartsProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodexCliRunner_PersistentTransport_ScopedMcpFilterChangeRestartsProcess,
	"OsvayderUE.CodexCliRunner.PersistentTransport.ScopedMcpFilterChangeRestartsProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexCliRunner_PersistentTransport_ResetConversationRestartsProcess::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("PersistentTransportResetConversationRestartsProcess"));
	const FString NodeExecutablePath = ResolveNodeExecutableForTest();
	TestFalse(TEXT("node executable path should not be empty"), NodeExecutablePath.IsEmpty());
	if (NodeExecutablePath.IsEmpty())
	{
		return false;
	}

	const FString ScriptPath = WriteFakeCodexAppServerScript(TestRoot);
	TestFalse(TEXT("fake app-server script path should not be empty"), ScriptPath.IsEmpty());
	if (ScriptPath.IsEmpty())
	{
		return false;
	}

	const FString CodexHome = FPaths::Combine(TestRoot, TEXT("CodexHome"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CounterPath = FPaths::Combine(TestRoot, TEXT("launch_counter.txt"));
	const FString AllowedToolsLogPath = FPaths::Combine(TestRoot, TEXT("allowed_tools_log.txt"));
	TestTrue(TEXT("fake codex auth artifact should be created"), WriteCodexArtifact(CodexHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedEnvironmentVariable ScenarioOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_SCENARIO"), TEXT("always_success"));
	FScopedEnvironmentVariable MarkerOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_MARKER"), FString());
	FScopedEnvironmentVariable CounterOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_COUNTER"), CounterPath);
	FScopedEnvironmentVariable AllowedToolsLogOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_ALLOWED_TOOLS_LOG"), AllowedToolsLogPath);
	FScopedCodexHomeOverride CodexHomeOverride(CodexHome);
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::Auto);
	FScopedPersistentAppServerOverride PersistentTransportOverride(true);
	FScopedCodexLaunchOverride LaunchOverride(NodeExecutablePath, ScriptPath);

	const FAgentRequestConfig Config = MakeLivePersistentTransportConfig(1.0f);
	FCodexCliRunner Runner;

	FString FirstResponse;
	TestTrue(TEXT("first persistent run should succeed"), Runner.ExecuteSync(Config, FirstResponse));
	TestTrue(TEXT("first response should come from the first fake app-server launch"), FirstResponse.Contains(TEXT("launch=1"), ESearchCase::IgnoreCase));

	Runner.ResetConversation();

	FString SecondResponse;
	TestTrue(TEXT("second persistent run after reset should succeed"), Runner.ExecuteSync(Config, SecondResponse));
	TestTrue(TEXT("reset should force a fresh fake app-server launch"), SecondResponse.Contains(TEXT("launch=2"), ESearchCase::IgnoreCase));
	return true;
}

bool FCodexCliRunner_PersistentTransport_ScopedMcpFilterChangeRestartsProcess::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeFreshTestRoot(TEXT("PersistentTransportScopedMcpFilterChangeRestartsProcess"));
	const FString NodeExecutablePath = ResolveNodeExecutableForTest();
	TestFalse(TEXT("node executable path should not be empty"), NodeExecutablePath.IsEmpty());
	if (NodeExecutablePath.IsEmpty())
	{
		return false;
	}

	const FString ScriptPath = WriteFakeCodexAppServerScript(TestRoot);
	TestFalse(TEXT("fake app-server script path should not be empty"), ScriptPath.IsEmpty());
	if (ScriptPath.IsEmpty())
	{
		return false;
	}

	const FString CodexHome = FPaths::Combine(TestRoot, TEXT("CodexHome"));
	const FString UserHome = FPaths::Combine(TestRoot, TEXT("UserHome"));
	const FString CounterPath = FPaths::Combine(TestRoot, TEXT("launch_counter.txt"));
	const FString AllowedToolsLogPath = FPaths::Combine(TestRoot, TEXT("allowed_tools_log.txt"));
	TestTrue(TEXT("fake codex auth artifact should be created"), WriteCodexArtifact(CodexHome));

	FScopedEnvironmentVariable UserProfileOverride(TEXT("USERPROFILE"), UserHome);
	FScopedEnvironmentVariable HomeOverride(TEXT("HOME"), UserHome);
	FScopedEnvironmentVariable MachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OsvayderMachineEnvOverride(TEXT("OSVAYDERUE_CODEX_HOME"), FString());
	FScopedEnvironmentVariable OpenAiKeyOverride(TEXT("OPENAI_API_KEY"), FString());
	FScopedEnvironmentVariable ScenarioOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_SCENARIO"), TEXT("always_success"));
	FScopedEnvironmentVariable MarkerOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_MARKER"), FString());
	FScopedEnvironmentVariable CounterOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_COUNTER"), CounterPath);
	FScopedEnvironmentVariable AllowedToolsLogOverride(TEXT("OSVAYDERUE_FAKE_APP_SERVER_ALLOWED_TOOLS_LOG"), AllowedToolsLogPath);
	FScopedCodexHomeOverride CodexHomeOverride(CodexHome);
	FScopedCodexAuthModeOverride AuthModeOverride(EOsvayderUECodexAuthMode::Auto);
	FScopedPersistentAppServerOverride PersistentTransportOverride(true);
	FScopedCodexLaunchOverride LaunchOverride(NodeExecutablePath, ScriptPath);

	FAgentRequestConfig InputConfig = MakeLivePersistentTransportConfig(1.0f);
	InputConfig.bEnableUnrealMcpBridge = false;
	InputConfig.AllowedTools.Add(TEXT("mcp__osvayderue__enhanced_input"));
	TestTrue(TEXT("input config should still request the Unreal MCP bridge through scoped tools"), FCodexCliRunner::DoesTestConfigRequestUnrealMcpBridge(InputConfig));
	TestEqual(TEXT("input config should resolve to the expected scoped Unreal MCP filter"), FCodexCliRunner::GetTestRequestedUnrealMcpToolFilterCsv(InputConfig), FString(TEXT("enhanced_input,restart_survival")));

	FAgentRequestConfig AnimationConfig = MakeLivePersistentTransportConfig(1.0f);
	AnimationConfig.bEnableUnrealMcpBridge = false;
	AnimationConfig.AllowedTools.Add(TEXT("mcp__osvayderue__anim_blueprint_modify"));
	TestTrue(TEXT("animation config should still request the Unreal MCP bridge through scoped tools"), FCodexCliRunner::DoesTestConfigRequestUnrealMcpBridge(AnimationConfig));
	TestEqual(TEXT("animation config should resolve to the expected scoped Unreal MCP filter"), FCodexCliRunner::GetTestRequestedUnrealMcpToolFilterCsv(AnimationConfig), FString(TEXT("anim_blueprint_modify,restart_survival")));

	FCodexCliRunner Runner;

	FString FirstResponse;
	TestTrue(TEXT("first persistent run should succeed"), Runner.ExecuteSync(InputConfig, FirstResponse));
	TestTrue(TEXT("first response should come from the first fake app-server launch"), FirstResponse.Contains(TEXT("launch=1"), ESearchCase::IgnoreCase));

	FString LaunchCounterAfterFirstRun;
	TestTrue(TEXT("launch counter should be readable after the first run"), FFileHelper::LoadFileToString(LaunchCounterAfterFirstRun, *CounterPath));
	TestEqual(TEXT("first run should launch exactly one fake app-server process"), LaunchCounterAfterFirstRun.TrimStartAndEnd(), FString(TEXT("1")));

	FString SecondResponse;
	TestTrue(TEXT("second persistent run should succeed after scoped filter change"), Runner.ExecuteSync(AnimationConfig, SecondResponse));

	FString LaunchCounterAfterSecondRun;
	TestTrue(TEXT("launch counter should be readable after the second run"), FFileHelper::LoadFileToString(LaunchCounterAfterSecondRun, *CounterPath));
	TestEqual(TEXT("changing the scoped MCP filter should force a fresh fake app-server launch"), LaunchCounterAfterSecondRun.TrimStartAndEnd(), FString(TEXT("2")));

	FString AllowedToolsLog;
	TestTrue(TEXT("allowed tools log should be readable after both runs"), FFileHelper::LoadFileToString(AllowedToolsLog, *AllowedToolsLogPath));
	TestTrue(TEXT("first launch should carry the input scoped MCP filter"), AllowedToolsLog.Contains(TEXT("1:enhanced_input,restart_survival"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("second launch should carry the animation scoped MCP filter"), AllowedToolsLog.Contains(TEXT("2:anim_blueprint_modify,restart_survival"), ESearchCase::IgnoreCase));
	return true;
}

#endif
