// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_EyeProxy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"

// Use WinHTTP for synchronous requests (UE's FHttpModule is async-only and crashes on game thread)
#include "Windows/AllowWindowsPlatformTypes.h"
#include <winhttp.h>
#include "Windows/HideWindowsPlatformTypes.h"

#pragma comment(lib, "winhttp.lib")

DEFINE_LOG_CATEGORY_STATIC(LogEyeProxy, Log, All);

// ─── Simple synchronous HTTP helper ───

static FString SyncHttpRequest(const FString& Verb, const FString& Url, const FString& Body = FString())
{
	// Parse URL: http://127.0.0.1:3001/path
	FString Host = TEXT("127.0.0.1");
	int32 Port = 3001;
	FString Path = TEXT("/");

	// Extract path from URL
	FString UrlCopy = Url;
	if (UrlCopy.RemoveFromStart(TEXT("http://")))
	{
		int32 PathStart;
		if (UrlCopy.FindChar('/', PathStart))
		{
			Path = UrlCopy.Mid(PathStart);
			UrlCopy = UrlCopy.Left(PathStart);
		}
		int32 ColonPos;
		if (UrlCopy.FindChar(':', ColonPos))
		{
			Host = UrlCopy.Left(ColonPos);
			Port = FCString::Atoi(*UrlCopy.Mid(ColonPos + 1));
		}
		else
		{
			Host = UrlCopy;
		}
	}

	HINTERNET hSession = WinHttpOpen(L"OsvayderEye/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
	if (!hSession) return FString();

	HINTERNET hConnect = WinHttpConnect(hSession, *Host, (INTERNET_PORT)Port, 0);
	if (!hConnect) { WinHttpCloseHandle(hSession); return FString(); }

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, *Verb, *Path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
	if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return FString(); }

	// Set timeout (5 seconds)
	WinHttpSetTimeouts(hRequest, 5000, 5000, 5000, 5000);

	BOOL bResult;
	if (Body.Len() > 0)
	{
		FTCHARToUTF8 BodyUtf8(*Body);
		LPCWSTR ContentType = L"Content-Type: application/json";
		bResult = WinHttpSendRequest(hRequest, ContentType, -1, (LPVOID)BodyUtf8.Get(), BodyUtf8.Length(), BodyUtf8.Length(), 0);
	}
	else
	{
		bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
	}

	if (!bResult || !WinHttpReceiveResponse(hRequest, NULL))
	{
		WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
		return FString();
	}

	// Read response
	TArray<uint8> ResponseData;
	DWORD BytesRead = 0;
	DWORD BytesAvailable = 0;
	while (WinHttpQueryDataAvailable(hRequest, &BytesAvailable) && BytesAvailable > 0)
	{
		int32 Offset = ResponseData.Num();
		ResponseData.AddUninitialized(BytesAvailable);
		WinHttpReadData(hRequest, ResponseData.GetData() + Offset, BytesAvailable, &BytesRead);
		ResponseData.SetNum(Offset + BytesRead);
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	// Convert to FString (UTF-8)
	ResponseData.Add(0);
	return FString(UTF8_TO_TCHAR(ResponseData.GetData()));
}

// ─── FMCPTool_EyeProxy ───

FMCPTool_EyeProxy::FMCPTool_EyeProxy(const FMCPToolInfo& InToolInfo, const FString& InBaseUrl)
	: ToolInfo(InToolInfo)
	, BaseUrl(InBaseUrl)
{
}

FMCPToolResult FMCPTool_EyeProxy::Execute(const TSharedRef<FJsonObject>& Params)
{
	// Build request body
	TSharedRef<FJsonObject> RequestBody = MakeShared<FJsonObject>();
	RequestBody->SetStringField(TEXT("tool"), ToolInfo.Name);
	RequestBody->SetObjectField(TEXT("args"), Params);

	FString RequestBodyString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
	FJsonSerializer::Serialize(RequestBody, Writer);

	// Synchronous HTTP POST
	FString ResponseString = SyncHttpRequest(TEXT("POST"), BaseUrl + TEXT("/execute"), RequestBodyString);

	if (ResponseString.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Eye sidecar not responding"));
	}

	// Parse response
	TSharedPtr<FJsonObject> ResponseJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
	if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
	{
		return FMCPToolResult::Error(TEXT("Failed to parse Eye response"));
	}

	bool bSuccess = ResponseJson->GetBoolField(TEXT("success"));
	FString Message = ResponseJson->GetStringField(TEXT("message"));
	TSharedPtr<FJsonObject> Data;
	const TSharedPtr<FJsonObject>* DataPtr;
	if (ResponseJson->TryGetObjectField(TEXT("data"), DataPtr) && DataPtr)
	{
		Data = *DataPtr;
	}

	if (bSuccess)
	{
		return FMCPToolResult::Success(Message, Data);
	}
	return FMCPToolResult::Error(Message);
}

// ─── EyeToolDiscovery ───

namespace EyeToolDiscovery
{
	static FMCPToolAnnotations ParseAnnotations(const TSharedPtr<FJsonObject>& AnnotJson)
	{
		FMCPToolAnnotations Annotations;
		if (AnnotJson.IsValid())
		{
			AnnotJson->TryGetBoolField(TEXT("readOnly"), Annotations.bReadOnlyHint);
			AnnotJson->TryGetBoolField(TEXT("destructive"), Annotations.bDestructiveHint);
			AnnotJson->TryGetBoolField(TEXT("idempotent"), Annotations.bIdempotentHint);
			AnnotJson->TryGetBoolField(TEXT("openWorld"), Annotations.bOpenWorldHint);
		}
		return Annotations;
	}

	int32 DiscoverAndRegister(FMCPToolRegistry& Registry, const FString& EyeServerUrl)
	{
		// Check /status
		FString StatusResponse = SyncHttpRequest(TEXT("GET"), EyeServerUrl + TEXT("/status"));
		if (StatusResponse.IsEmpty())
		{
			UE_LOG(LogEyeProxy, Log, TEXT("OsvayderEye sidecar not available at %s"), *EyeServerUrl);
			return -1;
		}

		// Get /tools
		FString ToolsResponse = SyncHttpRequest(TEXT("GET"), EyeServerUrl + TEXT("/tools"));
		if (ToolsResponse.IsEmpty())
		{
			UE_LOG(LogEyeProxy, Warning, TEXT("OsvayderEye /tools returned empty"));
			return -1;
		}

		// Parse tools JSON
		TSharedPtr<FJsonObject> ToolsJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolsResponse);
		if (!FJsonSerializer::Deserialize(Reader, ToolsJson) || !ToolsJson.IsValid())
		{
			UE_LOG(LogEyeProxy, Warning, TEXT("Failed to parse OsvayderEye /tools JSON"));
			return -1;
		}

		const TArray<TSharedPtr<FJsonValue>>* ToolsArray;
		if (!ToolsJson->TryGetArrayField(TEXT("tools"), ToolsArray))
		{
			UE_LOG(LogEyeProxy, Warning, TEXT("OsvayderEye /tools missing 'tools' array"));
			return -1;
		}

		int32 RegisteredCount = 0;

		for (const TSharedPtr<FJsonValue>& ToolValue : *ToolsArray)
		{
			const TSharedPtr<FJsonObject>* ToolObj;
			if (!ToolValue->TryGetObject(ToolObj) || !(*ToolObj).IsValid())
			{
				continue;
			}

			FMCPToolInfo Info;
			Info.Name = (*ToolObj)->GetStringField(TEXT("name"));
			Info.Description = (*ToolObj)->GetStringField(TEXT("description"));

			// Parse parameters
			const TArray<TSharedPtr<FJsonValue>>* ParamsArray;
			if ((*ToolObj)->TryGetArrayField(TEXT("parameters"), ParamsArray))
			{
				for (const TSharedPtr<FJsonValue>& ParamValue : *ParamsArray)
				{
					const TSharedPtr<FJsonObject>* ParamObj;
					if (ParamValue->TryGetObject(ParamObj) && (*ParamObj).IsValid())
					{
						FMCPToolParameter Param;
						Param.Name = (*ParamObj)->GetStringField(TEXT("name"));
						Param.Type = (*ParamObj)->GetStringField(TEXT("type"));
						Param.Description = (*ParamObj)->GetStringField(TEXT("description"));
						(*ParamObj)->TryGetBoolField(TEXT("required"), Param.bRequired);
						(*ParamObj)->TryGetStringField(TEXT("defaultValue"), Param.DefaultValue);
						Info.Parameters.Add(Param);
					}
				}
			}

			// Parse annotations
			const TSharedPtr<FJsonObject>* AnnotObj;
			if ((*ToolObj)->TryGetObjectField(TEXT("annotations"), AnnotObj))
			{
				Info.Annotations = ParseAnnotations(*AnnotObj);
			}
			Info.Annotations.bOpenWorldHint = true;

			if (Registry.HasTool(Info.Name))
			{
				continue;
			}

			TSharedPtr<FMCPTool_EyeProxy> ProxyTool = MakeShared<FMCPTool_EyeProxy>(Info, EyeServerUrl);
			Registry.RegisterTool(ProxyTool);
			RegisteredCount++;
		}

		UE_LOG(LogEyeProxy, Log, TEXT("OsvayderEye: registered %d tools from %s"), RegisteredCount, *EyeServerUrl);
		return RegisteredCount;
	}
}
