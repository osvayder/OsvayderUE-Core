// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_CppReflection.h"
#include "CppReflectionMutationSupport.h"

#include "ScriptExecutionManager.h"
#include "UnrealClaudeModule.h"
#include "UnrealClaudeReportArtifacts.h"
#include "UnrealClaudeScopePolicy.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"
#include "ModuleDescriptor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/FieldIterator.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

// ---------------------------------------------------------------------------
// 619 P4 Fix #4: classifier + Testing seam for the CppReflection mutation
// paths. The classifier distinguishes meta-only diffs (safe for Live Coding
// hot-patch) from structural diffs (require editor restart). The Testing
// seam injects a mock TriggerLiveCodingCompile so unit tests can exercise
// the post-write branch without a real LC module.
// ---------------------------------------------------------------------------
namespace UnrealClaude
{
namespace CppReflectionCompile
{
namespace
{
	// Null by default -> production path falls through to the real
	// FScriptExecutionManager::TriggerLiveCodingCompile call.
	TFunction<Testing::FMockTriggerLiveCodingCompileResult()> GTestingTriggerLiveCodingCompileOverride;

	Testing::FMockTriggerLiveCodingCompileResult InvokeTriggerLiveCodingCompile()
	{
		if (GTestingTriggerLiveCodingCompileOverride)
		{
			return GTestingTriggerLiveCodingCompileOverride();
		}
		Testing::FMockTriggerLiveCodingCompileResult Result;
		Result.bSuccess = FScriptExecutionManager::Get().TriggerLiveCodingCompile(
			Result.ErrorLog, Result.Diagnostics);
		return Result;
	}
}

namespace Testing
{
	void SetTriggerLiveCodingCompileOverride(TFunction<FMockTriggerLiveCodingCompileResult()> Override)
	{
		GTestingTriggerLiveCodingCompileOverride = MoveTemp(Override);
	}

	void ClearAllOverrides()
	{
		GTestingTriggerLiveCodingCompileOverride = TFunction<FMockTriggerLiveCodingCompileResult()>();
	}
} // namespace Testing
} // namespace CppReflectionCompile
} // namespace UnrealClaude

