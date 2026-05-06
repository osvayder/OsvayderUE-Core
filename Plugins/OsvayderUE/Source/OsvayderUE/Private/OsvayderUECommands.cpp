// Copyright Natali Caggiano. All Rights Reserved.

#include "OsvayderUECommands.h"

#define LOCTEXT_NAMESPACE "OsvayderUE"

void FOsvayderUECommands::RegisterCommands()
{
	UI_COMMAND(
		OpenClaudePanel,
		"Osvayder UE",
		"Open the Osvayder UE assistant panel for UE5.7 help",
		EUserInterfaceActionType::Button,
		FInputChord()
	);

	UI_COMMAND(
		QuickAsk,
		"Quick Ask",
		"Quickly ask the active assistant provider a question",
		EUserInterfaceActionType::Button,
		FInputChord()
	);
}

#undef LOCTEXT_NAMESPACE
