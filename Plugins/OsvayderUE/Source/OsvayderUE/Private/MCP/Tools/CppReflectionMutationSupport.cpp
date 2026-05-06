// Copyright Natali Caggiano. All Rights Reserved.

#include "CppReflectionMutationSupport.h"

#include "OsvayderUEScopePolicy.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace
{
	struct FHeaderMutationSite
	{
		bool bFound = false;
		FString NewlineStyle;
		bool bEndsWithNewline = false;
		TArray<FString> Lines;
		int32 MacroStartLine = INDEX_NONE;
		int32 MacroEndLine = INDEX_NONE;
		int32 DeclarationLine = INDEX_NONE;
		FString DeclarationText;
		FString MacroText;
		FString OriginalFileContent;
	};

	const FString& GetPropertyDeclarationPreviewSchemaVersion()
	{
		static const FString SchemaVersion = TEXT("reflected_property_declaration_preview_v1");
		return SchemaVersion;
	}

	const FString& GetPropertyDeclarationApplySchemaVersion()
	{
		static const FString SchemaVersion = TEXT("reflected_property_declaration_apply_v1");
		return SchemaVersion;
	}

	const FString& GetPropertyDeclarationRevertSchemaVersion()
	{
		static const FString SchemaVersion = TEXT("reflected_property_declaration_revert_v1");
		return SchemaVersion;
	}

	const TMap<FString, FString>& GetAllowedMetadataKeys()
	{
		static const TMap<FString, FString> Allowed = {
			{ TEXT("displayname"), TEXT("DisplayName") },
			{ TEXT("tooltip"), TEXT("ToolTip") },
			{ TEXT("clampmin"), TEXT("ClampMin") },
			{ TEXT("clampmax"), TEXT("ClampMax") },
			{ TEXT("uimin"), TEXT("UIMin") },
			{ TEXT("uimax"), TEXT("UIMax") },
			{ TEXT("multiline"), TEXT("MultiLine") },
		};
		return Allowed;
	}

	bool IsWordBoundary(const FString& Text, const int32 Index)
	{
		return Index < 0
			|| Index >= Text.Len()
			|| !(FChar::IsAlnum(Text[Index]) || Text[Index] == TEXT('_'));
	}

	bool ContainsWholeWord(const FString& Text, const FString& Word)
	{
		if (Text.IsEmpty() || Word.IsEmpty())
		{
			return false;
		}

		int32 SearchFrom = 0;
		while (SearchFrom < Text.Len())
		{
			const int32 FoundAt = Text.Find(Word, ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchFrom);
			if (FoundAt == INDEX_NONE)
			{
				return false;
			}

			const int32 WordEnd = FoundAt + Word.Len();
			if (IsWordBoundary(Text, FoundAt - 1) && IsWordBoundary(Text, WordEnd))
			{
				return true;
			}

			SearchFrom = WordEnd;
		}

		return false;
	}

	FString DetectNewlineStyle(const FString& Text)
	{
		return Text.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");
	}

	FString ComputeContentHash(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	}

	FString JoinLines(const TArray<FString>& Lines, const FString& NewlineStyle, const bool bEndsWithNewline)
	{
		FString Result = FString::Join(Lines, *NewlineStyle);
		if (bEndsWithNewline)
		{
			Result += NewlineStyle;
		}
		return Result;
	}

	TArray<FString> SplitTopLevelCommaSeparated(const FString& Text)
	{
		TArray<FString> Parts;
		FString Current;
		int32 ParenDepth = 0;
		bool bInQuotes = false;
		bool bEscape = false;

		for (const TCHAR Ch : Text)
		{
			if (bEscape)
			{
				Current.AppendChar(Ch);
				bEscape = false;
				continue;
			}

			if (bInQuotes && Ch == TEXT('\\'))
			{
				Current.AppendChar(Ch);
				bEscape = true;
				continue;
			}

			if (Ch == TEXT('"'))
			{
				bInQuotes = !bInQuotes;
				Current.AppendChar(Ch);
				continue;
			}

			if (!bInQuotes)
			{
				if (Ch == TEXT('('))
				{
					ParenDepth++;
					Current.AppendChar(Ch);
					continue;
				}

				if (Ch == TEXT(')'))
				{
					ParenDepth = FMath::Max(0, ParenDepth - 1);
					Current.AppendChar(Ch);
					continue;
				}

				if (Ch == TEXT(',') && ParenDepth == 0)
				{
					Parts.Add(Current.TrimStartAndEnd());
					Current.Empty();
					continue;
				}
			}

			Current.AppendChar(Ch);
		}

		if (!Current.IsEmpty() || Text.EndsWith(TEXT(",")))
		{
			Parts.Add(Current.TrimStartAndEnd());
		}

		Parts.RemoveAll([](const FString& Part)
		{
			return Part.IsEmpty();
		});
		return Parts;
	}

	int32 FindTopLevelEquals(const FString& Text)
	{
		int32 ParenDepth = 0;
		bool bInQuotes = false;
		bool bEscape = false;

		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Ch = Text[Index];
			if (bEscape)
			{
				bEscape = false;
				continue;
			}

			if (bInQuotes && Ch == TEXT('\\'))
			{
				bEscape = true;
				continue;
			}

			if (Ch == TEXT('"'))
			{
				bInQuotes = !bInQuotes;
				continue;
			}

			if (bInQuotes)
			{
				continue;
			}

			if (Ch == TEXT('('))
			{
				ParenDepth++;
				continue;
			}

			if (Ch == TEXT(')'))
			{
				ParenDepth = FMath::Max(0, ParenDepth - 1);
				continue;
			}

			if (Ch == TEXT('=') && ParenDepth == 0)
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	bool SplitTopLevelAssignment(const FString& Text, FString& OutLeft, FString& OutRight)
	{
		const int32 EqualsIndex = FindTopLevelEquals(Text);
		if (EqualsIndex == INDEX_NONE)
		{
			return false;
		}

		OutLeft = Text.Left(EqualsIndex).TrimStartAndEnd();
		OutRight = Text.Mid(EqualsIndex + 1).TrimStartAndEnd();
		return true;
	}

	FString EscapeMetadataLiteral(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Escaped.ReplaceInline(TEXT("\r\n"), TEXT("\\n"));
		Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Escaped.ReplaceInline(TEXT("\r"), TEXT("\\n"));
		return Escaped;
	}

	FString QuoteMetadataLiteral(const FString& Value)
	{
		return FString::Printf(TEXT("\"%s\""), *EscapeMetadataLiteral(Value));
	}

	FString GetLineIndentation(const FString& Line)
	{
		int32 Index = 0;
		while (Index < Line.Len() && (Line[Index] == TEXT(' ') || Line[Index] == TEXT('\t')))
		{
			++Index;
		}

		return Line.Left(Index);
	}

	bool IsValidCppIdentifier(const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return false;
		}

		const TCHAR First = Value[0];
		if (!(FChar::IsAlpha(First) || First == TEXT('_')))
		{
			return false;
		}

		for (int32 Index = 1; Index < Value.Len(); ++Index)
		{
			const TCHAR Ch = Value[Index];
			if (!(FChar::IsAlnum(Ch) || Ch == TEXT('_')))
			{
				return false;
			}
		}

		return true;
	}

	bool TryNormalizePreviewPropertyType(
		const FString& RequestedType,
		FString& OutCanonicalType,
		FString& OutError)
	{
		const FString Normalized = RequestedType.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("bool"))
		{
			OutCanonicalType = TEXT("bool");
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported property_cpp_type '%s'. This preview-only declaration foundation currently supports only bool."),
			*RequestedType);
		return false;
	}

	bool TryNormalizePreviewDefaultValue(
		const FString& RequestedDefaultValue,
		const FString& PropertyCppType,
		FString& OutCanonicalDefaultValue,
		FString& OutError)
	{
		if (!PropertyCppType.Equals(TEXT("bool"), ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(
				TEXT("Unsupported normalized property_cpp_type '%s' in preview default value normalization."),
				*PropertyCppType);
			return false;
		}

		const FString Normalized = RequestedDefaultValue.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("false"))
		{
			OutCanonicalDefaultValue = TEXT("false");
			return true;
		}

		if (Normalized == TEXT("true"))
		{
			OutCanonicalDefaultValue = TEXT("true");
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported default_value '%s' for bool preview. Accepted values are true or false."),
			*RequestedDefaultValue);
		return false;
	}

	FString BuildPreviewExcerpt(
		const TArray<FString>& Lines,
		const FString& NewlineStyle,
		const int32 StartLine,
		const int32 EndLine)
	{
		TArray<FString> ExcerptLines;
		for (int32 LineIndex = StartLine; LineIndex <= EndLine && Lines.IsValidIndex(LineIndex); ++LineIndex)
		{
			ExcerptLines.Add(Lines[LineIndex]);
		}

		return JoinLines(ExcerptLines, NewlineStyle, false);
	}

	bool TryFindHeaderMutationSite(
		const FString& HeaderPath,
		const FString& PropertyName,
		FHeaderMutationSite& OutSite,
		FString& OutError)
	{
		FString FileContent;
		if (!FFileHelper::LoadFileToString(FileContent, *HeaderPath))
		{
			OutError = FString::Printf(TEXT("Could not load header '%s'."), *HeaderPath);
			return false;
		}

		OutSite.OriginalFileContent = FileContent;
		OutSite.NewlineStyle = DetectNewlineStyle(FileContent);
		OutSite.bEndsWithNewline =
			FileContent.EndsWith(TEXT("\r\n")) || FileContent.EndsWith(TEXT("\n")) || FileContent.EndsWith(TEXT("\r"));
		FileContent.ParseIntoArrayLines(OutSite.Lines, false);

		TArray<int32> CandidateDeclarationLines;
		for (int32 LineIndex = 0; LineIndex < OutSite.Lines.Num(); ++LineIndex)
		{
			const FString& Line = OutSite.Lines[LineIndex];
			const FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("//")) || Trimmed.StartsWith(TEXT("*")))
			{
				continue;
			}

			if (Line.Contains(TEXT("UPROPERTY")))
			{
				continue;
			}

			if (ContainsWholeWord(Line, PropertyName) && Line.Contains(TEXT(";")))
			{
				CandidateDeclarationLines.Add(LineIndex);
			}
		}

		if (CandidateDeclarationLines.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("Could not find a single-line declaration for property '%s' in header '%s'."),
				*PropertyName,
				*HeaderPath);
			return false;
		}

		TArray<int32> MatchingDeclarationLines;
		TMap<int32, int32> MacroStartByDeclaration;
		for (const int32 CandidateLine : CandidateDeclarationLines)
		{
			int32 MacroStart = INDEX_NONE;
			for (int32 ScanLine = CandidateLine - 1; ScanLine >= 0; --ScanLine)
			{
				const FString Trimmed = OutSite.Lines[ScanLine].TrimStartAndEnd();
				if (Trimmed.Contains(TEXT("UPROPERTY")))
				{
					MacroStart = ScanLine;
					break;
				}

				if (Trimmed.EndsWith(TEXT(";"))
					|| Trimmed.EndsWith(TEXT("{"))
					|| Trimmed.StartsWith(TEXT("public:"))
					|| Trimmed.StartsWith(TEXT("protected:"))
					|| Trimmed.StartsWith(TEXT("private:"))
					|| Trimmed.StartsWith(TEXT("UFUNCTION"))
					|| Trimmed.StartsWith(TEXT("UCLASS"))
					|| Trimmed.StartsWith(TEXT("USTRUCT"))
					|| Trimmed.StartsWith(TEXT("GENERATED_BODY")))
				{
					break;
				}
			}

			if (MacroStart != INDEX_NONE)
			{
				MatchingDeclarationLines.Add(CandidateLine);
				MacroStartByDeclaration.Add(CandidateLine, MacroStart);
			}
		}

		if (MatchingDeclarationLines.Num() != 1)
		{
			OutError = FString::Printf(
				TEXT("Property '%s' in header '%s' resolved to %d candidate declaration sites. This mutation lane requires one unambiguous plugin-owned declaration."),
				*PropertyName,
				*HeaderPath,
				MatchingDeclarationLines.Num());
			return false;
		}

		OutSite.DeclarationLine = MatchingDeclarationLines[0];
		OutSite.MacroStartLine = MacroStartByDeclaration[OutSite.DeclarationLine];
		OutSite.DeclarationText = OutSite.Lines[OutSite.DeclarationLine].TrimStartAndEnd();

		bool bStartedParen = false;
		int32 ParenDepth = 0;
		for (int32 LineIndex = OutSite.MacroStartLine; LineIndex < OutSite.Lines.Num(); ++LineIndex)
		{
			const FString& Line = OutSite.Lines[LineIndex];
			for (const TCHAR Ch : Line)
			{
				if (Ch == TEXT('('))
				{
					ParenDepth++;
					bStartedParen = true;
				}
				else if (Ch == TEXT(')') && bStartedParen)
				{
					ParenDepth--;
					if (ParenDepth == 0)
					{
						OutSite.MacroEndLine = LineIndex;
						break;
					}
				}
			}

			if (OutSite.MacroEndLine != INDEX_NONE)
			{
				break;
			}
		}

		if (OutSite.MacroEndLine == INDEX_NONE)
		{
			OutError = FString::Printf(
				TEXT("Could not determine UPROPERTY macro bounds for property '%s' in header '%s'."),
				*PropertyName,
				*HeaderPath);
			return false;
		}

		TArray<FString> MacroLines;
		for (int32 LineIndex = OutSite.MacroStartLine; LineIndex <= OutSite.MacroEndLine; ++LineIndex)
		{
			MacroLines.Add(OutSite.Lines[LineIndex]);
		}
		OutSite.MacroText = JoinLines(MacroLines, OutSite.NewlineStyle, false);
		OutSite.bFound = true;
		return true;
	}

	bool TryMutateMacroText(
		const FString& MacroText,
		const FString& MetadataKey,
		const FString& MetadataValue,
		FString& OutMacroAfter,
		FString& OutChangeKind,
		FString& OutExistingMetadataValue,
		bool& bOutHadMetaBlock,
		bool& bOutHadExistingKey,
		FString& OutError)
	{
		const int32 PropertyIndex = MacroText.Find(TEXT("UPROPERTY"), ESearchCase::IgnoreCase);
		if (PropertyIndex == INDEX_NONE)
		{
			OutError = TEXT("Macro text does not contain UPROPERTY.");
			return false;
		}

		const int32 OpenParenIndex = MacroText.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, PropertyIndex);
		const int32 CloseParenIndex = MacroText.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (OpenParenIndex == INDEX_NONE || CloseParenIndex == INDEX_NONE || CloseParenIndex <= OpenParenIndex)
		{
			OutError = TEXT("Could not parse UPROPERTY argument list.");
			return false;
		}

		const FString Prefix = MacroText.Left(OpenParenIndex + 1);
		const FString Suffix = MacroText.Mid(CloseParenIndex);
		const FString Args = MacroText.Mid(OpenParenIndex + 1, CloseParenIndex - OpenParenIndex - 1);
		TArray<FString> TopLevelArgs = SplitTopLevelCommaSeparated(Args);
		const FString QuotedValue = QuoteMetadataLiteral(MetadataValue);

		int32 MetaItemIndex = INDEX_NONE;
		FString MetaInnerArgs;
		for (int32 Index = 0; Index < TopLevelArgs.Num(); ++Index)
		{
			FString Left;
			FString Right;
			if (!SplitTopLevelAssignment(TopLevelArgs[Index], Left, Right))
			{
				continue;
			}

			if (Left.TrimStartAndEnd().Equals(TEXT("meta"), ESearchCase::IgnoreCase))
			{
				const int32 MetaOpen = Right.Find(TEXT("("));
				const int32 MetaClose = Right.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				if (MetaOpen == INDEX_NONE || MetaClose == INDEX_NONE || MetaClose <= MetaOpen)
				{
					OutError = TEXT("Existing meta=(...) block could not be parsed.");
					return false;
				}

				MetaItemIndex = Index;
				MetaInnerArgs = Right.Mid(MetaOpen + 1, MetaClose - MetaOpen - 1);
				break;
			}
		}

		bOutHadMetaBlock = MetaItemIndex != INDEX_NONE;
		if (MetaItemIndex != INDEX_NONE)
		{
			TArray<FString> MetaItems = SplitTopLevelCommaSeparated(MetaInnerArgs);
			int32 ExistingKeyIndex = INDEX_NONE;
			for (int32 Index = 0; Index < MetaItems.Num(); ++Index)
			{
				FString Left;
				FString Right;
				if (!SplitTopLevelAssignment(MetaItems[Index], Left, Right))
				{
					continue;
				}

				if (Left.TrimStartAndEnd().Equals(MetadataKey, ESearchCase::IgnoreCase))
				{
					ExistingKeyIndex = Index;
					OutExistingMetadataValue = Right.TrimStartAndEnd();
					break;
				}
			}

			bOutHadExistingKey = ExistingKeyIndex != INDEX_NONE;
			if (ExistingKeyIndex != INDEX_NONE)
			{
				if (OutExistingMetadataValue.Equals(QuotedValue, ESearchCase::CaseSensitive))
				{
					OutChangeKind = TEXT("noop_same_value");
					OutMacroAfter = MacroText;
					return true;
				}

				MetaItems[ExistingKeyIndex] = FString::Printf(TEXT("%s = %s"), *MetadataKey, *QuotedValue);
				OutChangeKind = TEXT("update_metadata_key");
			}
			else
			{
				MetaItems.Add(FString::Printf(TEXT("%s = %s"), *MetadataKey, *QuotedValue));
				OutChangeKind = TEXT("insert_metadata_key");
			}

			TopLevelArgs[MetaItemIndex] = FString::Printf(TEXT("meta = (%s)"), *FString::Join(MetaItems, TEXT(", ")));
		}
		else
		{
			bOutHadExistingKey = false;
			TopLevelArgs.Add(FString::Printf(TEXT("meta = (%s = %s)"), *MetadataKey, *QuotedValue));
			OutChangeKind = TEXT("create_meta_block_and_insert_key");
		}

		OutMacroAfter = Prefix + FString::Join(TopLevelArgs, TEXT(", ")) + Suffix;
		return true;
	}

	TSharedPtr<FJsonObject> BuildScopeObject(const FCppReflectionPropertyMetadataMutationResult& Result)
	{
		TSharedPtr<FJsonObject> ScopeObject = MakeShared<FJsonObject>();
		ScopeObject->SetBoolField(TEXT("allowed"), Result.bScopeAllowed);
		ScopeObject->SetStringField(TEXT("matched_root"), Result.ScopeMatchedRoot);
		ScopeObject->SetStringField(TEXT("denial_reason"), Result.ScopeDenialReason);
		return ScopeObject;
	}

	TSharedPtr<FJsonObject> BuildScopeObject(const FCppReflectionPropertyDeclarationPreviewResult& Result)
	{
		TSharedPtr<FJsonObject> ScopeObject = MakeShared<FJsonObject>();
		ScopeObject->SetBoolField(TEXT("allowed"), Result.bScopeAllowed);
		ScopeObject->SetStringField(TEXT("matched_root"), Result.ScopeMatchedRoot);
		ScopeObject->SetStringField(TEXT("denial_reason"), Result.ScopeDenialReason);
		return ScopeObject;
	}

	TSharedPtr<FJsonObject> BuildScopeObject(const FCppReflectionPropertyDeclarationApplyResult& Result)
	{
		TSharedPtr<FJsonObject> ScopeObject = MakeShared<FJsonObject>();
		ScopeObject->SetBoolField(TEXT("allowed"), Result.bScopeAllowed);
		ScopeObject->SetStringField(TEXT("matched_root"), Result.ScopeMatchedRoot);
		ScopeObject->SetStringField(TEXT("denial_reason"), Result.ScopeDenialReason);
		return ScopeObject;
	}

	TSharedPtr<FJsonObject> BuildScopeObject(const FCppReflectionPropertyDeclarationRevertResult& Result)
	{
		TSharedPtr<FJsonObject> ScopeObject = MakeShared<FJsonObject>();
		ScopeObject->SetBoolField(TEXT("allowed"), Result.bScopeAllowed);
		ScopeObject->SetStringField(TEXT("matched_root"), Result.ScopeMatchedRoot);
		ScopeObject->SetStringField(TEXT("denial_reason"), Result.ScopeDenialReason);
		return ScopeObject;
	}
}

