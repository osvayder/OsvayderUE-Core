// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "MCP/MCPTaskQueue.h"

/**
 * MCP Tool: Cancel an async task
 *
 * Requests cancellation of a pending or running task.
 * Pending tasks are cancelled immediately, running tasks are cancelled at the next opportunity.
 */
class FMCPTool_TaskCancel : public FMCPToolBase
{
public:
	FMCPTool_TaskCancel(TSharedPtr<FMCPTaskQueue> InTaskQueue)
		: TaskQueue(InTaskQueue)
	{
	}

	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("task_cancel");
		Info.Description = TEXT(
			"Cancel an async task.\n\n"
			"For pending tasks: Immediately marks the task as cancelled.\n"
			"For running tasks: Requests cancellation - the task will be cancelled at the next opportunity.\n"
			"For completed tasks: Returns an error (cannot cancel completed tasks).\n\n"
			"Note: Some tools may not check for cancellation during execution, "
			"so running tasks may still complete before the cancellation takes effect."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("task_id"), TEXT("string"),
				TEXT("Task ID to cancel"), true)
		};
		// P7 lifecycle override — documented uniformly across all Modifying/Destructive tools.
		Info.Parameters.Add(FMCPToolParameter(TEXT("auto_save"), TEXT("boolean"),
			TEXT("Override the project's autonomous mutation mode for this single call. Default: project setting (bAutonomousMutationMode). Pass false to leave the asset dirty in memory."), false));
		Info.Annotations = FMCPToolAnnotations::Destructive(
			TEXT("Cancels task execution. The task's work may be partially completed."));
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override
	{
		if (!TaskQueue.IsValid())
		{
			return FMCPToolResult::Error(TEXT("Task queue not initialized"));
		}

		// Extract task ID
		FString TaskIdString;
		TOptional<FMCPToolResult> Error;
		if (!ExtractRequiredString(Params, TEXT("task_id"), TaskIdString, Error))
		{
			return Error.GetValue();
		}

		// Parse GUID
		FGuid TaskId;
		if (!FGuid::Parse(TaskIdString, TaskId))
		{
			return FMCPToolResult::Error(TEXT("Invalid task_id format"));
		}

		// Try to cancel
		if (TaskQueue->CancelTask(TaskId))
		{
			TSharedPtr<FMCPAsyncTask> Task = TaskQueue->GetTask(TaskId);
			const EMCPTaskStatus Status = Task.IsValid() ? Task->Status.Load() : EMCPTaskStatus::Cancelled;
			TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
			ResultData->SetStringField(TEXT("task_id"), TaskIdString);
			ResultData->SetBoolField(TEXT("cancel_requested"), true);
			ResultData->SetBoolField(TEXT("cancelled"), Status == EMCPTaskStatus::Cancelled);
			ResultData->SetBoolField(TEXT("terminal"), Task.IsValid() ? Task->IsComplete() : true);
			ResultData->SetStringField(TEXT("status"), FMCPAsyncTask::StatusToString(Status));

			return FMCPToolResult::Success(
				FString::Printf(TEXT("Cancellation requested for task %s"), *TaskIdString),
				ResultData);
		}
		else
		{
			// Check why it failed
			TSharedPtr<FMCPAsyncTask> Task = TaskQueue->GetTask(TaskId);
			if (!Task.IsValid())
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Task not found: %s"), *TaskIdString));
			}
			else
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Cannot cancel task (status: %s)"),
						*FMCPAsyncTask::StatusToString(Task->Status.Load())));
			}
		}
	}

private:
	TSharedPtr<FMCPTaskQueue> TaskQueue;
};
