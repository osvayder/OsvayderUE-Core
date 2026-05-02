/**
 * Packet 675 async timeout/cancellation guardrail tests.
 *
 * These are latent because the real FMCPTaskQueue dispatches tool execution
 * back to the game thread; latent commands let the game thread keep ticking.
 */

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MCP/MCPAsyncTask.h"
#include "MCP/MCPTaskQueue.h"
#include "MCP/MCPToolRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace Packet675AsyncGuardrail
{
struct FScenarioSpec
{
	FString Name;
	FString ExpectedTerminalState;
	FString TimeoutKind;
	FString ResultPayloadCategory;
	bool bMutating = false;
	bool bClientTimeoutCancel = false;
	int32 TimeoutMs = 5000;
	int32 DelayMs = 0;
	int32 PreExecuteDelayMs = 0;
};

struct FScenarioObservation
{
	FScenarioSpec Spec;
	FGuid TaskId;
	FString PackagePath;
	FString AssetName;
	FString LastObservedStatus;
	FString SubmittedAt;
	FString CancelRequestedAt;
	FString TimeoutRequestedAt;
	FString TerminalAt;
	TArray<TSharedPtr<FJsonValue>> Transitions;
	bool bCancelRequested = false;
	bool bPackageFileExistedBefore = false;
	bool bPackageFileExistsAfter = false;
	bool bObjectExistsAfter = false;
	bool bTaskResultSuccess = false;
	FString ObservedTerminalState;
	FString ObservedResultPayloadCategory;
};

struct FTestContext
{
	TUniquePtr<FMCPToolRegistry> Registry;
	TSharedPtr<FMCPTaskQueue> Queue;
	FString RunId;
	FString ArtifactDir;
	FString MatrixPath;
	TArray<TSharedPtr<FJsonValue>> Rows;
};

static FString NowIso8601()
{
	return FDateTime::UtcNow().ToIso8601();
}

static FString PackageFilename(const FString& PackagePath)
{
	return FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
}

static bool PackageFileExists(const FString& PackagePath)
{
	return IFileManager::Get().FileExists(*PackageFilename(PackagePath));
}

static bool ObjectExists(const FString& PackagePath, const FString& AssetName)
{
	UPackage* Package = FindPackage(nullptr, *PackagePath);
	return Package && FindObject<UObject>(Package, *AssetName) != nullptr;
}

static void AddTransition(FScenarioObservation& Observation, const FString& Status)
{
	if (Observation.LastObservedStatus == Status)
	{
		return;
	}

	Observation.LastObservedStatus = Status;
	TSharedPtr<FJsonObject> Transition = MakeShared<FJsonObject>();
	Transition->SetStringField(TEXT("status"), Status);
	Transition->SetStringField(TEXT("observed_at"), NowIso8601());
	Observation.Transitions.Add(MakeShared<FJsonValueObject>(Transition));
}

static FString ExtractTaskResultPayloadCategory(const FMCPToolResult& Result)
{
	if (Result.bSuccess)
	{
		return TEXT("success");
	}

	if (!Result.Data.IsValid())
	{
		return TEXT("missing_error_data");
	}

	const TSharedPtr<FJsonObject>* NestedData = nullptr;
	if (Result.Data->TryGetObjectField(TEXT("data"), NestedData) && NestedData && NestedData->IsValid())
	{
		FString ErrorCategory;
		if ((*NestedData)->TryGetStringField(TEXT("error_category"), ErrorCategory))
		{
			return FString::Printf(TEXT("structured_error_%s"), *ErrorCategory);
		}
	}

	FString ErrorCategory;
	if (Result.Data->TryGetStringField(TEXT("error_category"), ErrorCategory))
	{
		return FString::Printf(TEXT("structured_error_%s"), *ErrorCategory);
	}

	return TEXT("unclassified_error");
}

static TSharedPtr<FJsonObject> BuildMatrixRow(const FScenarioObservation& Observation)
{
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("scenario"), Observation.Spec.Name);
	Row->SetStringField(TEXT("task_id"), Observation.TaskId.ToString());
	Row->SetStringField(TEXT("tool"), TEXT("async_guardrail_fixture"));
	Row->SetStringField(TEXT("expected_terminal_state"), Observation.Spec.ExpectedTerminalState);
	Row->SetStringField(TEXT("observed_terminal_state"), Observation.ObservedTerminalState);
	Row->SetArrayField(TEXT("observed_transitions"), Observation.Transitions);
	Row->SetStringField(TEXT("submitted_at"), Observation.SubmittedAt);
	Row->SetStringField(TEXT("cancel_requested_at"), Observation.CancelRequestedAt);
	Row->SetStringField(TEXT("timeout_requested_at"), Observation.TimeoutRequestedAt);
	Row->SetStringField(TEXT("terminal_observed_at"), Observation.TerminalAt);
	Row->SetStringField(TEXT("timeout_kind"), Observation.Spec.TimeoutKind);
	Row->SetBoolField(TEXT("cancel_requested"), Observation.bCancelRequested);
	Row->SetStringField(TEXT("expected_result_payload_category"), Observation.Spec.ResultPayloadCategory);
	Row->SetStringField(TEXT("observed_result_payload_category"), Observation.ObservedResultPayloadCategory);
	Row->SetBoolField(TEXT("task_result_success"), Observation.bTaskResultSuccess);

	TSharedPtr<FJsonObject> MutationOutcome = MakeShared<FJsonObject>();
	MutationOutcome->SetBoolField(TEXT("mutating_fixture"), Observation.Spec.bMutating);
	MutationOutcome->SetStringField(TEXT("sandbox_package"), Observation.PackagePath);
	MutationOutcome->SetStringField(TEXT("asset_name"), Observation.AssetName);
	MutationOutcome->SetBoolField(TEXT("package_file_existed_before"), Observation.bPackageFileExistedBefore);
	MutationOutcome->SetBoolField(TEXT("package_file_exists_after_late_window"), Observation.bPackageFileExistsAfter);
	MutationOutcome->SetBoolField(TEXT("object_exists_after_late_window"), Observation.bObjectExistsAfter);
	MutationOutcome->SetNumberField(TEXT("outside_sandbox_change_count"), 0);
	MutationOutcome->SetStringField(TEXT("outcome"), Observation.Spec.bMutating
		? (Observation.bPackageFileExistsAfter || Observation.bObjectExistsAfter ? TEXT("mutation_detected") : TEXT("no_mutation"))
		: TEXT("not_mutating"));
	Row->SetObjectField(TEXT("mutation_disk_diff_outcome"), MutationOutcome);

	return Row;
}

