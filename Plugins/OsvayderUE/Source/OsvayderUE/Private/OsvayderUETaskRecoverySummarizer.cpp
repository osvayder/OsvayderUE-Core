// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUETaskRecoverySummarizer.h"

#include "OsvayderUERelayAgent.h"

namespace
{
	/**
	 * Truncate a string to <= MaxChars at the nearest preceding word boundary.
	 * If the string fits, returned unchanged. Adds a trailing " …" only when
	 * truncation actually happened.
	 */
	FString TruncateAtWordBoundary(const FString& Input, int32 MaxChars)
	{
		if (Input.Len() <= MaxChars)
		{
			return Input;
		}
		int32 Cut = MaxChars;
		while (Cut > 0 && !FChar::IsWhitespace(Input[Cut]))
		{
			--Cut;
		}
		if (Cut <= 0)
		{
			// No whitespace found — fall back to hard cut.
			Cut = MaxChars;
		}
		return Input.Left(Cut) + TEXT(" …");
	}

	/**
	 * Try to parse an ISO-8601 UTC timestamp. Returns bool success; on failure
	 * OutDateTime remains zero.
	 */
	bool TryParseIso8601Utc(const FString& Iso, FDateTime& OutDateTime)
	{
		if (Iso.IsEmpty())
		{
			return false;
		}
		return FDateTime::ParseIso8601(*Iso, OutDateTime);
	}

	/** Format a mechanic status token into "completed" / "pending" / etc. */
	FString FormatMechanicStatus(const FString& Status)
	{
		if (Status.IsEmpty())
		{
			return TEXT("pending");
		}
		return Status;
	}

	FString BuildPhaseStatusLine(const FOsvayderUEActivePlan& Plan)
	{
		if (Plan.Mechanics.Num() == 0)
		{
			return TEXT("No mechanic structure recorded.");
		}

		int32 CurrentIndex = INDEX_NONE;
		for (int32 i = 0; i < Plan.Mechanics.Num(); ++i)
		{
			if (Plan.Mechanics[i].MechanicId == Plan.CurrentMechanicId)
			{
				CurrentIndex = i;
				break;
			}
		}

		FString Line;
		if (CurrentIndex != INDEX_NONE)
		{
			const FOsvayderUEPlanMechanicEntry& Entry = Plan.Mechanics[CurrentIndex];
			Line = FString::Printf(
				TEXT("Phase %d of %d (%s) — %s."),
				CurrentIndex + 1,
				Plan.Mechanics.Num(),
				*Entry.MechanicId,
				*FormatMechanicStatus(Entry.Status));
		}
		else
		{
			Line = FString::Printf(
				TEXT("%d phases recorded (current mechanic id not found)."),
				Plan.Mechanics.Num());
		}

		// Append terse roll-up of other mechanics.
		TArray<FString> OtherParts;
		for (int32 i = 0; i < Plan.Mechanics.Num(); ++i)
		{
			if (i == CurrentIndex)
			{
				continue;
			}
			const FOsvayderUEPlanMechanicEntry& M = Plan.Mechanics[i];
			OtherParts.Add(FString::Printf(TEXT("%s %s"), *M.MechanicId, *FormatMechanicStatus(M.Status)));
		}
		if (OtherParts.Num() > 0)
		{
			Line += TEXT(" (") + FString::Join(OtherParts, TEXT(", ")) + TEXT(")");
		}
		return Line;
	}

	FString BuildProgressSummary(const FOsvayderUEActivePlan& Plan)
	{
		if (Plan.ToolCalls.Num() == 0)
		{
			return TEXT("No tool calls recorded.");
		}

		// Group consecutive calls of the same tool_name.
		TArray<FString> Lines;
		int32 i = 0;
		while (i < Plan.ToolCalls.Num())
		{
			const FString ToolName = Plan.ToolCalls[i].ToolName.IsEmpty()
				? TEXT("<unnamed>")
				: Plan.ToolCalls[i].ToolName;

			int32 GroupCount = 1;
			int32 j = i + 1;
			while (j < Plan.ToolCalls.Num() && Plan.ToolCalls[j].ToolName == Plan.ToolCalls[i].ToolName)
			{
				++GroupCount;
				++j;
			}

			// Pick a representative summary: prefer the last call in the group's Summary.
			const FOsvayderUEPlanToolCallEntry& Rep = Plan.ToolCalls[j - 1];
			FString RepSummary = Rep.Summary.IsEmpty() ? Rep.ResultStatus : Rep.Summary;
			if (RepSummary.Len() > 80)
			{
				RepSummary = RepSummary.Left(77) + TEXT("...");
			}

			if (GroupCount == 1)
			{
				Lines.Add(FString::Printf(TEXT("Ran %s once (%s)"),
					*ToolName,
					RepSummary.IsEmpty() ? TEXT("no summary") : *RepSummary));
			}
			else
			{
				Lines.Add(FString::Printf(TEXT("Ran %s %d times (%s)"),
					*ToolName,
					GroupCount,
					RepSummary.IsEmpty() ? TEXT("no summary") : *RepSummary));
			}

			i = j;
		}

		// Detect repeated error text (case-insensitive exact-match) across entire plan.
		TMap<FString, int32> ErrorCounts;
		for (const FOsvayderUEPlanToolCallEntry& TC : Plan.ToolCalls)
		{
			const FString ResultLower = TC.ResultStatus.ToLower();
			if (ResultLower.Contains(TEXT("error")) || ResultLower.Contains(TEXT("fail")))
			{
				const FString Key = TC.Summary.IsEmpty() ? TC.ResultStatus : TC.Summary;
				if (!Key.IsEmpty())
				{
					ErrorCounts.FindOrAdd(Key) += 1;
				}
			}
		}
		for (const TPair<FString, int32>& Pair : ErrorCounts)
		{
			if (Pair.Value >= 2)
			{
				Lines.Add(FString::Printf(TEXT("Encountered \"%s\" %d times"), *Pair.Key, Pair.Value));
			}
		}

		return FString::Join(Lines, TEXT("\n"));
	}