TSharedPtr<FJsonObject> FCppReflectionPropertyMetadataMutationResult::ToJson() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("success"), bSuccess);
	Object->SetBoolField(TEXT("applied"), bApplied);
	Object->SetBoolField(TEXT("would_change_file"), bWouldChangeFile);
	Object->SetStringField(TEXT("change_kind"), ChangeKind);
	Object->SetStringField(TEXT("header_path"), HeaderPath);
	Object->SetNumberField(TEXT("macro_start_line"), MacroStartLine);
	Object->SetNumberField(TEXT("macro_end_line"), MacroEndLine);
	Object->SetNumberField(TEXT("declaration_line"), DeclarationLine);
	Object->SetStringField(TEXT("declaration_text"), DeclarationText);
	Object->SetStringField(TEXT("metadata_key"), NormalizedMetadataKey);
	Object->SetStringField(TEXT("metadata_value"), MetadataValue);
	Object->SetStringField(TEXT("existing_metadata_value"), ExistingMetadataValue);
	Object->SetBoolField(TEXT("had_meta_block"), bHadMetaBlock);
	Object->SetBoolField(TEXT("had_existing_key"), bHadExistingKey);
	Object->SetStringField(TEXT("macro_before"), MacroBefore);
	Object->SetStringField(TEXT("macro_after"), MacroAfter);
	Object->SetStringField(TEXT("source_hash_before"), SourceHashBefore);
	Object->SetStringField(TEXT("source_hash_after"), SourceHashAfter);
	Object->SetObjectField(TEXT("scope"), BuildScopeObject(*this));
	return Object;
}

