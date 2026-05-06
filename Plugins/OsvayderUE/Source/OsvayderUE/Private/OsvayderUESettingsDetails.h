// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "IDetailCustomization.h"

class IDetailCategoryBuilder;
class IDetailLayoutBuilder;

class FOsvayderUESettingsDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override;

private:
	static void AddGroupHeader(IDetailCategoryBuilder& Category, const FText& Title, const FText& Body);
	static void AddInlineNote(
		IDetailCategoryBuilder& Category,
		const FText& SearchText,
		const FText& Body,
		const TAttribute<EVisibility>& Visibility = EVisibility::Visible);
	static void SortCategories(IDetailLayoutBuilder& DetailLayout);
};