// ---------------------------------------------------------------------------
// FCppReflectionMutationClassifier
// ---------------------------------------------------------------------------
// Pure classifier. Given (BeforeSource, AfterSource) snapshots of the full
// header text, determine whether the diff is structural (macro add/remove/
// rewrite) or meta-only (`meta=(...)` contents only, macro identity unchanged).
//
// Target structural triggers per 619 P4 dispatch: UPROPERTY, UFUNCTION,
// UCLASS, USTRUCT, UENUM, UINTERFACE, UDELEGATE. Word-boundary matching so
// UE_LOG / UE_CUSTOM_* / UE_BUILD_* / UE_DEPRECATED do not misfire.
//
// Comment stripping: single-line `//` + block `/* */` are stripped before
// counting macros, so comment-only additions like `// UPROPERTY(...)` do
// not trigger structural classification.
//
// Whitespace churn: if the normalized text of both snapshots is identical
// (all whitespace collapsed, comments stripped), classifier returns
// Unchanged. If only whitespace/comments differ but tokenized content
// matches, same Unchanged classification (safe LC path).
//
// Ambiguous bias: when the classifier cannot confidently pick between
// MetaOnly and Structural, it returns AmbiguousBiasToRestart. The caller
// surfaces a warning log + treats it as structural per spec D4 + Risks #3
// conservative-cost asymmetry.
// ---------------------------------------------------------------------------
namespace
{
	FString StripLineComments(const FString& Source)
	{
		// Remove `//` line comments. Preserves `/*` block comments for the
		// block-stripper. Works line-by-line to avoid regex complexity on
		// multi-line content.
		FString Result;
		Result.Reserve(Source.Len());
		TArray<FString> Lines;
		Source.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			int32 CommentStart = INDEX_NONE;
			bool bInString = false;
			for (int32 i = 0; i < Line.Len(); ++i)
			{
				const TCHAR Ch = Line[i];
				if (Ch == TEXT('"') && (i == 0 || Line[i - 1] != TEXT('\\')))
				{
					bInString = !bInString;
				}
				else if (!bInString && Ch == TEXT('/') && i + 1 < Line.Len() && Line[i + 1] == TEXT('/'))
				{
					CommentStart = i;
					break;
				}
			}
			if (CommentStart != INDEX_NONE)
			{
				Result.Append(Line.Mid(0, CommentStart));
			}
			else
			{
				Result.Append(Line);
			}
			Result.AppendChar(TEXT('\n'));
		}
		return Result;
	}

	FString StripBlockComments(const FString& Source)
	{
		// Remove `/* ... */` block comments. Non-nested per C++ spec.
		FString Result;
		Result.Reserve(Source.Len());
		int32 i = 0;
		while (i < Source.Len())
		{
			if (i + 1 < Source.Len() && Source[i] == TEXT('/') && Source[i + 1] == TEXT('*'))
			{
				int32 End = Source.Find(TEXT("*/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 2);
				if (End == INDEX_NONE)
				{
					// Unterminated block comment - skip to end.
					break;
				}
				i = End + 2;
			}
			else
			{
				Result.AppendChar(Source[i]);
				++i;
			}
		}
		return Result;
	}

	FString StripCommentsAndNormalizeWhitespace(const FString& Source)
	{
		FString Stripped = StripBlockComments(StripLineComments(Source));
		// Collapse whitespace runs to single space + trim.
		FString Normalized;
		Normalized.Reserve(Stripped.Len());
		bool bLastWasSpace = true; // trim leading whitespace
		for (int32 i = 0; i < Stripped.Len(); ++i)
		{
			const TCHAR Ch = Stripped[i];
			if (FChar::IsWhitespace(Ch))
			{
				if (!bLastWasSpace)
				{
					Normalized.AppendChar(TEXT(' '));
					bLastWasSpace = true;
				}
			}
			else
			{
				Normalized.AppendChar(Ch);
				bLastWasSpace = false;
			}
		}
		return Normalized.TrimStartAndEnd();
	}

	// Count occurrences of a structural UHT macro, word-boundary matching.
	// "Word-boundary" = preceding char (or start) is not identifier, and
	// following char (or end) is not identifier. This prevents UE_LOG or
	// MY_UPROPERTY_WRAPPER from matching UPROPERTY.
	int32 CountWordBoundaryOccurrences(const FString& Source, const FString& Token)
	{
		int32 Count = 0;
		int32 From = 0;
		while (From < Source.Len())
		{
			const int32 Idx = Source.Find(Token, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
			if (Idx == INDEX_NONE)
			{
				break;
			}
			const bool bLeftOk = (Idx == 0) || !(FChar::IsAlnum(Source[Idx - 1]) || Source[Idx - 1] == TEXT('_'));
			const int32 RightIdx = Idx + Token.Len();
			const bool bRightOk = (RightIdx >= Source.Len()) || !(FChar::IsAlnum(Source[RightIdx]) || Source[RightIdx] == TEXT('_'));
			if (bLeftOk && bRightOk)
			{
				++Count;
			}
			From = Idx + Token.Len();
		}
		return Count;
	}

	struct FMacroFingerprint
	{
		int32 UPROPERTY = 0;
		int32 UFUNCTION = 0;
		int32 UCLASS = 0;
		int32 USTRUCT = 0;
		int32 UENUM = 0;
		int32 UINTERFACE = 0;
		int32 UDELEGATE = 0;

		bool operator==(const FMacroFingerprint& Other) const
		{
			return UPROPERTY == Other.UPROPERTY
				&& UFUNCTION == Other.UFUNCTION
				&& UCLASS == Other.UCLASS
				&& USTRUCT == Other.USTRUCT
				&& UENUM == Other.UENUM
				&& UINTERFACE == Other.UINTERFACE
				&& UDELEGATE == Other.UDELEGATE;
		}

		bool operator!=(const FMacroFingerprint& Other) const { return !(*this == Other); }

		FString ToString() const
		{
			return FString::Printf(
				TEXT("UPROPERTY=%d UFUNCTION=%d UCLASS=%d USTRUCT=%d UENUM=%d UINTERFACE=%d UDELEGATE=%d"),
				UPROPERTY, UFUNCTION, UCLASS, USTRUCT, UENUM, UINTERFACE, UDELEGATE);
		}
	};

	FMacroFingerprint FingerprintMacros(const FString& CommentsStrippedSource)
	{
		FMacroFingerprint FP;
		FP.UPROPERTY = CountWordBoundaryOccurrences(CommentsStrippedSource, TEXT("UPROPERTY"));
		FP.UFUNCTION = CountWordBoundaryOccurrences(CommentsStrippedSource, TEXT("UFUNCTION"));
		FP.UCLASS = CountWordBoundaryOccurrences(CommentsStrippedSource, TEXT("UCLASS"));
		FP.USTRUCT = CountWordBoundaryOccurrences(CommentsStrippedSource, TEXT("USTRUCT"));
		FP.UENUM = CountWordBoundaryOccurrences(CommentsStrippedSource, TEXT("UENUM"));
		FP.UINTERFACE = CountWordBoundaryOccurrences(CommentsStrippedSource, TEXT("UINTERFACE"));
		FP.UDELEGATE = CountWordBoundaryOccurrences(CommentsStrippedSource, TEXT("UDELEGATE"));
		return FP;
	}
}

FCppReflectionDiffClassificationResult FCppReflectionMutationClassifier::ClassifyDiff(
	const FString& BeforeSource,
	const FString& AfterSource)
{
	FCppReflectionDiffClassificationResult Out;

	// Cheap early-out: byte-identical text => Unchanged.
	if (BeforeSource.Equals(AfterSource, ESearchCase::CaseSensitive))
	{
		Out.Classification = ECppReflectionDiffClassification::Unchanged;
		Out.ReasonCode = TEXT("identical_source");
		Out.ReasonDetail = TEXT("Before and after source snapshots are byte-identical.");
		return Out;
	}

	// Normalized comparison: whitespace/comments collapsed. If identical
	// after strip, the diff is cosmetic only -> safe LC path.
	const FString NormBefore = StripCommentsAndNormalizeWhitespace(BeforeSource);
	const FString NormAfter = StripCommentsAndNormalizeWhitespace(AfterSource);
	if (NormBefore.Equals(NormAfter, ESearchCase::CaseSensitive))
	{
		Out.Classification = ECppReflectionDiffClassification::Unchanged;
		Out.ReasonCode = TEXT("whitespace_or_comment_only");
		Out.ReasonDetail = TEXT("Only whitespace and/or comments changed; semantic content is identical after normalization.");
		return Out;
	}

	// Fingerprint the macro counts after stripping comments. If counts
	// differ, a structural macro was added or removed.
	const FString StrippedBefore = StripBlockComments(StripLineComments(BeforeSource));
	const FString StrippedAfter = StripBlockComments(StripLineComments(AfterSource));
	const FMacroFingerprint FPBefore = FingerprintMacros(StrippedBefore);
	const FMacroFingerprint FPAfter = FingerprintMacros(StrippedAfter);
	if (FPBefore != FPAfter)
	{
		Out.Classification = ECppReflectionDiffClassification::Structural;
		Out.ReasonCode = TEXT("uht_macro_count_changed");
		Out.ReasonDetail = FString::Printf(
			TEXT("UHT macro count changed. Before: [%s]. After: [%s]. Reflection layout differs."),
			*FPBefore.ToString(), *FPAfter.ToString());
		return Out;
	}

	// Same macro counts but semantic content differs: could be:
	// (a) meta=() contents changed only - MetaOnly (safe),
	// (b) macro invocation contents changed (e.g. UPROPERTY(EditAnywhere) -> UPROPERTY(VisibleAnywhere))
	//     or property type/name changed - Structural,
	// (c) unrelated function-body edit inside the same file - MetaOnly-equivalent.
	//
	// We cannot parse C++ in this classifier. Conservative bias: if the
	// diff touches text inside any UHT macro invocation parenthesis group
	// outside a `meta=(` subgroup, treat as AmbiguousBiasToRestart.
	//
	// Heuristic: strip all `meta=( ... )` groups from both normalized
	// texts and compare what's left. If the residuals are identical,
	// the diff is meta-only (MetaOnly). Otherwise, ambiguous -> restart.
	auto StripMetaGroups = [](const FString& In) -> FString
	{
		FString Out;
		Out.Reserve(In.Len());
		int32 i = 0;
		while (i < In.Len())
		{
			const int32 MetaStart = In.Find(TEXT("meta=("), ESearchCase::CaseSensitive, ESearchDir::FromStart, i);
			if (MetaStart == INDEX_NONE)
			{
				Out.Append(In.Mid(i));
				break;
			}
			Out.Append(In.Mid(i, MetaStart - i));
			// Skip the meta=(...) group, balancing parens.
			int32 Depth = 1;
			int32 Cursor = MetaStart + 6; // past "meta=("
			while (Cursor < In.Len() && Depth > 0)
			{
				if (In[Cursor] == TEXT('(')) { ++Depth; }
				else if (In[Cursor] == TEXT(')')) { --Depth; }
				++Cursor;
			}
			i = Cursor;
		}
		return Out;
	};

	const FString ResidualBefore = StripMetaGroups(NormBefore);
	const FString ResidualAfter = StripMetaGroups(NormAfter);
	if (ResidualBefore.Equals(ResidualAfter, ESearchCase::CaseSensitive))
	{
		Out.Classification = ECppReflectionDiffClassification::MetaOnly;
		Out.ReasonCode = TEXT("meta_contents_only");
		Out.ReasonDetail = TEXT("Only `meta=()` group contents changed; macro invocations and reflected layout are unchanged outside meta groups.");
		return Out;
	}

	// Residuals differ: the macro parenthesis content changed outside
	// meta=(), OR text adjacent to the macros changed. Could be structural
	// (specifier change, type rename, new reflected member) or non-reflected
	// code edit. Conservative bias: treat as restart-required and log.
	Out.Classification = ECppReflectionDiffClassification::AmbiguousBiasToRestart;
	Out.ReasonCode = TEXT("residual_diff_outside_meta_group");
	Out.ReasonDetail = TEXT("Diff touches text outside `meta=()` groups; classifier cannot confirm reflected layout is unchanged. Conservative bias: requires restart.");
	Out.bBiasedToRestart = true;
	return Out;
}

namespace
{
	enum class ECppReflectionKindFilter : uint8
	{
		All,
		Class,
		Struct,
		Enum,
	};

	enum class ECppReflectionModuleScope : uint8
	{
		ProjectAndPlugin,
		ProjectOnly,
		PluginOnly,
		EngineOnly,
		AllLoadedModules,
	};

	const FString& GetPropertyDeclarationBuildFailureDiagnosticSchemaVersion()
	{
		static const FString SchemaVersion = TEXT("reflected_property_declaration_build_failure_diagnostics_v1");
		return SchemaVersion;
	}

	const FString& GetPropertyDeclarationEvidenceBundleSchemaVersion()
	{
		static const FString SchemaVersion = TEXT("reflected_property_declaration_evidence_bundle_v1");
		return SchemaVersion;
	}

	struct FCppModuleLocator
	{
		TMap<FString, FString> ProjectModuleRoots;
		TMap<FString, FString> PluginModuleRoots;

		FCppModuleLocator()
		{
			const FString ProjectSourceDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
			if (FPaths::DirectoryExists(ProjectSourceDir))
			{
				TArray<FString> ModuleDirs;
				IFileManager::Get().FindFiles(ModuleDirs, *(ProjectSourceDir / TEXT("*")), false, true);
				for (const FString& ModuleDir : ModuleDirs)
				{
					ProjectModuleRoots.Add(ModuleDir, ProjectSourceDir / ModuleDir);
				}
			}

			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
			{
				const FString PluginSourceDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Source"));
				for (const FModuleDescriptor& ModuleDescriptor : Plugin->GetDescriptor().Modules)
				{
					PluginModuleRoots.FindOrAdd(ModuleDescriptor.Name.ToString(), PluginSourceDir / ModuleDescriptor.Name.ToString());
				}
			}
		}

		FString GetModuleOrigin(const FString& ModuleName) const
		{
			if (ModuleName.IsEmpty())
			{
				return TEXT("unknown");
			}

			if (ProjectModuleRoots.Contains(ModuleName))
			{
				return TEXT("project");
			}

			if (PluginModuleRoots.Contains(ModuleName))
			{
				return TEXT("plugin");
			}

			return TEXT("engine_or_external");
		}

		FString ResolveHeaderPath(const FString& ModuleName, const FString& ModuleRelativePath) const
		{
			if (ModuleName.IsEmpty() || ModuleRelativePath.IsEmpty())
			{
				return FString();
			}

			if (const FString* ProjectRoot = ProjectModuleRoots.Find(ModuleName))
			{
				FString Candidate = FPaths::ConvertRelativePathToFull(*ProjectRoot / ModuleRelativePath);
				FPaths::NormalizeFilename(Candidate);
				if (FPaths::FileExists(Candidate))
				{
					return Candidate;
				}
			}

			if (const FString* PluginRoot = PluginModuleRoots.Find(ModuleName))
			{
				FString Candidate = FPaths::ConvertRelativePathToFull(*PluginRoot / ModuleRelativePath);
				FPaths::NormalizeFilename(Candidate);
				if (FPaths::FileExists(Candidate))
				{
					return Candidate;
				}
			}

			return FString();
		}
	};

	FString NormalizeFilterValue(const FString& Value)
	{
		return Value.TrimStartAndEnd().ToLower();
	}

	ECppReflectionKindFilter ParseKindFilter(const FString& KindValue, FString& OutError)
	{
		const FString Normalized = NormalizeFilterValue(KindValue);
		if (Normalized.IsEmpty() || Normalized == TEXT("all"))
		{
			return ECppReflectionKindFilter::All;
		}

		if (Normalized == TEXT("class"))
		{
			return ECppReflectionKindFilter::Class;
		}

		if (Normalized == TEXT("struct"))
		{
			return ECppReflectionKindFilter::Struct;
		}

		if (Normalized == TEXT("enum"))
		{
			return ECppReflectionKindFilter::Enum;
		}

		OutError = FString::Printf(
			TEXT("Unknown symbol_kind '%s'. Expected one of: all, class, struct, enum"),
			*KindValue);
		return ECppReflectionKindFilter::All;
	}

	ECppReflectionModuleScope ParseModuleScope(const FString& ScopeValue, FString& OutError)
	{
		const FString Normalized = NormalizeFilterValue(ScopeValue);
		if (Normalized.IsEmpty() || Normalized == TEXT("project_and_plugin"))
		{
			return ECppReflectionModuleScope::ProjectAndPlugin;
		}

		if (Normalized == TEXT("project_only"))
		{
			return ECppReflectionModuleScope::ProjectOnly;
		}

		if (Normalized == TEXT("plugin_only"))
		{
			return ECppReflectionModuleScope::PluginOnly;
		}

		if (Normalized == TEXT("engine_only"))
		{
			return ECppReflectionModuleScope::EngineOnly;
		}

		if (Normalized == TEXT("all_loaded_modules"))
		{
			return ECppReflectionModuleScope::AllLoadedModules;
		}

		OutError = FString::Printf(
			TEXT("Unknown module_scope '%s'. Expected one of: project_and_plugin, project_only, plugin_only, engine_only, all_loaded_modules"),
			*ScopeValue);
		return ECppReflectionModuleScope::ProjectAndPlugin;
	}

	FString KindFilterToString(const ECppReflectionKindFilter KindFilter)
	{
		switch (KindFilter)
		{
		case ECppReflectionKindFilter::Class:
			return TEXT("class");
		case ECppReflectionKindFilter::Struct:
			return TEXT("struct");
		case ECppReflectionKindFilter::Enum:
			return TEXT("enum");
		case ECppReflectionKindFilter::All:
		default:
			return TEXT("all");
		}
	}

	FString ModuleScopeToString(const ECppReflectionModuleScope Scope)
	{
		switch (Scope)
		{
		case ECppReflectionModuleScope::ProjectOnly:
			return TEXT("project_only");
		case ECppReflectionModuleScope::PluginOnly:
			return TEXT("plugin_only");
		case ECppReflectionModuleScope::EngineOnly:
			return TEXT("engine_only");
		case ECppReflectionModuleScope::AllLoadedModules:
			return TEXT("all_loaded_modules");
		case ECppReflectionModuleScope::ProjectAndPlugin:
		default:
			return TEXT("project_and_plugin");
		}
	}

	FString GetPackageNameSafe(const UObject* Object)
	{
		const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
		return Package ? Package->GetName() : FString();
	}

	FString GetModuleNameForObject(const UObject* Object)
	{
		const FString PackageName = GetPackageNameSafe(Object);
		const FString ScriptPrefix = TEXT("/Script/");
		if (PackageName.StartsWith(ScriptPrefix))
		{
			return PackageName.RightChop(ScriptPrefix.Len());
		}

		return FString();
	}

	bool IsNativeScriptObject(const UObject* Object)
	{
		return GetPackageNameSafe(Object).StartsWith(TEXT("/Script/"));
	}

	FString GetCppSymbolName(const UObject* Object)
	{
		if (const UClass* Class = Cast<UClass>(Object))
		{
			return FString(Class->GetPrefixCPP()) + Class->GetName();
		}

		if (const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(Object))
		{
			return FString(ScriptStruct->GetPrefixCPP()) + ScriptStruct->GetName();
		}

		if (const UEnum* Enum = Cast<UEnum>(Object))
		{
			return Enum->CppType.IsEmpty() ? Enum->GetName() : Enum->CppType;
		}

		return Object ? Object->GetName() : FString();
	}

	FString GetKindString(const UObject* Object)
	{
		if (Cast<UClass>(Object))
		{
			return TEXT("class");
		}

		if (Cast<UScriptStruct>(Object))
		{
			return TEXT("struct");
		}

		if (Cast<UEnum>(Object))
		{
			return TEXT("enum");
		}

		return TEXT("unknown");
	}

	const UField* GetFieldForMetadata(const UObject* Object)
	{
		return Cast<UField>(Object);
	}

	FString GetModuleRelativePath(const UObject* Object)
	{
		const UField* Field = GetFieldForMetadata(Object);
		if (Field && Field->HasMetaData(TEXT("ModuleRelativePath")))
		{
			return Field->GetMetaData(TEXT("ModuleRelativePath"));
		}

		return FString();
	}

	bool MatchesModuleScope(const FString& ModuleOrigin, const ECppReflectionModuleScope ModuleScope)
	{
		if (ModuleScope == ECppReflectionModuleScope::AllLoadedModules)
		{
			return true;
		}

		if (ModuleScope == ECppReflectionModuleScope::ProjectAndPlugin)
		{
			return ModuleOrigin == TEXT("project") || ModuleOrigin == TEXT("plugin");
		}

		if (ModuleScope == ECppReflectionModuleScope::ProjectOnly)
		{
			return ModuleOrigin == TEXT("project");
		}

		if (ModuleScope == ECppReflectionModuleScope::PluginOnly)
		{
			return ModuleOrigin == TEXT("plugin");
		}

		if (ModuleScope == ECppReflectionModuleScope::EngineOnly)
		{
			return ModuleOrigin == TEXT("engine_or_external");
		}

		return false;
	}

	bool MatchesKind(const UObject* Object, const ECppReflectionKindFilter KindFilter)
	{
		switch (KindFilter)
		{
		case ECppReflectionKindFilter::Class:
			return Cast<UClass>(Object) != nullptr;
		case ECppReflectionKindFilter::Struct:
			return Cast<UScriptStruct>(Object) != nullptr;
		case ECppReflectionKindFilter::Enum:
			return Cast<UEnum>(Object) != nullptr;
		case ECppReflectionKindFilter::All:
		default:
			return true;
		}
	}

	bool MatchesSubstringFilter(const FString& Candidate, const FString& Filter)
	{
		return Filter.IsEmpty() || Candidate.Contains(Filter, ESearchCase::IgnoreCase);
	}

	bool MatchesObjectFilters(
		const UObject* Object,
		const ECppReflectionKindFilter KindFilter,
		const ECppReflectionModuleScope ModuleScope,
		const FString& NameFilter,
		const FString& ModuleFilter,
		const FCppModuleLocator& ModuleLocator)
	{
		if (!Object || !IsNativeScriptObject(Object))
		{
			return false;
		}

		if (!MatchesKind(Object, KindFilter))
		{
			return false;
		}

		const FString ModuleName = GetModuleNameForObject(Object);
		const FString ModuleOrigin = ModuleLocator.GetModuleOrigin(ModuleName);
		if (!MatchesModuleScope(ModuleOrigin, ModuleScope))
		{
			return false;
		}

		if (!MatchesSubstringFilter(ModuleName, ModuleFilter))
		{
			return false;
		}

		if (NameFilter.IsEmpty())
		{
			return true;
		}

		return MatchesSubstringFilter(Object->GetName(), NameFilter)
			|| MatchesSubstringFilter(GetCppSymbolName(Object), NameFilter)
			|| MatchesSubstringFilter(Object->GetPathName(), NameFilter);
	}

	TSharedPtr<FJsonObject> BuildSelectedMetadata(const UField* Field, const TArray<FString>& Keys)
	{
		if (!Field)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> MetadataObject = MakeShared<FJsonObject>();
		int32 AddedCount = 0;
		for (const FString& Key : Keys)
		{
			if (Field->HasMetaData(*Key))
			{
				MetadataObject->SetStringField(Key, Field->GetMetaData(*Key));
				AddedCount++;
			}
		}

		return AddedCount > 0 ? MetadataObject : nullptr;
	}

	TSharedPtr<FJsonObject> BuildSelectedMetadata(const FField* Field, const TArray<FString>& Keys)
	{
		if (!Field)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> MetadataObject = MakeShared<FJsonObject>();
		int32 AddedCount = 0;
		for (const FString& Key : Keys)
		{
			if (Field->HasMetaData(*Key))
			{
				MetadataObject->SetStringField(Key, Field->GetMetaData(*Key));
				AddedCount++;
			}
		}

		return AddedCount > 0 ? MetadataObject : nullptr;
	}

	int32 CountDeclaredProperties(const UStruct* OwnerStruct, const bool bIncludeInherited)
	{
		if (!OwnerStruct)
		{
			return 0;
		}

		int32 Count = 0;
		const EFieldIteratorFlags::SuperClassFlags SuperFlags =
			bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
		for (TFieldIterator<FProperty> It(OwnerStruct, SuperFlags); It; ++It)
		{
			Count++;
		}

		return Count;
	}

	int32 CountDeclaredFunctions(const UClass* OwnerClass, const bool bIncludeInherited)
	{
		if (!OwnerClass)
		{
			return 0;
		}

		int32 Count = 0;
		const EFieldIteratorFlags::SuperClassFlags SuperFlags =
			bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
		for (TFieldIterator<UFunction> It(OwnerClass, SuperFlags); It; ++It)
		{
			Count++;
		}

		return Count;
	}

	TSharedPtr<FJsonObject> BuildSourceLocationObject(const UObject* Object, const FCppModuleLocator& ModuleLocator)
	{
		TSharedPtr<FJsonObject> SourceObject = MakeShared<FJsonObject>();
		const FString ModuleName = GetModuleNameForObject(Object);
		const FString ModuleRelativePath = GetModuleRelativePath(Object);
		const FString ResolvedHeaderPath = ModuleLocator.ResolveHeaderPath(ModuleName, ModuleRelativePath);

		SourceObject->SetStringField(TEXT("module_name"), ModuleName);
		SourceObject->SetStringField(TEXT("module_origin"), ModuleLocator.GetModuleOrigin(ModuleName));
		SourceObject->SetStringField(TEXT("module_relative_path"), ModuleRelativePath);
		SourceObject->SetStringField(TEXT("resolved_header_path"), ResolvedHeaderPath);
		SourceObject->SetStringField(TEXT("package_path"), GetPackageNameSafe(Object));
		return SourceObject;
	}

	TSharedPtr<FJsonObject> BuildTypeFlagsObject(const UObject* Object)
	{
		TSharedPtr<FJsonObject> FlagsObject = MakeShared<FJsonObject>();

		if (const UClass* Class = Cast<UClass>(Object))
		{
			FlagsObject->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
			FlagsObject->SetBoolField(TEXT("blueprint_type"), Class->HasMetaData(TEXT("BlueprintType")));
			FlagsObject->SetBoolField(TEXT("blueprintable"), Class->HasMetaData(TEXT("Blueprintable")));
			FlagsObject->SetBoolField(TEXT("config_class"), Class->HasAnyClassFlags(CLASS_Config));
			FlagsObject->SetBoolField(TEXT("default_config"), Class->HasAnyClassFlags(CLASS_DefaultConfig));
			FlagsObject->SetBoolField(TEXT("placeable"), !Class->HasAnyClassFlags(CLASS_NotPlaceable));
			FlagsObject->SetBoolField(TEXT("edit_inline_new"), Class->HasAnyClassFlags(CLASS_EditInlineNew));
			if (!Class->ClassConfigName.IsNone())
			{
				FlagsObject->SetStringField(TEXT("class_config_name"), Class->ClassConfigName.ToString());
			}
		}
		else if (const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(Object))
		{
			FlagsObject->SetBoolField(TEXT("blueprint_type"), ScriptStruct->HasMetaData(TEXT("BlueprintType")));
			FlagsObject->SetBoolField(TEXT("atomic"), ScriptStruct->StructFlags & STRUCT_Atomic);
			FlagsObject->SetBoolField(TEXT("immutable"), ScriptStruct->StructFlags & STRUCT_Immutable);
		}
		else if (const UEnum* Enum = Cast<UEnum>(Object))
		{
			FlagsObject->SetBoolField(TEXT("blueprint_type"), Enum->HasMetaData(TEXT("BlueprintType")));
			FlagsObject->SetStringField(
				TEXT("cpp_form"),
				Enum->GetCppForm() == UEnum::ECppForm::EnumClass ? TEXT("enum_class") :
				Enum->GetCppForm() == UEnum::ECppForm::Namespaced ? TEXT("namespaced") :
				TEXT("regular"));
		}

		return FlagsObject;
	}

	TSharedPtr<FJsonObject> BuildTypeSummaryObject(
		const UObject* Object,
		const FCppModuleLocator& ModuleLocator,
		const bool bIncludeMemberCounts)
	{
		TSharedPtr<FJsonObject> SummaryObject = MakeShared<FJsonObject>();
		SummaryObject->SetStringField(TEXT("kind"), GetKindString(Object));
		SummaryObject->SetStringField(TEXT("reflection_name"), Object->GetName());
		SummaryObject->SetStringField(TEXT("cpp_name"), GetCppSymbolName(Object));
		SummaryObject->SetStringField(TEXT("path"), Object->GetPathName());
		SummaryObject->SetStringField(TEXT("module_name"), GetModuleNameForObject(Object));
		SummaryObject->SetStringField(TEXT("module_origin"), ModuleLocator.GetModuleOrigin(GetModuleNameForObject(Object)));
		SummaryObject->SetObjectField(TEXT("source_location"), BuildSourceLocationObject(Object, ModuleLocator));
		SummaryObject->SetObjectField(TEXT("flags"), BuildTypeFlagsObject(Object));

		if (const UClass* Class = Cast<UClass>(Object))
		{
			if (const UClass* SuperClass = Class->GetSuperClass())
			{
				SummaryObject->SetStringField(TEXT("base_cpp_name"), GetCppSymbolName(SuperClass));
				SummaryObject->SetStringField(TEXT("base_path"), SuperClass->GetPathName());
			}

			if (bIncludeMemberCounts)
			{
				TSharedPtr<FJsonObject> CountsObject = MakeShared<FJsonObject>();
				CountsObject->SetNumberField(TEXT("declared_properties"), CountDeclaredProperties(Class, false));
				CountsObject->SetNumberField(TEXT("declared_functions"), CountDeclaredFunctions(Class, false));
				SummaryObject->SetObjectField(TEXT("member_counts"), CountsObject);
			}
		}
		else if (const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(Object))
		{
			if (const UStruct* SuperStruct = ScriptStruct->GetSuperStruct())
			{
				SummaryObject->SetStringField(TEXT("base_cpp_name"), GetCppSymbolName(SuperStruct));
				SummaryObject->SetStringField(TEXT("base_path"), SuperStruct->GetPathName());
			}

			if (bIncludeMemberCounts)
			{
				TSharedPtr<FJsonObject> CountsObject = MakeShared<FJsonObject>();
				CountsObject->SetNumberField(TEXT("declared_properties"), CountDeclaredProperties(ScriptStruct, false));
				SummaryObject->SetObjectField(TEXT("member_counts"), CountsObject);
			}
		}
		else if (const UEnum* Enum = Cast<UEnum>(Object))
		{
			SummaryObject->SetNumberField(TEXT("enumerator_count"), Enum->NumEnums());
		}

		return SummaryObject;
	}

	TSharedPtr<FJsonObject> BuildPropertyTypeDetailObject(const FProperty* Property)
	{
		TSharedPtr<FJsonObject> TypeDetailObject = MakeShared<FJsonObject>();

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("array"));
			TypeDetailObject->SetStringField(TEXT("inner_cpp_type"), ArrayProperty->Inner ? ArrayProperty->Inner->GetCPPType() : FString());
			return TypeDetailObject;
		}

		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("set"));
			TypeDetailObject->SetStringField(TEXT("element_cpp_type"), SetProperty->ElementProp ? SetProperty->ElementProp->GetCPPType() : FString());
			return TypeDetailObject;
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("map"));
			TypeDetailObject->SetStringField(TEXT("key_cpp_type"), MapProperty->KeyProp ? MapProperty->KeyProp->GetCPPType() : FString());
			TypeDetailObject->SetStringField(TEXT("value_cpp_type"), MapProperty->ValueProp ? MapProperty->ValueProp->GetCPPType() : FString());
			return TypeDetailObject;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("struct"));
			TypeDetailObject->SetStringField(TEXT("struct_cpp_name"), GetCppSymbolName(StructProperty->Struct));
			TypeDetailObject->SetStringField(TEXT("struct_path"), StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString());
			return TypeDetailObject;
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("enum"));
			TypeDetailObject->SetStringField(TEXT("enum_cpp_name"), EnumProperty->GetEnum() ? GetCppSymbolName(EnumProperty->GetEnum()) : FString());
			TypeDetailObject->SetStringField(TEXT("enum_path"), EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetPathName() : FString());
			return TypeDetailObject;
		}

		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (ByteProperty->Enum)
			{
				TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("enum"));
				TypeDetailObject->SetStringField(TEXT("enum_cpp_name"), GetCppSymbolName(ByteProperty->Enum));
				TypeDetailObject->SetStringField(TEXT("enum_path"), ByteProperty->Enum->GetPathName());
				return TypeDetailObject;
			}
		}

		if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("soft_class"));
			TypeDetailObject->SetStringField(TEXT("meta_class_cpp_name"), SoftClassProperty->MetaClass ? GetCppSymbolName(SoftClassProperty->MetaClass) : FString());
			return TypeDetailObject;
		}

		if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("soft_object"));
			TypeDetailObject->SetStringField(TEXT("property_class_cpp_name"), SoftObjectProperty->PropertyClass ? GetCppSymbolName(SoftObjectProperty->PropertyClass) : FString());
			return TypeDetailObject;
		}

		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("class"));
			TypeDetailObject->SetStringField(TEXT("meta_class_cpp_name"), ClassProperty->MetaClass ? GetCppSymbolName(ClassProperty->MetaClass) : FString());
			return TypeDetailObject;
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("object"));
			TypeDetailObject->SetStringField(TEXT("property_class_cpp_name"), ObjectProperty->PropertyClass ? GetCppSymbolName(ObjectProperty->PropertyClass) : FString());
			return TypeDetailObject;
		}

		if (const FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("interface"));
			TypeDetailObject->SetStringField(TEXT("interface_cpp_name"), InterfaceProperty->InterfaceClass ? GetCppSymbolName(InterfaceProperty->InterfaceClass) : FString());
			return TypeDetailObject;
		}

		if (CastField<const FDelegateProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("delegate"));
			return TypeDetailObject;
		}

		if (CastField<const FMulticastDelegateProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("multicast_delegate"));
			return TypeDetailObject;
		}

		if (CastField<const FBoolProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("bool"));
			return TypeDetailObject;
		}

		if (CastField<const FNameProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("name"));
			return TypeDetailObject;
		}

		if (CastField<const FStrProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("string"));
			return TypeDetailObject;
		}

		if (CastField<const FTextProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("text"));
			return TypeDetailObject;
		}

		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			TypeDetailObject->SetStringField(TEXT("property_kind"), NumericProperty->IsFloatingPoint() ? TEXT("float") : TEXT("integer"));
			return TypeDetailObject;
		}

		TypeDetailObject->SetStringField(TEXT("property_kind"), TEXT("unknown"));
		return TypeDetailObject;
	}

	TSharedPtr<FJsonObject> BuildPropertyObject(const FProperty* Property, const bool bIncludeMetadata)
	{
		TSharedPtr<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
		PropertyObject->SetStringField(TEXT("name"), Property->GetName());
		PropertyObject->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		PropertyObject->SetStringField(TEXT("declaring_type"), Property->GetOwnerStruct() ? GetCppSymbolName(Property->GetOwnerStruct()) : FString());
		PropertyObject->SetStringField(TEXT("category"), Property->HasMetaData(TEXT("Category")) ? Property->GetMetaData(TEXT("Category")) : FString());

		TSharedPtr<FJsonObject> FlagsObject = MakeShared<FJsonObject>();
		FlagsObject->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
		FlagsObject->SetBoolField(TEXT("edit_const"), Property->HasAnyPropertyFlags(CPF_EditConst));
		FlagsObject->SetBoolField(TEXT("blueprint_visible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
		FlagsObject->SetBoolField(TEXT("blueprint_read_only"), Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
		FlagsObject->SetBoolField(TEXT("config"), Property->HasAnyPropertyFlags(CPF_Config));
		FlagsObject->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));
		FlagsObject->SetBoolField(TEXT("save_game"), Property->HasAnyPropertyFlags(CPF_SaveGame));
		FlagsObject->SetBoolField(TEXT("replicated"), Property->HasAnyPropertyFlags(CPF_Net));
		FlagsObject->SetBoolField(TEXT("rep_notify"), Property->HasAnyPropertyFlags(CPF_RepNotify));
		PropertyObject->SetObjectField(TEXT("flags"), FlagsObject);
		PropertyObject->SetObjectField(TEXT("type_detail"), BuildPropertyTypeDetailObject(Property));

		if (bIncludeMetadata)
		{
			const TArray<FString> MetadataKeys = {
				TEXT("DisplayName"),
				TEXT("ToolTip"),
				TEXT("ClampMin"),
				TEXT("ClampMax"),
				TEXT("UIMin"),
				TEXT("UIMax"),
				TEXT("MultiLine"),
				TEXT("AllowPrivateAccess"),
				TEXT("EditCondition")
			};
			if (TSharedPtr<FJsonObject> MetadataObject = BuildSelectedMetadata(Property, MetadataKeys))
			{
				PropertyObject->SetObjectField(TEXT("metadata"), MetadataObject);
			}
		}

		return PropertyObject;
	}

	TSharedPtr<FJsonObject> BuildFunctionParameterObject(const FProperty* ParameterProperty)
	{
		TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
		ParameterObject->SetStringField(TEXT("name"), ParameterProperty->GetName());
		ParameterObject->SetStringField(TEXT("cpp_type"), ParameterProperty->GetCPPType());

		TSharedPtr<FJsonObject> FlagsObject = MakeShared<FJsonObject>();
		FlagsObject->SetBoolField(TEXT("is_out"), ParameterProperty->HasAnyPropertyFlags(CPF_OutParm));
		FlagsObject->SetBoolField(TEXT("is_reference"), ParameterProperty->HasAnyPropertyFlags(CPF_ReferenceParm));
		FlagsObject->SetBoolField(TEXT("is_const"), ParameterProperty->HasAnyPropertyFlags(CPF_ConstParm));
		FlagsObject->SetBoolField(TEXT("is_return"), ParameterProperty->HasAnyPropertyFlags(CPF_ReturnParm));
		ParameterObject->SetObjectField(TEXT("flags"), FlagsObject);
		ParameterObject->SetObjectField(TEXT("type_detail"), BuildPropertyTypeDetailObject(ParameterProperty));
		return ParameterObject;
	}

	TSharedPtr<FJsonObject> BuildFunctionObject(const UFunction* Function, const bool bIncludeMetadata)
	{
		TSharedPtr<FJsonObject> FunctionObject = MakeShared<FJsonObject>();
		FunctionObject->SetStringField(TEXT("name"), Function->GetName());
		FunctionObject->SetStringField(TEXT("declaring_type"), Function->GetOwnerClass() ? GetCppSymbolName(Function->GetOwnerClass()) : FString());
		FunctionObject->SetStringField(TEXT("category"), Function->HasMetaData(TEXT("Category")) ? Function->GetMetaData(TEXT("Category")) : FString());

		TSharedPtr<FJsonObject> FlagsObject = MakeShared<FJsonObject>();
		FlagsObject->SetBoolField(TEXT("blueprint_callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		FlagsObject->SetBoolField(TEXT("blueprint_pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
		FlagsObject->SetBoolField(TEXT("blueprint_event"), Function->HasAnyFunctionFlags(FUNC_BlueprintEvent));
		FlagsObject->SetBoolField(TEXT("blueprint_authority_only"), Function->HasAnyFunctionFlags(FUNC_BlueprintAuthorityOnly));
		FlagsObject->SetBoolField(TEXT("blueprint_cosmetic"), Function->HasAnyFunctionFlags(FUNC_BlueprintCosmetic));
		FlagsObject->SetBoolField(TEXT("net"), Function->HasAnyFunctionFlags(FUNC_Net));
		FlagsObject->SetBoolField(TEXT("net_server"), Function->HasAnyFunctionFlags(FUNC_NetServer));
		FlagsObject->SetBoolField(TEXT("net_client"), Function->HasAnyFunctionFlags(FUNC_NetClient));
		FlagsObject->SetBoolField(TEXT("net_multicast"), Function->HasAnyFunctionFlags(FUNC_NetMulticast));
		FlagsObject->SetBoolField(TEXT("reliable"), Function->HasAnyFunctionFlags(FUNC_NetReliable));
		FlagsObject->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
		FlagsObject->SetBoolField(TEXT("const"), Function->HasAnyFunctionFlags(FUNC_Const));
		FlagsObject->SetBoolField(TEXT("exec"), Function->HasAnyFunctionFlags(FUNC_Exec));
		FunctionObject->SetObjectField(TEXT("flags"), FlagsObject);

		TArray<TSharedPtr<FJsonValue>> Parameters;
		FString ReturnType = TEXT("void");
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			const FProperty* ParameterProperty = *It;
			if (!ParameterProperty->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}

			if (ParameterProperty->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnType = ParameterProperty->GetCPPType();
				continue;
			}

			Parameters.Add(MakeShared<FJsonValueObject>(BuildFunctionParameterObject(ParameterProperty)));
		}

		FunctionObject->SetStringField(TEXT("return_type"), ReturnType);
		FunctionObject->SetNumberField(TEXT("parameter_count"), Parameters.Num());
		FunctionObject->SetArrayField(TEXT("parameters"), Parameters);

		if (bIncludeMetadata)
		{
			const TArray<FString> MetadataKeys = {
				TEXT("DisplayName"),
				TEXT("ToolTip"),
				TEXT("DeprecatedFunction"),
				TEXT("DeprecationMessage"),
				TEXT("ScriptName")
			};
			if (TSharedPtr<FJsonObject> MetadataObject = BuildSelectedMetadata(Function, MetadataKeys))
			{
				FunctionObject->SetObjectField(TEXT("metadata"), MetadataObject);
			}
		}

		return FunctionObject;
	}

	void AddTypeMembers(
		const UObject* Object,
		TSharedPtr<FJsonObject>& ContractObject,
		const bool bIncludeProperties,
		const bool bIncludeFunctions,
		const bool bIncludeMetadata,
		const bool bIncludeInherited,
		const int32 MemberLimit)
	{
		if (const UClass* Class = Cast<UClass>(Object))
		{
			if (bIncludeProperties)
			{
				TArray<TSharedPtr<FJsonValue>> PropertyValues;
				const EFieldIteratorFlags::SuperClassFlags SuperFlags =
					bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
				for (TFieldIterator<FProperty> It(Class, SuperFlags); It; ++It)
				{
					if (PropertyValues.Num() >= MemberLimit)
					{
						break;
					}
					PropertyValues.Add(MakeShared<FJsonValueObject>(BuildPropertyObject(*It, bIncludeMetadata)));
				}
				ContractObject->SetArrayField(TEXT("properties"), PropertyValues);
				ContractObject->SetNumberField(TEXT("returned_property_count"), PropertyValues.Num());
			}

			if (bIncludeFunctions)
			{
				TArray<TSharedPtr<FJsonValue>> FunctionValues;
				const EFieldIteratorFlags::SuperClassFlags SuperFlags =
					bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
				for (TFieldIterator<UFunction> It(Class, SuperFlags); It; ++It)
				{
					if (FunctionValues.Num() >= MemberLimit)
					{
						break;
					}
					FunctionValues.Add(MakeShared<FJsonValueObject>(BuildFunctionObject(*It, bIncludeMetadata)));
				}
				ContractObject->SetArrayField(TEXT("functions"), FunctionValues);
				ContractObject->SetNumberField(TEXT("returned_function_count"), FunctionValues.Num());
			}

			return;
		}

		if (const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(Object))
		{
			if (bIncludeProperties)
			{
				TArray<TSharedPtr<FJsonValue>> PropertyValues;
				const EFieldIteratorFlags::SuperClassFlags SuperFlags =
					bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
				for (TFieldIterator<FProperty> It(ScriptStruct, SuperFlags); It; ++It)
				{
					if (PropertyValues.Num() >= MemberLimit)
					{
						break;
					}
					PropertyValues.Add(MakeShared<FJsonValueObject>(BuildPropertyObject(*It, bIncludeMetadata)));
				}
				ContractObject->SetArrayField(TEXT("properties"), PropertyValues);
				ContractObject->SetNumberField(TEXT("returned_property_count"), PropertyValues.Num());
			}

			return;
		}

		if (const UEnum* Enum = Cast<UEnum>(Object))
		{
			TArray<TSharedPtr<FJsonValue>> EnumeratorValues;
			const int32 EnumeratorCount = FMath::Min(Enum->NumEnums(), MemberLimit);
			for (int32 Index = 0; Index < EnumeratorCount; ++Index)
			{
				TSharedPtr<FJsonObject> EnumeratorObject = MakeShared<FJsonObject>();
				EnumeratorObject->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
				EnumeratorObject->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
				EnumeratorObject->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
				EnumeratorValues.Add(MakeShared<FJsonValueObject>(EnumeratorObject));
			}

			ContractObject->SetArrayField(TEXT("enumerators"), EnumeratorValues);
			ContractObject->SetNumberField(TEXT("returned_enumerator_count"), EnumeratorValues.Num());
			ContractObject->SetStringField(TEXT("underlying_cpp_type"), Enum->CppType.IsEmpty() ? TEXT("int32") : Enum->CppType);
		}
	}

	void CollectMatchingObjects(
		TArray<const UObject*>& OutObjects,
		const ECppReflectionKindFilter KindFilter,
		const ECppReflectionModuleScope ModuleScope,
		const FString& NameFilter,
		const FString& ModuleFilter,
		const FCppModuleLocator& ModuleLocator)
	{
		if (KindFilter == ECppReflectionKindFilter::All || KindFilter == ECppReflectionKindFilter::Class)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				const UClass* Class = *It;
				if (MatchesObjectFilters(Class, KindFilter, ModuleScope, NameFilter, ModuleFilter, ModuleLocator))
				{
					OutObjects.Add(Class);
				}
			}
		}

		if (KindFilter == ECppReflectionKindFilter::All || KindFilter == ECppReflectionKindFilter::Struct)
		{
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				const UScriptStruct* ScriptStruct = *It;
				if (MatchesObjectFilters(ScriptStruct, KindFilter, ModuleScope, NameFilter, ModuleFilter, ModuleLocator))
				{
					OutObjects.Add(ScriptStruct);
				}
			}
		}

		if (KindFilter == ECppReflectionKindFilter::All || KindFilter == ECppReflectionKindFilter::Enum)
		{
			for (TObjectIterator<UEnum> It; It; ++It)
			{
				const UEnum* Enum = *It;
				if (MatchesObjectFilters(Enum, KindFilter, ModuleScope, NameFilter, ModuleFilter, ModuleLocator))
				{
					OutObjects.Add(Enum);
				}
			}
		}

		OutObjects.Sort([](const UObject& Left, const UObject& Right)
		{
			const FString LeftModule = GetModuleNameForObject(&Left);
			const FString RightModule = GetModuleNameForObject(&Right);
			if (LeftModule != RightModule)
			{
				return LeftModule < RightModule;
			}

			return GetCppSymbolName(&Left) < GetCppSymbolName(&Right);
		});
	}

	const UObject* ResolveObjectIdentifier(
		const FString& SymbolIdentifier,
		const ECppReflectionKindFilter KindFilter,
		const ECppReflectionModuleScope ModuleScope,
		const FString& ModuleFilter,
		const FCppModuleLocator& ModuleLocator,
		FString& OutError)
	{
		TArray<const UObject*> MatchingObjects;
		CollectMatchingObjects(
			MatchingObjects,
			KindFilter,
			ModuleScope,
			FString(),
			ModuleFilter,
			ModuleLocator);

		const FString NormalizedIdentifier = NormalizeFilterValue(SymbolIdentifier);
		TArray<const UObject*> ExactPathMatches;
		TArray<const UObject*> NameMatches;

		for (const UObject* Object : MatchingObjects)
		{
			if (!Object)
			{
				continue;
			}

			if (NormalizeFilterValue(Object->GetPathName()) == NormalizedIdentifier)
			{
				ExactPathMatches.Add(Object);
				continue;
			}

			if (NormalizeFilterValue(Object->GetName()) == NormalizedIdentifier
				|| NormalizeFilterValue(GetCppSymbolName(Object)) == NormalizedIdentifier)
			{
				NameMatches.Add(Object);
			}
		}

		if (ExactPathMatches.Num() == 1)
		{
			return ExactPathMatches[0];
		}

		if (ExactPathMatches.Num() > 1)
		{
			OutError = FString::Printf(TEXT("Multiple reflected contracts matched exact path '%s'."), *SymbolIdentifier);
			return nullptr;
		}

		if (NameMatches.Num() == 1)
		{
			return NameMatches[0];
		}

		if (NameMatches.Num() > 1)
		{
			TArray<FString> CandidatePaths;
			for (const UObject* Match : NameMatches)
			{
				CandidatePaths.Add(Match->GetPathName());
			}

			OutError = FString::Printf(
				TEXT("Reflected contract '%s' is ambiguous. Use the exact path. Candidates: %s"),
				*SymbolIdentifier,
				*FString::Join(CandidatePaths, TEXT(", ")));
			return nullptr;
		}

		OutError = FString::Printf(TEXT("Could not find reflected contract '%s' in the requested scope."), *SymbolIdentifier);
		return nullptr;
	}

	const FProperty* ResolveDeclaredPropertyIdentifier(
		const UObject* ContractObject,
		const FString& MemberName,
		FString& OutError)
	{
		const FString NormalizedMember = NormalizeFilterValue(MemberName);

		if (const UClass* Class = Cast<UClass>(ContractObject))
		{
			TArray<const FProperty*> Matches;
			for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				if (NormalizeFilterValue((*It)->GetName()) == NormalizedMember)
				{
					Matches.Add(*It);
				}
			}

			if (Matches.Num() == 1)
			{
				return Matches[0];
			}

			if (Matches.Num() > 1)
			{
				OutError = FString::Printf(
					TEXT("Property '%s' is ambiguous on contract '%s'."),
					*MemberName,
					*GetCppSymbolName(ContractObject));
				return nullptr;
			}

			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				if (NormalizeFilterValue((*It)->GetName()) == NormalizedMember)
				{
					OutError = FString::Printf(
						TEXT("Member '%s' resolves to a UFUNCTION on '%s'. This mutation lane only supports UPROPERTY metadata upserts."),
						*MemberName,
						*GetCppSymbolName(ContractObject));
					return nullptr;
				}
			}

			OutError = FString::Printf(
				TEXT("Could not find a declared reflected property '%s' on contract '%s'. Inherited members are intentionally excluded from this bounded mutation lane."),
				*MemberName,
				*GetCppSymbolName(ContractObject));
			return nullptr;
		}

		if (const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(ContractObject))
		{
			TArray<const FProperty*> Matches;
			for (TFieldIterator<FProperty> It(ScriptStruct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				if (NormalizeFilterValue((*It)->GetName()) == NormalizedMember)
				{
					Matches.Add(*It);
				}
			}

			if (Matches.Num() == 1)
			{
				return Matches[0];
			}

			if (Matches.Num() > 1)
			{
				OutError = FString::Printf(
					TEXT("Property '%s' is ambiguous on contract '%s'."),
					*MemberName,
					*GetCppSymbolName(ContractObject));
				return nullptr;
			}

			OutError = FString::Printf(
				TEXT("Could not find a declared reflected property '%s' on struct '%s'. Inherited members are intentionally excluded from this bounded mutation lane."),
				*MemberName,
				*GetCppSymbolName(ContractObject));
			return nullptr;
		}

		OutError = FString::Printf(
			TEXT("Contract '%s' is not a reflected class/struct. This mutation lane only supports UPROPERTY metadata on plugin-owned UCLASS/USTRUCT declarations."),
			*GetCppSymbolName(ContractObject));
		return nullptr;
	}

	bool DoesClassHierarchyAlreadyDefineProperty(const UClass* Class, const FString& MemberName)
	{
		if (!Class)
		{
			return false;
		}

		const FString NormalizedMember = NormalizeFilterValue(MemberName);
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			if (NormalizeFilterValue((*It)->GetName()) == NormalizedMember)
			{
				return true;
			}
		}

		return false;
	}

	TSharedPtr<FJsonObject> BuildBuildSyncObject(const FUnrealClaudeBuildSyncStatus& Status)
	{
		TSharedPtr<FJsonObject> BuildObject = MakeShared<FJsonObject>();
		BuildObject->SetBoolField(TEXT("binary_present"), Status.bBinaryPresent);
		BuildObject->SetBoolField(TEXT("fresh"), Status.bFresh);
		BuildObject->SetStringField(TEXT("detail"), Status.Detail);
		BuildObject->SetStringField(TEXT("binary_path"), Status.BinaryPath);
		BuildObject->SetStringField(TEXT("latest_source_path"), Status.LatestSourcePath);
		BuildObject->SetStringField(TEXT("binary_timestamp"), Status.BinaryTimestamp == FDateTime::MinValue() ? FString() : Status.BinaryTimestamp.ToIso8601());
		BuildObject->SetStringField(TEXT("latest_source_timestamp"), Status.LatestSourceTimestamp == FDateTime::MinValue() ? FString() : Status.LatestSourceTimestamp.ToIso8601());
		return BuildObject;
	}

	FString SerializeJsonPretty(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		return Output;
	}

	bool LoadJsonObjectFromPath(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
	{
		OutObject.Reset();

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *Path))
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool LoadOperationResultObjectFromPath(
		const FString& Path,
		const FString& ExpectedOperation,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError)
	{
		OutError.Reset();
		OutObject.Reset();

		if (Path.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}

		if (!LoadJsonObjectFromPath(Path, OutObject))
		{
			OutError = FString::Printf(TEXT("Could not load JSON object from '%s'."), *Path);
			return false;
		}

		FString OperationName;
		OutObject->TryGetStringField(TEXT("operation"), OperationName);
		if (!ExpectedOperation.IsEmpty() && !OperationName.Equals(ExpectedOperation, ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(
				TEXT("JSON object '%s' is not a %s result."),
				*Path,
				*ExpectedOperation);
			return false;
		}

		return true;
	}

	void SetStringArrayField(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName,
		const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	}

	FString ExtractObjectStringField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(FieldName, Value);
		}
		return Value;
	}

	TArray<FString> ExtractStringArrayField(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName)
	{
		TArray<FString> Values;
		if (!Object.IsValid())
		{
			return Values;
		}

		const TArray<TSharedPtr<FJsonValue>>* JsonValues = nullptr;
		if (!Object->TryGetArrayField(FieldName, JsonValues) || !JsonValues)
		{
			return Values;
		}

		for (const TSharedPtr<FJsonValue>& JsonValue : *JsonValues)
		{
			FString StringValue;
			if (JsonValue.IsValid() && JsonValue->TryGetString(StringValue))
			{
				Values.Add(StringValue);
			}
		}

		return Values;
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

	FString JoinQuotedLabels(const TArray<FString>& Labels)
	{
		if (Labels.Num() == 0)
		{
			return FString();
		}

		TArray<FString> Quoted;
		Quoted.Reserve(Labels.Num());
		for (const FString& Label : Labels)
		{
			Quoted.Add(FString::Printf(TEXT("`%s`"), *Label));
		}

		return FString::Join(Quoted, TEXT(", "));
	}

	FString DetermineDeclarationEvidenceBundlePrimaryState(const TSharedPtr<FJsonObject>& BundleObject)
	{
		const TSharedPtr<FJsonObject> ReflectionReadbackObject = GetObjectFieldOrNull(BundleObject, TEXT("reflection_readback"));
		if (ReflectionReadbackObject.IsValid())
		{
			const FString ReadbackClassification = ExtractObjectStringField(ReflectionReadbackObject, TEXT("readback_classification"));
			if (ReadbackClassification == TEXT("post_revert_property_absent"))
			{
				return TEXT("revert_closeout_cycle");
			}
			if (ReadbackClassification == TEXT("post_build_property_present"))
			{
				return TEXT("success_path_authoring_cycle");
			}
		}

		if (GetObjectFieldOrNull(BundleObject, TEXT("revert_receipt")).IsValid())
		{
			return TEXT("revert_closeout_cycle");
		}
		if (GetObjectFieldOrNull(BundleObject, TEXT("failed_build_diagnostic")).IsValid())
		{
			return TEXT("failed_build_cycle");
		}
		if (ReflectionReadbackObject.IsValid())
		{
			return TEXT("success_path_authoring_cycle");
		}

		const TArray<FString> StateLabels = ExtractStringArrayField(BundleObject, TEXT("bundle_state_labels"));
		return StateLabels.Num() > 0 ? StateLabels[0] : TEXT("receipt_backed_evidence_bundle");
	}

	FString DeclarationEvidenceBundleStateTitle(const FString& PrimaryState)
	{
		if (PrimaryState == TEXT("success_path_authoring_cycle"))
		{
			return TEXT("Success Path Authoring Cycle");
		}
		if (PrimaryState == TEXT("failed_build_cycle"))
		{
			return TEXT("Failed-Build Cycle");
		}
		if (PrimaryState == TEXT("revert_closeout_cycle"))
		{
			return TEXT("Revert Closeout Cycle");
		}
		return TEXT("Receipt-Backed Evidence Bundle");
	}

	void AppendMarkdownSection(FString& Markdown, const FString& Title, const TArray<FString>& Items)
	{
		if (Items.Num() == 0)
		{
			return;
		}

		Markdown += FString::Printf(TEXT("## %s\n\n"), *Title);
		for (const FString& Item : Items)
		{
			Markdown += FString::Printf(TEXT("- %s\n"), *Item);
		}
		Markdown += TEXT("\n");
	}

	TArray<FString> BuildDeclarationEvidenceBundleEvidenceClasses(const TSharedPtr<FJsonObject>& BundleObject)
	{
		TArray<FString> EvidenceClasses = {
			TEXT("cpp_reflected_contract"),
			TEXT("bounded_declaration_evidence"),
			TEXT("mutation_receipt"),
			TEXT("compile_handshake")
		};

		if (GetObjectFieldOrNull(BundleObject, TEXT("reflection_readback")).IsValid())
		{
			EvidenceClasses.Add(TEXT("reflection_readback"));
		}
		if (GetObjectFieldOrNull(BundleObject, TEXT("failed_build_diagnostic")).IsValid())
		{
			EvidenceClasses.Add(TEXT("build_failure_diagnostic"));
		}
		if (GetObjectFieldOrNull(BundleObject, TEXT("revert_receipt")).IsValid())
		{
			EvidenceClasses.Add(TEXT("revert_closeout"));
		}

		return EvidenceClasses;
	}

	FUnrealClaudeReportTruthSummary BuildDeclarationEvidenceBundleTruthSummary(const TSharedPtr<FJsonObject>& BundleObject)
	{
		FUnrealClaudeReportTruthSummary TruthSummary;
		const FString ContractCppName = ExtractObjectStringField(BundleObject, TEXT("contract_cpp_name"));
		const FString PropertyName = ExtractObjectStringField(BundleObject, TEXT("property_name"));
		const FString PrimaryState = DetermineDeclarationEvidenceBundlePrimaryState(BundleObject);
		const TArray<FString> StateLabels = ExtractStringArrayField(BundleObject, TEXT("bundle_state_labels"));

		TruthSummary.Inspected.Add(FString::Printf(
			TEXT("Bounded declaration evidence bundle built for %s.%s."),
			ContractCppName.IsEmpty() ? TEXT("contract") : *ContractCppName,
			PropertyName.IsEmpty() ? TEXT("property") : *PropertyName));
		TruthSummary.Inspected.Add(FString::Printf(TEXT("Primary cycle state: %s."), *PrimaryState));
		if (StateLabels.Num() > 0)
		{
			TruthSummary.Inspected.Add(FString::Printf(TEXT("Observed state labels: %s."), *FString::Join(StateLabels, TEXT(", "))));
		}

		const TSharedPtr<FJsonObject> ReflectionReadbackObject = GetObjectFieldOrNull(BundleObject, TEXT("reflection_readback"));
		if (ReflectionReadbackObject.IsValid())
		{
			const FString ReadbackClassification = ExtractObjectStringField(ReflectionReadbackObject, TEXT("readback_classification"));
			if (!ReadbackClassification.IsEmpty())
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Reflection readback classification: %s."), *ReadbackClassification));
			}
		}

		const TSharedPtr<FJsonObject> FailedBuildObject = GetObjectFieldOrNull(BundleObject, TEXT("failed_build_diagnostic"));
		if (FailedBuildObject.IsValid())
		{
			const FString LinkageConfidence = ExtractObjectStringField(FailedBuildObject, TEXT("diagnostic_linkage_confidence"));
			if (!LinkageConfidence.IsEmpty())
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Failed-build diagnostic linkage confidence: %s."), *LinkageConfidence));
			}

			const FString RecommendedCloseoutStatus = ExtractObjectStringField(FailedBuildObject, TEXT("recommended_closeout_status"));
			if (!RecommendedCloseoutStatus.IsEmpty())
			{
				TruthSummary.Inspected.Add(FString::Printf(TEXT("Recommended closeout status: %s."), *RecommendedCloseoutStatus));
			}

			const TSharedPtr<FJsonObject> PresentationObject = GetObjectFieldOrNull(FailedBuildObject, TEXT("presentation"));
			if (PresentationObject.IsValid())
			{
				const FString FirstMatchingDisplay = ExtractObjectStringField(PresentationObject, TEXT("first_matching_error_display"));
				if (!FirstMatchingDisplay.IsEmpty())
				{
					TruthSummary.Inspected.Add(FString::Printf(TEXT("Readable failed-build display: %s."), *FirstMatchingDisplay));
				}
			}
		}

		const TSharedPtr<FJsonObject> CompileHandshakeObject = GetObjectFieldOrNull(BundleObject, TEXT("compile_handshake"));
		if (CompileHandshakeObject.IsValid())
		{
			const TSharedPtr<FJsonObject> RevertHandshakeObject = GetObjectFieldOrNull(CompileHandshakeObject, TEXT("revert"));
			if (RevertHandshakeObject.IsValid())
			{
				const FString RevertStatus = ExtractObjectStringField(RevertHandshakeObject, TEXT("status"));
				if (!RevertStatus.IsEmpty())
				{
					TruthSummary.Inspected.Add(FString::Printf(TEXT("Revert compile-handshake status: %s."), *RevertStatus));
				}
			}
		}

		TruthSummary.Limited.Add(TEXT("This artifact is limited to the already accepted bounded reflected bool-property declaration lane."));
		TruthSummary.Limited.Add(TEXT("It does not widen declaration families, does not replace rebuild/readback proof, and does not allow arbitrary native source editing."));
		TruthSummary.Limited.Add(TEXT("Arbitrary .cpp implementation-body editing remains forbidden."));

		if (FailedBuildObject.IsValid())
		{
			const FString LinkageConfidence = ExtractObjectStringField(FailedBuildObject, TEXT("diagnostic_linkage_confidence"));
			if (!LinkageConfidence.IsEmpty() && LinkageConfidence != TEXT("receipt_snippet_or_member_linked"))
			{
				TruthSummary.Limited.Add(FString::Printf(TEXT("The strongest failed-build linkage tier remains unproven here; current accepted tier is %s."), *LinkageConfidence));
			}
		}

		return TruthSummary;
	}

	TSharedPtr<FJsonObject> BuildDeclarationEvidenceBundleExtraMetadata(const TSharedPtr<FJsonObject>& BundleObject)
	{
		TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
		const FString PrimaryState = DetermineDeclarationEvidenceBundlePrimaryState(BundleObject);
		const TArray<FString> StateLabels = ExtractStringArrayField(BundleObject, TEXT("bundle_state_labels"));

		Metadata->SetStringField(TEXT("source_tool"), TEXT("cpp_reflection"));
		Metadata->SetStringField(TEXT("source_operation"), TEXT("build_reflected_property_declaration_evidence_bundle"));
		Metadata->SetStringField(TEXT("primary_cycle_state"), PrimaryState);
		Metadata->SetStringField(TEXT("bundle_schema_version"), ExtractObjectStringField(BundleObject, TEXT("bundle_schema_version")));
		Metadata->SetStringField(TEXT("mutation_lane"), ExtractObjectStringField(BundleObject, TEXT("mutation_lane")));
		Metadata->SetStringField(TEXT("contract_cpp_name"), ExtractObjectStringField(BundleObject, TEXT("contract_cpp_name")));
		Metadata->SetStringField(TEXT("contract_path"), ExtractObjectStringField(BundleObject, TEXT("contract_path")));
		Metadata->SetStringField(TEXT("property_name"), ExtractObjectStringField(BundleObject, TEXT("property_name")));
		Metadata->SetStringField(TEXT("header_path"), ExtractObjectStringField(BundleObject, TEXT("header_path")));
		Metadata->SetStringField(TEXT("truth_boundary"), ExtractObjectStringField(BundleObject, TEXT("truth_boundary")));
		Metadata->SetStringField(TEXT("evidence_bundle_path"), ExtractObjectStringField(BundleObject, TEXT("evidence_bundle_path")));
		SetStringArrayField(Metadata, TEXT("bundle_state_labels"), StateLabels);

		const TSharedPtr<FJsonObject> NarrowedTruthBoundary = GetObjectFieldOrNull(BundleObject, TEXT("narrowed_truth_boundary"));
		if (NarrowedTruthBoundary.IsValid())
		{
			Metadata->SetObjectField(TEXT("narrowed_truth_boundary"), NarrowedTruthBoundary);
		}

		const TSharedPtr<FJsonObject> ReflectionReadbackObject = GetObjectFieldOrNull(BundleObject, TEXT("reflection_readback"));
		if (ReflectionReadbackObject.IsValid())
		{
			Metadata->SetStringField(TEXT("readback_classification"), ExtractObjectStringField(ReflectionReadbackObject, TEXT("readback_classification")));
		}

		const TSharedPtr<FJsonObject> FailedBuildObject = GetObjectFieldOrNull(BundleObject, TEXT("failed_build_diagnostic"));
		if (FailedBuildObject.IsValid())
		{
			Metadata->SetStringField(TEXT("diagnostic_linkage_confidence"), ExtractObjectStringField(FailedBuildObject, TEXT("diagnostic_linkage_confidence")));
			Metadata->SetStringField(TEXT("recommended_closeout_status"), ExtractObjectStringField(FailedBuildObject, TEXT("recommended_closeout_status")));

			const TSharedPtr<FJsonObject> PresentationObject = GetObjectFieldOrNull(FailedBuildObject, TEXT("presentation"));
			if (PresentationObject.IsValid())
			{
				Metadata->SetStringField(TEXT("first_matching_error_display"), ExtractObjectStringField(PresentationObject, TEXT("first_matching_error_display")));
			}
		}

		const TSharedPtr<FJsonObject> CompileHandshakeObject = GetObjectFieldOrNull(BundleObject, TEXT("compile_handshake"));
		if (CompileHandshakeObject.IsValid())
		{
			const TSharedPtr<FJsonObject> RevertHandshakeObject = GetObjectFieldOrNull(CompileHandshakeObject, TEXT("revert"));
			if (RevertHandshakeObject.IsValid())
			{
				Metadata->SetStringField(TEXT("compile_handshake_revert_status"), ExtractObjectStringField(RevertHandshakeObject, TEXT("status")));
			}
		}

		return Metadata;
	}

	FString BuildDeclarationEvidenceBundleMarkdown(const TSharedPtr<FJsonObject>& BundleObject, const FString& ResultMessage)
	{
		const FString ContractCppName = ExtractObjectStringField(BundleObject, TEXT("contract_cpp_name"));
		const FString PropertyName = ExtractObjectStringField(BundleObject, TEXT("property_name"));
		const FString PrimaryState = DetermineDeclarationEvidenceBundlePrimaryState(BundleObject);
		const TArray<FString> StateLabels = ExtractStringArrayField(BundleObject, TEXT("bundle_state_labels"));
		FString Markdown = FString::Printf(
			TEXT("# %s\n\n"),
			*FString::Printf(TEXT("Bounded Reflected Declaration Evidence Bundle - %s"), *DeclarationEvidenceBundleStateTitle(PrimaryState)));

		Markdown += TEXT("## Summary\n\n");
		Markdown += TEXT("- Source tool: `cpp_reflection`\n");
		Markdown += TEXT("- Operation: `build_reflected_property_declaration_evidence_bundle`\n");
		if (!ContractCppName.IsEmpty())
		{
			Markdown += FString::Printf(TEXT("- Contract: `%s`\n"), *ContractCppName);
		}
		if (!PropertyName.IsEmpty())
		{
			Markdown += FString::Printf(TEXT("- Property: `%s`\n"), *PropertyName);
		}
		Markdown += FString::Printf(TEXT("- Bundle schema: `%s`\n"), *ExtractObjectStringField(BundleObject, TEXT("bundle_schema_version")));
		Markdown += FString::Printf(TEXT("- Mutation lane: `%s`\n"), *ExtractObjectStringField(BundleObject, TEXT("mutation_lane")));
		Markdown += FString::Printf(TEXT("- Primary cycle state: `%s`\n"), *PrimaryState);
		if (StateLabels.Num() > 0)
		{
			Markdown += FString::Printf(TEXT("- State labels: %s\n"), *JoinQuotedLabels(StateLabels));
		}
		const FString EvidenceBundlePath = ExtractObjectStringField(BundleObject, TEXT("evidence_bundle_path"));
		if (!EvidenceBundlePath.IsEmpty())
		{
			Markdown += FString::Printf(TEXT("- Evidence bundle path: `%s`\n"), *EvidenceBundlePath);
		}
		Markdown += FString::Printf(TEXT("- Result: %s\n\n"), *ResultMessage);

		TArray<FString> ObservedStateLines;
		const TSharedPtr<FJsonObject> ReflectionReadbackObject = GetObjectFieldOrNull(BundleObject, TEXT("reflection_readback"));
		if (ReflectionReadbackObject.IsValid())
		{
			const FString ReadbackClassification = ExtractObjectStringField(ReflectionReadbackObject, TEXT("readback_classification"));
			if (!ReadbackClassification.IsEmpty())
			{
				ObservedStateLines.Add(FString::Printf(TEXT("Reflection readback classification: `%s`"), *ReadbackClassification));
			}
		}

		const TSharedPtr<FJsonObject> FailedBuildObject = GetObjectFieldOrNull(BundleObject, TEXT("failed_build_diagnostic"));
		if (FailedBuildObject.IsValid())
		{
			const FString LinkageConfidence = ExtractObjectStringField(FailedBuildObject, TEXT("diagnostic_linkage_confidence"));
			if (!LinkageConfidence.IsEmpty())
			{
				ObservedStateLines.Add(FString::Printf(TEXT("Failed-build diagnostic linkage confidence: `%s`"), *LinkageConfidence));
			}

			const FString RecommendedCloseoutStatus = ExtractObjectStringField(FailedBuildObject, TEXT("recommended_closeout_status"));
			if (!RecommendedCloseoutStatus.IsEmpty())
			{
				ObservedStateLines.Add(FString::Printf(TEXT("Recommended closeout status: `%s`"), *RecommendedCloseoutStatus));
			}

			const TSharedPtr<FJsonObject> PresentationObject = GetObjectFieldOrNull(FailedBuildObject, TEXT("presentation"));
			if (PresentationObject.IsValid())
			{
				const FString FirstMatchingDisplay = ExtractObjectStringField(PresentationObject, TEXT("first_matching_error_display"));
				if (!FirstMatchingDisplay.IsEmpty())
				{
					ObservedStateLines.Add(FString::Printf(TEXT("Readable failed-build display: `%s`"), *FirstMatchingDisplay));
				}
			}
		}

		const TSharedPtr<FJsonObject> CompileHandshakeObject = GetObjectFieldOrNull(BundleObject, TEXT("compile_handshake"));
		if (CompileHandshakeObject.IsValid())
		{
			const TSharedPtr<FJsonObject> RevertHandshakeObject = GetObjectFieldOrNull(CompileHandshakeObject, TEXT("revert"));
			if (RevertHandshakeObject.IsValid())
			{
				const FString RevertStatus = ExtractObjectStringField(RevertHandshakeObject, TEXT("status"));
				if (!RevertStatus.IsEmpty())
				{
					ObservedStateLines.Add(FString::Printf(TEXT("Revert compile-handshake status: `%s`"), *RevertStatus));
				}
			}
		}
		AppendMarkdownSection(Markdown, TEXT("Observed State"), ObservedStateLines);

		const TSharedPtr<FJsonObject> SourcesObject = GetObjectFieldOrNull(BundleObject, TEXT("evidence_sources"));
		if (SourcesObject.IsValid())
		{
			TArray<FString> SourceLines;
			for (const FString& FieldName : { TEXT("receipt_path"), TEXT("preview_result_path"), TEXT("failed_build_result_path"), TEXT("revert_result_path"), TEXT("reflection_readback_path") })
			{
				const FString SourcePath = ExtractObjectStringField(SourcesObject, FieldName);
				if (!SourcePath.IsEmpty())
				{
					SourceLines.Add(FString::Printf(TEXT("%s: `%s`"), *FieldName, *SourcePath));
				}
			}
			AppendMarkdownSection(Markdown, TEXT("Evidence Sources"), SourceLines);
		}

		Markdown += TEXT("## Truth Boundary\n\n");
		const FString TruthBoundary = ExtractObjectStringField(BundleObject, TEXT("truth_boundary"));
		if (!TruthBoundary.IsEmpty())
		{
			Markdown += FString::Printf(TEXT("- %s\n"), *TruthBoundary);
		}

		const TSharedPtr<FJsonObject> NarrowedTruthBoundary = GetObjectFieldOrNull(BundleObject, TEXT("narrowed_truth_boundary"));
		if (NarrowedTruthBoundary.IsValid())
		{
			const TArray<FString> NonClaims = ExtractStringArrayField(NarrowedTruthBoundary, TEXT("non_claims"));
			for (const FString& NonClaim : NonClaims)
			{
				Markdown += FString::Printf(TEXT("- Non-claim: `%s`\n"), *NonClaim);
			}
		}
		Markdown += TEXT("\n");
		return Markdown;
	}

	FMCPToolResult MaybeAttachDeclarationEvidenceBundleReportArtifact(
		FMCPToolResult Result,
		const bool bExportReport,
		const FString& CustomReportName,
		const FString& CustomReportSlug)
	{
		if (!bExportReport || !Result.bSuccess || !Result.Data.IsValid())
		{
			return Result;
		}

		const FString ContractCppName = ExtractObjectStringField(Result.Data, TEXT("contract_cpp_name"));
		const FString PropertyName = ExtractObjectStringField(Result.Data, TEXT("property_name"));
		const FString PrimaryState = DetermineDeclarationEvidenceBundlePrimaryState(Result.Data);
		FUnrealClaudeReportExportRequest ExportRequest;
		ExportRequest.ReportName = !CustomReportName.TrimStartAndEnd().IsEmpty()
			? CustomReportName
			: FString::Printf(
				TEXT("%s - %s.%s"),
				*DeclarationEvidenceBundleStateTitle(PrimaryState),
				ContractCppName.IsEmpty() ? TEXT("contract") : *ContractCppName,
				PropertyName.IsEmpty() ? TEXT("property") : *PropertyName);
		ExportRequest.ReportSlug = !CustomReportSlug.TrimStartAndEnd().IsEmpty()
			? CustomReportSlug
			: FString::Printf(
				TEXT("cpp_reflection_%s_%s_%s"),
				*PrimaryState,
				ContractCppName.IsEmpty() ? TEXT("contract") : *ContractCppName,
				PropertyName.IsEmpty() ? TEXT("property") : *PropertyName);
		ExportRequest.Markdown = BuildDeclarationEvidenceBundleMarkdown(Result.Data, Result.Message);
		ExportRequest.SummaryText = FString::Printf(TEXT("%s | state=%s"), *Result.Message, *PrimaryState);
		ExportRequest.RunKind = TEXT("cpp_reflected_declaration_evidence_bundle");
		ExportRequest.ExecutionMode = TEXT("mixed");
		ExportRequest.ToolNames = { TEXT("cpp_reflection") };
		ExportRequest.EvidenceClasses = BuildDeclarationEvidenceBundleEvidenceClasses(Result.Data);
		ExportRequest.TruthSummary = BuildDeclarationEvidenceBundleTruthSummary(Result.Data);
		ExportRequest.ExtraMetadata = BuildDeclarationEvidenceBundleExtraMetadata(Result.Data);

		FUnrealClaudeReportExportResult ExportResult;
		if (!FUnrealClaudeReportArtifacts::ExportReport(ExportRequest, ExportResult))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("%s Report export failed: %s"), *Result.Message, *ExportResult.ErrorMessage));
		}

		TSharedPtr<FJsonObject> ArtifactObject = MakeShared<FJsonObject>();
		ArtifactObject->SetStringField(TEXT("report_id"), ExportResult.ReportId);
		ArtifactObject->SetStringField(TEXT("markdown_path"), ExportResult.MarkdownPath);
		ArtifactObject->SetStringField(TEXT("summary_path"), ExportResult.SummaryPath);
		ArtifactObject->SetStringField(TEXT("export_status"), ExportResult.ExportStatus);
		ArtifactObject->SetBoolField(TEXT("roundtrip_exact"), ExportResult.bRoundTripExact);
		ArtifactObject->SetStringField(TEXT("status_tool"), TEXT("report_artifact_status"));
		ArtifactObject->SetStringField(TEXT("status_query_report_id"), ExportResult.ReportId);
		Result.Data->SetObjectField(TEXT("report_artifact"), ArtifactObject);
		Result.Message = FString::Printf(TEXT("%s | report=%s"), *Result.Message, *ExportResult.ReportId);
		return Result;
	}

	FString ExtractDiagnosticCodeLabel(const TSharedPtr<FJsonObject>& ErrorObject)
	{
		FString CodeAndDetail;
		if (!ErrorObject.IsValid() || !ErrorObject->TryGetStringField(TEXT("code_and_detail"), CodeAndDetail))
		{
			return FString();
		}

		FString CodeOnly = CodeAndDetail.TrimStartAndEnd();
		int32 ColonIndex = INDEX_NONE;
		if (CodeOnly.FindChar(TEXT(':'), ColonIndex))
		{
			CodeOnly = CodeOnly.Left(ColonIndex).TrimStartAndEnd();
		}

		int32 SpaceIndex = INDEX_NONE;
		if (CodeOnly.FindChar(TEXT(' '), SpaceIndex))
		{
			CodeOnly = CodeOnly.Left(SpaceIndex).TrimStartAndEnd();
		}

		return CodeOnly;
	}

	FString BuildReadableDiagnosticSummary(const TSharedPtr<FJsonObject>& ErrorObject)
	{
		if (!ErrorObject.IsValid())
		{
			return FString();
		}

		const FString Severity = ExtractObjectStringField(ErrorObject, TEXT("severity"));
		const FString FilePath = ExtractObjectStringField(ErrorObject, TEXT("file"));
		const FString Line = ExtractObjectStringField(ErrorObject, TEXT("line"));
		const FString Column = ExtractObjectStringField(ErrorObject, TEXT("column"));
		const FString Code = ExtractDiagnosticCodeLabel(ErrorObject);
		const FString FileName = FilePath.IsEmpty() ? TEXT("unknown_file") : FPaths::GetCleanFilename(FilePath);

		FString Location = FileName;
		if (!Line.IsEmpty() && !Column.IsEmpty())
		{
			Location = FString::Printf(TEXT("%s(%s,%s)"), *FileName, *Line, *Column);
		}
		else if (!Line.IsEmpty())
		{
			Location = FString::Printf(TEXT("%s(%s)"), *FileName, *Line);
		}

		if (!Severity.IsEmpty() && !Code.IsEmpty())
		{
			return FString::Printf(TEXT("%s %s at %s"), *Severity, *Code, *Location);
		}

		if (!Severity.IsEmpty())
		{
			return FString::Printf(TEXT("%s at %s"), *Severity, *Location);
		}

		return Location;
	}

	FString GetMutationReceiptDirectory()
	{
		const FString ReceiptDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealClaude"), TEXT("cpp_reflection_mutations"));
		IFileManager::Get().MakeDirectory(*ReceiptDirectory, true);
		return ReceiptDirectory;
	}

	FString SanitizeMutationToken(const FString& Text)
	{
		FString Sanitized = Text.TrimStartAndEnd();
		if (Sanitized.IsEmpty())
		{
			return TEXT("value");
		}

		for (TCHAR& Ch : Sanitized)
		{
			if (!(FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-')))
			{
				Ch = TEXT('_');
			}
		}

		while (Sanitized.Contains(TEXT("__")))
		{
			Sanitized.ReplaceInline(TEXT("__"), TEXT("_"));
		}
		return Sanitized.Left(64);
	}

	FString WritePropertyMutationReceipt(
		const TSharedPtr<FJsonObject>& ReceiptObject,
		const FString& ContractName,
		const FString& MemberName,
		const FString& MetadataKey)
	{
		const FString ReceiptPath = FPaths::Combine(
			GetMutationReceiptDirectory(),
			FString::Printf(
				TEXT("%s_%s_%s_%s.json"),
				*FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")),
				*SanitizeMutationToken(ContractName),
				*SanitizeMutationToken(MemberName),
				*SanitizeMutationToken(MetadataKey)));

		FFileHelper::SaveStringToFile(
			SerializeJsonPretty(ReceiptObject),
			*ReceiptPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return ReceiptPath;
	}

	FString MakeDeclarationCheckpointPath(const FString& ContractName, const FString& MemberName)
	{
		return FPaths::Combine(
			GetMutationReceiptDirectory(),
			FString::Printf(
				TEXT("%s_%s_%s_checkpoint.h"),
				*FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")),
				*SanitizeMutationToken(ContractName),
				*SanitizeMutationToken(MemberName)));
	}

	FString WriteDeclarationMutationReceipt(
		const TSharedPtr<FJsonObject>& ReceiptObject,
		const FString& ContractName,
		const FString& MemberName,
		const FString& Suffix)
	{
		const FString ReceiptPath = FPaths::Combine(
			GetMutationReceiptDirectory(),
			FString::Printf(
				TEXT("%s_%s_%s_%s.json"),
				*FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")),
				*SanitizeMutationToken(ContractName),
				*SanitizeMutationToken(MemberName),
				*SanitizeMutationToken(Suffix)));

		FFileHelper::SaveStringToFile(
			SerializeJsonPretty(ReceiptObject),
			*ReceiptPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return ReceiptPath;
	}

	TSharedPtr<FJsonObject> BuildExpectedPropertyMetadataObject(
		const FProperty* Property,
		const FString& MetadataKey,
		const FString& MetadataValue)
	{
		TSharedPtr<FJsonObject> MetadataObject = MakeShared<FJsonObject>();
		const TArray<FString> MetadataKeys = {
			TEXT("DisplayName"),
			TEXT("ToolTip"),
			TEXT("ClampMin"),
			TEXT("ClampMax"),
			TEXT("UIMin"),
			TEXT("UIMax"),
			TEXT("MultiLine"),
			TEXT("AllowPrivateAccess"),
			TEXT("EditCondition")
		};

		for (const FString& Key : MetadataKeys)
		{
			if (Property && Property->HasMetaData(*Key))
			{
				MetadataObject->SetStringField(Key, Property->GetMetaData(*Key));
			}
		}

		MetadataObject->SetStringField(MetadataKey, MetadataValue);
		return MetadataObject;
	}

	TSharedPtr<FJsonObject> BuildDeclarationOwnershipScopeConstraintsObject(
		const FString& ModuleOrigin,
		const FCppReflectionPropertyDeclarationPreviewResult& Preview)
	{
		TSharedPtr<FJsonObject> ConstraintsObject = MakeShared<FJsonObject>();
		ConstraintsObject->SetStringField(TEXT("accepted_module_scope"), TEXT("plugin_only"));
		ConstraintsObject->SetStringField(TEXT("resolved_module_origin"), ModuleOrigin);
		ConstraintsObject->SetBoolField(TEXT("requires_plugin_owned_header"), true);
		ConstraintsObject->SetBoolField(TEXT("requires_reflected_uclass"), true);
		ConstraintsObject->SetBoolField(TEXT("requires_declared_anchor_member"), true);
		ConstraintsObject->SetBoolField(TEXT("requires_unique_new_member_name"), true);
		ConstraintsObject->SetBoolField(TEXT("preview_only"), true);
		ConstraintsObject->SetBoolField(TEXT("declaration_apply_available_now"), true);
		ConstraintsObject->SetStringField(TEXT("accepted_apply_operation"), TEXT("apply_reflected_property_declaration"));
		ConstraintsObject->SetStringField(TEXT("accepted_failed_build_diagnostic_operation"), TEXT("inspect_reflected_property_declaration_build_failure"));
		ConstraintsObject->SetBoolField(TEXT("implementation_body_editing_allowed"), false);
		TSharedPtr<FJsonObject> ScopeObject = MakeShared<FJsonObject>();
		ScopeObject->SetBoolField(TEXT("allowed"), Preview.bScopeAllowed);
		ScopeObject->SetStringField(TEXT("matched_root"), Preview.ScopeMatchedRoot);
		ScopeObject->SetStringField(TEXT("denial_reason"), Preview.ScopeDenialReason);
		ConstraintsObject->SetObjectField(TEXT("resolved_write_scope"), ScopeObject);

		TArray<TSharedPtr<FJsonValue>> SupportedTypes;
		SupportedTypes.Add(MakeShared<FJsonValueString>(TEXT("bool")));
		ConstraintsObject->SetArrayField(TEXT("supported_property_cpp_types"), SupportedTypes);
		return ConstraintsObject;
	}

	TSharedPtr<FJsonObject> BuildDeclarationFutureApplyExpectationsObject(
		const FUnrealClaudeBuildSyncStatus& BuildSyncStatus)
	{
		TSharedPtr<FJsonObject> ExpectationsObject = MakeShared<FJsonObject>();
		ExpectationsObject->SetStringField(TEXT("status"), TEXT("single_shape_apply_lane_live"));
		ExpectationsObject->SetBoolField(TEXT("apply_available_now"), true);
		ExpectationsObject->SetStringField(TEXT("available_apply_operation"), TEXT("apply_reflected_property_declaration"));
		ExpectationsObject->SetStringField(TEXT("available_revert_operation"), TEXT("revert_reflected_property_declaration"));
		ExpectationsObject->SetStringField(TEXT("available_failed_build_diagnostic_operation"), TEXT("inspect_reflected_property_declaration_build_failure"));
		ExpectationsObject->SetBoolField(TEXT("metadata_mutation_is_not_the_only_apply_lane_anymore"), true);
		ExpectationsObject->SetBoolField(TEXT("apply_remains_bounded_to_one_bool_property_shape"), true);
		ExpectationsObject->SetBoolField(TEXT("requires_fresh_preflight_rebuild_after_apply"), true);
		ExpectationsObject->SetBoolField(TEXT("requires_post_build_reflection_readback"), true);
		ExpectationsObject->SetBoolField(TEXT("requires_restore_or_revert_receipt_after_apply"), true);
		ExpectationsObject->SetBoolField(TEXT("supports_receipt_linked_failed_build_diagnostics"), true);
		ExpectationsObject->SetBoolField(TEXT("supports_in_process_reflected_compile"), false);
		ExpectationsObject->SetStringField(TEXT("recommended_future_sequence"), TEXT("preview_then_apply_reflected_property_declaration_then_close_editor_and_run_preflight_launcher_then_post_build_reflection_readback_then_revert_reflected_property_declaration_then_rebuild_again"));
		ExpectationsObject->SetStringField(TEXT("preflight_launcher_path"), FUnrealClaudeModule::GetPreflightLauncherPath());
		ExpectationsObject->SetObjectField(TEXT("build_status_current"), BuildBuildSyncObject(BuildSyncStatus));
		return ExpectationsObject;
	}

	TSharedPtr<FJsonObject> BuildExpectedReflectedDeclarationAfterRebuildObject(
		const FString& PropertyName,
		const FString& PropertyCppType,
		const FString& Category,
		const FString& DefaultValueLiteral)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("expected_present_after_rebuild"), true);
		Object->SetStringField(TEXT("name"), PropertyName);
		Object->SetStringField(TEXT("cpp_type"), PropertyCppType);
		Object->SetStringField(TEXT("category"), Category);
		Object->SetStringField(TEXT("default_value_literal"), DefaultValueLiteral);

		TSharedPtr<FJsonObject> FlagsObject = MakeShared<FJsonObject>();
		FlagsObject->SetBoolField(TEXT("editable"), true);
		FlagsObject->SetBoolField(TEXT("blueprint_visible"), false);
		FlagsObject->SetBoolField(TEXT("config"), false);
		Object->SetObjectField(TEXT("expected_flags"), FlagsObject);
		return Object;
	}

	TSharedPtr<FJsonObject> BuildExpectedDeclarationAbsenceAfterRebuildObject(const FString& PropertyName)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("expected_present_after_rebuild"), false);
		Object->SetStringField(TEXT("name"), PropertyName);
		Object->SetStringField(TEXT("reason"), TEXT("revert should restore the checkpointed header content before the bounded declaration apply."));
		return Object;
	}

	TSharedPtr<FJsonObject> BuildDeclarationPreviewContinuityObject(
		const FCppReflectionPropertyDeclarationPreviewResult& Preview,
		const FCppReflectionPropertyDeclarationApplyResult& Apply)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), TEXT("exact_preview_shape_reapplied"));
		Object->SetStringField(TEXT("preview_schema_version"), Preview.SchemaVersion);
		Object->SetStringField(TEXT("apply_schema_version"), Apply.SchemaVersion);
		Object->SetStringField(TEXT("caller_expected_source_hash_before"), Apply.ExpectedSourceHashBefore);
		Object->SetStringField(TEXT("preview_source_hash_before"), Preview.SourceHashBefore);
		Object->SetStringField(TEXT("preview_source_hash_after"), Preview.SourceHashAfterPreview);
		Object->SetBoolField(TEXT("preview_hash_matches_apply_basis"), Apply.ExpectedSourceHashBefore.Equals(Preview.SourceHashBefore, ESearchCase::CaseSensitive));
		Object->SetBoolField(TEXT("generated_snippet_matches_apply"), Preview.GeneratedSnippet.Equals(Apply.GeneratedSnippet, ESearchCase::CaseSensitive));
		return Object;
	}

	TSharedPtr<FJsonObject> BuildDeclarationRestoreRevertContractObject(
		const FString& ReceiptPath,
		const FString& CheckpointPath)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), TEXT("revert_available_via_receipt"));
		Object->SetStringField(TEXT("revert_operation"), TEXT("revert_reflected_property_declaration"));
		Object->SetStringField(TEXT("receipt_path"), ReceiptPath);
		Object->SetStringField(TEXT("checkpoint_path"), CheckpointPath);
		Object->SetBoolField(TEXT("restore_required_before_closeout"), true);
		Object->SetBoolField(TEXT("requires_fresh_preflight_rebuild_after_revert"), true);
		Object->SetStringField(TEXT("drift_guard"), TEXT("current_header_must_match_applied_hash_before_revert"));
		return Object;
	}

	TSharedPtr<FJsonObject> BuildDeclarationCompileHandshakeObject(
		const FString& MutationAction,
		const bool bMutationWritten,
		const FUnrealClaudeBuildSyncStatus& BuildSyncBefore,
		const FUnrealClaudeBuildSyncStatus& BuildSyncAfter)
	{
		TSharedPtr<FJsonObject> HandshakeObject = MakeShared<FJsonObject>();
		const bool bNeedsRebuild = !BuildSyncAfter.bFresh;

		FString Status = bNeedsRebuild ? TEXT("rebuild_required") : TEXT("no_rebuild_required");
		if (!bMutationWritten && bNeedsRebuild)
		{
			Status = TEXT("build_already_stale");
		}

		HandshakeObject->SetStringField(TEXT("status"), Status);
		HandshakeObject->SetStringField(TEXT("mutation_action"), MutationAction);
		HandshakeObject->SetBoolField(TEXT("source_mutation_written"), bMutationWritten);
		HandshakeObject->SetBoolField(TEXT("supports_in_process_reflected_compile"), false);
		HandshakeObject->SetStringField(
			TEXT("reason"),
			TEXT("Plugin-owned reflected header declaration changes require a fresh preflight rebuild with the editor closed instead of claiming arbitrary in-process reflected C++ hot-reload."));
		HandshakeObject->SetStringField(TEXT("recommended_action"), bNeedsRebuild ? TEXT("close_editor_and_run_preflight_launcher") : TEXT("no_action"));
		HandshakeObject->SetStringField(TEXT("preflight_launcher_path"), FUnrealClaudeModule::GetPreflightLauncherPath());
		HandshakeObject->SetObjectField(TEXT("build_status_before"), BuildBuildSyncObject(BuildSyncBefore));
		HandshakeObject->SetObjectField(TEXT("build_status_after"), BuildBuildSyncObject(BuildSyncAfter));
		return HandshakeObject;
	}

	struct FReceiptLinkedBuildArtifactParseResult
	{
		bool bArtifactExists = false;
		bool bBuildFailureObserved = false;
		bool bReceiptHeaderPathAvailable = false;
		bool bReceiptMemberNameAvailable = false;
		bool bReceiptLineContextAvailable = false;
		bool bReceiptGeneratedDeclarationAvailable = false;
		bool bReceiptGeneratedSnippetAvailable = false;
		FString ArtifactKind = TEXT("build_log");
		FString DiagnosticsSource = TEXT("receipt_linked_build_log_parse_v1");
		FString ArtifactPath;
		FString ArtifactLastWriteTimeUtc;
		int32 ReceiptLineWindowStart = INDEX_NONE;
		int32 ReceiptLineWindowEnd = INDEX_NONE;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		int32 MatchingErrorCount = 0;
		int32 HeaderPathMatchCount = 0;
		int32 LineContextMatchCount = 0;
		int32 MemberNameMatchCount = 0;
		int32 GeneratedDeclarationMatchCount = 0;
		int32 GeneratedSnippetMatchCount = 0;
		FString LinkageConfidence = TEXT("none");
		FString Summary;
		TSharedPtr<FJsonObject> FirstMatchingError;
		TArray<TSharedPtr<FJsonValue>> Errors;
		TArray<TSharedPtr<FJsonValue>> MatchingErrors;

		TSharedPtr<FJsonObject> ToJson() const
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("schema_version"), GetPropertyDeclarationBuildFailureDiagnosticSchemaVersion());
			Object->SetStringField(TEXT("artifact_kind"), ArtifactKind);
			Object->SetStringField(TEXT("diagnostics_source"), DiagnosticsSource);
			Object->SetStringField(TEXT("artifact_path"), ArtifactPath);
			Object->SetBoolField(TEXT("artifact_exists"), bArtifactExists);
			Object->SetBoolField(TEXT("build_failure_observed"), bBuildFailureObserved);
			Object->SetStringField(TEXT("artifact_last_write_time_utc"), ArtifactLastWriteTimeUtc);
			Object->SetBoolField(TEXT("receipt_header_path_available"), bReceiptHeaderPathAvailable);
			Object->SetBoolField(TEXT("receipt_member_name_available"), bReceiptMemberNameAvailable);
			Object->SetBoolField(TEXT("receipt_line_context_available"), bReceiptLineContextAvailable);
			Object->SetBoolField(TEXT("receipt_generated_declaration_available"), bReceiptGeneratedDeclarationAvailable);
			Object->SetBoolField(TEXT("receipt_generated_snippet_available"), bReceiptGeneratedSnippetAvailable);
			Object->SetNumberField(TEXT("receipt_line_window_start"), ReceiptLineWindowStart);
			Object->SetNumberField(TEXT("receipt_line_window_end"), ReceiptLineWindowEnd);
			Object->SetNumberField(TEXT("error_count"), ErrorCount);
			Object->SetNumberField(TEXT("warning_count"), WarningCount);
			Object->SetNumberField(TEXT("matching_error_count"), MatchingErrorCount);
			Object->SetNumberField(TEXT("header_path_match_count"), HeaderPathMatchCount);
			Object->SetNumberField(TEXT("line_context_match_count"), LineContextMatchCount);
			Object->SetNumberField(TEXT("member_name_match_count"), MemberNameMatchCount);
			Object->SetNumberField(TEXT("generated_declaration_match_count"), GeneratedDeclarationMatchCount);
			Object->SetNumberField(TEXT("generated_snippet_match_count"), GeneratedSnippetMatchCount);
			Object->SetStringField(TEXT("linkage_confidence"), LinkageConfidence);
			Object->SetStringField(TEXT("summary"), Summary);
			if (FirstMatchingError.IsValid())
			{
				Object->SetObjectField(TEXT("first_matching_error"), FirstMatchingError);
			}
			Object->SetArrayField(TEXT("errors"), Errors);
			Object->SetArrayField(TEXT("matching_errors"), MatchingErrors);
			return Object;
		}
	};

	FString GetDefaultDeclarationBuildLogPath()
	{
		return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("UnrealBuildTool"), TEXT("Log.txt"));
	}

	FString NormalizeAbsolutePathForComparison(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return FString();
		}

		FString Normalized = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Normalized);
		return Normalized;
	}

	FString NormalizeWhitespaceForComparison(const FString& Text)
	{
		FString Normalized = Text;
		Normalized.ReplaceInline(TEXT("\r"), TEXT(" "));
		Normalized.ReplaceInline(TEXT("\n"), TEXT(" "));
		Normalized.ReplaceInline(TEXT("\t"), TEXT(" "));
		while (Normalized.Contains(TEXT("  ")))
		{
			Normalized.ReplaceInline(TEXT("  "), TEXT(" "));
		}

		return Normalized.TrimStartAndEnd();
	}

	bool ContainsNormalizedSubstring(const FString& Haystack, const FString& Needle)
	{
		if (Needle.IsEmpty())
		{
			return false;
		}

		const FString NormalizedHaystack = NormalizeWhitespaceForComparison(Haystack);
		const FString NormalizedNeedle = NormalizeWhitespaceForComparison(Needle);
		return !NormalizedNeedle.IsEmpty() && NormalizedHaystack.Contains(NormalizedNeedle, ESearchCase::IgnoreCase);
	}

	struct FReceiptLinkedBuildArtifactContext
	{
		FString HeaderPath;
		FString MemberName;
		int32 ReceiptLineWindowStart = INDEX_NONE;
		int32 ReceiptLineWindowEnd = INDEX_NONE;
		FString GeneratedDeclaration;
		FString GeneratedSnippet;
	};

	FString DetermineReceiptDiagnosticLinkageConfidence(const FReceiptLinkedBuildArtifactParseResult& ParseResult)
	{
		if (ParseResult.MemberNameMatchCount > 0
			|| ParseResult.GeneratedDeclarationMatchCount > 0
			|| ParseResult.GeneratedSnippetMatchCount > 0)
		{
			return TEXT("receipt_snippet_or_member_linked");
		}

		if (ParseResult.HeaderPathMatchCount > 0 && ParseResult.LineContextMatchCount > 0)
		{
			return TEXT("header_plus_line_context");
		}

		if (ParseResult.HeaderPathMatchCount > 0)
		{
			return TEXT("header_match_only");
		}

		return TEXT("none");
	}

	FReceiptLinkedBuildArtifactParseResult ParseReceiptLinkedBuildArtifact(
		const FString& BuildLogPath,
		const FReceiptLinkedBuildArtifactContext& ReceiptContext)
	{
		FReceiptLinkedBuildArtifactParseResult Result;
		Result.ArtifactPath = BuildLogPath;
		Result.bReceiptHeaderPathAvailable = !ReceiptContext.HeaderPath.IsEmpty();
		Result.bReceiptMemberNameAvailable = !ReceiptContext.MemberName.IsEmpty();
		Result.bReceiptLineContextAvailable =
			ReceiptContext.ReceiptLineWindowStart != INDEX_NONE
			&& ReceiptContext.ReceiptLineWindowEnd != INDEX_NONE
			&& ReceiptContext.ReceiptLineWindowEnd >= ReceiptContext.ReceiptLineWindowStart;
		Result.bReceiptGeneratedDeclarationAvailable = !ReceiptContext.GeneratedDeclaration.IsEmpty();
		Result.bReceiptGeneratedSnippetAvailable = !ReceiptContext.GeneratedSnippet.IsEmpty();
		Result.ReceiptLineWindowStart = ReceiptContext.ReceiptLineWindowStart;
		Result.ReceiptLineWindowEnd = ReceiptContext.ReceiptLineWindowEnd;

		if (BuildLogPath.IsEmpty() || !IFileManager::Get().FileExists(*BuildLogPath))
		{
			Result.Summary = FString::Printf(TEXT("Build artifact '%s' does not exist."), *BuildLogPath);
			return Result;
		}

		Result.bArtifactExists = true;
		Result.ArtifactLastWriteTimeUtc = IFileManager::Get().GetTimeStamp(*BuildLogPath).ToIso8601();

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *BuildLogPath))
		{
			Result.Summary = FString::Printf(TEXT("Could not load build artifact '%s'."), *BuildLogPath);
			return Result;
		}

		Result.bBuildFailureObserved = Content.Contains(TEXT("Result: Failed"));

		const FString NormalizedHeaderPath = NormalizeAbsolutePathForComparison(ReceiptContext.HeaderPath);
		const FString HeaderFilename = FPaths::GetCleanFilename(NormalizedHeaderPath);
		const FRegexPattern DiagnosticPattern(TEXT("^(.+?)\\((\\d+)(?:,(\\d+))?\\)\\s*:\\s*(error|fatal error|warning)\\s+(.+)$"));

		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (Line.IsEmpty())
			{
				continue;
			}

			const bool bIsError = Line.Contains(TEXT(": error ")) || Line.Contains(TEXT(": fatal error "));
			const bool bIsWarning = Line.Contains(TEXT(": warning ")) && !Line.Contains(TEXT("is not a preferred version"));
			if (!bIsError && !bIsWarning)
			{
				continue;
			}

			FString ParsedFile;
			FString ParsedLine;
			FString ParsedColumn;
			FString ParsedSeverity = bIsWarning ? TEXT("warning") : TEXT("error");
			FString ParsedCodeAndDetail;

			FRegexMatcher Matcher(DiagnosticPattern, Line);
			if (Matcher.FindNext())
			{
				ParsedFile = Matcher.GetCaptureGroup(1).TrimStartAndEnd();
				ParsedLine = Matcher.GetCaptureGroup(2).TrimStartAndEnd();
				ParsedColumn = Matcher.GetCaptureGroup(3).TrimStartAndEnd();
				ParsedSeverity = Matcher.GetCaptureGroup(4).TrimStartAndEnd();
				ParsedCodeAndDetail = Matcher.GetCaptureGroup(5).TrimStartAndEnd();
			}

			const FString NormalizedParsedFile = NormalizeAbsolutePathForComparison(ParsedFile);
			const bool bHeaderPathMatch =
				!NormalizedHeaderPath.IsEmpty()
				&& ((!NormalizedParsedFile.IsEmpty() && NormalizedParsedFile.Equals(NormalizedHeaderPath, ESearchCase::IgnoreCase))
					|| (!HeaderFilename.IsEmpty() && Line.Contains(HeaderFilename, ESearchCase::IgnoreCase)));
			const int32 ParsedLineNumber =
				!ParsedLine.IsEmpty() ? FCString::Atoi(*ParsedLine) : INDEX_NONE;
			const bool bLineContextMatch =
				bHeaderPathMatch
				&& Result.bReceiptLineContextAvailable
				&& ParsedLineNumber != INDEX_NONE
				&& ParsedLineNumber >= ReceiptContext.ReceiptLineWindowStart
				&& ParsedLineNumber <= ReceiptContext.ReceiptLineWindowEnd;
			const bool bMemberNameMatch =
				!ReceiptContext.MemberName.IsEmpty()
				&& (Line.Contains(ReceiptContext.MemberName, ESearchCase::IgnoreCase)
					|| ParsedCodeAndDetail.Contains(ReceiptContext.MemberName, ESearchCase::IgnoreCase));
			const bool bGeneratedDeclarationMatch =
				ContainsNormalizedSubstring(Line, ReceiptContext.GeneratedDeclaration)
				|| ContainsNormalizedSubstring(ParsedCodeAndDetail, ReceiptContext.GeneratedDeclaration);
			const bool bGeneratedSnippetMatch =
				ContainsNormalizedSubstring(Line, ReceiptContext.GeneratedSnippet)
				|| ContainsNormalizedSubstring(ParsedCodeAndDetail, ReceiptContext.GeneratedSnippet);

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("message"), Line);
			Entry->SetStringField(TEXT("file"), ParsedFile);
			Entry->SetStringField(TEXT("line"), ParsedLine);
			Entry->SetStringField(TEXT("column"), ParsedColumn);
			Entry->SetStringField(TEXT("severity"), ParsedSeverity);
			Entry->SetStringField(TEXT("code_and_detail"), ParsedCodeAndDetail);
			Entry->SetBoolField(TEXT("matches_receipt_header_path"), bHeaderPathMatch);
			Entry->SetBoolField(TEXT("matches_receipt_line_context"), bLineContextMatch);
			Entry->SetBoolField(TEXT("matches_receipt_member_name"), bMemberNameMatch);
			Entry->SetBoolField(TEXT("matches_receipt_generated_declaration"), bGeneratedDeclarationMatch);
			Entry->SetBoolField(TEXT("matches_receipt_generated_snippet"), bGeneratedSnippetMatch);

			if (bIsError)
			{
				Result.ErrorCount++;
				Result.bBuildFailureObserved = true;
				if (Result.Errors.Num() < 20)
				{
					Result.Errors.Add(MakeShared<FJsonValueObject>(Entry));
				}

				if (bHeaderPathMatch)
				{
					Result.HeaderPathMatchCount++;
				}

				if (bLineContextMatch)
				{
					Result.LineContextMatchCount++;
				}

				if (bMemberNameMatch)
				{
					Result.MemberNameMatchCount++;
				}

				if (bGeneratedDeclarationMatch)
				{
					Result.GeneratedDeclarationMatchCount++;
				}

				if (bGeneratedSnippetMatch)
				{
					Result.GeneratedSnippetMatchCount++;
				}

				if (bHeaderPathMatch || bLineContextMatch || bMemberNameMatch || bGeneratedDeclarationMatch || bGeneratedSnippetMatch)
				{
					Result.MatchingErrorCount++;
					if (!Result.FirstMatchingError.IsValid())
					{
						Result.FirstMatchingError = Entry;
					}

					if (Result.MatchingErrors.Num() < 10)
					{
						Result.MatchingErrors.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
			}
			else
			{
				Result.WarningCount++;
			}
		}

		Result.LinkageConfidence = DetermineReceiptDiagnosticLinkageConfidence(Result);
		Result.Summary = FString::Printf(
			TEXT("Parsed %d errors and %d warnings from build artifact; %d errors matched the receipt context (header=%d, line_context=%d, member=%d, generated_declaration=%d, generated_snippet=%d)."),
			Result.ErrorCount,
			Result.WarningCount,
			Result.MatchingErrorCount,
			Result.HeaderPathMatchCount,
			Result.LineContextMatchCount,
			Result.MemberNameMatchCount,
			Result.GeneratedDeclarationMatchCount,
			Result.GeneratedSnippetMatchCount);
		return Result;
	}

	TSharedPtr<FJsonObject> BuildDeclarationCurrentSourceStateObject(
		const FString& HeaderPath,
		const FString& CheckpointPath,
		const FString& ExpectedAppliedSourceHash)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("header_path"), HeaderPath);
		Object->SetStringField(TEXT("checkpoint_path"), CheckpointPath);
		Object->SetStringField(TEXT("expected_applied_source_hash"), ExpectedAppliedSourceHash);

		const bool bHeaderExists = !HeaderPath.IsEmpty() && IFileManager::Get().FileExists(*HeaderPath);
		const bool bCheckpointExists = !CheckpointPath.IsEmpty() && IFileManager::Get().FileExists(*CheckpointPath);
		Object->SetBoolField(TEXT("header_exists"), bHeaderExists);
		Object->SetBoolField(TEXT("checkpoint_exists"), bCheckpointExists);

		const FUnrealClaudeScopePolicy::FScopeCheckResult ScopeCheck =
			FUnrealClaudeScopePolicy::IsAbsoluteWriteAllowed(HeaderPath);
		Object->SetBoolField(TEXT("scope_allowed"), ScopeCheck.bAllowed);
		Object->SetStringField(TEXT("scope_matched_root"), ScopeCheck.MatchedRoot);
		Object->SetStringField(TEXT("scope_denial_reason"), ScopeCheck.DenialReason);

		FString CurrentSourceHash;
		if (bHeaderExists)
		{
			FString CurrentContent;
			if (FFileHelper::LoadFileToString(CurrentContent, *HeaderPath))
			{
				FTCHARToUTF8 Utf8(*CurrentContent);
				FMD5 Md5;
				Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
				uint8 Digest[16];
				Md5.Final(Digest);
				CurrentSourceHash = BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
			}
		}

		const bool bMatchesAppliedHash =
			!ExpectedAppliedSourceHash.IsEmpty()
			&& !CurrentSourceHash.IsEmpty()
			&& CurrentSourceHash.Equals(ExpectedAppliedSourceHash, ESearchCase::CaseSensitive);
		const bool bRevertAvailableNow = ScopeCheck.bAllowed && bHeaderExists && bCheckpointExists && bMatchesAppliedHash;

		Object->SetStringField(TEXT("current_source_hash"), CurrentSourceHash);
		Object->SetBoolField(TEXT("matches_expected_applied_hash"), bMatchesAppliedHash);
		Object->SetBoolField(TEXT("revert_available_now"), bRevertAvailableNow);
		Object->SetStringField(
			TEXT("revert_state_classification"),
			bRevertAvailableNow ? TEXT("revert_available") : TEXT("revert_not_yet_proven"));
		return Object;
	}

	TSharedPtr<FJsonObject> BuildDeclarationFailedBuildDiagnosticContractObject(
		const FString& ReceiptPath,
		const FString& DefaultBuildLogPath)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), TEXT("receipt_linked_failed_build_diagnostics_available"));
		Object->SetStringField(TEXT("diagnostic_operation"), TEXT("inspect_reflected_property_declaration_build_failure"));
		Object->SetStringField(TEXT("receipt_path"), ReceiptPath);
		Object->SetStringField(TEXT("default_build_log_path"), DefaultBuildLogPath);
		Object->SetBoolField(TEXT("accepts_explicit_build_log_path"), true);
		Object->SetBoolField(TEXT("requires_failed_build_artifact"), true);
		Object->SetStringField(TEXT("primary_failure_state"), TEXT("mutation_written_but_build_failed"));
		Object->SetStringField(TEXT("primary_reflection_state"), TEXT("reflection_not_yet_proven_due_to_build_failure"));
		Object->SetStringField(TEXT("primary_revert_state"), TEXT("revert_available"));
		TArray<TSharedPtr<FJsonValue>> ConfidenceTiers;
		ConfidenceTiers.Add(MakeShared<FJsonValueString>(TEXT("receipt_snippet_or_member_linked")));
		ConfidenceTiers.Add(MakeShared<FJsonValueString>(TEXT("header_plus_line_context")));
		ConfidenceTiers.Add(MakeShared<FJsonValueString>(TEXT("header_match_only")));
		ConfidenceTiers.Add(MakeShared<FJsonValueString>(TEXT("none")));
		Object->SetArrayField(TEXT("supported_linkage_confidence_tiers"), ConfidenceTiers);

		TArray<TSharedPtr<FJsonValue>> ParticipatingReceiptFields;
		ParticipatingReceiptFields.Add(MakeShared<FJsonValueString>(TEXT("header_path")));
		ParticipatingReceiptFields.Add(MakeShared<FJsonValueString>(TEXT("new_member_name")));
		ParticipatingReceiptFields.Add(MakeShared<FJsonValueString>(TEXT("insertion_line")));
		ParticipatingReceiptFields.Add(MakeShared<FJsonValueString>(TEXT("generated_declaration")));
		ParticipatingReceiptFields.Add(MakeShared<FJsonValueString>(TEXT("generated_snippet")));
		Object->SetArrayField(TEXT("receipt_fields_participating_in_linkage"), ParticipatingReceiptFields);
		return Object;
	}

	TSharedPtr<FJsonObject> BuildDeclarationFailureCloseoutSequenceObject(const bool bRevertAvailableNow)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), bRevertAvailableNow ? TEXT("revert_then_rebuild") : TEXT("investigate_before_revert"));

		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueString>(TEXT("inspect_reflected_property_declaration_build_failure")));
		Steps.Add(MakeShared<FJsonValueString>(bRevertAvailableNow ? TEXT("revert_reflected_property_declaration") : TEXT("verify_receipt_checkpoint_and_source_drift_state")));
		Steps.Add(MakeShared<FJsonValueString>(TEXT("close_editor_and_run_preflight_launcher")));
		Steps.Add(MakeShared<FJsonValueString>(TEXT("confirm_clean_build_before_any_reflection_claim")));
		Steps.Add(MakeShared<FJsonValueString>(TEXT("optional_repreview_and_reapply_with_corrected_member_name")));
		Object->SetArrayField(TEXT("recommended_steps"), Steps);
		return Object;
	}

	TSharedPtr<FJsonObject> BuildCompileHandshakeObject(
		const bool bApplied,
		const FUnrealClaudeBuildSyncStatus& BuildSyncBefore,
		const FUnrealClaudeBuildSyncStatus& BuildSyncAfter)
	{
		TSharedPtr<FJsonObject> HandshakeObject = MakeShared<FJsonObject>();
		const bool bNeedsRebuild = !BuildSyncAfter.bFresh;

		FString Status = bNeedsRebuild ? TEXT("rebuild_required") : TEXT("no_rebuild_required");
		if (!bApplied && bNeedsRebuild)
		{
			Status = TEXT("build_already_stale");
		}

		HandshakeObject->SetStringField(TEXT("status"), Status);
		HandshakeObject->SetBoolField(TEXT("mutation_applied"), bApplied);
		HandshakeObject->SetBoolField(TEXT("supports_in_process_reflected_compile"), false);
		HandshakeObject->SetStringField(
			TEXT("reason"),
			TEXT("Reflected header metadata changes touch tracked plugin source. This slice requires a fresh preflight rebuild with the editor closed instead of claiming arbitrary in-process reflected C++ hot-reload."));
		HandshakeObject->SetStringField(TEXT("recommended_action"), bNeedsRebuild ? TEXT("close_editor_and_run_preflight_launcher") : TEXT("no_action"));
		HandshakeObject->SetStringField(TEXT("preflight_launcher_path"), FUnrealClaudeModule::GetPreflightLauncherPath());
		HandshakeObject->SetObjectField(TEXT("build_status_before"), BuildBuildSyncObject(BuildSyncBefore));
		HandshakeObject->SetObjectField(TEXT("build_status_after"), BuildBuildSyncObject(BuildSyncAfter));
		return HandshakeObject;
	}
}