TSharedPtr<FJsonObject> FCppReflectionPropertyDeclarationPreviewResult::ToJson() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("success"), bSuccess);
	Object->SetBoolField(TEXT("would_change_file"), bWouldChangeFile);
	Object->SetBoolField(TEXT("preview_only"), true);
	Object->SetStringField(TEXT("schema_version"), SchemaVersion);
	Object->SetStringField(TEXT("declaration_kind"), DeclarationKind);
	Object->SetStringField(TEXT("preview_kind"), PreviewKind);
	Object->SetStringField(TEXT("header_path"), HeaderPath);
	Object->SetStringField(TEXT("anchor_member_name"), AnchorPropertyName);
	Object->SetNumberField(TEXT("anchor_macro_start_line"), AnchorMacroStartLine);
	Object->SetNumberField(TEXT("anchor_declaration_line"), AnchorDeclarationLine);
	Object->SetStringField(TEXT("anchor_declaration_text"), AnchorDeclarationText);
	Object->SetNumberField(TEXT("insertion_line"), InsertionLine);
	Object->SetStringField(TEXT("new_member_name"), PropertyName);
	Object->SetStringField(TEXT("property_cpp_type"), PropertyCppType);
	Object->SetStringField(TEXT("category"), Category);
	Object->SetStringField(TEXT("default_value"), DefaultValueLiteral);
	Object->SetStringField(TEXT("generated_uproperty"), GeneratedMacroLine);
	Object->SetStringField(TEXT("generated_declaration"), GeneratedDeclarationLine);
	Object->SetStringField(TEXT("generated_snippet"), GeneratedSnippet);
	Object->SetStringField(TEXT("preview_excerpt_after"), PreviewExcerptAfter);
	Object->SetStringField(TEXT("source_hash_before"), SourceHashBefore);
	Object->SetStringField(TEXT("source_hash_after_preview"), SourceHashAfterPreview);
	Object->SetObjectField(TEXT("scope"), BuildScopeObject(*this));
	return Object;
}

