#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class FMCPTool_SandboxMapPlacement : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("sandbox_map_placement");
		Info.Description = TEXT(
			"Create/open a generated sandbox map, place one actor, save only that sandbox map package, and emit readback/proof data. "
			"Map paths must stay under /Game/__UnrealClaudeTestSandbox/packet<digits>_*; production maps and raw disk paths are rejected.");
		Info.Parameters = {
			FMCPToolParameter(TEXT("map_path"), TEXT("string"), TEXT("Required long package path under /Game/__UnrealClaudeTestSandbox/packet<digits>_*"), true),
			FMCPToolParameter(TEXT("actor_class"), TEXT("string"), TEXT("Actor class to place. Defaults to /Script/Engine.StaticMeshActor."), false, TEXT("/Script/Engine.StaticMeshActor")),
			FMCPToolParameter(TEXT("actor_name"), TEXT("string"), TEXT("Optional safe actor name. Defaults to Packet691_SandboxActor."), false),
			FMCPToolParameter(TEXT("location"), TEXT("object"), TEXT("Optional location object {x,y,z}."), false),
			FMCPToolParameter(TEXT("rotation"), TEXT("object"), TEXT("Optional rotation object {pitch,yaw,roll}."), false),
			FMCPToolParameter(TEXT("scale"), TEXT("object"), TEXT("Optional scale object {x,y,z}."), false),
			FMCPToolParameter(TEXT("replace_existing"), TEXT("boolean"), TEXT("If true, overwrite an existing sandbox map at the same path. Defaults false."), false, TEXT("false")),
			FMCPToolParameter(TEXT("export_proof"), TEXT("boolean"), TEXT("Write a JSON placement proof under Saved/Logs/<packet>_sandbox_map_placement. Defaults true."), false, TEXT("true"))
		};
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

	static bool NormalizeAndValidateSandboxMapPath(const FString& InPath, FString& OutMapPath, FString& OutError, FString& OutReason);
	static FString GetSandboxRootPath();
	static FString GetPacketSandboxPrefix();
	static bool TryExtractPacketLabelFromSandboxMapPath(const FString& MapPath, FString& OutPacketLabel);
};
