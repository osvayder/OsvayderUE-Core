// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUECommandClassification.h"

#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace OsvayderUE
{
namespace CommandClassification
{

namespace
{
	FString NormalizeTruthText(const FString& Text)
	{
		FString Normalized = Text.ToLower();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Normalized;
	}

	bool ContainsAnyTruthToken(const FString& LowerHaystack, const TArray<FString>& Needles)
	{
		for (const FString& Needle : Needles)
		{
			if (LowerHaystack.Contains(Needle, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	void AddUniqueTrimmedCommandText(TArray<FString>& OutCommandTexts, const FString& Text)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			OutCommandTexts.AddUnique(Trimmed);
		}
	}

	TArray<FString> ExtractRawJsonCommandTexts(const FString& RawJson)
	{
		TArray<FString> CommandTexts;
		if (RawJson.TrimStartAndEnd().IsEmpty())
		{
			return CommandTexts;
		}

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			return CommandTexts;
		}

		const TArray<TSharedPtr<FJsonValue>>* CommandActions = nullptr;
		if (RootObject->TryGetArrayField(TEXT("commandActions"), CommandActions) && CommandActions != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& ActionValue : *CommandActions)
			{
				const TSharedPtr<FJsonObject> ActionObject = ActionValue.IsValid()
					? ActionValue->AsObject()
					: nullptr;
				if (!ActionObject.IsValid())
				{
					continue;
				}

				FString ActionCommand;
				if (ActionObject->TryGetStringField(TEXT("command"), ActionCommand))
				{
					AddUniqueTrimmedCommandText(CommandTexts, ActionCommand);
				}
			}
		}
		if (CommandTexts.Num() > 0)
		{
			return CommandTexts;
		}

		FString Command;
		if (RootObject->TryGetStringField(TEXT("command"), Command))
		{
			AddUniqueTrimmedCommandText(CommandTexts, Command);
		}

		return CommandTexts;
	}

	TArray<FString> BuildCommandAnalysisTexts(const FString& CommandInput, const FString& RawJson)
	{
		TArray<FString> CommandTexts = ExtractRawJsonCommandTexts(RawJson);
		if (CommandTexts.Num() > 0)
		{
			return CommandTexts;
		}

		AddUniqueTrimmedCommandText(CommandTexts, CommandInput);
		if (CommandTexts.Num() == 0)
		{
			AddUniqueTrimmedCommandText(CommandTexts, RawJson);
		}
		return CommandTexts;
	}

	FString StripQuotedShellLiterals(const FString& CommandText)
	{
		FString Result;
		Result.Reserve(CommandText.Len());

		bool bInSingleQuote = false;
		bool bInDoubleQuote = false;
		for (int32 Index = 0; Index < CommandText.Len(); ++Index)
		{
			const TCHAR Character = CommandText[Index];
			const TCHAR Previous = Index > 0 ? CommandText[Index - 1] : TCHAR(0);

			if (bInSingleQuote)
			{
				if (Character == TEXT('\''))
				{
					bInSingleQuote = false;
				}
				Result.AppendChar(TEXT(' '));
				continue;
			}

			if (bInDoubleQuote)
			{
				if (Character == TEXT('"') && Previous != TEXT('\\') && Previous != TEXT('`'))
				{
					bInDoubleQuote = false;
				}
				Result.AppendChar(TEXT(' '));
				continue;
			}

			if (Character == TEXT('\''))
			{
				bInSingleQuote = true;
				Result.AppendChar(TEXT(' '));
				continue;
			}

			if (Character == TEXT('"'))
			{
				bInDoubleQuote = true;
				Result.AppendChar(TEXT(' '));
				continue;
			}

			Result.AppendChar(Character);
		}

		return Result;
	}

	FString BuildExecutableShellText(const FString& CommandText)
	{
		return StripQuotedShellLiterals(StripKnownShellWrapperPrefixes(CommandText));
	}

	bool HasShellMutationVerb(const FString& CommandText)
	{
		const FString LowerCommand = NormalizeTruthText(BuildExecutableShellText(CommandText));
		const FString Padded = FString(TEXT(" ")) + LowerCommand + TEXT(" ");
		return ContainsAnyTruthToken(
			Padded,
			{
				TEXT(" set-content"), TEXT("|set-content"), TEXT("| set-content"),
				TEXT(" add-content"), TEXT("|add-content"), TEXT("| add-content"),
				TEXT(" out-file"), TEXT("|out-file"), TEXT("| out-file"),
				TEXT(" copy-item"), TEXT(" move-item"), TEXT(" remove-item"),
				TEXT(" rename-item"), TEXT(" new-item"), TEXT(" apply_patch"),
				TEXT(" git apply"), TEXT(" del "), TEXT(" rm "), TEXT(" ni "),
				TEXT(" >"), TEXT(">>")
			});
	}

	bool HasShellInspectionVerb(const FString& CommandText)
	{
		const FString LowerCommand = NormalizeTruthText(BuildExecutableShellText(CommandText));
		const FString Padded = FString(TEXT(" ")) + LowerCommand + TEXT(" ");
		return ContainsAnyTruthToken(
			Padded,
			{
				TEXT(" get-content"), TEXT("|get-content"), TEXT("| get-content"),
				TEXT(" select-string"), TEXT("|select-string"), TEXT("| select-string"),
				TEXT(" test-path"), TEXT("|test-path"), TEXT("| test-path"),
				TEXT(" resolve-path"), TEXT("|resolve-path"), TEXT("| resolve-path"),
				TEXT(" get-childitem"), TEXT("|get-childitem"), TEXT("| get-childitem"),
				TEXT(" rg "), TEXT(" rg.exe"), TEXT("|rg "), TEXT("| rg "),
				TEXT(" grep "), TEXT("|grep "), TEXT("| grep "),
				TEXT(" findstr "), TEXT("|findstr "), TEXT("| findstr "),
				TEXT(" cat "), TEXT("|cat "), TEXT("| cat "),
				TEXT(" type "), TEXT("|type "), TEXT("| type "),
				TEXT(" gc "), TEXT("|gc "), TEXT("| gc ")
			});
	}

	bool IsDriveRootToken(const FString& Token)
	{
		return Token.Len() == 3
			&& FChar::IsAlpha(Token[0])
			&& Token[1] == TEXT(':')
			&& Token[2] == TEXT('/');
	}

	bool ContainsStandaloneDriveRootOperand(const FString& CommandText)
	{
		TArray<FString> Tokens;
		CommandText.ParseIntoArrayWS(Tokens);
		for (FString Token : Tokens)
		{
			Token = NormalizeTruthText(Token);
			Token.TrimStartAndEndInline();
			while (!Token.IsEmpty())
			{
				const TCHAR First = Token[0];
				if (First != TEXT('"')
					&& First != TEXT('\'')
					&& First != TEXT('('))
				{
					break;
				}
				Token.RightChopInline(1, EAllowShrinking::No);
			}
			while (!Token.IsEmpty())
			{
				const TCHAR Last = Token[Token.Len() - 1];
				if (Last != TEXT('"')
					&& Last != TEXT('\'')
					&& Last != TEXT(',')
					&& Last != TEXT(';')
					&& Last != TEXT(')'))
				{
					break;
				}
				Token.LeftChopInline(1, EAllowShrinking::No);
			}
			if (IsDriveRootToken(Token))
			{
				return true;
			}
		}

		return false;
	}

	bool IsOverbroadInspectionCommand(const FString& CommandText)
	{
		const FString LowerCommand = NormalizeTruthText(CommandText);
		const FString LowerExecutable = NormalizeTruthText(BuildExecutableShellText(CommandText));
		const FString PaddedExecutable = FString(TEXT(" ")) + LowerExecutable + TEXT(" ");
		const bool bRipgrepLike = ContainsAnyTruthToken(
			PaddedExecutable,
			{
				TEXT(" rg "), TEXT(" rg.exe"),
				TEXT(" grep "), TEXT(" findstr ")
			});
		const bool bRecursiveChildEnumeration =
			(LowerExecutable.Contains(TEXT("get-childitem"), ESearchCase::CaseSensitive)
				|| LowerExecutable.Contains(TEXT(" gci "), ESearchCase::CaseSensitive)
				|| LowerExecutable.StartsWith(TEXT("gci "), ESearchCase::CaseSensitive))
			&& LowerExecutable.Contains(TEXT("-recurse"), ESearchCase::CaseSensitive);
		if (!bRipgrepLike && !bRecursiveChildEnumeration)
		{
			return false;
		}

		if (ContainsStandaloneDriveRootOperand(CommandText))
		{
			return true;
		}

		const bool bTouchesEpicGamesInstallRoot =
			LowerCommand.Contains(TEXT("program files/epic games"), ESearchCase::CaseSensitive)
			&& !LowerCommand.Contains(TEXT("engine/source/"), ESearchCase::CaseSensitive)
			&& !LowerCommand.Contains(TEXT(".h"), ESearchCase::CaseSensitive)
			&& !LowerCommand.Contains(TEXT(".hpp"), ESearchCase::CaseSensitive)
			&& !LowerCommand.Contains(TEXT(".cpp"), ESearchCase::CaseSensitive)
			&& !LowerCommand.Contains(TEXT(".inl"), ESearchCase::CaseSensitive);
		return bTouchesEpicGamesInstallRoot;
	}

	bool HasAutomationTestInvocation(const FString& LowerCommand)
	{
		return LowerCommand.Contains(TEXT("automation runtests"), ESearchCase::CaseSensitive)
			|| (LowerCommand.Contains(TEXT("-execcmds"), ESearchCase::CaseSensitive)
				&& LowerCommand.Contains(TEXT("runtests"), ESearchCase::CaseSensitive));
	}

	bool HasAutomationTypedMarkers(const FString& LowerEvidence)
	{
		return LowerEvidence.Contains(TEXT("automation tests"), ESearchCase::CaseSensitive)
			&& LowerEvidence.Contains(TEXT("result={success}"), ESearchCase::CaseSensitive)
			&& LowerEvidence.Contains(TEXT("test complete"), ESearchCase::CaseSensitive);
	}

	bool HasExitZeroMarker(const FString& LowerEvidence)
	{
		return LowerEvidence.Contains(TEXT("exit code: 0"), ESearchCase::CaseSensitive)
			|| LowerEvidence.Contains(TEXT("exitcode=0"), ESearchCase::CaseSensitive)
			|| LowerEvidence.Contains(TEXT("exit_code=0"), ESearchCase::CaseSensitive)
			|| LowerEvidence.Contains(TEXT("exited with code 0"), ESearchCase::CaseSensitive);
	}

	TArray<FString> ExtractExecutionTruthArtifactPaths(const FString& Text)
	{
		TArray<FString> Paths;
		TArray<FString> Tokens;
		Text.ParseIntoArrayWS(Tokens);
		for (FString Token : Tokens)
		{
			Token.ReplaceInline(TEXT("\\\""), TEXT(""));
			Token.ReplaceInline(TEXT("\""), TEXT(""));
			Token.ReplaceInline(TEXT("'"), TEXT(""));
			Token.ReplaceInline(TEXT(","), TEXT(""));
			Token.ReplaceInline(TEXT(";"), TEXT(""));
			Token.TrimStartAndEndInline();
			const FString LowerToken = NormalizeTruthText(Token);
			if (LowerToken.Contains(TEXT(".log"), ESearchCase::CaseSensitive)
				|| LowerToken.Contains(TEXT("saved/osvayderue"), ESearchCase::CaseSensitive)
				|| LowerToken.Contains(TEXT("saved/logs"), ESearchCase::CaseSensitive)
				|| LowerToken.Contains(TEXT("intermediate/build"), ESearchCase::CaseSensitive))
			{
				Paths.AddUnique(Token);
			}
		}
		return Paths;
	}

	TArray<TSharedPtr<FJsonValue>> MakeExecutionTruthStringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}
}

FString StripKnownShellWrapperPrefixes(const FString& CommandText)
{
	FString Working = CommandText.TrimStartAndEnd();
	// Strip common PowerShell / cmd wrappers so the inner command leads.
	// Sequence is tolerant of repeated wrappers and of quoted bodies.
	const TArray<FString> WrapperPrefixes = {
		TEXT("powershell.exe -NoProfile -Command "),
		TEXT("powershell.exe -NoProfile -NonInteractive -Command "),
		TEXT("powershell.exe -Command "),
		TEXT("powershell -NoProfile -Command "),
		TEXT("powershell -Command "),
		TEXT("pwsh.exe -Command "),
		TEXT("pwsh -Command "),
		TEXT("cmd.exe /c "),
		TEXT("cmd /c "),
		TEXT("bash -c "),
		TEXT("sh -c ")
	};
	bool bStrippedAny = true;
	while (bStrippedAny)
	{
		bStrippedAny = false;
		for (const FString& Prefix : WrapperPrefixes)
		{
			if (Working.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				Working = Working.RightChop(Prefix.Len()).TrimStart();
				bStrippedAny = true;
			}
		}
		// Strip surrounding quotes left over from wrapper unwrap.
		while (Working.Len() >= 2
			&& (Working.StartsWith(TEXT("\""), ESearchCase::CaseSensitive)
				|| Working.StartsWith(TEXT("'"), ESearchCase::CaseSensitive)))
		{
			const TCHAR Quote = Working[0];
			const int32 CloseIndex = Working.Find(FString::Chr(Quote), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
			if (CloseIndex == INDEX_NONE)
			{
				Working = Working.RightChop(1);
				break;
			}
			Working = Working.Mid(1, CloseIndex - 1) + Working.Mid(CloseIndex + 1);
			bStrippedAny = true;
		}
	}
	return Working.TrimStartAndEnd();
}

bool IsKnownInspectionCommand(const FString& CommandText)
{
	if (CommandText.IsEmpty())
	{
		return false;
	}
	const FString Inner = StripKnownShellWrapperPrefixes(CommandText);
	if (Inner.IsEmpty())
	{
		return false;
	}
	// First whitespace-delimited token (handles `Get-Content foo` or `rg pat`).
	FString FirstToken = Inner;
	int32 FirstSpace = INDEX_NONE;
	if (Inner.FindChar(TEXT(' '), FirstSpace) && FirstSpace > 0)
	{
		FirstToken = Inner.Left(FirstSpace);
	}
	// Strip any `.exe` suffix for comparison friendliness.
	if (FirstToken.EndsWith(TEXT(".exe"), ESearchCase::IgnoreCase))
	{
		FirstToken = FirstToken.LeftChop(4);
	}

	// Canonical PowerShell inspection cmdlets + Unix shell / ripgrep / grep / findstr.
	static const TArray<FString> Inspection = {
		TEXT("Get-Content"), TEXT("gc"), TEXT("cat"),
		TEXT("Get-Item"), TEXT("gi"),
		TEXT("Get-ChildItem"), TEXT("gci"), TEXT("dir"), TEXT("ls"),
		TEXT("Test-Path"),
		TEXT("Select-String"), TEXT("sls"),
		TEXT("rg"), TEXT("ripgrep"), TEXT("grep"), TEXT("findstr"),
		TEXT("head"), TEXT("tail"), TEXT("more"), TEXT("less"), TEXT("type"),
		TEXT("wc"), TEXT("file"), TEXT("stat"),
		TEXT("which"), TEXT("where"), TEXT("where.exe")
	};
	for (const FString& Known : Inspection)
	{
		if (FirstToken.Equals(Known, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	// git sub-commands that are pure inspection (status / diff / log / show / branch etc.).
	if (FirstToken.Equals(TEXT("git"), ESearchCase::IgnoreCase))
	{
		FString Rest = Inner.Mid(FirstSpace + 1).TrimStart();
		int32 SubSpace = INDEX_NONE;
		FString SubToken = Rest;
		if (Rest.FindChar(TEXT(' '), SubSpace) && SubSpace > 0)
		{
			SubToken = Rest.Left(SubSpace);
		}
		static const TArray<FString> GitInspect = {
			TEXT("status"), TEXT("diff"), TEXT("log"), TEXT("show"),
			TEXT("branch"), TEXT("remote"), TEXT("ls-files"), TEXT("ls-tree"),
			TEXT("rev-parse"), TEXT("reflog"), TEXT("blame"), TEXT("config"),
			TEXT("describe"), TEXT("tag")
		};
		for (const FString& G : GitInspect)
		{
			if (SubToken.Equals(G, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
	}
	return false;
}

bool IsBuildContextCommand(const FString& CommandText)
{
	if (CommandText.IsEmpty())
	{
		return false;
	}
	const FString Lower = CommandText.ToLower();
	// Markers that unambiguously invoke a real build/compile path.
	static const TArray<FString> BuildInvocations = {
		TEXT("build.bat"),
		TEXT("unrealbuildtool"),
		TEXT("ubt.exe"),
		TEXT("runuat"),
		TEXT("cl.exe"),
		TEXT("link.exe"),
		TEXT("msbuild"),
		TEXT("dotnet build"),
		TEXT("unrealheadertool"),
		TEXT("uht.exe")
	};
	for (const FString& Needle : BuildInvocations)
	{
		if (Lower.Contains(Needle))
		{
			return true;
		}
	}
	return false;
}

bool HasStructuredBuildOutputSignature(const FString& LowerHaystack)
{
	if (LowerHaystack.IsEmpty())
	{
		return false;
	}
	// UBT / UHT / Live Coding / compiler-output characteristic markers.
	// These are strict enough that ordinary prose or file listings won't
	// all carry them; typical UBT invocation output emits one or more.
	static const TArray<FString> Signatures = {
		TEXT("running unrealbuildtool"),
		TEXT("unrealheadertool"),
		TEXT("[loglivecoding]"),
		TEXT("loglivecoding:"),
		TEXT("loglivecodingcompile"),
		TEXT("link :"),
		TEXT("[adaptive build]"),
		TEXT("building gdr_shooter"),
		TEXT("building poligon1"),
		TEXT("epicgames.core.log"),
		TEXT("epicgames.ubt"),
		TEXT("intermediate/build/win64"),
		TEXT("intermediate\\build\\win64"),
		TEXT("unable to build while live coding"),
		TEXT("unauthorizedaccessexception"),
		TEXT("lnk1104"),
		TEXT("failed to rename exported file")
	};
	for (const FString& Sig : Signatures)
	{
		if (LowerHaystack.Contains(Sig))
		{
			return true;
		}
	}
	return false;
}

FString ExecutionTruthCategoryToString(const EOsvayderUEExecutionTruthCategory Category)
{
	switch (Category)
	{
	case EOsvayderUEExecutionTruthCategory::ReadOnlyInspection:
		return TEXT("read_only_inspection");
	case EOsvayderUEExecutionTruthCategory::ApprovedBuildOrTestExecution:
		return TEXT("approved_build_or_test_execution");
	case EOsvayderUEExecutionTruthCategory::ApprovedProjectMutation:
		return TEXT("approved_project_mutation");
	case EOsvayderUEExecutionTruthCategory::ManagedStateWrite:
		return TEXT("managed_state_write");
	case EOsvayderUEExecutionTruthCategory::StaleOrOutOfRun:
		return TEXT("stale_or_out_of_run");
	case EOsvayderUEExecutionTruthCategory::UnsafeOrUnknown:
	default:
		return TEXT("unsafe_or_unknown");
	}
}

bool IsCommandExecutionLikeToolName(const FString& ToolName)
{
	const FString LowerToolName = ToolName.ToLower();
	return LowerToolName.Equals(TEXT("command_execution"), ESearchCase::CaseSensitive)
		|| LowerToolName.EndsWith(TEXT("/command_execution"), ESearchCase::CaseSensitive)
		|| LowerToolName.Contains(TEXT("command_execution"), ESearchCase::CaseSensitive)
		|| LowerToolName.Equals(TEXT("bash"), ESearchCase::CaseSensitive)
		|| LowerToolName.EndsWith(TEXT("/bash"), ESearchCase::CaseSensitive)
		|| LowerToolName.Equals(TEXT("execute_terminal"), ESearchCase::CaseSensitive)
		|| LowerToolName.EndsWith(TEXT("/execute_terminal"), ESearchCase::CaseSensitive);
}

bool IsManagedOsvayderUEStatePathMentioned(const FString& Text)
{
	const FString Normalized = NormalizeTruthText(Text);
	return Normalized.Contains(TEXT("saved/osvayderue/active_plan.json"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("saved/osvayderue/planarchives"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("saved/osvayderue/closeoutdecisions"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("saved/osvayderue/closeout_decision.json"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("saved/osvayderue/visible_session"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("saved/osvayderue/agent_trace"), ESearchCase::CaseSensitive);
}

FString FOsvayderUEExecutionTruthDecision::ToSummaryString() const
{
	return FString::Printf(
		TEXT("category=%s reason=%s run_id=%s plan_id=%s feature_workflow_id=%s tool=%s family=%s exit_code=%d typed_current_run_evidence=%s managed_state_write=%s artifacts=%s"),
		*ExecutionTruthCategoryToString(Category),
		*ReasonCode,
		*RunId,
		*PlanId,
		*FeatureWorkflowId,
		*ToolName,
		*ToolFamily,
		ExitCode,
		bTypedCurrentRunEvidence ? TEXT("true") : TEXT("false"),
		bManagedStateWrite ? TEXT("true") : TEXT("false"),
		*FString::Join(ArtifactPaths, TEXT("|")));
}

TSharedPtr<FJsonObject> FOsvayderUEExecutionTruthDecision::ToJsonObject() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("category"), ExecutionTruthCategoryToString(Category));
	Object->SetStringField(TEXT("reason_code"), ReasonCode);
	Object->SetStringField(TEXT("run_id"), RunId);
	Object->SetStringField(TEXT("expected_run_id"), ExpectedRunId);
	Object->SetStringField(TEXT("plan_id"), PlanId);
	Object->SetStringField(TEXT("feature_workflow_id"), FeatureWorkflowId);
	Object->SetStringField(TEXT("project_root"), ProjectRoot);
	Object->SetStringField(TEXT("cwd"), Cwd);
	Object->SetStringField(TEXT("tool_name"), ToolName);
	Object->SetStringField(TEXT("command_input"), CommandInput);
	Object->SetStringField(TEXT("tool_family"), ToolFamily);
	Object->SetNumberField(TEXT("exit_code"), ExitCode);
	Object->SetBoolField(TEXT("current_run"), bCurrentRun);
	Object->SetBoolField(TEXT("is_error"), bIsError);
	Object->SetBoolField(TEXT("managed_state_touched"), bManagedStateTouched);
	Object->SetBoolField(TEXT("managed_state_write"), bManagedStateWrite);
	Object->SetBoolField(TEXT("typed_log_markers_present"), bTypedLogMarkersPresent);
	Object->SetBoolField(TEXT("exit_code_present"), bExitCodePresent);
	Object->SetBoolField(TEXT("exit_code_success"), bExitCodeSuccess);
	Object->SetBoolField(TEXT("typed_current_run_evidence"), bTypedCurrentRunEvidence);
	Object->SetArrayField(TEXT("artifact_paths"), MakeExecutionTruthStringArrayJson(ArtifactPaths));
	return Object;
}

FOsvayderUEExecutionTruthDecision ClassifyExecutionTruth(
	const FOsvayderUEExecutionTruthInputs& Inputs)
{
	FOsvayderUEExecutionTruthDecision Decision;
	Decision.RunId = Inputs.RunId.TrimStartAndEnd();
	Decision.ExpectedRunId = Inputs.ExpectedRunId.TrimStartAndEnd();
	Decision.PlanId = Inputs.PlanId.TrimStartAndEnd();
	Decision.FeatureWorkflowId = Inputs.FeatureWorkflowId.TrimStartAndEnd();
	Decision.ProjectRoot = Inputs.ProjectRoot.TrimStartAndEnd();
	Decision.Cwd = Inputs.Cwd.TrimStartAndEnd();
	Decision.ToolName = Inputs.ToolName.TrimStartAndEnd();
	Decision.CommandInput = Inputs.CommandInput.TrimStartAndEnd();
	Decision.ToolFamily = Inputs.ToolFamily.TrimStartAndEnd();
	Decision.ExitCode = Inputs.ExitCode;
	Decision.bIsError = Inputs.bIsError;

	const bool bExpectedRunKnown = !Decision.ExpectedRunId.IsEmpty();
	Decision.bCurrentRun =
		!bExpectedRunKnown
		|| Decision.RunId.IsEmpty()
		|| Decision.RunId.Equals(Decision.ExpectedRunId, ESearchCase::CaseSensitive);

	const FString CommandEnvelope =
		FString::Join(BuildCommandAnalysisTexts(Decision.CommandInput, Inputs.RawJson), TEXT("\n"));
	const FString FullEnvelope =
		Decision.CommandInput + TEXT("\n") + Inputs.RawJson + TEXT("\n") + Inputs.ToolResult;
	const FString LowerCommand = NormalizeTruthText(CommandEnvelope);
	const FString LowerEvidence = NormalizeTruthText(FullEnvelope);
	const bool bCommandLike = IsCommandExecutionLikeToolName(Decision.ToolName);
	const bool bManagedStateTouched = IsManagedOsvayderUEStatePathMentioned(CommandEnvelope);
	const bool bManagedStateWrite = bCommandLike && bManagedStateTouched && HasShellMutationVerb(CommandEnvelope);
	const bool bAutomationExecution = bCommandLike && HasAutomationTestInvocation(LowerCommand);
	const bool bBuildExecution = bCommandLike && IsBuildContextCommand(CommandEnvelope);
	const bool bBuildOrTestExecution = bAutomationExecution || bBuildExecution;
	const bool bKnownInspectionCommand =
		bCommandLike
		&& (IsKnownInspectionCommand(CommandEnvelope) || HasShellInspectionVerb(CommandEnvelope));

	Decision.bManagedStateTouched = bManagedStateTouched;
	Decision.bManagedStateWrite = bManagedStateWrite;
	Decision.bExitCodePresent = Inputs.ExitCode != INDEX_NONE || HasExitZeroMarker(LowerEvidence);
	Decision.bExitCodeSuccess = Inputs.ExitCode == 0 || HasExitZeroMarker(LowerEvidence);
	Decision.bTypedLogMarkersPresent =
		(bAutomationExecution && HasAutomationTypedMarkers(LowerEvidence))
		|| (bBuildExecution && HasStructuredBuildOutputSignature(LowerEvidence));
	Decision.bTypedCurrentRunEvidence =
		Decision.bCurrentRun
		&& !Decision.bIsError
		&& bBuildOrTestExecution
		&& Decision.bExitCodeSuccess
		&& Decision.bTypedLogMarkersPresent;
	Decision.ArtifactPaths = ExtractExecutionTruthArtifactPaths(FullEnvelope);

	if (!Decision.bCurrentRun)
	{
		Decision.Category = EOsvayderUEExecutionTruthCategory::StaleOrOutOfRun;
		Decision.ReasonCode = TEXT("run_id_mismatch");
		return Decision;
	}

	if (bManagedStateWrite)
	{
		Decision.Category = EOsvayderUEExecutionTruthCategory::ManagedStateWrite;
		Decision.ReasonCode = TEXT("command_mutates_managed_osvayderue_state");
		return Decision;
	}

	if (bBuildOrTestExecution)
	{
		Decision.Category = EOsvayderUEExecutionTruthCategory::ApprovedBuildOrTestExecution;
		Decision.ReasonCode = Decision.bTypedCurrentRunEvidence
			? TEXT("typed_current_run_build_or_test_evidence")
			: TEXT("approved_build_or_test_command_shape_pending_typed_result");
		return Decision;
	}

	if (bKnownInspectionCommand)
	{
		if (IsOverbroadInspectionCommand(CommandEnvelope))
		{
			Decision.Category = EOsvayderUEExecutionTruthCategory::UnsafeOrUnknown;
			Decision.ReasonCode = TEXT("overbroad_root_inspection_command");
			return Decision;
		}

		Decision.Category = EOsvayderUEExecutionTruthCategory::ReadOnlyInspection;
		Decision.ReasonCode = bManagedStateTouched
			? TEXT("read_only_managed_state_inspection")
			: TEXT("known_read_only_command");
		return Decision;
	}

	if (Inputs.bClassifiedMutatingTool
		|| Inputs.bPrimaryMutationAssigned
		|| Decision.ToolFamily.Equals(TEXT("workspace_file_build"), ESearchCase::IgnoreCase))
	{
		Decision.Category = EOsvayderUEExecutionTruthCategory::ApprovedProjectMutation;
		Decision.ReasonCode = TEXT("canon_routing_classified_project_mutation");
		return Decision;
	}

	Decision.Category = EOsvayderUEExecutionTruthCategory::UnsafeOrUnknown;
	Decision.ReasonCode = bCommandLike
		? TEXT("command_shape_not_approved_for_execution_truth")
		: TEXT("non_command_tool_without_execution_truth_rule");
	return Decision;
}

} // namespace CommandClassification
} // namespace OsvayderUE
