// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class FMCPTool_MetadataTruth : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("metadata_truth");
		Info.Description = TEXT(
			"Compare metadata/declaration truth against shipping config and current discovery state.\n\n"
			"This U3 slice is read-only and comparison-first. It does not repair content.\n"
			"It focuses on truthful consistency buckets across declaration surfaces,\n"
			"shipping/cook surfaces, and current discovery surfaces.\n\n"
			"Currently supported declaration surfaces:\n"
			"- explicit asset lists\n"
			"- DataTable string/name/text field values mapped into asset package paths\n\n"
			"Currently supported shipping surfaces:\n"
			"- MapsToCook from project config\n"
			"- explicit shipping asset lists\n\n"
			"Current discovery uses package presence checks through file/package resolution and asset-registry presence.\n"
			"Current discovery is not runtime proof."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("Operation to run. Supported: compare_asset_truth (default)."), false, TEXT("compare_asset_truth")),
			FMCPToolParameter(TEXT("declaration_surface_kind"), TEXT("string"),
				TEXT("Declaration surface kind: explicit_assets or data_table_string_field."), true),
			FMCPToolParameter(TEXT("declaration_items"), TEXT("array"),
				TEXT("Explicit declaration items. Each item may be a string asset path or an object with asset_path, item_id, and display_name."), false),
			FMCPToolParameter(TEXT("data_table_path"), TEXT("string"),
				TEXT("DataTable asset path for data_table_string_field declaration mode."), false),
			FMCPToolParameter(TEXT("data_table_field_name"), TEXT("string"),
				TEXT("Exact DataTable field name for data_table_string_field declaration mode."), false),
			FMCPToolParameter(TEXT("data_table_field_name_contains"), TEXT("string"),
				TEXT("Substring match for a unique DataTable field name when exact field_name is not convenient."), false),
			FMCPToolParameter(TEXT("data_table_package_template"), TEXT("string"),
				TEXT("Package template for DataTable values, for example /Game/Maps/{value}. If omitted and the field already contains a package path, the raw value is used."), false),
			FMCPToolParameter(TEXT("metadata_surface_name"), TEXT("string"),
				TEXT("Optional human-readable label for the declaration surface."), false),
			FMCPToolParameter(TEXT("shipping_surface_kind"), TEXT("string"),
				TEXT("Shipping surface kind: maps_to_cook or explicit_assets."), true),
			FMCPToolParameter(TEXT("shipping_items"), TEXT("array"),
				TEXT("Explicit shipping asset paths when shipping_surface_kind=explicit_assets."), false),
			FMCPToolParameter(TEXT("shipping_surface_name"), TEXT("string"),
				TEXT("Optional human-readable label for the shipping surface."), false),
			FMCPToolParameter(TEXT("include_unadvertised_shipped"), TEXT("boolean"),
				TEXT("If true, include shipped-but-unadvertised items from the shipping surface (default: true)."), false, TEXT("true")),
			FMCPToolParameter(TEXT("export_report"), TEXT("boolean"),
				TEXT("If true, persist a Markdown report and normalized sidecar under Saved/OsvayderUE/Reports."), false, TEXT("false")),
			FMCPToolParameter(TEXT("report_name"), TEXT("string"),
				TEXT("Optional custom report name when export_report=true."), false),
			FMCPToolParameter(TEXT("report_slug"), TEXT("string"),
				TEXT("Optional custom slug for saved report filenames when export_report=true."), false)
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
