// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Forward declarations
class FMCPTaskQueue;

/**
 * Tool behavior annotations (hints for LLM clients)
 * These help clients understand tool behavior without being security guarantees
 */
struct FMCPToolAnnotations
{
	/** Tool does not modify its environment (read-only operation) */
	bool bReadOnlyHint = false;

	/** Tool may perform destructive updates (delete, overwrite) */
	bool bDestructiveHint = true;

	/** Repeated calls with same args have no additional effect */
	bool bIdempotentHint = false;

	/** Tool interacts with external entities beyond local environment */
	bool bOpenWorldHint = false;

	FMCPToolAnnotations() = default;

	/** True when this tool does not mutate its environment. Used by the mutation-lifecycle wrapper to bypass dirty-package snapshotting and save. */
	bool IsReadOnly() const
	{
		return bReadOnlyHint;
	}

	/** Create read-only tool annotations */
	static FMCPToolAnnotations ReadOnly()
	{
		FMCPToolAnnotations A;
		A.bReadOnlyHint = true;
		A.bDestructiveHint = false;
		A.bIdempotentHint = true;
		A.bOpenWorldHint = false;
		return A;
	}

	/** Create modifying (non-destructive) tool annotations */
	static FMCPToolAnnotations Modifying()
	{
		FMCPToolAnnotations A;
		A.bReadOnlyHint = false;
		A.bDestructiveHint = false;
		A.bIdempotentHint = false;
		A.bOpenWorldHint = false;
		return A;
	}

	/** Create destructive tool annotations */
	static FMCPToolAnnotations Destructive()
	{
		FMCPToolAnnotations A;
		A.bReadOnlyHint = false;
		A.bDestructiveHint = true;
		A.bIdempotentHint = false;
		A.bOpenWorldHint = false;
		return A;
	}

	/** Create destructive tool annotations with message (for documentation) */
	static FMCPToolAnnotations Destructive(const FString& /*WarningMessage*/)
	{
		// Message is for documentation purposes only
		return Destructive();
	}
};

/**
 * Parameter definition for an MCP tool
 */
struct FMCPToolParameter
{
	/** Parameter name */
	FString Name;

	/** Parameter type (string, number, boolean, array, object) */
	FString Type;

	/** Description of the parameter */
	FString Description;

	/** Whether this parameter is required */
	bool bRequired;

	/** Default value if not provided */
	FString DefaultValue;

	FMCPToolParameter()
		: bRequired(false)
	{}

	FMCPToolParameter(const FString& InName, const FString& InType, const FString& InDescription, bool bInRequired = false, const FString& InDefault = TEXT(""))
		: Name(InName)
		, Type(InType)
		, Description(InDescription)
		, bRequired(bInRequired)
		, DefaultValue(InDefault)
	{}
};

/**
 * Information about an MCP tool
 */
struct FMCPToolInfo
{
	/** Unique name of the tool */
	FString Name;

	/** Human-readable description */
	FString Description;

	/** Parameter definitions */
	TArray<FMCPToolParameter> Parameters;

	/** Behavioral annotations/hints for LLM clients */
	FMCPToolAnnotations Annotations;
};

/**
 * Result from executing an MCP tool
 */
struct FMCPToolResult
{
	/** Whether the operation succeeded */
	bool bSuccess;

	/** Human-readable message */
	FString Message;

	/** Optional structured data result */
	TSharedPtr<FJsonObject> Data;

	FMCPToolResult()
		: bSuccess(false)
	{}

	static FMCPToolResult Success(const FString& InMessage, TSharedPtr<FJsonObject> InData = nullptr)
	{
		FMCPToolResult Result;
		Result.bSuccess = true;
		Result.Message = InMessage;
		Result.Data = InData;
		return Result;
	}