FMCPToolInfo FMCPTool_CppReflection::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("cpp_reflection");
	Info.Description = TEXT(
		"Reflected C++ contract discovery plus bounded plugin-owned mutation lanes for native Unreal types loaded under /Script modules.\n\n"
		"Operations:\n"
		"- 'list_reflected_contracts': discover reflected native UCLASS/USTRUCT/UENUM contracts with module and header metadata\n"
		"- 'get_reflected_contract': inspect one reflected contract in structured form, including UPROPERTY/UFUNCTION members where applicable\n"
		"- 'preview_reflected_property_declaration': preview one plugin-owned reflected bool property declaration shape after an existing declared UPROPERTY anchor\n"
		"- 'apply_reflected_property_declaration': apply that same exact plugin-owned reflected bool property declaration shape with receipt/checkpoint/rebuild truth\n"
		"- 'revert_reflected_property_declaration': restore the checkpointed header content for that bounded declaration apply via receipt\n"
		"- 'inspect_reflected_property_declaration_build_failure': inspect one receipt-linked failed-build diagnostic path for that same bounded declaration lane\n"
		"- 'build_reflected_property_declaration_evidence_bundle': normalize preview/apply/failure/revert/readback evidence for that same bounded declaration lane into one machine-readable bundle\n"
		"- 'preview_property_metadata_mutation': machine-readable preview of one safe plugin-owned UPROPERTY metadata upsert\n"
		"- 'apply_property_metadata_mutation': apply that bounded metadata upsert and return a rebuild-handshake receipt\n\n"
		"Report export:\n"
		"- bounded declaration evidence bundles can persist a Markdown report + normalized artifact sidecar by setting export_report=true on build_reflected_property_declaration_evidence_bundle\n\n"
		"Truth boundary:\n"
		"- uses runtime reflection plus selected UHT metadata such as ModuleRelativePath\n"
		"- declaration authoring now has one bounded plugin-owned bool-property preview/apply/revert lane; broader declaration families remain future work\n"
		"- mutation is limited to reflection-resolved plugin-owned headers for one exact bool declaration shape plus bounded UPROPERTY metadata updates\n"
		"- compile handshakes now also include one receipt-linked failed-build diagnostic inspection path plus one bounded evidence-bundle/readback layer, not broad arbitrary C++ hot-reload claims\n"
		"- does not claim a full C++ AST or arbitrary source parsing/editing"
	);

	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"), TEXT("Operation: list_reflected_contracts, get_reflected_contract, preview_reflected_property_declaration, apply_reflected_property_declaration, revert_reflected_property_declaration, inspect_reflected_property_declaration_build_failure, build_reflected_property_declaration_evidence_bundle, preview_property_metadata_mutation, or apply_property_metadata_mutation"), true),
		FMCPToolParameter(TEXT("symbol_kind"), TEXT("string"), TEXT("Filter by reflected symbol kind: all, class, struct, enum (default: all)"), false, TEXT("all")),
		FMCPToolParameter(TEXT("module_scope"), TEXT("string"), TEXT("Module scope: project_and_plugin, project_only, plugin_only, engine_only, all_loaded_modules (default: project_and_plugin)"), false, TEXT("project_and_plugin")),
		FMCPToolParameter(TEXT("name_filter"), TEXT("string"), TEXT("Substring filter for list_reflected_contracts against reflection name, C++ name, or path"), false),
		FMCPToolParameter(TEXT("module_filter"), TEXT("string"), TEXT("Substring filter for module name"), false),
		FMCPToolParameter(TEXT("limit"), TEXT("number"), TEXT("Maximum contracts returned by list_reflected_contracts (1-200, default: 25)"), false, TEXT("25")),
		FMCPToolParameter(TEXT("symbol"), TEXT("string"), TEXT("Exact path or simple reflected/C++ symbol name for get_reflected_contract and the bounded preview/apply declaration or metadata surfaces"), false),
		FMCPToolParameter(TEXT("member_name"), TEXT("string"), TEXT("Declared reflected property name for preview/apply_property_metadata_mutation"), false),
		FMCPToolParameter(TEXT("anchor_member_name"), TEXT("string"), TEXT("Declared reflected property name that acts as the insertion anchor for preview_reflected_property_declaration and apply_reflected_property_declaration"), false),
		FMCPToolParameter(TEXT("new_member_name"), TEXT("string"), TEXT("New reflected property identifier for preview_reflected_property_declaration and apply_reflected_property_declaration (current accepted shape is bool members that start with 'b')"), false),
		FMCPToolParameter(TEXT("property_cpp_type"), TEXT("string"), TEXT("Requested C++ property type for preview_reflected_property_declaration/apply_reflected_property_declaration (current bounded shape supports bool only)"), false),
		FMCPToolParameter(TEXT("category"), TEXT("string"), TEXT("Category string for preview_reflected_property_declaration and apply_reflected_property_declaration"), false),
		FMCPToolParameter(TEXT("default_value"), TEXT("string"), TEXT("Optional default value literal for preview_reflected_property_declaration/apply_reflected_property_declaration (current bool shape supports true or false; default: false)"), false, TEXT("false")),
		FMCPToolParameter(TEXT("expected_preview_schema_version"), TEXT("string"), TEXT("Required for apply_reflected_property_declaration. Must match the current preview schema version to keep preview -> apply continuity explicit."), false),
		FMCPToolParameter(TEXT("expected_source_hash_before"), TEXT("string"), TEXT("Required for apply_reflected_property_declaration. The source_hash_before returned by preview_reflected_property_declaration."), false),
		FMCPToolParameter(TEXT("receipt_path"), TEXT("string"), TEXT("Required for revert_reflected_property_declaration, inspect_reflected_property_declaration_build_failure, and build_reflected_property_declaration_evidence_bundle. Points at the prior apply receipt emitted by apply_reflected_property_declaration."), false),
		FMCPToolParameter(TEXT("build_log_path"), TEXT("string"), TEXT("Optional explicit build-log artifact path for inspect_reflected_property_declaration_build_failure. Empty defaults to the standard UnrealBuildTool log path."), false),
		FMCPToolParameter(TEXT("preview_result_path"), TEXT("string"), TEXT("Optional JSON result path from preview_reflected_property_declaration for build_reflected_property_declaration_evidence_bundle."), false),
		FMCPToolParameter(TEXT("failed_build_result_path"), TEXT("string"), TEXT("Optional JSON result path from inspect_reflected_property_declaration_build_failure for build_reflected_property_declaration_evidence_bundle."), false),
		FMCPToolParameter(TEXT("revert_result_path"), TEXT("string"), TEXT("Optional JSON result path from revert_reflected_property_declaration for build_reflected_property_declaration_evidence_bundle."), false),
		FMCPToolParameter(TEXT("reflection_readback_path"), TEXT("string"), TEXT("Optional JSON result path from get_reflected_contract after rebuild/readback for build_reflected_property_declaration_evidence_bundle."), false),
		FMCPToolParameter(TEXT("export_report"), TEXT("boolean"), TEXT("For build_reflected_property_declaration_evidence_bundle, if true, persist a Markdown report and normalized sidecar under Saved/UnrealClaude/Reports."), false, TEXT("false")),
		FMCPToolParameter(TEXT("report_name"), TEXT("string"), TEXT("Optional custom report name when export_report=true for build_reflected_property_declaration_evidence_bundle."), false),
		FMCPToolParameter(TEXT("report_slug"), TEXT("string"), TEXT("Optional custom slug for saved report filenames when export_report=true for build_reflected_property_declaration_evidence_bundle."), false),
		FMCPToolParameter(TEXT("metadata_key"), TEXT("string"), TEXT("Allowed metadata key for mutation preview/apply: DisplayName, ToolTip, ClampMin, ClampMax, UIMin, UIMax, MultiLine"), false),
		FMCPToolParameter(TEXT("metadata_value"), TEXT("string"), TEXT("Metadata value to upsert for preview/apply_property_metadata_mutation"), false),
		FMCPToolParameter(TEXT("include_properties"), TEXT("boolean"), TEXT("Include reflected properties on class/struct contracts (default: true)"), false, TEXT("true")),
		FMCPToolParameter(TEXT("include_functions"), TEXT("boolean"), TEXT("Include reflected functions on class contracts (default: true)"), false, TEXT("true")),
		FMCPToolParameter(TEXT("include_inherited"), TEXT("boolean"), TEXT("Include inherited reflected members where supported (default: false)"), false, TEXT("false")),
		FMCPToolParameter(TEXT("include_metadata"), TEXT("boolean"), TEXT("Include a bounded selected metadata subset for the contract members (default: false)"), false, TEXT("false")),
		FMCPToolParameter(TEXT("member_limit"), TEXT("number"), TEXT("Maximum reflected members/enumerators returned by get_reflected_contract (1-500, default: 200)"), false, TEXT("200")),
	};

	// P7 lifecycle override -- documented uniformly across all Modifying/Destructive tools.
	Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
		TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_CppReflection::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("list_reflected_contracts"))
	{
		return ExecuteListReflectedContracts(Params);
	}

	if (Operation == TEXT("get_reflected_contract"))
	{
		return ExecuteGetReflectedContract(Params);
	}

	if (Operation == TEXT("preview_reflected_property_declaration"))
	{
		return ExecutePreviewReflectedPropertyDeclaration(Params);
	}

	if (Operation == TEXT("apply_reflected_property_declaration"))
	{
		return ExecuteApplyReflectedPropertyDeclaration(Params);
	}

	if (Operation == TEXT("revert_reflected_property_declaration"))
	{
		return ExecuteRevertReflectedPropertyDeclaration(Params);
	}

	if (Operation == TEXT("inspect_reflected_property_declaration_build_failure"))
	{
		return ExecuteInspectReflectedPropertyDeclarationBuildFailure(Params);
	}

	if (Operation == TEXT("build_reflected_property_declaration_evidence_bundle"))
	{
		return ExecuteBuildReflectedPropertyDeclarationEvidenceBundle(Params);
	}

	if (Operation == TEXT("preview_property_metadata_mutation"))
	{
		return ExecutePreviewPropertyMetadataMutation(Params);
	}

	if (Operation == TEXT("apply_property_metadata_mutation"))
	{
		return ExecuteApplyPropertyMetadataMutation(Params);
	}

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown cpp_reflection operation: '%s'. Valid: list_reflected_contracts, get_reflected_contract, ")
		TEXT("preview_reflected_property_declaration, apply_reflected_property_declaration, ")
		TEXT("revert_reflected_property_declaration, inspect_reflected_property_declaration_build_failure, ")
		TEXT("build_reflected_property_declaration_evidence_bundle, preview_property_metadata_mutation, ")
		TEXT("apply_property_metadata_mutation"),
		*Operation));
}

