// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_LocalAnimationPackIntake.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace
{
	constexpr int32 DefaultMaxFiles = 5000;
	constexpr int32 HardMaxFiles = 20000;
	constexpr int64 DefaultMaxTotalSizeMb = 8192;
	constexpr int64 HardMaxTotalSizeMb = 65536;
	constexpr int32 MaxReportedRows = 50;

	struct FIntakeFilePlan
	{
		FString SourceFile;
		FString DestinationFile;
		FString RelativePath;
		FString GamePackagePath;
		int64 SourceSize = 0;
		FString SourceHash;
		FString DestinationHash;
		FString Status;
	};

	struct FIntakePlan
	{
		FString OriginalSourceRoot;
		FString SelectedSourceRoot;
		FString DestinationGameRoot;
		FString DestinationContentRoot;
		FString DestinationPhysicalRoot;
		FString PackIdentifier;
		TArray<FIntakeFilePlan> Missing;
		TArray<FIntakeFilePlan> AlreadyPresent;
		TArray<FIntakeFilePlan> Conflicts;
		TArray<FString> CandidateAnimationPaths;
		int32 EnumeratedFileCount = 0;
		int32 CandidateAssetCount = 0;
		int64 CandidateTotalSize = 0;
	};

	FString NormalizeFilePath(FString Path)
	{
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		FPaths::CollapseRelativeDirectories(Path);
		FPaths::MakeStandardFilename(Path);
		return Path;
	}

	FString NormalizeDirectoryPath(FString Path)
	{
		Path = NormalizeFilePath(Path);
		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	FString NormalizeRelativeAssetPath(FString Path)
	{
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Path.StartsWith(TEXT("/")))
		{
			Path.RightChopInline(1);
		}
		return Path;
	}

	bool IsSafeRelativeAssetPath(const FString& Path)
	{
		if (Path.IsEmpty() || !FPaths::IsRelative(Path) || Path.Contains(TEXT(":")))
		{
			return false;
		}

		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("/"), false);
		for (const FString& Segment : Segments)
		{
			if (Segment == TEXT("..") || Segment == TEXT("."))
			{
				return false;
			}
		}
		return true;
	}

	bool IsSameOrChildPath(FString Candidate, FString Root)
	{
		Candidate = NormalizeDirectoryPath(Candidate);
		Root = NormalizeDirectoryPath(Root);
		if (Candidate.Equals(Root, ESearchCase::IgnoreCase))
		{
			return true;
		}
		if (!Root.EndsWith(TEXT("/")))
		{
			Root += TEXT("/");
		}
		return Candidate.StartsWith(Root, ESearchCase::IgnoreCase);
	}

	bool IsFileUnderDirectory(FString FilePath, FString DirectoryPath)
	{
		FilePath = NormalizeFilePath(FilePath);
		DirectoryPath = NormalizeDirectoryPath(DirectoryPath);
		if (!DirectoryPath.EndsWith(TEXT("/")))
		{
			DirectoryPath += TEXT("/");
		}
		return FilePath.StartsWith(DirectoryPath, ESearchCase::IgnoreCase);
	}

	FString AbsoluteDirectoryForRead(const FString& Path)
	{
		FString FullPath = Path;
		FullPath.TrimStartAndEndInline();
		if (FPaths::IsRelative(FullPath))
		{
			FullPath = FPaths::ConvertRelativePathToFull(FullPath);
		}
		return NormalizeDirectoryPath(IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*FullPath));
	}

	FString AbsoluteDirectoryForWrite(const FString& Path)
	{
		FString FullPath = Path;
		FullPath.TrimStartAndEndInline();
		if (FPaths::IsRelative(FullPath))
		{
			FullPath = FPaths::ConvertRelativePathToFull(FullPath);
		}
		return NormalizeDirectoryPath(IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*FullPath));
	}

	bool IsDriveRoot(const FString& Path)
	{
		FString Normalized = NormalizeDirectoryPath(Path);
		return (Normalized.Len() == 2 && Normalized[1] == TEXT(':'))
			|| (Normalized.Len() == 3 && Normalized[1] == TEXT(':') && Normalized[2] == TEXT('/'));
	}

	bool EqualsKnownRoot(const FString& Path, const FString& Root)
	{
		return !Root.IsEmpty() && NormalizeDirectoryPath(Path).Equals(NormalizeDirectoryPath(Root), ESearchCase::IgnoreCase);
	}

	bool IsRejectedBroadRoot(const FString& SourceRoot, FString& OutReason)
	{
		const FString Normalized = NormalizeDirectoryPath(SourceRoot);
		if (IsDriveRoot(Normalized))
		{
			OutReason = TEXT("drive_root");
			return true;
		}

#if PLATFORM_WINDOWS
		if (Normalized.Equals(TEXT("C:/Windows"), ESearchCase::IgnoreCase)
			|| Normalized.StartsWith(TEXT("C:/Windows/"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("C:/Program Files"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("C:/Program Files (x86)"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("D:/Program Files"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("D:/Program Files (x86)"), ESearchCase::IgnoreCase))
		{
			OutReason = TEXT("system_root");
			return true;
		}
#endif

		const FString UserProfile = AbsoluteDirectoryForRead(FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE")));
		if (EqualsKnownRoot(Normalized, UserProfile))
		{
			OutReason = TEXT("user_profile_root");
			return true;
		}

		const FString ProjectDir = AbsoluteDirectoryForRead(FPaths::ProjectDir());
		const FString ContentDir = AbsoluteDirectoryForRead(FPaths::ProjectContentDir());
		if (EqualsKnownRoot(Normalized, ProjectDir) || EqualsKnownRoot(Normalized, ContentDir))
		{
			OutReason = TEXT("project_root");
			return true;
		}

		return false;
	}

	bool IsCandidateAssetFile(const FString& FilePath)
	{
		return FilePath.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			|| FilePath.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase);
	}

	bool IsCanonicalPackChildName(const FString& Name)
	{
		return Name.Equals(TEXT("Animations"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Animation"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Anims"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Characters"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Character"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Levels"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Level"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Maps"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Meshes"), ESearchCase::IgnoreCase);
	}

	int32 CountCandidateFilesBounded(const FString& Root, const int32 Limit)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TArray<FString> Directories;
		Directories.Add(Root);
		int32 Count = 0;

		while (Directories.Num() > 0 && Count < Limit)
		{
			const FString CurrentDir = Directories.Pop(EAllowShrinking::No);
			class FVisitor final : public IPlatformFile::FDirectoryVisitor
			{
			public:
				TArray<FString>& OutDirectories;
				int32& CountRef;
				const int32 LimitRef;

				FVisitor(TArray<FString>& InDirectories, int32& InCount, const int32 InLimit)
					: OutDirectories(InDirectories)
					, CountRef(InCount)
					, LimitRef(InLimit)
				{
				}

				virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
				{
					if (bIsDirectory)
					{
						OutDirectories.Add(NormalizeDirectoryPath(FilenameOrDirectory));
						return true;
					}

					if (IsCandidateAssetFile(FilenameOrDirectory))
					{
						++CountRef;
					}
					return CountRef < LimitRef;
				}
			};

			FVisitor Visitor(Directories, Count, Limit);
			PlatformFile.IterateDirectory(*CurrentDir, Visitor);
		}

		return Count;
	}

	int32 ScorePackRoot(const FString& Root)
	{
		int32 Score = 0;
		IFileManager& FileManager = IFileManager::Get();
		for (const TCHAR* Name : { TEXT("Animations"), TEXT("Animation"), TEXT("Anims"), TEXT("Characters"), TEXT("Levels"), TEXT("Level"), TEXT("Maps") })
		{
			if (FileManager.DirectoryExists(*FPaths::Combine(Root, Name)))
			{
				Score += 100;
			}
		}
		Score += FMath::Min(CountCandidateFilesBounded(Root, 50), 50);
		return Score;
	}

	FString SelectPackRoot(const FString& SourceRoot)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TArray<FString> ChildDirectories;

		class FChildVisitor final : public IPlatformFile::FDirectoryVisitor
		{
		public:
			TArray<FString>& Children;
			explicit FChildVisitor(TArray<FString>& InChildren)
				: Children(InChildren)
			{
			}

			virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
			{
				if (bIsDirectory)
				{
					Children.Add(NormalizeDirectoryPath(FilenameOrDirectory));
				}
				return true;
			}
		};

		FChildVisitor Visitor(ChildDirectories);
		PlatformFile.IterateDirectory(*SourceRoot, Visitor);

		const int32 RootScore = ScorePackRoot(SourceRoot);
		int32 BestChildScore = 0;
		FString BestChild;
		for (const FString& Child : ChildDirectories)
		{
			const FString ChildName = FPaths::GetCleanFilename(Child);
			const int32 ChildScore = ScorePackRoot(Child) + (IsCanonicalPackChildName(ChildName) ? 25 : 0);
			if (ChildScore > BestChildScore)
			{
				BestChildScore = ChildScore;
				BestChild = Child;
			}
		}

		if (!BestChild.IsEmpty() && BestChildScore >= 100 && BestChildScore > RootScore)
		{
			return BestChild;
		}

		return SourceRoot;
	}

	FString SanitizePackageSegment(const FString& InSegment)
	{
		FString Result;
		for (const TCHAR Character : InSegment)
		{
			if (FChar::IsAlnum(Character) || Character == TEXT('_'))
			{
				Result.AppendChar(Character);
			}
		}
		if (Result.IsEmpty())
		{
			Result = TEXT("LocalAnimationPack");
		}
		if (Result.Len() > 0 && FChar::IsDigit(Result[0]))
		{
			Result = TEXT("Pack_") + Result;
		}
		return Result;
	}

	bool NormalizeDestinationGameRoot(FString InRoot, const FString& SelectedSourceRoot, FString& OutRoot, FString& OutError)
	{
		InRoot.TrimStartAndEndInline();
		InRoot.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (InRoot.IsEmpty())
		{
			InRoot = TEXT("/Game/") + SanitizePackageSegment(FPaths::GetCleanFilename(SelectedSourceRoot));
		}
		while (InRoot.Contains(TEXT("//")))
		{
			InRoot.ReplaceInline(TEXT("//"), TEXT("/"));
		}
		if (InRoot.EndsWith(TEXT("/")))
		{
			InRoot.LeftChopInline(1, EAllowShrinking::No);
		}
		if (!InRoot.StartsWith(TEXT("/Game/")) || InRoot.Equals(TEXT("/Game"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("destination_game_root must be a /Game/<PackName> path.");
			return false;
		}
		if (InRoot.Contains(TEXT("..")) || InRoot.Contains(TEXT(":")))
		{
			OutError = TEXT("destination_game_root cannot contain path traversal or drive syntax.");
			return false;
		}
		for (const TCHAR Character : InRoot)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('/')))
			{
				OutError = TEXT("destination_game_root may contain only letters, digits, underscores, and slashes.");
				return false;
			}
		}
		OutRoot = InRoot;
		return true;
	}

	FString HashFileToString(const FString& Path)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			return FString();
		}

		FSHAHash Hash;
		FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
		return Hash.ToString();
	}

	TSharedPtr<FJsonObject> FilePlanToJson(const FIntakeFilePlan& Plan)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("relative_path"), Plan.RelativePath);
		Object->SetStringField(TEXT("source_file"), Plan.SourceFile);
		Object->SetStringField(TEXT("destination_file"), Plan.DestinationFile);
		Object->SetStringField(TEXT("game_package_path"), Plan.GamePackagePath);
		Object->SetNumberField(TEXT("source_size"), static_cast<double>(Plan.SourceSize));
		Object->SetStringField(TEXT("status"), Plan.Status);
		if (!Plan.SourceHash.IsEmpty())
		{
			Object->SetStringField(TEXT("source_sha1"), Plan.SourceHash);
		}
		if (!Plan.DestinationHash.IsEmpty())
		{
			Object->SetStringField(TEXT("destination_sha1"), Plan.DestinationHash);
		}
		return Object;
	}

	void AddPlanArray(TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const TArray<FIntakeFilePlan>& Rows)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const int32 Count = FMath::Min(Rows.Num(), MaxReportedRows);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Values.Add(MakeShared<FJsonValueObject>(FilePlanToJson(Rows[Index])));
		}
		Object->SetArrayField(FieldName, Values);
	}

	void AddStringArray(TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const TArray<FString>& Rows)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const int32 Count = FMath::Min(Rows.Num(), MaxReportedRows);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Values.Add(MakeShared<FJsonValueString>(Rows[Index]));
		}
		Object->SetArrayField(FieldName, Values);
	}

	TSharedPtr<FJsonObject> BuildResultData(const FIntakePlan& Plan, const FString& Status)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("result_type"), TEXT("local_animation_pack_intake"));
		Data->SetStringField(TEXT("schema_version"), TEXT("local_animation_pack_intake.v1"));
		Data->SetStringField(TEXT("status"), Status);
		Data->SetStringField(TEXT("source_root"), Plan.OriginalSourceRoot);
		Data->SetStringField(TEXT("selected_source_root"), Plan.SelectedSourceRoot);
		Data->SetStringField(TEXT("destination_game_root"), Plan.DestinationGameRoot);
		Data->SetStringField(TEXT("destination_content_root"), Plan.DestinationContentRoot);
		Data->SetStringField(TEXT("destination_physical_root"), Plan.DestinationPhysicalRoot);
		Data->SetStringField(TEXT("pack_identifier"), Plan.PackIdentifier);
		Data->SetNumberField(TEXT("enumerated_file_count"), Plan.EnumeratedFileCount);
		Data->SetNumberField(TEXT("candidate_asset_count"), Plan.CandidateAssetCount);
		Data->SetNumberField(TEXT("candidate_total_size"), static_cast<double>(Plan.CandidateTotalSize));
		Data->SetNumberField(TEXT("missing_count"), Plan.Missing.Num());
		Data->SetNumberField(TEXT("already_present_count"), Plan.AlreadyPresent.Num());
		Data->SetNumberField(TEXT("conflict_count"), Plan.Conflicts.Num());
		AddPlanArray(Data, TEXT("missing_files"), Plan.Missing);
		AddPlanArray(Data, TEXT("already_present_files"), Plan.AlreadyPresent);
		AddPlanArray(Data, TEXT("conflict_files"), Plan.Conflicts);
		AddStringArray(Data, TEXT("candidate_animation_paths"), Plan.CandidateAnimationPaths);

		TSharedPtr<FJsonObject> NextCall = MakeShared<FJsonObject>();
		NextCall->SetStringField(TEXT("tool"), TEXT("unreal_ue"));
		NextCall->SetStringField(TEXT("domain"), TEXT("blueprint"));
		NextCall->SetStringField(TEXT("operation"), TEXT("animation_preflight"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), TEXT("animation_preflight"));
		Params->SetStringField(TEXT("asset_root"), Plan.DestinationGameRoot);
		Params->SetStringField(TEXT("pack_identifier"), Plan.PackIdentifier);
		NextCall->SetObjectField(TEXT("params"), Params);
		Data->SetObjectField(TEXT("next_tool_call"), NextCall);
		Data->SetStringField(TEXT("exact_next_action"), FString::Printf(
			TEXT("Call animation_preflight with asset_root='%s' and pack_identifier='%s' before claiming the imported pack is usable."),
			*Plan.DestinationGameRoot,
			*Plan.PackIdentifier));
		return Data;
	}

	FMCPToolResult MakeBlocker(const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Data = nullptr)
	{
		if (!Data.IsValid())
		{
			Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("result_type"), TEXT("local_animation_pack_intake"));
			Data->SetStringField(TEXT("schema_version"), TEXT("local_animation_pack_intake.v1"));
		}
		Data->SetStringField(TEXT("status"), TEXT("blocker"));
		Data->SetStringField(TEXT("blocker_code"), Code);
		Data->SetStringField(TEXT("blocker_message"), Message);

		FMCPToolResult Result;
		Result.bSuccess = false;
		Result.Message = Message;
		Result.Data = Data;
		return Result;
	}

	bool BuildPlan(
		const FString& SourceRoot,
		const FString& DestinationGameRootParam,
		const int32 MaxFiles,
		const int64 MaxTotalSizeBytes,
		const FString& DestinationContentRoot,
		FIntakePlan& OutPlan,
		FString& OutBlockerCode,
		FString& OutBlockerMessage)
	{
		OutPlan = FIntakePlan();
		OutPlan.OriginalSourceRoot = SourceRoot;

		FString BroadRootReason;
		if (IsRejectedBroadRoot(SourceRoot, BroadRootReason))
		{
			OutBlockerCode = TEXT("rejected_broad_root");
			OutBlockerMessage = FString::Printf(TEXT("source_root is too broad for local animation intake: %s"), *BroadRootReason);
			return false;
		}

		if (!IFileManager::Get().DirectoryExists(*SourceRoot))
		{
			OutBlockerCode = TEXT("source_root_not_found");
			OutBlockerMessage = FString::Printf(TEXT("source_root does not exist or is not a directory: %s"), *SourceRoot);
			return false;
		}

		OutPlan.SelectedSourceRoot = SelectPackRoot(SourceRoot);
		FString DestinationGameRoot;
		FString DestinationError;
		if (!NormalizeDestinationGameRoot(DestinationGameRootParam, OutPlan.SelectedSourceRoot, DestinationGameRoot, DestinationError))
		{
			OutBlockerCode = TEXT("invalid_destination_game_root");
			OutBlockerMessage = DestinationError;
			return false;
		}

		OutPlan.DestinationGameRoot = DestinationGameRoot;
		OutPlan.DestinationContentRoot = DestinationContentRoot;
		OutPlan.PackIdentifier = FPaths::GetCleanFilename(OutPlan.SelectedSourceRoot);

		FString RelativeDestinationRoot = DestinationGameRoot.RightChop(6);
		OutPlan.DestinationPhysicalRoot = NormalizeDirectoryPath(FPaths::Combine(DestinationContentRoot, RelativeDestinationRoot));
		if (!IsSameOrChildPath(OutPlan.DestinationPhysicalRoot, DestinationContentRoot))
		{
			OutBlockerCode = TEXT("destination_outside_content");
			OutBlockerMessage = TEXT("Resolved destination would escape the project Content directory.");
			return false;
		}

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TArray<FString> Directories;
		Directories.Add(OutPlan.SelectedSourceRoot);

		while (Directories.Num() > 0)
		{
			const FString CurrentDir = Directories.Pop(EAllowShrinking::No);
			class FVisitor final : public IPlatformFile::FDirectoryVisitor
			{
			public:
				TArray<FString>& DirectoriesRef;
				FIntakePlan& PlanRef;
				const int32 MaxFilesRef;
				const int64 MaxTotalSizeBytesRef;
				bool bGuardTripped = false;
				FString GuardCode;
				FString GuardMessage;

				FVisitor(TArray<FString>& InDirectories, FIntakePlan& InPlan, const int32 InMaxFiles, const int64 InMaxTotalSizeBytes)
					: DirectoriesRef(InDirectories)
					, PlanRef(InPlan)
					, MaxFilesRef(InMaxFiles)
					, MaxTotalSizeBytesRef(InMaxTotalSizeBytes)
				{
				}

				virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
				{
					const FString Current = bIsDirectory
						? NormalizeDirectoryPath(FilenameOrDirectory)
						: NormalizeFilePath(FilenameOrDirectory);
					if (bIsDirectory)
					{
						DirectoriesRef.Add(Current);
						return true;
					}

					++PlanRef.EnumeratedFileCount;
					if (PlanRef.EnumeratedFileCount > MaxFilesRef)
					{
						bGuardTripped = true;
						GuardCode = TEXT("file_count_guardrail");
						GuardMessage = FString::Printf(TEXT("source_root exceeded max_files guardrail (%d)."), MaxFilesRef);
						return false;
					}

					if (!IsCandidateAssetFile(Current))
					{
						return true;
					}

					const int64 Size = IFileManager::Get().FileSize(*Current);
					PlanRef.CandidateTotalSize += FMath::Max<int64>(0, Size);
					if (PlanRef.CandidateTotalSize > MaxTotalSizeBytesRef)
					{
						bGuardTripped = true;
						GuardCode = TEXT("total_size_guardrail");
						GuardMessage = TEXT("source_root exceeded max_total_size_mb guardrail.");
						return false;
					}

					++PlanRef.CandidateAssetCount;
					FString SelectedSourceRootForRelative = NormalizeDirectoryPath(PlanRef.SelectedSourceRoot);
					if (!SelectedSourceRootForRelative.EndsWith(TEXT("/")))
					{
						SelectedSourceRootForRelative += TEXT("/");
					}
					if (!Current.StartsWith(SelectedSourceRootForRelative, ESearchCase::IgnoreCase))
					{
						bGuardTripped = true;
						GuardCode = TEXT("relative_path_resolution_failed");
						GuardMessage = FString::Printf(TEXT("File is outside selected source root: %s"), *Current);
						return false;
					}
					FString RelativePath = NormalizeRelativeAssetPath(Current.RightChop(SelectedSourceRootForRelative.Len()));
					if (!IsSafeRelativeAssetPath(RelativePath))
					{
						bGuardTripped = true;
						GuardCode = TEXT("relative_path_escape");
						GuardMessage = FString::Printf(TEXT("Resolved unsafe relative asset path: %s"), *RelativePath);
						return false;
					}
					FIntakeFilePlan Row;
					Row.SourceFile = Current;
					Row.RelativePath = RelativePath;
					Row.SourceSize = Size;
					Row.DestinationFile = NormalizeFilePath(FPaths::Combine(PlanRef.DestinationPhysicalRoot, RelativePath));
					const FString RelativeWithoutExtension = FPaths::ChangeExtension(RelativePath, FString()).Replace(TEXT("\\"), TEXT("/"));
					Row.GamePackagePath = PlanRef.DestinationGameRoot + TEXT("/") + RelativeWithoutExtension;
					Row.SourceHash = HashFileToString(Row.SourceFile);

					if (!IsFileUnderDirectory(Row.DestinationFile, PlanRef.DestinationPhysicalRoot))
					{
						bGuardTripped = true;
						GuardCode = TEXT("destination_file_escape");
						GuardMessage = FString::Printf(
							TEXT("Resolved destination escapes pack root: %s (destination_root=%s, relative_path=%s, selected_source_root=%s, source_file=%s)"),
							*Row.DestinationFile,
							*PlanRef.DestinationPhysicalRoot,
							*RelativePath,
							*PlanRef.SelectedSourceRoot,
							*Row.SourceFile);
						return false;
					}

					const FString LowerRelative = RelativePath.ToLower();
					const FString LowerName = FPaths::GetBaseFilename(RelativePath).ToLower();
					if (LowerRelative.Contains(TEXT("anim"))
						|| LowerRelative.Contains(TEXT("motion"))
						|| LowerName.Contains(TEXT("walk"))
						|| LowerName.Contains(TEXT("run"))
						|| LowerName.Contains(TEXT("jump"))
						|| LowerName.Contains(TEXT("parkour"))
						|| LowerName.Contains(TEXT("montage")))
					{
						PlanRef.CandidateAnimationPaths.Add(Row.GamePackagePath);
					}

					if (IFileManager::Get().FileExists(*Row.DestinationFile))
					{
						Row.DestinationHash = HashFileToString(Row.DestinationFile);
						if (Row.SourceHash.IsEmpty() || Row.DestinationHash.IsEmpty() || Row.SourceHash != Row.DestinationHash)
						{
							Row.Status = TEXT("conflict");
							PlanRef.Conflicts.Add(Row);
						}
						else
						{
							Row.Status = TEXT("already_present");
							PlanRef.AlreadyPresent.Add(Row);
						}
					}
					else
					{
						Row.Status = TEXT("missing");
						PlanRef.Missing.Add(Row);
					}

					return true;
				}
			};

			FVisitor Visitor(Directories, OutPlan, MaxFiles, MaxTotalSizeBytes);
			PlatformFile.IterateDirectory(*CurrentDir, Visitor);
			if (Visitor.bGuardTripped)
			{
				OutBlockerCode = Visitor.GuardCode;
				OutBlockerMessage = Visitor.GuardMessage;
				return false;
			}
		}

		OutPlan.CandidateAnimationPaths.Sort();
		OutPlan.Missing.Sort([](const FIntakeFilePlan& A, const FIntakeFilePlan& B) { return A.RelativePath < B.RelativePath; });
		OutPlan.AlreadyPresent.Sort([](const FIntakeFilePlan& A, const FIntakeFilePlan& B) { return A.RelativePath < B.RelativePath; });
		OutPlan.Conflicts.Sort([](const FIntakeFilePlan& A, const FIntakeFilePlan& B) { return A.RelativePath < B.RelativePath; });

		if (OutPlan.CandidateAssetCount == 0)
		{
			OutBlockerCode = TEXT("no_uasset_or_umap_candidates");
			OutBlockerMessage = TEXT("No .uasset or .umap files were found under the selected pack root.");
			return false;
		}

		return true;
	}
}

