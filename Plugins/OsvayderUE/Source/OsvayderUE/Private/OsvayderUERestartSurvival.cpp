// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUERestartSurvival.h"
#include "OsvayderUECommandClassification.h" // 626 P3: shared command classifiers promoted from this file's anon namespace
#include "OsvayderUERelayAgent.h"
#include "JsonUtils.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "OsvayderUEStorageMigration.h"

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
	FString GTestRestartSurvivalRootOverride;
#endif

	constexpr int32 RestartSurvivalSchemaVersion = 2;
	constexpr TCHAR RestartSurvivalSupervisorScriptName[] = TEXT("OsvayderUE-RestartSurvival.ps1");
	constexpr TCHAR RestartSurvivalMonitorScriptName[] = TEXT("OsvayderUE-RestartSurvivalMonitor.ps1");
	constexpr TCHAR RestartSurvivalPreflightScriptName[] = TEXT("Launch-OsvayderPlugin-WithPreflight.ps1");

	struct FPreparedRequestAutoStartArm
	{
		FString RequestId;
		EOsvayderUEProviderBackend Backend = EOsvayderUEProviderBackend::CodexCli;
		FString ProjectRoot;
		FString ProviderSessionId;
	};

	TOptional<FPreparedRequestAutoStartArm> GPreparedRequestAutoStartArm;
	TOptional<FOsvayderUERestartSurvivalPreparedStartOverride> GPreparedRequestStartOverride;

	FString GetOsvayderUEPluginBaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("OsvayderUE"));
		if (Plugin.IsValid())
		{
			return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		}

		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("OsvayderUE")));
	}

	FString GetPluginOwnedSupportBundleRoot()
	{
		return FPaths::Combine(GetOsvayderUEPluginBaseDir(), TEXT("Script"));
	}

	FString GetLegacyProjectSupportBundleRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Script")));
	}

	void PopulateSupportBundlePaths(
		const FString& BundleRoot,
		const FString& ResolutionLabel,
		FOsvayderUERestartSurvivalSupportBundle& OutBundle)
	{
		OutBundle.ResolutionLabel = ResolutionLabel;
		OutBundle.BundleRoot = FPaths::ConvertRelativePathToFull(BundleRoot);
		OutBundle.SupervisorScriptPath = FPaths::Combine(OutBundle.BundleRoot, RestartSurvivalSupervisorScriptName);
		OutBundle.MonitorScriptPath = FPaths::Combine(OutBundle.BundleRoot, RestartSurvivalMonitorScriptName);
		OutBundle.PreflightScriptPath = FPaths::Combine(OutBundle.BundleRoot, RestartSurvivalPreflightScriptName);
	}

	bool IsSupportBundleComplete(const FOsvayderUERestartSurvivalSupportBundle& Bundle)
	{
		return IFileManager::Get().FileExists(*Bundle.SupervisorScriptPath)
			&& IFileManager::Get().FileExists(*Bundle.MonitorScriptPath)
			&& IFileManager::Get().FileExists(*Bundle.PreflightScriptPath);
	}

	FString DescribeSupportBundleMissingMembers(const FOsvayderUERestartSurvivalSupportBundle& Bundle)
	{
		TArray<FString> MissingPaths;
		if (!IFileManager::Get().FileExists(*Bundle.SupervisorScriptPath))
		{
			MissingPaths.Add(Bundle.SupervisorScriptPath);
		}
		if (!IFileManager::Get().FileExists(*Bundle.MonitorScriptPath))
		{
			MissingPaths.Add(Bundle.MonitorScriptPath);
		}
		if (!IFileManager::Get().FileExists(*Bundle.PreflightScriptPath))
		{
			MissingPaths.Add(Bundle.PreflightScriptPath);
		}

		return MissingPaths.Num() > 0
			? FString::Join(MissingPaths, TEXT("; "))
			: FString();
	}

	bool TryResolveSupportBundleInternal(FOsvayderUERestartSurvivalSupportBundle& OutBundle, FString& OutError)
	{
		FOsvayderUERestartSurvivalSupportBundle PluginBundle;
		PopulateSupportBundlePaths(GetPluginOwnedSupportBundleRoot(), TEXT("plugin_owned"), PluginBundle);
		if (IsSupportBundleComplete(PluginBundle))
		{
			OutBundle = PluginBundle;
			OutError.Reset();
			return true;
		}

		FOsvayderUERestartSurvivalSupportBundle LegacyBundle;
		PopulateSupportBundlePaths(GetLegacyProjectSupportBundleRoot(), TEXT("legacy_project_root"), LegacyBundle);
		if (IsSupportBundleComplete(LegacyBundle))
		{
			OutBundle = LegacyBundle;
			OutError.Reset();
			return true;
		}

		OutBundle = PluginBundle;
		OutBundle.ResolutionLabel = TEXT("unresolved");
		OutError = FString::Printf(
			TEXT("Restart-survival support bundle is incomplete. Missing plugin-owned files: %s. Missing legacy project-root files: %s."),
			*DescribeSupportBundleMissingMembers(PluginBundle),
			*DescribeSupportBundleMissingMembers(LegacyBundle));
		return false;
	}

	EOsvayderUEProviderBackend ParseBackendIdentity(const FString& BackendName, const EOsvayderUEProviderBackend FallbackBackend)
	{
		if (BackendName.Equals(TEXT("CodexCli"), ESearchCase::IgnoreCase))
		{
			return EOsvayderUEProviderBackend::CodexCli;
		}

		if (BackendName.Equals(TEXT("ClaudeCli"), ESearchCase::IgnoreCase))
		{
			return EOsvayderUEProviderBackend::ClaudeCli;
		}

		return FallbackBackend;
	}

	FString GetStateRootDirInternal()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!GTestRestartSurvivalRootOverride.IsEmpty())
		{
			FString OverrideDir = FPaths::ConvertRelativePathToFull(GTestRestartSurvivalRootOverride);
			FPaths::NormalizeDirectoryName(OverrideDir);
			return OverrideDir;
		}
#endif

		return OsvayderUEStorageMigration::GetPreferredSavedRoot();
	}

	FString GetLegacyStateRootDirInternal()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!GTestRestartSurvivalRootOverride.IsEmpty())
		{
			FString OverrideDir = FPaths::ConvertRelativePathToFull(GTestRestartSurvivalRootOverride);
			FPaths::NormalizeDirectoryName(OverrideDir);
			return OsvayderUEStorageMigration::DeriveLegacyRootFromPreferred(OverrideDir);
		}