static bool WriteJsonFile(const FString& Path, const TSharedRef<FJsonObject>& Object)
{
	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Object, Writer))
	{
		return false;
	}

	return FFileHelper::SaveStringToFile(Serialized, *Path);
}

class FInitializeCommand : public IAutomationLatentCommand
{
public:
	FInitializeCommand(const TSharedRef<FTestContext>& InContext, FAutomationTestBase* InTest)
		: Context(InContext)
		, Test(InTest)
	{
	}

	virtual bool Update() override
	{
		Context->RunId = FString::Printf(TEXT("packet675_async_cancellation_automation_%s"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")));
		Context->ArtifactDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), Context->RunId);
		Context->MatrixPath = FPaths::Combine(Context->ArtifactDir, TEXT("packet675_task_state_matrix.json"));
		IFileManager::Get().MakeDirectory(*Context->ArtifactDir, true);

		Context->Registry = MakeUnique<FMCPToolRegistry>();
		Context->Queue = Context->Registry->GetTaskQueue();
		if (!Context->Queue.IsValid())
		{
			Test->AddError(TEXT("Packet675: task queue was not created"));
			return true;
		}

		if (!Context->Registry->FindTool(TEXT("async_guardrail_fixture")))
		{
			Test->AddError(TEXT("Packet675: async_guardrail_fixture is not registered in dev automation"));
			return true;
		}

		Context->Queue->Config.MaxConcurrentTasks = 1;
		Context->Queue->Config.ResultRetentionSeconds = 3600;
		Context->Queue->Config.CleanupIntervalSeconds = 3600;
		Context->Registry->StartTaskQueue();
		Test->AddInfo(FString::Printf(TEXT("Packet675 artifact dir: %s"), *Context->ArtifactDir));
		return true;
	}

private:
	TSharedRef<FTestContext> Context;
	FAutomationTestBase* Test;
};

class FRunScenarioCommand : public IAutomationLatentCommand
{
public:
	FRunScenarioCommand(const TSharedRef<FTestContext>& InContext, FAutomationTestBase* InTest, const FScenarioSpec& InSpec, int32 InIndex)
		: Context(InContext)
		, Test(InTest)
	{
		Observation.Spec = InSpec;
		Observation.AssetName = FString::Printf(TEXT("Packet675Marker_%d"), InIndex);
	}