FMCPToolResult FMCPTool_CppReflection::ExecuteListReflectedContracts(const TSharedRef<FJsonObject>& Params)
{
	FString ParseError;
	const ECppReflectionKindFilter KindFilter =
		ParseKindFilter(ExtractOptionalString(Params, TEXT("symbol_kind"), TEXT("all")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const ECppReflectionModuleScope ModuleScope =
		ParseModuleScope(ExtractOptionalString(Params, TEXT("module_scope"), TEXT("project_and_plugin")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	const FString ModuleFilter = ExtractOptionalString(Params, TEXT("module_filter"));
	const int32 Limit = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25), 1, 200);

	const FCppModuleLocator ModuleLocator;
	TArray<const UObject*> MatchingObjects;
	CollectMatchingObjects(MatchingObjects, KindFilter, ModuleScope, NameFilter, ModuleFilter, ModuleLocator);

	TArray<TSharedPtr<FJsonValue>> ContractValues;
	const int32 ReturnedCount = FMath::Min(MatchingObjects.Num(), Limit);
	for (int32 Index = 0; Index < ReturnedCount; ++Index)
	{
		ContractValues.Add(MakeShared<FJsonValueObject>(BuildTypeSummaryObject(MatchingObjects[Index], ModuleLocator, true)));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("list_reflected_contracts"));
	Data->SetStringField(TEXT("discovery_basis"), TEXT("runtime_reflection_and_uht_metadata"));
	Data->SetStringField(TEXT("note"), TEXT("Read-only reflected native contract discovery over loaded /Script types. This is not a full C++ AST/source parser."));
	Data->SetStringField(TEXT("symbol_kind"), KindFilterToString(KindFilter));
	Data->SetStringField(TEXT("module_scope"), ModuleScopeToString(ModuleScope));
	Data->SetStringField(TEXT("name_filter"), NameFilter);
	Data->SetStringField(TEXT("module_filter"), ModuleFilter);
	Data->SetNumberField(TEXT("matching_total"), MatchingObjects.Num());
	Data->SetNumberField(TEXT("returned_count"), ReturnedCount);
	Data->SetArrayField(TEXT("contracts"), ContractValues);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Reflected C++ contracts: %d returned (%d matching)"), ReturnedCount, MatchingObjects.Num()),
		Data);
}

FMCPToolResult FMCPTool_CppReflection::ExecuteGetReflectedContract(const TSharedRef<FJsonObject>& Params)
{
	FString SymbolIdentifier;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("symbol"), SymbolIdentifier, Error))
	{
		return Error.GetValue();
	}

	FString ParseError;
	const ECppReflectionKindFilter KindFilter =
		ParseKindFilter(ExtractOptionalString(Params, TEXT("symbol_kind"), TEXT("all")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const ECppReflectionModuleScope ModuleScope =
		ParseModuleScope(ExtractOptionalString(Params, TEXT("module_scope"), TEXT("project_and_plugin")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const FString ModuleFilter = ExtractOptionalString(Params, TEXT("module_filter"));
	const bool bIncludeProperties = ExtractOptionalBool(Params, TEXT("include_properties"), true);
	const bool bIncludeFunctions = ExtractOptionalBool(Params, TEXT("include_functions"), true);
	const bool bIncludeInherited = ExtractOptionalBool(Params, TEXT("include_inherited"), false);
	const bool bIncludeMetadata = ExtractOptionalBool(Params, TEXT("include_metadata"), false);
	const int32 MemberLimit = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("member_limit"), 200), 1, 500);

	const FCppModuleLocator ModuleLocator;
	FString ResolveError;
	const UObject* ContractObject = ResolveObjectIdentifier(
		SymbolIdentifier,
		KindFilter,
		ModuleScope,
		ModuleFilter,
		ModuleLocator,
		ResolveError);
	if (!ContractObject)
	{
		return FMCPToolResult::Error(ResolveError);
	}

	TSharedPtr<FJsonObject> ContractJson = BuildTypeSummaryObject(ContractObject, ModuleLocator, true);
	ContractJson->SetBoolField(TEXT("include_properties"), bIncludeProperties);
	ContractJson->SetBoolField(TEXT("include_functions"), bIncludeFunctions);
	ContractJson->SetBoolField(TEXT("include_inherited"), bIncludeInherited);
	ContractJson->SetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	ContractJson->SetNumberField(TEXT("member_limit"), MemberLimit);

	if (bIncludeMetadata)
	{
		const TArray<FString> MetadataKeys = {
			TEXT("DisplayName"),
			TEXT("ToolTip"),
			TEXT("ModuleRelativePath")
		};
		if (const UField* Field = GetFieldForMetadata(ContractObject))
		{
			if (TSharedPtr<FJsonObject> MetadataObject = BuildSelectedMetadata(Field, MetadataKeys))
			{
				ContractJson->SetObjectField(TEXT("metadata"), MetadataObject);
			}
		}
	}

	AddTypeMembers(
		ContractObject,
		ContractJson,
		bIncludeProperties,
		bIncludeFunctions,
		bIncludeMetadata,
		bIncludeInherited,
		MemberLimit);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("get_reflected_contract"));
	Data->SetStringField(TEXT("discovery_basis"), TEXT("runtime_reflection_and_uht_metadata"));
	Data->SetStringField(TEXT("note"), TEXT("Structured contract output comes from loaded reflection data plus selected UHT metadata. It does not imply full arbitrary C++ parsing."));
	Data->SetObjectField(TEXT("contract"), ContractJson);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Reflected C++ contract: %s"), *GetCppSymbolName(ContractObject)),
		Data);
}