#endif

		return OsvayderUEStorageMigration::GetLegacySavedRoot();
	}

	FString GetStatePathInternal()
	{
		return FPaths::Combine(GetStateRootDirInternal(), TEXT("restart_survival_state.json"));
	}

	FString GetLegacyStatePathInternal()
	{
		return FPaths::Combine(GetLegacyStateRootDirInternal(), TEXT("restart_survival_state.json"));
	}

	FString GetPreparedRestoreRequestPathInternal()
	{
		return FPaths::Combine(GetStateRootDirInternal(), TEXT("restart_survival_restore_request.json"));
	}

	FString GetLegacyPreparedRestoreRequestPathInternal()
	{
		return FPaths::Combine(GetLegacyStateRootDirInternal(), TEXT("restart_survival_restore_request.json"));
	}

	FString GetClosedEditorResultPathInternal()
	{
		return FPaths::Combine(GetStateRootDirInternal(), TEXT("closed_editor_result.json"));
	}

	FString GetLegacyClosedEditorResultPathInternal()
	{
		return FPaths::Combine(GetLegacyStateRootDirInternal(), TEXT("closed_editor_result.json"));
	}

	FString ComputeRestartSurvivalPromptHash(const FString& Prompt)
	{
		FTCHARToUTF8 Utf8Prompt(*Prompt);
		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8Prompt.Get()), Utf8Prompt.Length());

		uint8 Digest[16];
		Md5.Final(Digest);
		return FString::Printf(TEXT("md5:%s"), *BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower());
	}

	FString NormalizeAbsoluteFilePath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return FString();
		}

		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	FString NormalizeAbsoluteDirectoryPath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return FString();
		}

		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	bool IsPathInsideDirectory(const FString& CandidatePath, const FString& DirectoryPath)
	{
		const FString NormalizedCandidate = NormalizeAbsoluteFilePath(CandidatePath);
		FString NormalizedDirectory = NormalizeAbsoluteDirectoryPath(DirectoryPath);
		if (NormalizedCandidate.IsEmpty() || NormalizedDirectory.IsEmpty())
		{
			return false;
		}

		if (!NormalizedDirectory.EndsWith(TEXT("/")))
		{
			NormalizedDirectory += TEXT("/");
		}

		return NormalizedCandidate.StartsWith(NormalizedDirectory, ESearchCase::IgnoreCase);
	}

	FString GetStringFieldOrEmpty(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
	{
		FString Value;
		FJsonUtils::GetStringField(Object, FieldName, Value);
		return Value;
	}

	bool GetObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TSharedPtr<FJsonObject>& OutObject)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* NestedObject = nullptr;
		if (!Object->TryGetObjectField(FieldName, NestedObject) || NestedObject == nullptr || !(*NestedObject).IsValid())
		{
			return false;
		}

		OutObject = *NestedObject;
		return true;
	}

	FString GetPreparedRequestContinuationIntentPrompt(const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request)
	{
		return Request.ContinuationIntentPrompt.IsEmpty()
			? Request.PostReattachCompletionText
			: Request.ContinuationIntentPrompt;
	}

	FString GetPreparedRequestPostReattachCompletionText(const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request)
	{
		return Request.PostReattachCompletionText.IsEmpty()
			? GetPreparedRequestContinuationIntentPrompt(Request)
			: Request.PostReattachCompletionText;
	}

	bool IsPreparedRequestRestoreEnabled(const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request);

	FString BuildPreparedRequestClosedEditorTransitionNoticeInternal(
		const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request)
	{
		return IsPreparedRequestRestoreEnabled(Request)
			? TEXT("Continuing outside the editor because an open-editor build lock was detected. Unreal will close, bounded restore/build work will continue, then the same task will reopen the editor and reattach.")
			: TEXT("Continuing outside the editor because an open-editor build lock was detected. Unreal will close, bounded file/build work will continue, then the same task will reopen the editor and reattach.");
	}

	FString MakePreparedRequestId()
	{
		return FString::Printf(TEXT("request_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	FString MakeAutonomousTaskId(const FString& ProviderSessionId)
	{
		FString SessionHint = ProviderSessionId;
		SessionHint.ReplaceInline(TEXT("-"), TEXT(""));
		SessionHint.ReplaceInline(TEXT("{"), TEXT(""));
		SessionHint.ReplaceInline(TEXT("}"), TEXT(""));
		if (SessionHint.IsEmpty())
		{
			SessionHint = TEXT("current");
		}

		SessionHint = SessionHint.Left(8);
		return FString::Printf(
			TEXT("task_autonomous_%s_%s"),
			*SessionHint,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	FString MakeSessionIdFromTaskId(const FString& TaskId)
	{
		return FString::Printf(TEXT("restart_survival_%s"), *TaskId);
	}

	FString BuildClosedEditorBuildBlockerPostReattachCompletionText(
		const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request,
		const FOsvayderUEClosedEditorBuildBlocker& Blocker)
	{
		const FString ContinuationPrefix = TEXT("Continue the same ordinary bounded project-local task after restart-survival reattach.");
		const FOsvayderUERestartSurvivalOriginTaskContext& OriginTask = Request.OriginTask;
		FString Prompt =
			TEXT("Continue the same ordinary bounded project-local task after restart-survival reattach.\n")
			TEXT("Detached closed-editor build/preflight work already completed while Unreal was closed.\n")
			TEXT("Do not call `restart_survival` again, do not close Unreal again, and do not use Stop-Process on UnrealEditor.\n")
			TEXT("Continue only in the reopened editor. Use verification only where needed to finish the original task truthfully, not as a replacement for it.");

		if (!OriginTask.OriginatingRunId.IsEmpty())
		{
			Prompt += FString::Printf(
				TEXT("\nOriginating pre-detach run id: %s."),
				*OriginTask.OriginatingRunId);
		}

		if (!OriginTask.OriginatingPromptHash.IsEmpty())
		{
			Prompt += FString::Printf(
				TEXT("\nOriginating prompt hash: %s."),
				*OriginTask.OriginatingPromptHash);
		}

		if (!OriginTask.OriginatingTaskMode.IsEmpty())
		{
			Prompt += FString::Printf(
				TEXT("\nOriginating task mode: %s."),
				*OriginTask.OriginatingTaskMode);
		}

		if (!OriginTask.OriginatingRequestedToolFamily.IsEmpty())
		{
			Prompt += FString::Printf(
				TEXT("\nOriginating requested tool family: %s."),
				*OriginTask.OriginatingRequestedToolFamily);
		}

		if (!OriginTask.OriginatingPrimaryMutationToolFamily.IsEmpty())
		{
			Prompt += FString::Printf(
				TEXT("\nOriginating primary mutation tool family: %s."),
				*OriginTask.OriginatingPrimaryMutationToolFamily);
		}

		if (!OriginTask.OriginatingUserPrompt.IsEmpty()
			&& !OriginTask.OriginatingUserPrompt.StartsWith(ContinuationPrefix, ESearchCase::CaseSensitive))
		{
			Prompt += FString::Printf(
				TEXT("\nOriginal task prompt follows exactly:\n%s"),
				*OriginTask.OriginatingUserPrompt);
		}
		else if (!OriginTask.OriginatingUserPrompt.IsEmpty())
		{
			Prompt += TEXT("\nOriginal task prompt is already preserved in restart-survival origin context; do not recurse another continuation wrapper.");
		}

		if (OriginTask.bOriginatingHasAttachments || OriginTask.bOriginatingHasVisualReference || OriginTask.bVisualProofRequired || OriginTask.bVisualQaManifestRequired)
		{
			Prompt += FString::Printf(
				TEXT("\nOriginal task visual/reference metadata: attachments=%d; visual_reference=%s; visual_proof_required=%s; visual_qa_manifest_required=%s."),
				OriginTask.OriginatingAttachedImagePaths.Num(),
				OriginTask.bOriginatingHasVisualReference ? TEXT("true") : TEXT("false"),
				OriginTask.bVisualProofRequired ? TEXT("true") : TEXT("false"),
				OriginTask.bVisualQaManifestRequired ? TEXT("true") : TEXT("false"));
			if (OriginTask.OriginatingAttachedImagePaths.Num() > 0)
			{
				Prompt += TEXT("\nOriginal attached image paths:");
				for (const FString& ImagePath : OriginTask.OriginatingAttachedImagePaths)
				{
					Prompt += FString::Printf(TEXT("\n- %s"), *ImagePath);
				}
			}
			if (!OriginTask.VisualReferenceRequirement.IsEmpty())
			{
				Prompt += FString::Printf(TEXT("\nVisual proof requirement: %s"), *OriginTask.VisualReferenceRequirement);
			}
			else
			{
				Prompt += TEXT("\nVisual proof requirement: final acceptance requires a visual_qa_manifest.json with verdict=passed and actual_screenshot_paths, or an explicit visual-proof blocker.");
			}
		}

		if (!Blocker.EscalationReason.IsEmpty())
		{
			Prompt += FString::Printf(
				TEXT("\nOpen-editor build lock already confirmed: %s"),
				*Blocker.EscalationReason);
		}

		if (Request.bRestoreEnabled
			&& !Request.AutosaveSourcePath.IsEmpty()
			&& !Request.TargetPath.IsEmpty())
		{
			Prompt += FString::Printf(
				TEXT("\nThe detached restore already copied \"%s\" to \"%s\".")
				TEXT("\nVerify that bounded restore result only as part of finishing the original task."),
				*Request.AutosaveSourcePath,
				*Request.TargetPath);
		}
		else if (Request.bFileWriteEnabled
			&& !Request.FileWriteSourcePath.IsEmpty()
			&& !Request.FileWriteTargetPath.IsEmpty())
		{
			Prompt += FString::Printf(
				TEXT("\nThe detached exact file write already copied \"%s\" to \"%s\".")
				TEXT("\nVerify that bounded file-write result only as part of finishing the original task."),
				*Request.FileWriteSourcePath,
				*Request.FileWriteTargetPath);
		}

		if (OriginTask.OriginatingTaskMode.Equals(TEXT("read_only_analysis"), ESearchCase::CaseSensitive))
		{
			Prompt += TEXT("\nThe originating task was already read-only, so remain read-only in the reopened editor unless new truthful evidence requires nothing more.");
		}
		else
		{
			Prompt += TEXT("\nDo not replace the original task with generic workspace inspection or read-only re-analysis.");
		}

		Prompt += TEXT("\nWhen the original task is complete, respond concisely.")
			TEXT("\nThe final answer must include: result status (full/partial/fail), concrete changes, known working, known non-working or unverified, and a short verification checklist in user language.");
		return Prompt;
	}

	FString ExtractClosedEditorBuildBlockerMatchedEvidence(
		const FString& OriginalText,
		const FString& Needle,
		const int32 ContextChars = 120)
	{
		if (OriginalText.IsEmpty() || Needle.IsEmpty())
		{
			return FString();
		}

		const int32 MatchIndex = OriginalText.Find(Needle, ESearchCase::IgnoreCase);
		if (MatchIndex == INDEX_NONE)
		{
			return FString();
		}

		const int32 StartIndex = FMath::Max(0, MatchIndex - ContextChars);
		const int32 EndIndex = FMath::Min(OriginalText.Len(), MatchIndex + Needle.Len() + ContextChars);
		return OriginalText.Mid(StartIndex, EndIndex - StartIndex).TrimStartAndEnd();
	}

	bool IsCommandExecutionToolResult(const FString& ToolName)
	{
		return ToolName.Equals(TEXT("command_execution"), ESearchCase::IgnoreCase)
			|| ToolName.EndsWith(TEXT("/command_execution"), ESearchCase::IgnoreCase)
			|| ToolName.Contains(TEXT("command_execution"), ESearchCase::IgnoreCase);
	}

	FString BuildClosedEditorBuildBlockerDetectionHaystack(
		const FString& ToolResultContent,
		const FString& RawProviderEvent)
	{
		if (RawProviderEvent.IsEmpty())
		{
			return ToolResultContent;
		}

		if (ToolResultContent.IsEmpty())
		{
			return RawProviderEvent;
		}

		return ToolResultContent + TEXT("\n") + RawProviderEvent;
	}

	// ------------------------------------------------------------------
	// 626 P2 Layer A build-context gate helpers.
	//
	// 626 P3 promotion note: the four functions (StripKnownShellWrapperPrefixes,
	// IsKnownInspectionCommand, IsBuildContextCommand, HasStructuredBuildOutputSignature)
	// previously lived in this anonymous namespace and are now shared via
	// `OsvayderUECommandClassification.h` so `FCompileIntentPolicyGate` and
	// future policy surfaces can reuse them without duplication. Behavior is
	// byte-equivalent; the move was pure refactoring.
	// ------------------------------------------------------------------

	// ------------------------------------------------------------------
	// 626 P2 Layer B: origin/evidence exclusion. Strip lines from the
	// haystack that are unambiguously repository inspection output:
	//   - ripgrep / grep / findstr `path:line:content` format
	//   - lines containing excluded origin paths under the plugin source
	//     tree, docs, or saved artifacts
	//   - lines that are obviously C++/TEXT(...) literals (detector source
	//     quoting itself)
	// If the remaining haystack after stripping no longer matches any
	// blocker family, the detector short-circuits. Defense-in-depth
	// behind Layer A.
	// ------------------------------------------------------------------

	bool LineMatchesRipGrepPrefix(const FString& Line)
	{
		// rg/grep format: `path:line:content` where path includes an
		// extension like .cpp/.h/.ps1/.md/.json/.txt/.log/.jsonl.
		// Simplified heuristic: look for `.(ext):<digit>+:` at any position
		// in the first ~200 chars. Robust against Windows paths (drive letter
		// makes a path like `D:\...:123:` ambiguous; we only match `.<ext>:<digit>+:`).
		static const TArray<FString> Extensions = {
			TEXT(".cpp:"), TEXT(".h:"), TEXT(".hpp:"), TEXT(".c:"),
			TEXT(".ps1:"), TEXT(".psm1:"),
			TEXT(".md:"), TEXT(".txt:"), TEXT(".log:"),
			TEXT(".json:"), TEXT(".jsonl:"), TEXT(".yaml:"), TEXT(".yml:"),
			TEXT(".cs:"), TEXT(".py:"), TEXT(".ini:"), TEXT(".bat:")
		};
		for (const FString& Ext : Extensions)
		{
			const int32 ExtIdx = Line.Find(Ext, ESearchCase::IgnoreCase);
			if (ExtIdx == INDEX_NONE)
			{
				continue;
			}
			// Require a digit right after the ext colon and another colon after digits.
			int32 Cursor = ExtIdx + Ext.Len();
			if (Cursor >= Line.Len() || !FChar::IsDigit(Line[Cursor]))
			{
				continue;
			}
			while (Cursor < Line.Len() && FChar::IsDigit(Line[Cursor]))
			{
				++Cursor;
			}
			if (Cursor < Line.Len() && Line[Cursor] == TEXT(':'))
			{
				return true;
			}
		}
		return false;
	}

	bool LineMatchesExcludedOriginPath(const FString& Line)
	{
		// Paths inside the plugin tree, docs, or saved artifacts that the
		// detector is known to reference. Any line containing these path
		// substrings is treated as self-reference.
		static const TArray<FString> OriginPaths = {
			TEXT("plugins\\osvayderue\\source\\"),
			TEXT("plugins/osvayderue/source/"),
			TEXT("plugins\\osvayderue\\docs\\"),
			TEXT("plugins/osvayderue/docs/"),
			TEXT("docs\\osvayderue\\"),
			TEXT("docs/osvayderue/"),
			TEXT("saved\\osvayderue\\"),
			TEXT("saved/osvayderue/"),
			TEXT("osvayderuerestartsurvival.cpp"),
			TEXT("osvayderuerestartsurvival.h"),
			TEXT("restartsurvivaltests.cpp"),
			TEXT("widgetcrashsafetytests.cpp"),
			TEXT("observed_failures.md"),
			TEXT("agentbridge\\"),
			TEXT("agentbridge/")
		};
		const FString LowerLine = Line.ToLower();
		for (const FString& Origin : OriginPaths)
		{
			if (LowerLine.Contains(Origin))
			{
				return true;
			}
		}
		return false;
	}

	bool LineMatchesCppLiteralPattern(const FString& Line)
	{
		// Heuristic: C++ string-literal wrappers used by detector source
		// when quoting blocker phrases back to itself.
		return Line.Contains(TEXT("TEXT(\""), ESearchCase::CaseSensitive)
			|| Line.Contains(TEXT("FString(TEXT(\""), ESearchCase::CaseSensitive);
	}

	FString FilterExcludedOriginHaystack(const FString& Haystack)
	{
		if (Haystack.IsEmpty())
		{
			return Haystack;
		}
		TArray<FString> Lines;
		Haystack.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
		TArray<FString> Kept;
		Kept.Reserve(Lines.Num());
		for (const FString& Line : Lines)
		{
			if (LineMatchesRipGrepPrefix(Line))
			{
				continue;
			}
			if (LineMatchesExcludedOriginPath(Line))
			{
				continue;
			}
			if (LineMatchesCppLiteralPattern(Line))
			{
				continue;
			}
			Kept.Add(Line);
		}
		return FString::Join(Kept, TEXT("\n"));
	}

	bool TryMatchClosedEditorBuildBlockerInternal(
		const FString& DetectionText,
		FOsvayderUEClosedEditorBuildBlocker& OutBlocker)
	{
		const FString CombinedText = DetectionText;
		const FString Haystack = CombinedText.ToLower();
		const auto SetMatch = [&OutBlocker, &CombinedText](
			const EOsvayderUEClosedEditorBuildBlockerFamily Family,
			const FString& Reason,
			const TCHAR* EvidenceNeedle)
		{
			OutBlocker.bDetected = true;
			OutBlocker.Family = Family;
			OutBlocker.ClassificationLabel = TEXT("open_editor_build_lock");
			OutBlocker.FamilyLabel = OsvayderUEClosedEditorBuildBlockerFamilyToString(Family);
			OutBlocker.EscalationReason = Reason;
			OutBlocker.MatchedEvidence = ExtractClosedEditorBuildBlockerMatchedEvidence(CombinedText, EvidenceNeedle);
			if (OutBlocker.MatchedEvidence.IsEmpty())
			{
				OutBlocker.MatchedEvidence = Reason;
			}
			return true;
		};

		if (Haystack.Contains(TEXT("unauthorizedaccessexception"))
			&& (Haystack.Contains(TEXT("backuplogfile"))
				|| Haystack.Contains(TEXT("log.addfilewriter"))
				|| (Haystack.Contains(TEXT("access to the path is denied")) && Haystack.Contains(TEXT(".log")))))
		{
			return SetMatch(
				EOsvayderUEClosedEditorBuildBlockerFamily::UbtLogBackupAccessDenied,
				TEXT("UBT/log backup access is denied while Unreal keeps the active build log path open."),
				TEXT("UnauthorizedAccessException"));
		}

		if (Haystack.Contains(TEXT("failed to rename exported file"))
			&& (Haystack.Contains(TEXT("/uht/"))
				|| Haystack.Contains(TEXT("\\uht\\"))
				|| Haystack.Contains(TEXT(".tmp"))))
		{
			return SetMatch(
				EOsvayderUEClosedEditorBuildBlockerFamily::UhtGeneratedRenameLock,
				TEXT("UHT/generated-file rename lock shows that editor-open generated outputs cannot be replaced while Unreal is still running."),
				TEXT("Failed to rename exported file"));
		}

		if (Haystack.Contains(TEXT("unable to build while live coding is active"))
			|| (Haystack.Contains(TEXT("live coding")) && Haystack.Contains(TEXT("unable to build"))))
		{
			return SetMatch(
				EOsvayderUEClosedEditorBuildBlockerFamily::LiveCodingActive,
				TEXT("Live Coding is active, so the editor-open build lane must close before a bounded external build can continue."),
				TEXT("Unable to build while Live Coding is active"));
		}

		const bool bMentionsIntermediateBuildPath =
			Haystack.Contains(TEXT("intermediate/build"))
			|| Haystack.Contains(TEXT("intermediate\\build"));
		const bool bMentionsIntermediateArtifact =
			Haystack.Contains(TEXT(".rsp"))
			|| Haystack.Contains(TEXT(".old"))
			|| Haystack.Contains(TEXT("shareddefinitions"));
		const bool bDeniedIntermediateArtifactWrite =
			(Haystack.Contains(TEXT("unauthorizedaccessexception"))
				&& Haystack.Contains(TEXT("access to the path"))
				&& Haystack.Contains(TEXT("denied")))
			|| Haystack.Contains(TEXT("unable to rename"));
		if (bMentionsIntermediateBuildPath
			&& bMentionsIntermediateArtifact
			&& bDeniedIntermediateArtifactWrite)
		{
			return SetMatch(
				EOsvayderUEClosedEditorBuildBlockerFamily::IntermediateBuildArtifactAccessDenied,
				TEXT("Open-editor build metadata (.rsp, .old, SharedDefinitions) is locked, so the build must continue in the closed-editor lane."),
				Haystack.Contains(TEXT("unauthorizedaccessexception"))
					? TEXT("UnauthorizedAccessException")
					: TEXT("Unable to rename"));
		}

		if ((Haystack.Contains(TEXT("fatal error lnk1104")) || Haystack.Contains(TEXT("lnk1104")))
			&& Haystack.Contains(TEXT("unrealeditor-"))
			&& Haystack.Contains(TEXT(".dll")))
		{
			return SetMatch(
				EOsvayderUEClosedEditorBuildBlockerFamily::EditorBinaryLinkerLock,
				TEXT("The live editor is holding UnrealEditor DLL build outputs, so link must continue in the closed-editor lane."),
				TEXT("LNK1104"));
		}

		if (Haystack.Contains(TEXT("being used by another process"))
			&& (bMentionsIntermediateBuildPath
				|| Haystack.Contains(TEXT(".generated.h"))
				|| Haystack.Contains(TEXT(".gen.cpp"))
				|| Haystack.Contains(TEXT("rename"))))
		{
			return SetMatch(
				EOsvayderUEClosedEditorBuildBlockerFamily::EditorOpenFileLock,
				TEXT("Editor-open file locking is blocking the bounded file/build lane, so Unreal must close before work can continue."),
				TEXT("used by another process"));
		}

		return false;
	}

	bool IsPreparedRequestRestoreEnabled(const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request)
	{
		return Request.bRestoreEnabled
			|| !Request.AutosaveSourcePath.IsEmpty()
			|| !Request.TargetPath.IsEmpty()
			|| !Request.BackupPath.IsEmpty();
	}

	bool IsPreparedRequestFileWriteEnabled(const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request)
	{
		return Request.bFileWriteEnabled
			|| !Request.FileWriteSourcePath.IsEmpty()
			|| !Request.FileWriteTargetPath.IsEmpty()
			|| !Request.FileWriteBackupPath.IsEmpty();
	}

	void WriteRestoreIntentJson(
		const FOsvayderUERestartSurvivalRestoreIntent& RestoreIntent,
		const TSharedRef<FJsonObject>& RootObject)
	{
		TSharedPtr<FJsonObject> RestoreObject = MakeShared<FJsonObject>();
		RestoreObject->SetBoolField(TEXT("enabled"), RestoreIntent.bEnabled);
		RestoreObject->SetStringField(TEXT("autosave_source_path"), RestoreIntent.AutosaveSourcePath);
		RestoreObject->SetStringField(TEXT("target_path"), RestoreIntent.TargetPath);
		RestoreObject->SetStringField(TEXT("backup_path"), RestoreIntent.BackupPath);
		RestoreObject->SetStringField(TEXT("detail"), RestoreIntent.Detail);
		RootObject->SetObjectField(TEXT("restore_intent"), RestoreObject);
	}

	void WriteFileWriteIntentJson(
		const FOsvayderUERestartSurvivalFileWriteIntent& FileWriteIntent,
		const TSharedRef<FJsonObject>& RootObject)
	{
		TSharedPtr<FJsonObject> FileWriteObject = MakeShared<FJsonObject>();
		FileWriteObject->SetBoolField(TEXT("enabled"), FileWriteIntent.bEnabled);
		FileWriteObject->SetStringField(TEXT("source_path"), FileWriteIntent.SourcePath);
		FileWriteObject->SetStringField(TEXT("target_path"), FileWriteIntent.TargetPath);
		FileWriteObject->SetStringField(TEXT("backup_path"), FileWriteIntent.BackupPath);
		FileWriteObject->SetStringField(TEXT("detail"), FileWriteIntent.Detail);
		RootObject->SetObjectField(TEXT("file_write_intent"), FileWriteObject);
	}

	void WriteOriginTaskContextJson(
		const FOsvayderUERestartSurvivalOriginTaskContext& OriginTask,
		const TSharedRef<FJsonObject>& RootObject)
	{
		TSharedPtr<FJsonObject> OriginTaskObject = MakeShared<FJsonObject>();
		OriginTaskObject->SetStringField(TEXT("originating_run_id"), OriginTask.OriginatingRunId);
		OriginTaskObject->SetStringField(TEXT("originating_user_prompt"), OriginTask.OriginatingUserPrompt);
		OriginTaskObject->SetStringField(TEXT("originating_prompt_hash"), OriginTask.OriginatingPromptHash);
		OriginTaskObject->SetStringField(TEXT("originating_task_mode"), OriginTask.OriginatingTaskMode);
		OriginTaskObject->SetStringField(TEXT("originating_requested_tool_family"), OriginTask.OriginatingRequestedToolFamily);
		OriginTaskObject->SetStringField(TEXT("originating_primary_mutation_tool_family"), OriginTask.OriginatingPrimaryMutationToolFamily);
		OriginTaskObject->SetBoolField(TEXT("originating_has_attachments"), OriginTask.bOriginatingHasAttachments);
		OriginTaskObject->SetBoolField(TEXT("originating_has_visual_reference"), OriginTask.bOriginatingHasVisualReference);
		OriginTaskObject->SetBoolField(TEXT("visual_proof_required"), OriginTask.bVisualProofRequired);
		OriginTaskObject->SetBoolField(TEXT("visual_qa_manifest_required"), OriginTask.bVisualQaManifestRequired);
		OriginTaskObject->SetArrayField(TEXT("originating_attached_image_paths"), FJsonUtils::StringArrayToJson(OriginTask.OriginatingAttachedImagePaths));
		OriginTaskObject->SetArrayField(TEXT("originating_attachment_names"), FJsonUtils::StringArrayToJson(OriginTask.OriginatingAttachmentNames));
		OriginTaskObject->SetStringField(TEXT("visual_reference_requirement"), OriginTask.VisualReferenceRequirement);
		RootObject->SetObjectField(TEXT("origin_task"), OriginTaskObject);
	}

	void WritePreparedRestoreRequestJson(
		const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request,
		const TSharedRef<FJsonObject>& RootObject)
	{
		RootObject->SetNumberField(TEXT("schema_version"), Request.SchemaVersion);
		RootObject->SetStringField(TEXT("request_id"), Request.RequestId);
		RootObject->SetStringField(TEXT("task_id"), Request.TaskId);
		RootObject->SetStringField(TEXT("session_id"), Request.SessionId);
		RootObject->SetStringField(TEXT("backend"), OsvayderUEProviderBackendToString(Request.Backend));
		RootObject->SetStringField(TEXT("linked_provider_session_id"), Request.LinkedProviderSessionId);
		RootObject->SetStringField(TEXT("project_root"), Request.ProjectRoot);
		RootObject->SetBoolField(TEXT("task_driven_handoff"), Request.bTaskDrivenHandoff);
		RootObject->SetBoolField(TEXT("auto_start_after_response"), Request.bAutoStartAfterResponse);
		RootObject->SetBoolField(TEXT("restore_enabled"), IsPreparedRequestRestoreEnabled(Request));
		RootObject->SetBoolField(TEXT("file_write_enabled"), IsPreparedRequestFileWriteEnabled(Request));
		RootObject->SetBoolField(TEXT("autonomous_closed_editor_escalation"), Request.bAutonomousClosedEditorEscalation);
		RootObject->SetBoolField(TEXT("use_relay_agent"), Request.bUseRelayAgent);
		RootObject->SetStringField(TEXT("continuation_intent_prompt"), GetPreparedRequestContinuationIntentPrompt(Request));
		RootObject->SetStringField(TEXT("autosave_source_path"), Request.AutosaveSourcePath);
		RootObject->SetStringField(TEXT("target_path"), Request.TargetPath);
		RootObject->SetStringField(TEXT("backup_path"), Request.BackupPath);
		RootObject->SetStringField(TEXT("file_write_source_path"), Request.FileWriteSourcePath);
		RootObject->SetStringField(TEXT("file_write_target_path"), Request.FileWriteTargetPath);
		RootObject->SetStringField(TEXT("file_write_backup_path"), Request.FileWriteBackupPath);
		RootObject->SetStringField(TEXT("file_write_detail"), Request.FileWriteDetail);
		RootObject->SetStringField(TEXT("detail"), Request.Detail);
		RootObject->SetStringField(TEXT("created_at_utc"), Request.CreatedAtUtc);
		RootObject->SetStringField(TEXT("post_reattach_completion_text"), GetPreparedRequestPostReattachCompletionText(Request));
		WriteOriginTaskContextJson(Request.OriginTask, RootObject);
	}

	void WriteProofJson(
		const FOsvayderUERestartSurvivalProofState& ProofState,
		const TSharedRef<FJsonObject>& RootObject)
	{
		TSharedPtr<FJsonObject> ProofObject = MakeShared<FJsonObject>();
		ProofObject->SetBoolField(TEXT("enabled"), ProofState.bEnabled);
		ProofObject->SetStringField(TEXT("run_tag"), ProofState.RunTag);
		ProofObject->SetStringField(TEXT("proof_output_path"), ProofState.ProofOutputPath);
		ProofObject->SetStringField(TEXT("detached_log_path"), ProofState.DetachedLogPath);
		ProofObject->SetStringField(TEXT("build_log_path"), ProofState.BuildLogPath);
		ProofObject->SetStringField(TEXT("detached_file_target_path"), ProofState.DetachedFileTargetPath);
		ProofObject->SetStringField(TEXT("detached_file_expected_text"), ProofState.DetachedFileExpectedText);
		ProofObject->SetStringField(TEXT("restore_expected_text"), ProofState.RestoreExpectedText);
		ProofObject->SetStringField(TEXT("finalize_command"), ProofState.FinalizeCommand);
		ProofObject->SetBoolField(TEXT("detached_file_write_completed"), ProofState.bDetachedFileWriteCompleted);
		ProofObject->SetBoolField(TEXT("restore_completed"), ProofState.bRestoreCompleted);
		ProofObject->SetBoolField(TEXT("build_completed"), ProofState.bBuildCompleted);
		ProofObject->SetBoolField(TEXT("relaunch_started"), ProofState.bRelaunchStarted);
		ProofObject->SetBoolField(TEXT("reattach_validated"), ProofState.bReattachValidated);
		RootObject->SetObjectField(TEXT("proof"), ProofObject);
	}

	void ReadRestoreIntentJson(
		const TSharedPtr<FJsonObject>& RootObject,
		FOsvayderUERestartSurvivalRestoreIntent& OutRestoreIntent)
	{
		TSharedPtr<FJsonObject> RestoreObject;
		if (!GetObjectField(RootObject, TEXT("restore_intent"), RestoreObject))
		{
			return;
		}

		FJsonUtils::GetBoolField(RestoreObject, TEXT("enabled"), OutRestoreIntent.bEnabled);
		OutRestoreIntent.AutosaveSourcePath = GetStringFieldOrEmpty(RestoreObject, TEXT("autosave_source_path"));
		OutRestoreIntent.TargetPath = GetStringFieldOrEmpty(RestoreObject, TEXT("target_path"));
		OutRestoreIntent.BackupPath = GetStringFieldOrEmpty(RestoreObject, TEXT("backup_path"));
		OutRestoreIntent.Detail = GetStringFieldOrEmpty(RestoreObject, TEXT("detail"));
	}

	void ReadFileWriteIntentJson(
		const TSharedPtr<FJsonObject>& RootObject,
		FOsvayderUERestartSurvivalFileWriteIntent& OutFileWriteIntent)
	{
		TSharedPtr<FJsonObject> FileWriteObject;
		if (!GetObjectField(RootObject, TEXT("file_write_intent"), FileWriteObject))
		{
			return;
		}

		FJsonUtils::GetBoolField(FileWriteObject, TEXT("enabled"), OutFileWriteIntent.bEnabled);
		OutFileWriteIntent.SourcePath = GetStringFieldOrEmpty(FileWriteObject, TEXT("source_path"));
		OutFileWriteIntent.TargetPath = GetStringFieldOrEmpty(FileWriteObject, TEXT("target_path"));
		OutFileWriteIntent.BackupPath = GetStringFieldOrEmpty(FileWriteObject, TEXT("backup_path"));
		OutFileWriteIntent.Detail = GetStringFieldOrEmpty(FileWriteObject, TEXT("detail"));
	}

	void ReadOriginTaskContextJson(
		const TSharedPtr<FJsonObject>& RootObject,
		FOsvayderUERestartSurvivalOriginTaskContext& OutOriginTask)
	{
		TSharedPtr<FJsonObject> OriginTaskObject;
		if (!GetObjectField(RootObject, TEXT("origin_task"), OriginTaskObject))
		{
			return;
		}

		OutOriginTask.OriginatingRunId = GetStringFieldOrEmpty(OriginTaskObject, TEXT("originating_run_id"));
		OutOriginTask.OriginatingUserPrompt = GetStringFieldOrEmpty(OriginTaskObject, TEXT("originating_user_prompt"));
		OutOriginTask.OriginatingPromptHash = GetStringFieldOrEmpty(OriginTaskObject, TEXT("originating_prompt_hash"));
		OutOriginTask.OriginatingTaskMode = GetStringFieldOrEmpty(OriginTaskObject, TEXT("originating_task_mode"));
		OutOriginTask.OriginatingRequestedToolFamily = GetStringFieldOrEmpty(OriginTaskObject, TEXT("originating_requested_tool_family"));
		OutOriginTask.OriginatingPrimaryMutationToolFamily = GetStringFieldOrEmpty(OriginTaskObject, TEXT("originating_primary_mutation_tool_family"));
		FJsonUtils::GetBoolField(OriginTaskObject, TEXT("originating_has_attachments"), OutOriginTask.bOriginatingHasAttachments);
		FJsonUtils::GetBoolField(OriginTaskObject, TEXT("originating_has_visual_reference"), OutOriginTask.bOriginatingHasVisualReference);
		FJsonUtils::GetBoolField(OriginTaskObject, TEXT("visual_proof_required"), OutOriginTask.bVisualProofRequired);
		FJsonUtils::GetBoolField(OriginTaskObject, TEXT("visual_qa_manifest_required"), OutOriginTask.bVisualQaManifestRequired);
		TArray<TSharedPtr<FJsonValue>> AttachedImageValues;
		if (FJsonUtils::GetArrayField(OriginTaskObject, TEXT("originating_attached_image_paths"), AttachedImageValues))
		{
			OutOriginTask.OriginatingAttachedImagePaths = FJsonUtils::JsonArrayToStrings(AttachedImageValues);
		}
		TArray<TSharedPtr<FJsonValue>> AttachmentNameValues;
		if (FJsonUtils::GetArrayField(OriginTaskObject, TEXT("originating_attachment_names"), AttachmentNameValues))
		{
			OutOriginTask.OriginatingAttachmentNames = FJsonUtils::JsonArrayToStrings(AttachmentNameValues);
		}
		OutOriginTask.VisualReferenceRequirement = GetStringFieldOrEmpty(OriginTaskObject, TEXT("visual_reference_requirement"));
	}

	bool ParsePreparedRestoreRequestJson(
		const TSharedPtr<FJsonObject>& RootObject,
		FOsvayderUERestartSurvivalPreparedRestoreRequest& OutRequest,
		FString& OutError)
	{
		if (!RootObject.IsValid())
		{
			OutError = TEXT("Prepared restart-survival restore request JSON is invalid.");
			return false;
		}

		double SchemaVersion = RestartSurvivalSchemaVersion;
		FJsonUtils::GetNumberField(RootObject, TEXT("schema_version"), SchemaVersion);
		OutRequest.SchemaVersion = static_cast<int32>(SchemaVersion);
		OutRequest.RequestId = GetStringFieldOrEmpty(RootObject, TEXT("request_id"));
		OutRequest.TaskId = GetStringFieldOrEmpty(RootObject, TEXT("task_id"));
		OutRequest.SessionId = GetStringFieldOrEmpty(RootObject, TEXT("session_id"));
		OutRequest.Backend = ParseBackendIdentity(GetStringFieldOrEmpty(RootObject, TEXT("backend")), OutRequest.Backend);
		OutRequest.LinkedProviderSessionId = GetStringFieldOrEmpty(RootObject, TEXT("linked_provider_session_id"));
		OutRequest.ProjectRoot = GetStringFieldOrEmpty(RootObject, TEXT("project_root"));
		FJsonUtils::GetBoolField(RootObject, TEXT("task_driven_handoff"), OutRequest.bTaskDrivenHandoff);
		FJsonUtils::GetBoolField(RootObject, TEXT("auto_start_after_response"), OutRequest.bAutoStartAfterResponse);
		FJsonUtils::GetBoolField(RootObject, TEXT("restore_enabled"), OutRequest.bRestoreEnabled);
		FJsonUtils::GetBoolField(RootObject, TEXT("file_write_enabled"), OutRequest.bFileWriteEnabled);
		FJsonUtils::GetBoolField(RootObject, TEXT("autonomous_closed_editor_escalation"), OutRequest.bAutonomousClosedEditorEscalation);
		FJsonUtils::GetBoolField(RootObject, TEXT("use_relay_agent"), OutRequest.bUseRelayAgent);
		OutRequest.ContinuationIntentPrompt = GetStringFieldOrEmpty(RootObject, TEXT("continuation_intent_prompt"));
		OutRequest.AutosaveSourcePath = GetStringFieldOrEmpty(RootObject, TEXT("autosave_source_path"));
		OutRequest.TargetPath = GetStringFieldOrEmpty(RootObject, TEXT("target_path"));
		OutRequest.BackupPath = GetStringFieldOrEmpty(RootObject, TEXT("backup_path"));
		OutRequest.FileWriteSourcePath = GetStringFieldOrEmpty(RootObject, TEXT("file_write_source_path"));
		OutRequest.FileWriteTargetPath = GetStringFieldOrEmpty(RootObject, TEXT("file_write_target_path"));
		OutRequest.FileWriteBackupPath = GetStringFieldOrEmpty(RootObject, TEXT("file_write_backup_path"));
		OutRequest.FileWriteDetail = GetStringFieldOrEmpty(RootObject, TEXT("file_write_detail"));
		OutRequest.Detail = GetStringFieldOrEmpty(RootObject, TEXT("detail"));
		OutRequest.CreatedAtUtc = GetStringFieldOrEmpty(RootObject, TEXT("created_at_utc"));
		OutRequest.PostReattachCompletionText = GetStringFieldOrEmpty(RootObject, TEXT("post_reattach_completion_text"));
		ReadOriginTaskContextJson(RootObject, OutRequest.OriginTask);
		if (OutRequest.ContinuationIntentPrompt.IsEmpty())
		{
			OutRequest.ContinuationIntentPrompt = OutRequest.PostReattachCompletionText;
		}
		if (!OutRequest.bRestoreEnabled)
		{
			OutRequest.bRestoreEnabled = IsPreparedRequestRestoreEnabled(OutRequest);
		}
		if (!OutRequest.bFileWriteEnabled)
		{
			OutRequest.bFileWriteEnabled = IsPreparedRequestFileWriteEnabled(OutRequest);
		}
		return true;
	}

	void ReadProofJson(
		const TSharedPtr<FJsonObject>& RootObject,
		FOsvayderUERestartSurvivalProofState& OutProofState)
	{
		TSharedPtr<FJsonObject> ProofObject;
		if (!GetObjectField(RootObject, TEXT("proof"), ProofObject))
		{
			return;
		}

		FJsonUtils::GetBoolField(ProofObject, TEXT("enabled"), OutProofState.bEnabled);
		OutProofState.RunTag = GetStringFieldOrEmpty(ProofObject, TEXT("run_tag"));
		OutProofState.ProofOutputPath = GetStringFieldOrEmpty(ProofObject, TEXT("proof_output_path"));
		OutProofState.DetachedLogPath = GetStringFieldOrEmpty(ProofObject, TEXT("detached_log_path"));
		OutProofState.BuildLogPath = GetStringFieldOrEmpty(ProofObject, TEXT("build_log_path"));
		OutProofState.DetachedFileTargetPath = GetStringFieldOrEmpty(ProofObject, TEXT("detached_file_target_path"));
		OutProofState.DetachedFileExpectedText = GetStringFieldOrEmpty(ProofObject, TEXT("detached_file_expected_text"));
		OutProofState.RestoreExpectedText = GetStringFieldOrEmpty(ProofObject, TEXT("restore_expected_text"));
		OutProofState.FinalizeCommand = GetStringFieldOrEmpty(ProofObject, TEXT("finalize_command"));
		FJsonUtils::GetBoolField(ProofObject, TEXT("detached_file_write_completed"), OutProofState.bDetachedFileWriteCompleted);
		FJsonUtils::GetBoolField(ProofObject, TEXT("restore_completed"), OutProofState.bRestoreCompleted);
		FJsonUtils::GetBoolField(ProofObject, TEXT("build_completed"), OutProofState.bBuildCompleted);
		FJsonUtils::GetBoolField(ProofObject, TEXT("relaunch_started"), OutProofState.bRelaunchStarted);
		FJsonUtils::GetBoolField(ProofObject, TEXT("reattach_validated"), OutProofState.bReattachValidated);
	}

	FString GetPlanPhaseIdForLaneContinuity(const FOsvayderUEActivePlan& Plan)
	{
		if (Plan.FeatureWorkflow.HasAnySignal() && !Plan.FeatureWorkflow.CurrentPhase.IsEmpty())
		{
			return Plan.FeatureWorkflow.CurrentPhase;
		}

		return Plan.CurrentMechanicId;
	}

	void SeedRestartLaneStateFromActivePlan(FOsvayderUETaskLaneState& LaneState)
	{
		if (!LaneState.ContinuityPlanId.IsEmpty()
			&& !LaneState.ContinuityWorkflowId.IsEmpty()
			&& !LaneState.ContinuityPhaseId.IsEmpty()
			&& !LaneState.ContinuationIntent.IsEmpty())
		{
			return;
		}

		FOsvayderUEActivePlan ActivePlan;
		FString ActivePlanError;
		if (!FOsvayderUERelayAgentManager::LoadActivePlan(ActivePlan, ActivePlanError))
		{
			return;
		}

		if (LaneState.ContinuityPlanId.IsEmpty())
		{
			LaneState.ContinuityPlanId = ActivePlan.PlanId;
		}
		if (LaneState.ContinuityWorkflowId.IsEmpty())
		{
			LaneState.ContinuityWorkflowId = ActivePlan.FeatureWorkflow.FeatureWorkflowId;
		}
		if (LaneState.ContinuityPhaseId.IsEmpty())
		{
			LaneState.ContinuityPhaseId = GetPlanPhaseIdForLaneContinuity(ActivePlan);
		}
		if (LaneState.ContinuationIntent.IsEmpty())
		{
			LaneState.ContinuationIntent = ActivePlan.ResumeHint;
		}
	}

	void RefreshRestartSurvivalLaneState(FOsvayderUERestartSurvivalState& State)
	{
		FOsvayderUETaskLaneState LaneState = State.LaneState;
		SeedRestartLaneStateFromActivePlan(LaneState);

		if (!State.TaskId.IsEmpty())
		{
			LaneState.ContinuityTaskId = State.TaskId;
		}
		if (!State.DetachedLastBlockerFamily.IsEmpty())
		{
			LaneState.BlockerFamily = State.DetachedLastBlockerFamily;
		}
		if (LaneState.BlockerFamily.IsEmpty() && !State.FailureReason.IsEmpty())
		{
			LaneState.BlockerFamily = TEXT("closed_editor_transition");
		}
		if (LaneState.ContinuationIntent.IsEmpty() && !State.PostReattachCompletionText.IsEmpty())
		{
			LaneState.ContinuationIntent = State.PostReattachCompletionText;
		}

		switch (State.Phase)
		{
		case EOsvayderUERestartSurvivalPhase::Detaching:
			LaneState.CurrentLane = TEXT("live_editor");
			LaneState.TargetLane = TEXT("closed_editor_detached");
			LaneState.ExpectedReturnLane = TEXT("live_editor");
			LaneState.TransitionKind = TEXT("lane_escalation");
			LaneState.TransitionState = TEXT("armed");
			LaneState.TransitionReason = !State.PhaseDetail.IsEmpty()
				? State.PhaseDetail
				: TEXT("Continuing the same task outside the editor because the live editor lane is blocked.");
			LaneState.ExpectedReturnCondition =
				TEXT("Continue the same persisted task outside the editor, then relaunch Unreal and resume from the saved boundary.");
			break;

		case EOsvayderUERestartSurvivalPhase::DetachedRunning:
			LaneState.CurrentLane = TEXT("closed_editor_detached");
			LaneState.TargetLane.Reset();
			LaneState.ExpectedReturnLane = TEXT("live_editor");
			LaneState.TransitionKind = TEXT("lane_escalation");
			LaneState.TransitionState = TEXT("in_progress");
			LaneState.TransitionReason = !State.PhaseDetail.IsEmpty()
				? State.PhaseDetail
				: TEXT("The same task is continuing outside the editor.");
			LaneState.ExpectedReturnCondition =
				TEXT("Return to the editor after the detached build/file/recovery work reaches a truthful terminal result.");
			break;

		case EOsvayderUERestartSurvivalPhase::AwaitingRelaunch:
		case EOsvayderUERestartSurvivalPhase::Relaunching:
		case EOsvayderUERestartSurvivalPhase::AwaitingReattach:
			LaneState.CurrentLane = TEXT("closed_editor_detached");
			LaneState.TargetLane = TEXT("live_editor");
			LaneState.ExpectedReturnLane = TEXT("live_editor");
			LaneState.TransitionKind = TEXT("lane_return");
			LaneState.TransitionState = TEXT("in_progress");
			LaneState.TransitionReason = !State.PhaseDetail.IsEmpty()
				? State.PhaseDetail
				: TEXT("The detached lane is returning control to the editor.");
			LaneState.ExpectedReturnCondition =
				TEXT("Reopen the editor and reattach the same task without restarting the workflow.");
			break;

		case EOsvayderUERestartSurvivalPhase::Reattached:
			LaneState.CurrentLane = TEXT("live_editor");
			LaneState.TargetLane.Reset();
			LaneState.ExpectedReturnLane.Reset();
			LaneState.TransitionKind = TEXT("lane_return");
			LaneState.TransitionState =
				(State.bPostReattachCompletionPending && !State.bPostReattachCompletionDispatched)
					? TEXT("in_progress")
					: TEXT("completed");
			LaneState.TransitionReason = !State.ReattachNotice.IsEmpty()
				? State.ReattachNotice
				: TEXT("The same task has reattached to the editor.");
			LaneState.ExpectedReturnCondition =
				(State.bPostReattachCompletionPending && !State.bPostReattachCompletionDispatched)
					? TEXT("Resume the same persisted task from the saved editor boundary.")
					: TEXT("Closed-editor continuation returned to the editor successfully.");
			break;

		case EOsvayderUERestartSurvivalPhase::FailedTerminal:
		{
			const bool bFailureReturnedToEditor =
				State.bDetachedOwnerManualReopenDetected
				|| State.bPostReattachCompletionPending
				|| State.bPostReattachCompletionDispatched;
			LaneState.CurrentLane = bFailureReturnedToEditor
				? TEXT("live_editor")
				: TEXT("closed_editor_detached");
			LaneState.TargetLane.Reset();
			LaneState.ExpectedReturnLane = TEXT("live_editor");
			LaneState.TransitionKind = bFailureReturnedToEditor
				? TEXT("lane_return")
				: TEXT("lane_escalation");
			LaneState.TransitionState = TEXT("blocked");
			LaneState.TransitionReason = !State.FailureReason.IsEmpty()
				? State.FailureReason
				: State.PhaseDetail;
			LaneState.ExpectedReturnCondition = TEXT("Need your review before continuing.");
			break;
		}

		case EOsvayderUERestartSurvivalPhase::AttachedInEditor:
		default:
			LaneState.CurrentLane = TEXT("live_editor");
			LaneState.TargetLane.Reset();
			LaneState.ExpectedReturnLane.Reset();
			LaneState.TransitionKind = TEXT("none");
			LaneState.TransitionState = TEXT("steady");
			if (LaneState.TransitionReason.IsEmpty())
			{
				LaneState.TransitionReason = TEXT("The same task is working in the editor.");
			}
			if (LaneState.ExpectedReturnCondition.IsEmpty())
			{
				LaneState.ExpectedReturnCondition = TEXT("Finish the current task in the editor lane unless a bounded closed-editor continuation is armed.");
			}
			break;
		}

		State.LaneState = LaneState;
	}

	TSharedRef<FJsonObject> MakeStateJson(const FOsvayderUERestartSurvivalState& State)
	{
		TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetNumberField(TEXT("schema_version"), State.SchemaVersion);
		RootObject->SetStringField(TEXT("session_id"), State.SessionId);
		RootObject->SetStringField(TEXT("task_id"), State.TaskId);
		RootObject->SetStringField(TEXT("project_root"), State.ProjectRoot);
		RootObject->SetStringField(TEXT("uproject_path"), State.UProjectPath);
		RootObject->SetStringField(TEXT("backend"), OsvayderUEProviderBackendToString(State.Backend));
		RootObject->SetStringField(TEXT("backend_display_name"), State.BackendDisplayName);
		RootObject->SetStringField(TEXT("execution_control_profile_id"), State.ExecutionControlProfileId);
		RootObject->SetStringField(TEXT("execution_transport_label"), State.ExecutionTransportLabel);
		RootObject->SetStringField(TEXT("detached_supervisor_kind"), State.DetachedSupervisorKind);
		RootObject->SetStringField(TEXT("provider_session_id"), State.ProviderSessionId);
		RootObject->SetStringField(TEXT("provider_thread_state_path"), State.ProviderThreadStatePath);
		RootObject->SetBoolField(TEXT("provider_thread_resume_pending"), State.bProviderThreadResumePending);
		RootObject->SetStringField(TEXT("phase"), OsvayderUERestartSurvivalPhaseToString(State.Phase));
		RootObject->SetStringField(TEXT("phase_detail"), State.PhaseDetail);
		if (State.LaneState.HasAnySignal())
		{
			RootObject->SetObjectField(TEXT("lane_state"), State.LaneState.ToJsonObject());
		}
		RootObject->SetStringField(TEXT("reattach_token"), State.ReattachToken);
		RootObject->SetStringField(TEXT("reattach_notice"), State.ReattachNotice);
		RootObject->SetNumberField(TEXT("editor_process_id"), State.EditorProcessId);
		RootObject->SetStringField(TEXT("engine_root"), State.EngineRoot);
		RootObject->SetStringField(TEXT("editor_executable_path"), State.EditorExecutablePath);
		RootObject->SetStringField(TEXT("build_batch_path"), State.BuildBatchPath);
		RootObject->SetStringField(TEXT("build_target"), State.BuildTarget);
		RootObject->SetStringField(TEXT("relaunch_arguments"), State.RelaunchArguments);
		RootObject->SetStringField(TEXT("support_bundle_resolution"), State.SupportBundleResolution);
		RootObject->SetStringField(TEXT("support_bundle_root"), State.SupportBundleRoot);
		RootObject->SetStringField(TEXT("supervisor_script_path"), State.SupervisorScriptPath);
		RootObject->SetStringField(TEXT("monitor_script_path"), State.MonitorScriptPath);
		RootObject->SetStringField(TEXT("preflight_launcher_path"), State.PreflightLauncherPath);
		RootObject->SetStringField(TEXT("created_at_utc"), State.CreatedAtUtc);
		RootObject->SetStringField(TEXT("last_updated_at_utc"), State.LastUpdatedAtUtc);
		RootObject->SetStringField(TEXT("detached_objective"), State.DetachedObjective);
		RootObject->SetStringField(TEXT("detached_objective_detail"), State.DetachedObjectiveDetail);
		RootObject->SetNumberField(TEXT("detached_step_index"), State.DetachedStepIndex);
		RootObject->SetNumberField(TEXT("detached_step_budget"), State.DetachedStepBudget);
		RootObject->SetStringField(TEXT("detached_current_step"), State.DetachedCurrentStep);
		RootObject->SetStringField(TEXT("detached_pending_step"), State.DetachedPendingStep);
		RootObject->SetStringField(TEXT("detached_last_step_outcome"), State.DetachedLastStepOutcome);
		RootObject->SetStringField(TEXT("detached_last_blocker_family"), State.DetachedLastBlockerFamily);
		RootObject->SetStringField(TEXT("detached_last_blocker_signature"), State.DetachedLastBlockerSignature);
		RootObject->SetStringField(TEXT("detached_terminal_outcome"), State.DetachedTerminalOutcome);
		RootObject->SetBoolField(TEXT("detached_file_write_completed"), State.bDetachedFileWriteCompleted);
		RootObject->SetBoolField(TEXT("detached_restore_completed"), State.bDetachedRestoreCompleted);
		RootObject->SetBoolField(TEXT("detached_build_completed"), State.bDetachedBuildCompleted);
		RootObject->SetNumberField(TEXT("detached_owner_process_id"), State.DetachedOwnerProcessId);
		RootObject->SetBoolField(TEXT("detached_owner_active"), State.bDetachedOwnerActive);
		RootObject->SetBoolField(TEXT("detached_owner_manual_reopen_detected"), State.bDetachedOwnerManualReopenDetected);
		RootObject->SetBoolField(TEXT("detached_owner_crash_observed"), State.bDetachedOwnerCrashObserved);
		RootObject->SetStringField(TEXT("failure_reason"), State.FailureReason);
		RootObject->SetStringField(TEXT("prepared_restore_request_id"), State.PreparedRestoreRequestId);
		RootObject->SetStringField(TEXT("prepared_restore_request_created_at_utc"), State.PreparedRestoreRequestCreatedAtUtc);
		RootObject->SetStringField(TEXT("post_reattach_completion_text"), State.PostReattachCompletionText);
		RootObject->SetBoolField(TEXT("post_reattach_completion_pending"), State.bPostReattachCompletionPending);
		RootObject->SetBoolField(TEXT("post_reattach_completion_dispatched"), State.bPostReattachCompletionDispatched);
		WriteOriginTaskContextJson(State.OriginTask, RootObject);
		WriteRestoreIntentJson(State.RestoreIntent, RootObject);
		WriteFileWriteIntentJson(State.FileWriteIntent, RootObject);
		WriteProofJson(State.Proof, RootObject);
		return RootObject;
	}

	bool ParseStateJson(const TSharedPtr<FJsonObject>& RootObject, FOsvayderUERestartSurvivalState& OutState, FString& OutError)
	{
		if (!RootObject.IsValid())
		{
			OutError = TEXT("Restart-survival state JSON is invalid.");
			return false;
		}

		double SchemaVersion = RestartSurvivalSchemaVersion;
		FJsonUtils::GetNumberField(RootObject, TEXT("schema_version"), SchemaVersion);
		OutState.SchemaVersion = static_cast<int32>(SchemaVersion);
		OutState.SessionId = GetStringFieldOrEmpty(RootObject, TEXT("session_id"));
		OutState.TaskId = GetStringFieldOrEmpty(RootObject, TEXT("task_id"));
		OutState.ProjectRoot = GetStringFieldOrEmpty(RootObject, TEXT("project_root"));
		OutState.UProjectPath = GetStringFieldOrEmpty(RootObject, TEXT("uproject_path"));
		OutState.Backend = ParseBackendIdentity(GetStringFieldOrEmpty(RootObject, TEXT("backend")), OutState.Backend);
		OutState.BackendDisplayName = GetStringFieldOrEmpty(RootObject, TEXT("backend_display_name"));
		OutState.ExecutionControlProfileId = GetStringFieldOrEmpty(RootObject, TEXT("execution_control_profile_id"));
		OutState.ExecutionTransportLabel = GetStringFieldOrEmpty(RootObject, TEXT("execution_transport_label"));
		OutState.DetachedSupervisorKind = GetStringFieldOrEmpty(RootObject, TEXT("detached_supervisor_kind"));
		OutState.ProviderSessionId = GetStringFieldOrEmpty(RootObject, TEXT("provider_session_id"));
		OutState.ProviderThreadStatePath = GetStringFieldOrEmpty(RootObject, TEXT("provider_thread_state_path"));
		FJsonUtils::GetBoolField(RootObject, TEXT("provider_thread_resume_pending"), OutState.bProviderThreadResumePending);
		ParseOsvayderUERestartSurvivalPhase(GetStringFieldOrEmpty(RootObject, TEXT("phase")), OutState.Phase);
		OutState.PhaseDetail = GetStringFieldOrEmpty(RootObject, TEXT("phase_detail"));
		TSharedPtr<FJsonObject> LaneStateObject;
		if (GetObjectField(RootObject, TEXT("lane_state"), LaneStateObject))
		{
			OutState.LaneState = FOsvayderUETaskLaneState::FromJsonObject(LaneStateObject);
		}
		OutState.ReattachToken = GetStringFieldOrEmpty(RootObject, TEXT("reattach_token"));
		OutState.ReattachNotice = GetStringFieldOrEmpty(RootObject, TEXT("reattach_notice"));
		double EditorProcessId = 0.0;
		FJsonUtils::GetNumberField(RootObject, TEXT("editor_process_id"), EditorProcessId);
		OutState.EditorProcessId = static_cast<int32>(EditorProcessId);
		OutState.EngineRoot = GetStringFieldOrEmpty(RootObject, TEXT("engine_root"));
		OutState.EditorExecutablePath = GetStringFieldOrEmpty(RootObject, TEXT("editor_executable_path"));
		OutState.BuildBatchPath = GetStringFieldOrEmpty(RootObject, TEXT("build_batch_path"));
		OutState.BuildTarget = GetStringFieldOrEmpty(RootObject, TEXT("build_target"));
		OutState.RelaunchArguments = GetStringFieldOrEmpty(RootObject, TEXT("relaunch_arguments"));
		OutState.SupportBundleResolution = GetStringFieldOrEmpty(RootObject, TEXT("support_bundle_resolution"));
		OutState.SupportBundleRoot = GetStringFieldOrEmpty(RootObject, TEXT("support_bundle_root"));
		OutState.SupervisorScriptPath = GetStringFieldOrEmpty(RootObject, TEXT("supervisor_script_path"));
		OutState.MonitorScriptPath = GetStringFieldOrEmpty(RootObject, TEXT("monitor_script_path"));
		OutState.PreflightLauncherPath = GetStringFieldOrEmpty(RootObject, TEXT("preflight_launcher_path"));
		OutState.CreatedAtUtc = GetStringFieldOrEmpty(RootObject, TEXT("created_at_utc"));
		OutState.LastUpdatedAtUtc = GetStringFieldOrEmpty(RootObject, TEXT("last_updated_at_utc"));
		OutState.DetachedObjective = GetStringFieldOrEmpty(RootObject, TEXT("detached_objective"));
		OutState.DetachedObjectiveDetail = GetStringFieldOrEmpty(RootObject, TEXT("detached_objective_detail"));
		double DetachedStepIndex = 0.0;
		FJsonUtils::GetNumberField(RootObject, TEXT("detached_step_index"), DetachedStepIndex);
		OutState.DetachedStepIndex = static_cast<int32>(DetachedStepIndex);
		double DetachedStepBudget = 3.0;
		FJsonUtils::GetNumberField(RootObject, TEXT("detached_step_budget"), DetachedStepBudget);
		OutState.DetachedStepBudget = static_cast<int32>(DetachedStepBudget);
		OutState.DetachedCurrentStep = GetStringFieldOrEmpty(RootObject, TEXT("detached_current_step"));
		OutState.DetachedPendingStep = GetStringFieldOrEmpty(RootObject, TEXT("detached_pending_step"));
		OutState.DetachedLastStepOutcome = GetStringFieldOrEmpty(RootObject, TEXT("detached_last_step_outcome"));
		OutState.DetachedLastBlockerFamily = GetStringFieldOrEmpty(RootObject, TEXT("detached_last_blocker_family"));
		OutState.DetachedLastBlockerSignature = GetStringFieldOrEmpty(RootObject, TEXT("detached_last_blocker_signature"));
		OutState.DetachedTerminalOutcome = GetStringFieldOrEmpty(RootObject, TEXT("detached_terminal_outcome"));
		FJsonUtils::GetBoolField(RootObject, TEXT("detached_file_write_completed"), OutState.bDetachedFileWriteCompleted);
		FJsonUtils::GetBoolField(RootObject, TEXT("detached_restore_completed"), OutState.bDetachedRestoreCompleted);
		FJsonUtils::GetBoolField(RootObject, TEXT("detached_build_completed"), OutState.bDetachedBuildCompleted);
		double DetachedOwnerProcessId = 0.0;
		FJsonUtils::GetNumberField(RootObject, TEXT("detached_owner_process_id"), DetachedOwnerProcessId);
		OutState.DetachedOwnerProcessId = static_cast<int32>(DetachedOwnerProcessId);
		FJsonUtils::GetBoolField(RootObject, TEXT("detached_owner_active"), OutState.bDetachedOwnerActive);
		FJsonUtils::GetBoolField(RootObject, TEXT("detached_owner_manual_reopen_detected"), OutState.bDetachedOwnerManualReopenDetected);
		FJsonUtils::GetBoolField(RootObject, TEXT("detached_owner_crash_observed"), OutState.bDetachedOwnerCrashObserved);
		OutState.FailureReason = GetStringFieldOrEmpty(RootObject, TEXT("failure_reason"));
		OutState.PreparedRestoreRequestId = GetStringFieldOrEmpty(RootObject, TEXT("prepared_restore_request_id"));
		OutState.PreparedRestoreRequestCreatedAtUtc = GetStringFieldOrEmpty(RootObject, TEXT("prepared_restore_request_created_at_utc"));
		OutState.PostReattachCompletionText = GetStringFieldOrEmpty(RootObject, TEXT("post_reattach_completion_text"));
		FJsonUtils::GetBoolField(RootObject, TEXT("post_reattach_completion_pending"), OutState.bPostReattachCompletionPending);
		FJsonUtils::GetBoolField(RootObject, TEXT("post_reattach_completion_dispatched"), OutState.bPostReattachCompletionDispatched);
		ReadOriginTaskContextJson(RootObject, OutState.OriginTask);
		ReadRestoreIntentJson(RootObject, OutState.RestoreIntent);
		ReadFileWriteIntentJson(RootObject, OutState.FileWriteIntent);
		ReadProofJson(RootObject, OutState.Proof);

		if (OutState.DetachedObjective.IsEmpty())
		{
			OutState.DetachedObjective = TEXT("closed_editor_task_owner_continuity_v1");
		}

		if (OutState.DetachedStepBudget <= 0)
		{
			OutState.DetachedStepBudget = 3;
		}

		return true;
	}

	bool LoadStateInternal(FOsvayderUERestartSurvivalState& OutState, FString& OutError)
	{
		const auto ValidateStatePath = [](const FString& CandidatePath, FString& OutValidationError)
		{
			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *CandidatePath))
			{
				OutValidationError = FString::Printf(TEXT("Failed to load restart-survival state from %s"), *CandidatePath);
				return false;
			}

			FOsvayderUERestartSurvivalState CandidateState;
			return ParseStateJson(FJsonUtils::Parse(JsonText), CandidateState, OutValidationError);
		};

		OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
		if (!OsvayderUEStorageMigration::ResolveManagedReadPath(
			GetStatePathInternal(),
			GetLegacyStatePathInternal(),
			TEXT("restart_survival_state"),
			ValidateStatePath,
			ManagedRead,
			OutError))
		{
			return false;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *ManagedRead.ResolvedPath))
		{
			OutError = FString::Printf(TEXT("Failed to load restart-survival state from %s"), *ManagedRead.ResolvedPath);
			return false;
		}

		return ParseStateJson(FJsonUtils::Parse(JsonText), OutState, OutError);
	}

	bool SaveStateInternal(FOsvayderUERestartSurvivalState State, FString& OutError)
	{
		const FString StateRootDir = GetStateRootDirInternal();
		IFileManager::Get().MakeDirectory(*StateRootDir, true);
		State.SchemaVersion = RestartSurvivalSchemaVersion;
		if (State.CreatedAtUtc.IsEmpty())
		{
			State.CreatedAtUtc = FDateTime::UtcNow().ToIso8601();
		}
		State.LastUpdatedAtUtc = FDateTime::UtcNow().ToIso8601();
		RefreshRestartSurvivalLaneState(State);

		const FString JsonText = FJsonUtils::Stringify(MakeStateJson(State), true);
		if (!FFileHelper::SaveStringToFile(JsonText, *GetStatePathInternal(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to save restart-survival state to %s"), *GetStatePathInternal());
			return false;
		}

		return true;
	}

	bool LoadPreparedRestoreRequestInternal(FOsvayderUERestartSurvivalPreparedRestoreRequest& OutRequest, FString& OutError)
	{
		const auto ValidateRequestPath = [](const FString& CandidatePath, FString& OutValidationError)
		{
			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *CandidatePath))
			{
				OutValidationError = FString::Printf(
					TEXT("Failed to load prepared restart-survival restore request from %s"),
					*CandidatePath);
				return false;
			}

			FOsvayderUERestartSurvivalPreparedRestoreRequest CandidateRequest;
			return ParsePreparedRestoreRequestJson(FJsonUtils::Parse(JsonText), CandidateRequest, OutValidationError);
		};

		OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
		if (!OsvayderUEStorageMigration::ResolveManagedReadPath(
			GetPreparedRestoreRequestPathInternal(),
			GetLegacyPreparedRestoreRequestPathInternal(),
			TEXT("restart_survival_restore_request"),
			ValidateRequestPath,
			ManagedRead,
			OutError))
		{
			return false;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *ManagedRead.ResolvedPath))
		{
			OutError = FString::Printf(
				TEXT("Failed to load prepared restart-survival restore request from %s"),
				*ManagedRead.ResolvedPath);
			return false;
		}

		return ParsePreparedRestoreRequestJson(FJsonUtils::Parse(JsonText), OutRequest, OutError);
	}

	bool SavePreparedRestoreRequestInternal(FOsvayderUERestartSurvivalPreparedRestoreRequest Request, FString& OutError)
	{
		const FString StateRootDir = GetStateRootDirInternal();
		IFileManager::Get().MakeDirectory(*StateRootDir, true);
		Request.SchemaVersion = RestartSurvivalSchemaVersion;
		if (Request.CreatedAtUtc.IsEmpty())
		{
			Request.CreatedAtUtc = FDateTime::UtcNow().ToIso8601();
		}

		const TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		WritePreparedRestoreRequestJson(Request, RootObject);
		const FString JsonText = FJsonUtils::Stringify(RootObject, true);
		if (!FFileHelper::SaveStringToFile(JsonText, *GetPreparedRestoreRequestPathInternal(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to save prepared restart-survival restore request to %s"), *GetPreparedRestoreRequestPathInternal());
			return false;
		}

		return true;
	}

	bool ValidatePreparedRestoreRequestInternal(
		const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request,
		const EOsvayderUEProviderBackend CurrentBackend,
		const FString& CurrentProjectRoot,
		const FString& CurrentProviderSessionId,
		FString& OutError)
	{
		if (Request.RequestId.IsEmpty())
		{
			OutError = TEXT("Prepared restart-survival restore request is missing request_id.");
			return false;
		}

		if (Request.TaskId.IsEmpty() || Request.SessionId.IsEmpty())
		{
			OutError = TEXT("Prepared restart-survival restore request must include exact task_id and session_id.");
			return false;
		}

		if (Request.LinkedProviderSessionId.IsEmpty())
		{
			OutError = TEXT("Prepared restart-survival restore request is missing linked_provider_session_id.");
			return false;
		}

		if (Request.Detail.IsEmpty())
		{
			OutError = TEXT("Prepared restart-survival restore request is missing detail.");
			return false;
		}

		if (Request.CreatedAtUtc.IsEmpty())
		{
			OutError = TEXT("Prepared restart-survival restore request is missing created_at_utc.");
			return false;
		}

		if (GetPreparedRequestContinuationIntentPrompt(Request).IsEmpty())
		{
			OutError = TEXT("Prepared restart-survival restore request must include a continuation_intent_prompt.");
			return false;
		}

		if (Request.Backend != CurrentBackend)
		{
			OutError = FString::Printf(
				TEXT("Prepared restart-survival restore request backend %s does not match current backend %s."),
				OsvayderUEProviderBackendToString(Request.Backend),
				OsvayderUEProviderBackendToString(CurrentBackend));
			return false;
		}

		if (!Request.LinkedProviderSessionId.Equals(CurrentProviderSessionId, ESearchCase::CaseSensitive))
		{
			OutError = TEXT("Prepared restart-survival restore request does not belong to the current surviving provider session.");
			return false;
		}

		const FString NormalizedProjectRoot = NormalizeAbsoluteDirectoryPath(CurrentProjectRoot);
		const FString NormalizedRequestProjectRoot = NormalizeAbsoluteDirectoryPath(Request.ProjectRoot);
		if (NormalizedProjectRoot.IsEmpty() || NormalizedRequestProjectRoot.IsEmpty())
		{
			OutError = TEXT("Prepared restart-survival restore request must include an explicit project-local project_root.");
			return false;
		}

		if (!NormalizedProjectRoot.Equals(NormalizedRequestProjectRoot, ESearchCase::IgnoreCase))
		{
			OutError = TEXT("Prepared restart-survival restore request project_root does not match the current project root.");
			return false;
		}

		const bool bRestoreEnabled = IsPreparedRequestRestoreEnabled(Request);
		const bool bFileWriteEnabled = IsPreparedRequestFileWriteEnabled(Request);
		if (bRestoreEnabled && bFileWriteEnabled)
		{
			OutError = TEXT("Prepared restart-survival restore request may carry either one restore intent or one exact file-write intent, but not both.");
			return false;
		}

		if (!bRestoreEnabled && !bFileWriteEnabled)
		{
			return true;
		}

		if (bRestoreEnabled)
		{
			if (Request.AutosaveSourcePath.IsEmpty() || Request.TargetPath.IsEmpty() || Request.BackupPath.IsEmpty())
			{
				OutError = TEXT("Prepared restart-survival restore request must include explicit autosave_source_path, target_path, and backup_path.");
				return false;
			}

			if (FPaths::IsRelative(Request.AutosaveSourcePath) || FPaths::IsRelative(Request.TargetPath) || FPaths::IsRelative(Request.BackupPath))
			{
				OutError = TEXT("Prepared restart-survival restore request paths must be explicit absolute paths.");
				return false;
			}

			if (!IFileManager::Get().FileExists(*Request.AutosaveSourcePath))
			{
				OutError = FString::Printf(
					TEXT("Prepared restart-survival restore request source path does not exist: %s"),
					*Request.AutosaveSourcePath);
				return false;
			}

			if (!IsPathInsideDirectory(Request.AutosaveSourcePath, NormalizedProjectRoot)
				|| !IsPathInsideDirectory(Request.TargetPath, NormalizedProjectRoot)
				|| !IsPathInsideDirectory(Request.BackupPath, NormalizedProjectRoot))
			{
				OutError = TEXT("Prepared restart-survival restore request paths must stay inside the current project root.");
				return false;
			}

			return true;
		}

		if (Request.FileWriteSourcePath.IsEmpty() || Request.FileWriteTargetPath.IsEmpty())
		{
			OutError = TEXT("Prepared restart-survival file-write intent must include explicit file_write_source_path and file_write_target_path.");
			return false;
		}

		if (FPaths::IsRelative(Request.FileWriteSourcePath)
			|| FPaths::IsRelative(Request.FileWriteTargetPath)
			|| (!Request.FileWriteBackupPath.IsEmpty() && FPaths::IsRelative(Request.FileWriteBackupPath)))
		{
			OutError = TEXT("Prepared restart-survival file-write paths must be explicit absolute paths.");
			return false;
		}

		if (!IFileManager::Get().FileExists(*Request.FileWriteSourcePath))
		{
			OutError = FString::Printf(
				TEXT("Prepared restart-survival file-write source path does not exist: %s"),
				*Request.FileWriteSourcePath);
			return false;
		}

		if (!IsPathInsideDirectory(Request.FileWriteSourcePath, NormalizedProjectRoot)
			|| !IsPathInsideDirectory(Request.FileWriteTargetPath, NormalizedProjectRoot)
			|| (!Request.FileWriteBackupPath.IsEmpty() && !IsPathInsideDirectory(Request.FileWriteBackupPath, NormalizedProjectRoot)))
		{
			OutError = TEXT("Prepared restart-survival file-write paths must stay inside the current project root.");
			return false;
		}

		return true;
	}
}

const TCHAR* OsvayderUERestartSurvivalPhaseToString(const EOsvayderUERestartSurvivalPhase Phase)
{
	switch (Phase)
	{
	case EOsvayderUERestartSurvivalPhase::Detaching:
		return TEXT("Detaching");
	case EOsvayderUERestartSurvivalPhase::DetachedRunning:
		return TEXT("DetachedRunning");
	case EOsvayderUERestartSurvivalPhase::AwaitingRelaunch:
		return TEXT("AwaitingRelaunch");
	case EOsvayderUERestartSurvivalPhase::Relaunching:
		return TEXT("Relaunching");
	case EOsvayderUERestartSurvivalPhase::AwaitingReattach:
		return TEXT("AwaitingReattach");
	case EOsvayderUERestartSurvivalPhase::Reattached:
		return TEXT("Reattached");
	case EOsvayderUERestartSurvivalPhase::FailedTerminal:
		return TEXT("FailedTerminal");
	case EOsvayderUERestartSurvivalPhase::AttachedInEditor:
	default:
		return TEXT("AttachedInEditor");
	}
}

bool ParseOsvayderUERestartSurvivalPhase(const FString& Value, EOsvayderUERestartSurvivalPhase& OutPhase)
{
	if (Value.Equals(TEXT("Detaching"), ESearchCase::IgnoreCase))
	{
		OutPhase = EOsvayderUERestartSurvivalPhase::Detaching;
		return true;
	}
	if (Value.Equals(TEXT("DetachedRunning"), ESearchCase::IgnoreCase))
	{
		OutPhase = EOsvayderUERestartSurvivalPhase::DetachedRunning;
		return true;
	}
	if (Value.Equals(TEXT("AwaitingRelaunch"), ESearchCase::IgnoreCase))
	{
		OutPhase = EOsvayderUERestartSurvivalPhase::AwaitingRelaunch;
		return true;
	}
	if (Value.Equals(TEXT("Relaunching"), ESearchCase::IgnoreCase))
	{
		OutPhase = EOsvayderUERestartSurvivalPhase::Relaunching;
		return true;
	}
	if (Value.Equals(TEXT("AwaitingReattach"), ESearchCase::IgnoreCase))
	{
		OutPhase = EOsvayderUERestartSurvivalPhase::AwaitingReattach;
		return true;
	}
	if (Value.Equals(TEXT("Reattached"), ESearchCase::IgnoreCase))
	{
		OutPhase = EOsvayderUERestartSurvivalPhase::Reattached;
		return true;
	}
	if (Value.Equals(TEXT("FailedTerminal"), ESearchCase::IgnoreCase))
	{
		OutPhase = EOsvayderUERestartSurvivalPhase::FailedTerminal;
		return true;
	}

	OutPhase = EOsvayderUERestartSurvivalPhase::AttachedInEditor;
	return Value.Equals(TEXT("AttachedInEditor"), ESearchCase::IgnoreCase);
}

const TCHAR* OsvayderUEClosedEditorBuildBlockerFamilyToString(const EOsvayderUEClosedEditorBuildBlockerFamily Family)
{
	switch (Family)
	{
	case EOsvayderUEClosedEditorBuildBlockerFamily::UbtLogBackupAccessDenied:
		return TEXT("ubt_log_backup_access_denied");
	case EOsvayderUEClosedEditorBuildBlockerFamily::UhtGeneratedRenameLock:
		return TEXT("uht_generated_rename_lock");
	case EOsvayderUEClosedEditorBuildBlockerFamily::LiveCodingActive:
		return TEXT("live_coding_active");
	case EOsvayderUEClosedEditorBuildBlockerFamily::IntermediateBuildArtifactAccessDenied:
		return TEXT("intermediate_build_artifact_access_denied");
	case EOsvayderUEClosedEditorBuildBlockerFamily::EditorBinaryLinkerLock:
		return TEXT("editor_binary_linker_lock");
	case EOsvayderUEClosedEditorBuildBlockerFamily::EditorOpenFileLock:
		return TEXT("editor_open_file_lock");
	case EOsvayderUEClosedEditorBuildBlockerFamily::None:
	default:
		return TEXT("none");
	}
}

FString FOsvayderUERestartSurvivalManager::GetStateRootDir()
{
	return GetStateRootDirInternal();
}

FString FOsvayderUERestartSurvivalManager::GetStatePath()
{
	return GetStatePathInternal();
}

FString FOsvayderUERestartSurvivalManager::GetPreparedRestoreRequestPath()
{
	return GetPreparedRestoreRequestPathInternal();
}

FString FOsvayderUERestartSurvivalManager::GetClosedEditorResultPath()
{
	return GetClosedEditorResultPathInternal();
}

FString FOsvayderUERestartSurvivalManager::GetSupervisorScriptPath()
{
	FOsvayderUERestartSurvivalSupportBundle Bundle;
	FString Error;
	TryResolveSupportBundleInternal(Bundle, Error);
	return Bundle.SupervisorScriptPath;
}

FString FOsvayderUERestartSurvivalManager::GetMonitorScriptPath()
{
	FOsvayderUERestartSurvivalSupportBundle Bundle;
	FString Error;
	TryResolveSupportBundleInternal(Bundle, Error);
	return Bundle.MonitorScriptPath;
}

FString FOsvayderUERestartSurvivalManager::GetPreflightScriptPath()
{
	FOsvayderUERestartSurvivalSupportBundle Bundle;
	FString Error;
	TryResolveSupportBundleInternal(Bundle, Error);
	return Bundle.PreflightScriptPath;
}

FString FOsvayderUERestartSurvivalManager::BuildPreparedRequestClosedEditorTransitionNotice(
	const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request)
{
	return BuildPreparedRequestClosedEditorTransitionNoticeInternal(Request);
}

bool FOsvayderUERestartSurvivalManager::TryResolveSupportBundle(
	FOsvayderUERestartSurvivalSupportBundle& OutBundle,
	FString& OutError)
{
	return TryResolveSupportBundleInternal(OutBundle, OutError);
}

bool FOsvayderUERestartSurvivalManager::TryDetectClosedEditorBuildBlocker(
	const FString& ToolName,
	const FString& ToolInput,
	const FString& ToolResultContent,
	const FString& RawProviderEvent,
	FOsvayderUEClosedEditorBuildBlocker& OutBlocker)
{
	OutBlocker = FOsvayderUEClosedEditorBuildBlocker();
	if (!IsCommandExecutionToolResult(ToolName))
	{
		return false;
	}

	// 626 P2 Layer A: reject known inspection commands outright.
	// 626 P3: classifiers now live in OsvayderUE::CommandClassification.
	if (OsvayderUE::CommandClassification::IsKnownInspectionCommand(ToolInput))
	{
		return false;
	}

	// 626 P2 Layer B: filter grep-output / excluded-origin lines from the
	// haystack BEFORE admission check, so the structured-signature fallback
	// can't be fooled by repo inspection quoting UBT-ish strings back.
	const FString RawHaystack = BuildClosedEditorBuildBlockerDetectionHaystack(ToolResultContent, RawProviderEvent);
	const FString FilteredHaystack = FilterExcludedOriginHaystack(RawHaystack);
	const FString LowerFiltered = FilteredHaystack.ToLower();

	// 626 P2 Layer A: require either an explicit build invocation in
	// ToolInput OR a structured-build-output signature in the filtered
	// haystack. Ordinary inspection output with neither is bypassed.
	if (!OsvayderUE::CommandClassification::IsBuildContextCommand(ToolInput)
		&& !OsvayderUE::CommandClassification::HasStructuredBuildOutputSignature(LowerFiltered))
	{
		return false;
	}

	return TryMatchClosedEditorBuildBlockerInternal(FilteredHaystack, OutBlocker);
}

bool FOsvayderUERestartSurvivalManager::TryPrepareClosedEditorBuildBlockerAutoStart(
	const EOsvayderUEProviderBackend Backend,
	const FString& ProjectRoot,
	const FString& ProviderSessionId,
	const FOsvayderUEClosedEditorBuildBlocker& Blocker,
	const FOsvayderUERestartSurvivalOriginTaskContext& OriginTask,
	FOsvayderUERestartSurvivalPreparedRestoreRequest& OutRequest,
	FString& OutTransitionNotice,
	FString& OutError)
{
	OutRequest = FOsvayderUERestartSurvivalPreparedRestoreRequest();
	OutTransitionNotice.Reset();
	OutError.Reset();

	if (!Blocker.bDetected || Blocker.Family == EOsvayderUEClosedEditorBuildBlockerFamily::None)
	{
		OutError = TEXT("Closed-editor build blocker auto-start requires one detected blocker family.");
		return false;
	}

	if (Backend != EOsvayderUEProviderBackend::CodexCli)
	{
		OutError = TEXT("Closed-editor build blocker auto-start is supported only for the ordinary Codex runtime.");
		return false;
	}

	if (ProviderSessionId.IsEmpty())
	{
		OutError = TEXT("Closed-editor build blocker auto-start requires the current surviving provider session id.");
		return false;
	}

	FOsvayderUERestartSurvivalOriginTaskContext NormalizedOriginTask = OriginTask;
	NormalizedOriginTask.OriginatingUserPrompt = NormalizedOriginTask.OriginatingUserPrompt.TrimStartAndEnd();
	if (NormalizedOriginTask.OriginatingUserPrompt.IsEmpty())
	{
		OutError = TEXT("Closed-editor build blocker auto-start refused to arm because the original user prompt could not be captured.");
		return false;
	}

	if (NormalizedOriginTask.OriginatingPromptHash.IsEmpty())
	{
		NormalizedOriginTask.OriginatingPromptHash = ComputeRestartSurvivalPromptHash(NormalizedOriginTask.OriginatingUserPrompt);
	}
	NormalizedOriginTask.bOriginatingHasAttachments =
		NormalizedOriginTask.bOriginatingHasAttachments || NormalizedOriginTask.OriginatingAttachedImagePaths.Num() > 0;
	if (NormalizedOriginTask.bOriginatingHasVisualReference || NormalizedOriginTask.bVisualProofRequired)
	{
		NormalizedOriginTask.bVisualProofRequired = true;
		NormalizedOriginTask.bVisualQaManifestRequired = true;
	}
	if ((NormalizedOriginTask.bOriginatingHasVisualReference || NormalizedOriginTask.bVisualProofRequired || NormalizedOriginTask.bVisualQaManifestRequired)
		&& NormalizedOriginTask.VisualReferenceRequirement.IsEmpty())
	{
		NormalizedOriginTask.VisualReferenceRequirement =
			TEXT("Final acceptance requires a visual_qa_manifest.json with verdict=passed and actual_screenshot_paths, or an explicit visual-proof blocker.");
	}

	const FString NormalizedProjectRoot = NormalizeAbsoluteDirectoryPath(ProjectRoot);
	if (NormalizedProjectRoot.IsEmpty())
	{
		OutError = TEXT("Closed-editor build blocker auto-start requires an explicit project-local project_root.");
		return false;
	}

	FOsvayderUERestartSurvivalSupportBundle SupportBundle;
	if (!TryResolveSupportBundleInternal(SupportBundle, OutError))
	{
		return false;
	}

	FOsvayderUERestartSurvivalPreparedRestoreRequest Request;
	Request.RequestId = MakePreparedRequestId();
	Request.TaskId = MakeAutonomousTaskId(ProviderSessionId);
	Request.SessionId = MakeSessionIdFromTaskId(Request.TaskId);
	Request.Backend = Backend;
	Request.LinkedProviderSessionId = ProviderSessionId;
	Request.ProjectRoot = NormalizedProjectRoot;
	Request.bTaskDrivenHandoff = true;
	Request.bAutoStartAfterResponse = true;
	Request.bAutonomousClosedEditorEscalation = true;
	Request.Detail = FString::Printf(
		TEXT("open_editor_build_lock_auto_escalation:%s:%s"),
		OsvayderUEClosedEditorBuildBlockerFamilyToString(Blocker.Family),
		*SupportBundle.ResolutionLabel);
	Request.OriginTask = NormalizedOriginTask;
	Request.ContinuationIntentPrompt = BuildClosedEditorBuildBlockerPostReattachCompletionText(Request, Blocker);
	Request.PostReattachCompletionText = Request.ContinuationIntentPrompt;
	Request.CreatedAtUtc = FDateTime::UtcNow().ToIso8601();

	if (!ValidatePreparedRestoreRequestInternal(
		Request,
		Backend,
		NormalizedProjectRoot,
		ProviderSessionId,
		OutError))
	{
		return false;
	}

	if (!SavePreparedRestoreRequestInternal(Request, OutError))
	{
		return false;
	}

	ArmPreparedRequestAutoStart(
		Request.RequestId,
		Request.Backend,
		Request.ProjectRoot,
		Request.LinkedProviderSessionId);

	OutRequest = Request;
	OutTransitionNotice = BuildPreparedRequestClosedEditorTransitionNoticeInternal(Request);
	return true;
}

bool FOsvayderUERestartSurvivalManager::LoadState(FOsvayderUERestartSurvivalState& OutState, FString& OutError)
{
	return LoadStateInternal(OutState, OutError);
}

bool FOsvayderUERestartSurvivalManager::SaveState(FOsvayderUERestartSurvivalState State, FString& OutError)
{
	return SaveStateInternal(State, OutError);
}

bool FOsvayderUERestartSurvivalManager::DeleteState(FString& OutError)
{
	return OsvayderUEStorageMigration::DeleteManagedFileCopies(
		GetStatePathInternal(),
		GetLegacyStatePathInternal(),
		TEXT("restart_survival_state"),
		OutError);
}

bool FOsvayderUERestartSurvivalManager::DescribeCurrentState(FOsvayderUERestartSurvivalState& OutState)
{
	FString Error;
	return LoadStateInternal(OutState, Error);
}

bool FOsvayderUERestartSurvivalManager::CanReplaceExistingStateForFreshStart(
	const FOsvayderUERestartSurvivalState& State)
{
	if (State.Phase == EOsvayderUERestartSurvivalPhase::FailedTerminal)
	{
		return true;
	}

	if (State.Phase != EOsvayderUERestartSurvivalPhase::Reattached)
	{
		return false;
	}

	return !State.bProviderThreadResumePending || State.bPostReattachCompletionDispatched;
}

bool FOsvayderUERestartSurvivalManager::IsDetachedOwnerPhaseActive(const EOsvayderUERestartSurvivalPhase Phase)
{
	return Phase == EOsvayderUERestartSurvivalPhase::Detaching
		|| Phase == EOsvayderUERestartSurvivalPhase::DetachedRunning
		|| Phase == EOsvayderUERestartSurvivalPhase::AwaitingRelaunch
		|| Phase == EOsvayderUERestartSurvivalPhase::Relaunching;
}

bool FOsvayderUERestartSurvivalManager::HasPreparedRestoreRequest()
{
	FString ResolveError;
	OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
	return OsvayderUEStorageMigration::ResolveManagedReadPath(
		GetPreparedRestoreRequestPathInternal(),
		GetLegacyPreparedRestoreRequestPathInternal(),
		TEXT("restart_survival_restore_request"),
		[](const FString& CandidatePath, FString& OutValidationError)
		{
			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *CandidatePath))
			{
				OutValidationError = FString::Printf(
					TEXT("Failed to load prepared restart-survival restore request from %s"),
					*CandidatePath);
				return false;
			}

			FOsvayderUERestartSurvivalPreparedRestoreRequest CandidateRequest;
			return ParsePreparedRestoreRequestJson(FJsonUtils::Parse(JsonText), CandidateRequest, OutValidationError);
		},
		ManagedRead,
		ResolveError);
}