TSharedPtr<FJsonObject> FCppReflectionPropertyDeclarationApplyResult::ToJson() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("success"), bSuccess);
	Object->SetBoolField(TEXT("applied"), bApplied);
	Object->SetBoolField(TEXT("would_change_file"), bWouldChangeFile);
	Object->SetBoolField(TEXT("restore_ready"), bRestoreReady);
	Object->SetStringField(TEXT("schema_version"), SchemaVersion);
	Object->SetStringField(TEXT("preview_schema_version"), PreviewSchemaVersion);
	Object->SetStringField(TEXT("declaration_kind"), DeclarationKind);
	Object->SetStringField(TEXT("apply_kind"), ApplyKind);
	Object->SetStringField(TEXT("header_path"), HeaderPath);
	Object->SetStringField(TEXT("anchor_member_name"), AnchorPropertyName);
	Object->SetNumberField(TEXT("anchor_macro_start_line"), AnchorMacroStartLine);
	Object->SetNumberField(TEXT("anchor_declaration_line"), AnchorDeclarationLine);
	Object->SetStringField(TEXT("anchor_declaration_text"), AnchorDeclarationText);
	Object->SetNumberField(TEXT("insertion_line"), InsertionLine);
	Object->SetStringField(TEXT("new_member_name"), PropertyName);
	Object->SetStringField(TEXT("property_cpp_type"), PropertyCppType);
	Object->SetStringField(TEXT("category"), Category);
	Object->SetStringField(TEXT("default_value"), DefaultValueLiteral);
	Object->SetStringField(TEXT("generated_uproperty"), GeneratedMacroLine);
	Object->SetStringField(TEXT("generated_declaration"), GeneratedDeclarationLine);
	Object->SetStringField(TEXT("generated_snippet"), GeneratedSnippet);
	Object->SetStringField(TEXT("expected_source_hash_before"), ExpectedSourceHashBefore);
	Object->SetStringField(TEXT("source_hash_before_apply"), SourceHashBeforeApply);
	Object->SetStringField(TEXT("source_hash_after_preview"), SourceHashAfterPreview);
	Object->SetStringField(TEXT("source_hash_after_apply"), SourceHashAfterApply);
	Object->SetStringField(TEXT("checkpoint_path"), CheckpointPath);
	Object->SetObjectField(TEXT("scope"), BuildScopeObject(*this));
	return Object;
}