FMCPToolResult FMCPTool_CppReflection::ExecutePreviewReflectedPropertyDeclaration(const TSharedRef<FJsonObject>& Params)
{
	FString SymbolIdentifier;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("symbol"), SymbolIdentifier, Error))
	{
		return Error.GetValue();
	}

	FString AnchorMemberName;
	if (!ExtractRequiredString(Params, TEXT("anchor_member_name"), AnchorMemberName, Error))
	{
		return Error.GetValue();
	}

	FString NewMemberName;
	if (!ExtractRequiredString(Params, TEXT("new_member_name"), NewMemberName, Error))
	{
		return Error.GetValue();
	}

	FString PropertyCppType;
	if (!ExtractRequiredString(Params, TEXT("property_cpp_type"), PropertyCppType, Error))
	{
		return Error.GetValue();
	}

	FString Category;
	if (!ExtractRequiredString(Params, TEXT("category"), Category, Error))
	{
		return Error.GetValue();
	}

	const FString DefaultValue = ExtractOptionalString(Params, TEXT("default_value"), TEXT("false"));

	FString ParseError;
	const ECppReflectionKindFilter KindFilter =
		ParseKindFilter(ExtractOptionalString(Params, TEXT("symbol_kind"), TEXT("all")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const ECppReflectionModuleScope ModuleScope =
		ParseModuleScope(ExtractOptionalString(Params, TEXT("module_scope"), TEXT("plugin_only")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const FString ModuleFilter = ExtractOptionalString(Params, TEXT("module_filter"));
	const FCppModuleLocator ModuleLocator;
	FString ResolveError;
	const UObject* ContractObject = ResolveObjectIdentifier(
		SymbolIdentifier,
		KindFilter,
		ModuleScope,
		ModuleFilter,
		ModuleLocator,
		ResolveError);
	if (!ContractObject)
	{
		return FMCPToolResult::Error(ResolveError);
	}

	const UClass* ContractClass = Cast<UClass>(ContractObject);
	if (!ContractClass)
	{
		return FMCPToolResult::Error(TEXT("preview_reflected_property_declaration is currently bounded to plugin-owned UCLASS contracts only."));
	}

	if (DoesClassHierarchyAlreadyDefineProperty(ContractClass, NewMemberName))
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("New member '%s' already exists in reflected class hierarchy '%s'. This preview-only lane requires a unique property identifier."),
			*NewMemberName,
			*GetCppSymbolName(ContractObject)));
	}

	TSharedPtr<FJsonObject> ContractSummary = BuildTypeSummaryObject(ContractObject, ModuleLocator, true);
	const TSharedPtr<FJsonObject>* SourceLocationPtr = nullptr;
	if (!ContractSummary->TryGetObjectField(TEXT("source_location"), SourceLocationPtr)
		|| !SourceLocationPtr
		|| !(*SourceLocationPtr).IsValid())
	{
		return FMCPToolResult::Error(TEXT("Resolved contract does not expose a source_location object."));
	}

	FString ModuleOrigin;
	FString HeaderPath;
	(*SourceLocationPtr)->TryGetStringField(TEXT("module_origin"), ModuleOrigin);
	(*SourceLocationPtr)->TryGetStringField(TEXT("resolved_header_path"), HeaderPath);

	if (ModuleOrigin != TEXT("plugin"))
	{
		return FMCPToolResult::Error(TEXT("preview_reflected_property_declaration is bounded to plugin-owned headers only."));
	}

	if (HeaderPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Resolved contract does not expose a concrete plugin header path."));
	}

	FString AnchorResolveError;
	const FProperty* AnchorProperty = ResolveDeclaredPropertyIdentifier(ContractObject, AnchorMemberName, AnchorResolveError);
	if (!AnchorProperty)
	{
		return FMCPToolResult::Error(AnchorResolveError);
	}

	const FCppReflectionPropertyDeclarationPreviewResult Preview =
		PreviewPluginReflectedPropertyDeclaration(
			HeaderPath,
			AnchorProperty->GetName(),
			NewMemberName,
			PropertyCppType,
			Category,
			DefaultValue);
	if (!Preview.bSuccess)
	{
		return FMCPToolResult::Error(Preview.ErrorMessage);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("preview_reflected_property_declaration"));
	Data->SetStringField(TEXT("preview_schema_version"), Preview.SchemaVersion);
	Data->SetStringField(TEXT("mutation_lane"), TEXT("preview_only_reflected_property_declaration_foundation"));
	Data->SetStringField(TEXT("declaration_kind"), Preview.DeclarationKind);
	Data->SetStringField(TEXT("discovery_basis"), TEXT("runtime_reflection_uht_metadata_plus_bounded_header_insertion_preview"));
	Data->SetStringField(TEXT("note"), TEXT("Preview only. No header file was written. The matching bounded apply lane now exists for this same bool-property declaration shape, but broader declaration authoring is still future work."));
	Data->SetStringField(TEXT("truth_boundary"), TEXT("This slice previews one plugin-owned reflected bool property declaration shape after an existing declared UPROPERTY anchor. It is not declaration apply, and arbitrary .cpp implementation-body editing remains forbidden."));
	Data->SetBoolField(TEXT("preview_only"), true);
	Data->SetBoolField(TEXT("declaration_apply_available_now"), true);
	Data->SetBoolField(TEXT("implementation_body_editing_allowed"), false);
	Data->SetObjectField(TEXT("contract"), ContractSummary);
	Data->SetObjectField(TEXT("anchor_member_before"), BuildPropertyObject(AnchorProperty, true));
	Data->SetObjectField(TEXT("ownership_scope_constraints"), BuildDeclarationOwnershipScopeConstraintsObject(ModuleOrigin, Preview));
	Data->SetObjectField(TEXT("future_apply_expectations"), BuildDeclarationFutureApplyExpectationsObject(FUnrealClaudeModule::GetBuildSyncStatus()));
	Data->SetObjectField(TEXT("preview"), Preview.ToJson());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Previewed reflected property declaration for %s after %s"), *GetCppSymbolName(ContractObject), *AnchorProperty->GetName()),
		Data);
}