FMCPToolResult FMCPTool_LocalAnimationPackIntake::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString SourceRootParam;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("source_root"), SourceRootParam, Error))
	{
		return Error.GetValue();
	}

	const FString SourceRoot = AbsoluteDirectoryForRead(SourceRootParam);
	const FString DestinationGameRootParam = ExtractOptionalString(Params, TEXT("destination_game_root"));
	const FString Mode = ExtractOptionalString(Params, TEXT("mode"), TEXT("dry_run")).ToLower();
	if (Mode != TEXT("dry_run") && Mode != TEXT("execute"))
	{
		return MakeBlocker(TEXT("invalid_mode"), TEXT("mode must be 'dry_run' or 'execute'."));
	}

	const int32 MaxFiles = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("max_files"), DefaultMaxFiles), 1, HardMaxFiles);
	const int64 MaxTotalSizeMb = FMath::Clamp<int64>(ExtractOptionalNumber<int64>(Params, TEXT("max_total_size_mb"), DefaultMaxTotalSizeMb), 1, HardMaxTotalSizeMb);
	const int64 MaxTotalSizeBytes = MaxTotalSizeMb * 1024 * 1024;

	FString DestinationContentRoot = AbsoluteDirectoryForWrite(FPaths::ProjectContentDir());
#if WITH_DEV_AUTOMATION_TESTS
	const FString TestDestinationContentRoot = ExtractOptionalString(Params, TEXT("test_destination_content_root"));
	const bool bUsingTestDestination = !TestDestinationContentRoot.IsEmpty();
	if (bUsingTestDestination)
	{
		DestinationContentRoot = AbsoluteDirectoryForWrite(TestDestinationContentRoot);
	}