TSharedPtr<FJsonObject> FCppReflectionPropertyDeclarationRevertResult::ToJson() const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("success"), bSuccess);
	Object->SetBoolField(TEXT("reverted"), bReverted);
	Object->SetStringField(TEXT("schema_version"), SchemaVersion);
	Object->SetStringField(TEXT("declaration_kind"), DeclarationKind);
	Object->SetStringField(TEXT("revert_kind"), RevertKind);
	Object->SetStringField(TEXT("header_path"), HeaderPath);
	Object->SetStringField(TEXT("new_member_name"), PropertyName);
	Object->SetStringField(TEXT("checkpoint_path"), CheckpointPath);
	Object->SetStringField(TEXT("expected_applied_source_hash"), ExpectedAppliedSourceHash);
	Object->SetStringField(TEXT("expected_restored_source_hash"), ExpectedRestoredSourceHash);
	Object->SetStringField(TEXT("source_hash_before_revert"), SourceHashBeforeRevert);
	Object->SetStringField(TEXT("source_hash_after_revert"), SourceHashAfterRevert);
	Object->SetObjectField(TEXT("scope"), BuildScopeObject(*this));
	return Object;
}

bool TryNormalizeAllowedPropertyMetadataKey(const FString& RequestedKey, FString& OutCanonicalKey, FString& OutError)
{
	const FString Normalized = RequestedKey.TrimStartAndEnd().ToLower();
	if (const FString* Canonical = GetAllowedMetadataKeys().Find(Normalized))
	{
		OutCanonicalKey = *Canonical;
		return true;
	}

	TArray<FString> AllowedValues;
	GetAllowedMetadataKeys().GenerateValueArray(AllowedValues);
	AllowedValues.Sort();
	OutError = FString::Printf(
		TEXT("Unsupported metadata_key '%s'. This bounded mutation lane only allows: %s"),
		*RequestedKey,
		*FString::Join(AllowedValues, TEXT(", ")));
	return false;
}