FMCPToolResult FMCPTool_CppReflection::ExecuteApplyReflectedPropertyDeclaration(const TSharedRef<FJsonObject>& Params)
{
	const FUnrealClaudeBuildSyncStatus BuildSyncBefore = FUnrealClaudeModule::GetBuildSyncStatus();

	FString SymbolIdentifier;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("symbol"), SymbolIdentifier, Error))
	{
		return Error.GetValue();
	}

	FString AnchorMemberName;
	if (!ExtractRequiredString(Params, TEXT("anchor_member_name"), AnchorMemberName, Error))
	{
		return Error.GetValue();
	}

	FString NewMemberName;
	if (!ExtractRequiredString(Params, TEXT("new_member_name"), NewMemberName, Error))
	{
		return Error.GetValue();
	}

	FString PropertyCppType;
	if (!ExtractRequiredString(Params, TEXT("property_cpp_type"), PropertyCppType, Error))
	{
		return Error.GetValue();
	}

	FString Category;
	if (!ExtractRequiredString(Params, TEXT("category"), Category, Error))
	{
		return Error.GetValue();
	}

	FString ExpectedPreviewSchemaVersion;
	if (!ExtractRequiredString(Params, TEXT("expected_preview_schema_version"), ExpectedPreviewSchemaVersion, Error))
	{
		return Error.GetValue();
	}

	FString ExpectedSourceHashBefore;
	if (!ExtractRequiredString(Params, TEXT("expected_source_hash_before"), ExpectedSourceHashBefore, Error))
	{
		return Error.GetValue();
	}

	const FString DefaultValue = ExtractOptionalString(Params, TEXT("default_value"), TEXT("false"));

	if (!ExpectedPreviewSchemaVersion.Equals(TEXT("reflected_property_declaration_preview_v1"), ESearchCase::CaseSensitive))
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("expected_preview_schema_version '%s' does not match the accepted preview schema 'reflected_property_declaration_preview_v1'. Re-run preview before apply."),
			*ExpectedPreviewSchemaVersion));
	}

	FString ParseError;
	const ECppReflectionKindFilter KindFilter =
		ParseKindFilter(ExtractOptionalString(Params, TEXT("symbol_kind"), TEXT("all")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const ECppReflectionModuleScope ModuleScope =
		ParseModuleScope(ExtractOptionalString(Params, TEXT("module_scope"), TEXT("plugin_only")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const FString ModuleFilter = ExtractOptionalString(Params, TEXT("module_filter"));
	const FCppModuleLocator ModuleLocator;
	FString ResolveError;
	const UObject* ContractObject = ResolveObjectIdentifier(
		SymbolIdentifier,
		KindFilter,
		ModuleScope,
		ModuleFilter,
		ModuleLocator,
		ResolveError);
	if (!ContractObject)
	{
		return FMCPToolResult::Error(ResolveError);
	}

	const UClass* ContractClass = Cast<UClass>(ContractObject);
	if (!ContractClass)
	{
		return FMCPToolResult::Error(TEXT("apply_reflected_property_declaration is currently bounded to plugin-owned UCLASS contracts only."));
	}

	if (DoesClassHierarchyAlreadyDefineProperty(ContractClass, NewMemberName))
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("New member '%s' already exists in reflected class hierarchy '%s'. This bounded declaration lane requires a unique property identifier."),
			*NewMemberName,
			*GetCppSymbolName(ContractObject)));
	}

	TSharedPtr<FJsonObject> ContractSummary = BuildTypeSummaryObject(ContractObject, ModuleLocator, true);
	const TSharedPtr<FJsonObject>* SourceLocationPtr = nullptr;
	if (!ContractSummary->TryGetObjectField(TEXT("source_location"), SourceLocationPtr)
		|| !SourceLocationPtr
		|| !(*SourceLocationPtr).IsValid())
	{
		return FMCPToolResult::Error(TEXT("Resolved contract does not expose a source_location object."));
	}

	FString ModuleOrigin;
	FString HeaderPath;
	(*SourceLocationPtr)->TryGetStringField(TEXT("module_origin"), ModuleOrigin);
	(*SourceLocationPtr)->TryGetStringField(TEXT("resolved_header_path"), HeaderPath);

	if (ModuleOrigin != TEXT("plugin"))
	{
		return FMCPToolResult::Error(TEXT("apply_reflected_property_declaration is bounded to plugin-owned headers only."));
	}

	if (HeaderPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Resolved contract does not expose a concrete plugin header path."));
	}

	FString AnchorResolveError;
	const FProperty* AnchorProperty = ResolveDeclaredPropertyIdentifier(ContractObject, AnchorMemberName, AnchorResolveError);
	if (!AnchorProperty)
	{
		return FMCPToolResult::Error(AnchorResolveError);
	}

	const FCppReflectionPropertyDeclarationPreviewResult PreviewBeforeApply =
		PreviewPluginReflectedPropertyDeclaration(
			HeaderPath,
			AnchorProperty->GetName(),
			NewMemberName,
			PropertyCppType,
			Category,
			DefaultValue);
	if (!PreviewBeforeApply.bSuccess)
	{
		return FMCPToolResult::Error(PreviewBeforeApply.ErrorMessage);
	}

	if (!ExpectedPreviewSchemaVersion.Equals(PreviewBeforeApply.SchemaVersion, ESearchCase::CaseSensitive))
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("expected_preview_schema_version '%s' no longer matches current preview schema '%s'. Re-run preview before apply."),
			*ExpectedPreviewSchemaVersion,
			*PreviewBeforeApply.SchemaVersion));
	}

	const FString CheckpointPath = MakeDeclarationCheckpointPath(GetCppSymbolName(ContractObject), NewMemberName);
	const FCppReflectionPropertyDeclarationApplyResult ApplyResult =
		ApplyPluginReflectedPropertyDeclaration(
			HeaderPath,
			AnchorProperty->GetName(),
			NewMemberName,
			PropertyCppType,
			Category,
			DefaultValue,
			ExpectedSourceHashBefore,
			CheckpointPath);
	if (!ApplyResult.bSuccess)
	{
		return FMCPToolResult::Error(ApplyResult.ErrorMessage);
	}

	const FUnrealClaudeBuildSyncStatus BuildSyncAfter = FUnrealClaudeModule::GetBuildSyncStatus();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("apply_reflected_property_declaration"));
	Data->SetStringField(TEXT("apply_schema_version"), ApplyResult.SchemaVersion);
	Data->SetStringField(TEXT("mutation_lane"), TEXT("single_shape_plugin_owned_reflected_property_declaration_apply"));
	Data->SetStringField(TEXT("declaration_kind"), ApplyResult.DeclarationKind);
	Data->SetStringField(TEXT("discovery_basis"), TEXT("runtime_reflection_uht_metadata_plus_bounded_header_insertion_apply"));
	Data->SetStringField(TEXT("note"), TEXT("Apply writes the plugin-owned header and a checkpoint immediately, but runtime reflection will not prove the new property until a fresh preflight rebuild completes."));
	Data->SetStringField(TEXT("truth_boundary"), TEXT("This apply lane is still bounded to one plugin-owned reflected bool property declaration shape after an existing declared UPROPERTY anchor. It is not arbitrary source editing, and .cpp implementation-body editing remains forbidden."));
	Data->SetBoolField(TEXT("preview_only"), false);
	Data->SetBoolField(TEXT("declaration_apply_available_now"), true);
	Data->SetBoolField(TEXT("implementation_body_editing_allowed"), false);
	// 619 P4 Fix #4 (structural add path): adding a new UPROPERTY changes
	// reflected layout. Live Coding cannot safely re-instance existing
	// objects against a new layout -> requires full restart. Do NOT call
	// TriggerLiveCodingCompile here; surface the agent-facing structured
	// hint so the agent escalates to restart_survival.
	Data->SetBoolField(TEXT("requires_restart"), true);
	Data->SetStringField(TEXT("reason"), TEXT("reflected_uproperty_structural_add"));
	Data->SetStringField(TEXT("recommended_next_step"), TEXT("restart_survival"));
	Data->SetObjectField(TEXT("contract"), ContractSummary);
	Data->SetObjectField(TEXT("anchor_member_before"), BuildPropertyObject(AnchorProperty, true));
	Data->SetObjectField(TEXT("ownership_scope_constraints"), BuildDeclarationOwnershipScopeConstraintsObject(ModuleOrigin, PreviewBeforeApply));
	Data->SetObjectField(TEXT("preview"), PreviewBeforeApply.ToJson());
	Data->SetObjectField(TEXT("preview_continuity"), BuildDeclarationPreviewContinuityObject(PreviewBeforeApply, ApplyResult));
	Data->SetObjectField(TEXT("apply"), ApplyResult.ToJson());
	Data->SetObjectField(TEXT("expected_property_after_rebuild"), BuildExpectedReflectedDeclarationAfterRebuildObject(
		ApplyResult.PropertyName,
		ApplyResult.PropertyCppType,
		ApplyResult.Category,
		ApplyResult.DefaultValueLiteral));
	Data->SetObjectField(TEXT("compile_handshake"), BuildDeclarationCompileHandshakeObject(
		TEXT("apply_reflected_property_declaration"),
		ApplyResult.bApplied,
		BuildSyncBefore,
		BuildSyncAfter));

	const FString ReceiptPath = WriteDeclarationMutationReceipt(
		Data,
		GetCppSymbolName(ContractObject),
		ApplyResult.PropertyName,
		TEXT("declaration_apply"));
	Data->SetStringField(TEXT("receipt_path"), ReceiptPath);
	Data->SetObjectField(TEXT("restore_revert_contract"), BuildDeclarationRestoreRevertContractObject(
		ReceiptPath,
		ApplyResult.CheckpointPath));
	Data->SetObjectField(TEXT("failed_build_diagnostic_contract"), BuildDeclarationFailedBuildDiagnosticContractObject(
		ReceiptPath,
		GetDefaultDeclarationBuildLogPath()));
	FFileHelper::SaveStringToFile(
		SerializeJsonPretty(Data),
		*ReceiptPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	const FString StatusVerb = ApplyResult.bApplied ? TEXT("Applied") : TEXT("No-op");
	return FMCPToolResult::Success(
		FString::Printf(TEXT("%s reflected property declaration for %s after %s"), *StatusVerb, *GetCppSymbolName(ContractObject), *AnchorProperty->GetName()),
		Data);
}

FMCPToolResult FMCPTool_CppReflection::ExecuteRevertReflectedPropertyDeclaration(const TSharedRef<FJsonObject>& Params)
{
	const FUnrealClaudeBuildSyncStatus BuildSyncBefore = FUnrealClaudeModule::GetBuildSyncStatus();

	FString ReceiptPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("receipt_path"), ReceiptPath, Error))
	{
		return Error.GetValue();
	}

	TSharedPtr<FJsonObject> ReceiptObject;
	if (!LoadJsonObjectFromPath(ReceiptPath, ReceiptObject))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load declaration apply receipt '%s'."), *ReceiptPath));
	}

	FString OperationName;
	ReceiptObject->TryGetStringField(TEXT("operation"), OperationName);
	if (!OperationName.Equals(TEXT("apply_reflected_property_declaration"), ESearchCase::CaseSensitive))
	{
		return FMCPToolResult::Error(TEXT("receipt_path does not point at an apply_reflected_property_declaration receipt."));
	}

	const TSharedPtr<FJsonObject>* ContractObjectPtr = nullptr;
	const TSharedPtr<FJsonObject>* ApplyObjectPtr = nullptr;
	if (!ReceiptObject->TryGetObjectField(TEXT("contract"), ContractObjectPtr)
		|| !ContractObjectPtr
		|| !(*ContractObjectPtr).IsValid()
		|| !ReceiptObject->TryGetObjectField(TEXT("apply"), ApplyObjectPtr)
		|| !ApplyObjectPtr
		|| !(*ApplyObjectPtr).IsValid())
	{
		return FMCPToolResult::Error(TEXT("Declaration apply receipt is missing required contract/apply data."));
	}

	FString ContractCppName;
	(*ContractObjectPtr)->TryGetStringField(TEXT("cpp_name"), ContractCppName);

	FString HeaderPath;
	FString PropertyName;
	FString CheckpointPath;
	FString ExpectedAppliedSourceHash;
	FString ExpectedRestoredSourceHash;
	(*ApplyObjectPtr)->TryGetStringField(TEXT("header_path"), HeaderPath);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("new_member_name"), PropertyName);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("checkpoint_path"), CheckpointPath);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("source_hash_after_apply"), ExpectedAppliedSourceHash);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("source_hash_before_apply"), ExpectedRestoredSourceHash);

	if (HeaderPath.IsEmpty() || PropertyName.IsEmpty() || CheckpointPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Declaration apply receipt is missing header_path, new_member_name, or checkpoint_path."));
	}

	const FCppReflectionPropertyDeclarationRevertResult RevertResult =
		RevertPluginReflectedPropertyDeclaration(
			HeaderPath,
			PropertyName,
			CheckpointPath,
			ExpectedAppliedSourceHash,
			ExpectedRestoredSourceHash);
	if (!RevertResult.bSuccess)
	{
		return FMCPToolResult::Error(RevertResult.ErrorMessage);
	}

	const FUnrealClaudeBuildSyncStatus BuildSyncAfter = FUnrealClaudeModule::GetBuildSyncStatus();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("revert_reflected_property_declaration"));
	Data->SetStringField(TEXT("revert_schema_version"), RevertResult.SchemaVersion);
	Data->SetStringField(TEXT("mutation_lane"), TEXT("single_shape_plugin_owned_reflected_property_declaration_revert"));
	Data->SetStringField(TEXT("declaration_kind"), RevertResult.DeclarationKind);
	Data->SetStringField(TEXT("discovery_basis"), TEXT("receipt_backed_checkpoint_restore_for_bounded_header_declaration_apply"));
	Data->SetStringField(TEXT("note"), TEXT("Revert restores the checkpointed plugin-owned header content immediately, but runtime reflection will only prove the property is gone after another fresh preflight rebuild completes."));
	Data->SetStringField(TEXT("truth_boundary"), TEXT("This revert lane only closes out the same bounded reflected bool-property declaration apply shape. It does not imply general source restore or arbitrary native mutation support."));
	Data->SetBoolField(TEXT("preview_only"), false);
	Data->SetBoolField(TEXT("declaration_apply_available_now"), true);
	Data->SetBoolField(TEXT("implementation_body_editing_allowed"), false);
	// 619 P4 Fix #4 (structural revert path): reverting the declaration
	// also changes reflected layout. Additive to the existing
	// `recommended_closeout_status=rebuild_required` field emitted by the
	// compile handshake object below -- not a replacement.
	Data->SetBoolField(TEXT("requires_restart"), true);
	Data->SetStringField(TEXT("reason"), TEXT("reflected_uproperty_structural_revert"));
	Data->SetStringField(TEXT("recommended_next_step"), TEXT("restart_survival"));
	Data->SetStringField(TEXT("receipt_path"), ReceiptPath);
	Data->SetObjectField(TEXT("revert"), RevertResult.ToJson());
	Data->SetObjectField(TEXT("expected_property_after_rebuild"), BuildExpectedDeclarationAbsenceAfterRebuildObject(PropertyName));
	Data->SetObjectField(TEXT("compile_handshake"), BuildDeclarationCompileHandshakeObject(
		TEXT("revert_reflected_property_declaration"),
		RevertResult.bReverted,
		BuildSyncBefore,
		BuildSyncAfter));

	const FString RevertReceiptPath = WriteDeclarationMutationReceipt(
		Data,
		ContractCppName.IsEmpty() ? TEXT("contract") : ContractCppName,
		PropertyName,
		TEXT("declaration_revert"));
	Data->SetStringField(TEXT("revert_receipt_path"), RevertReceiptPath);
	FFileHelper::SaveStringToFile(
		SerializeJsonPretty(Data),
		*RevertReceiptPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Reverted reflected property declaration for %s.%s"), *ContractCppName, *PropertyName),
		Data);
}