	static FString ClassifyErrorMessage(const FString& InMessage)
	{
		const FString LowerMessage = InMessage.ToLower();

		if (LowerMessage.Contains(TEXT("missing required parameter"))
			|| (LowerMessage.Contains(TEXT("missing")) && LowerMessage.Contains(TEXT("parameter"))))
		{
			return TEXT("missing_required_parameter");
		}

		if (LowerMessage.Contains(TEXT("unknown operation")))
		{
			return TEXT("unknown_operation");
		}

		if (LowerMessage.Contains(TEXT("unknown execution_profile")))
		{
			return TEXT("unknown_execution_profile");
		}

		if (LowerMessage.Contains(TEXT("property path")))
		{
			return TEXT("invalid_property_path");
		}

		if (LowerMessage.Contains(TEXT("actor name")))
		{
			return TEXT("invalid_actor_name");
		}

		if (LowerMessage.Contains(TEXT("cannot access engine or script"))
			|| LowerMessage.Contains(TEXT("blocked for safety"))
			|| LowerMessage.Contains(TEXT("not allowed")))
		{
			return TEXT("safety_blocked");
		}

		if (LowerMessage.Contains(TEXT("path traversal"))
			|| LowerMessage.Contains(TEXT("dangerous character"))
			|| LowerMessage.Contains(TEXT("contains invalid character"))
			|| LowerMessage.Contains(TEXT("invalid path"))
			|| LowerMessage.Contains(TEXT("invalid package path"))
			|| LowerMessage.Contains(TEXT("invalid asset path"))
			|| LowerMessage.Contains(TEXT("cannot contain")))
		{
			return TEXT("invalid_path");
		}

		if (LowerMessage.Contains(TEXT("not found"))
			|| LowerMessage.Contains(TEXT("could not find"))
			|| LowerMessage.Contains(TEXT("could not load"))
			|| LowerMessage.Contains(TEXT("failed to load")))
		{
			return TEXT("not_found");
		}

		if (LowerMessage.Contains(TEXT("timed out")))
		{
			return TEXT("timeout");
		}

		if (LowerMessage.Contains(TEXT("cancelled"))
			|| LowerMessage.Contains(TEXT("canceled")))
		{
			return TEXT("cancelled");
		}

		return TEXT("tool_error");
	}

	static TSharedPtr<FJsonObject> MakeErrorData(const FString& InMessage)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("result_type"), TEXT("tool_error"));
		ErrorData->SetStringField(TEXT("schema_version"), TEXT("mcp_tool_error.v1"));
		ErrorData->SetStringField(TEXT("error_category"), ClassifyErrorMessage(InMessage));
		ErrorData->SetStringField(TEXT("error_message"), InMessage);
		ErrorData->SetBoolField(TEXT("structured_error"), true);
		return ErrorData;
	}

	static FMCPToolResult Error(const FString& InMessage)
	{
		FMCPToolResult Result;
		Result.bSuccess = false;
		Result.Message = InMessage;
		Result.Data = MakeErrorData(InMessage);
		return Result;
	}
};

/**
 * Base class for MCP tools
 */
class IMCPTool
{
public:
	virtual ~IMCPTool() = default;

	/** Get tool info (name, description, parameters) */
	virtual FMCPToolInfo GetInfo() const = 0;

	/** Execute the tool with given parameters */
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) = 0;
};

/**
 * Registry for managing MCP tools
 */
class FMCPToolRegistry
{
public:
	FMCPToolRegistry();
	~FMCPToolRegistry();

	/** Register a tool */
	void RegisterTool(TSharedPtr<IMCPTool> Tool);

	/** Unregister a tool by name */
	void UnregisterTool(const FString& ToolName);

	/** Get all registered tools */
	TArray<FMCPToolInfo> GetAllTools() const;

	/** Execute a tool by name */
	FMCPToolResult ExecuteTool(const FString& ToolName, const TSharedRef<FJsonObject>& Params);

	/** Build a real policy-denied result for governed direct-tool surfaces when current execution profile forbids them */
	bool TryBuildGovernanceDenyResult(const FString& ToolName, const TSharedRef<FJsonObject>& Params, FMCPToolResult& OutResult) const;

	/** Check if a tool exists */
	bool HasTool(const FString& ToolName) const;

	/** Find a tool by name (returns nullptr if not found) */
	IMCPTool* FindTool(const FString& ToolName) const
	{
		const TSharedPtr<IMCPTool>* Found = Tools.Find(ToolName);
		return Found && Found->IsValid() ? Found->Get() : nullptr;
	}

	/** Get the async task queue */
	TSharedPtr<FMCPTaskQueue> GetTaskQueue() const { return TaskQueue; }

	/** Start the async task queue (call after construction) */
	void StartTaskQueue();

	/** Stop the async task queue (call before destruction) */
	void StopTaskQueue();

private:
	/** Register all built-in tools */
	void RegisterBuiltinTools();

	/** Invalidate cached tool list */
	void InvalidateToolCache();

	/** Map of tool name to tool instance */
	TMap<FString, TSharedPtr<IMCPTool>> Tools;

	/** Cached tool info list for performance */
	mutable TArray<FMCPToolInfo> CachedToolInfo;

	/** Whether the cached tool list is valid */
	mutable bool bCacheValid = false;

	/** Async task queue for long-running operations */
	TSharedPtr<FMCPTaskQueue> TaskQueue;
};