FCppReflectionPropertyDeclarationPreviewResult PreviewPluginReflectedPropertyDeclaration(
	const FString& HeaderPath,
	const FString& AnchorPropertyName,
	const FString& NewPropertyName,
	const FString& PropertyCppType,
	const FString& Category,
	const FString& DefaultValueLiteral)
{
	FCppReflectionPropertyDeclarationPreviewResult Result;
	Result.SchemaVersion = GetPropertyDeclarationPreviewSchemaVersion();
	Result.DeclarationKind = TEXT("reflected_uproperty_member_declaration");
	Result.PreviewKind = TEXT("insert_after_declared_reflected_property");
	Result.HeaderPath = HeaderPath;
	Result.AnchorPropertyName = AnchorPropertyName;
	Result.PropertyName = NewPropertyName;

	FString Error;
	FString CanonicalType;
	if (!TryNormalizePreviewPropertyType(PropertyCppType, CanonicalType, Error))
	{
		Result.ErrorMessage = Error;
		return Result;
	}
	Result.PropertyCppType = CanonicalType;

	FString CanonicalDefaultValue;
	if (!TryNormalizePreviewDefaultValue(DefaultValueLiteral, CanonicalType, CanonicalDefaultValue, Error))
	{
		Result.ErrorMessage = Error;
		return Result;
	}
	Result.DefaultValueLiteral = CanonicalDefaultValue;

	Result.Category = Category.TrimStartAndEnd();
	if (Result.Category.IsEmpty())
	{
		Result.ErrorMessage = TEXT("category is required for preview_reflected_property_declaration.");
		return Result;
	}

	if (!IsValidCppIdentifier(NewPropertyName))
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("new_member_name '%s' is not a valid simple C++ identifier."),
			*NewPropertyName);
		return Result;
	}

	if (!NewPropertyName.StartsWith(TEXT("b")))
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("new_member_name '%s' is outside the current bool-only preview shape. Bool preview members must start with 'b'."),
			*NewPropertyName);
		return Result;
	}

	if (HeaderPath.IsEmpty() || !IFileManager::Get().FileExists(*HeaderPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Header path '%s' does not exist."), *HeaderPath);
		return Result;
	}

	const FOsvayderUEScopePolicy::FScopeCheckResult ScopeCheck =
		FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(HeaderPath);
	Result.bScopeAllowed = ScopeCheck.bAllowed;
	Result.ScopeMatchedRoot = ScopeCheck.MatchedRoot;
	Result.ScopeDenialReason = ScopeCheck.DenialReason;
	if (!ScopeCheck.bAllowed)
	{
		Result.ErrorMessage = ScopeCheck.DenialReason;
		return Result;
	}

	FHeaderMutationSite MutationSite;
	if (!TryFindHeaderMutationSite(HeaderPath, AnchorPropertyName, MutationSite, Error))
	{
		Result.ErrorMessage = Error;
		return Result;
	}

	if (ContainsWholeWord(MutationSite.OriginalFileContent, NewPropertyName))
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("new_member_name '%s' is already present in the target header text. This bounded declaration lane requires a unique member name in both reflection and source."),
			*NewPropertyName);
		return Result;
	}

	Result.AnchorMacroStartLine = MutationSite.MacroStartLine + 1;
	Result.AnchorDeclarationLine = MutationSite.DeclarationLine + 1;
	Result.AnchorDeclarationText = MutationSite.DeclarationText;
	Result.InsertionLine = MutationSite.DeclarationLine + 2;
	Result.SourceHashBefore = ComputeContentHash(MutationSite.OriginalFileContent);

	const FString Indentation = GetLineIndentation(MutationSite.Lines[MutationSite.MacroStartLine]);
	Result.GeneratedMacroLine = FString::Printf(
		TEXT("%sUPROPERTY(EditAnywhere, Category = %s)"),
		*Indentation,
		*QuoteMetadataLiteral(Result.Category));
	Result.GeneratedDeclarationLine = FString::Printf(
		TEXT("%s%s %s = %s;"),
		*Indentation,
		*Result.PropertyCppType,
		*Result.PropertyName,
		*Result.DefaultValueLiteral);
	Result.GeneratedSnippet =
		Result.GeneratedMacroLine
		+ MutationSite.NewlineStyle
		+ Result.GeneratedDeclarationLine;

	TArray<FString> PreviewLines = MutationSite.Lines;
	const int32 InsertIndex = MutationSite.DeclarationLine + 1;
	PreviewLines.Insert(Result.GeneratedMacroLine, InsertIndex);
	PreviewLines.Insert(Result.GeneratedDeclarationLine, InsertIndex + 1);
	const FString PreviewContent = JoinLines(PreviewLines, MutationSite.NewlineStyle, MutationSite.bEndsWithNewline);
	Result.SourceHashAfterPreview = ComputeContentHash(PreviewContent);
	Result.bWouldChangeFile = Result.SourceHashAfterPreview != Result.SourceHashBefore;

	const int32 ExcerptStartLine = FMath::Max(0, MutationSite.MacroStartLine - 1);
	const int32 ExcerptEndLine = FMath::Min(PreviewLines.Num() - 1, InsertIndex + 2);
	Result.PreviewExcerptAfter = BuildPreviewExcerpt(
		PreviewLines,
		MutationSite.NewlineStyle,
		ExcerptStartLine,
		ExcerptEndLine);
	Result.bSuccess = true;
	return Result;
}