FMCPToolResult FMCPTool_CppReflection::ExecuteInspectReflectedPropertyDeclarationBuildFailure(const TSharedRef<FJsonObject>& Params)
{
	const FUnrealClaudeBuildSyncStatus BuildSyncCurrent = FUnrealClaudeModule::GetBuildSyncStatus();

	FString ReceiptPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("receipt_path"), ReceiptPath, Error))
	{
		return Error.GetValue();
	}

	const FString BuildLogPath = ExtractOptionalString(Params, TEXT("build_log_path"), GetDefaultDeclarationBuildLogPath());

	TSharedPtr<FJsonObject> ReceiptObject;
	if (!LoadJsonObjectFromPath(ReceiptPath, ReceiptObject))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load declaration apply receipt '%s'."), *ReceiptPath));
	}

	FString OperationName;
	ReceiptObject->TryGetStringField(TEXT("operation"), OperationName);
	if (!OperationName.Equals(TEXT("apply_reflected_property_declaration"), ESearchCase::CaseSensitive))
	{
		return FMCPToolResult::Error(TEXT("receipt_path does not point at an apply_reflected_property_declaration receipt."));
	}

	const TSharedPtr<FJsonObject>* ContractObjectPtr = nullptr;
	const TSharedPtr<FJsonObject>* ApplyObjectPtr = nullptr;
	if (!ReceiptObject->TryGetObjectField(TEXT("contract"), ContractObjectPtr)
		|| !ContractObjectPtr
		|| !(*ContractObjectPtr).IsValid()
		|| !ReceiptObject->TryGetObjectField(TEXT("apply"), ApplyObjectPtr)
		|| !ApplyObjectPtr
		|| !(*ApplyObjectPtr).IsValid())
	{
		return FMCPToolResult::Error(TEXT("Declaration apply receipt is missing required contract/apply data."));
	}

	FString ContractCppName;
	(*ContractObjectPtr)->TryGetStringField(TEXT("cpp_name"), ContractCppName);

	FString HeaderPath;
	FString PropertyName;
	FString PropertyCppType;
	FString Category;
	FString DefaultValueLiteral;
	FString GeneratedDeclaration;
	FString GeneratedSnippet;
	FString CheckpointPath;
	FString ExpectedAppliedSourceHash;
	int32 InsertionLine = INDEX_NONE;
	double InsertionLineNumber = 0.0;
	(*ApplyObjectPtr)->TryGetStringField(TEXT("header_path"), HeaderPath);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("new_member_name"), PropertyName);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("property_cpp_type"), PropertyCppType);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("category"), Category);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("default_value"), DefaultValueLiteral);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("generated_declaration"), GeneratedDeclaration);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("generated_snippet"), GeneratedSnippet);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("checkpoint_path"), CheckpointPath);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("source_hash_after_apply"), ExpectedAppliedSourceHash);
	if ((*ApplyObjectPtr)->TryGetNumberField(TEXT("insertion_line"), InsertionLineNumber))
	{
		InsertionLine = static_cast<int32>(InsertionLineNumber);
	}

	if (HeaderPath.IsEmpty() || PropertyName.IsEmpty() || CheckpointPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Declaration apply receipt is missing header_path, new_member_name, or checkpoint_path."));
	}

	TArray<FString> GeneratedSnippetLines;
	GeneratedSnippet.ParseIntoArrayLines(GeneratedSnippetLines, true);
	const int32 GeneratedLineCount = GeneratedSnippetLines.Num() > 0 ? GeneratedSnippetLines.Num() : 1;

	FReceiptLinkedBuildArtifactContext ParseContext;
	ParseContext.HeaderPath = HeaderPath;
	ParseContext.MemberName = PropertyName;
	ParseContext.GeneratedDeclaration = GeneratedDeclaration;
	ParseContext.GeneratedSnippet = GeneratedSnippet;
	if (InsertionLine != INDEX_NONE)
	{
		ParseContext.ReceiptLineWindowStart = InsertionLine;
		ParseContext.ReceiptLineWindowEnd = InsertionLine + GeneratedLineCount - 1;
	}

	const FReceiptLinkedBuildArtifactParseResult ParseResult =
		ParseReceiptLinkedBuildArtifact(BuildLogPath, ParseContext);
	const TSharedPtr<FJsonObject> CurrentSourceState = BuildDeclarationCurrentSourceStateObject(
		HeaderPath,
		CheckpointPath,
		ExpectedAppliedSourceHash);

	bool bRevertAvailableNow = false;
	CurrentSourceState->TryGetBoolField(TEXT("revert_available_now"), bRevertAvailableNow);

	const FString FailureStateClassification =
		!ParseResult.bBuildFailureObserved
			? TEXT("build_failure_not_observed")
			: (ParseResult.MatchingErrorCount > 0
				? TEXT("mutation_written_but_build_failed")
				: TEXT("build_failed_but_receipt_linkage_not_proven"));
	const FString ReflectionStateClassification =
		ParseResult.bBuildFailureObserved
			? TEXT("reflection_not_yet_proven_due_to_build_failure")
			: TEXT("reflection_state_not_advanced");
	const FString RevertStateClassification =
		bRevertAvailableNow ? TEXT("revert_available") : TEXT("revert_not_yet_proven");

	TSharedPtr<FJsonObject> ReflectionState = MakeShared<FJsonObject>();
	ReflectionState->SetStringField(TEXT("classification"), ReflectionStateClassification);
	ReflectionState->SetBoolField(TEXT("reflection_proven"), false);
	ReflectionState->SetStringField(
		TEXT("reason"),
		ParseResult.bBuildFailureObserved
			? TEXT("The bounded declaration was written, but failed-build diagnostics mean reflected runtime readback is still pending until the source is restored or fixed and a fresh rebuild succeeds.")
			: TEXT("No failed-build artifact was observed, so reflection state did not advance through this diagnostic path."));
	ReflectionState->SetObjectField(TEXT("build_status_current"), BuildBuildSyncObject(BuildSyncCurrent));
	ReflectionState->SetObjectField(TEXT("expected_property_after_successful_rebuild"), BuildExpectedReflectedDeclarationAfterRebuildObject(
		PropertyName,
		PropertyCppType,
		Category,
		DefaultValueLiteral));

	TSharedPtr<FJsonObject> RestoreRevertContract = BuildDeclarationRestoreRevertContractObject(ReceiptPath, CheckpointPath);
	RestoreRevertContract->SetStringField(
		TEXT("status"),
		bRevertAvailableNow ? TEXT("revert_available_after_failed_build") : TEXT("revert_not_yet_proven_after_failed_build"));
	RestoreRevertContract->SetStringField(TEXT("revert_state_classification"), RevertStateClassification);
	RestoreRevertContract->SetStringField(TEXT("build_log_path"), BuildLogPath);

	const TSharedPtr<FJsonObject> FailureArtifactObject = ParseResult.ToJson();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("inspect_reflected_property_declaration_build_failure"));
	Data->SetStringField(TEXT("diagnostic_schema_version"), GetPropertyDeclarationBuildFailureDiagnosticSchemaVersion());
	Data->SetStringField(TEXT("mutation_lane"), TEXT("single_shape_plugin_owned_reflected_property_declaration_failed_build_diagnostics"));
	Data->SetStringField(TEXT("declaration_kind"), TEXT("reflected_uproperty_member_declaration"));
	Data->SetStringField(TEXT("discovery_basis"), TEXT("receipt_linked_build_artifact_parse_for_bounded_declaration_apply"));
	Data->SetStringField(TEXT("note"), TEXT("This inspection path links one failed-build artifact back to the bounded declaration apply receipt and keeps rebuild/revert truth explicit instead of treating the declaration as successfully proven."));
	Data->SetStringField(TEXT("truth_boundary"), TEXT("This diagnostic lane only inspects failed-build evidence for the same bounded plugin-owned reflected bool-property declaration shape. It does not widen declaration authoring breadth, does not claim successful reflection after failure, and does not allow arbitrary native source editing."));
	Data->SetBoolField(TEXT("preview_only"), false);
	Data->SetBoolField(TEXT("declaration_apply_available_now"), true);
	Data->SetBoolField(TEXT("implementation_body_editing_allowed"), false);
	Data->SetStringField(TEXT("receipt_path"), ReceiptPath);
	Data->SetStringField(TEXT("build_log_path"), BuildLogPath);
	Data->SetStringField(TEXT("failure_state_classification"), FailureStateClassification);
	Data->SetStringField(TEXT("revert_state_classification"), RevertStateClassification);
	Data->SetStringField(TEXT("reflection_state_classification"), ReflectionStateClassification);
	Data->SetStringField(TEXT("diagnostic_linkage_confidence"), ParseResult.LinkageConfidence);
	Data->SetObjectField(TEXT("contract"), *ContractObjectPtr);
	Data->SetObjectField(TEXT("apply"), *ApplyObjectPtr);
	const TSharedPtr<FJsonObject>* ApplyHandshakePtr = nullptr;
	if (ReceiptObject->TryGetObjectField(TEXT("compile_handshake"), ApplyHandshakePtr)
		&& ApplyHandshakePtr
		&& (*ApplyHandshakePtr).IsValid())
	{
		Data->SetObjectField(TEXT("apply_compile_handshake"), *ApplyHandshakePtr);
	}
	Data->SetObjectField(TEXT("build_diagnostic_artifact"), FailureArtifactObject);
	Data->SetObjectField(TEXT("current_source_state"), CurrentSourceState);
	Data->SetObjectField(TEXT("reflection_state"), ReflectionState);
	Data->SetObjectField(TEXT("restore_revert_contract"), RestoreRevertContract);
	Data->SetObjectField(TEXT("recommended_closeout_sequence"), BuildDeclarationFailureCloseoutSequenceObject(bRevertAvailableNow));

	const FString DiagnosticReceiptPath = WriteDeclarationMutationReceipt(
		Data,
		ContractCppName.IsEmpty() ? TEXT("contract") : ContractCppName,
		PropertyName,
		TEXT("declaration_build_failure_diagnostic"));
	Data->SetStringField(TEXT("diagnostic_receipt_path"), DiagnosticReceiptPath);
	FFileHelper::SaveStringToFile(
		SerializeJsonPretty(Data),
		*DiagnosticReceiptPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Inspected receipt-linked failed-build diagnostics for %s.%s"), *ContractCppName, *PropertyName),
		Data);
}

FMCPToolResult FMCPTool_CppReflection::ExecuteBuildReflectedPropertyDeclarationEvidenceBundle(const TSharedRef<FJsonObject>& Params)
{
	FString ReceiptPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("receipt_path"), ReceiptPath, Error))
	{
		return Error.GetValue();
	}

	const FString PreviewResultPath = ExtractOptionalString(Params, TEXT("preview_result_path"));
	const FString FailedBuildResultPath = ExtractOptionalString(Params, TEXT("failed_build_result_path"));
	const FString RevertResultPath = ExtractOptionalString(Params, TEXT("revert_result_path"));
	const FString ReflectionReadbackPath = ExtractOptionalString(Params, TEXT("reflection_readback_path"));
	const bool bExportReport = ExtractOptionalBool(Params, TEXT("export_report"), false);
	const FString CustomReportName = ExtractOptionalString(Params, TEXT("report_name"));
	const FString CustomReportSlug = ExtractOptionalString(Params, TEXT("report_slug"));

	TSharedPtr<FJsonObject> ReceiptObject;
	if (!LoadJsonObjectFromPath(ReceiptPath, ReceiptObject))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load declaration apply receipt '%s'."), *ReceiptPath));
	}

	FString OperationName;
	ReceiptObject->TryGetStringField(TEXT("operation"), OperationName);
	if (!OperationName.Equals(TEXT("apply_reflected_property_declaration"), ESearchCase::CaseSensitive))
	{
		return FMCPToolResult::Error(TEXT("receipt_path does not point at an apply_reflected_property_declaration receipt."));
	}

	const TSharedPtr<FJsonObject>* ContractObjectPtr = nullptr;
	const TSharedPtr<FJsonObject>* ApplyObjectPtr = nullptr;
	if (!ReceiptObject->TryGetObjectField(TEXT("contract"), ContractObjectPtr)
		|| !ContractObjectPtr
		|| !(*ContractObjectPtr).IsValid()
		|| !ReceiptObject->TryGetObjectField(TEXT("apply"), ApplyObjectPtr)
		|| !ApplyObjectPtr
		|| !(*ApplyObjectPtr).IsValid())
	{
		return FMCPToolResult::Error(TEXT("Declaration apply receipt is missing required contract/apply data."));
	}

	TSharedPtr<FJsonObject> PreviewResultObject;
	FString LoadError;
	if (!LoadOperationResultObjectFromPath(
		PreviewResultPath,
		TEXT("preview_reflected_property_declaration"),
		PreviewResultObject,
		LoadError))
	{
		return FMCPToolResult::Error(LoadError);
	}

	TSharedPtr<FJsonObject> FailedBuildResultObject;
	if (!LoadOperationResultObjectFromPath(
		FailedBuildResultPath,
		TEXT("inspect_reflected_property_declaration_build_failure"),
		FailedBuildResultObject,
		LoadError))
	{
		return FMCPToolResult::Error(LoadError);
	}

	TSharedPtr<FJsonObject> RevertResultObject;
	if (!LoadOperationResultObjectFromPath(
		RevertResultPath,
		TEXT("revert_reflected_property_declaration"),
		RevertResultObject,
		LoadError))
	{
		return FMCPToolResult::Error(LoadError);
	}

	TSharedPtr<FJsonObject> ReflectionReadbackObject;
	if (!LoadOperationResultObjectFromPath(
		ReflectionReadbackPath,
		TEXT("get_reflected_contract"),
		ReflectionReadbackObject,
		LoadError))
	{
		return FMCPToolResult::Error(LoadError);
	}

	FString ContractCppName;
	FString ContractPath;
	(*ContractObjectPtr)->TryGetStringField(TEXT("cpp_name"), ContractCppName);
	(*ContractObjectPtr)->TryGetStringField(TEXT("path"), ContractPath);

	FString PropertyName;
	FString HeaderPath;
	FString AnchorMemberName;
	FString PropertyCppType;
	FString Category;
	FString DefaultValueLiteral;
	FString GeneratedDeclaration;
	FString GeneratedSnippet;
	FString ApplySchemaVersion;
	FString PreviewSchemaVersion;
	FString CheckpointPath;
	FString SourceHashBefore;
	FString SourceHashAfterApply;
	(*ApplyObjectPtr)->TryGetStringField(TEXT("new_member_name"), PropertyName);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("header_path"), HeaderPath);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("anchor_member_name"), AnchorMemberName);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("property_cpp_type"), PropertyCppType);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("category"), Category);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("default_value"), DefaultValueLiteral);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("generated_declaration"), GeneratedDeclaration);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("generated_snippet"), GeneratedSnippet);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("schema_version"), ApplySchemaVersion);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("preview_schema_version"), PreviewSchemaVersion);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("checkpoint_path"), CheckpointPath);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("expected_source_hash_before"), SourceHashBefore);
	(*ApplyObjectPtr)->TryGetStringField(TEXT("source_hash_after_apply"), SourceHashAfterApply);

	if (PropertyName.IsEmpty() || HeaderPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Declaration apply receipt is missing new_member_name or header_path."));
	}

	TSharedPtr<FJsonObject> PreviewEvidenceObject = MakeShared<FJsonObject>();
	PreviewEvidenceObject->SetStringField(TEXT("schema_version"), PreviewSchemaVersion);
	PreviewEvidenceObject->SetStringField(TEXT("property_name"), PropertyName);
	PreviewEvidenceObject->SetStringField(TEXT("anchor_member_name"), AnchorMemberName);
	PreviewEvidenceObject->SetStringField(TEXT("property_cpp_type"), PropertyCppType);
	PreviewEvidenceObject->SetStringField(TEXT("category"), Category);
	PreviewEvidenceObject->SetStringField(TEXT("default_value"), DefaultValueLiteral);
	PreviewEvidenceObject->SetStringField(TEXT("generated_declaration"), GeneratedDeclaration);
	PreviewEvidenceObject->SetStringField(TEXT("generated_snippet"), GeneratedSnippet);
	PreviewEvidenceObject->SetStringField(TEXT("expected_source_hash_before"), SourceHashBefore);
	PreviewEvidenceObject->SetStringField(TEXT("source"), PreviewResultObject.IsValid() ? TEXT("preview_result") : TEXT("apply_receipt_preview_continuity"));
	if (PreviewResultObject.IsValid())
	{
		PreviewEvidenceObject->SetStringField(TEXT("source_path"), PreviewResultPath);
		const TSharedPtr<FJsonObject>* PreviewObjectPtr = nullptr;
		if (PreviewResultObject->TryGetObjectField(TEXT("preview"), PreviewObjectPtr) && PreviewObjectPtr && (*PreviewObjectPtr).IsValid())
		{
			const TSharedPtr<FJsonObject>& ExplicitPreviewObject = *PreviewObjectPtr;
			FString ExplicitPreviewSchemaVersion;
			FString PreviewExcerptAfter;
			ExplicitPreviewObject->TryGetStringField(TEXT("schema_version"), ExplicitPreviewSchemaVersion);
			ExplicitPreviewObject->TryGetStringField(TEXT("generated_declaration"), GeneratedDeclaration);
			ExplicitPreviewObject->TryGetStringField(TEXT("generated_snippet"), GeneratedSnippet);
			ExplicitPreviewObject->TryGetStringField(TEXT("preview_excerpt_after"), PreviewExcerptAfter);
			if (!ExplicitPreviewSchemaVersion.IsEmpty())
			{
				PreviewEvidenceObject->SetStringField(TEXT("schema_version"), ExplicitPreviewSchemaVersion);
			}
			if (!GeneratedDeclaration.IsEmpty())
			{
				PreviewEvidenceObject->SetStringField(TEXT("generated_declaration"), GeneratedDeclaration);
			}
			if (!GeneratedSnippet.IsEmpty())
			{
				PreviewEvidenceObject->SetStringField(TEXT("generated_snippet"), GeneratedSnippet);
			}
			if (!PreviewExcerptAfter.IsEmpty())
			{
				PreviewEvidenceObject->SetStringField(TEXT("preview_excerpt_after"), PreviewExcerptAfter);
			}
		}
	}

	TSharedPtr<FJsonObject> MutationReceiptObject = MakeShared<FJsonObject>();
	MutationReceiptObject->SetStringField(TEXT("receipt_path"), ReceiptPath);
	MutationReceiptObject->SetStringField(TEXT("schema_version"), ApplySchemaVersion);
	MutationReceiptObject->SetStringField(TEXT("property_name"), PropertyName);
	MutationReceiptObject->SetStringField(TEXT("header_path"), HeaderPath);
	MutationReceiptObject->SetStringField(TEXT("anchor_member_name"), AnchorMemberName);
	MutationReceiptObject->SetStringField(TEXT("checkpoint_path"), CheckpointPath);
	MutationReceiptObject->SetStringField(TEXT("source_hash_before"), SourceHashBefore);
	MutationReceiptObject->SetStringField(TEXT("source_hash_after_apply"), SourceHashAfterApply);
	MutationReceiptObject->SetStringField(TEXT("property_cpp_type"), PropertyCppType);
	MutationReceiptObject->SetStringField(TEXT("category"), Category);
	MutationReceiptObject->SetStringField(TEXT("default_value"), DefaultValueLiteral);

	TSharedPtr<FJsonObject> CompileHandshakeObject = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* ApplyHandshakePtr = nullptr;
	if (ReceiptObject->TryGetObjectField(TEXT("compile_handshake"), ApplyHandshakePtr)
		&& ApplyHandshakePtr
		&& (*ApplyHandshakePtr).IsValid())
	{
		CompileHandshakeObject->SetObjectField(TEXT("apply"), *ApplyHandshakePtr);
	}

	TSharedPtr<FJsonObject> FailedBuildBundleObject;
	if (FailedBuildResultObject.IsValid())
	{
		FailedBuildBundleObject = MakeShared<FJsonObject>();
		FailedBuildBundleObject->SetStringField(TEXT("source_path"), FailedBuildResultPath);
		FailedBuildBundleObject->SetStringField(TEXT("diagnostic_linkage_confidence"), ExtractObjectStringField(FailedBuildResultObject, TEXT("diagnostic_linkage_confidence")));
		FailedBuildBundleObject->SetStringField(TEXT("failure_state_classification"), ExtractObjectStringField(FailedBuildResultObject, TEXT("failure_state_classification")));
		FailedBuildBundleObject->SetStringField(TEXT("reflection_state_classification"), ExtractObjectStringField(FailedBuildResultObject, TEXT("reflection_state_classification")));
		FailedBuildBundleObject->SetStringField(TEXT("revert_state_classification"), ExtractObjectStringField(FailedBuildResultObject, TEXT("revert_state_classification")));

		FString RecommendedCloseoutStatus = ExtractObjectStringField(FailedBuildResultObject, TEXT("recommended_closeout_status"));
		const TSharedPtr<FJsonObject>* RecommendedCloseoutSequencePtr = nullptr;
		if (FailedBuildResultObject->TryGetObjectField(TEXT("recommended_closeout_sequence"), RecommendedCloseoutSequencePtr)
			&& RecommendedCloseoutSequencePtr
			&& (*RecommendedCloseoutSequencePtr).IsValid())
		{
			const TSharedPtr<FJsonObject>& RecommendedCloseoutSequenceObject = *RecommendedCloseoutSequencePtr;
			if (RecommendedCloseoutStatus.IsEmpty())
			{
				RecommendedCloseoutSequenceObject->TryGetStringField(TEXT("status"), RecommendedCloseoutStatus);
			}

			FailedBuildBundleObject->SetObjectField(TEXT("recommended_closeout_sequence"), RecommendedCloseoutSequenceObject);
		}
		FailedBuildBundleObject->SetStringField(TEXT("recommended_closeout_status"), RecommendedCloseoutStatus);

		const TSharedPtr<FJsonObject>* FailureArtifactPtr = nullptr;
		if (FailedBuildResultObject->TryGetObjectField(TEXT("build_diagnostic_artifact"), FailureArtifactPtr)
			&& FailureArtifactPtr
			&& (*FailureArtifactPtr).IsValid())
		{
			const TSharedPtr<FJsonObject>& FailureArtifactObject = *FailureArtifactPtr;
			FailedBuildBundleObject->SetObjectField(TEXT("artifact"), FailureArtifactObject);

			TSharedPtr<FJsonObject> PresentationObject = MakeShared<FJsonObject>();
			PresentationObject->SetStringField(
				TEXT("quality_note"),
				TEXT("Localized compiler detail may remain raw or mojibake-like in some logs. Readable summaries below are derived from code/location fields without inventing missing text."));

			const TSharedPtr<FJsonObject>* FirstMatchingErrorPtr = nullptr;
			if (FailureArtifactObject->TryGetObjectField(TEXT("first_matching_error"), FirstMatchingErrorPtr)
				&& FirstMatchingErrorPtr
				&& (*FirstMatchingErrorPtr).IsValid())
			{
				const FString ReadableFirstMatch = BuildReadableDiagnosticSummary(*FirstMatchingErrorPtr);
				PresentationObject->SetStringField(TEXT("first_matching_error_display"), ReadableFirstMatch);
			}

			TArray<FString> MatchingSamples;
			const TArray<TSharedPtr<FJsonValue>>* ErrorsArray = nullptr;
			if (FailureArtifactObject->TryGetArrayField(TEXT("errors"), ErrorsArray) && ErrorsArray)
			{
				for (const TSharedPtr<FJsonValue>& ErrorValue : *ErrorsArray)
				{
					const TSharedPtr<FJsonObject> ErrorObject = ErrorValue.IsValid() ? ErrorValue->AsObject() : nullptr;
					if (!ErrorObject.IsValid())
					{
						continue;
					}

					bool bMatchesHeader = false;
					bool bMatchesLineContext = false;
					bool bMatchesMember = false;
					bool bMatchesGeneratedDeclaration = false;
					bool bMatchesGeneratedSnippet = false;
					ErrorObject->TryGetBoolField(TEXT("matches_receipt_header_path"), bMatchesHeader);
					ErrorObject->TryGetBoolField(TEXT("matches_receipt_line_context"), bMatchesLineContext);
					ErrorObject->TryGetBoolField(TEXT("matches_receipt_member_name"), bMatchesMember);
					ErrorObject->TryGetBoolField(TEXT("matches_receipt_generated_declaration"), bMatchesGeneratedDeclaration);
					ErrorObject->TryGetBoolField(TEXT("matches_receipt_generated_snippet"), bMatchesGeneratedSnippet);

					if (!(bMatchesHeader || bMatchesLineContext || bMatchesMember || bMatchesGeneratedDeclaration || bMatchesGeneratedSnippet))
					{
						continue;
					}

					const FString ReadableSummary = BuildReadableDiagnosticSummary(ErrorObject);
					if (!ReadableSummary.IsEmpty() && !MatchingSamples.Contains(ReadableSummary))
					{
						MatchingSamples.Add(ReadableSummary);
					}

					if (MatchingSamples.Num() >= 5)
					{
						break;
					}
				}
			}

			SetStringArrayField(PresentationObject, TEXT("matching_error_display_samples"), MatchingSamples);
			FailedBuildBundleObject->SetObjectField(TEXT("presentation"), PresentationObject);
		}
	}

	TSharedPtr<FJsonObject> RevertBundleObject;
	if (RevertResultObject.IsValid())
	{
		RevertBundleObject = MakeShared<FJsonObject>();
		RevertBundleObject->SetStringField(TEXT("source_path"), RevertResultPath);
		RevertBundleObject->SetStringField(TEXT("revert_schema_version"), ExtractObjectStringField(RevertResultObject, TEXT("revert_schema_version")));
		RevertBundleObject->SetStringField(TEXT("revert_receipt_path"), ExtractObjectStringField(RevertResultObject, TEXT("revert_receipt_path")));
		const TSharedPtr<FJsonObject>* RevertObjectPtr = nullptr;
		if (RevertResultObject->TryGetObjectField(TEXT("revert"), RevertObjectPtr)
			&& RevertObjectPtr
			&& (*RevertObjectPtr).IsValid())
		{
			RevertBundleObject->SetObjectField(TEXT("revert"), *RevertObjectPtr);
		}

		const TSharedPtr<FJsonObject>* RevertHandshakePtr = nullptr;
		if (RevertResultObject->TryGetObjectField(TEXT("compile_handshake"), RevertHandshakePtr)
			&& RevertHandshakePtr
			&& (*RevertHandshakePtr).IsValid())
		{
			CompileHandshakeObject->SetObjectField(TEXT("revert"), *RevertHandshakePtr);
		}
	}

	TSharedPtr<FJsonObject> ReflectionReadbackBundleObject;
	bool bPropertyPresentInReadback = false;
	if (ReflectionReadbackObject.IsValid())
	{
		ReflectionReadbackBundleObject = MakeShared<FJsonObject>();
		ReflectionReadbackBundleObject->SetStringField(TEXT("source_path"), ReflectionReadbackPath);
		ReflectionReadbackBundleObject->SetStringField(TEXT("operation"), TEXT("get_reflected_contract"));
		ReflectionReadbackBundleObject->SetStringField(TEXT("property_name"), PropertyName);

		const TSharedPtr<FJsonObject>* ReadbackContractPtr = nullptr;
		if (ReflectionReadbackObject->TryGetObjectField(TEXT("contract"), ReadbackContractPtr)
			&& ReadbackContractPtr
			&& (*ReadbackContractPtr).IsValid())
		{
			const TSharedPtr<FJsonObject>& ReadbackContractObject = *ReadbackContractPtr;
			ReflectionReadbackBundleObject->SetObjectField(TEXT("contract"), ReadbackContractObject);

			const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
			if (ReadbackContractObject->TryGetArrayField(TEXT("properties"), PropertiesArray) && PropertiesArray)
			{
				for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
				{
					const TSharedPtr<FJsonObject> PropertyObject = PropertyValue.IsValid() ? PropertyValue->AsObject() : nullptr;
					if (!PropertyObject.IsValid())
					{
						continue;
					}

					FString ReadbackPropertyName;
					PropertyObject->TryGetStringField(TEXT("name"), ReadbackPropertyName);
					if (ReadbackPropertyName == PropertyName)
					{
						bPropertyPresentInReadback = true;
						ReflectionReadbackBundleObject->SetObjectField(TEXT("matched_property"), PropertyObject);
						break;
					}
				}
			}

			const TSharedPtr<FJsonObject>* MemberCountsPtr = nullptr;
			if (ReadbackContractObject->TryGetObjectField(TEXT("member_counts"), MemberCountsPtr)
				&& MemberCountsPtr
				&& (*MemberCountsPtr).IsValid())
			{
				double DeclaredPropertyCount = 0.0;
				if ((*MemberCountsPtr)->TryGetNumberField(TEXT("declared_properties"), DeclaredPropertyCount))
				{
					ReflectionReadbackBundleObject->SetNumberField(TEXT("declared_properties"), DeclaredPropertyCount);
				}
			}
		}

		ReflectionReadbackBundleObject->SetBoolField(TEXT("property_present"), bPropertyPresentInReadback);
		ReflectionReadbackBundleObject->SetStringField(
			TEXT("readback_classification"),
			bPropertyPresentInReadback
				? TEXT("post_build_property_present")
				: (RevertResultObject.IsValid()
					? TEXT("post_revert_property_absent")
					: TEXT("property_absent_in_supplied_readback")));
	}

	TArray<FString> StateLabels;
	if (ReflectionReadbackObject.IsValid() && bPropertyPresentInReadback)
	{
		StateLabels.Add(TEXT("success_path_authoring_cycle"));
	}
	if (FailedBuildResultObject.IsValid())
	{
		StateLabels.Add(TEXT("failed_build_cycle"));
	}
	if (RevertResultObject.IsValid())
	{
		StateLabels.Add(TEXT("revert_closeout_cycle"));
	}
	if (StateLabels.Num() == 0)
	{
		StateLabels.Add(TEXT("receipt_backed_evidence_bundle"));
	}

	TSharedPtr<FJsonObject> SourcesObject = MakeShared<FJsonObject>();
	SourcesObject->SetStringField(TEXT("receipt_path"), ReceiptPath);
	SourcesObject->SetStringField(TEXT("preview_result_path"), PreviewResultPath);
	SourcesObject->SetStringField(TEXT("failed_build_result_path"), FailedBuildResultPath);
	SourcesObject->SetStringField(TEXT("revert_result_path"), RevertResultPath);
	SourcesObject->SetStringField(TEXT("reflection_readback_path"), ReflectionReadbackPath);

	TSharedPtr<FJsonObject> TruthBoundaryObject = MakeShared<FJsonObject>();
	TruthBoundaryObject->SetStringField(TEXT("lane_scope"), TEXT("single_plugin_owned_reflected_bool_property_declaration_lane"));
	TruthBoundaryObject->SetBoolField(TEXT("supports_only_current_bounded_declaration_shape"), true);
	TruthBoundaryObject->SetBoolField(TEXT("broader_declaration_families_available_now"), false);
	TruthBoundaryObject->SetBoolField(TEXT("arbitrary_cpp_implementation_body_editing_allowed"), false);
	TruthBoundaryObject->SetBoolField(TEXT("reflection_success_requires_post_build_readback"), true);
	TruthBoundaryObject->SetBoolField(TEXT("failed_build_truth_requires_receipt_linked_diagnostics"), true);
	TArray<FString> NonClaims;
	NonClaims.Add(TEXT("broader_declaration_authoring"));
	NonClaims.Add(TEXT("function_declarations"));
	NonClaims.Add(TEXT("batch_declaration_authoring"));
	NonClaims.Add(TEXT("arbitrary_cpp_implementation_body_editing"));
	SetStringArrayField(TruthBoundaryObject, TEXT("non_claims"), NonClaims);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("build_reflected_property_declaration_evidence_bundle"));
	Data->SetStringField(TEXT("bundle_schema_version"), GetPropertyDeclarationEvidenceBundleSchemaVersion());
	Data->SetStringField(TEXT("mutation_lane"), TEXT("single_shape_plugin_owned_reflected_property_declaration_evidence_bundle"));
	Data->SetStringField(TEXT("declaration_kind"), TEXT("reflected_uproperty_member_declaration"));
	Data->SetStringField(TEXT("discovery_basis"), TEXT("receipt_backed_result_bundle_for_bounded_reflected_property_declaration_lane"));
	Data->SetStringField(TEXT("note"), TEXT("This evidence bundle normalizes preview/apply/failure/revert/readback evidence for the same bounded plugin-owned reflected bool-property declaration lane into one machine-readable surface."));
	Data->SetStringField(TEXT("truth_boundary"), TEXT("This bundle is limited to the already accepted bounded reflected bool-property declaration lane. It does not widen declaration families, does not replace rebuild/readback proof, and does not allow arbitrary native source editing."));
	Data->SetStringField(TEXT("contract_cpp_name"), ContractCppName);
	Data->SetStringField(TEXT("contract_path"), ContractPath);
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetStringField(TEXT("header_path"), HeaderPath);
	SetStringArrayField(Data, TEXT("bundle_state_labels"), StateLabels);
	Data->SetObjectField(TEXT("evidence_sources"), SourcesObject);
	Data->SetObjectField(TEXT("preview_evidence"), PreviewEvidenceObject);
	Data->SetObjectField(TEXT("mutation_receipt"), MutationReceiptObject);
	Data->SetObjectField(TEXT("compile_handshake"), CompileHandshakeObject);
	if (FailedBuildBundleObject.IsValid())
	{
		Data->SetObjectField(TEXT("failed_build_diagnostic"), FailedBuildBundleObject);
	}
	if (RevertBundleObject.IsValid())
	{
		Data->SetObjectField(TEXT("revert_receipt"), RevertBundleObject);
	}
	if (ReflectionReadbackBundleObject.IsValid())
	{
		Data->SetObjectField(TEXT("reflection_readback"), ReflectionReadbackBundleObject);
	}
	Data->SetObjectField(TEXT("narrowed_truth_boundary"), TruthBoundaryObject);

	const FString EvidenceBundlePath = WriteDeclarationMutationReceipt(
		Data,
		ContractCppName.IsEmpty() ? TEXT("contract") : ContractCppName,
		PropertyName,
		TEXT("declaration_evidence_bundle"));
	Data->SetStringField(TEXT("evidence_bundle_path"), EvidenceBundlePath);
	FFileHelper::SaveStringToFile(
		SerializeJsonPretty(Data),
		*EvidenceBundlePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	FMCPToolResult Result = FMCPToolResult::Success(
		FString::Printf(
			TEXT("Built bounded declaration evidence bundle for %s.%s [%s]"),
			*ContractCppName,
			*PropertyName,
			*FString::Join(StateLabels, TEXT(", "))),
		Data);
	return MaybeAttachDeclarationEvidenceBundleReportArtifact(Result, bExportReport, CustomReportName, CustomReportSlug);
}

