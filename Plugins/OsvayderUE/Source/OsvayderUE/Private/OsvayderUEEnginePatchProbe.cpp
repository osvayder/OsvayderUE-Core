// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUEEnginePatchProbe.h"

#include "OsvayderSubsystem.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "OsvayderUEAgentTrace.h"
#include "OsvayderUEModule.h"

namespace
{
	// Canonical signatures for patch-presence detection. Mirrors the
	// corresponding strings emitted by `Apply_628v3_LiveCodingLimitPatch.ps1`
	// when applying the engine-source modification.
	const TCHAR* GPatchCommentSignature = TEXT("OsvayderUE 628-v3 patch");
	const TCHAR* GPatchLineSignature    = TEXT("Arguments += TEXT(\" -LiveCodingLimit=1000\");");

	const TCHAR* GEngineSourceRelativeSuffix =
		TEXT("Source/Developer/Windows/LiveCoding/Private/LiveCodingModule.cpp");
}

FString FOsvayderUEEnginePatchProbe::GetEngineSourceAbsolutePath()
{
	// FPaths::EngineDir() returns either relative (from cwd) or absolute
	// path depending on platform + install shape. ConvertRelativePathToFull
	// normalizes to absolute in either case.
	const FString EngineDirAbs = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
	return FPaths::Combine(EngineDirAbs, GEngineSourceRelativeSuffix);
}

bool FOsvayderUEEnginePatchProbe::DetectPatchPresence(const FString& EngineSourceContent)
{
	if (EngineSourceContent.IsEmpty())
	{
		return false;
	}
	// OR logic — either signature confirms. Robust against formatting
	// drift that might alter one signature but not both.
	return EngineSourceContent.Contains(GPatchCommentSignature, ESearchCase::CaseSensitive)
		|| EngineSourceContent.Contains(GPatchLineSignature, ESearchCase::CaseSensitive);
}

void FOsvayderUEEnginePatchProbe::RunStartupProbe()
{
	const FString EngineSourcePath = GetEngineSourceAbsolutePath();
	const FString DetectionTimestampUtc = FDateTime::UtcNow().ToString();

	bool bPatchPresent = false;
	bool bFileReadOk = false;
	FString EngineSourceContent;

	if (!IFileManager::Get().FileExists(*EngineSourcePath))
	{
		UE_LOG(LogOsvayderUE, Warning,
			TEXT("Engine patch probe: target file not found at %s. Non-Epic-installed engine layout? Skipping patch-presence check."),
			*EngineSourcePath);
	}
	else
	{
		bFileReadOk = FFileHelper::LoadFileToString(EngineSourceContent, *EngineSourcePath);
		if (!bFileReadOk)
		{
			UE_LOG(LogOsvayderUE, Warning,
				TEXT("Engine patch probe: could not read %s (permissions? locked file?). Skipping patch-presence check."),
				*EngineSourcePath);
		}
		else
		{
			bPatchPresent = DetectPatchPresence(EngineSourceContent);
		}
	}

	// Emit log + agent_trace regardless of file-read outcome so the state
	// is always observable in the transcript.
	if (bPatchPresent)
	{
		UE_LOG(LogOsvayderUE, Verbose,
			TEXT("Engine patch LiveCodingModule.cpp:1294 verified present."));
	}
	else if (bFileReadOk)
	{
		UE_LOG(LogOsvayderUE, Warning,
			TEXT("Engine patch LiveCodingModule.cpp:1294 missing (likely engine reinstall overwrote it). "
				 "In-session LiveCoding may hit environmental 100-action limit. "
				 "Re-apply via 628-v3 task or workflow-equivalent: "
				 "run Plugins/OsvayderUE/Script/Apply_628v3_LiveCodingLimitPatch.ps1 from an elevated PowerShell."));
	}
	// else: already warned above (file missing or unreadable).

	// Emit structured agent_trace event. Backend = configured (for attribution
	// — the event is plugin-state, not per-backend, but the event sink needs
	// a backend key).
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("patch_name"), TEXT("628-v3 LiveCoding limit"));
	Payload->SetBoolField(TEXT("patch_present"), bPatchPresent);
	Payload->SetBoolField(TEXT("engine_source_readable"), bFileReadOk);
	Payload->SetStringField(TEXT("engine_source_path"), EngineSourcePath);
	Payload->SetStringField(TEXT("detection_timestamp_utc"), DetectionTimestampUtc);

	const EOsvayderUEProviderBackend Backend = FOsvayderCodeSubsystem::Get().GetConfiguredBackend();
	FOsvayderUEAgentTraceLog::Get().AppendEvent(
		TEXT("engine_patch_status"),
		Backend,
		Payload);
}