bool FOsvayderUERestartSurvivalManager::LoadPreparedRestoreRequest(FOsvayderUERestartSurvivalPreparedRestoreRequest& OutRequest, FString& OutError)
{
	return LoadPreparedRestoreRequestInternal(OutRequest, OutError);
}

bool FOsvayderUERestartSurvivalManager::SavePreparedRestoreRequest(FOsvayderUERestartSurvivalPreparedRestoreRequest Request, FString& OutError)
{
	return SavePreparedRestoreRequestInternal(Request, OutError);
}

FString FOsvayderUERestartSurvivalManager::ResolvePreparedRequestPostReattachCompletionText(
	const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request)
{
	return GetPreparedRequestPostReattachCompletionText(Request);
}

bool FOsvayderUERestartSurvivalManager::DeletePreparedRestoreRequest(FString& OutError)
{
	GPreparedRequestAutoStartArm.Reset();
	GPreparedRequestStartOverride.Reset();

	return OsvayderUEStorageMigration::DeleteManagedFileCopies(
		GetPreparedRestoreRequestPathInternal(),
		GetLegacyPreparedRestoreRequestPathInternal(),
		TEXT("restart_survival_restore_request"),
		OutError);
}

bool FOsvayderUERestartSurvivalManager::ValidatePreparedRestoreRequestForStart(
	const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request,
	const EOsvayderUEProviderBackend CurrentBackend,
	const FString& CurrentProjectRoot,
	const FString& CurrentProviderSessionId,
	FString& OutError)
{
	return ValidatePreparedRestoreRequestInternal(
		Request,
		CurrentBackend,
		CurrentProjectRoot,
		CurrentProviderSessionId,
		OutError);
}