FCppReflectionPropertyDeclarationApplyResult ApplyPluginReflectedPropertyDeclaration(
	const FString& HeaderPath,
	const FString& AnchorPropertyName,
	const FString& NewPropertyName,
	const FString& PropertyCppType,
	const FString& Category,
	const FString& DefaultValueLiteral,
	const FString& ExpectedSourceHashBefore,
	const FString& CheckpointPath)
{
	FCppReflectionPropertyDeclarationApplyResult Result;
	Result.SchemaVersion = GetPropertyDeclarationApplySchemaVersion();
	Result.PreviewSchemaVersion = GetPropertyDeclarationPreviewSchemaVersion();
	Result.DeclarationKind = TEXT("reflected_uproperty_member_declaration");
	Result.ApplyKind = TEXT("insert_after_declared_reflected_property");
	Result.HeaderPath = HeaderPath;
	Result.AnchorPropertyName = AnchorPropertyName;
	Result.PropertyName = NewPropertyName;
	Result.ExpectedSourceHashBefore = ExpectedSourceHashBefore.TrimStartAndEnd();

	const FCppReflectionPropertyDeclarationPreviewResult Preview =
		PreviewPluginReflectedPropertyDeclaration(
			HeaderPath,
			AnchorPropertyName,
			NewPropertyName,
			PropertyCppType,
			Category,
			DefaultValueLiteral);
	Result.bScopeAllowed = Preview.bScopeAllowed;
	Result.ScopeMatchedRoot = Preview.ScopeMatchedRoot;
	Result.ScopeDenialReason = Preview.ScopeDenialReason;
	if (!Preview.bSuccess)
	{
		Result.ErrorMessage = Preview.ErrorMessage;
		return Result;
	}

	Result.bWouldChangeFile = Preview.bWouldChangeFile;
	Result.AnchorMacroStartLine = Preview.AnchorMacroStartLine;
	Result.AnchorDeclarationLine = Preview.AnchorDeclarationLine;
	Result.AnchorDeclarationText = Preview.AnchorDeclarationText;
	Result.InsertionLine = Preview.InsertionLine;
	Result.PropertyCppType = Preview.PropertyCppType;
	Result.Category = Preview.Category;
	Result.DefaultValueLiteral = Preview.DefaultValueLiteral;
	Result.GeneratedMacroLine = Preview.GeneratedMacroLine;
	Result.GeneratedDeclarationLine = Preview.GeneratedDeclarationLine;
	Result.GeneratedSnippet = Preview.GeneratedSnippet;
	Result.SourceHashBeforeApply = Preview.SourceHashBefore;
	Result.SourceHashAfterPreview = Preview.SourceHashAfterPreview;

	if (Result.ExpectedSourceHashBefore.IsEmpty())
	{
		Result.ErrorMessage = TEXT("expected_source_hash_before is required for apply_reflected_property_declaration to enforce preview -> apply continuity.");
		return Result;
	}

	if (!Result.ExpectedSourceHashBefore.Equals(Preview.SourceHashBefore, ESearchCase::CaseSensitive))
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("Current header source hash '%s' no longer matches expected_source_hash_before '%s'. Re-run preview before apply."),
			*Preview.SourceHashBefore,
			*Result.ExpectedSourceHashBefore);
		return Result;
	}

	if (!Result.bWouldChangeFile)
	{
		Result.bSuccess = true;
		return Result;
	}

	if (CheckpointPath.TrimStartAndEnd().IsEmpty())
	{
		Result.ErrorMessage = TEXT("checkpoint_path is required for apply_reflected_property_declaration so the bounded restore path stays durable.");
		return Result;
	}

	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *HeaderPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Could not reload header '%s' for declaration apply."), *HeaderPath);
		return Result;
	}

	const FString CurrentSourceHash = ComputeContentHash(FileContent);
	if (!CurrentSourceHash.Equals(Result.ExpectedSourceHashBefore, ESearchCase::CaseSensitive))
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("Header source drift detected before declaration apply. Expected '%s' but loaded '%s'. Re-run preview before apply."),
			*Result.ExpectedSourceHashBefore,
			*CurrentSourceHash);
		return Result;
	}

	const FString CheckpointDirectory = FPaths::GetPath(CheckpointPath);
	if (!CheckpointDirectory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*CheckpointDirectory, true);
	}

	if (!FFileHelper::SaveStringToFile(FileContent, *CheckpointPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Could not write declaration checkpoint '%s'."), *CheckpointPath);
		return Result;
	}

	const FString NewlineStyle = DetectNewlineStyle(FileContent);
	const bool bEndsWithNewline =
		FileContent.EndsWith(TEXT("\r\n")) || FileContent.EndsWith(TEXT("\n")) || FileContent.EndsWith(TEXT("\r"));

	TArray<FString> Lines;
	FileContent.ParseIntoArrayLines(Lines, false);

	const int32 InsertIndex = Preview.InsertionLine - 1;
	if (!Lines.IsValidIndex(InsertIndex - 1))
	{
		Result.ErrorMessage = TEXT("Stored insertion line is no longer valid during declaration apply.");
		return Result;
	}

	Lines.Insert(Preview.GeneratedMacroLine, InsertIndex);
	Lines.Insert(Preview.GeneratedDeclarationLine, InsertIndex + 1);

	const FString NewFileContent = JoinLines(Lines, NewlineStyle, bEndsWithNewline);
	Result.SourceHashAfterApply = ComputeContentHash(NewFileContent);

	if (!FFileHelper::SaveStringToFile(NewFileContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Could not write updated header '%s' for declaration apply."), *HeaderPath);
		return Result;
	}

	Result.CheckpointPath = CheckpointPath;
	Result.bRestoreReady = true;
	Result.bApplied = true;
	Result.bSuccess = true;
	return Result;
}

FCppReflectionPropertyDeclarationRevertResult RevertPluginReflectedPropertyDeclaration(
	const FString& HeaderPath,
	const FString& PropertyName,
	const FString& CheckpointPath,
	const FString& ExpectedAppliedSourceHash,
	const FString& ExpectedRestoredSourceHash)
{
	FCppReflectionPropertyDeclarationRevertResult Result;
	Result.SchemaVersion = GetPropertyDeclarationRevertSchemaVersion();
	Result.DeclarationKind = TEXT("reflected_uproperty_member_declaration");
	Result.RevertKind = TEXT("restore_checkpointed_header_before_property_declaration_apply");
	Result.HeaderPath = HeaderPath;
	Result.PropertyName = PropertyName;
	Result.CheckpointPath = CheckpointPath;
	Result.ExpectedAppliedSourceHash = ExpectedAppliedSourceHash.TrimStartAndEnd();
	Result.ExpectedRestoredSourceHash = ExpectedRestoredSourceHash.TrimStartAndEnd();

	if (HeaderPath.IsEmpty() || !IFileManager::Get().FileExists(*HeaderPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Header path '%s' does not exist."), *HeaderPath);
		return Result;
	}

	const FOsvayderUEScopePolicy::FScopeCheckResult ScopeCheck =
		FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(HeaderPath);
	Result.bScopeAllowed = ScopeCheck.bAllowed;
	Result.ScopeMatchedRoot = ScopeCheck.MatchedRoot;
	Result.ScopeDenialReason = ScopeCheck.DenialReason;
	if (!ScopeCheck.bAllowed)
	{
		Result.ErrorMessage = ScopeCheck.DenialReason;
		return Result;
	}

	if (CheckpointPath.TrimStartAndEnd().IsEmpty() || !IFileManager::Get().FileExists(*CheckpointPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Checkpoint path '%s' does not exist."), *CheckpointPath);
		return Result;
	}

	FString CurrentFileContent;
	if (!FFileHelper::LoadFileToString(CurrentFileContent, *HeaderPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Could not reload header '%s' for declaration revert."), *HeaderPath);
		return Result;
	}
	Result.SourceHashBeforeRevert = ComputeContentHash(CurrentFileContent);

	if (!Result.ExpectedAppliedSourceHash.IsEmpty()
		&& !Result.SourceHashBeforeRevert.Equals(Result.ExpectedAppliedSourceHash, ESearchCase::CaseSensitive))
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("Declaration revert blocked because current header hash '%s' no longer matches applied hash '%s'."),
			*Result.SourceHashBeforeRevert,
			*Result.ExpectedAppliedSourceHash);
		return Result;
	}

	FString CheckpointContent;
	if (!FFileHelper::LoadFileToString(CheckpointContent, *CheckpointPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Could not load declaration checkpoint '%s'."), *CheckpointPath);
		return Result;
	}

	const FString RestoredHash = ComputeContentHash(CheckpointContent);
	if (!Result.ExpectedRestoredSourceHash.IsEmpty()
		&& !RestoredHash.Equals(Result.ExpectedRestoredSourceHash, ESearchCase::CaseSensitive))
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("Declaration checkpoint hash '%s' no longer matches expected restored hash '%s'."),
			*RestoredHash,
			*Result.ExpectedRestoredSourceHash);
		return Result;
	}

	if (!FFileHelper::SaveStringToFile(CheckpointContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Could not write restored header '%s' during declaration revert."), *HeaderPath);
		return Result;
	}

	Result.SourceHashAfterRevert = RestoredHash;
	Result.bReverted = true;
	Result.bSuccess = true;
	return Result;
}

FCppReflectionPropertyMetadataMutationResult PreviewPluginPropertyMetadataMutation(
	const FString& HeaderPath,
	const FString& PropertyName,
	const FString& MetadataKey,
	const FString& MetadataValue)
{
	FCppReflectionPropertyMetadataMutationResult Result;
	Result.HeaderPath = HeaderPath;
	Result.MetadataValue = MetadataValue;

	FString CanonicalKey;
	FString Error;
	if (!TryNormalizeAllowedPropertyMetadataKey(MetadataKey, CanonicalKey, Error))
	{
		Result.ErrorMessage = Error;
		return Result;
	}
	Result.NormalizedMetadataKey = CanonicalKey;

	if (HeaderPath.IsEmpty() || !IFileManager::Get().FileExists(*HeaderPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Header path '%s' does not exist."), *HeaderPath);
		return Result;
	}

	const FOsvayderUEScopePolicy::FScopeCheckResult ScopeCheck =
		FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(HeaderPath);
	Result.bScopeAllowed = ScopeCheck.bAllowed;
	Result.ScopeMatchedRoot = ScopeCheck.MatchedRoot;
	Result.ScopeDenialReason = ScopeCheck.DenialReason;
	if (!ScopeCheck.bAllowed)
	{
		Result.ErrorMessage = ScopeCheck.DenialReason;
		return Result;
	}

	FHeaderMutationSite MutationSite;
	if (!TryFindHeaderMutationSite(HeaderPath, PropertyName, MutationSite, Error))
	{
		Result.ErrorMessage = Error;
		return Result;
	}

	Result.MacroStartLine = MutationSite.MacroStartLine + 1;
	Result.MacroEndLine = MutationSite.MacroEndLine + 1;
	Result.DeclarationLine = MutationSite.DeclarationLine + 1;
	Result.DeclarationText = MutationSite.DeclarationText;
	Result.MacroBefore = MutationSite.MacroText;
	Result.SourceHashBefore = ComputeContentHash(MutationSite.OriginalFileContent);

	if (!TryMutateMacroText(
			MutationSite.MacroText,
			CanonicalKey,
			MetadataValue,
			Result.MacroAfter,
			Result.ChangeKind,
			Result.ExistingMetadataValue,
			Result.bHadMetaBlock,
			Result.bHadExistingKey,
			Error))
	{
		Result.ErrorMessage = Error;
		return Result;
	}

	Result.bSuccess = true;
	Result.bWouldChangeFile = Result.MacroBefore != Result.MacroAfter;
	if (Result.bWouldChangeFile)
	{
		TArray<FString> PreviewLines = MutationSite.Lines;
		const int32 ReplaceAt = MutationSite.MacroStartLine;
		const int32 RemoveCount = MutationSite.MacroEndLine - MutationSite.MacroStartLine + 1;
		for (int32 Index = 0; Index < RemoveCount; ++Index)
		{
			PreviewLines.RemoveAt(ReplaceAt);
		}

		TArray<FString> ReplacementLines;
		Result.MacroAfter.ParseIntoArrayLines(ReplacementLines, false);
		for (int32 Index = ReplacementLines.Num() - 1; Index >= 0; --Index)
		{
			PreviewLines.Insert(ReplacementLines[Index], ReplaceAt);
		}

		Result.SourceHashAfter = ComputeContentHash(
			JoinLines(PreviewLines, MutationSite.NewlineStyle, MutationSite.bEndsWithNewline));
	}
	else
	{
		Result.SourceHashAfter = Result.SourceHashBefore;
	}
	return Result;
}

FCppReflectionPropertyMetadataMutationResult ApplyPluginPropertyMetadataMutation(
	const FString& HeaderPath,
	const FString& PropertyName,
	const FString& MetadataKey,
	const FString& MetadataValue)
{
	FCppReflectionPropertyMetadataMutationResult Result =
		PreviewPluginPropertyMetadataMutation(HeaderPath, PropertyName, MetadataKey, MetadataValue);
	if (!Result.bSuccess)
	{
		return Result;
	}

	if (!Result.bWouldChangeFile)
	{
		return Result;
	}

	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *HeaderPath))
	{
		Result.bSuccess = false;
		Result.ErrorMessage = FString::Printf(TEXT("Could not reload header '%s' for apply."), *HeaderPath);
		return Result;
	}

	const FString NewlineStyle = DetectNewlineStyle(FileContent);
	const bool bEndsWithNewline =
		FileContent.EndsWith(TEXT("\r\n")) || FileContent.EndsWith(TEXT("\n")) || FileContent.EndsWith(TEXT("\r"));

	TArray<FString> Lines;
	FileContent.ParseIntoArrayLines(Lines, false);

	if (!Lines.IsValidIndex(Result.MacroStartLine - 1) || !Lines.IsValidIndex(Result.MacroEndLine - 1))
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Stored macro line range is no longer valid during apply.");
		return Result;
	}

	const int32 ReplaceAt = Result.MacroStartLine - 1;
	const int32 RemoveCount = Result.MacroEndLine - Result.MacroStartLine + 1;
	for (int32 Index = 0; Index < RemoveCount; ++Index)
	{
		Lines.RemoveAt(ReplaceAt);
	}

	TArray<FString> ReplacementLines;
	Result.MacroAfter.ParseIntoArrayLines(ReplacementLines, false);
	for (int32 Index = ReplacementLines.Num() - 1; Index >= 0; --Index)
	{
		Lines.Insert(ReplacementLines[Index], ReplaceAt);
	}

	const FString NewFileContent = JoinLines(Lines, NewlineStyle, bEndsWithNewline);
	Result.SourceHashAfter = ComputeContentHash(NewFileContent);

	if (!FFileHelper::SaveStringToFile(NewFileContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result.bSuccess = false;
		Result.ErrorMessage = FString::Printf(TEXT("Could not write updated header '%s'."), *HeaderPath);
		return Result;
	}

	Result.bApplied = true;
	return Result;
}