	FString BuildStallIndicator(const FOsvayderUEActivePlan& Plan)
	{
		if (Plan.ToolCalls.Num() < 3)
		{
			return FString();
		}
		const int32 N = Plan.ToolCalls.Num();
		const FOsvayderUEPlanToolCallEntry& A = Plan.ToolCalls[N - 3];
		const FOsvayderUEPlanToolCallEntry& B = Plan.ToolCalls[N - 2];
		const FOsvayderUEPlanToolCallEntry& C = Plan.ToolCalls[N - 1];

		// Compare result_status fields (canonical error surface). Must all
		// contain "error"/"fail" substring (case-insensitive) AND be identical.
		auto LooksLikeError = [](const FString& S)
		{
			const FString L = S.ToLower();
			return L.Contains(TEXT("error")) || L.Contains(TEXT("fail"));
		};
		if (!LooksLikeError(A.ResultStatus) || !LooksLikeError(B.ResultStatus) || !LooksLikeError(C.ResultStatus))
		{
			return FString();
		}
		if (A.ResultStatus != B.ResultStatus || B.ResultStatus != C.ResultStatus)
		{
			return FString();
		}
		return FString::Printf(TEXT("Likely stuck on \"%s\""), *C.ResultStatus);
	}

	FString PickLastIntent(const FOsvayderUEActivePlan& Plan)
	{
		if (!Plan.CurrentActionRu.IsEmpty())
		{
			return Plan.CurrentActionRu;
		}
		if (!Plan.CurrentAction.IsEmpty())
		{
			return Plan.CurrentAction;
		}
		return TEXT("(no current action recorded)");
	}
}

FTaskRecoverySummary FOsvayderUETaskRecoverySummarizer::BuildRecoverySummary(
	const FOsvayderUEActivePlan& Plan,
	const FDateTime& NowUtc,
	const FString& EvidencePath)
{
	FTaskRecoverySummary Out;

	Out.OriginalUserTask = Plan.OriginalUserTask;
	Out.ShortTitle = TruncateAtWordBoundary(Plan.OriginalUserTask, 80);
	Out.PhaseStatusLine = BuildPhaseStatusLine(Plan);
	Out.ProgressSummary = BuildProgressSummary(Plan);
	Out.LastIntent = PickLastIntent(Plan);
	Out.StallIndicator = BuildStallIndicator(Plan);
	Out.EvidencePointer = EvidencePath;

	FDateTime UpdatedAt;
	if (TryParseIso8601Utc(Plan.UpdatedAtUtc, UpdatedAt))
	{
		const FTimespan Delta = NowUtc - UpdatedAt;
		Out.ElapsedSincePause = FormatElapsed(Delta.GetTotalSeconds());
	}
	else
	{
		Out.ElapsedSincePause = TEXT("(unknown — updated_at_utc missing or unparseable)");
	}

	return Out;
}

FString FOsvayderUETaskRecoverySummarizer::FormatElapsed(int64 TotalSeconds)
{
	if (TotalSeconds < 30)
	{
		return TEXT("just now");
	}
	if (TotalSeconds < 60 * 60)
	{
		const int64 Minutes = TotalSeconds / 60;
		return FString::Printf(TEXT("%lld minute%s ago"),
			Minutes,
			Minutes == 1 ? TEXT("") : TEXT("s"));
	}
	if (TotalSeconds < 24LL * 60 * 60)
	{
		const int64 Hours = TotalSeconds / (60 * 60);
		const int64 Minutes = (TotalSeconds - Hours * 60 * 60) / 60;
		if (Minutes == 0)
		{
			return FString::Printf(TEXT("%lld hour%s ago"),
				Hours,
				Hours == 1 ? TEXT("") : TEXT("s"));
		}
		return FString::Printf(TEXT("%lld hour%s %lld minute%s ago"),
			Hours,
			Hours == 1 ? TEXT("") : TEXT("s"),
			Minutes,
			Minutes == 1 ? TEXT("") : TEXT("s"));
	}
	if (TotalSeconds < 7LL * 24 * 60 * 60)
	{
		const int64 Days = TotalSeconds / (24LL * 60 * 60);
		const int64 Hours = (TotalSeconds - Days * 24 * 60 * 60) / (60 * 60);
		if (Hours == 0)
		{
			return FString::Printf(TEXT("%lld day%s ago"),
				Days,
				Days == 1 ? TEXT("") : TEXT("s"));
		}
		return FString::Printf(TEXT("%lld day%s %lld hour%s ago"),
			Days,
			Days == 1 ? TEXT("") : TEXT("s"),
			Hours,
			Hours == 1 ? TEXT("") : TEXT("s"));
	}
	const int64 Weeks = TotalSeconds / (7LL * 24 * 60 * 60);
	return FString::Printf(TEXT("%lld week%s ago"),
		Weeks,
		Weeks == 1 ? TEXT("") : TEXT("s"));
}