void FOsvayderUERestartSurvivalManager::ArmPreparedRequestAutoStart(
	const FString& RequestId,
	const EOsvayderUEProviderBackend Backend,
	const FString& ProjectRoot,
	const FString& ProviderSessionId)
{
	FPreparedRequestAutoStartArm Arm;
	Arm.RequestId = RequestId;
	Arm.Backend = Backend;
	Arm.ProjectRoot = NormalizeAbsoluteDirectoryPath(ProjectRoot);
	Arm.ProviderSessionId = ProviderSessionId;
	GPreparedRequestAutoStartArm = Arm;
}

void FOsvayderUERestartSurvivalManager::ClearPreparedRequestAutoStartArm()
{
	GPreparedRequestAutoStartArm.Reset();
}

bool FOsvayderUERestartSurvivalManager::HasPreparedRequestAutoStartArm()
{
	return GPreparedRequestAutoStartArm.IsSet();
}

bool FOsvayderUERestartSurvivalManager::TryConsumePreparedRequestAutoStart(
	const EOsvayderUEProviderBackend CurrentBackend,
	const FString& CurrentProjectRoot,
	const FString& CurrentProviderSessionId,
	FOsvayderUERestartSurvivalPreparedRestoreRequest& OutRequest,
	FString& OutError)
{
	OutError.Reset();

	if (!GPreparedRequestAutoStartArm.IsSet())
	{
		return false;
	}

	const FPreparedRequestAutoStartArm Arm = GPreparedRequestAutoStartArm.GetValue();
	GPreparedRequestAutoStartArm.Reset();

	if (Arm.Backend != CurrentBackend)
	{
		OutError = TEXT("Prepared restart-survival handoff auto-start backend no longer matches the current backend.");
		return false;
	}

	const FString NormalizedCurrentProjectRoot = NormalizeAbsoluteDirectoryPath(CurrentProjectRoot);
	if (!Arm.ProjectRoot.IsEmpty() && !NormalizedCurrentProjectRoot.Equals(Arm.ProjectRoot, ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Prepared restart-survival handoff auto-start no longer belongs to the current project root.");
		return false;
	}

	if (!CurrentProviderSessionId.IsEmpty()
		&& !Arm.ProviderSessionId.IsEmpty()
		&& !Arm.ProviderSessionId.Equals(CurrentProviderSessionId, ESearchCase::CaseSensitive))
	{
		OutError = TEXT("Prepared restart-survival handoff auto-start no longer belongs to the current surviving provider session.");
		return false;
	}

	if (!LoadPreparedRestoreRequestInternal(OutRequest, OutError))
	{
		return false;
	}

	if (!Arm.RequestId.Equals(OutRequest.RequestId, ESearchCase::CaseSensitive))
	{
		OutError = TEXT("Prepared restart-survival handoff auto-start no longer matches the current project-local request.");
		return false;
	}

	if (!OutRequest.bTaskDrivenHandoff || !OutRequest.bAutoStartAfterResponse)
	{
		OutError = TEXT("Prepared restart-survival request is not eligible for task-driven auto-start.");
		return false;
	}

	if (!Arm.ProviderSessionId.IsEmpty()
		&& !OutRequest.LinkedProviderSessionId.IsEmpty()
		&& !Arm.ProviderSessionId.Equals(OutRequest.LinkedProviderSessionId, ESearchCase::CaseSensitive))
	{
		OutError = TEXT("Prepared restart-survival handoff auto-start no longer matches the armed provider-session truth.");
		return false;
	}

	const FString EffectiveProviderSessionId = CurrentProviderSessionId.IsEmpty()
		? (!Arm.ProviderSessionId.IsEmpty() ? Arm.ProviderSessionId : OutRequest.LinkedProviderSessionId)
		: CurrentProviderSessionId;
	if (EffectiveProviderSessionId.IsEmpty())
	{
		OutError = TEXT("Prepared restart-survival handoff auto-start no longer has a surviving provider session to start from.");
		return false;
	}

	return ValidatePreparedRestoreRequestInternal(
		OutRequest,
		CurrentBackend,
		CurrentProjectRoot,
		EffectiveProviderSessionId,
		OutError);
}

void FOsvayderUERestartSurvivalManager::ArmPreparedRequestStartOverride(
	const FOsvayderUERestartSurvivalPreparedStartOverride& Override)
{
	GPreparedRequestStartOverride = Override;
}

void FOsvayderUERestartSurvivalManager::ClearPreparedRequestStartOverride()
{
	GPreparedRequestStartOverride.Reset();
}

bool FOsvayderUERestartSurvivalManager::TryConsumePreparedRequestStartOverride(
	const FOsvayderUERestartSurvivalPreparedRestoreRequest& Request,
	FOsvayderUERestartSurvivalPreparedStartOverride& OutOverride)
{
	if (!GPreparedRequestStartOverride.IsSet())
	{
		return false;
	}

	const FOsvayderUERestartSurvivalPreparedStartOverride Override = GPreparedRequestStartOverride.GetValue();
	GPreparedRequestStartOverride.Reset();

	if (!Override.TaskId.IsEmpty() && !Override.TaskId.Equals(Request.TaskId, ESearchCase::CaseSensitive))
	{
		return false;
	}

	if (!Override.SessionId.IsEmpty() && !Override.SessionId.Equals(Request.SessionId, ESearchCase::CaseSensitive))
	{
		return false;
	}

	OutOverride = Override;
	return true;
}

bool FOsvayderUERestartSurvivalManager::HasPendingResume(const EOsvayderUEProviderBackend Backend)
{
	FOsvayderUERestartSurvivalState State;
	FString Error;
	if (!LoadStateInternal(State, Error))
	{
		return false;
	}

	return State.Backend == Backend
		&& State.bProviderThreadResumePending
		&& (State.Phase == EOsvayderUERestartSurvivalPhase::AwaitingReattach
			|| State.Phase == EOsvayderUERestartSurvivalPhase::Reattached);
}

bool FOsvayderUERestartSurvivalManager::TryMarkReattached(
	const EOsvayderUEProviderBackend ExpectedBackend,
	const FString& PresentedToken,
	FString& OutNotice,
	FString& OutError)
{
	FOsvayderUERestartSurvivalState State;
	if (!LoadStateInternal(State, OutError))
	{
		return false;
	}

	if (State.Backend != ExpectedBackend)
	{
		OutError = TEXT("Restart-survival backend does not match the currently configured backend.");
		return false;
	}

	if (State.Phase != EOsvayderUERestartSurvivalPhase::AwaitingReattach)
	{
		OutError = FString::Printf(
			TEXT("Restart-survival is not ready to reattach from phase %s."),
			OsvayderUERestartSurvivalPhaseToString(State.Phase));
		return false;
	}

	if (!State.bProviderThreadResumePending)
	{
		OutError = TEXT("Restart-survival does not have a pending provider-thread resume to reattach.");
		return false;
	}

	if (State.ReattachToken.IsEmpty())
	{
		OutError = TEXT("Restart-survival state is missing a reattach token.");
		return false;
	}

	if (PresentedToken.IsEmpty())
	{
		OutError = TEXT("Restart-survival reattach token was not presented on relaunch.");
		return false;
	}

	if (!State.ReattachToken.Equals(PresentedToken, ESearchCase::CaseSensitive))
	{
		OutError = TEXT("Restart-survival reattach token does not match the saved project-local state.");
		return false;
	}

	State.Phase = EOsvayderUERestartSurvivalPhase::Reattached;
	State.PhaseDetail = TEXT("Editor widget reattached to the surviving detached task/session through a validated reattach token.");
	State.bDetachedOwnerActive = false;
	State.DetachedOwnerProcessId = 0;
	State.bDetachedOwnerCrashObserved = false;
	if (State.Proof.bEnabled)
	{
		State.Proof.bRelaunchStarted = true;
	}

	OutNotice = State.ReattachNotice.IsEmpty()
		? FString::Printf(TEXT("Restart-survival reattached to task %s."), *State.TaskId)
		: State.ReattachNotice;
	return SaveStateInternal(State, OutError);
}

bool FOsvayderUERestartSurvivalManager::TryMarkReattachedFromManualReopen(
	const EOsvayderUEProviderBackend ExpectedBackend,
	FString& OutNotice,
	FString& OutError)
{
	FOsvayderUERestartSurvivalState State;
	if (!LoadStateInternal(State, OutError))
	{
		return false;
	}

	if (State.Backend != ExpectedBackend)
	{
		OutError = TEXT("Restart-survival backend does not match the currently configured backend.");
		return false;
	}

	if (State.Phase != EOsvayderUERestartSurvivalPhase::AwaitingReattach)
	{
		OutError = FString::Printf(
			TEXT("Restart-survival manual reopen attach requires phase AwaitingReattach, but current phase is %s."),
			OsvayderUERestartSurvivalPhaseToString(State.Phase));
		return false;
	}

	if (!State.bProviderThreadResumePending)
	{
		OutError = TEXT("Restart-survival does not have a pending provider-thread resume to adopt from manual reopen.");
		return false;
	}

	if (!State.bDetachedOwnerManualReopenDetected)
	{
		OutError = TEXT("Restart-survival manual reopen attach is allowed only after the existing detached owner was explicitly observed by the reopened editor.");
		return false;
	}

	State.Phase = EOsvayderUERestartSurvivalPhase::Reattached;
	State.PhaseDetail = TEXT("Editor widget reattached to the surviving detached task/session after manual editor reopen without forking competing detached ownership.");
	State.bDetachedOwnerActive = false;
	State.DetachedOwnerProcessId = 0;
	State.bDetachedOwnerCrashObserved = false;

	OutNotice = State.ReattachNotice.IsEmpty()
		? FString::Printf(TEXT("Restart-survival manually reattached to task %s."), *State.TaskId)
		: State.ReattachNotice;
	return SaveStateInternal(State, OutError);
}

bool FOsvayderUERestartSurvivalManager::MarkReattachValidated(FString& OutError)
{
	FOsvayderUERestartSurvivalState State;
	if (!LoadStateInternal(State, OutError))
	{
		return false;
	}

	if (State.Phase != EOsvayderUERestartSurvivalPhase::Reattached)
	{
		OutError = FString::Printf(
			TEXT("Restart-survival validation requires phase Reattached, but current phase is %s."),
			OsvayderUERestartSurvivalPhaseToString(State.Phase));
		return false;
	}

	if (State.bPostReattachCompletionPending && !State.bPostReattachCompletionDispatched)
	{
		OutError = TEXT("Restart-survival reattach validation is blocked until the pending continuation has been dispatched.");
		return false;
	}

	if (!State.bProviderThreadResumePending)
	{
		if (!State.Proof.bEnabled || State.Proof.bReattachValidated)
		{
			return true;
		}
	}

	State.bProviderThreadResumePending = false;
	State.bDetachedOwnerActive = false;
	State.DetachedOwnerProcessId = 0;
	if (State.Proof.bEnabled)
	{
		State.Proof.bReattachValidated = true;
	}
	State.Phase = EOsvayderUERestartSurvivalPhase::Reattached;
	State.PhaseDetail = TEXT("Detached restart-survival task/session was validated in the reopened editor. Provider-thread continuity is backend-dependent.");
	State.FailureReason.Empty();
	return SaveStateInternal(State, OutError);
}

bool FOsvayderUERestartSurvivalManager::MarkFailed(const FString& Reason, FString& OutError)
{
	FOsvayderUERestartSurvivalState State;
	if (!LoadStateInternal(State, OutError))
	{
		return false;
	}

	State.Phase = EOsvayderUERestartSurvivalPhase::FailedTerminal;
	State.PhaseDetail = Reason;
	State.FailureReason = Reason;
	State.DetachedTerminalOutcome = TEXT("failed");
	State.bDetachedOwnerActive = false;
	State.DetachedOwnerProcessId = 0;
	return SaveStateInternal(State, OutError);
}

TSharedPtr<FJsonObject> FOsvayderUERestartSurvivalManager::BuildReadbackJson()
{
	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	FOsvayderUERestartSurvivalSupportBundle SupportBundle;
	FString SupportBundleError;
	const bool bSupportBundleResolved = TryResolveSupportBundleInternal(SupportBundle, SupportBundleError);
	RootObject->SetBoolField(TEXT("state_present"), false);
	RootObject->SetStringField(TEXT("state_path"), GetStatePathInternal());
	RootObject->SetStringField(TEXT("prepared_restore_request_path"), GetPreparedRestoreRequestPathInternal());
	RootObject->SetStringField(TEXT("closed_editor_result_path"), GetClosedEditorResultPathInternal());
	RootObject->SetBoolField(TEXT("prepared_restore_request_present"), HasPreparedRestoreRequest());
	{
		FString ResolveError;
		OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
		const bool bClosedEditorResultPresent = OsvayderUEStorageMigration::ResolveManagedReadPath(
			GetClosedEditorResultPathInternal(),
			GetLegacyClosedEditorResultPathInternal(),
			TEXT("closed_editor_result"),
			[](const FString& CandidatePath, FString& OutValidationError)
			{
				if (IFileManager::Get().FileExists(*CandidatePath))
				{
					return true;
				}

				OutValidationError = FString::Printf(TEXT("Closed-editor result file not found at %s"), *CandidatePath);
				return false;
			},
			ManagedRead,
			ResolveError);
		RootObject->SetBoolField(TEXT("closed_editor_result_present"), bClosedEditorResultPresent);
	}
	RootObject->SetBoolField(TEXT("prepared_restore_request_auto_start_armed"), GPreparedRequestAutoStartArm.IsSet());
	RootObject->SetStringField(TEXT("relay_handoff_context_path"), FOsvayderUERelayAgentManager::GetHandoffContextPath());
	RootObject->SetStringField(TEXT("relay_progress_path"), FOsvayderUERelayAgentManager::GetRelayProgressPath());
	RootObject->SetStringField(TEXT("relay_result_path"), FOsvayderUERelayAgentManager::GetRelayResultPath());
	RootObject->SetStringField(TEXT("relay_cancel_request_path"), FOsvayderUERelayAgentManager::GetRelayCancelRequestPath());
	RootObject->SetStringField(TEXT("relay_agent_script_path"), FOsvayderUERelayAgentManager::GetRelayAgentScriptPath());
	RootObject->SetStringField(TEXT("active_plan_path"), FOsvayderUERelayAgentManager::GetActivePlanPath());
	RootObject->SetBoolField(TEXT("relay_handoff_context_present"), IFileManager::Get().FileExists(*FOsvayderUERelayAgentManager::GetHandoffContextPath()));
	RootObject->SetBoolField(TEXT("relay_progress_present"), IFileManager::Get().FileExists(*FOsvayderUERelayAgentManager::GetRelayProgressPath()));
	RootObject->SetBoolField(TEXT("relay_result_present"), IFileManager::Get().FileExists(*FOsvayderUERelayAgentManager::GetRelayResultPath()));
	RootObject->SetBoolField(TEXT("relay_cancel_request_present"), FOsvayderUERelayAgentManager::HasCancelRequest());
	RootObject->SetBoolField(TEXT("active_plan_present"), IFileManager::Get().FileExists(*FOsvayderUERelayAgentManager::GetActivePlanPath()));
	RootObject->SetBoolField(TEXT("project_local"), true);
	RootObject->SetStringField(TEXT("detached_supervisor_kind"), TEXT("powershell_local_task_owner_v2"));
	RootObject->SetStringField(TEXT("lane_scope"), TEXT("file_write_build_restore_relaunch_reattach"));
	RootObject->SetBoolField(TEXT("support_bundle_resolved"), bSupportBundleResolved);
	RootObject->SetStringField(TEXT("support_bundle_resolution"), SupportBundle.ResolutionLabel);
	RootObject->SetStringField(TEXT("support_bundle_root"), SupportBundle.BundleRoot);
	RootObject->SetStringField(TEXT("support_bundle_supervisor_script_path"), SupportBundle.SupervisorScriptPath);
	RootObject->SetStringField(TEXT("support_bundle_monitor_script_path"), SupportBundle.MonitorScriptPath);
	RootObject->SetStringField(TEXT("support_bundle_preflight_script_path"), SupportBundle.PreflightScriptPath);
	if (!SupportBundleError.IsEmpty())
	{
		RootObject->SetStringField(TEXT("support_bundle_detail"), SupportBundleError);
	}

	if (HasPreparedRestoreRequest())
	{
		FOsvayderUERestartSurvivalPreparedRestoreRequest Request;
		FString RequestError;
		if (LoadPreparedRestoreRequestInternal(Request, RequestError))
		{
			const TSharedRef<FJsonObject> RequestObject = MakeShared<FJsonObject>();
			WritePreparedRestoreRequestJson(Request, RequestObject);
			RequestObject->SetBoolField(TEXT("valid_json"), true);
			RootObject->SetObjectField(TEXT("prepared_restore_request"), RequestObject);
		}
		else
		{
			TSharedPtr<FJsonObject> RequestObject = MakeShared<FJsonObject>();
			RequestObject->SetBoolField(TEXT("valid_json"), false);
			RequestObject->SetStringField(TEXT("detail"), RequestError);
			RootObject->SetObjectField(TEXT("prepared_restore_request"), RequestObject);
		}
	}

	FOsvayderUERestartSurvivalState State;
	FString Error;
	if (!LoadStateInternal(State, Error))
	{
		RootObject->SetStringField(TEXT("phase"), TEXT("none"));
		RootObject->SetStringField(TEXT("detail"), Error);
		return RootObject;
	}

	const TSharedRef<FJsonObject> StateObject = MakeStateJson(State);
	RootObject->SetBoolField(TEXT("state_present"), true);
	RootObject->SetStringField(TEXT("phase"), OsvayderUERestartSurvivalPhaseToString(State.Phase));
	RootObject->SetStringField(TEXT("detail"), State.PhaseDetail);
	RootObject->SetStringField(TEXT("session_id"), State.SessionId);
	RootObject->SetStringField(TEXT("task_id"), State.TaskId);
	if (State.LaneState.HasAnySignal())
	{
		RootObject->SetObjectField(TEXT("lane_state"), State.LaneState.ToJsonObject());
	}
	RootObject->SetStringField(TEXT("backend"), OsvayderUEProviderBackendToString(State.Backend));
	RootObject->SetStringField(TEXT("backend_display_name"), State.BackendDisplayName);
	RootObject->SetStringField(TEXT("execution_control_profile_id"), State.ExecutionControlProfileId);
	RootObject->SetStringField(TEXT("execution_transport_label"), State.ExecutionTransportLabel);
	RootObject->SetStringField(TEXT("provider_session_id"), State.ProviderSessionId);
	RootObject->SetStringField(TEXT("provider_thread_state_path"), State.ProviderThreadStatePath);
	RootObject->SetBoolField(TEXT("provider_thread_resume_pending"), State.bProviderThreadResumePending);
	RootObject->SetStringField(TEXT("project_root"), State.ProjectRoot);
	RootObject->SetStringField(TEXT("uproject_path"), State.UProjectPath);
	RootObject->SetStringField(TEXT("reattach_token"), State.ReattachToken);
	RootObject->SetStringField(TEXT("reattach_notice"), State.ReattachNotice);
	RootObject->SetStringField(TEXT("failure_reason"), State.FailureReason);
	RootObject->SetStringField(TEXT("detached_objective"), State.DetachedObjective);
	RootObject->SetStringField(TEXT("detached_objective_detail"), State.DetachedObjectiveDetail);
	RootObject->SetNumberField(TEXT("detached_step_index"), State.DetachedStepIndex);
	RootObject->SetNumberField(TEXT("detached_step_budget"), State.DetachedStepBudget);
	RootObject->SetStringField(TEXT("detached_current_step"), State.DetachedCurrentStep);
	RootObject->SetStringField(TEXT("detached_pending_step"), State.DetachedPendingStep);
	RootObject->SetStringField(TEXT("detached_last_step_outcome"), State.DetachedLastStepOutcome);
	RootObject->SetStringField(TEXT("detached_last_blocker_family"), State.DetachedLastBlockerFamily);
	RootObject->SetStringField(TEXT("detached_last_blocker_signature"), State.DetachedLastBlockerSignature);
	RootObject->SetStringField(TEXT("detached_terminal_outcome"), State.DetachedTerminalOutcome);
	RootObject->SetBoolField(TEXT("detached_file_write_completed"), State.bDetachedFileWriteCompleted);
	RootObject->SetBoolField(TEXT("detached_restore_completed"), State.bDetachedRestoreCompleted);
	RootObject->SetBoolField(TEXT("detached_build_completed"), State.bDetachedBuildCompleted);
	RootObject->SetNumberField(TEXT("detached_owner_process_id"), State.DetachedOwnerProcessId);
	RootObject->SetBoolField(TEXT("detached_owner_active"), State.bDetachedOwnerActive);
	RootObject->SetBoolField(TEXT("detached_owner_manual_reopen_detected"), State.bDetachedOwnerManualReopenDetected);
	RootObject->SetBoolField(TEXT("detached_owner_crash_observed"), State.bDetachedOwnerCrashObserved);
	RootObject->SetStringField(TEXT("created_at_utc"), State.CreatedAtUtc);
	RootObject->SetStringField(TEXT("last_updated_at_utc"), State.LastUpdatedAtUtc);
	RootObject->SetStringField(TEXT("state_support_bundle_resolution"), State.SupportBundleResolution);
	RootObject->SetStringField(TEXT("state_support_bundle_root"), State.SupportBundleRoot);
	RootObject->SetStringField(TEXT("state_supervisor_script_path"), State.SupervisorScriptPath);
	RootObject->SetStringField(TEXT("state_monitor_script_path"), State.MonitorScriptPath);
	RootObject->SetStringField(TEXT("state_preflight_script_path"), State.PreflightLauncherPath);
	RootObject->SetObjectField(TEXT("restore_intent"), StateObject->GetObjectField(TEXT("restore_intent")));
	RootObject->SetObjectField(TEXT("file_write_intent"), StateObject->GetObjectField(TEXT("file_write_intent")));
	RootObject->SetObjectField(TEXT("proof"), StateObject->GetObjectField(TEXT("proof")));

	FOsvayderUEActivePlan ActivePlan;
	FString ActivePlanError;
	if (FOsvayderUERelayAgentManager::LoadActivePlan(ActivePlan, ActivePlanError))
	{
		TSharedPtr<FJsonObject> ActivePlanObject = MakeShared<FJsonObject>();
		ActivePlanObject->SetStringField(TEXT("plan_id"), ActivePlan.PlanId);
		ActivePlanObject->SetStringField(TEXT("reviewer_plan_reference"), ActivePlan.ReviewerPlanReference);
		ActivePlanObject->SetStringField(TEXT("original_user_task"), ActivePlan.OriginalUserTask);
		ActivePlanObject->SetStringField(TEXT("status"), ActivePlan.Status);
		ActivePlanObject->SetStringField(TEXT("result_status"), ActivePlan.ResultStatus);
		ActivePlanObject->SetStringField(TEXT("summary"), ActivePlan.Summary);
		ActivePlanObject->SetStringField(TEXT("summary_ru"), ActivePlan.SummaryRu);
		ActivePlanObject->SetStringField(TEXT("technical_detail"), ActivePlan.TechnicalDetail);
		ActivePlanObject->SetStringField(TEXT("current_mechanic_id"), ActivePlan.CurrentMechanicId);
		ActivePlanObject->SetStringField(TEXT("current_tool_call_id"), ActivePlan.CurrentToolCallId);
		ActivePlanObject->SetStringField(TEXT("current_action"), ActivePlan.CurrentAction);
		ActivePlanObject->SetStringField(TEXT("current_action_ru"), ActivePlan.CurrentActionRu);
		ActivePlanObject->SetStringField(TEXT("resume_hint"), ActivePlan.ResumeHint);
		if (ActivePlan.LaneState.HasAnySignal())
		{
			ActivePlanObject->SetObjectField(TEXT("lane_state"), ActivePlan.LaneState.ToJsonObject());
		}
		ActivePlanObject->SetStringField(TEXT("handoff_policy"), ActivePlan.HandoffPolicy);
		ActivePlanObject->SetStringField(TEXT("hybrid_split_reason"), ActivePlan.HybridSplitReason);
		ActivePlanObject->SetBoolField(TEXT("hybrid_split_triggered"), ActivePlan.bHybridSplitTriggered);
		ActivePlanObject->SetBoolField(TEXT("post_reattach_verification_required"), ActivePlan.bPostReattachVerificationRequired);
		ActivePlanObject->SetBoolField(TEXT("visual_proof_required"), ActivePlan.bVisualProofRequired);
		ActivePlanObject->SetBoolField(TEXT("visual_qa_manifest_required"), ActivePlan.bVisualQaManifestRequired);
		ActivePlanObject->SetStringField(TEXT("visual_proof_requirement"), ActivePlan.VisualProofRequirement);
		ActivePlanObject->SetStringField(TEXT("visual_proof_status"), ActivePlan.VisualProofStatus);
		ActivePlanObject->SetStringField(TEXT("visual_proof_artifact_path"), ActivePlan.VisualProofArtifactPath);
		ActivePlanObject->SetStringField(TEXT("visual_proof_blocker"), ActivePlan.VisualProofBlocker);
		ActivePlanObject->SetStringField(TEXT("visual_qa_manifest_path"), ActivePlan.VisualQaManifestPath);
		ActivePlanObject->SetStringField(TEXT("visual_qa_manifest_verdict"), ActivePlan.VisualQaManifestVerdict);
		ActivePlanObject->SetArrayField(TEXT("completed_mechanic_ids"), FJsonUtils::StringArrayToJson(ActivePlan.CompletedMechanicIds));
		ActivePlanObject->SetArrayField(TEXT("verification_checklist"), FJsonUtils::StringArrayToJson(ActivePlan.VerificationChecklist));
		ActivePlanObject->SetArrayField(TEXT("visual_reference_artifact_paths"), FJsonUtils::StringArrayToJson(ActivePlan.VisualReferenceArtifactPaths));

		TArray<TSharedPtr<FJsonValue>> MechanicsArray;
		for (const FOsvayderUEPlanMechanicEntry& Mechanic : ActivePlan.Mechanics)
		{
			TSharedPtr<FJsonObject> MechanicObject = MakeShared<FJsonObject>();
			MechanicObject->SetStringField(TEXT("mechanic_id"), Mechanic.MechanicId);
			MechanicObject->SetStringField(TEXT("label"), Mechanic.Label);
			MechanicObject->SetStringField(TEXT("label_ru"), Mechanic.LabelRu);
			MechanicObject->SetStringField(TEXT("status"), Mechanic.Status);
			MechanicObject->SetStringField(TEXT("last_summary"), Mechanic.LastSummary);
			MechanicObject->SetStringField(TEXT("last_summary_ru"), Mechanic.LastSummaryRu);
			MechanicsArray.Add(MakeShared<FJsonValueObject>(MechanicObject));
		}
		ActivePlanObject->SetArrayField(TEXT("mechanics"), MechanicsArray);

		const FOsvayderUERelaySettingsSnapshot CurrentSettings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
		const bool bSettingsParityMatched =
			ActivePlan.Settings.Model.Equals(CurrentSettings.Model, ESearchCase::CaseSensitive)
			&& ActivePlan.Settings.Profile.Equals(CurrentSettings.Profile, ESearchCase::CaseSensitive)
			&& ActivePlan.Settings.WorkMode.Equals(CurrentSettings.WorkMode, ESearchCase::CaseSensitive)
			&& ActivePlan.Settings.ReasoningEffort.Equals(CurrentSettings.ReasoningEffort, ESearchCase::CaseSensitive)
			&& ActivePlan.Settings.Verbosity.Equals(CurrentSettings.Verbosity, ESearchCase::CaseSensitive);
		ActivePlanObject->SetStringField(TEXT("settings_parity"), bSettingsParityMatched ? TEXT("matched") : TEXT("mismatch"));
		RootObject->SetObjectField(TEXT("active_plan"), ActivePlanObject);
	}
	else if (!ActivePlanError.IsEmpty())
	{
		RootObject->SetStringField(TEXT("active_plan_detail"), ActivePlanError);
	}

	FOsvayderUERelayProgressEntry RelayProgress;
	FString RelayProgressError;
	if (!State.TaskId.IsEmpty()
		&& FOsvayderUERelayAgentManager::LoadLatestProgressEntryForTask(State.TaskId, RelayProgress, RelayProgressError))
	{
		TSharedPtr<FJsonObject> RelayProgressObject = MakeShared<FJsonObject>();
		RelayProgressObject->SetStringField(TEXT("task_id"), RelayProgress.TaskId);
		RelayProgressObject->SetStringField(TEXT("plan_id"), RelayProgress.PlanId);
		RelayProgressObject->SetStringField(TEXT("mechanic_id"), RelayProgress.MechanicId);
		RelayProgressObject->SetStringField(TEXT("tool_call_id"), RelayProgress.ToolCallId);
		RelayProgressObject->SetStringField(TEXT("relay_session_id"), RelayProgress.RelaySessionId);
		RelayProgressObject->SetStringField(TEXT("entry_kind"), RelayProgress.EntryKind);
		RelayProgressObject->SetStringField(TEXT("summary"), RelayProgress.Summary);
		RelayProgressObject->SetStringField(TEXT("summary_ru"), RelayProgress.SummaryRu);
		RelayProgressObject->SetStringField(TEXT("technical_detail"), RelayProgress.TechnicalDetail);
		RelayProgressObject->SetStringField(TEXT("current_action"), RelayProgress.CurrentAction);
		RelayProgressObject->SetStringField(TEXT("current_action_ru"), RelayProgress.CurrentActionRu);
		RelayProgressObject->SetStringField(TEXT("current_tool_name"), RelayProgress.CurrentToolName);
		RelayProgressObject->SetNumberField(TEXT("iteration_index"), RelayProgress.IterationIndex);
		RelayProgressObject->SetNumberField(TEXT("elapsed_seconds"), RelayProgress.ElapsedSeconds);
		RelayProgressObject->SetNumberField(TEXT("heartbeat_age_seconds"), RelayProgress.HeartbeatAgeSeconds);
		RelayProgressObject->SetBoolField(TEXT("is_stale"), RelayProgress.bIsStale);
		RelayProgressObject->SetStringField(TEXT("terminal_outcome"), OsvayderUERelayTerminalOutcomeToString(RelayProgress.TerminalOutcome));
		RootObject->SetObjectField(TEXT("relay_latest_progress"), RelayProgressObject);
	}
	else if (!RelayProgressError.IsEmpty()
		&& State.DetachedObjective.Equals(TEXT("closed_editor_complex_build_relay_v1"), ESearchCase::CaseSensitive))
	{
		RootObject->SetStringField(TEXT("relay_progress_detail"), RelayProgressError);
	}

	FOsvayderUERelayResult RelayResult;
	FString RelayResultError;
	if (!State.TaskId.IsEmpty()
		&& FOsvayderUERelayAgentManager::LoadRelayResultForTask(State.TaskId, RelayResult, RelayResultError))
	{
		TSharedPtr<FJsonObject> RelayResultObject = MakeShared<FJsonObject>();
		RelayResultObject->SetStringField(TEXT("artifact_type"), RelayResult.ArtifactType);
		RelayResultObject->SetStringField(TEXT("task_id"), RelayResult.TaskId);
		RelayResultObject->SetStringField(TEXT("plan_id"), RelayResult.PlanId);
		RelayResultObject->SetStringField(TEXT("workflow_id"), RelayResult.WorkflowId);
		RelayResultObject->SetStringField(TEXT("current_mechanic_id"), RelayResult.CurrentMechanicId);
		RelayResultObject->SetStringField(TEXT("current_tool_call_id"), RelayResult.CurrentToolCallId);
		RelayResultObject->SetStringField(TEXT("relay_session_id"), RelayResult.RelaySessionId);
		RelayResultObject->SetStringField(TEXT("completed_at_utc"), RelayResult.CompletedAtUtc);
		RelayResultObject->SetStringField(TEXT("terminal_outcome"), OsvayderUERelayTerminalOutcomeToString(RelayResult.TerminalOutcome));
		RelayResultObject->SetStringField(TEXT("status"), RelayResult.Status);
		RelayResultObject->SetStringField(TEXT("origin_prompt_hash"), RelayResult.OriginPromptHash);
		RelayResultObject->SetStringField(TEXT("summary"), RelayResult.Summary);
		RelayResultObject->SetStringField(TEXT("summary_ru"), RelayResult.SummaryRu);
		RelayResultObject->SetStringField(TEXT("technical_detail"), RelayResult.TechnicalDetail);
		RelayResultObject->SetStringField(TEXT("final_blocker_family"), RelayResult.FinalBlockerFamily);
		RelayResultObject->SetStringField(TEXT("final_blocker_signature"), RelayResult.FinalBlockerSignature);
		RelayResultObject->SetStringField(TEXT("plan_status"), RelayResult.PlanStatus);
		RelayResultObject->SetStringField(TEXT("next_resume_phase"), RelayResult.NextResumePhase);
		RelayResultObject->SetStringField(TEXT("build_command"), RelayResult.BuildCommand);
		RelayResultObject->SetStringField(TEXT("build_log_path"), RelayResult.BuildLogPath);
		RelayResultObject->SetStringField(TEXT("target_proof_status"), RelayResult.TargetProofStatus);
		RelayResultObject->SetBoolField(TEXT("requires_post_reattach_verification"), RelayResult.bRequiresPostReattachVerification);
		RelayResultObject->SetArrayField(TEXT("completed_mechanic_ids"), FJsonUtils::StringArrayToJson(RelayResult.CompletedMechanicIds));
		RelayResultObject->SetArrayField(TEXT("changed_files"), FJsonUtils::StringArrayToJson(RelayResult.ChangedFiles));
		RelayResultObject->SetArrayField(TEXT("required_live_checks"), FJsonUtils::StringArrayToJson(RelayResult.RequiredLiveChecks));
		RelayResultObject->SetStringField(TEXT("relay_progress_path"), RelayResult.RelayProgressPath);
		RelayResultObject->SetNumberField(TEXT("iterations_used"), RelayResult.IterationsUsed);
		RelayResultObject->SetNumberField(TEXT("elapsed_seconds"), RelayResult.ElapsedSeconds);
		RelayResultObject->SetStringField(TEXT("reattach_summary"), RelayResult.ReattachSummary);
		RootObject->SetObjectField(TEXT("relay_result"), RelayResultObject);
	}
	else if (!RelayResultError.IsEmpty()
		&& State.DetachedObjective.Equals(TEXT("closed_editor_complex_build_relay_v1"), ESearchCase::CaseSensitive))
	{
		RootObject->SetStringField(TEXT("relay_result_detail"), RelayResultError);
	}

	TSharedPtr<FJsonObject> RelaySettingsObject = MakeShared<FJsonObject>();
	const FOsvayderUERelaySettingsSnapshot RelaySettings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
	RelaySettingsObject->SetStringField(TEXT("backend"), RelaySettings.Backend);
	RelaySettingsObject->SetStringField(TEXT("backend_display_name"), RelaySettings.BackendDisplayName);
	RelaySettingsObject->SetStringField(TEXT("model"), RelaySettings.Model);
	RelaySettingsObject->SetStringField(TEXT("profile"), RelaySettings.Profile);
	RelaySettingsObject->SetStringField(TEXT("requested_speed_mode"), RelaySettings.RequestedSpeedMode);
	RelaySettingsObject->SetStringField(TEXT("effective_speed_mode"), RelaySettings.EffectiveSpeedMode);
	RelaySettingsObject->SetStringField(TEXT("speed_support"), RelaySettings.SpeedSupport);
	RelaySettingsObject->SetStringField(TEXT("work_mode"), RelaySettings.WorkMode);
	RelaySettingsObject->SetStringField(TEXT("reasoning_effort"), RelaySettings.ReasoningEffort);
	RelaySettingsObject->SetStringField(TEXT("verbosity"), RelaySettings.Verbosity);
	RelaySettingsObject->SetStringField(TEXT("auth_mode"), RelaySettings.AuthMode);
	RelaySettingsObject->SetStringField(TEXT("auth_path"), RelaySettings.AuthPath);
	RelaySettingsObject->SetStringField(TEXT("auth_ownership"), RelaySettings.AuthOwnership);
	RelaySettingsObject->SetStringField(TEXT("codex_home_path"), RelaySettings.CodexHomePath);
	RelaySettingsObject->SetStringField(TEXT("codex_home_resolution_source"), RelaySettings.CodexHomeResolutionSource);
	RelaySettingsObject->SetStringField(TEXT("execution_transport"), RelaySettings.ExecutionTransport);
	RelaySettingsObject->SetBoolField(TEXT("persistent_app_server_enabled"), RelaySettings.bPersistentAppServerEnabled);
	RelaySettingsObject->SetBoolField(TEXT("clear_proxy_env_for_exec"), RelaySettings.bClearProxyEnvForExec);
	RelaySettingsObject->SetBoolField(TEXT("has_explicit_codex_home_override"), RelaySettings.bHasExplicitCodexHomeOverride);
	RootObject->SetObjectField(TEXT("relay_settings_snapshot"), RelaySettingsObject);
	return RootObject;
}

FString FOsvayderUERestartSurvivalManager::BuildWidgetDebugSummary()
{
	FOsvayderUERestartSurvivalSupportBundle SupportBundle;
	FString SupportBundleError;
	const bool bSupportBundleResolved = TryResolveSupportBundleInternal(SupportBundle, SupportBundleError);

	FOsvayderUERestartSurvivalState State;
	FString Error;
	if (!LoadStateInternal(State, Error))
	{
		FString Summary = FString::Printf(
			TEXT("restart_survival = none\nrestart_survival_state_path = %s"),
			*GetStatePathInternal());
		Summary += FString::Printf(TEXT("\nrestart_survival_support_bundle_resolved = %s"), bSupportBundleResolved ? TEXT("true") : TEXT("false"));
		Summary += FString::Printf(TEXT("\nrestart_survival_support_bundle_resolution = %s"), *SupportBundle.ResolutionLabel);
		Summary += FString::Printf(TEXT("\nrestart_survival_support_bundle_root = %s"), *SupportBundle.BundleRoot);
		Summary += FString::Printf(TEXT("\nrestart_survival_supervisor_script_path = %s"), *SupportBundle.SupervisorScriptPath);
		Summary += FString::Printf(TEXT("\nrestart_survival_monitor_script_path = %s"), *SupportBundle.MonitorScriptPath);
		Summary += FString::Printf(TEXT("\nrestart_survival_preflight_script_path = %s"), *SupportBundle.PreflightScriptPath);
		Summary += FString::Printf(TEXT("\nrelay_handoff_context_path = %s"), *FOsvayderUERelayAgentManager::GetHandoffContextPath());
		Summary += FString::Printf(TEXT("\nrelay_progress_path = %s"), *FOsvayderUERelayAgentManager::GetRelayProgressPath());
		Summary += FString::Printf(TEXT("\nrelay_result_path = %s"), *FOsvayderUERelayAgentManager::GetRelayResultPath());
		Summary += FString::Printf(TEXT("\nclosed_editor_result_path = %s"), *GetClosedEditorResultPathInternal());
		Summary += FString::Printf(TEXT("\nrelay_cancel_request_path = %s"), *FOsvayderUERelayAgentManager::GetRelayCancelRequestPath());
		Summary += FString::Printf(TEXT("\nrelay_agent_script_path = %s"), *FOsvayderUERelayAgentManager::GetRelayAgentScriptPath());
		if (!SupportBundleError.IsEmpty())
		{
			Summary += FString::Printf(TEXT("\nrestart_survival_support_bundle_detail = %s"), *SupportBundleError);
		}
		return Summary;
	}

	FString Summary = FString::Printf(TEXT("restart_survival = %s\n"), OsvayderUERestartSurvivalPhaseToString(State.Phase));
	Summary += FString::Printf(TEXT("restart_survival_task_id = %s\n"), *State.TaskId);
	Summary += FString::Printf(TEXT("restart_survival_transport = %s\n"), *State.DetachedSupervisorKind);
	Summary += FString::Printf(TEXT("restart_survival_lane = %s\n"), *State.LaneState.GetEffectiveCurrentLane());
	Summary += FString::Printf(TEXT("restart_survival_lane_transition_kind = %s\n"), *State.LaneState.TransitionKind);
	Summary += FString::Printf(TEXT("restart_survival_lane_transition_state = %s\n"), *State.LaneState.TransitionState);
	Summary += FString::Printf(TEXT("restart_survival_resume_pending = %s\n"), State.bProviderThreadResumePending ? TEXT("true") : TEXT("false"));
	Summary += FString::Printf(TEXT("restart_survival_detached_objective = %s\n"), *State.DetachedObjective);
	Summary += FString::Printf(TEXT("restart_survival_detached_step = %d/%d\n"), State.DetachedStepIndex, State.DetachedStepBudget);
	Summary += FString::Printf(TEXT("restart_survival_detached_pending_step = %s\n"), *State.DetachedPendingStep);
	Summary += FString::Printf(TEXT("restart_survival_detached_owner_active = %s\n"), State.bDetachedOwnerActive ? TEXT("true") : TEXT("false"));
	Summary += FString::Printf(TEXT("restart_survival_detached_owner_manual_reopen_detected = %s\n"), State.bDetachedOwnerManualReopenDetected ? TEXT("true") : TEXT("false"));
	Summary += FString::Printf(TEXT("restart_survival_detached_terminal_outcome = %s\n"), *State.DetachedTerminalOutcome);
	Summary += FString::Printf(TEXT("restart_survival_state_path = %s\n"), *GetStatePathInternal());
	Summary += FString::Printf(TEXT("restart_survival_restore_request_path = %s\n"), *GetPreparedRestoreRequestPathInternal());
	Summary += FString::Printf(TEXT("restart_survival_restore_request_present = %s\n"), HasPreparedRestoreRequest() ? TEXT("true") : TEXT("false"));
	Summary += FString::Printf(TEXT("closed_editor_result_path = %s\n"), *GetClosedEditorResultPathInternal());
	{
		FString ResolveError;
		OsvayderUEStorageMigration::FManagedReadResult ManagedRead;
		const bool bClosedEditorResultPresent = OsvayderUEStorageMigration::ResolveManagedReadPath(
			GetClosedEditorResultPathInternal(),
			GetLegacyClosedEditorResultPathInternal(),
			TEXT("closed_editor_result"),
			[](const FString& CandidatePath, FString& OutValidationError)
			{
				if (IFileManager::Get().FileExists(*CandidatePath))
				{
					return true;
				}

				OutValidationError = FString::Printf(TEXT("Closed-editor result file not found at %s"), *CandidatePath);
				return false;
			},
			ManagedRead,
			ResolveError);
		Summary += FString::Printf(TEXT("closed_editor_result_present = %s\n"), bClosedEditorResultPresent ? TEXT("true") : TEXT("false"));
	}
	Summary += FString::Printf(TEXT("restart_survival_restore_request_auto_start_armed = %s\n"), GPreparedRequestAutoStartArm.IsSet() ? TEXT("true") : TEXT("false"));
	Summary += FString::Printf(TEXT("restart_survival_support_bundle_resolution = %s\n"), *State.SupportBundleResolution);
	Summary += FString::Printf(TEXT("restart_survival_support_bundle_root = %s\n"), *State.SupportBundleRoot);
	Summary += FString::Printf(TEXT("restart_survival_supervisor_script_path = %s\n"), *State.SupervisorScriptPath);
	Summary += FString::Printf(TEXT("restart_survival_monitor_script_path = %s\n"), *State.MonitorScriptPath);
	Summary += FString::Printf(TEXT("restart_survival_preflight_script_path = %s\n"), *State.PreflightLauncherPath);
	Summary += FString::Printf(TEXT("active_plan_path = %s\n"), *FOsvayderUERelayAgentManager::GetActivePlanPath());

	FOsvayderUEActivePlan ActivePlan;
	FString ActivePlanError;
	if (FOsvayderUERelayAgentManager::LoadActivePlan(ActivePlan, ActivePlanError))
	{
		const FOsvayderUERelaySettingsSnapshot CurrentSettings = FOsvayderUERelayAgentManager::BuildCurrentCodexSettingsSnapshot();
		const FString SettingsParity =
			ActivePlan.Settings.Model.Equals(CurrentSettings.Model, ESearchCase::CaseSensitive)
			&& ActivePlan.Settings.Profile.Equals(CurrentSettings.Profile, ESearchCase::CaseSensitive)
			&& ActivePlan.Settings.WorkMode.Equals(CurrentSettings.WorkMode, ESearchCase::CaseSensitive)
			&& ActivePlan.Settings.ReasoningEffort.Equals(CurrentSettings.ReasoningEffort, ESearchCase::CaseSensitive)
			&& ActivePlan.Settings.Verbosity.Equals(CurrentSettings.Verbosity, ESearchCase::CaseSensitive)
			? TEXT("matched")
			: TEXT("mismatch");
		Summary += FString::Printf(TEXT("active_plan_status = %s\n"), *ActivePlan.Status);
		Summary += FString::Printf(TEXT("active_plan_id = %s\n"), *ActivePlan.PlanId);
		Summary += FString::Printf(TEXT("active_plan_current_mechanic = %s\n"), *ActivePlan.CurrentMechanicId);
		Summary += FString::Printf(TEXT("active_plan_current_tool_call = %s\n"), *ActivePlan.CurrentToolCallId);
		Summary += FString::Printf(TEXT("active_plan_lane = %s\n"), *ActivePlan.LaneState.GetEffectiveCurrentLane());
		Summary += FString::Printf(TEXT("active_plan_lane_transition_kind = %s\n"), *ActivePlan.LaneState.TransitionKind);
		Summary += FString::Printf(TEXT("active_plan_lane_transition_state = %s\n"), *ActivePlan.LaneState.TransitionState);
		Summary += FString::Printf(TEXT("active_plan_summary_ru = %s\n"), *ActivePlan.SummaryRu);
		Summary += FString::Printf(TEXT("active_plan_resume_hint = %s\n"), *ActivePlan.ResumeHint);
		Summary += FString::Printf(TEXT("active_plan_settings_parity = %s\n"), *SettingsParity);
	}
	else if (!ActivePlanError.IsEmpty())
	{
		Summary += FString::Printf(TEXT("active_plan_detail = %s\n"), *ActivePlanError);
	}
	Summary += FString::Printf(TEXT("relay_handoff_context_path = %s\n"), *FOsvayderUERelayAgentManager::GetHandoffContextPath());
	Summary += FString::Printf(TEXT("relay_progress_path = %s\n"), *FOsvayderUERelayAgentManager::GetRelayProgressPath());
	Summary += FString::Printf(TEXT("relay_result_path = %s\n"), *FOsvayderUERelayAgentManager::GetRelayResultPath());
	Summary += FString::Printf(TEXT("relay_cancel_request_path = %s\n"), *FOsvayderUERelayAgentManager::GetRelayCancelRequestPath());
	Summary += FString::Printf(TEXT("relay_agent_script_path = %s\n"), *FOsvayderUERelayAgentManager::GetRelayAgentScriptPath());
	Summary += FString::Printf(TEXT("relay_cancel_request_present = %s\n"), FOsvayderUERelayAgentManager::HasCancelRequest() ? TEXT("true") : TEXT("false"));

	FOsvayderUERelayProgressEntry RelayProgress;
	FString RelayProgressError;
	if (!State.TaskId.IsEmpty()
		&& FOsvayderUERelayAgentManager::LoadLatestProgressEntryForTask(State.TaskId, RelayProgress, RelayProgressError))
	{
		Summary += FString::Printf(TEXT("relay_latest_summary = %s\n"), *RelayProgress.Summary);
		Summary += FString::Printf(TEXT("relay_latest_action = %s\n"), *RelayProgress.CurrentAction);
		Summary += FString::Printf(TEXT("relay_latest_tool = %s\n"), *RelayProgress.CurrentToolName);
		Summary += FString::Printf(TEXT("relay_latest_iteration = %d\n"), RelayProgress.IterationIndex);
		Summary += FString::Printf(TEXT("relay_latest_elapsed_seconds = %.3f\n"), RelayProgress.ElapsedSeconds);
		Summary += FString::Printf(TEXT("relay_latest_heartbeat_age_seconds = %.3f\n"), RelayProgress.HeartbeatAgeSeconds);
		Summary += FString::Printf(TEXT("relay_latest_is_stale = %s\n"), RelayProgress.bIsStale ? TEXT("true") : TEXT("false"));
		Summary += FString::Printf(TEXT("relay_latest_terminal_outcome = %s\n"), OsvayderUERelayTerminalOutcomeToString(RelayProgress.TerminalOutcome));
	}
	else if (!RelayProgressError.IsEmpty()
		&& State.DetachedObjective.Equals(TEXT("closed_editor_complex_build_relay_v1"), ESearchCase::CaseSensitive))
	{
		Summary += FString::Printf(TEXT("relay_progress_detail = %s\n"), *RelayProgressError);
	}

	FOsvayderUERelayResult RelayResult;
	FString RelayResultError;
	if (!State.TaskId.IsEmpty()
		&& FOsvayderUERelayAgentManager::LoadRelayResultForTask(State.TaskId, RelayResult, RelayResultError))
	{
		Summary += FString::Printf(TEXT("relay_terminal_outcome = %s\n"), OsvayderUERelayTerminalOutcomeToString(RelayResult.TerminalOutcome));
		Summary += FString::Printf(TEXT("relay_terminal_summary = %s\n"), *RelayResult.Summary);
		Summary += FString::Printf(TEXT("relay_terminal_blocker_family = %s\n"), *RelayResult.FinalBlockerFamily);
		Summary += FString::Printf(TEXT("relay_terminal_blocker_signature = %s\n"), *RelayResult.FinalBlockerSignature);
	}
	else if (!RelayResultError.IsEmpty()
		&& State.DetachedObjective.Equals(TEXT("closed_editor_complex_build_relay_v1"), ESearchCase::CaseSensitive))
	{
		Summary += FString::Printf(TEXT("relay_result_detail = %s\n"), *RelayResultError);
	}

	if (!State.ProviderSessionId.IsEmpty())
	{
		Summary += FString::Printf(TEXT("restart_survival_provider_session_id = %s\n"), *State.ProviderSessionId);
	}
	if (!State.PhaseDetail.IsEmpty())
	{
		Summary += FString::Printf(TEXT("restart_survival_detail = %s\n"), *State.PhaseDetail);
	}
	Summary.TrimEndInline();
	return Summary;
}

bool FOsvayderUERestartSurvivalManager::ExecuteSpawnBeforeTransportResetSequence(
	TFunctionRef<bool()> WriteSpawnRequestFn,
	TFunctionRef<void()> TransportResetFn)
{
	// Ordering contract for 624: persist the spawn-request artifact first, then
	// reset the transport. If the spawn-request write fails, skip the reset so
	// the caller can surface the failure without leaving the turn in an
	// indeterminate state.
	if (!WriteSpawnRequestFn())
	{
		return false;
	}

	TransportResetFn();
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
void FOsvayderUERestartSurvivalManager::SetTestStateRootOverride(const FString& InDir)
{
	GTestRestartSurvivalRootOverride = InDir;
}

void FOsvayderUERestartSurvivalManager::ClearTestStateRootOverride()
{
	GTestRestartSurvivalRootOverride.Empty();
}
#endif
