// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUEScopePolicy.h"
#include "Misc/Paths.h"

const TArray<FString>& FOsvayderUEScopePolicy::GetPluginOnlyRoots()
{
	static const TArray<FString> Roots = {
		TEXT("Plugins/OsvayderUE"),
		TEXT("Docs/OsvayderUE"),
		TEXT("AgentBridge"),
	};
	return Roots;
}

TArray<FString> FOsvayderUEScopePolicy::GetAllowedWriteRoots()
{
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();
	if (!Settings || Settings->ScopeMode == EOsvayderUEScopeMode::PluginOnly)
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
		TEXT("Saved/OsvayderUE"),
		TEXT("Saved/OsvayderUE"),
	};
	return Roots;
}

FOsvayderUEScopePolicy::FScopeCheckResult FOsvayderUEScopePolicy::IsWriteAllowed(const FString& ProjectRelativePath)
{
	FScopeCheckResult Result;
	const UOsvayderUESettings* Settings = UOsvayderUESettings::Get();

	FString NormalizedPath = ProjectRelativePath;
	NormalizedPath.ReplaceInline(TEXT("\\"), TEXT("/"));

	// PluginAndProject mode — everything allowed as user mutation
	if (Settings && Settings->ScopeMode == EOsvayderUEScopeMode::PluginAndProject)
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
		TEXT("ScopeMode is PluginOnly. Path '%s' is outside allowed roots: Plugins/OsvayderUE, Docs/OsvayderUE, AgentBridge (user) + Saved/OsvayderUE and Saved/OsvayderUE (internal)"),
		*ProjectRelativePath);
	return Result;
}

FOsvayderUEScopePolicy::FScopeCheckResult FOsvayderUEScopePolicy::IsAbsoluteWriteAllowed(const FString& AbsolutePath)
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