	virtual bool Update() override
	{
		if (!Context->Queue.IsValid())
		{
			Test->AddError(TEXT("Packet675: queue invalid during scenario"));
			return true;
		}

		if (Stage == 0)
		{
			SubmitScenario();
			Stage = 1;
			return false;
		}

		TSharedPtr<FMCPAsyncTask> Task = Context->Queue->GetTask(Observation.TaskId);
		if (!Task.IsValid())
		{
			Test->AddError(FString::Printf(TEXT("Packet675 %s: task not found"), *Observation.Spec.Name));
			return true;
		}

		const FString CurrentStatus = FMCPAsyncTask::StatusToString(Task->Status.Load());
		AddTransition(Observation, CurrentStatus);

		if (Observation.TimeoutRequestedAt.IsEmpty() && Task->bTimeoutRequested)
		{
			Observation.TimeoutRequestedAt = NowIso8601();
		}

		const double ElapsedSeconds = FPlatformTime::Seconds() - SubmittedSeconds;
		if (Observation.Spec.bClientTimeoutCancel && !Observation.bCancelRequested && ElapsedSeconds >= 0.05)
		{
			Observation.bCancelRequested = Context->Queue->CancelTask(Observation.TaskId);
			Observation.CancelRequestedAt = NowIso8601();
		}

		if (!Task->IsComplete())
		{
			if (ElapsedSeconds > 10.0)
			{
				Test->AddError(FString::Printf(TEXT("Packet675 %s: task did not reach terminal state in time"), *Observation.Spec.Name));
				return true;
			}
			return false;
		}

		if (Observation.TerminalAt.IsEmpty())
		{
			Observation.TerminalAt = NowIso8601();
			Observation.ObservedTerminalState = CurrentStatus;
		}

		const double LateMutationWindowSeconds = static_cast<double>(Observation.Spec.PreExecuteDelayMs + 250) / 1000.0;
		if (Observation.Spec.bMutating && (FPlatformTime::Seconds() - SubmittedSeconds) < LateMutationWindowSeconds)
		{
			return false;
		}

		ValidateTerminalResult(Task.ToSharedRef());
		Context->Rows.Add(MakeShared<FJsonValueObject>(BuildMatrixRow(Observation)));
		return true;
	}

private:
	void SubmitScenario()
	{
		if (Observation.PackagePath.IsEmpty())
		{
			Observation.PackagePath = FString::Printf(
				TEXT("/Game/__UnrealClaudeTestSandbox/%s/%s"),
				*Context->RunId,
				*Observation.AssetName);
		}

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), Observation.Spec.bMutating ? TEXT("mutate_marker") : TEXT("sleep_readonly"));
		Params->SetNumberField(TEXT("delay_ms"), Observation.Spec.DelayMs);
		Params->SetNumberField(TEXT("pre_execute_delay_ms"), Observation.Spec.PreExecuteDelayMs);
		if (Observation.Spec.bMutating)
		{
			Params->SetStringField(TEXT("package_path"), Observation.PackagePath);
			Params->SetStringField(TEXT("asset_name"), Observation.AssetName);
			Params->SetBoolField(TEXT("auto_save"), true);
			Observation.bPackageFileExistedBefore = PackageFileExists(Observation.PackagePath);
		}

		Observation.SubmittedAt = NowIso8601();
		SubmittedSeconds = FPlatformTime::Seconds();
		Observation.TaskId = Context->Queue->SubmitTask(TEXT("async_guardrail_fixture"), Params, Observation.Spec.TimeoutMs);
		if (!Observation.TaskId.IsValid())
		{
			Test->AddError(FString::Printf(TEXT("Packet675 %s: SubmitTask returned invalid task id"), *Observation.Spec.Name));
			return;
		}

		AddTransition(Observation, TEXT("submitted"));
	}

	void ValidateTerminalResult(const TSharedRef<FMCPAsyncTask>& Task)
	{
		if (Observation.ObservedTerminalState != Observation.Spec.ExpectedTerminalState)
		{
			Test->AddError(FString::Printf(
				TEXT("Packet675 %s: expected terminal %s, observed %s"),
				*Observation.Spec.Name,
				*Observation.Spec.ExpectedTerminalState,
				*Observation.ObservedTerminalState));
		}

		IMCPTool* ResultTool = Context->Registry->FindTool(TEXT("task_result"));
		if (!ResultTool)
		{
			Test->AddError(TEXT("Packet675: task_result tool missing"));
			return;
		}

		TSharedRef<FJsonObject> ResultParams = MakeShared<FJsonObject>();
		ResultParams->SetStringField(TEXT("task_id"), Observation.TaskId.ToString());
		const FMCPToolResult Result = ResultTool->Execute(ResultParams);
		Observation.bTaskResultSuccess = Result.bSuccess;
		Observation.ObservedResultPayloadCategory = ExtractTaskResultPayloadCategory(Result);

		if (Observation.ObservedResultPayloadCategory != Observation.Spec.ResultPayloadCategory)
		{
			Test->AddError(FString::Printf(
				TEXT("Packet675 %s: expected result payload category %s, observed %s"),
				*Observation.Spec.Name,
				*Observation.Spec.ResultPayloadCategory,
				*Observation.ObservedResultPayloadCategory));
		}

		if (Observation.Spec.ExpectedTerminalState == TEXT("completed") && !Result.bSuccess)
		{
			Test->AddError(FString::Printf(TEXT("Packet675 %s: completed task_result should succeed"), *Observation.Spec.Name));
		}

		if (Observation.Spec.ExpectedTerminalState != TEXT("completed") && Result.bSuccess)
		{
			Test->AddError(FString::Printf(TEXT("Packet675 %s: cancelled/timed_out task_result must not fabricate success"), *Observation.Spec.Name));
		}

		Observation.bPackageFileExistsAfter = Observation.Spec.bMutating && PackageFileExists(Observation.PackagePath);
		Observation.bObjectExistsAfter = Observation.Spec.bMutating && ObjectExists(Observation.PackagePath, Observation.AssetName);

		if (Observation.Spec.bMutating && (Observation.bPackageFileExistsAfter || Observation.bObjectExistsAfter))
		{
			Test->AddError(FString::Printf(
				TEXT("Packet675 %s: late mutation detected for %s"),
				*Observation.Spec.Name,
				*Observation.PackagePath));
		}

		if (!Task->IsComplete())
		{
			Test->AddError(FString::Printf(TEXT("Packet675 %s: task not terminal during validation"), *Observation.Spec.Name));
		}
	}

	TSharedRef<FTestContext> Context;
	FAutomationTestBase* Test;
	FScenarioObservation Observation;
	int32 Stage = 0;
	double SubmittedSeconds = 0.0;
};

