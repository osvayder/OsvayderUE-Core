// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Gameplay Ability System (GAS) authoring surfaces.
 *
 * Query operations (read-only):
 *   - list_abilities: Discover Gameplay Ability blueprints
 *   - get_ability_info: Introspect ability (tags, cost, cooldown, instancing, net)
 *   - list_effects: Discover Gameplay Effect assets
 *   - get_effect_info: Introspect effect (duration, modifiers, tags, stacking)
 *   - list_attribute_sets: Discover AttributeSet C++ classes
 *   - get_attribute_set_info: Introspect attribute set (attributes list)
 *
 * Modify operations:
 *   - set_effect_properties: Configure GE properties via reflection
 *   - configure_gas_ability: Composed bundle (ability + cost + cooldown + tags)
 *   - classify_gas_multiplayer: Classify ability multiplayer behavior (authority/predicted/cosmetic)
 *   - configure_gas_multiplayer_setup: Composed — GA net policies + authoritative GE + cosmetic VFX ref
 */
class FMCPTool_GAS : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	// Query ops
	FMCPToolResult ExecuteListAbilities(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetAbilityInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteListEffects(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetEffectInfo(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteListAttributeSets(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetAttributeSetInfo(const TSharedRef<FJsonObject>& Params);

	// Modify ops
	FMCPToolResult ExecuteSetEffectProperties(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteConfigureGasAbility(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteClassifyGasMultiplayer(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteConfigureGasMultiplayerSetup(const TSharedRef<FJsonObject>& Params);

	// Helpers
	UObject* LoadAssetByPath(const FString& AssetPath, FString& OutError);
	TSharedPtr<FJsonObject> SerializeGameplayTagContainer(const struct FGameplayTagContainer& Tags);
};
