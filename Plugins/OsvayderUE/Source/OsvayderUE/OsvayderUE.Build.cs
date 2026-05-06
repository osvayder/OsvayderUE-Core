// Copyright Natali Caggiano. All Rights Reserved.

using UnrealBuildTool;

public class OsvayderUE : ModuleRules
{
	public OsvayderUE(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
			}
		);
				
		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);
			
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Slate",
				"SlateCore",
				"EditorStyle",
				"UnrealEd",
				"ToolMenus",
				"Projects",
				"EditorFramework",
				"WorkspaceMenuStructure"
			}
		);
			
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Json",
				"JsonUtilities",
				"HTTP",
				"HTTPServer",
				"Sockets",
				"Networking",
				"ImageWrapper",
				// Blueprint manipulation
				"Kismet",
				"KismetCompiler",
				"BlueprintGraph",
				"GraphEditor",
				"AssetRegistry",
				"AssetTools",
				// Animation Blueprint manipulation
				"AnimGraph",
				"AnimGraphRuntime",
				"IKRig",
				"IKRigEditor",
				// Asset saving
				"EditorScriptingUtilities",
				// Enhanced Input
				"EnhancedInput",
				// Settings UI
				"DeveloperSettings",
				"PropertyEditor",
				"Settings",
				// Editor Utilities
				"Blutility",
				"UMG",
				"UMGEditor",
				// GAS (Gameplay Ability System)
				"GameplayAbilities",
				"GameplayTags",
				"GameplayTasks",
				// Niagara VFX
				"Niagara",
				"NiagaraCore",
				// Online/Sessions
				"OnlineSubsystem",
				"OnlineSubsystemUtils",
				// AI
				"AIModule",
				"GameplayTasks",
				// Sequencer / Cinematics
				"LevelSequence",
				"MovieScene",
				"MovieSceneTracks",
				// Source control (plan 616 P3 — mutation-lifecycle silent checkout)
				"SourceControl"
			}
		);

		// Clipboard support (FPlatformApplicationMisc) on all platforms
		PrivateDependencyModuleNames.Add("ApplicationCore");
		PrivateDependencyModuleNames.Add("AudioCaptureCore");

		// Windows only
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// LiveCoding is only available in editor builds on Windows
			if (Target.bBuildEditor)
			{
				PrivateDependencyModuleNames.Add("LiveCoding");
			}
		}
	}
}