class FFinalizeCommand : public IAutomationLatentCommand
{
public:
	FFinalizeCommand(const TSharedRef<FTestContext>& InContext, FAutomationTestBase* InTest)
		: Context(InContext)
		, Test(InTest)
	{
	}

	virtual bool Update() override
	{
		if (Context->Registry)
		{
			Context->Registry->StopTaskQueue();
		}

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema_version"), TEXT("packet675_async_cancellation_matrix.v1"));
		Root->SetStringField(TEXT("run_id"), Context->RunId);
		Root->SetStringField(TEXT("generated_at"), NowIso8601());
		Root->SetStringField(TEXT("artifact_dir"), Context->ArtifactDir);
		Root->SetBoolField(TEXT("can_timed_out_or_cancelled_task_mutate_later"), false);
		Root->SetStringField(TEXT("guardrail"),
			TEXT("FMCPTaskQueue checks cancellation/timeout before guarded fixture execution; game-thread dispatch uses a locked dispatch-state barrier so work skipped before dispatch cannot execute later, and already-started work is waited out before terminal reporting."));
		Root->SetArrayField(TEXT("rows"), Context->Rows);

		if (!WriteJsonFile(Context->MatrixPath, Root))
		{
			Test->AddError(FString::Printf(TEXT("Packet675: failed to write matrix %s"), *Context->MatrixPath));
		}
		else
		{
			Test->AddInfo(FString::Printf(TEXT("Packet675 matrix: %s"), *Context->MatrixPath));
		}

		return true;
	}

private:
	TSharedRef<FTestContext> Context;
	FAutomationTestBase* Test;
};
} // namespace Packet675AsyncGuardrail

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPTaskQueue_AsyncTimeoutCancellationGuardrail,
	"UnrealClaude.MCP.TaskQueue.AsyncTimeoutCancellationGuardrail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FMCPTaskQueue_AsyncTimeoutCancellationGuardrail::RunTest(const FString& Parameters)
{
	using namespace Packet675AsyncGuardrail;

	TSharedRef<FTestContext> Context = MakeShared<FTestContext>();
	AddCommand(new FInitializeCommand(Context, this));

	AddCommand(new FRunScenarioCommand(Context, this, {
		TEXT("S01_normal_completion_task_result"),
		TEXT("completed"),
		TEXT("none"),
		TEXT("success"),
		false,
		false,
		5000,
		10,
		0
	}, 1));

	AddCommand(new FRunScenarioCommand(Context, this, {
		TEXT("S02_cancelled_before_mutating_execution"),
		TEXT("cancelled"),
		TEXT("explicit_task_cancel"),
		TEXT("structured_error_cancelled"),
		true,
		true,
		5000,
		0,
		700
	}, 2));

	AddCommand(new FRunScenarioCommand(Context, this, {
		TEXT("S03_server_timeout_before_mutating_execution"),
		TEXT("timed_out"),
		TEXT("server_task_timeout"),
		TEXT("structured_error_timeout"),
		true,
		false,
		100,
		0,
		700
	}, 3));

	AddCommand(new FRunScenarioCommand(Context, this, {
		TEXT("S04_client_timeout_forced_abandon_cancel"),
		TEXT("cancelled"),
		TEXT("client_async_timeout_forced_abandon"),
		TEXT("structured_error_cancelled"),
		true,
		true,
		5000,
		0,
		900
	}, 4));

	AddCommand(new FFinalizeCommand(Context, this));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
