// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeCommands.h"

#define LOCTEXT_NAMESPACE "UnrealClaude"

void FUnrealClaudeCommands::RegisterCommands()
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