#else
	const bool bUsingTestDestination = false;
#endif

	FIntakePlan Plan;
	FString BlockerCode;
	FString BlockerMessage;
	if (!BuildPlan(SourceRoot, DestinationGameRootParam, MaxFiles, MaxTotalSizeBytes, DestinationContentRoot, Plan, BlockerCode, BlockerMessage))
	{
		return MakeBlocker(BlockerCode, BlockerMessage, BuildResultData(Plan, TEXT("blocker")));
	}

	if (Plan.Conflicts.Num() > 0)
	{
		return MakeBlocker(
			TEXT("destination_conflict"),
			TEXT("One or more destination files already exist but do not byte-match the source; no files were copied."),
			BuildResultData(Plan, TEXT("blocker")));
	}

	if (Mode == TEXT("dry_run"))
	{
		return FMCPToolResult::Success(
			FString::Printf(
				TEXT("Dry-run ready: %d missing, %d already present, %d conflicts under %s"),
				Plan.Missing.Num(),
				Plan.AlreadyPresent.Num(),
				Plan.Conflicts.Num(),
				*Plan.DestinationGameRoot),
			BuildResultData(Plan, TEXT("dry_run_ready")));
	}

	IFileManager& FileManager = IFileManager::Get();
	for (const FIntakeFilePlan& Row : Plan.Missing)
	{
		if (FileManager.FileExists(*Row.DestinationFile))
		{
			FIntakePlan RacePlan = Plan;
			RacePlan.Conflicts.Add(Row);
			return MakeBlocker(
				TEXT("destination_race_conflict"),
				FString::Printf(TEXT("Destination appeared before copy and was not overwritten: %s"), *Row.DestinationFile),
				BuildResultData(RacePlan, TEXT("blocker")));
		}

		FileManager.MakeDirectory(*FPaths::GetPath(Row.DestinationFile), true);
		if (FileManager.Copy(*Row.DestinationFile, *Row.SourceFile, false, false) != COPY_OK)
		{
			if (FileManager.FileExists(*Row.DestinationFile))
			{
				FIntakePlan RacePlan = Plan;
				RacePlan.Conflicts.Add(Row);
				return MakeBlocker(
					TEXT("destination_race_conflict"),
					FString::Printf(TEXT("Destination appeared during copy and was not overwritten: %s"), *Row.DestinationFile),
					BuildResultData(RacePlan, TEXT("blocker")));
			}
			return MakeBlocker(
				TEXT("copy_failed"),
				FString::Printf(TEXT("Failed to copy %s to %s without overwrite or read-only override."), *Row.SourceFile, *Row.DestinationFile),
				BuildResultData(Plan, TEXT("blocker")));
		}

		const FString DestinationHashAfterCopy = HashFileToString(Row.DestinationFile);
		if (Row.SourceHash.IsEmpty() || DestinationHashAfterCopy.IsEmpty() || Row.SourceHash != DestinationHashAfterCopy)
		{
			TSharedPtr<FJsonObject> Data = BuildResultData(Plan, TEXT("blocker"));
			Data->SetStringField(TEXT("source_file"), Row.SourceFile);
			Data->SetStringField(TEXT("destination_file"), Row.DestinationFile);
			Data->SetStringField(TEXT("source_hash"), Row.SourceHash);
			Data->SetStringField(TEXT("destination_hash"), DestinationHashAfterCopy);
			return MakeBlocker(
				TEXT("copy_verification_failed"),
				FString::Printf(TEXT("Copied file hash mismatch for %s"), *Row.DestinationFile),
				Data);
		}
	}

	if (!bUsingTestDestination)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FString> PathsToScan = { Plan.DestinationGameRoot };
		AssetRegistryModule.Get().ScanPathsSynchronous(PathsToScan, true);
	}

	return FMCPToolResult::Success(
		FString::Printf(
			TEXT("Copied %d missing local animation pack files into %s; run animation_preflight next."),
			Plan.Missing.Num(),
			*Plan.DestinationGameRoot),
		BuildResultData(Plan, TEXT("executed")));
}
