// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/AppStyle.h"

/**
 * Editor commands for the Osvayder UE plugin
 */
class FOsvayderUECommands : public TCommands<FOsvayderUECommands>
{
public:
	FOsvayderUECommands()
		: TCommands<FOsvayderUECommands>(
			TEXT("OsvayderUE"),
			NSLOCTEXT("Contexts", "OsvayderUE", "Osvayder UE"),
			NAME_None,
			FAppStyle::GetAppStyleSetName())
	{
	}

	// TCommands interface
	virtual void RegisterCommands() override;

public:
	/** Open the Osvayder UE assistant panel */
	TSharedPtr<FUICommandInfo> OpenClaudePanel;
	
	/** Quick ask - opens a small dialog for quick questions */
	TSharedPtr<FUICommandInfo> QuickAsk;
};
