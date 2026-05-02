// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeScopePolicy.h"
#include "Misc/Paths.h"

const TArray<FString>& FUnrealClaudeScopePolicy::GetPluginOnlyRoots()
{
	static const TArray<FString> Roots = {
		TEXT("Plugins/UnrealClaude"),
		TEXT("Docs/UnrealClaude"),
		TEXT("AgentBridge"),
	};
	return Roots;
}

TArray<FString> FUnrealClaudeScopePolicy::GetAllowedWriteRoots()
{
	const UUnrealClaudeSettings* Settings = UUnrealClaudeSettings::Get();
	if (!Settings || Settings->ScopeMode == EUnrealClaudeScopeMode::PluginOnly)
	{
		return GetPluginOnlyRoots();
	}
	// PluginAndProject — all paths allowed
	return TArray<FString>{ TEXT("") }; // empty root = everything
}

// Internal runtime-state roots (always allowed, classified differently)
static const TArray<FString>& GetInternalStateRoots()
{
	static const TArray<FString> Roots = {
		TEXT("Saved/UnrealClaude"),
	};
	return Roots;
}

FUnrealClaudeScopePolicy::FScopeCheckResult FUnrealClaudeScopePolicy::IsWriteAllowed(const FString& ProjectRelativePath)
{
	FScopeCheckResult Result;
	const UUnrealClaudeSettings* Settings = UUnrealClaudeSettings::Get();

	FString NormalizedPath = ProjectRelativePath;
	NormalizedPath.ReplaceInline(TEXT("\\"), TEXT("/"));

	// PluginAndProject mode — everything allowed as user mutation
	if (Settings && Settings->ScopeMode == EUnrealClaudeScopeMode::PluginAndProject)
	{
		Result.bAllowed = true;
		Result.Classification = EScopeClassification::UserMutation;
		Result.MatchedRoot = TEXT("PluginAndProject (all paths)");
		return Result;
	}

	// PluginOnly mode — check user-facing roots first
	for (const FString& Root : GetPluginOnlyRoots())
	{
		if (NormalizedPath.StartsWith(Root))
		{
			Result.bAllowed = true;
			Result.Classification = EScopeClassification::UserMutation;
			Result.MatchedRoot = Root;
			return Result;
		}
	}

	// Check internal runtime-state roots
	for (const FString& Root : GetInternalStateRoots())
	{
		if (NormalizedPath.StartsWith(Root))
		{
			Result.bAllowed = true;
			Result.Classification = EScopeClassification::InternalState;
			Result.MatchedRoot = Root;
			return Result;
		}
	}

	Result.bAllowed = false;
	Result.Classification = EScopeClassification::Denied;
	Result.DenialReason = FString::Printf(
		TEXT("ScopeMode is PluginOnly. Path '%s' is outside allowed roots: Plugins/UnrealClaude, Docs/UnrealClaude, AgentBridge (user) + Saved/UnrealClaude (internal)"),
		*ProjectRelativePath);
	return Result;
}

FUnrealClaudeScopePolicy::FScopeCheckResult FUnrealClaudeScopePolicy::IsAbsoluteWriteAllowed(const FString& AbsolutePath)
{
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);

	FString RelativePath = AbsolutePath;
	FPaths::NormalizeFilename(RelativePath);

	if (RelativePath.StartsWith(ProjectDir))
	{
		RelativePath = RelativePath.RightChop(ProjectDir.Len()).TrimStartAndEnd();
		RelativePath.RemoveFromStart(TEXT("/"));
	}

	return IsWriteAllowed(RelativePath);
}