FMCPToolResult FMCPTool_CppReflection::ExecutePreviewPropertyMetadataMutation(const TSharedRef<FJsonObject>& Params)
{
	FString SymbolIdentifier;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("symbol"), SymbolIdentifier, Error))
	{
		return Error.GetValue();
	}

	FString MemberName;
	if (!ExtractRequiredString(Params, TEXT("member_name"), MemberName, Error))
	{
		return Error.GetValue();
	}

	FString MetadataKey;
	if (!ExtractRequiredString(Params, TEXT("metadata_key"), MetadataKey, Error))
	{
		return Error.GetValue();
	}

	FString MetadataValue;
	if (!ExtractRequiredString(Params, TEXT("metadata_value"), MetadataValue, Error))
	{
		return Error.GetValue();
	}

	FString ParseError;
	const ECppReflectionKindFilter KindFilter =
		ParseKindFilter(ExtractOptionalString(Params, TEXT("symbol_kind"), TEXT("all")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const ECppReflectionModuleScope ModuleScope =
		ParseModuleScope(ExtractOptionalString(Params, TEXT("module_scope"), TEXT("plugin_only")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const FString ModuleFilter = ExtractOptionalString(Params, TEXT("module_filter"));
	const FCppModuleLocator ModuleLocator;
	FString ResolveError;
	const UObject* ContractObject = ResolveObjectIdentifier(
		SymbolIdentifier,
		KindFilter,
		ModuleScope,
		ModuleFilter,
		ModuleLocator,
		ResolveError);
	if (!ContractObject)
	{
		return FMCPToolResult::Error(ResolveError);
	}

	TSharedPtr<FJsonObject> ContractSummary = BuildTypeSummaryObject(ContractObject, ModuleLocator, true);
	const TSharedPtr<FJsonObject>* SourceLocationPtr = nullptr;
	if (!ContractSummary->TryGetObjectField(TEXT("source_location"), SourceLocationPtr)
		|| !SourceLocationPtr
		|| !(*SourceLocationPtr).IsValid())
	{
		return FMCPToolResult::Error(TEXT("Resolved contract does not expose a source_location object."));
	}

	FString ModuleOrigin;
	FString HeaderPath;
	(*SourceLocationPtr)->TryGetStringField(TEXT("module_origin"), ModuleOrigin);
	(*SourceLocationPtr)->TryGetStringField(TEXT("resolved_header_path"), HeaderPath);

	if (ModuleOrigin != TEXT("plugin"))
	{
		return FMCPToolResult::Error(TEXT("This mutation lane is bounded to plugin-owned headers only."));
	}

	if (HeaderPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Resolved contract does not expose a concrete plugin header path."));
	}

	FString PropertyResolveError;
	const FProperty* Property = ResolveDeclaredPropertyIdentifier(ContractObject, MemberName, PropertyResolveError);
	if (!Property)
	{
		return FMCPToolResult::Error(PropertyResolveError);
	}

	const FCppReflectionPropertyMetadataMutationResult MutationPreview =
		PreviewPluginPropertyMetadataMutation(HeaderPath, Property->GetName(), MetadataKey, MetadataValue);
	if (!MutationPreview.bSuccess)
	{
		return FMCPToolResult::Error(MutationPreview.ErrorMessage);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("preview_property_metadata_mutation"));
	Data->SetStringField(TEXT("mutation_lane"), TEXT("plugin_owned_uproperty_metadata_upsert"));
	Data->SetStringField(TEXT("discovery_basis"), TEXT("runtime_reflection_uht_metadata_plus_bounded_header_macro_preview"));
	Data->SetStringField(TEXT("note"), TEXT("This is a preview only. Reflected header metadata changes require a fresh preflight rebuild before runtime reflection can prove the new metadata."));
	Data->SetObjectField(TEXT("contract"), ContractSummary);
	Data->SetObjectField(TEXT("member_before"), BuildPropertyObject(Property, true));
	Data->SetObjectField(TEXT("expected_metadata_after_rebuild"), BuildExpectedPropertyMetadataObject(Property, MutationPreview.NormalizedMetadataKey, MetadataValue));
	Data->SetObjectField(TEXT("preview"), MutationPreview.ToJson());

	TSharedPtr<FJsonObject> PreviewHandshake = MakeShared<FJsonObject>();
	PreviewHandshake->SetStringField(TEXT("status"), TEXT("preview_only"));
	PreviewHandshake->SetBoolField(TEXT("supports_in_process_reflected_compile"), false);
	PreviewHandshake->SetStringField(TEXT("recommended_action_after_apply"), TEXT("close_editor_and_run_preflight_launcher"));
	PreviewHandshake->SetStringField(TEXT("preflight_launcher_path"), FUnrealClaudeModule::GetPreflightLauncherPath());
	PreviewHandshake->SetObjectField(TEXT("build_status_current"), BuildBuildSyncObject(FUnrealClaudeModule::GetBuildSyncStatus()));
	Data->SetObjectField(TEXT("compile_handshake"), PreviewHandshake);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Previewed plugin-owned reflected metadata mutation for %s.%s"), *GetCppSymbolName(ContractObject), *Property->GetName()),
		Data);
}

FMCPToolResult FMCPTool_CppReflection::ExecuteApplyPropertyMetadataMutation(const TSharedRef<FJsonObject>& Params)
{
	const FUnrealClaudeBuildSyncStatus BuildSyncBefore = FUnrealClaudeModule::GetBuildSyncStatus();

	FString SymbolIdentifier;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("symbol"), SymbolIdentifier, Error))
	{
		return Error.GetValue();
	}

	FString MemberName;
	if (!ExtractRequiredString(Params, TEXT("member_name"), MemberName, Error))
	{
		return Error.GetValue();
	}

	FString MetadataKey;
	if (!ExtractRequiredString(Params, TEXT("metadata_key"), MetadataKey, Error))
	{
		return Error.GetValue();
	}

	FString MetadataValue;
	if (!ExtractRequiredString(Params, TEXT("metadata_value"), MetadataValue, Error))
	{
		return Error.GetValue();
	}

	FString ParseError;
	const ECppReflectionKindFilter KindFilter =
		ParseKindFilter(ExtractOptionalString(Params, TEXT("symbol_kind"), TEXT("all")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const ECppReflectionModuleScope ModuleScope =
		ParseModuleScope(ExtractOptionalString(Params, TEXT("module_scope"), TEXT("plugin_only")), ParseError);
	if (!ParseError.IsEmpty())
	{
		return FMCPToolResult::Error(ParseError);
	}

	const FString ModuleFilter = ExtractOptionalString(Params, TEXT("module_filter"));
	const FCppModuleLocator ModuleLocator;
	FString ResolveError;
	const UObject* ContractObject = ResolveObjectIdentifier(
		SymbolIdentifier,
		KindFilter,
		ModuleScope,
		ModuleFilter,
		ModuleLocator,
		ResolveError);
	if (!ContractObject)
	{
		return FMCPToolResult::Error(ResolveError);
	}

	TSharedPtr<FJsonObject> ContractSummary = BuildTypeSummaryObject(ContractObject, ModuleLocator, true);
	const TSharedPtr<FJsonObject>* SourceLocationPtr = nullptr;
	if (!ContractSummary->TryGetObjectField(TEXT("source_location"), SourceLocationPtr)
		|| !SourceLocationPtr
		|| !(*SourceLocationPtr).IsValid())
	{
		return FMCPToolResult::Error(TEXT("Resolved contract does not expose a source_location object."));
	}

	FString ModuleOrigin;
	FString HeaderPath;
	(*SourceLocationPtr)->TryGetStringField(TEXT("module_origin"), ModuleOrigin);
	(*SourceLocationPtr)->TryGetStringField(TEXT("resolved_header_path"), HeaderPath);

	if (ModuleOrigin != TEXT("plugin"))
	{
		return FMCPToolResult::Error(TEXT("This mutation lane is bounded to plugin-owned headers only."));
	}

	if (HeaderPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Resolved contract does not expose a concrete plugin header path."));
	}

	FString PropertyResolveError;
	const FProperty* Property = ResolveDeclaredPropertyIdentifier(ContractObject, MemberName, PropertyResolveError);
	if (!Property)
	{
		return FMCPToolResult::Error(PropertyResolveError);
	}

	const FCppReflectionPropertyMetadataMutationResult MutationApply =
		ApplyPluginPropertyMetadataMutation(HeaderPath, Property->GetName(), MetadataKey, MetadataValue);
	if (!MutationApply.bSuccess)
	{
		return FMCPToolResult::Error(MutationApply.ErrorMessage);
	}

	const FUnrealClaudeBuildSyncStatus BuildSyncAfter = FUnrealClaudeModule::GetBuildSyncStatus();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("apply_property_metadata_mutation"));
	Data->SetStringField(TEXT("mutation_lane"), TEXT("plugin_owned_uproperty_metadata_upsert"));
	Data->SetStringField(TEXT("discovery_basis"), TEXT("runtime_reflection_uht_metadata_plus_bounded_header_macro_apply"));
	Data->SetStringField(TEXT("note"), TEXT("Apply writes the plugin-owned header immediately, but runtime reflection will not prove the new metadata until a fresh preflight rebuild completes."));
	Data->SetObjectField(TEXT("contract"), ContractSummary);
	Data->SetObjectField(TEXT("member_before"), BuildPropertyObject(Property, true));
	Data->SetObjectField(TEXT("expected_metadata_after_rebuild"), BuildExpectedPropertyMetadataObject(Property, MutationApply.NormalizedMetadataKey, MetadataValue));
	Data->SetObjectField(TEXT("apply"), MutationApply.ToJson());
	Data->SetObjectField(TEXT("compile_handshake"), BuildCompileHandshakeObject(MutationApply.bApplied, BuildSyncBefore, BuildSyncAfter));

	// 619 P4 Fix #4 (meta-only path): classify the actual MacroBefore/After
	// diff as a safety net. Under normal use, ApplyPluginPropertyMetadataMutation
	// only touches the `meta=()` group of an existing UPROPERTY, so the
	// classifier should return MetaOnly. If a future maintainer extends the
	// helper to touch macro structure, the classifier will catch it and the
	// code here will bias to requires_restart=true instead of a dangerous LC
	// hot-patch against a changed reflection layout.
	const FCppReflectionDiffClassificationResult DiffClassification =
		FCppReflectionMutationClassifier::ClassifyDiff(MutationApply.MacroBefore, MutationApply.MacroAfter);

	TSharedPtr<FJsonObject> ClassifierObject = MakeShared<FJsonObject>();
	ClassifierObject->SetStringField(TEXT("classification"),
		DiffClassification.Classification == ECppReflectionDiffClassification::MetaOnly ? TEXT("meta_only") :
		DiffClassification.Classification == ECppReflectionDiffClassification::Structural ? TEXT("structural") :
		DiffClassification.Classification == ECppReflectionDiffClassification::AmbiguousBiasToRestart ? TEXT("ambiguous_bias_to_restart") :
		TEXT("unchanged"));
	ClassifierObject->SetStringField(TEXT("reason_code"), DiffClassification.ReasonCode);
	ClassifierObject->SetStringField(TEXT("reason_detail"), DiffClassification.ReasonDetail);
	ClassifierObject->SetBoolField(TEXT("biased_to_restart"), DiffClassification.bBiasedToRestart);
	Data->SetObjectField(TEXT("classifier"), ClassifierObject);

	const bool bSafeForLiveCoding =
		DiffClassification.Classification == ECppReflectionDiffClassification::MetaOnly
		|| DiffClassification.Classification == ECppReflectionDiffClassification::Unchanged;

	if (bSafeForLiveCoding && MutationApply.bApplied)
	{
		// 619 P4 Fix #4 meta-only path -> TriggerLiveCodingCompile (D2 single
		// source of truth; same public helper P1's livecoding_compile tool
		// calls).
		const UnrealClaude::CppReflectionCompile::Testing::FMockTriggerLiveCodingCompileResult LcResult =
			UnrealClaude::CppReflectionCompile::InvokeTriggerLiveCodingCompile();
		TSharedPtr<FJsonObject> LcObject = MakeShared<FJsonObject>();
		LcObject->SetBoolField(TEXT("success"), LcResult.bSuccess);
		LcObject->SetStringField(TEXT("refresh_status"),
			LcResult.bSuccess ? TEXT("live_coding_patched") : TEXT("live_coding_failed"));
		if (!LcResult.ErrorLog.IsEmpty())
		{
			LcObject->SetStringField(TEXT("error_log"), LcResult.ErrorLog);
		}
		if (LcResult.Diagnostics.IsValid())
		{
			LcObject->SetObjectField(TEXT("diagnostics"), LcResult.Diagnostics);
		}
		Data->SetObjectField(TEXT("live_coding_compile"), LcObject);
		Data->SetStringField(TEXT("refresh_status"),
			LcResult.bSuccess ? TEXT("live_coding_patched") : TEXT("live_coding_failed"));
		if (!LcResult.bSuccess)
		{
			Data->SetBoolField(TEXT("requires_restart"), true);
			Data->SetStringField(TEXT("reason"), TEXT("live_coding_failed_for_metadata_upsert"));
			Data->SetStringField(TEXT("recommended_next_step"), TEXT("restart_survival"));
		}
	}
	else if (MutationApply.bApplied)
	{
		// Classifier says Structural or AmbiguousBiasToRestart. Do NOT call
		// LC; surface requires_restart=true so the agent escalates to
		// restart_survival instead of silently leaving a stale DLL loaded.
		Data->SetBoolField(TEXT("requires_restart"), true);
		Data->SetStringField(TEXT("reason"),
			DiffClassification.Classification == ECppReflectionDiffClassification::Structural
				? TEXT("classifier_detected_structural_diff_in_metadata_path")
				: TEXT("classifier_ambiguous_bias_to_restart"));
		Data->SetStringField(TEXT("recommended_next_step"), TEXT("restart_survival"));
		if (DiffClassification.bBiasedToRestart)
		{
			UE_LOG(LogUnrealClaude, Warning,
				TEXT("MCPTool_CppReflection: metadata path classifier biased to restart. %s"),
				*DiffClassification.ReasonDetail);
		}
	}

	const FString ReceiptPath = WritePropertyMutationReceipt(
		Data,
		GetCppSymbolName(ContractObject),
		Property->GetName(),
		MutationApply.NormalizedMetadataKey);
	Data->SetStringField(TEXT("receipt_path"), ReceiptPath);

	const FString StatusVerb = MutationApply.bApplied ? TEXT("Applied") : TEXT("No-op");
	return FMCPToolResult::Success(
		FString::Printf(TEXT("%s plugin-owned reflected metadata mutation for %s.%s"), *StatusVerb, *GetCppSymbolName(ContractObject), *Property->GetName()),
		Data);
}
