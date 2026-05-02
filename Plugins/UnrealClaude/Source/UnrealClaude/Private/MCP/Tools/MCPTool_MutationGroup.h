// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class FMCPTool_MutationGroup : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecutePreviewGroup(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteApplyGroup(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAbortGroup(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRevertGroup(const TSharedRef<FJsonObject>& Params);
};
